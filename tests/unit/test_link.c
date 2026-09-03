#include "optilink/link.h"

#include "../../firmware/hal/hal_sim.h"
#include "optilink/ddm.h"
#include "optilink/hal.h"
#include "suites.h"
#include "tinytest.h"

/* The link task runs every 10 ms in the real build, so the tests drive it at
 * exactly that cadence rather than calling it in a tight loop. Timing bugs
 * that only appear at the shipping period are the whole point. */
#define LINK_PERIOD_MS 10u

static uint32_t s_now;

static void advance(uint32_t ms)
{
    uint32_t end = s_now + ms;

    while (s_now < end) {
        s_now += LINK_PERIOD_MS;
        ddm_sample();
        link_service(s_now);
    }
}

static void start(void)
{
    hal_init();
    ddm_init();
    s_now = 0u;
    link_init(s_now);
}

void suite_link(void)
{
    TT_CASE("power up sequence");
    start();
    CHECK_EQ(link_module_state(), MODULE_RESET);
    advance(10u);
    CHECK_EQ(link_module_state(), MODULE_LOW_PWR);
    advance(10u);
    CHECK_EQ(link_module_state(), MODULE_PWR_UP);
    /* 50 ms of supply and bias settling before anything downstream runs. */
    advance(40u);
    CHECK_EQ(link_module_state(), MODULE_PWR_UP);
    advance(20u);
    CHECK_EQ(link_module_state(), MODULE_READY);

    TT_CASE("lanes reach the data path activated state");
    advance(200u);
    CHECK_EQ(link_active_lane_mask(), 0x0F);
    CHECK(link_is_up());
    for (uint8_t l = 0u; l < OPTILINK_LANES; l++) {
        CHECK_EQ(link_lane_state(l), DP_ACTIVATED);
        CHECK(hal_sim_tx_enabled(l));
    }

    TT_CASE("a two sample glitch does not drop the lane");
    hal_sim_set_los(1u, true);
    /* Three consecutive agreeing samples are required, so two must not be
     * enough. This is the assertion that a naive implementation fails. */
    advance(20u);
    CHECK_EQ(link_lane_state(1u), DP_ACTIVATED);
    CHECK_EQ(link_active_lane_mask(), 0x0F);

    TT_CASE("a sustained loss of signal does drop it");
    advance(20u);
    CHECK_EQ(link_lane_state(1u), DP_FAULT);
    CHECK_EQ(link_lane_stats(1u)->last_fault, LANE_FAULT_LOS);
    CHECK_EQ(link_active_lane_mask(), 0x0D);
    /* Neighbouring lanes are untouched: that is why the data path machine is
     * per lane rather than per module. */
    CHECK_EQ(link_lane_state(0u), DP_ACTIVATED);
    CHECK_EQ(link_lane_state(2u), DP_ACTIVATED);

    TT_CASE("a faulted lane darks its transmitter");
    CHECK(!hal_sim_tx_enabled(1u));
    CHECK(hal_sim_tx_enabled(0u));
    /* The module as a whole is still up on three lanes. */
    CHECK(link_is_up());

    TT_CASE("the lane recovers once the signal returns");
    hal_sim_set_los(1u, false);
    advance(300u);
    CHECK_EQ(link_lane_state(1u), DP_ACTIVATED);
    CHECK(hal_sim_tx_enabled(1u));
    CHECK_EQ(link_active_lane_mask(), 0x0F);
    CHECK_EQ(link_lane_stats(1u)->retries, 1);
    CHECK_EQ(link_lane_stats(1u)->faults, 1);

    TT_CASE("a laser fault is distinguished from a signal loss");
    start();
    advance(300u);
    hal_sim_set_tx_fault(2u, true);
    advance(50u);
    CHECK_EQ(link_lane_state(2u), DP_FAULT);
    CHECK_EQ(link_lane_stats(2u)->last_fault, LANE_FAULT_TX);
    CHECK(!hal_sim_tx_enabled(2u));

    TT_CASE("retries are capped so a dead lane stays down");
    start();
    advance(300u);
    /* Hold the fault asserted long past every holdoff. The lane must settle
     * in FAULT rather than cycling its laser indefinitely. */
    hal_sim_set_cdr_lol(3u, true);
    advance(3000u);
    CHECK_EQ(link_lane_state(3u), DP_FAULT);
    CHECK_EQ(link_lane_stats(3u)->last_fault, LANE_FAULT_LOL);
    CHECK(link_lane_stats(3u)->retries <= 5u);
    CHECK(!hal_sim_tx_enabled(3u));

    TT_CASE("a module alarm faults every lane at once");
    start();
    advance(300u);
    CHECK_EQ(link_active_lane_mask(), 0x0F);
    hal_sim_set_adc(HAL_ADC_MODULE_TEMP, 4095u); /* 125 C */
    advance(200u);
    CHECK_EQ(link_module_state(), MODULE_FAULT);
    CHECK_EQ(link_active_lane_mask(), 0x00);
    CHECK(!link_is_up());
    CHECK_EQ(link_module_faults(), 1);
    for (uint8_t l = 0u; l < OPTILINK_LANES; l++) {
        CHECK(!hal_sim_tx_enabled(l));
    }

    TT_CASE("removing the module resets everything");
    start();
    advance(300u);
    hal_sim_set_gpio(HAL_PIN_MODULE_PRESENT, false);
    advance(20u);
    CHECK_EQ(link_module_state(), MODULE_RESET);
    CHECK_EQ(link_active_lane_mask(), 0x00);
    hal_sim_set_gpio(HAL_PIN_MODULE_PRESENT, true);
    advance(300u);
    CHECK_EQ(link_module_state(), MODULE_READY);
    CHECK_EQ(link_active_lane_mask(), 0x0F);

    TT_CASE("low power mode parks the lanes without faulting them");
    start();
    advance(300u);
    hal_sim_set_gpio(HAL_PIN_LPMODE, true);
    advance(50u);
    CHECK_EQ(link_module_state(), MODULE_LOW_PWR);
    CHECK_EQ(link_active_lane_mask(), 0x00);
    CHECK_EQ(link_lane_stats(0u)->faults, 0);
    hal_sim_set_gpio(HAL_PIN_LPMODE, false);
    advance(300u);
    CHECK_EQ(link_active_lane_mask(), 0x0F);

    TT_CASE("a disabled lane stays down and its neighbours do not");
    start();
    advance(300u);
    link_lane_enable(0u, false);
    advance(100u);
    CHECK_EQ(link_lane_state(0u), DP_DEACTIVATED);
    CHECK(!hal_sim_tx_enabled(0u));
    CHECK_EQ(link_active_lane_mask(), 0x0E);
    link_lane_enable(0u, true);
    advance(200u);
    CHECK_EQ(link_active_lane_mask(), 0x0F);

    TT_CASE("out of range lanes are inert");
    CHECK_EQ(link_lane_state(OPTILINK_LANES), DP_DEACTIVATED);
    CHECK(!link_lane_enabled(OPTILINK_LANES));
    CHECK(link_lane_stats(OPTILINK_LANES) == NULL);
    link_lane_enable(OPTILINK_LANES, true); /* must not corrupt anything */
    CHECK_EQ(link_active_lane_mask(), 0x0F);

    TT_CASE("state names are all populated");
    CHECK_STR(link_module_state_name(MODULE_READY), "READY");
    CHECK_STR(link_dp_state_name(DP_ACTIVATED), "ACTIVATED");
    CHECK_STR(link_fault_name(LANE_FAULT_LOS), "RX_LOS");
    CHECK_STR(link_module_state_name((module_state_t)99), "?");
    CHECK_STR(link_dp_state_name((dp_state_t)99), "?");
    CHECK_STR(link_fault_name((lane_fault_t)99), "?");
}
