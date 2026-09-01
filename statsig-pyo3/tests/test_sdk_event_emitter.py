import json
import threading
import time
from typing import Any, Callable, List

import pytest
from pytest_httpserver import HTTPServer
from werkzeug import Request, Response

from statsig_python_core import Statsig, StatsigOptions
from utils import get_test_data_resource

# The SDK clamps anything below 1000ms back to the default, so this is as fast
# as the background update thread can be driven.
SPECS_SYNC_INTERVAL_MS = 1000
WAIT_TIMEOUT_S = 15.0


class SpecsFeed:
    """Serves the DCS fixture with a newer lcut on every request, so each poll is
    a real spec update and `specs_updated` keeps firing for as long as we poll."""

    def __init__(self, httpserver: HTTPServer):
        self._specs = json.loads(get_test_data_resource("eval_proj_dcs.json"))
        del self._specs["checksum"]
        self._lock = threading.Lock()
        self._request_count = 0

        httpserver.expect_request(
            "/v2/download_config_specs/secret-key.json"
        ).respond_with_handler(self._respond)
        httpserver.expect_request("/v1/log_event").respond_with_json({"success": True})

    @property
    def request_count(self) -> int:
        with self._lock:
            return self._request_count

    def _respond(self, _request: Request) -> Response:
        with self._lock:
            self._request_count += 1
            self._specs["time"] += 1
            body = json.dumps(self._specs)

        return Response(body, content_type="application/json")


class EventRecorder:
    """Subscriber that collects the events the SDK emits on its update thread."""

    def __init__(self) -> None:
        self.events: List[Any] = []

    def __call__(self, event: Any) -> None:
        self.events.append(event)

    @property
    def count(self) -> int:
        return len(self.events)


def wait_until(predicate: Callable[[], bool], timeout_s: float = WAIT_TIMEOUT_S) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.01)
    return predicate()


@pytest.fixture
def specs_feed(httpserver: HTTPServer) -> SpecsFeed:
    return SpecsFeed(httpserver)


@pytest.fixture
def statsig(httpserver: HTTPServer, specs_feed: SpecsFeed):
    options = StatsigOptions()
    options.specs_url = httpserver.url_for("/v2/download_config_specs")
    options.log_event_url = httpserver.url_for("/v1/log_event")
    options.specs_sync_interval_ms = SPECS_SYNC_INTERVAL_MS
    options.output_log_level = "error"

    statsig = Statsig("secret-key", options)
    yield statsig
    statsig.shutdown().wait()


def test_specs_updated_fires_with_payload(statsig):
    recorder = EventRecorder()
    statsig.subscribe("specs_updated", recorder)

    statsig.initialize().wait()

    assert wait_until(lambda: recorder.count > 0), "specs_updated never fired"

    event = recorder.events[0]
    assert event["event_name"] == "specs_updated"

    data = event["data"]
    assert data["source"] == "Network"
    assert data["source_api"].startswith("http://")
    assert isinstance(data["values"]["time"], int)
    assert "test_public" in data["values"]["feature_gates"]


def test_wildcard_subscription_receives_specs_updated(statsig):
    recorder = EventRecorder()
    statsig.subscribe("*", recorder)

    statsig.initialize().wait()

    assert wait_until(
        lambda: any(e["event_name"] == "specs_updated" for e in recorder.events)
    ), "'*' subscription never received specs_updated"


def test_unsubscribe_by_id_stops_only_that_subscription(statsig):
    kept, dropped = EventRecorder(), EventRecorder()
    statsig.subscribe("specs_updated", kept)
    dropped_id = statsig.subscribe("specs_updated", dropped)
    assert isinstance(dropped_id, str) and dropped_id

    statsig.initialize().wait()
    assert wait_until(lambda: dropped.count > 0), "specs_updated never fired"

    statsig.unsubscribe_by_id(dropped_id)
    dropped_at, kept_at = dropped.count, kept.count

    assert wait_until(lambda: kept.count >= kept_at + 2), "specs stopped updating"
    assert dropped.count == dropped_at


def test_unsubscribe_removes_every_subscription_for_the_event(statsig):
    first, second, wildcard = EventRecorder(), EventRecorder(), EventRecorder()
    statsig.subscribe("specs_updated", first)
    statsig.subscribe("specs_updated", second)
    statsig.subscribe("*", wildcard)

    statsig.initialize().wait()
    assert wait_until(
        lambda: first.count > 0 and second.count > 0
    ), "specs_updated never fired"

    statsig.unsubscribe("specs_updated")
    first_at, second_at, wildcard_at = first.count, second.count, wildcard.count

    assert wait_until(
        lambda: wildcard.count >= wildcard_at + 2
    ), "specs stopped updating"
    assert first.count == first_at
    assert second.count == second_at


def test_unsubscribe_all_stops_every_subscription(statsig, specs_feed):
    specs_updated, wildcard = EventRecorder(), EventRecorder()
    statsig.subscribe("specs_updated", specs_updated)
    statsig.subscribe("*", wildcard)

    statsig.initialize().wait()
    assert wait_until(
        lambda: specs_updated.count > 0 and wildcard.count > 0
    ), "specs_updated never fired"

    statsig.unsubscribe_all()
    specs_updated_at, wildcard_at = specs_updated.count, wildcard.count
    requests_at = specs_feed.request_count

    assert wait_until(
        lambda: specs_feed.request_count >= requests_at + 2
    ), "specs stopped updating"
    assert specs_updated.count == specs_updated_at
    assert wildcard.count == wildcard_at


def test_subscriber_exception_does_not_break_other_subscribers(statsig):
    healthy = EventRecorder()

    def raises(_event: Any) -> None:
        raise RuntimeError("subscriber blew up")

    statsig.subscribe("specs_updated", raises)
    statsig.subscribe("specs_updated", healthy)

    statsig.initialize().wait()

    assert wait_until(lambda: healthy.count >= 2), "throwing subscriber halted emission"
