using System.Collections.Generic;
using Newtonsoft.Json;
using Statsig;
using Xunit;

namespace Statsig.Tests
{
    public class ObservabilityClientTests
    {
        [Fact]
        public void ObservabilityClientInitTest()
        {
            using var client = new MockObservabilityClient();

            CallObservabilityClient(client, "init", "", 0, null);

            Assert.True(client.InitCalled);
        }

        [Fact]
        public void ObservabilityClientIncrementTest()
        {
            using var client = new MockObservabilityClient();

            CallObservabilityClient(client, "increment", "my_metric", 1,
                new Dictionary<string, string> { { "key", "a_value" } });

            var call = Assert.Single(client.IncrementCalls);
            Assert.Equal("my_metric", call.Metric);
            Assert.Equal(1, call.Value);
            Assert.NotNull(call.Tags);
            Assert.Equal("a_value", call.Tags!["key"]);
        }

        [Fact]
        public void ObservabilityClientGaugeTest()
        {
            using var client = new MockObservabilityClient();

            CallObservabilityClient(client, "gauge", "my_gauge", 2,
                new Dictionary<string, string> { { "key", "a_value" } });

            var call = Assert.Single(client.GaugeCalls);
            Assert.Equal("my_gauge", call.Metric);
            Assert.Equal(2, call.Value);
            Assert.Equal("a_value", call.Tags!["key"]);
        }

        [Fact]
        public void ObservabilityClientDistTest()
        {
            using var client = new MockObservabilityClient();

            CallObservabilityClient(client, "dist", "my_dist", 3,
                new Dictionary<string, string> { { "key", "a_value" } });

            var call = Assert.Single(client.DistCalls);
            Assert.Equal("my_dist", call.Metric);
            Assert.Equal(3, call.Value);
            Assert.Equal("a_value", call.Tags!["key"]);
        }

        [Fact]
        public void ObservabilityClientErrorTest()
        {
            using var client = new MockObservabilityClient();

            CallObservabilityClient(client, "error", "my_error_tag", 0,
                new Dictionary<string, string> { { "test_error", "the error message" } });

            var call = Assert.Single(client.ErrorCalls);
            Assert.Equal("my_error_tag", call.Tag);
            Assert.Equal("the error message", call.Error);
        }

        [Fact]
        public void ObservabilityClientShouldEnableHighCardinalityTest()
        {
            using var client = new MockObservabilityClient { ShouldEnableHighCardinalityReturn = true };

            CallObservabilityClient(client, "should_enable_high_cardinality_for_this_tag", "my_tag", 0, null);

            var call = Assert.Single(client.ShouldEnableHighCardinalityCalls);
            Assert.Equal("my_tag", call);
        }

        [Fact]
        public void ObservabilityClientBuilderTest()
        {
            using var client = new MockObservabilityClient();
            var builder = new StatsigOptionsBuilder();

            var result = builder.SetObservabilityClient(client);
            Assert.Same(builder, result);

            using var options = builder.Build();
            Assert.Same(client, options.ObservabilityClient);
            Assert.NotEqual(0UL, client.Reference);
        }

        private unsafe void CallObservabilityClient(
            MockObservabilityClient client,
            string action,
            string metricName,
            double value,
            Dictionary<string, string>? tags)
        {
            var tagsJson = tags != null ? JsonConvert.SerializeObject(tags) : "";
            var actionBytes = StatsigUtils.ToUtf8NullTerminated(action);
            var metricBytes = StatsigUtils.ToUtf8NullTerminated(metricName);
            var tagsBytes = StatsigUtils.ToUtf8NullTerminated(tagsJson);

            fixed (byte* actionPtr = actionBytes)
            fixed (byte* metricPtr = metricBytes)
            fixed (byte* tagsPtr = tagsBytes)
            {
                StatsigFFI.__internal__test_observability_client(
                    client.Reference, actionPtr, metricPtr, value, tagsPtr);
            }
        }
    }
}
