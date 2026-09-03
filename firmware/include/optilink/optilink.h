/*
 * optilink.h - build-wide configuration for the OptiLink module controller.
 *
 * OptiLink is the firmware that runs on the controller inside a pluggable
 * optical transceiver. It owns the CMIS management memory map, samples the
 * analog monitors, runs the module and per-lane data path state machines,
 * and answers the host over an I2C target interface.
 */
#ifndef OPTILINK_H
#define OPTILINK_H

#define OPTILINK_VERSION_MAJOR 1
#define OPTILINK_VERSION_MINOR 0
#define OPTILINK_VERSION_PATCH 0
#define OPTILINK_VERSION_STR   "1.0.0"

/* A QSFP-DD style module: four electrical lanes onto four optical lanes. */
#define OPTILINK_LANES 4

/* CMIS splits its 256-byte I2C address space into a fixed lower page and a
 * banked/paged upper page, each 128 bytes. */
#define OPTILINK_PAGE_SIZE 128

/* I2C target address the host uses to reach module management (SFF/CMIS
 * modules answer at 1010000b). */
#define OPTILINK_I2C_ADDR 0x50u

/* Scheduler tick. Every task period must be a multiple of this. */
#define OPTILINK_TICK_MS 1u

/* Consecutive samples an input must hold before the state machines believe
 * it. Optical receivers chatter around threshold, so raw LOS is unusable. */
#define OPTILINK_DEBOUNCE_SAMPLES 3u

#endif /* OPTILINK_H */
