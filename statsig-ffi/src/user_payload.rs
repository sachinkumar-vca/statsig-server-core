//! [S2SDK-165] Binary user-construction payload decoder.
//!
//! Why this exists: user construction is the expensive JNI crossing for the
//! Java binding (evaluations only pass an 8-byte handle afterwards). The old
//! path serialized the user's map fields (`customIDs`, `custom`,
//! `privateAttributes`) to JSON *text* with fastjson2 on the JVM, pushed ten
//! separate `JString`s through JNI (each paying a UTF-16 -> UTF-8 conversion
//! and copy), and then immediately re-parsed the JSON back into maps here.
//! Every populated field paid: Java-side serialize -> JNI string copy ->
//! Rust-side parse. Customers with rich user objects (many custom fields)
//! measured construction cost scaling with field count for this reason.
//!
//! The new path encodes the *entire* user into a single `byte[]` on the Java
//! side (see `StatsigUserPayload.java`) using a small length-prefixed, tagged
//! binary format:
//! - one JNI argument and one array copy instead of ten string conversions
//! - strings are written as UTF-8 by the JVM, so no charset conversion at the
//!   boundary
//! - typed values (bool/long/double) cross as fixed-width bytes and become
//!   `DynamicValue`s directly -- no JSON stringify/parse round trip
//! - only genuinely nested values (lists / maps inside `custom`) fall back to
//!   a JSON-encoded leaf, which is the rare case
//!
//! Format (all integers big-endian, matching `java.io.DataOutputStream`):
//!
//! ```text
//! payload      := version:u8 scalar*7 string_map(customIDs)
//!                 typed_map(custom) typed_map(privateAttributes)
//! scalar       := len:i32 utf8-bytes        // len == -1 => absent (null)
//!                 // order: userID, email, ip, userAgent, country, locale,
//!                 //        appVersion
//! string_map   := count:i32 (key value)*    // count == -1 => absent map
//! typed_map    := count:i32 (key tagged)*   // count == -1 => absent map
//! key, value   := len:i32 utf8-bytes        // len == -1 => null (entry skipped)
//! tagged       := tag:u8 payload
//!   tag 0 is unassigned: the encoder skips null-valued entries entirely, so
//!         no null tag exists and a 0 tag is rejected as malformed
//!   tag 1 = string              (len:i32 utf8-bytes)
//!   tag 2 = bool true           (no payload)
//!   tag 3 = bool false          (no payload)
//!   tag 4 = i64                 (8 bytes)
//!   tag 5 = f64                 (8 bytes)
//!   tag 6 = json fallback       (len:i32 utf8-bytes, parsed with serde_json)
//! ```
//!
//! The version byte lets the format evolve without silent misreads: the Java
//! encoder and this decoder ship in the same artifact (the uber JAR bundles
//! both), but a mismatch fails loudly instead of decoding garbage.

use serde_json::Value as JsonValue;
use statsig_rust::{
    log_e, DynamicValue, StatsigUser, StatsigUserBuilder, StatsigUserDataMap,
    StatsigUserDataStringMap,
};

const TAG: &str = "UserPayload";

// pub(crate) so cbindgen doesn't export this into the C headers; the payload
// format is private to the Java binding.
pub(crate) const USER_PAYLOAD_VERSION: u8 = 1;

// Tag 0 is deliberately unassigned: the Java encoder skips null map entries
// entirely (matching what fastjson2 omission produced on the old JSON path),
// so no "null value" tag exists and a 0 tag is rejected as malformed.
const TAG_STRING: u8 = 1;
const TAG_BOOL_TRUE: u8 = 2;
const TAG_BOOL_FALSE: u8 = 3;
const TAG_I64: u8 = 4;
const TAG_F64: u8 = 5;
const TAG_JSON: u8 = 6;

