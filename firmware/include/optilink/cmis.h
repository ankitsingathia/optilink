/*
 * cmis.h - the host facing management memory map.
 *
 * A pluggable optical module presents itself to its host as 256 bytes of I2C
 * address space at 1010000b. The bottom half is fixed and always readable;
 * the top half is a window onto one of several pages, selected by writing
 * byte 127. That paging is the whole reason the interface has survived: a
 * host that understands only the lower page can still identify any module
 * ever built, while newer pages carry per-lane detail behind the same two
 * wires.
 *
 * Offsets here follow CMIS conventions for the fields this module actually
 * implements. It is a working subset, not the full specification: identity,
 * module state, latched flags, module and per-lane monitors, thresholds, and
 * lane control. Multi-byte fields are big endian, as the spec requires.
 */
#ifndef OPTILINK_CMIS_H
#define OPTILINK_CMIS_H

#include <stdbool.h>
#include <stdint.h>

#include "optilink/optilink.h"

/* --- lower page, always mapped at 0x00..0x7F ---------------------------- */
#define CMIS_OFF_IDENTIFIER   0x00u /* 0x18 = QSFP-DD */
#define CMIS_OFF_REVISION     0x01u /* 0x50 = CMIS 5.0 */
#define CMIS_OFF_MEM_MODEL    0x02u /* bit 7 clear = paged memory */
#define CMIS_OFF_MODULE_STATE 0x03u /* module state machine, bits 3:1 */
#define CMIS_OFF_MODULE_FLAGS 0x08u /* latched temp and Vcc flags */
#define CMIS_OFF_LOS_FLAGS    0x09u /* latched per-lane RX LOS, bit per lane */
#define CMIS_OFF_TXF_FLAGS    0x0Au /* latched per-lane TX fault */
#define CMIS_OFF_TEMP         0x0Eu /* int16, 1/256 C */
#define CMIS_OFF_VCC          0x10u /* uint16, 100 uV */
#define CMIS_OFF_ACTIVE_LANES 0x12u /* bit per lane currently carrying traffic */
#define CMIS_OFF_BANK_SELECT  0x7Eu
#define CMIS_OFF_PAGE_SELECT  0x7Fu

/* --- upper page 00h, static identity ------------------------------------ */
#define CMIS_P00_IDENTIFIER  0x80u /* repeats the lower page identifier */
#define CMIS_P00_VENDOR_NAME 0x81u /* 16 bytes, space padded ASCII */
#define CMIS_P00_VENDOR_OUI  0x91u /* 3 bytes */
#define CMIS_P00_VENDOR_PN   0x94u /* 16 bytes */
#define CMIS_P00_VENDOR_REV  0xA4u /* 2 bytes */
#define CMIS_P00_VENDOR_SN   0xA6u /* 16 bytes */
#define CMIS_P00_DATE_CODE   0xB6u /* 8 bytes, YYMMDDvv */

/* --- upper page 01h, advertising ---------------------------------------- */
#define CMIS_P01_MEDIA_TYPE  0x85u
#define CMIS_P01_LANE_COUNT  0x86u

/* --- upper page 02h, thresholds ----------------------------------------- */
/* Each set is four big endian 16 bit values: high alarm, low alarm, high
 * warning, low warning. Writable, because the host is allowed to tighten
 * them for its own margining. */
#define CMIS_P02_TEMP_TH     0x80u
#define CMIS_P02_VCC_TH      0x88u
#define CMIS_P02_TX_BIAS_TH  0x98u
#define CMIS_P02_TX_POWER_TH 0xA0u
#define CMIS_P02_RX_POWER_TH 0xA8u

/* --- upper page 10h, lane control --------------------------------------- */
#define CMIS_P10_TX_DISABLE 0x82u /* bit per lane, writable */

/* --- upper page 11h, per-lane monitors and state ------------------------ */
#define CMIS_P11_DP_STATE 0x80u /* one nibble per lane, two lanes per byte */
#define CMIS_P11_TX_BIAS  0x88u /* uint16 per lane, 2 uA */
#define CMIS_P11_TX_POWER 0x90u /* uint16 per lane, 0.1 uW */
#define CMIS_P11_RX_POWER 0x98u /* uint16 per lane, 0.1 uW */

#define CMIS_ID_QSFP_DD   0x18u
#define CMIS_REV_5_0      0x50u
#define CMIS_MEDIA_SMF    0x02u

void cmis_init(void);

/* Copies the live monitor, flag and state machine values into the map. Run
 * from a periodic task so a host read never has to wait on an ADC. */
void cmis_refresh(void);

/* A host read. Reading a latched flag byte clears it, which is why this is
 * not a const operation. */
uint8_t cmis_read(uint8_t offset);

/* A host write. Returns false when the offset is read only, which is what
 * the I2C target reports back as a NACK. */
bool cmis_write(uint8_t offset, uint8_t value);

uint8_t cmis_page(void);
uint8_t cmis_bank(void);

#endif /* OPTILINK_CMIS_H */
