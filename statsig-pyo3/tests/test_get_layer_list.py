"""
Tests focused on the get_layer_list API.
Verifies the configured layer names are returned.
"""

import json
import pytest
from statsig_python_core import Statsig, StatsigOptions
from pytest_httpserver import HTTPServer
from utils import get_test_data_resource


@pytest.fixture
def statsig_setup(httpserver: HTTPServer):
    dcs_content = get_test_data_resource("eval_proj_dcs.json")
    json_data = json.loads(dcs_content)

    httpserver.expect_request(
        "/v2/download_config_specs/secret-key.json"
    ).respond_with_json(json_data)

    httpserver.expect_request("/v1/log_event").respond_with_json({"success": True})

    options = StatsigOptions(
        specs_url=httpserver.url_for("/v2/download_config_specs"),
        log_event_url=httpserver.url_for("/v1/log_event"),
    )
    statsig = Statsig("secret-key", options)

    statsig.initialize().wait()

    yield statsig

    statsig.shutdown().wait()


def test_get_layer_list_returns_configured_layers(statsig_setup):
    statsig = statsig_setup
    layer_list = statsig.get_layer_list()

    assert isinstance(layer_list, list)
    assert "test_layer" in layer_list
