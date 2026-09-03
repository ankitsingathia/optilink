/*
 * i2c.h - the I2C target that fronts the management memory map.
 *
 * The host is the bus master; this module is a target at 1010000b. A host
 * transaction is either a write of one offset byte followed by data, or a
 * write of the offset followed by a repeated START and a read burst. In both
 * cases the offset auto-increments after every data byte and wraps at 256,
 * which is what lets a host pull the whole address space in one burst rather
 * than 256 separate transactions.
 *
 * On the Cortex-M port these entry points are called from the I2C
 * peripheral's interrupt handler. They are written to be short and
 * allocation free for that reason.
 */
#ifndef OPTILINK_I2C_H
#define OPTILINK_I2C_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t transactions;
    uint32_t bytes_read;
    uint32_t bytes_written;
    uint32_t nacks;          /* writes refused because the offset is read only */
    uint32_t addr_mismatch;
} i2c_stats_t;

void i2c_target_init(uint8_t addr);

/* START seen with a matching address. is_read selects the direction. Returns
 * false when the address is not ours, in which case the port should leave
 * the bus alone. */
bool i2c_target_start(uint8_t addr, bool is_read);

/* Host wrote a byte. The first byte of a write transaction is the offset;
 * every byte after it is data. Returns false to signal a NACK. */
bool i2c_target_write(uint8_t byte);

/* Host is clocking a byte out of us. */
uint8_t i2c_target_read(void);

void i2c_target_stop(void);

const i2c_stats_t *i2c_target_stats(void);
uint8_t            i2c_target_offset(void);

#endif /* OPTILINK_I2C_H */
