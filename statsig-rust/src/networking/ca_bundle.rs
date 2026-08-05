use std::io::BufReader;

pub struct ParsedCaCerts {
    pub certs: Vec<reqwest::Certificate>,
    pub skipped: usize,
}

/// Parses a PEM file or bundle into reqwest root certificates.
///
/// Lenient like reqwest's own native-root loading: entries that webpki
/// rejects (ancient/extension-less roots common in system bundles) are
/// skipped and counted rather than failing the whole bundle. Malformed PEM
/// sections (bad base64, bad framing, etc.) are also skipped and counted:
/// rustls-pemfile consumes the errored section and continues, so iteration
/// keeps going over the rest of the bundle and only ends at EOF.
pub fn parse_ca_certs(pem: &[u8]) -> ParsedCaCerts {
    let mut preflight_store = rustls::RootCertStore::empty();
    let mut certs = Vec::new();
    let mut skipped = 0;

    for item in rustls_pemfile::certs(&mut BufReader::new(pem)) {
        let der = match item {
            Ok(der) => der,
            Err(_) => {
                skipped += 1;
                continue;
            }
        };

        // Same acceptance semantics reqwest applies at ClientBuilder::build();
        // filtering here keeps one bad cert from failing the whole client.
        if preflight_store.add(der.clone()).is_err() {
            skipped += 1;
            continue;
        }

        match reqwest::Certificate::from_der(der.as_ref()) {
            Ok(cert) => certs.push(cert),
            Err(_) => skipped += 1,
        }
    }

    ParsedCaCerts { certs, skipped }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn generate_ca_pem() -> Vec<u8> {
        use rcgen::{BasicConstraints, CertificateParams, IsCa, KeyPair};
        let mut params = CertificateParams::new(vec![]).unwrap();
        params.is_ca = IsCa::Ca(BasicConstraints::Unconstrained);
        let key = KeyPair::generate().unwrap();
        params.self_signed(&key).unwrap().pem().into_bytes()
    }

    // valid base64, invalid DER — exercises the webpki-reject skip path
    const BAD_DER_ENTRY: &[u8] =
        b"-----BEGIN CERTIFICATE-----\naGVsbG8gd29ybGQ=\n-----END CERTIFICATE-----\n";

    #[test]
    fn test_single_valid_cert() {
        let result = parse_ca_certs(&generate_ca_pem());
        assert_eq!(result.certs.len(), 1);
        assert_eq!(result.skipped, 0);
    }

    #[test]
    fn test_multi_cert_bundle() {
        let mut bundle = generate_ca_pem();
        bundle.extend_from_slice(&generate_ca_pem());
        bundle.extend_from_slice(&generate_ca_pem());
        let result = parse_ca_certs(&bundle);
        assert_eq!(result.certs.len(), 3);
        assert_eq!(result.skipped, 0);
    }

    #[test]
    fn test_bundle_with_unparseable_entry_skips_and_continues() {
        let mut bundle = generate_ca_pem();
        bundle.extend_from_slice(BAD_DER_ENTRY);
        bundle.extend_from_slice(&generate_ca_pem());
        let result = parse_ca_certs(&bundle);
        assert_eq!(result.certs.len(), 2);
        assert_eq!(result.skipped, 1);
    }

    #[test]
    fn test_all_garbage_yields_zero_certs() {
        let result = parse_ca_certs(b"not pem at all");
        assert_eq!(result.certs.len(), 0);
    }

    // valid PEM armor, invalid base64 body — exercises the pemfile Err path
    const BAD_BASE64_ENTRY: &[u8] =
        b"-----BEGIN CERTIFICATE-----\n!!!not-base64!!!\n-----END CERTIFICATE-----\n";

    #[test]
    fn test_bundle_with_malformed_section_before_valid_certs() {
        let mut bundle = BAD_BASE64_ENTRY.to_vec();
        bundle.extend_from_slice(&generate_ca_pem());
        bundle.extend_from_slice(&generate_ca_pem());
        let result = parse_ca_certs(&bundle);
        assert_eq!(result.certs.len(), 2);
        assert_eq!(result.skipped, 1);
    }

    #[test]
    fn test_empty_input() {
        let result = parse_ca_certs(b"");
        assert_eq!(result.certs.len(), 0);
        assert_eq!(result.skipped, 0);
    }
}
