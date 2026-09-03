"""Layer 2 frame handling, driven by the traffic generator's frame builder."""

from __future__ import annotations

import random

import pytest
import trafficgen
from trafficgen import build_frame, corrupt

TYPE_IPV4 = 0x0800
TYPE_IPV6 = 0x86DD
TYPE_ARP = 0x0806
BROADCAST = b"\xff" * 6
MULTICAST = bytes.fromhex("0180c200000e")


def test_a_well_formed_frame_is_accepted(live):
    result = live.send_frame(build_frame(payload=b"hello optilink"))

    assert result["result"] == "OK"
    assert result["fcs_ok"] is True
    assert result["ethertype"] == TYPE_IPV4
    assert result["vlan"] is False
    assert result["dst"] == "001122334455"
    assert result["src"] == "aabbccddeeff"


def test_a_single_flipped_bit_is_caught(live):
    frame = build_frame(payload=bytes(64))
    damaged = corrupt(frame, random.Random(7))

    assert damaged != frame
    result = live.send_frame(damaged)
    assert result["result"] == "BAD_FCS"
    assert result["fcs_ok"] is False
    assert live.stats()["eth"]["fcs_err"] == 1


def test_vlan_tag_is_decoded(live):
    result = live.send_frame(
        build_frame(ethertype=TYPE_IPV6, vlan=(5, 100), payload=bytes(40))
    )

    assert result["result"] == "OK"
    assert result["vlan"] is True
    assert result["qinq"] is False
    assert result["outer_vid"] == 100
    assert result["outer_pcp"] == 5
    # The reported ethertype is the encapsulated one, not the tag protocol id.
    assert result["ethertype"] == TYPE_IPV6


def test_double_tagged_frame_is_decoded(live):
    result = live.send_frame(
        build_frame(
            ethertype=TYPE_ARP,
            vlan=(1, 4000),
            inner_vlan=(2, 10),
            payload=bytes(40),
        )
    )

    assert result["result"] == "OK"
    assert result["qinq"] is True
    assert result["outer_vid"] == 4000
    assert result["inner_vid"] == 10
    assert result["ethertype"] == TYPE_ARP


@pytest.mark.parametrize(
    "dst,counter",
    [(BROADCAST, "bc"), (MULTICAST, "mc"), (bytes.fromhex("001122334455"), "uc")],
)
def test_destination_classification(live, dst, counter):
    live.send_frame(build_frame(dst=dst, payload=bytes(40)))

    counters = live.stats()["eth"]
    assert counters[counter] == 1
    # Broadcast must not also be counted as multicast, even though it has the
    # group bit set.
    assert sum(counters[key] for key in ("uc", "mc", "bc")) == 1


def test_a_runt_is_rejected_before_it_is_parsed(live):
    result = live.send_frame(bytes(63))

    assert result["result"] == "UNDERSIZE"
    assert live.stats()["eth"]["undersize"] == 1


def test_a_giant_is_rejected(live):
    result = live.send_frame(bytes(1527))

    assert result["result"] == "OVERSIZE"
    assert live.stats()["eth"]["oversize"] == 1


def test_minimum_and_maximum_legal_sizes_are_accepted(live):
    smallest = build_frame(payload=b"")
    assert len(smallest) == 64
    assert live.send_frame(smallest)["result"] == "OK"

    largest = build_frame(payload=bytes(1500))
    assert len(largest) == 1518
    assert live.send_frame(largest)["result"] == "OK"


def test_counters_reconcile_over_a_mixed_run(live):
    """The check that actually catches parser bugs.

    Any generator can count what it sent. What finds a frame class the
    firmware silently drops is reconciling the offered load against the
    device's own counters.
    """
    rng = random.Random(20260826)
    total = 300
    corrupted = 0
    accepted = 0

    for _ in range(total):
        frame = trafficgen.random_frame(rng)
        if rng.random() < 0.2:
            frame = corrupt(frame, rng)
            corrupted += 1
            assert live.send_frame(frame)["result"] == "BAD_FCS"
        else:
            assert live.send_frame(frame)["result"] == "OK"
            accepted += 1

    counters = live.stats()["eth"]
    assert counters["frames"] == total
    assert counters["fcs_err"] == corrupted
    assert counters["uc"] + counters["mc"] + counters["bc"] == total
    assert counters["vlan"] >= counters["qinq"] > 0


def test_traffic_does_not_disturb_the_link(live):
    """Frame parsing runs in the console task. If it were starving the link
    task, a burst of traffic would show up as lane transitions."""
    before = live.link()
    rng = random.Random(1)
    for _ in range(200):
        live.send_frame(trafficgen.random_frame(rng))
    after = live.link()

    assert after["active_mask"] == before["active_mask"] == 0x0F
    assert after["faults"] == before["faults"]
    for index, lane in enumerate(after["lanes"]):
        assert lane["transitions"] == before["lanes"][index]["transitions"]

    for name, counters in live.stats()["tasks"].items():
        assert counters["overruns"] == 0, f"{name} overran under load"
