use rustler::{env::OwnedEnv, types::local_pid::LocalPid, Encoder};
use statsig_rust::{log_d, log_e, PersistentStorage, StickyValues, UserPersistedValues};

use crate::data_store_nfi::current_managed_env;
use crate::statsig_types_nfi::ValueMap;

const TAG: &str = "[PersistentStorage NFI] ";

mod atoms {
    rustler::atoms! {
        persistent_storage_request = "statsig_persistent_storage_request",
        save,
        delete
    }
}

#[derive(rustler::NifStruct)]
#[module = "Statsig.PersistentStorage.Reference"]
pub struct StatsigPersistentStorageReference {
    pub pid: LocalPid,
}

/// Persistent storage bridge that forwards save/delete notifications from the
/// evaluation path to an Elixir process as fire-and-forget messages.
///
/// `load` is intentionally not bridged: the core evaluation path never calls
/// it (values are supplied per call via the experiment/layer evaluation
/// options), and Elixir callers read their own store directly.
pub struct ElixirPersistentStorage {
    pid: LocalPid,
}

impl ElixirPersistentStorage {
    pub fn new(pid: LocalPid) -> Self {
        Self { pid }
    }

    fn send_message(&self, message: impl Encoder) {
        let delivered = if let Some(env) = current_managed_env() {
            env.send(&self.pid, message).is_ok()
        } else {
            let mut env = OwnedEnv::new();
            env.send_and_clear(&self.pid, |env| message.encode(env))
                .is_ok()
        };

        if !delivered {
            log_e!(TAG, "Failed to message Elixir persistent storage process");
        }
    }
}

impl PersistentStorage for ElixirPersistentStorage {
    fn load(&self, _key: String) -> Option<UserPersistedValues> {
        log_d!(
            TAG,
            "load is not bridged to Elixir; pass user_persisted_values via evaluation options"
        );
        None
    }

    fn save(&self, key: &str, config_name: &str, data: StickyValues) {
        // Deliver sticky values as a decoded map so the Elixir callback can
        // persist them without a JSON dependency; the shape is symmetric with
        // the `user_persisted_values` entries accepted by evaluation options.
        let sticky_map = match serde_json::to_value(&data) {
            Ok(serde_json::Value::Object(map)) => ValueMap(map.into_iter().collect()),
            Ok(other) => {
                log_e!(TAG, "Unexpected sticky values shape: {:?}", other);
                return;
            }
            Err(e) => {
                log_e!(TAG, "Failed to serialize sticky values: {:?}", e);
                return;
            }
        };

        self.send_message((
            atoms::persistent_storage_request(),
            atoms::save(),
            key,
            config_name,
            sticky_map,
        ));
    }

    fn delete(&self, key: &str, config_name: &str) {
        self.send_message((
            atoms::persistent_storage_request(),
            atoms::delete(),
            key,
            config_name,
        ));
    }
}
