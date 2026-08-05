use crate::user_payload::decode_user_payload;
use jni::objects::{JByteArray, JClass};
use jni::sys::jlong;
use jni::JNIEnv;
use statsig_rust::{log_d, log_e, InstanceRegistry};

const TAG: &str = "StatsigUserJNI";

/// [S2SDK-165] User construction is the expensive JNI crossing for this
/// binding (evaluations afterwards only pass the returned handle), and it used
/// to take ten separate `JString` arguments with the map fields serialized to
/// JSON on the JVM and re-parsed here -- a serialize -> copy -> parse round
/// trip per construction that scaled with the number of populated fields.
///
/// The user now arrives as a single length-prefixed binary payload (encoded by
/// `StatsigUserPayload.java`): one array argument, one copy, one typed decode,
/// and no JSON for flat values. See `user_payload.rs` for the format.
///
/// Named `...FromPayload` (not reusing `statsigUserCreate`) on purpose: JNI
/// symbol names for non-overloaded methods do not encode the signature, so a
/// mismatched native library (e.g. via a STATSIG_NATIVE_LIB override) would
/// otherwise resolve the old ten-string symbol with the wrong ABI and crash
/// the JVM. The rename turns that into a clear UnsatisfiedLinkError.
#[no_mangle]
pub extern "system" fn Java_com_statsig_StatsigJNI_statsigUserCreateFromPayload(
    env: JNIEnv,
    _class: JClass,
    payload: JByteArray,
) -> jlong {
    // One copy out of the JVM. The payload is raw UTF-8 + fixed-width scalars,
    // so unlike the old JString path there is no UTF-16 -> UTF-8 conversion.
    let bytes = match env.convert_byte_array(&payload) {
        Ok(bytes) => bytes,
        Err(e) => {
            log_e!(TAG, "Failed to read user payload: {}", e);
            return 0;
        }
    };

    let user = match decode_user_payload(&bytes) {
        Some(user) => user,
        None => {
            log_e!(TAG, "Failed to decode user payload");
            return 0;
        }
    };

    match InstanceRegistry::register(user) {
        Some(id) => {
            log_d!(TAG, "Created StatsigUser {}", id);
            id as jlong
        }
        None => {
            log_e!(TAG, "Failed to create StatsigUser");
            0
        }
    }
}

#[no_mangle]
pub extern "system" fn Java_com_statsig_StatsigJNI_statsigUserRelease(
    _env: JNIEnv,
    _class: JClass,
    user_ref: jlong,
) {
    InstanceRegistry::remove(&(user_ref as u64))
}
