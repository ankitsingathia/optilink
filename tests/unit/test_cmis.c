#include "optilink/cmis.h"

#include "../../firmware/hal/hal_sim.h"
#include "optilink/ddm.h"
#include "optilink/hal.h"
#include "optilink/i2c.h"
#include "optilink/link.h"
#include "suites.h"
#include "tinytest.h"

static void bring_up(void)
{
    hal_init();
    ddm_init();
    cmis_init();
    i2c_target_init(OPTILINK_I2C_ADDR);
    link_init(0u);
    ddm_sample();
    cmis_refresh();
}

/* Reads len bytes starting at off through the real I2C target, so the offset
 * pointer and the read side effects are exercised the way a host would hit
 * them. */
static void host_read(uint8_t off, uint8_t *dst, uint8_t len)
{
    (void)i2c_target_start(OPTILINK_I2C_ADDR, false);
    (void)i2c_target_write(off);
    (void)i2c_target_start(OPTILINK_I2C_ADDR, true);
    for (uint8_t i = 0u; i < len; i++) {
        dst[i] = i2c_target_read();
    }
    i2c_target_stop();
}

static bool host_write(uint8_t off, uint8_t value)
{
    bool ok;

    (void)i2c_target_start(OPTILINK_I2C_ADDR, false);
    (void)i2c_target_write(off);
    ok = i2c_target_write(value);
    i2c_target_stop();
    return ok;
}

void suite_cmis(void)
{
    uint8_t b[32];

    TT_CASE("identity bytes");
    bring_up();
    host_read(CMIS_OFF_IDENTIFIER, b, 3u);
    CHECK_EQ(b[0], CMIS_ID_QSFP_DD);
    CHECK_EQ(b[1], CMIS_REV_5_0);
    CHECK_EQ(b[2], 0x00); /* paged, not flat */

    TT_CASE("monitors are published big endian");
    host_read(CMIS_OFF_TEMP, b, 4u);
    CHECK_EQ(((int32_t)b[0] << 8) | b[1], ddm_value(DDM_IDX_TEMP));
    CHECK_EQ(((int32_t)b[2] << 8) | b[3], ddm_value(DDM_IDX_VCC));

    TT_CASE("offset auto-increments across a burst");
    (void)i2c_target_start(OPTILINK_I2C_ADDR, false);
    (void)i2c_target_write(0x00u);
    (void)i2c_target_start(OPTILINK_I2C_ADDR, true);
    (void)i2c_target_read();
    (void)i2c_target_read();
    CHECK_EQ(i2c_target_offset(), 2);
    i2c_target_stop();

    TT_CASE("the module owns the lower page");
    /* Temperature is a measurement, not a setting. A host write must be
     * refused rather than quietly accepted and then overwritten by the next
     * refresh. */
    CHECK(!host_write(CMIS_OFF_TEMP, 0x99u));
    CHECK(i2c_target_stats()->nacks > 0u);

    TT_CASE("page select gates the upper half");
    bring_up();
    CHECK(host_write(CMIS_OFF_PAGE_SELECT, 0x00u));
    CHECK_EQ(cmis_page(), 0x00);
    host_read(CMIS_P00_VENDOR_NAME, b, 8u);
    CHECK_EQ(b[0], 'O');
    CHECK_EQ(b[1], 'P');
    CHECK_EQ(b[7], 'K');

    CHECK(host_write(CMIS_OFF_PAGE_SELECT, 0x01u));
    CHECK_EQ(cmis_page(), 0x01);
    host_read(CMIS_P01_LANE_COUNT, b, 1u);
    CHECK_EQ(b[0], OPTILINK_LANES);

    TT_CASE("an unimplemented page is refused, not aliased");
    CHECK(!host_write(CMIS_OFF_PAGE_SELECT, 0x7Fu));
    CHECK_EQ(cmis_page(), 0x01); /* unchanged */

    TT_CASE("only bank zero exists on a four lane part");
    CHECK(host_write(CMIS_OFF_BANK_SELECT, 0x00u));
    CHECK(!host_write(CMIS_OFF_BANK_SELECT, 0x01u));
    CHECK_EQ(cmis_bank(), 0x00);

    TT_CASE("a threshold written on page 02h reaches DDM");
    bring_up();
    CHECK(host_write(CMIS_OFF_PAGE_SELECT, 0x02u));
    /* 60 C high alarm: 60 * 256 = 15360 = 0x3C00. */
    CHECK(host_write(CMIS_P02_TEMP_TH, 0x3Cu));
    CHECK(host_write((uint8_t)(CMIS_P02_TEMP_TH + 1u), 0x00u));
    CHECK_EQ(ddm_thresholds(DDM_IDX_TEMP)->high_alarm, 60 * 256);

    TT_CASE("a lane threshold applies to every lane");
    /* CMIS carries one threshold set for all lanes, so a single write has to
     * land on every one of them. Raising the RX low alarm to 1.0 mW puts it
     * above the 0.8 mW the lanes are receiving, which is what makes the
     * effect observable rather than merely stored. */
    CHECK(host_write((uint8_t)(CMIS_P02_RX_POWER_TH + 2u), 0x27u));
    CHECK(host_write((uint8_t)(CMIS_P02_RX_POWER_TH + 3u), 0x10u));
    for (uint8_t l = 0u; l < OPTILINK_LANES; l++) {
        CHECK_EQ(ddm_thresholds(DDM_IDX_RX_POWER(l))->low_alarm, 10000);
    }
    ddm_sample();
    CHECK(ddm_any_alarm());
    for (uint8_t l = 0u; l < OPTILINK_LANES; l++) {
        CHECK_EQ(ddm_flags_peek(DDM_IDX_RX_POWER(l)) & DDM_FLAG_LOW_ALARM,
                 DDM_FLAG_LOW_ALARM);
    }

    TT_CASE("a read of the flag byte clears the latch");
    bring_up();
    hal_sim_set_adc(HAL_ADC_MODULE_TEMP, 4095u);
    ddm_sample();
    cmis_refresh();
    host_read(CMIS_OFF_MODULE_FLAGS, b, 1u);
    CHECK_EQ(b[0] >> 4, DDM_FLAG_HIGH_ALARM | DDM_FLAG_HIGH_WARN);
    /* Second read comes back empty: those bits were consumed by the first. */
    host_read(CMIS_OFF_MODULE_FLAGS, b, 1u);
    CHECK_EQ(b[0], 0);
    CHECK_EQ(ddm_flags_peek(DDM_IDX_TEMP), 0);

    TT_CASE("page 10h disables a lane");
    bring_up();
    CHECK(host_write(CMIS_OFF_PAGE_SELECT, 0x10u));
    CHECK(host_write(CMIS_P10_TX_DISABLE, 0x05u)); /* lanes 0 and 2 off */
    CHECK(!link_lane_enabled(0u));
    CHECK(link_lane_enabled(1u));
    CHECK(!link_lane_enabled(2u));
    CHECK(link_lane_enabled(3u));

    TT_CASE("a transaction to another address is ignored");
    CHECK(!i2c_target_start((uint8_t)(OPTILINK_I2C_ADDR + 1u), false));
    CHECK(i2c_target_stats()->addr_mismatch > 0u);
    CHECK(!i2c_target_write(0x00u));
    CHECK_EQ(i2c_target_read(), 0xFF);
}