/// Decodes the payload produced by `StatsigUserPayload.encode` (Java) and
/// builds the Rust `StatsigUser`. Returns `None` on any malformed input
/// (logging the version-mismatch, bad-tag, and trailing-bytes cases; plain
/// bounds/UTF-8 failures return silently); the JNI layer logs the failure and
/// maps it to a 0 handle, mirroring the old behavior when user creation
/// failed.
pub(crate) fn decode_user_payload(bytes: &[u8]) -> Option<StatsigUser> {
    let mut reader = Reader { bytes, pos: 0 };

    let version = reader.read_u8()?;
    if version != USER_PAYLOAD_VERSION {
        log_e!(
            TAG,
            "User payload version mismatch: got {}, expected {}",
            version,
            USER_PAYLOAD_VERSION
        );
        return None;
    }

    let user_id = reader.read_opt_string()?;
    let email = reader.read_opt_string()?;
    let ip = reader.read_opt_string()?;
    let user_agent = reader.read_opt_string()?;
    let country = reader.read_opt_string()?;
    let locale = reader.read_opt_string()?;
    let app_version = reader.read_opt_string()?;

    let custom_ids = reader.read_string_map()?;
    let custom = reader.read_typed_map()?;
    let private_attributes = reader.read_typed_map()?;

    // A well-formed payload is consumed exactly. Leftover bytes mean the
    // encoder and decoder disagree about the format (e.g. a field added on one
    // side only), which the version byte alone cannot catch -- fail loudly
    // rather than silently dropping data.
    if reader.pos != reader.bytes.len() {
        log_e!(
            TAG,
            "User payload has {} unconsumed trailing bytes",
            reader.bytes.len() - reader.pos
        );
        return None;
    }

    // Same construction logic as the old JSON-based path: custom IDs take
    // precedence for choosing the builder entry point.
    let mut builder = match custom_ids {
        Some(custom_ids) => StatsigUserBuilder::new_with_custom_ids(custom_ids).user_id(user_id),
        None => StatsigUserBuilder::new_with_user_id(user_id.unwrap_or_default()),
    };

    builder = builder
        .email(email)
        .ip(ip)
        .user_agent(user_agent)
        .country(country)
        .locale(locale)
        .app_version(app_version)
        .custom(custom)
        .private_attributes(private_attributes);

    Some(builder.build())
}

/// Cursor over the payload. All reads are bounds-checked; any overrun means a
/// malformed payload and surfaces as `None` all the way up.
struct Reader<'a> {
    bytes: &'a [u8],
    pos: usize,
}

impl<'a> Reader<'a> {
    fn read_u8(&mut self) -> Option<u8> {
        let byte = *self.bytes.get(self.pos)?;
        self.pos += 1;
        Some(byte)
    }

    fn read_i32(&mut self) -> Option<i32> {
        let end = self.pos.checked_add(4)?;
        let slice = self.bytes.get(self.pos..end)?;
        self.pos = end;
        Some(i32::from_be_bytes(slice.try_into().ok()?))
    }

    fn read_i64(&mut self) -> Option<i64> {
        let end = self.pos.checked_add(8)?;
        let slice = self.bytes.get(self.pos..end)?;
        self.pos = end;
        Some(i64::from_be_bytes(slice.try_into().ok()?))
    }

    fn read_f64(&mut self) -> Option<f64> {
        let end = self.pos.checked_add(8)?;
        let slice = self.bytes.get(self.pos..end)?;
        self.pos = end;
        Some(f64::from_be_bytes(slice.try_into().ok()?))
    }

    /// Reads a length-prefixed UTF-8 string; a -1 length means null/absent.
    /// Any other negative length is not producible by the encoder and is
    /// rejected as a malformed payload.
    fn read_opt_string(&mut self) -> Option<Option<String>> {
        let len = self.read_i32()?;
        if len == -1 {
            return Some(None);
        }
        if len < 0 {
            return None;
        }
        let end = self.pos.checked_add(len as usize)?;
        let slice = self.bytes.get(self.pos..end)?;
        self.pos = end;
        // The JVM encodes with StandardCharsets.UTF_8, so this only fails on a
        // corrupted payload.
        match std::str::from_utf8(slice) {
            Ok(s) => {
                let owned = s.to_owned();
                Some(Some(owned))
            }
            Err(_) => None,
        }
    }

    /// String -> String map (customIDs). A -1 count means the whole map was
    /// null on the Java side. Entries with null values are skipped, matching
    /// what JSON serialization of a map with null values used to produce for
    /// the string-map parse path.
    fn read_string_map(&mut self) -> Option<Option<StatsigUserDataStringMap>> {
        let count = self.read_i32()?;
        if count == -1 {
            return Some(None);
        }
        if count < 0 {
            return None;
        }

        let mut map = StatsigUserDataStringMap::default();
        for _ in 0..count {
            let key = self.read_opt_string()?;
            let value = self.read_opt_string()?;
            if let (Some(key), Some(value)) = (key, value) {
                map.insert(key, value);
            }
        }
        Some(Some(map))
    }

