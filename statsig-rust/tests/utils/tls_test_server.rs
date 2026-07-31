use std::io::{Read, Write};
use std::net::TcpListener;
use std::sync::Arc;

use rcgen::{BasicConstraints, CertificateParams, IsCa, KeyPair};

pub struct TlsTestServer {
    pub port: u16,
    pub ca_pem: Vec<u8>,
}

impl TlsTestServer {
    pub fn spawn() -> Self {
        let mut ca_params = CertificateParams::new(vec![]).unwrap();
        ca_params.is_ca = IsCa::Ca(BasicConstraints::Unconstrained);
        let ca_key = KeyPair::generate().unwrap();
        let ca_cert = ca_params.self_signed(&ca_key).unwrap();

        let leaf_params = CertificateParams::new(vec!["localhost".into()]).unwrap();
        let leaf_key = KeyPair::generate().unwrap();
        let leaf_cert = leaf_params.signed_by(&leaf_key, &ca_cert, &ca_key).unwrap();

        let ca_pem = ca_cert.pem().into_bytes();

        let provider = Arc::new(rustls::crypto::ring::default_provider());
        let config = rustls::ServerConfig::builder_with_provider(provider)
            .with_safe_default_protocol_versions()
            .unwrap()
            .with_no_client_auth()
            .with_single_cert(
                vec![leaf_cert.der().clone()],
                rustls::pki_types::PrivateKeyDer::Pkcs8(leaf_key.serialize_der().into()),
            )
            .unwrap();
        let config = Arc::new(config);

        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();

        std::thread::spawn(move || {
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { break };
                let config = config.clone();
                std::thread::spawn(move || {
                    let Ok(mut conn) = rustls::ServerConnection::new(config) else {
                        return;
                    };
                    let mut tls = rustls::Stream::new(&mut conn, &mut stream);
                    let mut buf = [0u8; 8192];
                    let _ = tls.read(&mut buf);
                    let body = r#"{"ok":true}"#;
                    let response = format!(
                        "HTTP/1.1 200 OK\r\ncontent-type: application/json\r\ncontent-length: {}\r\nconnection: close\r\n\r\n{body}",
                        body.len()
                    );
                    let _ = tls.write_all(response.as_bytes());
                });
            }
        });

        Self { port, ca_pem }
    }

    pub fn url(&self, path: &str) -> String {
        format!("https://localhost:{}{}", self.port, path)
    }
}
