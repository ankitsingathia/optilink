"""Shared fixtures for the integration suite."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(Path(__file__).parent))

from optilink_harness import Optilink, default_binary  # noqa: E402


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption(
        "--binary",
        action="store",
        default=None,
        help="path to the host build of the firmware",
    )


@pytest.fixture
def fw(request: pytest.FixtureRequest):
    """A freshly started firmware instance, one per test.

    Per test rather than per session on purpose: the firmware holds latched
    flags, counters and state machine history, and a test that inherited
    another test's latches would pass or fail depending on collection order.
    Startup is a few milliseconds, so there is nothing to buy by sharing.
    """
    binary = request.config.getoption("--binary") or default_binary()
    with Optilink(binary) as instance:
        yield instance


@pytest.fixture
def live(fw):
    """A firmware instance already brought up to a working link."""
    fw.tick(300)
    state = fw.link()
    assert state["up"], f"link failed to come up: {state}"
    return fw
