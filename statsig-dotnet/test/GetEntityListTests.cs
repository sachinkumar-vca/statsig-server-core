using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Xunit;

namespace Statsig.Tests
{
    public class GetEntityListTests
    {
        public static TheoryData<string, string> EntityListCases() => new()
        {
            { "GetFeatureGateList", "test_country_partial" },
            { "GetDynamicConfigList", "operating_system_config" },
            { "GetExperimentList", "test_experiment_no_targeting" },
            { "GetAutotuneList", "test_autotune" },
            { "GetLayerList", "country_test_layer" },
            { "GetParameterStoreList", "gateParamsStore" },
        };

        private static List<string> InvokeListMethod(Statsig statsig, string methodName) => methodName switch
        {
            "GetFeatureGateList" => statsig.GetFeatureGateList(),
            "GetDynamicConfigList" => statsig.GetDynamicConfigList(),
            "GetExperimentList" => statsig.GetExperimentList(),
            "GetAutotuneList" => statsig.GetAutotuneList(),
            "GetLayerList" => statsig.GetLayerList(),
            "GetParameterStoreList" => statsig.GetParameterStoreList(),
            _ => throw new ArgumentOutOfRangeException(nameof(methodName), methodName, "Unknown entity list method"),
        };

        [Theory]
        [MemberData(nameof(EntityListCases))]
        public async Task GetEntityList_ReturnsConfiguredEntity(string methodName, string expectedMember)
        {
            Statsig.RemoveSharedInstance();
            var cachedSpecs = TestUtils.LoadJsonFile("eval_proj_dcs.json");

            using var dataStore = new MockDataStore
            {
                NextGetResponse = new DataStoreResponse(cachedSpecs, 999),
                ShouldReturnPolling = true,
            };

            using var options = new StatsigOptionsBuilder()
                .SetDataStore(dataStore)
                .SetDisableNetwork(true)
                .Build();

            using var statsig = new Statsig("secret-key", options);
            await statsig.Initialize();

            var list = InvokeListMethod(statsig, methodName);

            Assert.NotNull(list);
            Assert.Contains(expectedMember, list);

            await statsig.Shutdown();
        }
    }
}
