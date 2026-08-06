#include "../include/libstatsig_ffi.h"
#include "../src/options.h"
#include "../src/persistent_storage.h"
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using statsig_cpp_core::GetExperimentOptions;
using statsig_cpp_core::GetLayerOptions;
using statsig_cpp_core::PersistentStorage;
using statsig_cpp_core::PersistentStorageCallbacks;
using statsig_cpp_core::StatsigOptionsBuilder;
using statsig_cpp_core::StickyValues;
using statsig_cpp_core::UserPersistedValues;

namespace {

UserPersistedValues stickyValues() {
  StickyValues values;
  values.value = true;
  values.json_value = json{{"value", "sticky_value"}};
  values.rule_id = "sticky_rule_id";
  values.group_name = "Sticky Group";
  values.undelegated_secondary_exposures.emplace();
  values.time = 1700000000000ULL;
  return {{"enforce_exp", values}};
}

struct RecordingState {
  std::vector<std::string> loadCalls;
  std::vector<std::pair<std::string, std::string>> saveCalls;
  std::vector<std::pair<std::string, std::string>> deleteCalls;
  StickyValues lastSavedValues;
};

PersistentStorageCallbacks recordingCallbacks(RecordingState &state) {
  PersistentStorageCallbacks callbacks;
  callbacks.load =
      [&state](const std::string &key) -> std::optional<UserPersistedValues> {
    state.loadCalls.push_back(key);
    return stickyValues();
  };
  callbacks.save = [&state](const std::string &key,
                            const std::string &config_name,
                            const StickyValues &sticky_values) {
    state.saveCalls.emplace_back(key, config_name);
    state.lastSavedValues = sticky_values;
  };
  callbacks.delete_value = [&state](const std::string &key,
                                    const std::string &config_name) {
    state.deleteCalls.emplace_back(key, config_name);
  };
  return callbacks;
}

// Drives the native load path the way the core would and returns the
// serialized Option<UserPersistedValues>: "null" when the load produced no
// usable values, else the values JSON.
std::string loadThroughFfi(uint64_t ref, const std::string &key) {
  char *loaded = __internal__test_persistent_storage(ref, "load", key.c_str(),
                                                     "", "");
  if (loaded == nullptr) {
    return {};
  }
  std::string owned(loaded);
  free_string(loaded);
  return owned;
}

} // namespace

// The get options cross the C FFI as a JSON blob that the Rust core
// deserializes, so the serialized keys are the contract with the core
// (statsig-rust/src/statsig_core_api_options.rs).
TEST(PersistentAssignment, ExperimentOptionsSerialization) {
  GetExperimentOptions options;
  options.disableExposureLogging = true;
  options.enforceOverrides = true;
  options.enforceTargeting = true;
  options.userPersistedValues = stickyValues();

  json j = options;

  EXPECT_EQ(j["disable_exposure_logging"], true);
  EXPECT_EQ(j["enforce_overrides"], true);
  EXPECT_EQ(j["enforce_targeting"], true);
  EXPECT_EQ(j["user_persisted_values"]["enforce_exp"]["rule_id"],
            "sticky_rule_id");
}

TEST(PersistentAssignment, ExperimentOptionsSerializationDefaults) {
  GetExperimentOptions options;

  json j = options;

  EXPECT_EQ(j["disable_exposure_logging"], false);
  EXPECT_EQ(j["enforce_overrides"], false);
  EXPECT_EQ(j["enforce_targeting"], false);
  EXPECT_FALSE(j.contains("user_persisted_values"));
}

TEST(PersistentAssignment, LayerOptionsSerialization) {
  GetLayerOptions options;
  options.enforceOverrides = true;
  options.enforceTargeting = true;
  options.userPersistedValues = stickyValues();

  json j = options;

  EXPECT_EQ(j["disable_exposure_logging"], false);
  EXPECT_EQ(j["enforce_overrides"], true);
  EXPECT_EQ(j["enforce_targeting"], true);
  EXPECT_EQ(j["user_persisted_values"]["enforce_exp"]["value"], true);
}

// The core requires `value` and `secondary_exposures` when deserializing
// StickyValues (no serde default), and a parse failure of the options blob
// silently falls back to a default-constructed result. The typed struct must
// therefore always emit those fields, even when default-constructed.
TEST(PersistentAssignment, DefaultStickyValuesSerializeRequiredFields) {
  json j = StickyValues{};

  EXPECT_EQ(j["value"], false);
  ASSERT_TRUE(j.contains("secondary_exposures"));
  EXPECT_TRUE(j["secondary_exposures"].is_array());
  EXPECT_TRUE(j["json_value"].is_null());
  EXPECT_TRUE(j["config_delegate"].is_null());
  EXPECT_TRUE(j["time"].is_null());
}

