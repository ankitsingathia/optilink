"""The host facing management interface, exercised the way a host uses it."""

from __future__ import annotations

import pytest
from optilink_harness import OptilinkError

# Lower page.
OFF_IDENTIFIER = 0x00
OFF_REVISION = 0x01
OFF_MEM_MODEL = 0x02
OFF_MODULE_STATE = 0x03
OFF_TEMP = 0x0E
OFF_VCC = 0x10
OFF_ACTIVE_LANES = 0x12
OFF_BANK_SELECT = 0x7E
OFF_PAGE_SELECT = 0x7F

# Upper pages.
P00_VENDOR_NAME = 0x81
P00_VENDOR_PN = 0x94
P01_LANE_COUNT = 0x86
P02_TEMP_TH = 0x80
P02_RX_POWER_TH = 0xA8
P10_TX_DISABLE = 0x82
P11_DP_STATE = 0x80
P11_RX_POWER = 0x98

ID_QSFP_DD = 0x18
REV_CMIS_5_0 = 0x50

MODULE_STATE_READY = 3


def test_identity_bytes_match_a_qsfp_dd_module(live):
    page = live.i2c_read(OFF_IDENTIFIER, 3)
    assert page[0] == ID_QSFP_DD
    assert page[1] == REV_CMIS_5_0
    assert page[2] & 0x80 == 0, "advertises paged memory, not flat"


def test_module_state_is_published_in_the_lower_page(live):
    state = live.i2c_read(OFF_MODULE_STATE, 1)[0]
    assert (state >> 1) & 0x07 == MODULE_STATE_READY


def test_monitors_are_readable_without_selecting_a_page(live):
    """The lower page is always mapped. A host that understands nothing else
    about this module can still read its temperature."""
    temperature = live.read_u16(OFF_TEMP, signed=True)
    supply = live.read_u16(OFF_VCC)

    assert 39.0 < temperature / 256 < 41.0
    assert 3.25 < supply / 10000 < 3.35
    assert live.i2c_read(OFF_ACTIVE_LANES, 1)[0] == 0x0F


def test_a_burst_read_walks_the_offset_pointer(live):
    """One transaction must be able to pull a whole block. Reading twenty
    bytes has to give the same answer as twenty single byte reads."""
    burst = live.i2c_read(OFF_IDENTIFIER, 20)
    assert len(burst) == 20

    singles = bytes(live.i2c_read(offset, 1)[0] for offset in range(20))
    assert burst[:4] == singles[:4]


def test_vendor_identity_lives_on_page_00h(live):
    live.select_page(0x00)
    name = live.i2c_read(P00_VENDOR_NAME, 16).decode("ascii")
    part = live.i2c_read(P00_VENDOR_PN, 16).decode("ascii")

    # Space padded and never NUL terminated, as the spec requires.
    assert name.rstrip() == "OPTILINK"
    assert len(name) == 16
    assert part.rstrip() == "OL-4X100G-LR4"


def test_page_01h_advertises_the_lane_count(live):
    live.select_page(0x01)
    assert live.i2c_read(P01_LANE_COUNT, 1)[0] == 4


def test_an_unimplemented_page_is_refused(live):
    """Refusing beats aliasing. A host probing for an optional page needs a
    clean answer, not a silent copy of page zero."""
    live.select_page(0x01)
    with pytest.raises(OptilinkError):
        live.select_page(0x7F)

    # The selection is unchanged, so the probe cost the host nothing.
    live.i2c_read(P01_LANE_COUNT, 1)


def test_only_bank_zero_exists(live):
    live.i2c_write(OFF_BANK_SELECT, 0)
    with pytest.raises(OptilinkError):
        live.i2c_write(OFF_BANK_SELECT, 1)


@pytest.mark.parametrize("offset", [OFF_IDENTIFIER, OFF_TEMP, OFF_ACTIVE_LANES])
def test_module_owned_bytes_reject_host_writes(live, offset):
    """A measurement is not a setting. Accepting the write and then
    overwriting it on the next refresh would be worse than refusing."""
    with pytest.raises(OptilinkError):
        live.i2c_write(offset, 0x99)


def test_per_lane_monitors_are_on_page_11h(live):
    live.select_page(0x11)

    for lane in range(4):
        received = live.read_u16(P11_RX_POWER + lane * 2)
        assert 0.7 < received / 10000 < 0.9, f"lane {lane}"

    # Two lanes share a byte of data path state, even lane in the low nibble.
    packed = live.i2c_read(P11_DP_STATE, 2)
    for lane in range(4):
        nibble = packed[lane // 2] >> (0 if lane % 2 == 0 else 4)
        assert nibble & 0x0F == 3, f"lane {lane} not ACTIVATED"


def test_a_threshold_written_by_the_host_takes_effect(live):
    """Hosts tighten thresholds to margin a link. The write has to reach the
    comparison, not just the shadow copy in the memory map."""
    assert live.flags()["any_alarm"] == 0

    live.select_page(0x02)
    live.write_u16(P02_TEMP_TH, 30 * 256)  # 30 C high alarm
    live.tick(200)

    assert live.flags()["temp"] & 0x08, "40 C should now exceed a 30 C alarm"
    assert live.link()["module"] == "FAULT"


def test_one_lane_threshold_write_covers_every_lane(live):
    """CMIS carries a single threshold set for all lanes. A per-lane
    implementation would silently protect only lane zero."""
    live.select_page(0x02)
    live.write_u16(P02_RX_POWER_TH + 2, 10000)  # 1.0 mW low alarm
    live.tick(200)

    flags = live.flags()
    for lane in range(4):
        assert flags[f"lane{lane}_rxpower"] & 0x04, f"lane {lane} unprotected"


def test_page_10h_disables_a_lane_from_the_host_side(live):
    live.select_page(0x10)
    live.i2c_write(P10_TX_DISABLE, 0x05)  # lanes 0 and 2
    live.tick(200)

    state = live.link()
    assert state["active_mask"] == 0x0A
    assert state["lanes"][0]["tx_on"] is False
    assert state["lanes"][2]["tx_on"] is False
    assert state["lanes"][1]["tx_on"] is True

    live.i2c_write(P10_TX_DISABLE, 0x00)
    live.tick(300)
    assert live.link()["active_mask"] == 0x0F


def test_i2c_counters_reconcile_with_traffic_offered(live):
    before = live.stats()["i2c"]
    live.i2c_read(OFF_IDENTIFIER, 10)
    after = live.stats()["i2c"]

    # Two transactions per read: the offset write, then the repeated start.
    assert after["rd"] - before["rd"] == 10
    assert after["txn"] > before["txn"]
