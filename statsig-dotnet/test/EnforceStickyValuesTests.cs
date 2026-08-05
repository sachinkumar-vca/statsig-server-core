using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;
using WireMock.Matchers;
using WireMock.RequestBuilders;
using WireMock.ResponseBuilders;
using WireMock.Server;
using Xunit;

namespace Statsig.Tests
{
    /// <summary>
    /// Covers the EnforceOverrides / EnforceTargeting persistent-assignment
    /// options. Fixture (enforce_sticky_dcs.json): experiment `enforce_exp`
    /// with a console override rule matching userID `override-user`, a
    /// targeting gate passing only users with custom `targeted=yes`, and layer
    /// `enforce_layer` delegating to the experiment.
    /// </summary>
    public class EnforceStickyValuesTests : IAsyncLifetime
    {
        private readonly WireMockServer _mockServer;
        private Statsig? _statsig;
        private MockPersistentStorage? _storage;

        public EnforceStickyValuesTests()
        {
            _mockServer = WireMockServer.Start();

            _mockServer
                .Given(Request.Create()
                    .WithPath(new RegexMatcher(@"/v2/download_config_specs/.*\.json"))
                    .UsingGet())
                .RespondWith(Response.Create()
                    .WithStatusCode(200)
                    .WithHeader("Content-Type", "application/json")
                    .WithBody(TestUtils.LoadJsonFile("enforce_sticky_dcs.json")));

            _mockServer
                .Given(Request.Create().WithPath("/v1/log_event").UsingPost())
                .RespondWith(Response.Create()
                    .WithStatusCode(200)
                    .WithHeader("Content-Type", "application/json")
                    .WithBody("{\"success\": true}"));
        }

        public async Task InitializeAsync()
        {
            Statsig.RemoveSharedInstance();
            _storage = new MockPersistentStorage();

            var options = new StatsigOptionsBuilder()
                .SetSpecsURL($"{_mockServer.Urls[0]}/v2/download_config_specs")
                .SetLogEventURL($"{_mockServer.Urls[0]}/v1/log_event")
                // UserPersistedValues are only honored when a persistent
                // storage adapter is configured.
                .SetPersistentStorage(_storage)
                .Build();

            _statsig = new Statsig("secret-key", options);
            await _statsig.Initialize();
        }

        public async Task DisposeAsync()
        {
            if (_statsig != null)
            {
                await _statsig.Shutdown();
            }
            _storage?.Dispose();
            _mockServer.Stop();
            _mockServer.Dispose();
        }

        private static StatsigUser MakeUser(string userID, bool targeted)
        {
            return new StatsigUserBuilder()
                .SetUserID(userID)
                .AddCustomProperty("targeted", targeted ? "yes" : "no")
                .Build();
        }

        private static Dictionary<string, StickyValues> StickyValuesFor(
            string configName,
            string? configDelegate)
        {
            return new Dictionary<string, StickyValues>
            {
                [configName] = new StickyValues
                {
                    Value = true,
                    JsonValue = JObject.FromObject(new { value = "sticky_value" }),
                    RuleId = "sticky_rule_id",
                    GroupName = "Sticky Group",
                    SecondaryExposures = [],
                    UndelegatedSecondaryExposures = [],
                    ConfigDelegate = configDelegate,
                    Time = 1700000000000,
                }
            };
        }

        [Fact]
        public void TestFourArgConstructorLocksParameterOrder()
        {
            var options = new EvaluationOptions(
                false,
                StickyValuesFor("enforce_exp", null),
                true,
                false);

            Assert.False(options.DisableExposureLogging);
            Assert.NotNull(options.UserPersistedValues);
            Assert.True(options.EnforceOverrides);
            Assert.False(options.EnforceTargeting);

            var experiment = _statsig!.GetExperiment(
                MakeUser("override-user", true),
                "enforce_exp",
                options);

            Assert.Equal("override_value", experiment.Get("value", "err"));
        }

        [Fact]
        public void TestStickyValueWinsWithoutEnforceOverrides()
        {
            var experiment = _statsig!.GetExperiment(
                MakeUser("override-user", true),
                "enforce_exp",
                new EvaluationOptions(userPersistedValues: StickyValuesFor("enforce_exp", null)));

            Assert.Equal("sticky_value", experiment.Get("value", "err"));
            Assert.Equal("sticky_rule_id", experiment.RuleID);
        }

        [Fact]
        public void TestEnforceOverridesLetsOverrideWinOverSticky()
        {
            var experiment = _statsig!.GetExperiment(
                MakeUser("override-user", true),
                "enforce_exp",
                new EvaluationOptions(userPersistedValues: StickyValuesFor("enforce_exp", null))
                {
                    EnforceOverrides = true
                });

            Assert.Equal("override_value", experiment.Get("value", "err"));
            Assert.Equal("override_rule:userID:id_override", experiment.RuleID);
        }

        [Fact]
        public void TestEnforceOverridesKeepsStickyWhenNoOverrideMatches()
        {
            var experiment = _statsig!.GetExperiment(
                MakeUser("plain-user", true),
                "enforce_exp",
                new EvaluationOptions(userPersistedValues: StickyValuesFor("enforce_exp", null))
                {
                    EnforceOverrides = true
                });

            Assert.Equal("sticky_value", experiment.Get("value", "err"));
        }

        [Fact]
        public void TestEnforceTargetingKeepsStickyWhenStillTargeted()
        {
            var experiment = _statsig!.GetExperiment(
                MakeUser("plain-user", true),
                "enforce_exp",
                new EvaluationOptions(userPersistedValues: StickyValuesFor("enforce_exp", null))
                {
                    EnforceTargeting = true
                });

            Assert.Equal("sticky_value", experiment.Get("value", "err"));
        }

        [Fact]
        public void TestEnforceTargetingDropsStickyWhenNoLongerTargeted()
        {
            var experiment = _statsig!.GetExperiment(
                MakeUser("plain-user", false),
                "enforce_exp",
                new EvaluationOptions(userPersistedValues: StickyValuesFor("enforce_exp", null))
                {
                    EnforceTargeting = true
                });

            Assert.NotEqual("sticky_value", experiment.Get("value", "err"));
            Assert.Equal("targetingGate", experiment.RuleID);
        }

        [Fact]
        public void TestLayerStickyValueWinsWithoutEnforceOverrides()
        {
            var layer = _statsig!.GetLayer(
                MakeUser("override-user", true),
                "enforce_layer",
                new EvaluationOptions(
                    userPersistedValues: StickyValuesFor("enforce_layer", "enforce_exp")));

            Assert.Equal("sticky_value", layer.Get("value", "err"));
        }

        [Fact]
        public void TestLayerEnforceOverridesLetsOverrideWinOverSticky()
        {
            var layer = _statsig!.GetLayer(
                MakeUser("override-user", true),
                "enforce_layer",
                new EvaluationOptions(userPersistedValues: StickyValuesFor("enforce_layer", "enforce_exp"))
                {
                    EnforceOverrides = true
                });

            Assert.Equal("override_value", layer.Get("value", "err"));
        }

        [Fact]
        public void TestLayerEnforceTargetingDropsStickyWhenNoLongerTargeted()
        {
            var layer = _statsig!.GetLayer(
                MakeUser("plain-user", false),
                "enforce_layer",
                new EvaluationOptions(userPersistedValues: StickyValuesFor("enforce_layer", "enforce_exp"))
                {
                    EnforceTargeting = true
                });

            Assert.NotEqual("sticky_value", layer.Get("value", "err"));
        }
    }
}
