#include "optilink/cmis.h"

#include <string.h>

#include "optilink/ddm.h"
#include "optilink/link.h"

/* Only the pages this module actually implements are backed by storage. A
 * select of anything else is refused rather than silently aliased, so a host
 * probing for optional pages gets a clean answer instead of a copy of page
 * zero. */
static const uint8_t SUPPORTED_PAGES[] = { 0x00u, 0x01u, 0x02u, 0x10u, 0x11u };
#define PAGE_COUNT ((uint8_t)(sizeof SUPPORTED_PAGES))

static uint8_t s_lower[OPTILINK_PAGE_SIZE];
static uint8_t s_upper[PAGE_COUNT][OPTILINK_PAGE_SIZE];
static uint8_t s_page;
static uint8_t s_bank;

/* Host visible latches for the per-lane digital faults. They live here
 * rather than in link.c because the latch is a property of the management
 * interface: it exists so a host polling at 1 Hz still sees a 30 ms glitch. */
static uint8_t s_los_latch;
static uint8_t s_txf_latch;

static int page_index(uint8_t page)
{
    for (uint8_t i = 0u; i < PAGE_COUNT; i++) {
        if (SUPPORTED_PAGES[i] == page) {
            return (int)i;
        }
    }
    return -1;
}

static void be16_put(uint8_t *p, int32_t v)
{
    p[0] = (uint8_t)((uint32_t)v >> 8);
    p[1] = (uint8_t)((uint32_t)v & 0xFFu);
}

