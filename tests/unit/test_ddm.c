#include "optilink/ddm.h"

#include "../../firmware/hal/hal_sim.h"
#include "optilink/hal.h"
#include "suites.h"
#include "tinytest.h"

void suite_ddm(void)
{
    TT_CASE("conversion endpoints");
    hal_init();
    ddm_init();
    /* Full scale maps to the documented sensor range: -40 to +125 C in
     * 1/256 C, and 0 to 3.6 V in 100 uV. */
    CHECK_EQ(ddm_convert(DDM_IDX_TEMP, 0u), -40 * 256);
    CHECK_EQ(ddm_convert(DDM_IDX_TEMP, 4095u), 125 * 256);
    CHECK_EQ(ddm_convert(DDM_IDX_VCC, 0u), 0);
    CHECK_EQ(ddm_convert(DDM_IDX_VCC, 4095u), 36000);
    CHECK_EQ(ddm_convert(DDM_IDX_TX_BIAS(0), 4095u), 65535);
    CHECK_EQ(ddm_convert(DDM_IDX_RX_POWER(3), 4095u), 65535);

    TT_CASE("counts above full scale saturate");
    CHECK_EQ(ddm_convert(DDM_IDX_VCC, 60000u), 36000);

    TT_CASE("defaults are inside every band");
    hal_init();
    ddm_init();
    ddm_sample();
    CHECK(!ddm_any_alarm());
    CHECK_EQ(ddm_flags_peek(DDM_IDX_TEMP), 0);
    CHECK_EQ(ddm_flags_peek(DDM_IDX_RX_POWER(0)), 0);

    TT_CASE("high alarm asserts and implies the warning");
    hal_init();
    ddm_init();
    hal_sim_set_adc(HAL_ADC_MODULE_TEMP, 4095u); /* 125 C */
    ddm_sample();
    CHECK(ddm_any_alarm());
    CHECK_EQ(ddm_flags_peek(DDM_IDX_TEMP),
             DDM_FLAG_HIGH_ALARM | DDM_FLAG_HIGH_WARN);

    TT_CASE("flags latch across a transient");
    /* Put the temperature back in range. A non-latching implementation would
     * clear here, and a host polling at 1 Hz would never learn the module
     * had been over temperature at all. */
    hal_sim_set_adc(HAL_ADC_MODULE_TEMP, 1985u);
    ddm_sample();
    CHECK(ddm_value(DDM_IDX_TEMP) < 75 * 256);
    CHECK_EQ(ddm_flags_peek(DDM_IDX_TEMP),
             DDM_FLAG_HIGH_ALARM | DDM_FLAG_HIGH_WARN);
    CHECK(ddm_any_alarm());

    TT_CASE("reading clears the latch");
    CHECK_EQ(ddm_flags_read_clear(DDM_IDX_TEMP),
             DDM_FLAG_HIGH_ALARM | DDM_FLAG_HIGH_WARN);
    CHECK_EQ(ddm_flags_peek(DDM_IDX_TEMP), 0);
    CHECK(!ddm_any_alarm());

    TT_CASE("low alarm on a dark receiver");
    hal_init();
    ddm_init();
    hal_sim_set_adc(HAL_ADC_RX_POWER_0 + 2u, 0u);
    ddm_sample();
    CHECK_EQ(ddm_flags_peek(DDM_IDX_RX_POWER(2)),
             DDM_FLAG_LOW_ALARM | DDM_FLAG_LOW_WARN);
    CHECK_EQ(ddm_flags_peek(DDM_IDX_RX_POWER(0)), 0); /* other lanes unaffected */

    TT_CASE("host can retighten a threshold");
    hal_init();
    ddm_init();
    ddm_sample();
    CHECK(!ddm_any_alarm());
    {
        ddm_thresholds_t t = { 30 * 256, -5 * 256, 25 * 256, 0 };

        CHECK(ddm_set_thresholds(DDM_IDX_TEMP, &t));
        CHECK(!ddm_set_thresholds(DDM_IDX_COUNT, &t));
        CHECK(!ddm_set_thresholds(DDM_IDX_TEMP, NULL));
    }
    /* The default 40 C reading is now above a 30 C alarm limit. */
    ddm_sample();
    CHECK(ddm_any_alarm());
    CHECK_EQ(ddm_flags_peek(DDM_IDX_TEMP),
             DDM_FLAG_HIGH_ALARM | DDM_FLAG_HIGH_WARN);
    CHECK_EQ(ddm_thresholds(DDM_IDX_TEMP)->high_alarm, 30 * 256);
    CHECK(ddm_thresholds(DDM_IDX_COUNT) == NULL);

    TT_CASE("out of range index is inert");
    CHECK_EQ(ddm_value(DDM_IDX_COUNT), 0);
    CHECK_EQ(ddm_flags_peek(DDM_IDX_COUNT), 0);
    CHECK_EQ(ddm_flags_read_clear(DDM_IDX_COUNT), 0);
    CHECK_EQ(ddm_convert(DDM_IDX_COUNT, 100u), 0);
}
