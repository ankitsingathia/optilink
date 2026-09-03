"""Digital diagnostics: conversion, thresholds, latching and escalation."""

from __future__ import annotations

import pytest

HIGH_ALARM = 0x08
LOW_ALARM = 0x04
HIGH_WARN = 0x02
LOW_WARN = 0x01

# ADC counts, from the calibration documented in ddm.c.
COUNTS_FULL_SCALE = 4095
COUNTS_TEMP_NOMINAL = 1985  # about 40 C
COUNTS_TEMP_MAX = 4095  # 125 C
COUNTS_RX_DARK = 0

CMIS_MODULE_FLAGS = 0x08


def test_nominal_module_reports_no_alarms(live):
    flags = live.flags()
    assert flags["any_alarm"] == 0
    assert all(value == 0 for key, value in flags.items() if key != "any_alarm")


def test_monitors_convert_to_documented_units(live):
    monitors = live.monitors()

    # 1/256 C, 100 uV, 2 uA and 0.1 uW respectively. Tolerances are the
    # integer rounding in the conversion, nothing more.
    assert 39.0 < monitors["temp"] / 256 < 41.0
    assert 3.25 < monitors["vcc"] / 10000 < 3.35
    for lane in range(4):
        assert 30 < monitors[f"lane{lane}_bias"] * 2 / 1000 < 40  # mA
        assert 0.9 < monitors[f"lane{lane}_txpower"] / 10000 < 1.1  # mW
        assert 0.7 < monitors[f"lane{lane}_rxpower"] / 10000 < 0.9  # mW


def test_over_temperature_raises_alarm_and_warning(live):
    live.inject("temp", COUNTS_TEMP_MAX)
    live.tick(200)

    flags = live.flags()
    assert flags["temp"] == HIGH_ALARM | HIGH_WARN
    assert flags["any_alarm"] == 1


def test_a_transient_excursion_stays_visible_after_it_clears(live):
    """The behaviour the whole latching design exists for.

    A module that browns out for 40 ms and recovers must still be visible to
    a host that polls once a second. Without latching the excursion is
    invisible and the link looks healthy right up until it fails.
    """
    live.inject("temp", COUNTS_TEMP_MAX)
    live.tick(150)
    live.inject("temp", COUNTS_TEMP_NOMINAL)
    live.tick(150)

    monitors = live.monitors()
    assert monitors["temp"] / 256 < 75, "temperature really did return to normal"

    assert live.flags()["temp"] == HIGH_ALARM | HIGH_WARN


def test_reading_the_flag_byte_over_i2c_clears_it(live):
    live.inject("temp", COUNTS_TEMP_MAX)
    live.tick(200)

    first = live.i2c_read(CMIS_MODULE_FLAGS, 1)[0]
    assert first >> 4 == HIGH_ALARM | HIGH_WARN

    # Read to clear: those bits belonged to the host that just took them.
    second = live.i2c_read(CMIS_MODULE_FLAGS, 1)[0]
    assert second == 0
    assert live.flags()["temp"] == 0


def test_a_dark_receiver_raises_a_low_alarm_on_that_lane_only(live):
    live.inject("rxpower", 2, COUNTS_RX_DARK)
    live.tick(200)

    flags = live.flags()
    assert flags["lane2_rxpower"] == LOW_ALARM | LOW_WARN
    assert flags["lane0_rxpower"] == 0
    assert flags["lane1_rxpower"] == 0
    assert flags["lane3_rxpower"] == 0


def test_a_module_alarm_faults_the_whole_data_path(live):
    """Temperature and supply are module-wide, so unlike a per-lane fault
    they must take every lane down together."""
    assert live.link()["active_mask"] == 0x0F

    live.inject("temp", COUNTS_TEMP_MAX)
    live.tick(300)

    state = live.link()
    assert state["module"] == "FAULT"
    assert state["active_mask"] == 0
    assert state["up"] is False
    assert state["faults"] == 1
    assert all(lane["tx_on"] is False for lane in state["lanes"])


def test_module_recovers_once_the_alarm_is_read_and_the_cause_is_gone(live):
    live.inject("temp", COUNTS_TEMP_MAX)
    live.tick(300)
    assert live.link()["module"] == "FAULT"

    live.inject("temp", COUNTS_TEMP_NOMINAL)
    live.tick(200)
    # Still faulted: the latch is holding, which is correct. Recovery is
    # gated on the host acknowledging the alarm, not merely on the sensor
    # coming back.
    assert live.link()["module"] == "FAULT"

    live.i2c_read(CMIS_MODULE_FLAGS, 1)
    live.tick(500)
    assert live.link()["module"] == "READY"
    assert live.link()["active_mask"] == 0x0F


@pytest.mark.parametrize("lane", range(4))
def test_a_bias_excursion_is_attributed_to_the_right_lane(live, lane):
    live.inject("bias", lane, COUNTS_FULL_SCALE)
    live.tick(200)

    flags = live.flags()
    assert flags[f"lane{lane}_bias"] == HIGH_ALARM | HIGH_WARN
    for other in range(4):
        if other != lane:
            assert flags[f"lane{other}_bias"] == 0, f"lane {other} contaminated"
