#include "../src/observability_client.h"
#include "../src/options.h"
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <unordered_map>

using statsig_cpp_core::ObservabilityClient;
using statsig_cpp_core::ObservabilityClientCallbacks;
using statsig_cpp_core::StatsigOptionsBuilder;

namespace {

struct Recorder {
  bool init_called = false;

  bool increment_called = false;
  bool gauge_called = false;
  bool dist_called = false;
  std::string metric;
  double value = 0;
  ObservabilityClientCallbacks::Tags tags;

  bool error_called = false;
  std::string error_tag;
  std::string error_message;

  bool should_enable_called = false;
  std::string should_enable_tag;
  bool should_enable_return = false;
};

ObservabilityClientCallbacks make_callbacks(Recorder &r) {
  ObservabilityClientCallbacks cb;
  cb.init = [&r]() { r.init_called = true; };
  cb.increment = [&r](const std::string &m, double v,
                      const ObservabilityClientCallbacks::Tags &t) {
    r.increment_called = true;
    r.metric = m;
    r.value = v;
    r.tags = t;
  };
  cb.gauge = [&r](const std::string &m, double v,
                  const ObservabilityClientCallbacks::Tags &t) {
    r.gauge_called = true;
    r.metric = m;
    r.value = v;
    r.tags = t;
  };
  cb.dist = [&r](const std::string &m, double v,
                 const ObservabilityClientCallbacks::Tags &t) {
    r.dist_called = true;
    r.metric = m;
    r.value = v;
    r.tags = t;
  };
  cb.error = [&r](const std::string &tag, const std::string &err) {
    r.error_called = true;
    r.error_tag = tag;
    r.error_message = err;
  };
  cb.should_enable_high_cardinality_for_this_tag =
      [&r](const std::string &tag) {
        r.should_enable_called = true;
        r.should_enable_tag = tag;
        return r.should_enable_return;
      };
  return cb;
}

} // namespace

TEST(ObservabilityClient, RegistersNativeRef) {
  Recorder r;
  ObservabilityClient client(make_callbacks(r));
  EXPECT_NE(client.ref(), 0u);
}

TEST(ObservabilityClient, Init) {
  Recorder r;
  ObservabilityClient client(make_callbacks(r));
  client.INTERNAL_test("init", "", 0, "");
  EXPECT_TRUE(r.init_called);
}

TEST(ObservabilityClient, Increment) {
  Recorder r;
  ObservabilityClient client(make_callbacks(r));
  client.INTERNAL_test("increment", "my_metric", 1, R"({"key":"a_value"})");
  EXPECT_TRUE(r.increment_called);
  EXPECT_EQ(r.metric, "my_metric");
  EXPECT_EQ(r.value, 1);
  EXPECT_EQ(r.tags.at("key"), "a_value");
}

TEST(ObservabilityClient, Gauge) {
  Recorder r;
  ObservabilityClient client(make_callbacks(r));
  client.INTERNAL_test("gauge", "my_gauge", 2, R"({"key":"a_value"})");
  EXPECT_TRUE(r.gauge_called);
  EXPECT_EQ(r.metric, "my_gauge");
  EXPECT_EQ(r.value, 2);
  EXPECT_EQ(r.tags.at("key"), "a_value");
}

TEST(ObservabilityClient, Dist) {
  Recorder r;
  ObservabilityClient client(make_callbacks(r));
  client.INTERNAL_test("dist", "my_dist", 3, R"({"key":"a_value"})");
  EXPECT_TRUE(r.dist_called);
  EXPECT_EQ(r.metric, "my_dist");
  EXPECT_EQ(r.value, 3);
  EXPECT_EQ(r.tags.at("key"), "a_value");
}

TEST(ObservabilityClient, Error) {
  Recorder r;
  ObservabilityClient client(make_callbacks(r));
  client.INTERNAL_test("error", "my_error_tag", 0,
                       R"({"test_error":"the error message"})");
  EXPECT_TRUE(r.error_called);
  EXPECT_EQ(r.error_tag, "my_error_tag");
  EXPECT_EQ(r.error_message, "the error message");
}

TEST(ObservabilityClient, ShouldEnableHighCardinality) {
  Recorder r;
  r.should_enable_return = true;
  ObservabilityClient client(make_callbacks(r));
  client.INTERNAL_test("should_enable_high_cardinality_for_this_tag", "my_tag",
                       0, "");
  EXPECT_TRUE(r.should_enable_called);
  EXPECT_EQ(r.should_enable_tag, "my_tag");
}

TEST(ObservabilityClient, MultipleClientsRouteIndependently) {
  Recorder r1;
  Recorder r2;
  ObservabilityClient client1(make_callbacks(r1));
  ObservabilityClient client2(make_callbacks(r2));

  client1.INTERNAL_test("increment", "metric_one", 1, "{}");
  client2.INTERNAL_test("increment", "metric_two", 2, "{}");

  EXPECT_TRUE(r1.increment_called);
  EXPECT_EQ(r1.metric, "metric_one");
  EXPECT_TRUE(r2.increment_called);
  EXPECT_EQ(r2.metric, "metric_two");
}

// Callbacks are invoked across the FFI boundary, so a throw must never
// propagate; verify both std and non-std exceptions are contained and the
// client keeps working afterwards.
TEST(ObservabilityClient, ThrowingCallbacksAreContained) {
  Recorder r;
  ObservabilityClientCallbacks cb = make_callbacks(r);
  cb.init = []() { throw std::runtime_error("init boom"); };
  cb.increment = [](const std::string &, double,
                    const ObservabilityClientCallbacks::Tags &) {
    throw 42; // non-std exception
  };
  cb.should_enable_high_cardinality_for_this_tag = [](const std::string &) -> bool {
    throw std::runtime_error("should_enable boom");
  };
  ObservabilityClient client(std::move(cb));

  client.INTERNAL_test("init", "", 0, "");
  client.INTERNAL_test("increment", "m", 1, "{}");
  client.INTERNAL_test("should_enable_high_cardinality_for_this_tag", "lcut",
                       0, "");

  // The client is still functional after callbacks threw.
  client.INTERNAL_test("gauge", "still_alive", 5, "{}");
  EXPECT_TRUE(r.gauge_called);
  EXPECT_EQ(r.metric, "still_alive");
}

TEST(ObservabilityClient, BuilderSerializesRef) {
  Recorder r;
  ObservabilityClient client(make_callbacks(r));

  StatsigOptionsBuilder builder;
  builder.set_observability_client(client);

  json j;
  to_json(j, builder);
  EXPECT_EQ(j["observability_client_ref"], client.ref());

  StatsigOptionsBuilder unset_builder;
  json j_unset;
  to_json(j_unset, unset_builder);
  EXPECT_TRUE(j_unset["observability_client_ref"].is_null());
}
