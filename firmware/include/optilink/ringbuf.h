/*
 * ringbuf.h - single producer, single consumer byte ring.
 *
 * The UART ISR is the producer and the CLI task is the consumer, so the two
 * never touch the same index: the writer only advances head, the reader only
 * advances tail. That is what lets this be lock free on a single core with
 * no critical section around it. Capacity is forced to a power of two so the
 * wrap is a mask rather than a modulo.
 */
#ifndef OPTILINK_RINGBUF_H
#define OPTILINK_RINGBUF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t         *buf;
    uint16_t         mask;         /* capacity - 1, capacity is a power of 2 */
    volatile uint16_t head;        /* written by producer only */
    volatile uint16_t tail;        /* written by consumer only */
    volatile uint32_t dropped;     /* producer had nowhere to put a byte */
} ringbuf_t;

/* capacity must be a power of two and no larger than 32768. Returns false
 * and leaves rb unusable otherwise. */
bool     ringbuf_init(ringbuf_t *rb, uint8_t *storage, uint16_t capacity);
bool     ringbuf_push(ringbuf_t *rb, uint8_t byte);   /* producer side */
int      ringbuf_pop(ringbuf_t *rb);                  /* consumer side, -1 if empty */
uint16_t ringbuf_count(const ringbuf_t *rb);
bool     ringbuf_empty(const ringbuf_t *rb);
void     ringbuf_reset(ringbuf_t *rb);

#endif /* OPTILINK_RINGBUF_H */
