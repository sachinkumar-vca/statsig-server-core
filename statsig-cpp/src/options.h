#pragma once
#include "observability_client.h"
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

using json = nlohmann::json;
namespace statsig_cpp_core {

struct StatsigOptions {
  uint64_t ref;
  StatsigOptions() = default;
  StatsigOptions(const uint64_t ref) { this->ref = ref; }
  ~StatsigOptions();
};
struct StatsigOptionsBuilder {
public:
  std::optional<std::string> specs_url;
  std::optional<std::string> id_lists_url;
  std::optional<std::string> log_event_url;
  std::optional<std::string> output_log_level;
  std::optional<std::string> environment;
  bool enable_id_lists = false;
  bool enable_dcs_deltas = false;
  bool disable_all_logging = false;
  bool disable_country_lookup = false;
  bool disable_network = false;
  std::optional<uint32_t> exposure_dedupe_max_keys;
  // Non-owning: the caller owns the ObservabilityClient and must keep it alive
  // for the lifetime of the resulting Statsig instance.
  std::optional<uint64_t> observability_client_ref;
  StatsigOptionsBuilder() = default;
  StatsigOptionsBuilder &
  set_observability_client(const ObservabilityClient &client) {
    // A ref of 0 means native registration failed (e.g. slot exhaustion);
    // serialize null rather than an explicit-but-invalid ref.
    if (client.ref() != 0) {
      observability_client_ref = client.ref();
    } else {
      observability_client_ref = std::nullopt;
    }
    return *this;
  }
  StatsigOptions build();
};

inline void to_json(json &j, const StatsigOptionsBuilder &b) {
  j = json{{"specs_url", b.specs_url},
           {"id_lists_url", b.id_lists_url},
           {"log_event_url", b.log_event_url},
           {"environment", b.environment},
           {"output_log_level", b.output_log_level},
           {"enable_id_lists", b.enable_id_lists},
           {"enable_dcs_deltas", b.enable_dcs_deltas},
           {"disable_all_logging", b.disable_all_logging},
           {"disable_country_lookup", b.disable_country_lookup},
           {"disable_network", b.disable_network},
           {"exposure_dedupe_max_keys", b.exposure_dedupe_max_keys},
           {"observability_client_ref", b.observability_client_ref}};
}

struct CheckGateOptions {
  bool disableExposureLogging;
};

inline void to_json(json &j, const CheckGateOptions &b) {
  j = json{
      {"disable_exposure_logging", b.disableExposureLogging},
  };
}

struct GetDynamicConfigOptions {
  bool disableExposureLogging;
};

inline void to_json(json &j, const GetDynamicConfigOptions &b) {
  j = json{
      {"disable_exposure_logging", b.disableExposureLogging},
  };
}

struct GetExperimentOptions {
  bool disableExposureLogging;
};

inline void to_json(json &j, const GetExperimentOptions &b) {
  j = json{
      {"disable_exposure_logging", b.disableExposureLogging},
  };
}

struct GetLayerOptions {
  bool disableExposureLogging;
};

inline void to_json(json &j, const GetLayerOptions &b) {
  j = json{
      {"disable_exposure_logging", b.disableExposureLogging},
  };
}
}; // namespace statsig_cpp_core
