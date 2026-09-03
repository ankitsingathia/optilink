#include "optilink/ddm.h"

#include <stddef.h>

#include "optilink/hal.h"

/* The monitors share one 12 bit ADC. Each channel is turned into engineering
 * units by a straight line fit taken from factory calibration:
 *
 *     value = (counts * scale_num) / scale_den + offset
 *
 * Integer only, and the products are bounded: the largest is
 * 4095 * 42240 which is well inside int32, so no 64 bit maths creeps into
 * a routine that runs fourteen times every 100 ms. */
#define ADC_FULL_SCALE 4095

typedef struct {
    int32_t scale_num;
    int32_t scale_den;
    int32_t offset;
} ddm_cal_t;

/* -40..+125 C over full scale, expressed in 1/256 C. */
static const ddm_cal_t CAL_TEMP     = { 165 * 256, ADC_FULL_SCALE, -40 * 256 };
/* 0..3.6 V over full scale, expressed in 100 uV. */
static const ddm_cal_t CAL_VCC      = { 36000,     ADC_FULL_SCALE, 0 };
/* 0..131 mA over full scale, expressed in 2 uA. */
static const ddm_cal_t CAL_TX_BIAS  = { 65535,     ADC_FULL_SCALE, 0 };
/* 0..6.5535 mW over full scale, expressed in 0.1 uW. */
static const ddm_cal_t CAL_OPTICAL  = { 65535,     ADC_FULL_SCALE, 0 };

typedef struct {
    int32_t          value;
    uint8_t          latched;
    ddm_thresholds_t th;
} ddm_mon_t;

static ddm_mon_t s_mon[DDM_IDX_COUNT];

static const ddm_cal_t *cal_for(uint8_t idx)
{
    if (idx == DDM_IDX_TEMP) {
        return &CAL_TEMP;
    }
    if (idx == DDM_IDX_VCC) {
        return &CAL_VCC;
    }
    if (idx < DDM_IDX_TX_POWER(0)) {
        return &CAL_TX_BIAS;
    }
    return &CAL_OPTICAL;
}

static hal_adc_ch_t channel_for(uint8_t idx)
{
    if (idx == DDM_IDX_TEMP) {
        return HAL_ADC_MODULE_TEMP;
    }
    if (idx == DDM_IDX_VCC) {
        return HAL_ADC_MODULE_VCC;
    }
    if (idx < DDM_IDX_TX_POWER(0)) {
        return (hal_adc_ch_t)(HAL_ADC_TX_BIAS_0 + (idx - DDM_IDX_TX_BIAS(0)));
    }
    if (idx < DDM_IDX_RX_POWER(0)) {
        return (hal_adc_ch_t)(HAL_ADC_TX_POWER_0 + (idx - DDM_IDX_TX_POWER(0)));
    }
    return (hal_adc_ch_t)(HAL_ADC_RX_POWER_0 + (idx - DDM_IDX_RX_POWER(0)));
}

int32_t ddm_convert(uint8_t idx, uint16_t counts)
{
    const ddm_cal_t *c;

    if (idx >= DDM_IDX_COUNT) {
        return 0;
    }
    c = cal_for(idx);
    if (counts > ADC_FULL_SCALE) {
        counts = ADC_FULL_SCALE;
    }
    return (((int32_t)counts * c->scale_num) / c->scale_den) + c->offset;
}

void ddm_init(void)
{
    for (uint8_t i = 0u; i < DDM_IDX_COUNT; i++) {
        s_mon[i].value   = 0;
        s_mon[i].latched = 0u;
    }

    /* Defaults for a commercial temperature range 4x25G module. The host may
     * overwrite any of these through CMIS page 02h. */
    s_mon[DDM_IDX_TEMP].th = (ddm_thresholds_t){ 75 * 256, -5 * 256, 70 * 256, 0 };
    s_mon[DDM_IDX_VCC].th  = (ddm_thresholds_t){ 36000, 30000, 34700, 31300 };

    for (uint8_t l = 0u; l < OPTILINK_LANES; l++) {
        s_mon[DDM_IDX_TX_BIAS(l)].th  = (ddm_thresholds_t){ 45000, 7500, 42500, 10000 };
        s_mon[DDM_IDX_TX_POWER(l)].th = (ddm_thresholds_t){ 20000, 1500, 16000, 2500 };
        s_mon[DDM_IDX_RX_POWER(l)].th = (ddm_thresholds_t){ 20000, 300, 16000, 500 };
    }
}

static uint8_t evaluate(const ddm_thresholds_t *t, int32_t v)
{
    uint8_t f = 0u;

    if (v >= t->high_alarm) {
        f |= DDM_FLAG_HIGH_ALARM;
    }
    if (v <= t->low_alarm) {
        f |= DDM_FLAG_LOW_ALARM;
    }
    if (v >= t->high_warn) {
        f |= DDM_FLAG_HIGH_WARN;
    }
    if (v <= t->low_warn) {
        f |= DDM_FLAG_LOW_WARN;
    }
    return f;
}

void ddm_sample(void)
{
    for (uint8_t i = 0u; i < DDM_IDX_COUNT; i++) {
        uint16_t counts = hal_adc_read(channel_for(i));

        s_mon[i].value = ddm_convert(i, counts);
        /* OR rather than assign: this is what makes the flag latching. A
         * transient that has already cleared by the next sample still leaves
         * its bit set for the host to find. */
        s_mon[i].latched |= evaluate(&s_mon[i].th, s_mon[i].value);
    }
}

int32_t ddm_value(uint8_t idx)
{
    return (idx < DDM_IDX_COUNT) ? s_mon[idx].value : 0;
}

uint8_t ddm_flags_peek(uint8_t idx)
{
    return (idx < DDM_IDX_COUNT) ? s_mon[idx].latched : 0u;
}

uint8_t ddm_flags_read_clear(uint8_t idx)
{
    uint8_t f;

    if (idx >= DDM_IDX_COUNT) {
        return 0u;
    }
    f                  = s_mon[idx].latched;
    s_mon[idx].latched = 0u;
    return f;
}

bool ddm_any_alarm(void)
{
    for (uint8_t i = 0u; i < DDM_IDX_COUNT; i++) {
        if ((s_mon[i].latched & DDM_FLAG_ALARM_MASK) != 0u) {
            return true;
        }
    }
    return false;
}

const ddm_thresholds_t *ddm_thresholds(uint8_t idx)
{
    return (idx < DDM_IDX_COUNT) ? &s_mon[idx].th : NULL;
}

bool ddm_set_thresholds(uint8_t idx, const ddm_thresholds_t *t)
{
    if (idx >= DDM_IDX_COUNT || t == NULL) {
        return false;
    }
    s_mon[idx].th = *t;
    return true;
}
