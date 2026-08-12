#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace statsig_cpp_core {

namespace detail {
template <typename T>
std::optional<T> get_opt(const nlohmann::json &j, const char *key) {
  const auto it = j.find(key);
  if (it == j.end() || it->is_null()) {
    return std::nullopt;
  }
  return it->get<T>();
}
} // namespace detail

// One secondary exposure entry, matching the core's wire format
// (statsig-rust/src/evaluation/evaluation_types.rs: camelCase keys).
struct SecondaryExposure {
  std::string gate;
  std::string gate_value;
  std::string rule_id;
};

inline void to_json(nlohmann::json &j, const SecondaryExposure &e) {
  j = nlohmann::json{
      {"gate", e.gate}, {"gateValue", e.gate_value}, {"ruleID", e.rule_id}};
}

inline void from_json(const nlohmann::json &j, SecondaryExposure &e) {
  j.at("gate").get_to(e.gate);
  j.at("gateValue").get_to(e.gate_value);
  j.at("ruleID").get_to(e.rule_id);
}

// The sticky evaluation result for one config, matching the core's
// StickyValues wire format (statsig-rust/src/persistent_storage/
// persistent_storage_trait.rs). Serializing this struct always emits the
// fields the core requires (`value`, `secondary_exposures`), so a value built
// from defaults is guaranteed to deserialize on the Rust side.
struct StickyValues {
  bool value = false;
  // The group's parameter values, as arbitrary JSON (typically an object).
  std::optional<nlohmann::json> json_value;
  std::optional<std::string> rule_id;
  std::optional<std::string> group_name;
  std::vector<SecondaryExposure> secondary_exposures;
  std::optional<std::vector<SecondaryExposure>>
      undelegated_secondary_exposures;
  std::optional<std::string> config_delegate;
  std::optional<std::vector<std::string>> explicit_parameters;
  std::optional<uint64_t> time;
  std::optional<uint32_t> config_version;
};

inline void to_json(nlohmann::json &j, const StickyValues &v) {
  j = nlohmann::json{
      {"value", v.value},
      {"json_value", v.json_value.has_value() ? v.json_value.value()
                                              : nlohmann::json(nullptr)},
      {"rule_id", v.rule_id},
      {"group_name", v.group_name},
      {"secondary_exposures", v.secondary_exposures},
      {"undelegated_secondary_exposures", v.undelegated_secondary_exposures},
      {"config_delegate", v.config_delegate},
      {"explicit_parameters", v.explicit_parameters},
      {"time", v.time},
      {"config_version", v.config_version},
  };
}

inline void from_json(const nlohmann::json &j, StickyValues &v) {
  v.value = j.value("value", false);
  const auto json_value_it = j.find("json_value");
  if (json_value_it != j.end() && !json_value_it->is_null()) {
    v.json_value = *json_value_it;
  } else {
    v.json_value = std::nullopt;
  }
  v.rule_id = detail::get_opt<std::string>(j, "rule_id");
  v.group_name = detail::get_opt<std::string>(j, "group_name");
  v.secondary_exposures =
      detail::get_opt<std::vector<SecondaryExposure>>(j, "secondary_exposures")
          .value_or(std::vector<SecondaryExposure>{});
  v.undelegated_secondary_exposures =
      detail::get_opt<std::vector<SecondaryExposure>>(
          j, "undelegated_secondary_exposures");
  v.config_delegate = detail::get_opt<std::string>(j, "config_delegate");
  v.explicit_parameters =
      detail::get_opt<std::vector<std::string>>(j, "explicit_parameters");
  v.time = detail::get_opt<uint64_t>(j, "time");
  v.config_version = detail::get_opt<uint32_t>(j, "config_version");
}

// Map of config name -> sticky values, keyed by the storage key on disk.
// This is both what `load` returns and what GetExperimentOptions /
// GetLayerOptions accept as userPersistedValues.
using UserPersistedValues = std::unordered_map<std::string, StickyValues>;

// User-provided callbacks for a persistent assignment (sticky value) store.
// Any callback left unset is simply not invoked.
struct PersistentStorageCallbacks {
  // Returns the sticky values for the storage key, or std::nullopt when
  // nothing is stored.
  std::function<std::optional<UserPersistedValues>(const std::string &key)>
      load;

  std::function<void(const std::string &key, const std::string &config_name,
                     const StickyValues &sticky_values)>
      save;

  std::function<void(const std::string &key, const std::string &config_name)>
      delete_value;
};

// Wraps a native persistent storage adapter created through the C FFI.
// Register it on a StatsigOptionsBuilder via set_persistent_storage().
// Configuring persistent storage enables persistent assignment:
// userPersistedValues supplied via GetExperimentOptions / GetLayerOptions are
// honored, and the SDK calls save/delete_value as assignments change.
//
// Keep the instance alive for as long as the owning Statsig instance is in
// use. In-flight callbacks hold their own reference to the callbacks, so
// destroying the storage concurrently with a callback is safe; note that a
// callback already entered at destruction time may still complete, and a new
// storage constructed at that exact moment can reuse the freed slot, so
// destroy a storage only once its Statsig instance has shut down.
//
// At most 32 PersistentStorage instances can be live at once, process-wide
// (the C FFI requires context-free function pointers, so each instance claims
// a slot from a fixed trampoline pool). Constructing one past that limit logs
// to stderr and leaves the storage unregistered: ref() returns 0 and
// set_persistent_storage() will serialize null.
class PersistentStorage {
public:
  explicit PersistentStorage(PersistentStorageCallbacks callbacks);
  ~PersistentStorage();

  PersistentStorage(const PersistentStorage &) = delete;
  PersistentStorage &operator=(const PersistentStorage &) = delete;
  PersistentStorage(PersistentStorage &&) = delete;
  PersistentStorage &operator=(PersistentStorage &&) = delete;

  uint64_t ref() const { return ref_; }

private:
  // Shared with the trampoline slot pool so in-flight callbacks stay valid
  // even if this storage is destroyed concurrently.
  std::shared_ptr<const PersistentStorageCallbacks> callbacks_;
  uint64_t ref_ = 0;
  int slot_ = -1;
};

} // namespace statsig_cpp_core
