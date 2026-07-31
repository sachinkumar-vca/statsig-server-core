#[derive(Debug, Clone, Hash, PartialEq, Eq)]
pub struct ProxyConfig {
    pub proxy_host: Option<String>,
    pub proxy_port: Option<u16>,
    pub proxy_auth: Option<String>,     // e.g., "username:password"
    pub proxy_protocol: Option<String>, // e.g., "http", "socks5", "https"
    /// Path to a PEM file or multi-cert bundle of additional trusted root CAs
    /// (e.g. /etc/ssl/certs/ca-certificates.crt). Added on top of the OS trust
    /// store and built-in webpki roots. Unparseable bundle entries are skipped.
    pub ca_cert_path: Option<String>,
}
