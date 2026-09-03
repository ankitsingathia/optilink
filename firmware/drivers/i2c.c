#include "optilink/i2c.h"

#include <string.h>

#include "optilink/cmis.h"

static uint8_t     s_addr;
static bool        s_active;
static bool        s_have_offset;
static uint8_t     s_offset;
static i2c_stats_t s_stats;

void i2c_target_init(uint8_t addr)
{
    s_addr        = addr;
    s_active      = false;
    s_have_offset = false;
    s_offset      = 0u;
    memset(&s_stats, 0, sizeof s_stats);
}

bool i2c_target_start(uint8_t addr, bool is_read)
{
    if (addr != s_addr) {
        s_stats.addr_mismatch++;
        s_active = false;
        return false;
    }

    /* A repeated START into a read reuses the offset the preceding write
     * phase left behind, which is the whole point of the combined format.
     * A fresh write transaction discards it and waits for a new one. */
    if (!is_read) {
        s_have_offset = false;
    }
    s_active = true;
    s_stats.transactions++;
    return true;
}

bool i2c_target_write(uint8_t byte)
{
    if (!s_active) {
        return false;
    }
    if (!s_have_offset) {
        s_offset      = byte;
        s_have_offset = true;
        return true;
    }
    if (!cmis_write(s_offset, byte)) {
        /* Refusing the byte still advances the pointer. A host walking a
         * block write across a mixed read-only and read-write region
         * expects the addresses to line up regardless of which bytes stuck. */
        s_offset = (uint8_t)(s_offset + 1u);
        s_stats.nacks++;
        return false;
    }
    s_offset = (uint8_t)(s_offset + 1u);
    s_stats.bytes_written++;
    return true;
}

uint8_t i2c_target_read(void)
{
    uint8_t v;

    if (!s_active) {
        return 0xFFu;
    }
    v        = cmis_read(s_offset);
    s_offset = (uint8_t)(s_offset + 1u);
    s_stats.bytes_read++;
    return v;
}

void i2c_target_stop(void)
{
    s_active = false;
}

const i2c_stats_t *i2c_target_stats(void)
{
    return &s_stats;
}

uint8_t i2c_target_offset(void)
{
    return s_offset;
}
