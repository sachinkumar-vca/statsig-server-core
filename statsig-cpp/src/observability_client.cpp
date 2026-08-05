#include "observability_client.h"
#include "libstatsig_ffi.h"
#include <array>
#include <cstddef>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <utility>

using json = nlohmann::json;

namespace statsig_cpp_core {
namespace {

using CallbacksPtr = std::shared_ptr<const ObservabilityClientCallbacks>;

// The C FFI hands us plain, context-free function pointers, so we cannot pass a
// capturing lambda. Instead we keep a small pool of slots, each backed by a
// distinct set of static trampoline functions (generated via templates). Each
// live ObservabilityClient claims one slot; its trampolines look the callbacks
// back up by slot index. Slots hold shared_ptrs so an in-flight callback keeps
// the callbacks alive even if the owning client is destroyed concurrently.
constexpr std::size_t kMaxSlots = 32;

std::mutex &slots_mutex() {
  static std::mutex m;
  return m;
}

std::array<CallbacksPtr, kMaxSlots> &slots() {
  static std::array<CallbacksPtr, kMaxSlots> s{};
  return s;
}

// Returns a strong reference so the callbacks outlive this dispatch even if
// the owning ObservabilityClient is destroyed while the callback is in flight.
CallbacksPtr load_callbacks(std::size_t slot) {
  std::lock_guard<std::mutex> lock(slots_mutex());
  return slots()[slot];
}

ObservabilityClientCallbacks::Tags parse_tags(const json &j) {
  ObservabilityClientCallbacks::Tags tags;
  if (j.contains("tags") && j["tags"].is_object()) {
    for (auto &entry : j["tags"].items()) {
      if (entry.value().is_string()) {
        tags.emplace(entry.key(), entry.value().get<std::string>());
      }
    }
  }
  return tags;
}

enum class MetricKind { Increment, Gauge, Dist };

const std::function<void(const std::string &, double,
                         const ObservabilityClientCallbacks::Tags &)> &
metric_fn(const ObservabilityClientCallbacks &cbs, MetricKind kind) {
  switch (kind) {
  case MetricKind::Increment:
    return cbs.increment;
  case MetricKind::Gauge:
    return cbs.gauge;
  default:
    return cbs.dist;
  }
}

void free_args(const char *args_ptr) {
  if (args_ptr != nullptr) {
    free_string(const_cast<char *>(args_ptr));
  }
}

// Takes ownership of the FFI-allocated `args_ptr` (frees it via free_string)
// and copies it out using the explicit length, so embedded or missing NULs
// cannot truncate or overread.
std::string consume_args(const char *args_ptr, uint64_t args_length) {
  if (args_ptr == nullptr) {
    return {};
  }
  std::string owned(args_ptr, static_cast<std::size_t>(args_length));
  free_args(args_ptr);
  return owned;
}

// Every dispatch_* guard swallows all exceptions: these functions are invoked
// through extern "C" function pointers from Rust, and unwinding across the FFI
// boundary is undefined behavior.
void dispatch_init(std::size_t slot) {
  CallbacksPtr cbs = load_callbacks(slot);
  if (cbs == nullptr || !cbs->init) {
    return;
  }

  try {
    cbs->init();
  } catch (const std::exception &e) {
    std::cerr << "[Statsig] ObservabilityClient.init callback failed: "
              << e.what() << std::endl;
  } catch (...) {
    std::cerr << "[Statsig] ObservabilityClient.init callback failed with a "
                 "non-std exception"
              << std::endl;
  }
}

void dispatch_metric(std::size_t slot, const char *args_ptr,
                     uint64_t args_length, MetricKind kind) {
  CallbacksPtr cbs = load_callbacks(slot);
  // Short-circuit before the JSON parse when the target callback is unset;
  // metrics can be hot-path. The args pointer must still be freed.
  if (cbs == nullptr || !metric_fn(*cbs, kind)) {
    free_args(args_ptr);
    return;
  }

  std::string raw = consume_args(args_ptr, args_length);
  try {
    json j = json::parse(raw);
    std::string metric = j.value("metric", std::string{});
    double value = j.value("value", 0.0);
    ObservabilityClientCallbacks::Tags tags = parse_tags(j);

    metric_fn(*cbs, kind)(metric, value, tags);
  } catch (const std::exception &e) {
    std::cerr << "[Statsig] ObservabilityClient metric callback failed: "
              << e.what() << std::endl;
  } catch (...) {
    std::cerr << "[Statsig] ObservabilityClient metric callback failed with a "
                 "non-std exception"
              << std::endl;
  }
}

void dispatch_error(std::size_t slot, const char *args_ptr,
                    uint64_t args_length) {
  CallbacksPtr cbs = load_callbacks(slot);
  if (cbs == nullptr || !cbs->error) {
    free_args(args_ptr);
    return;
  }

  std::string raw = consume_args(args_ptr, args_length);
  try {
    json j = json::parse(raw);
    cbs->error(j.value("tag", std::string{}), j.value("error", std::string{}));
  } catch (const std::exception &e) {
    std::cerr << "[Statsig] ObservabilityClient.error callback failed: "
              << e.what() << std::endl;
  } catch (...) {
    std::cerr << "[Statsig] ObservabilityClient.error callback failed with a "
                 "non-std exception"
              << std::endl;
  }
}

bool dispatch_should_enable(std::size_t slot, const char *args_ptr,
                            uint64_t args_length) {
  CallbacksPtr cbs = load_callbacks(slot);
  if (cbs == nullptr || !cbs->should_enable_high_cardinality_for_this_tag) {
    free_args(args_ptr);
    return false;
  }

  // The core passes the tag as a raw (non-JSON) string here.
  std::string tag = consume_args(args_ptr, args_length);
  try {
    return cbs->should_enable_high_cardinality_for_this_tag(tag);
  } catch (const std::exception &e) {
    std::cerr << "[Statsig] ObservabilityClient.should_enable_high_cardinality"
                 "_for_this_tag callback failed: "
              << e.what() << std::endl;
    return false;
  } catch (...) {
    std::cerr << "[Statsig] ObservabilityClient.should_enable_high_cardinality"
                 "_for_this_tag callback failed with a non-std exception"
              << std::endl;
    return false;
  }
}

template <std::size_t Slot> struct Trampoline {
  static void init() { dispatch_init(Slot); }
  static void increment(const char *p, uint64_t len) {
    dispatch_metric(Slot, p, len, MetricKind::Increment);
  }
  static void gauge(const char *p, uint64_t len) {
    dispatch_metric(Slot, p, len, MetricKind::Gauge);
  }
  static void dist(const char *p, uint64_t len) {
    dispatch_metric(Slot, p, len, MetricKind::Dist);
  }
  static void error(const char *p, uint64_t len) {
    dispatch_error(Slot, p, len);
  }
  static bool should_enable(const char *p, uint64_t len) {
    return dispatch_should_enable(Slot, p, len);
  }
};

using InitFn = void (*)();
using ArgFn = void (*)(const char *, uint64_t);
using BoolFn = bool (*)(const char *, uint64_t);

template <std::size_t... I>
constexpr std::array<InitFn, sizeof...(I)>
make_init_table(std::index_sequence<I...>) {
  return {{&Trampoline<I>::init...}};
}
template <std::size_t... I>
constexpr std::array<ArgFn, sizeof...(I)>
make_increment_table(std::index_sequence<I...>) {
  return {{&Trampoline<I>::increment...}};
}
template <std::size_t... I>
constexpr std::array<ArgFn, sizeof...(I)>
make_gauge_table(std::index_sequence<I...>) {
  return {{&Trampoline<I>::gauge...}};
}
template <std::size_t... I>
constexpr std::array<ArgFn, sizeof...(I)>
make_dist_table(std::index_sequence<I...>) {
  return {{&Trampoline<I>::dist...}};
}
template <std::size_t... I>
constexpr std::array<ArgFn, sizeof...(I)>
make_error_table(std::index_sequence<I...>) {
  return {{&Trampoline<I>::error...}};
}
template <std::size_t... I>
constexpr std::array<BoolFn, sizeof...(I)>
make_should_enable_table(std::index_sequence<I...>) {
  return {{&Trampoline<I>::should_enable...}};
}

const auto kInitTable = make_init_table(std::make_index_sequence<kMaxSlots>{});
const auto kIncrementTable =
    make_increment_table(std::make_index_sequence<kMaxSlots>{});
const auto kGaugeTable =
    make_gauge_table(std::make_index_sequence<kMaxSlots>{});
const auto kDistTable = make_dist_table(std::make_index_sequence<kMaxSlots>{});
const auto kErrorTable =
    make_error_table(std::make_index_sequence<kMaxSlots>{});
const auto kShouldEnableTable =
    make_should_enable_table(std::make_index_sequence<kMaxSlots>{});

int claim_slot(CallbacksPtr cbs) {
  std::lock_guard<std::mutex> lock(slots_mutex());
  auto &s = slots();
  for (std::size_t i = 0; i < kMaxSlots; ++i) {
    if (s[i] == nullptr) {
      s[i] = std::move(cbs);
      return static_cast<int>(i);
    }
  }
  return -1;
}

void release_slot(int slot) {
  if (slot < 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(slots_mutex());
  slots()[static_cast<std::size_t>(slot)] = nullptr;
}

} // namespace

ObservabilityClient::ObservabilityClient(ObservabilityClientCallbacks callbacks)
    : callbacks_(std::make_shared<const ObservabilityClientCallbacks>(
          std::move(callbacks))) {
  slot_ = claim_slot(callbacks_);
  if (slot_ < 0) {
    std::cerr << "[Statsig] Exhausted ObservabilityClient slots (max "
              << kMaxSlots << "); client not registered." << std::endl;
    return;
  }

  auto s = static_cast<std::size_t>(slot_);
  ref_ = observability_client_create(kInitTable[s], kIncrementTable[s],
                                     kGaugeTable[s], kDistTable[s],
                                     kErrorTable[s], kShouldEnableTable[s]);
  if (ref_ == 0) {
    std::cerr << "[Statsig] Failed to register observability client with the "
                 "native bridge."
              << std::endl;
    release_slot(slot_);
    slot_ = -1;
  }
}

ObservabilityClient::~ObservabilityClient() {
  if (ref_ != 0) {
    observability_client_release(ref_);
    ref_ = 0;
  }
  // Clear the slot after releasing the ref. Any callback still in flight holds
  // its own strong reference to the callbacks, so this cannot dangle.
  release_slot(slot_);
  slot_ = -1;
}

void ObservabilityClient::INTERNAL_test(const std::string &action,
                                        const std::string &metric_name,
                                        double value,
                                        const std::string &tags) const {
  __internal__test_observability_client(ref_, action.c_str(),
                                        metric_name.c_str(), value,
                                        tags.c_str());
}

} // namespace statsig_cpp_core