TEST(PersistentAssignment, StickyValuesJsonRoundTrip) {
  statsig_cpp_core::SecondaryExposure exposure;
  exposure.gate = "dependent_gate";
  exposure.gate_value = "true";
  exposure.rule_id = "rule_1";

  StickyValues original = stickyValues()["enforce_exp"];
  original.secondary_exposures.push_back(exposure);
  original.config_version = 7;

  const StickyValues parsed = json(original).get<StickyValues>();

  EXPECT_EQ(parsed.value, true);
  EXPECT_EQ(parsed.json_value, json({{"value", "sticky_value"}}));
  EXPECT_EQ(parsed.rule_id, "sticky_rule_id");
  EXPECT_EQ(parsed.group_name, "Sticky Group");
  ASSERT_EQ(parsed.secondary_exposures.size(), 1UL);
  EXPECT_EQ(parsed.secondary_exposures[0].gate, "dependent_gate");
  EXPECT_EQ(parsed.secondary_exposures[0].gate_value, "true");
  EXPECT_EQ(parsed.secondary_exposures[0].rule_id, "rule_1");
  ASSERT_TRUE(parsed.undelegated_secondary_exposures.has_value());
  EXPECT_TRUE(parsed.undelegated_secondary_exposures->empty());
  EXPECT_FALSE(parsed.config_delegate.has_value());
  EXPECT_FALSE(parsed.explicit_parameters.has_value());
  EXPECT_EQ(parsed.time, 1700000000000ULL);
  EXPECT_EQ(parsed.config_version, 7U);
}

TEST(PersistentAssignment, OptionsBuilderSetsStorageRef) {
  RecordingState state;
  PersistentStorage storage(recordingCallbacks(state));
  ASSERT_NE(storage.ref(), 0UL);

  StatsigOptionsBuilder builder;
  builder.set_persistent_storage(storage);

  json j = builder;
  EXPECT_EQ(j["persistent_storage_ref"], storage.ref());
}

TEST(PersistentAssignment, StorageCallbacksRoundTrip) {
  RecordingState state;
  PersistentStorage storage(recordingCallbacks(state));
  ASSERT_NE(storage.ref(), 0UL);

  // load
  const std::string loaded = loadThroughFfi(storage.ref(), "user_123:userID");
  ASSERT_EQ(state.loadCalls.size(), 1UL);
  EXPECT_EQ(state.loadCalls[0], "user_123:userID");
  json loaded_json = json::parse(loaded);
  EXPECT_EQ(loaded_json["enforce_exp"]["rule_id"], "sticky_rule_id");

  // save: the FFI test helper parses `data` into the core's StickyValues and
  // re-serializes it, so the typed value observed by the callback has made a
  // full trip through the Rust wire format.
  const std::string sticky = json(stickyValues()["enforce_exp"]).dump();
  __internal__test_persistent_storage(storage.ref(), "save", "user_123:userID",
                                      "enforce_exp", sticky.c_str());
  ASSERT_EQ(state.saveCalls.size(), 1UL);
  EXPECT_EQ(state.saveCalls[0].first, "user_123:userID");
  EXPECT_EQ(state.saveCalls[0].second, "enforce_exp");
  EXPECT_EQ(state.lastSavedValues.value, true);
  EXPECT_EQ(state.lastSavedValues.rule_id, "sticky_rule_id");
  EXPECT_EQ(state.lastSavedValues.group_name, "Sticky Group");
  EXPECT_EQ(state.lastSavedValues.json_value, json({{"value", "sticky_value"}}));
  EXPECT_EQ(state.lastSavedValues.time, 1700000000000ULL);

  // delete
  __internal__test_persistent_storage(storage.ref(), "delete",
                                      "user_123:userID", "enforce_exp", "");
  ASSERT_EQ(state.deleteCalls.size(), 1UL);
  EXPECT_EQ(state.deleteCalls[0].first, "user_123:userID");
  EXPECT_EQ(state.deleteCalls[0].second, "enforce_exp");
}

// Callbacks run inside extern "C" trampolines invoked from Rust; exceptions
// must never unwind across that boundary.
TEST(PersistentAssignment, ThrowingLoadCallbackIsSwallowed) {
  PersistentStorageCallbacks callbacks;
  callbacks.load = [](const std::string &) -> std::optional<UserPersistedValues> {
    throw std::runtime_error("disk exploded");
  };
  PersistentStorage storage(std::move(callbacks));
  ASSERT_NE(storage.ref(), 0UL);

  EXPECT_EQ(loadThroughFfi(storage.ref(), "user_123:userID"), "null");
}

