mod utils;

use statsig_rust::networking::{NetworkClient, NetworkError, RequestArgs};
use utils::env_var_guard::EnvVarGuard;
use utils::tls_test_server::TlsTestServer;

#[tokio::test]
#[serial_test::serial]
async fn test_unknown_ca_fails_by_default() {
    let server = TlsTestServer::spawn();
    let client = NetworkClient::new("secret-test", None, None);
    let result = client
        .get(RequestArgs {
            url: server.url("/v1/test"),
            ..RequestArgs::new()
        })
        .await;
    assert!(
        result.is_err(),
        "self-signed CA must not be trusted, got status: {:?}",
        result.map(|r| r.status_code)
    );
}

#[tokio::test]
#[serial_test::serial]
async fn test_native_roots_honor_ssl_cert_file() {
    let server = TlsTestServer::spawn();
    let ca_file = tempfile::NamedTempFile::new().unwrap();
    std::fs::write(ca_file.path(), &server.ca_pem).unwrap();
    let _guard = EnvVarGuard::set(
        "SSL_CERT_FILE",
        ca_file.path().to_string_lossy().to_string(),
    );

    let client = NetworkClient::new("secret-test", None, None);
    let result = client
        .get(RequestArgs {
            url: server.url("/v1/test"),
            ..RequestArgs::new()
        })
        .await;
    assert!(
        result.is_ok(),
        "expected SSL_CERT_FILE to be honored: {:?}",
        result.err()
    );
}

#[tokio::test]
#[serial_test::serial]
async fn test_ca_cert_pem_multi_cert_bundle_works() {
    let server = TlsTestServer::spawn();
    let unrelated = TlsTestServer::spawn();

    // malformed leading entry + unrelated CA + webpki-rejected entry + the
    // real CA — like a system bundle with a corrupted entry ahead of it
    let mut bundle =
        b"-----BEGIN CERTIFICATE-----\n!!!not-base64!!!\n-----END CERTIFICATE-----\n".to_vec();
    bundle.extend_from_slice(&unrelated.ca_pem);
    bundle.extend_from_slice(
        b"-----BEGIN CERTIFICATE-----\naGVsbG8gd29ybGQ=\n-----END CERTIFICATE-----\n",
    );
    bundle.extend_from_slice(&server.ca_pem);

    let client = NetworkClient::new("secret-test", None, None);
    let result = client
        .get(RequestArgs {
            url: server.url("/v1/test"),
            ca_cert_pem: Some(bundle),
            ..RequestArgs::new()
        })
        .await;
    assert!(
        result.is_ok(),
        "bundle with extra/bad entries must work: {:?}",
        result.map(|r| r.status_code)
    );
}

#[tokio::test]
#[serial_test::serial]
async fn test_unusable_ca_cert_pem_fails_loudly() {
    let server = TlsTestServer::spawn();
    let client = NetworkClient::new("secret-test", None, None);
    let result = client
        .get(RequestArgs {
            url: server.url("/v1/test"),
            ca_cert_pem: Some(b"not pem at all".to_vec()),
            retries: 3,
            ..RequestArgs::new()
        })
        .await;
    let err = match result {
        Ok(_) => panic!("zero usable certs must fail the request"),
        Err(e) => e,
    };
    assert!(
        matches!(err, NetworkError::RequestNotRetryable(..)),
        "config errors must fail fast without burning the retry budget, got: {err:?}"
    );
    assert!(
        err.to_string().contains("No usable certificates"),
        "got: {err}"
    );
}

#[tokio::test]
#[serial_test::serial]
async fn test_sdk_exception_requests_use_configured_ca() {
    let server = TlsTestServer::spawn();
    let client = NetworkClient::new("secret-test", None, None);
    let result = client
        .post(
            RequestArgs {
                url: server.url("/v1/sdk_exception"),
                ca_cert_pem: Some(server.ca_pem.clone()),
                ..RequestArgs::new()
            },
            Some(b"{}".to_vec()),
        )
        .await;
    assert!(
        result.is_ok(),
        "sdk_exception must honor CA config: {:?}",
        result.map(|r| r.status_code)
    );
}

#[tokio::test]
#[serial_test::serial]
async fn test_tls_failure_error_names_certificates() {
    let server = TlsTestServer::spawn();
    let client = NetworkClient::new("secret-test", None, None);
    let result = client
        .get(RequestArgs {
            url: server.url("/v1/test"),
            ..RequestArgs::new()
        })
        .await;
    let err = match result {
        Ok(_) => panic!("unknown CA must fail"),
        Err(e) => e,
    };
    let message = err.to_string();
    assert!(message.contains("certificate"), "got: {message}");
    assert!(
        message.contains("TLS-inspecting"),
        "hint missing: {message}"
    );
}
