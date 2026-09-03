#include "optilink/ringbuf.h"

bool ringbuf_init(ringbuf_t *rb, uint8_t *storage, uint16_t capacity)
{
    if (rb == NULL || storage == NULL) {
        return false;
    }
    /* Power of two, and big enough that head/tail cannot alias. */
    if (capacity < 2u || capacity > 32768u || (capacity & (capacity - 1u)) != 0u) {
        return false;
    }
    rb->buf     = storage;
    rb->mask    = (uint16_t)(capacity - 1u);
    rb->head    = 0u;
    rb->tail    = 0u;
    rb->dropped = 0u;
    return true;
}

bool ringbuf_push(ringbuf_t *rb, uint8_t byte)
{
    uint16_t head = rb->head;
    uint16_t next = (uint16_t)((head + 1u) & rb->mask);

    /* Full is next==tail rather than head==tail, which costs one slot but
     * keeps empty and full distinguishable without a separate count that
     * both sides would have to write. */
    if (next == rb->tail) {
        rb->dropped++;
        return false;
    }
    rb->buf[head] = byte;
    rb->head      = next;
    return true;
}

int ringbuf_pop(ringbuf_t *rb)
{
    uint16_t tail = rb->tail;
    uint8_t  byte;

    if (tail == rb->head) {
        return -1;
    }
    byte     = rb->buf[tail];
    rb->tail = (uint16_t)((tail + 1u) & rb->mask);
    return (int)byte;
}

uint16_t ringbuf_count(const ringbuf_t *rb)
{
    return (uint16_t)((rb->head - rb->tail) & rb->mask);
}

bool ringbuf_empty(const ringbuf_t *rb)
{
    return rb->head == rb->tail;
}

void ringbuf_reset(ringbuf_t *rb)
{
    rb->tail    = rb->head;
    rb->dropped = 0u;
}
