/*
 * ddm.h - digital diagnostics monitoring.
 *
 * Samples the analog monitors, converts raw ADC counts into the fixed point
 * units CMIS mandates, and compares each against a four level threshold set.
 *
 * The flags are latching and read-to-clear, which is not an implementation
 * shortcut but the behaviour the host firmware depends on: a laser that
 * browns out for 40 ms and recovers must still be visible to a host that
 * polls once a second, otherwise the excursion is invisible and the link
 * looks healthy right up until it fails.
 */
#ifndef OPTILINK_DDM_H
#define OPTILINK_DDM_H

#include <stdbool.h>
#include <stdint.h>

#include "optilink/optilink.h"

/* Flat monitor index space: two module wide monitors followed by three
 * per-lane monitors. */
#define DDM_IDX_TEMP         0
#define DDM_IDX_VCC          1
#define DDM_IDX_TX_BIAS(l)   (2 + (l))
#define DDM_IDX_TX_POWER(l)  (2 + OPTILINK_LANES + (l))
#define DDM_IDX_RX_POWER(l)  (2 + (2 * OPTILINK_LANES) + (l))
#define DDM_IDX_COUNT        (2 + (3 * OPTILINK_LANES))

/* Latched flag bits, ordered so a byte reads high to low severity. */
#define DDM_FLAG_HIGH_ALARM  0x08u
#define DDM_FLAG_LOW_ALARM   0x04u
#define DDM_FLAG_HIGH_WARN   0x02u
#define DDM_FLAG_LOW_WARN    0x01u
#define DDM_FLAG_ALARM_MASK  (DDM_FLAG_HIGH_ALARM | DDM_FLAG_LOW_ALARM)

typedef struct {
    int32_t high_alarm;
    int32_t low_alarm;
    int32_t high_warn;
    int32_t low_warn;
} ddm_thresholds_t;

void ddm_init(void);

/* One pass over every monitor: read the ADC, convert, evaluate thresholds,
 * latch any new flags. Called from the DDM task. */
void ddm_sample(void);

/* Current converted reading, in CMIS units:
 *   temperature  1/256 degrees C, signed
 *   supply       100 uV
 *   TX bias      2 uA
 *   TX/RX power  0.1 uW  */
int32_t ddm_value(uint8_t idx);

uint8_t ddm_flags_peek(uint8_t idx);

/* Returns the latched flags and clears them, which is what a host read of
 * the flag bytes does on real silicon. */
uint8_t ddm_flags_read_clear(uint8_t idx);

/* True while any monitor holds a latched alarm. Drives INT_N and gates the
 * data path state machine. */
bool ddm_any_alarm(void);

const ddm_thresholds_t *ddm_thresholds(uint8_t idx);
bool ddm_set_thresholds(uint8_t idx, const ddm_thresholds_t *t);

/* Exposed so the unit tests can pin the conversion maths independently of
 * whatever the ADC happens to be reporting. */
int32_t ddm_convert(uint8_t idx, uint16_t counts);

#endif /* OPTILINK_DDM_H */
