using System.Collections.Generic;
using Statsig;

namespace Statsig.Tests
{
    public class MockObservabilityClient : ObservabilityClient
    {
        public bool InitCalled { get; private set; }
        public IList<MetricCall> IncrementCalls { get; } = [];
        public IList<MetricCall> GaugeCalls { get; } = [];
        public IList<MetricCall> DistCalls { get; } = [];
        public IList<ErrorCall> ErrorCalls { get; } = [];
        public IList<string> ShouldEnableHighCardinalityCalls { get; } = [];
        public bool ShouldEnableHighCardinalityReturn { get; set; }

        public override void Init() => InitCalled = true;

        public override void Increment(string metric, double value, Dictionary<string, string>? tags)
        {
            IncrementCalls.Add(new MetricCall(metric, value, tags));
        }

        public override void Gauge(string metric, double value, Dictionary<string, string>? tags)
        {
            GaugeCalls.Add(new MetricCall(metric, value, tags));
        }

        public override void Dist(string metric, double value, Dictionary<string, string>? tags)
        {
            DistCalls.Add(new MetricCall(metric, value, tags));
        }

        public override void Error(string tag, string error)
        {
            ErrorCalls.Add(new ErrorCall(tag, error));
        }

        public override bool ShouldEnableHighCardinalityForThisTag(string tag)
        {
            ShouldEnableHighCardinalityCalls.Add(tag);
            return ShouldEnableHighCardinalityReturn;
        }

        public sealed record MetricCall(string Metric, double Value, Dictionary<string, string>? Tags);
        public sealed record ErrorCall(string Tag, string Error);
    }
}