static int32_t be16_get_signed(const uint8_t *p)
{
    return (int32_t)(int16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int32_t be16_get_unsigned(const uint8_t *p)
{
    return (int32_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put_ascii(uint8_t *dst, const char *src, size_t len)
{
    size_t i = 0u;

    /* Space padded and never NUL terminated, which is what the spec asks
     * for and what host tooling expects to strip. */
    while (i < len && src[i] != '\0') {
        dst[i] = (uint8_t)src[i];
        i++;
    }
    while (i < len) {
        dst[i] = (uint8_t)' ';
        i++;
    }
}

/* Writes one threshold set out to page 02h from whatever DDM currently
 * holds, so a host read after reset sees the factory defaults. */
static void publish_thresholds(uint8_t *page02, uint8_t off, uint8_t ddm_idx)
{
    const ddm_thresholds_t *t = ddm_thresholds(ddm_idx);
    uint8_t                *p = &page02[off - OPTILINK_PAGE_SIZE];

    if (t == NULL) {
        return;
    }
    be16_put(&p[0], t->high_alarm);
    be16_put(&p[2], t->low_alarm);
    be16_put(&p[4], t->high_warn);
    be16_put(&p[6], t->low_warn);
}

void cmis_init(void)
{
    uint8_t *p00, *p01, *p02;

    memset(s_lower, 0, sizeof s_lower);
    memset(s_upper, 0, sizeof s_upper);
    s_page      = 0x00u;
    s_bank      = 0x00u;
    s_los_latch = 0u;
    s_txf_latch = 0u;

    s_lower[CMIS_OFF_IDENTIFIER] = CMIS_ID_QSFP_DD;
    s_lower[CMIS_OFF_REVISION]   = CMIS_REV_5_0;
    s_lower[CMIS_OFF_MEM_MODEL]  = 0x00u; /* paged, not flat */

    p00 = s_upper[page_index(0x00u)];
    p00[CMIS_P00_IDENTIFIER - OPTILINK_PAGE_SIZE] = CMIS_ID_QSFP_DD;
    put_ascii(&p00[CMIS_P00_VENDOR_NAME - OPTILINK_PAGE_SIZE], "OPTILINK", 16u);
    put_ascii(&p00[CMIS_P00_VENDOR_PN - OPTILINK_PAGE_SIZE], "OL-4X100G-LR4", 16u);
    put_ascii(&p00[CMIS_P00_VENDOR_REV - OPTILINK_PAGE_SIZE], "10", 2u);
    put_ascii(&p00[CMIS_P00_VENDOR_SN - OPTILINK_PAGE_SIZE], "OL00000000001", 16u);
    put_ascii(&p00[CMIS_P00_DATE_CODE - OPTILINK_PAGE_SIZE], "26010100", 8u);
    /* Vendor OUI stays all zeroes: this is not a registered part. */

    p01 = s_upper[page_index(0x01u)];
    p01[CMIS_P01_MEDIA_TYPE - OPTILINK_PAGE_SIZE] = CMIS_MEDIA_SMF;
    p01[CMIS_P01_LANE_COUNT - OPTILINK_PAGE_SIZE] = (uint8_t)OPTILINK_LANES;

    p02 = s_upper[page_index(0x02u)];
    publish_thresholds(p02, CMIS_P02_TEMP_TH, DDM_IDX_TEMP);
    publish_thresholds(p02, CMIS_P02_VCC_TH, DDM_IDX_VCC);
    publish_thresholds(p02, CMIS_P02_TX_BIAS_TH, DDM_IDX_TX_BIAS(0));
    publish_thresholds(p02, CMIS_P02_TX_POWER_TH, DDM_IDX_TX_POWER(0));
    publish_thresholds(p02, CMIS_P02_RX_POWER_TH, DDM_IDX_RX_POWER(0));
}

void cmis_refresh(void)
{
    uint8_t *p11 = s_upper[page_index(0x11u)];

    s_lower[CMIS_OFF_MODULE_STATE] =
        (uint8_t)(((uint8_t)link_module_state() & 0x07u) << 1);

    be16_put(&s_lower[CMIS_OFF_TEMP], ddm_value(DDM_IDX_TEMP));
    be16_put(&s_lower[CMIS_OFF_VCC], ddm_value(DDM_IDX_VCC));
    s_lower[CMIS_OFF_ACTIVE_LANES] = link_active_lane_mask();

    /* Temperature flags in the high nibble, supply in the low nibble. */
    s_lower[CMIS_OFF_MODULE_FLAGS] =
        (uint8_t)((ddm_flags_peek(DDM_IDX_TEMP) << 4) | ddm_flags_peek(DDM_IDX_VCC));

    s_los_latch |= link_los_mask();
    s_txf_latch |= link_tx_fault_mask();
    s_lower[CMIS_OFF_LOS_FLAGS] = s_los_latch;
    s_lower[CMIS_OFF_TXF_FLAGS] = s_txf_latch;

    s_lower[CMIS_OFF_BANK_SELECT] = s_bank;
    s_lower[CMIS_OFF_PAGE_SELECT] = s_page;

    for (uint8_t l = 0u; l < OPTILINK_LANES; l++) {
        uint8_t  nib_off = (uint8_t)(CMIS_P11_DP_STATE - OPTILINK_PAGE_SIZE + (l / 2u));
        uint8_t  state   = (uint8_t)link_lane_state(l) & 0x0Fu;

        /* Two lanes share a byte: even lane in the low nibble. */
        if ((l % 2u) == 0u) {
            p11[nib_off] = (uint8_t)((p11[nib_off] & 0xF0u) | state);
        } else {
            p11[nib_off] = (uint8_t)((p11[nib_off] & 0x0Fu) | (uint8_t)(state << 4));
        }

        be16_put(&p11[CMIS_P11_TX_BIAS - OPTILINK_PAGE_SIZE + (l * 2u)],
                 ddm_value(DDM_IDX_TX_BIAS(l)));
        be16_put(&p11[CMIS_P11_TX_POWER - OPTILINK_PAGE_SIZE + (l * 2u)],
                 ddm_value(DDM_IDX_TX_POWER(l)));
        be16_put(&p11[CMIS_P11_RX_POWER - OPTILINK_PAGE_SIZE + (l * 2u)],
                 ddm_value(DDM_IDX_RX_POWER(l)));
    }
}

uint8_t cmis_read(uint8_t offset)
{
    if (offset < OPTILINK_PAGE_SIZE) {
        uint8_t v = s_lower[offset];

        /* Read to clear. The value the host just took is the last time
         * anyone will see those bits, so they are dropped here and not on
         * the next refresh. */
        switch (offset) {
        case CMIS_OFF_MODULE_FLAGS:
            (void)ddm_flags_read_clear(DDM_IDX_TEMP);
            (void)ddm_flags_read_clear(DDM_IDX_VCC);
            s_lower[offset] = 0u;
            break;
        case CMIS_OFF_LOS_FLAGS:
            s_los_latch     = 0u;
            s_lower[offset] = 0u;
            break;
        case CMIS_OFF_TXF_FLAGS:
            s_txf_latch     = 0u;
            s_lower[offset] = 0u;
            break;
        default:
            break;
        }
        return v;
    }

    {
        int idx = page_index(s_page);

        if (idx < 0) {
            return 0u;
        }
        return s_upper[idx][offset - OPTILINK_PAGE_SIZE];
    }
}

/* Re-reads one threshold set out of page 02h and hands it to DDM. Lane
 * monitors share a single set across all lanes, exactly as CMIS defines
 * them. */
static void apply_threshold_write(const uint8_t *p02, uint8_t off, uint8_t base_idx,
                                  uint8_t lanes, bool is_signed)
{
    const uint8_t   *p = &p02[off - OPTILINK_PAGE_SIZE];
    ddm_thresholds_t t;

    if (is_signed) {
        t.high_alarm = be16_get_signed(&p[0]);
        t.low_alarm  = be16_get_signed(&p[2]);
        t.high_warn  = be16_get_signed(&p[4]);
        t.low_warn   = be16_get_signed(&p[6]);
    } else {
        t.high_alarm = be16_get_unsigned(&p[0]);
        t.low_alarm  = be16_get_unsigned(&p[2]);
        t.high_warn  = be16_get_unsigned(&p[4]);
        t.low_warn   = be16_get_unsigned(&p[6]);
    }
    for (uint8_t l = 0u; l < lanes; l++) {
        (void)ddm_set_thresholds((uint8_t)(base_idx + l), &t);
    }
}

static bool in_range(uint8_t off, uint8_t base, uint8_t len)
{
    return off >= base && off < (uint8_t)(base + len);
}

bool cmis_write(uint8_t offset, uint8_t value)
{
    if (offset == CMIS_OFF_PAGE_SELECT) {
        if (page_index(value) < 0) {
            return false;
        }
        s_page                        = value;
        s_lower[CMIS_OFF_PAGE_SELECT] = value;
        return true;
    }
    if (offset == CMIS_OFF_BANK_SELECT) {
        /* One bank of four lanes, so anything but bank 0 is out of range. */
        if (value != 0u) {
            return false;
        }
        s_bank                        = value;
        s_lower[CMIS_OFF_BANK_SELECT] = value;
        return true;
    }
    if (offset < OPTILINK_PAGE_SIZE) {
        return false; /* the rest of the lower page is module owned */
    }

    if (s_page == 0x02u) {
        uint8_t *p02 = s_upper[page_index(0x02u)];

        if (in_range(offset, CMIS_P02_TEMP_TH, 8u)) {
            p02[offset - OPTILINK_PAGE_SIZE] = value;
            apply_threshold_write(p02, CMIS_P02_TEMP_TH, DDM_IDX_TEMP, 1u, true);
            return true;
        }
        if (in_range(offset, CMIS_P02_VCC_TH, 8u)) {
            p02[offset - OPTILINK_PAGE_SIZE] = value;
            apply_threshold_write(p02, CMIS_P02_VCC_TH, DDM_IDX_VCC, 1u, false);
            return true;
        }
        if (in_range(offset, CMIS_P02_TX_BIAS_TH, 8u)) {
            p02[offset - OPTILINK_PAGE_SIZE] = value;
            apply_threshold_write(p02, CMIS_P02_TX_BIAS_TH, DDM_IDX_TX_BIAS(0),
                                  OPTILINK_LANES, false);
            return true;
        }
        if (in_range(offset, CMIS_P02_TX_POWER_TH, 8u)) {
            p02[offset - OPTILINK_PAGE_SIZE] = value;
            apply_threshold_write(p02, CMIS_P02_TX_POWER_TH, DDM_IDX_TX_POWER(0),
                                  OPTILINK_LANES, false);
            return true;
        }
        if (in_range(offset, CMIS_P02_RX_POWER_TH, 8u)) {
            p02[offset - OPTILINK_PAGE_SIZE] = value;
            apply_threshold_write(p02, CMIS_P02_RX_POWER_TH, DDM_IDX_RX_POWER(0),
                                  OPTILINK_LANES, false);
            return true;
        }
        return false;
    }

    if (s_page == 0x10u && offset == CMIS_P10_TX_DISABLE) {
        uint8_t *p10 = s_upper[page_index(0x10u)];

        p10[offset - OPTILINK_PAGE_SIZE] = value;
        for (uint8_t l = 0u; l < OPTILINK_LANES; l++) {
            link_lane_enable(l, (value & (uint8_t)(1u << l)) == 0u);
        }
        return true;
    }

    return false;
}

uint8_t cmis_page(void)
{
    return s_page;
}

uint8_t cmis_bank(void)
{
    return s_bank;
}