    /// String -> DynamicValue map (custom / privateAttributes). Values arrive
    /// as native types and are converted with the existing typed
    /// `DynamicValue::from` impls -- the JSON stringify/parse round trip only
    /// survives for nested structures (TAG_JSON), which are uncommon in user
    /// objects.
    fn read_typed_map(&mut self) -> Option<Option<StatsigUserDataMap>> {
        let count = self.read_i32()?;
        if count == -1 {
            return Some(None);
        }
        if count < 0 {
            return None;
        }

        let mut map = StatsigUserDataMap::default();
        for _ in 0..count {
            // A null key cannot come from the Java encoder; the double `?`
            // treats it as malformed rather than silently continuing misaligned.
            let key = self.read_opt_string()??;

            let tag = self.read_u8()?;
            let value = match tag {
                // The encoder skips null-valued entries, so a null string
                // payload (rejected by the double `?`) can only be corruption.
                TAG_STRING => DynamicValue::from(self.read_opt_string()??),
                TAG_BOOL_TRUE => DynamicValue::from(true),
                TAG_BOOL_FALSE => DynamicValue::from(false),
                TAG_I64 => DynamicValue::from(self.read_i64()?),
                TAG_F64 => DynamicValue::from(self.read_f64()?),
                TAG_JSON => {
                    let json = self.read_opt_string()??;
                    match serde_json::from_str::<JsonValue>(&json) {
                        Ok(value) => DynamicValue::from(value),
                        Err(e) => {
                            log_e!(TAG, "Failed to parse JSON fallback value: {}", e);
                            return None;
                        }
                    }
                }
                unknown => {
                    log_e!(TAG, "Unknown user payload value tag: {}", unknown);
                    return None;
                }
            };

            map.insert(key, value);
        }
        Some(Some(map))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Minimal encoder mirroring StatsigUserPayload.java, used to exercise the
    /// decoder without a JVM.
    struct Writer {
        buf: Vec<u8>,
    }

    impl Writer {
        fn new() -> Self {
            let mut writer = Writer { buf: Vec::new() };
            writer.buf.push(USER_PAYLOAD_VERSION);
            writer
        }

        fn string(&mut self, value: Option<&str>) -> &mut Self {
            match value {
                Some(s) => {
                    self.buf.extend((s.len() as i32).to_be_bytes());
                    self.buf.extend(s.as_bytes());
                }
                None => self.buf.extend((-1i32).to_be_bytes()),
            }
            self
        }

        fn count(&mut self, count: i32) -> &mut Self {
            self.buf.extend(count.to_be_bytes());
            self
        }

        fn tag(&mut self, tag: u8) -> &mut Self {
            self.buf.push(tag);
            self
        }
    }

    fn null_maps(writer: &mut Writer) {
        writer.count(-1).count(-1).count(-1);
    }

    fn str_of(value: &DynamicValue) -> Option<String> {
        value
            .string_value
            .as_ref()
            .map(|s| s.value.as_str().to_string())
    }

    #[test]
    fn decodes_minimal_user() {
        let mut w = Writer::new();
        w.string(Some("user_1"));
        for _ in 0..6 {
            w.string(None);
        }
        null_maps(&mut w);

        let user = decode_user_payload(&w.buf).expect("decode");
        let data = user.data.as_ref();
        assert_eq!(
            data.user_id.as_ref().and_then(str_of),
            Some("user_1".to_string())
        );
        assert!(data.custom.is_none());
        assert!(data.custom_ids.is_none());
    }

    #[test]
    fn decodes_all_scalars_and_custom_ids() {
        let mut w = Writer::new();
        w.string(Some("user_1"))
            .string(Some("a@b.com"))
            .string(Some("10.0.0.1"))
            .string(Some("Mozilla/5.0"))
            .string(Some("US"))
            .string(Some("en_US"))
            .string(Some("1.2.3"));
        w.count(1).string(Some("companyID")).string(Some("c42"));
        w.count(-1).count(-1);

        let user = decode_user_payload(&w.buf).expect("decode");
        let data = user.data.as_ref();
        assert_eq!(
            data.email.as_ref().and_then(str_of),
            Some("a@b.com".to_string())
        );
        assert_eq!(
            data.custom_ids
                .as_ref()
                .and_then(|m| m.get("companyID"))
                .and_then(str_of),
            Some("c42".to_string())
        );
    }

    #[test]
    fn decodes_typed_custom_values() {
        let mut w = Writer::new();
        w.string(Some("user_1"));
        for _ in 0..6 {
            w.string(None);
        }
        w.count(-1); // customIDs

        w.count(5);
        w.string(Some("plan")).tag(TAG_STRING).string(Some("pro"));
        w.string(Some("beta")).tag(TAG_BOOL_TRUE);
        w.string(Some("seats")).tag(TAG_I64);
        w.buf.extend(42i64.to_be_bytes());
        w.string(Some("score")).tag(TAG_F64);
        w.buf.extend(1.5f64.to_be_bytes());
        w.string(Some("tags"))
            .tag(TAG_JSON)
            .string(Some("[\"a\",\"b\"]"));

        w.count(-1); // privateAttributes

        let user = decode_user_payload(&w.buf).expect("decode");
        let data = user.data.as_ref();
        let custom = data.custom.as_ref().expect("custom map");

        assert_eq!(custom.get("plan").and_then(str_of), Some("pro".to_string()));
        assert_eq!(custom.get("beta").and_then(|v| v.bool_value), Some(true));
        assert_eq!(custom.get("seats").and_then(|v| v.int_value), Some(42));
        assert_eq!(custom.get("score").and_then(|v| v.float_value), Some(1.5));
        assert_eq!(
            custom
                .get("tags")
                .and_then(|v| v.array_value.as_ref())
                .map(|a| a.len()),
            Some(2)
        );
    }

    #[test]
    fn rejects_wrong_version() {
        let mut w = Writer::new();
        w.string(Some("user_1"));
        for _ in 0..6 {
            w.string(None);
        }
        null_maps(&mut w);
        w.buf[0] = 99;

        assert!(decode_user_payload(&w.buf).is_none());
    }

    #[test]
    fn rejects_truncated_payload() {
        let mut w = Writer::new();
        w.string(Some("user_1"));
        for _ in 0..6 {
            w.string(None);
        }
        null_maps(&mut w);

        for cut in 1..w.buf.len() {
            assert!(
                decode_user_payload(&w.buf[..cut]).is_none(),
                "truncation at {cut} should fail"
            );
        }
    }

    #[test]
    fn rejects_trailing_bytes() {
        let mut w = Writer::new();
        w.string(Some("user_1"));
        for _ in 0..6 {
            w.string(None);
        }
        null_maps(&mut w);

        // Sanity: the exact payload decodes...
        assert!(decode_user_payload(&w.buf).is_some());

        // ...but any unconsumed trailing bytes mean an encoder/decoder format
        // mismatch and must be rejected.
        w.buf.push(0);
        assert!(decode_user_payload(&w.buf).is_none());
    }

    #[test]
    fn rejects_negative_lengths_other_than_null_sentinel() {
        // -1 is the only valid null sentinel; any other negative string length
        // or map count means the payload is corrupted.
        let mut w = Writer::new();
        w.count(-2); // userID length
        assert!(decode_user_payload(&w.buf).is_none());

        let mut w = Writer::new();
        w.string(Some("user_1"));
        for _ in 0..6 {
            w.string(None);
        }
        w.count(-5); // customIDs count
        w.count(-1).count(-1);
        assert!(decode_user_payload(&w.buf).is_none());

        let mut w = Writer::new();
        w.string(Some("user_1"));
        for _ in 0..6 {
            w.string(None);
        }
        w.count(-1);
        w.count(i32::MIN); // custom count
        w.count(-1);
        assert!(decode_user_payload(&w.buf).is_none());
    }

    #[test]
    fn rejects_values_the_encoder_never_emits() {
        // The encoder skips null-valued entries, so a 0 tag (no null tag is
        // assigned) and a TAG_STRING with a null string payload are both
        // malformed, not "explicit null" values.
        let mut w = Writer::new();
        w.string(Some("user_1"));
        for _ in 0..6 {
            w.string(None);
        }
        w.count(-1);
        w.count(1);
        w.string(Some("key")).tag(0);
        w.count(-1);
        assert!(decode_user_payload(&w.buf).is_none());

        let mut w = Writer::new();
        w.string(Some("user_1"));
        for _ in 0..6 {
            w.string(None);
        }
        w.count(-1);
        w.count(1);
        w.string(Some("key")).tag(TAG_STRING).string(None);
        w.count(-1);
        assert!(decode_user_payload(&w.buf).is_none());
    }

    #[test]
    fn rejects_unknown_tag() {
        let mut w = Writer::new();
        w.string(Some("user_1"));
        for _ in 0..6 {
            w.string(None);
        }
        w.count(-1);
        w.count(1);
        w.string(Some("key")).tag(200);
        w.count(-1);

        assert!(decode_user_payload(&w.buf).is_none());
    }
}
