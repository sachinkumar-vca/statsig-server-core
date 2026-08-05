use napi_derive::napi;
use statsig_rust::{
    DynamicConfigEvaluationOptions, ExperimentEvaluationOptions, FeatureGateEvaluationOptions,
    LayerEvaluationOptions, ParameterStoreEvaluationOptions,
};
use std::collections::HashMap;

// -------------------------
//   Feature Gate Options
// -------------------------

#[napi(object, js_name = "FeatureGateEvaluationOptions")]
pub struct FeatureGateEvaluationOptionsNapi {
    pub disable_exposure_logging: Option<bool>,
}

impl From<FeatureGateEvaluationOptionsNapi> for FeatureGateEvaluationOptions {
    fn from(opts: FeatureGateEvaluationOptionsNapi) -> Self {
        FeatureGateEvaluationOptions {
            disable_exposure_logging: opts.disable_exposure_logging.unwrap_or(false),
        }
    }
}

// -------------------------
//   Dynamic Config Options
// -------------------------

#[napi(object, js_name = "DynamicConfigEvaluationOptions")]
pub struct DynamicConfigEvaluationOptionsNapi {
    pub disable_exposure_logging: Option<bool>,
}

impl From<DynamicConfigEvaluationOptionsNapi> for DynamicConfigEvaluationOptions {
    fn from(opts: DynamicConfigEvaluationOptionsNapi) -> Self {
        DynamicConfigEvaluationOptions {
            disable_exposure_logging: opts.disable_exposure_logging.unwrap_or(false),
        }
    }
}

// -------------------------
//   Experiment Options
// -------------------------

#[napi(object, js_name = "ExperimentEvaluationOptions")]
pub struct ExperimentEvaluationOptionsNapi {
    pub disable_exposure_logging: Option<bool>,
    pub user_persisted_values: Option<HashMap<String, serde_json::Value>>,
    /// When a persisted sticky value exists, let a matching console override
    /// rule take precedence over it.
    pub enforce_overrides: Option<bool>,
    /// When a persisted sticky value exists, re-check targeting and drop the
    /// sticky value if the user no longer passes targeting.
    pub enforce_targeting: Option<bool>,
}

impl From<ExperimentEvaluationOptionsNapi> for ExperimentEvaluationOptions {
    fn from(opts: ExperimentEvaluationOptionsNapi) -> Self {
        ExperimentEvaluationOptions {
            disable_exposure_logging: opts.disable_exposure_logging.unwrap_or(false),
            user_persisted_values: opts.user_persisted_values.and_then(|values| {
                serde_json::from_value(serde_json::Value::Object(values.into_iter().collect())).ok()
            }),
            enforce_overrides: opts.enforce_overrides.unwrap_or(false),
            enforce_targeting: opts.enforce_targeting.unwrap_or(false),
        }
    }
}

// -------------------------
//   Layer Options
// -------------------------

#[napi(object, js_name = "LayerEvaluationOptions")]
pub struct LayerEvaluationOptionsNapi {
    pub disable_exposure_logging: Option<bool>,
    pub user_persisted_values: Option<HashMap<String, serde_json::Value>>,
    /// When a persisted sticky value exists, let a matching console override
    /// rule take precedence over it.
    pub enforce_overrides: Option<bool>,
    /// When a persisted sticky value exists, re-check targeting and drop the
    /// sticky value if the user no longer passes targeting.
    pub enforce_targeting: Option<bool>,
}

impl From<LayerEvaluationOptionsNapi> for LayerEvaluationOptions {
    fn from(opts: LayerEvaluationOptionsNapi) -> Self {
        LayerEvaluationOptions {
            disable_exposure_logging: opts.disable_exposure_logging.unwrap_or(false),
            user_persisted_values: opts.user_persisted_values.and_then(|values| {
                serde_json::from_value(serde_json::Value::Object(values.into_iter().collect())).ok()
            }),
            enforce_overrides: opts.enforce_overrides.unwrap_or(false),
            enforce_targeting: opts.enforce_targeting.unwrap_or(false),
        }
    }
}

// -------------------------
//   Parameter Store Options
// -------------------------

#[napi(object, js_name = "ParameterStoreEvaluationOptions")]
pub struct ParameterStoreEvaluationOptionsNapi {
    pub disable_exposure_logging: Option<bool>,
}

impl From<ParameterStoreEvaluationOptionsNapi> for ParameterStoreEvaluationOptions {
    fn from(opts: ParameterStoreEvaluationOptionsNapi) -> Self {
        ParameterStoreEvaluationOptions {
            disable_exposure_logging: opts.disable_exposure_logging.unwrap_or(false),
        }
    }
}