TEST(PersistentAssignment, ThrowingSaveAndDeleteCallbacksAreSwallowed) {
  PersistentStorageCallbacks callbacks;
  callbacks.save = [](const std::string &, const std::string &,
                      const StickyValues &) {
    throw 42; // non-std exception
  };
  callbacks.delete_value = [](const std::string &, const std::string &) {
    throw std::runtime_error("db offline");
  };
  PersistentStorage storage(std::move(callbacks));
  ASSERT_NE(storage.ref(), 0UL);

  const std::string sticky = json(stickyValues()["enforce_exp"]).dump();
  __internal__test_persistent_storage(storage.ref(), "save", "user_123:userID",
                                      "enforce_exp", sticky.c_str());
  __internal__test_persistent_storage(storage.ref(), "delete",
                                      "user_123:userID", "enforce_exp", "");
  // Reaching here without std::terminate is the assertion.
  SUCCEED();
}

TEST(PersistentAssignment, LoadReturningNulloptYieldsNull) {
  PersistentStorageCallbacks callbacks;
  callbacks.load = [](const std::string &) -> std::optional<UserPersistedValues> {
    return std::nullopt;
  };
  PersistentStorage storage(std::move(callbacks));
  ASSERT_NE(storage.ref(), 0UL);

  EXPECT_EQ(loadThroughFfi(storage.ref(), "user_123:userID"), "null");
}

// A default-constructed StickyValues must survive the core's deserializer:
// the typed struct exists precisely so no shape a user can build is rejected
// rust-side (which would silently degrade the whole evaluation).
TEST(PersistentAssignment, DefaultConstructedStickyValuesLoadThroughFfi) {
  PersistentStorageCallbacks callbacks;
  callbacks.load = [](const std::string &) -> std::optional<UserPersistedValues> {
    return UserPersistedValues{{"enforce_exp", StickyValues{}}};
  };
  PersistentStorage storage(std::move(callbacks));
  ASSERT_NE(storage.ref(), 0UL);

  const std::string loaded = loadThroughFfi(storage.ref(), "user_123:userID");
  ASSERT_NE(loaded, "null");
  json loaded_json = json::parse(loaded);
  EXPECT_EQ(loaded_json["enforce_exp"]["value"], false);
}

TEST(PersistentAssignment, EmptyKeyLoadIsNotDispatched) {
  RecordingState state;
  PersistentStorage storage(recordingCallbacks(state));
  ASSERT_NE(storage.ref(), 0UL);

  EXPECT_EQ(loadThroughFfi(storage.ref(), ""), "null");
  EXPECT_TRUE(state.loadCalls.empty());
}

TEST(PersistentAssignment, UnsetCallbacksAreNoOps) {
  PersistentStorage storage(PersistentStorageCallbacks{});
  ASSERT_NE(storage.ref(), 0UL);

  EXPECT_EQ(loadThroughFfi(storage.ref(), "user_123:userID"), "null");

  const std::string sticky = json(stickyValues()["enforce_exp"]).dump();
  __internal__test_persistent_storage(storage.ref(), "save", "user_123:userID",
                                      "enforce_exp", sticky.c_str());
  __internal__test_persistent_storage(storage.ref(), "delete",
                                      "user_123:userID", "enforce_exp", "");
  SUCCEED();
}

TEST(PersistentAssignment, MultipleInstancesDispatchIndependently) {
  RecordingState stateA;
  RecordingState stateB;
  PersistentStorage storageA(recordingCallbacks(stateA));
  PersistentStorage storageB(recordingCallbacks(stateB));
  ASSERT_NE(storageA.ref(), 0UL);
  ASSERT_NE(storageB.ref(), 0UL);
  ASSERT_NE(storageA.ref(), storageB.ref());

  __internal__test_persistent_storage(storageA.ref(), "delete",
                                      "user_a:userID", "enforce_exp", "");
  __internal__test_persistent_storage(storageB.ref(), "delete",
                                      "user_b:userID", "enforce_exp", "");

  ASSERT_EQ(stateA.deleteCalls.size(), 1UL);
  EXPECT_EQ(stateA.deleteCalls[0].first, "user_a:userID");
  ASSERT_EQ(stateB.deleteCalls.size(), 1UL);
  EXPECT_EQ(stateB.deleteCalls[0].first, "user_b:userID");
}

TEST(PersistentAssignment, SlotExhaustionIsRecoverableAndSerializesNull) {
  std::vector<std::unique_ptr<PersistentStorage>> pool;
  for (int i = 0; i < 32; i++) {
    pool.push_back(
        std::make_unique<PersistentStorage>(PersistentStorageCallbacks{}));
    ASSERT_NE(pool.back()->ref(), 0UL);
  }

  // The 33rd instance exceeds the slot pool: unregistered, and the builder
  // maps the invalid ref to null rather than serializing 0.
  PersistentStorage overflow(PersistentStorageCallbacks{});
  EXPECT_EQ(overflow.ref(), 0UL);

  StatsigOptionsBuilder builder;
  builder.set_persistent_storage(overflow);
  json j = builder;
  EXPECT_TRUE(j["persistent_storage_ref"].is_null());

  // Releasing one slot makes construction succeed again.
  pool.pop_back();
  PersistentStorage recovered(PersistentStorageCallbacks{});
  EXPECT_NE(recovered.ref(), 0UL);
}
