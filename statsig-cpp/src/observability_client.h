#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace statsig_cpp_core {

// User-provided callbacks for an ObservabilityClient. Any callback left unset
// is simply not invoked. `tags` may be empty when the core provides none.
struct ObservabilityClientCallbacks {
  using Tags = std::unordered_map<std::string, std::string>;

  std::function<void()> init;
  std::function<void(const std::string &metric, double value, const Tags &tags)>
      increment;
  std::function<void(const std::string &metric, double value, const Tags &tags)>
      gauge;
  std::function<void(const std::string &metric, double value, const Tags &tags)>
      dist;
  std::function<void(const std::string &tag, const std::string &error)> error;
  std::function<bool(const std::string &tag)>
      should_enable_high_cardinality_for_this_tag;
};

// Wraps a native observability client created through the C FFI. Register it on
// a StatsigOptionsBuilder via set_observability_client(). Keep the instance
// alive for as long as the owning Statsig instance is in use.
//
// At most 32 ObservabilityClient instances can be live at once, process-wide
// (the C FFI requires context-free function pointers, so each instance claims
// a slot from a fixed trampoline pool). Constructing one past that limit logs
// to stderr and leaves the client unregistered: ref() returns 0 and
// set_observability_client() will serialize null.
class ObservabilityClient {
public:
  explicit ObservabilityClient(ObservabilityClientCallbacks callbacks);
  ~ObservabilityClient();

  ObservabilityClient(const ObservabilityClient &) = delete;
  ObservabilityClient &operator=(const ObservabilityClient &) = delete;
  ObservabilityClient(ObservabilityClient &&) = delete;
  ObservabilityClient &operator=(ObservabilityClient &&) = delete;

  uint64_t ref() const { return ref_; }

  // Test-only helper that drives the native callbacks the same way the core
  // would, mirroring __internal__test_observability_client.
  void INTERNAL_test(const std::string &action, const std::string &metric_name,
                     double value, const std::string &tags) const;

private:
  // Shared with the trampoline slot pool so in-flight callbacks stay valid
  // even if this client is destroyed concurrently.
  std::shared_ptr<const ObservabilityClientCallbacks> callbacks_;
  uint64_t ref_ = 0;
  int slot_ = -1;
};

} // namespace statsig_cpp_core
