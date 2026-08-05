import json
import os

import pytest
from pytest_httpserver import HTTPServer
from statsig_python_core import (
    ExperimentEvaluationOptions,
    LayerEvaluationOptions,
    PersistentStorage,
    Statsig,
    StatsigOptions,
    StatsigUser,
)

# Covers the enforce_overrides / enforce_targeting persistent-assignment
# options. Fixture (enforce_sticky_dcs.json): experiment `enforce_exp` with a
# console override rule matching userID `override-user`, a targeting gate
# passing only users with custom `targeted=yes`, and layer `enforce_layer`
# delegating to the experiment.
#
# The fixture's rule ids are load-bearing: core classifies rules via
# `Rule::is_override_rule` (id ends with "override") and
# `Rule::is_targeting_rule` (id == "targetingGate" / "inlineTargetingRules")
# in statsig-rust/src/specs_response/spec_types.rs, so the
# `override_rule:userID:id_override` and `targetingGate` ids must keep those
# shapes for the enforcement paths to trigger.


def load_fixture() -> dict:
    root = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(root, "data", "enforce_sticky_dcs.json"), "r") as file:
        return json.loads(file.read())


def sticky_values(config_name: str, config_delegate=None) -> dict:
    sticky = {
        "value": True,
        "json_value": {"value": "sticky_value"},
        "rule_id": "sticky_rule_id",
        "group_name": "Sticky Group",
        "secondary_exposures": [],
        "undelegated_secondary_exposures": [],
        "config_delegate": config_delegate,
        "explicit_parameters": None,
        "time": 1700000000000,
    }
    return {config_name: sticky}


def make_user(user_id: str, targeted: bool) -> StatsigUser:
    return StatsigUser(
        user_id, custom={"targeted": "yes" if targeted else "no"}
    )


@pytest.fixture
def statsig_setup(httpserver: HTTPServer):
    httpserver.expect_request(
        "/v2/download_config_specs/secret-key.json"
    ).respond_with_json(load_fixture())
    httpserver.expect_request("/v1/log_event").respond_with_json({"success": True})

    options = StatsigOptions(
        specs_url=httpserver.url_for("/v2/download_config_specs"),
        log_event_url=httpserver.url_for("/v1/log_event"),
        output_log_level="error",
        # user_persisted_values are only honored when a persistent storage
        # adapter is configured.
        persistent_storage=PersistentStorage(),
    )
    statsig = Statsig("secret-key", options)
    statsig.initialize().wait()

    yield statsig

    statsig.shutdown().wait()


def test_sticky_value_wins_without_enforce_overrides(statsig_setup):
    statsig = statsig_setup
    experiment = statsig.get_experiment(
        make_user("override-user", True),
        "enforce_exp",
        ExperimentEvaluationOptions(
            user_persisted_values=sticky_values("enforce_exp")
        ),
    )
    assert experiment.get_string("value", "ERR") == "sticky_value"
    assert experiment.rule_id == "sticky_rule_id"


def test_enforce_overrides_lets_override_win_over_sticky(statsig_setup):
    statsig = statsig_setup
    experiment = statsig.get_experiment(
        make_user("override-user", True),
        "enforce_exp",
        ExperimentEvaluationOptions(
            user_persisted_values=sticky_values("enforce_exp"),
            enforce_overrides=True,
        ),
    )
    assert experiment.get_string("value", "ERR") == "override_value"
    assert experiment.rule_id == "override_rule:userID:id_override"


def test_enforce_overrides_keeps_sticky_when_no_override_matches(statsig_setup):
    statsig = statsig_setup
    experiment = statsig.get_experiment(
        make_user("plain-user", True),
        "enforce_exp",
        ExperimentEvaluationOptions(
            user_persisted_values=sticky_values("enforce_exp"),
            enforce_overrides=True,
        ),
    )
    assert experiment.get_string("value", "ERR") == "sticky_value"


def test_enforce_targeting_keeps_sticky_when_still_targeted(statsig_setup):
    statsig = statsig_setup
    experiment = statsig.get_experiment(
        make_user("plain-user", True),
        "enforce_exp",
        ExperimentEvaluationOptions(
            user_persisted_values=sticky_values("enforce_exp"),
            enforce_targeting=True,
        ),
    )
    assert experiment.get_string("value", "ERR") == "sticky_value"


def test_enforce_targeting_drops_sticky_when_no_longer_targeted(statsig_setup):
    statsig = statsig_setup
    experiment = statsig.get_experiment(
        make_user("plain-user", False),
        "enforce_exp",
        ExperimentEvaluationOptions(
            user_persisted_values=sticky_values("enforce_exp"),
            enforce_targeting=True,
        ),
    )
    assert experiment.get_string("value", "ERR") != "sticky_value"
    assert experiment.rule_id == "targetingGate"


def test_layer_sticky_value_wins_without_enforce_overrides(statsig_setup):
    statsig = statsig_setup
    layer = statsig.get_layer(
        make_user("override-user", True),
        "enforce_layer",
        LayerEvaluationOptions(
            user_persisted_values=sticky_values("enforce_layer", "enforce_exp")
        ),
    )
    assert layer.get_string("value", "ERR") == "sticky_value"


def test_layer_enforce_overrides_lets_override_win_over_sticky(statsig_setup):
    statsig = statsig_setup
    layer = statsig.get_layer(
        make_user("override-user", True),
        "enforce_layer",
        LayerEvaluationOptions(
            user_persisted_values=sticky_values("enforce_layer", "enforce_exp"),
            enforce_overrides=True,
        ),
    )
    assert layer.get_string("value", "ERR") == "override_value"


def test_layer_enforce_overrides_keeps_sticky_when_no_override_matches(statsig_setup):
    statsig = statsig_setup
    layer = statsig.get_layer(
        make_user("plain-user", True),
        "enforce_layer",
        LayerEvaluationOptions(
            user_persisted_values=sticky_values("enforce_layer", "enforce_exp"),
            enforce_overrides=True,
        ),
    )
    assert layer.get_string("value", "ERR") == "sticky_value"


def test_layer_enforce_targeting_keeps_sticky_when_still_targeted(statsig_setup):
    statsig = statsig_setup
    layer = statsig.get_layer(
        make_user("plain-user", True),
        "enforce_layer",
        LayerEvaluationOptions(
            user_persisted_values=sticky_values("enforce_layer", "enforce_exp"),
            enforce_targeting=True,
        ),
    )
    assert layer.get_string("value", "ERR") == "sticky_value"


def test_layer_enforce_targeting_drops_sticky_when_no_longer_targeted(statsig_setup):
    statsig = statsig_setup
    layer = statsig.get_layer(
        make_user("plain-user", False),
        "enforce_layer",
        LayerEvaluationOptions(
            user_persisted_values=sticky_values("enforce_layer", "enforce_exp"),
            enforce_targeting=True,
        ),
    )
    assert layer.get_string("value", "ERR") != "sticky_value"
