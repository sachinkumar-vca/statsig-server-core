using System.Collections.Generic;
using Newtonsoft.Json;

namespace Statsig
{
    /// <summary>
    /// Configuration options for the GetClientInitializeResponse method in the Statsig Server SDK.
    ///
    /// The persistent-assignment fields (<see cref="UserPersistedValues"/>,
    /// <see cref="EnforceOverrides"/>, <see cref="EnforceTargeting"/>) only
    /// affect <c>GetExperiment</c> and <c>GetLayer</c>; they are ignored by
    /// <c>CheckGate</c> / <c>GetFeatureGate</c> / <c>GetDynamicConfig</c>.
    /// </summary>
    public class EvaluationOptions
    {
        [JsonProperty("disable_exposure_logging")]
        public bool DisableExposureLogging { get; set; }

        [JsonProperty("user_persisted_values", NullValueHandling = NullValueHandling.Ignore)]
        public Dictionary<string, StickyValues>? UserPersistedValues { get; set; }

        /// <summary>
        /// When a persisted sticky value exists, let a matching console override
        /// rule take precedence over it.
        /// </summary>
        [JsonProperty("enforce_overrides")]
        public bool EnforceOverrides { get; set; }

        /// <summary>
        /// When a persisted sticky value exists, re-check targeting and drop the
        /// sticky value if the user no longer passes targeting.
        /// </summary>
        [JsonProperty("enforce_targeting")]
        public bool EnforceTargeting { get; set; }

        public EvaluationOptions(bool disableExposureLogging = false, Dictionary<string, StickyValues>? userPersistedValues = null)
        {
            DisableExposureLogging = disableExposureLogging;
            UserPersistedValues = userPersistedValues;
        }

        public EvaluationOptions(
            bool disableExposureLogging,
            Dictionary<string, StickyValues>? userPersistedValues,
            bool enforceOverrides,
            bool enforceTargeting)
            : this(disableExposureLogging, userPersistedValues)
        {
            EnforceOverrides = enforceOverrides;
            EnforceTargeting = enforceTargeting;
        }
    }
}
