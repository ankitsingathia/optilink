"""Link bring-up, fault handling and recovery, driven end to end.

These run against the whole firmware image: scheduler, drivers, state
machines and console, all of it. The unit tests pin each module's behaviour
in isolation; what this file is for is the behaviour that only exists once
they are wired together and running on a real tick.
"""

from __future__ import annotations

import pytest

LANES = 4


def test_module_reaches_ready_and_reports_identity(fw):
    identity = fw.identity()
    assert identity["fw"] == "optilink"
    assert identity["lanes"] == str(LANES)

    assert fw.link()["module"] == "RESET"
    fw.tick(300)
    assert fw.link()["module"] == "READY"


def test_all_lanes_activate_with_transmitters_on(live):
    state = live.link()
    assert state["active_mask"] == 0x0F
    assert state["up"] is True
    for index, lane in enumerate(state["lanes"]):
        assert lane["state"] == "ACTIVATED", f"lane {index}"
        assert lane["tx_on"] is True
        assert lane["faults"] == 0


def test_power_up_is_not_instantaneous(fw):
    """The module must hold in PWR_UP while supplies settle.

    A module that jumps straight to READY would be advertising a data path
    before its bias is stable, and the first host read would get garbage.
    """
    fw.tick(25)
    assert fw.link()["module"] == "PWR_UP"
    fw.tick(20)
    assert fw.link()["module"] == "PWR_UP"
    fw.tick(30)
    assert fw.link()["module"] == "READY"


def test_los_is_debounced_before_the_lane_drops(live):
    """Two agreeing samples must not be enough; three must be.

    The link task runs every 10 ms and the debounce is three samples, so the
    lane survives 20 ms of asserted LOS and fails somewhere in the next 10.
    """
    live.inject("los", 1, 1)
    live.tick(20)
    assert live.link()["lanes"][1]["state"] == "ACTIVATED"

    live.tick(20)
    state = live.link()
    assert state["lanes"][1]["state"] == "FAULT"
    assert state["lanes"][1]["last_fault"] == "RX_LOS"


def test_a_faulted_lane_squelches_only_its_own_transmitter(live):
    live.inject("los", 2, 1)
    live.tick(50)

    lanes = live.link()["lanes"]
    assert lanes[2]["tx_on"] is False
    for index in (0, 1, 3):
        assert lanes[index]["state"] == "ACTIVATED", f"lane {index}"
        assert lanes[index]["tx_on"] is True, f"lane {index}"

    # Three of four lanes still carrying traffic is still a live module.
    assert live.link()["up"] is True
    assert live.link()["active_mask"] == 0x0B


def test_lane_recovers_after_the_signal_returns(live):
    live.inject("los", 0, 1)
    live.tick(50)
    assert live.link()["lanes"][0]["state"] == "FAULT"

    live.inject("los", 0, 0)
    live.tick(300)

    lane = live.link()["lanes"][0]
    assert lane["state"] == "ACTIVATED"
    assert lane["tx_on"] is True
    assert lane["faults"] == 1
    assert lane["retries"] == 1


@pytest.mark.parametrize(
    "signal,expected",
    [("los", "RX_LOS"), ("txfault", "TX_FAULT"), ("lol", "CDR_LOL")],
)
def test_each_fault_source_is_reported_distinctly(live, signal, expected):
    """Diagnosis depends on this. A dirty connector, a dead laser and a
    retimer that will not lock need different people to fix them."""
    live.inject(signal, 3, 1)
    live.tick(60)

    lane = live.link()["lanes"][3]
    assert lane["state"] == "FAULT"
    assert lane["last_fault"] == expected


def test_a_permanently_dead_lane_stops_retrying(live):
    """Retries are capped. A lane that cannot come up should stay down and
    visible rather than cycling its laser for the life of the system."""
    live.inject("txfault", 3, 1)
    live.tick(5000)

    lane = live.link()["lanes"][3]
    assert lane["state"] == "FAULT"
    assert lane["retries"] <= 5
    assert lane["tx_on"] is False


def test_module_removal_resets_every_lane(live):
    live.inject("present", 0)
    live.tick(30)

    state = live.link()
    assert state["module"] == "RESET"
    assert state["active_mask"] == 0
    assert all(lane["tx_on"] is False for lane in state["lanes"])

    live.inject("present", 1)
    live.tick(300)
    assert live.link()["active_mask"] == 0x0F


def test_low_power_parks_lanes_without_recording_faults(live):
    """Low power is a host request, not a failure, and must not inflate the
    fault counters a technician later reads."""
    before = live.link()["lanes"][0]["faults"]

    live.inject("lpmode", 1)
    live.tick(60)
    parked = live.link()
    assert parked["module"] == "LOW_PWR"
    assert parked["active_mask"] == 0
    assert parked["lanes"][0]["faults"] == before

    live.inject("lpmode", 0)
    live.tick(300)
    assert live.link()["active_mask"] == 0x0F


def test_disabling_a_lane_leaves_the_others_alone(live):
    live.set_lane(0, False)
    live.tick(100)

    state = live.link()
    assert state["lanes"][0]["state"] == "DEACTIVATED"
    assert state["active_mask"] == 0x0E

    live.set_lane(0, True)
    live.tick(300)
    assert live.link()["active_mask"] == 0x0F


def test_scheduler_meets_its_deadlines_under_load(live):
    """No task may fall a whole period behind. If the console or the link
    task overruns, every timing assertion in this file is meaningless."""
    for lane in range(LANES):
        live.inject("los", lane, 1)
    live.tick(1000)
    for lane in range(LANES):
        live.inject("los", lane, 0)
    live.tick(1000)

    tasks = live.stats()["tasks"]
    assert set(tasks) == {"cli", "link", "ddm", "cmis"}
    for name, counters in tasks.items():
        assert counters["overruns"] == 0, f"{name} overran: {counters}"
        assert counters["max_latency"] < counters["period"], name
    assert tasks["link"]["period"] == 10
    assert tasks["cli"]["period"] == 1
