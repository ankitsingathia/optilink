#include "optilink/ringbuf.h"

#include "suites.h"
#include "tinytest.h"

void suite_ringbuf(void)
{
    ringbuf_t rb;
    uint8_t   store[8];

    TT_CASE("init rejects bad capacity");
    CHECK(!ringbuf_init(&rb, store, 7u));  /* not a power of two */
    CHECK(!ringbuf_init(&rb, store, 0u));
    CHECK(!ringbuf_init(&rb, store, 1u));  /* one slot leaves none usable */
    CHECK(!ringbuf_init(&rb, NULL, 8u));
    CHECK(ringbuf_init(&rb, store, 8u));

    TT_CASE("fifo order");
    CHECK(ringbuf_empty(&rb));
    CHECK_EQ(ringbuf_pop(&rb), -1);
    CHECK(ringbuf_push(&rb, 0x11u));
    CHECK(ringbuf_push(&rb, 0x22u));
    CHECK(ringbuf_push(&rb, 0x33u));
    CHECK_EQ(ringbuf_count(&rb), 3);
    CHECK_EQ(ringbuf_pop(&rb), 0x11);
    CHECK_EQ(ringbuf_pop(&rb), 0x22);
    CHECK_EQ(ringbuf_pop(&rb), 0x33);
    CHECK_EQ(ringbuf_pop(&rb), -1);
    CHECK(ringbuf_empty(&rb));

    TT_CASE("full at capacity minus one");
    CHECK(ringbuf_init(&rb, store, 8u));
    for (uint8_t i = 0u; i < 7u; i++) {
        CHECK(ringbuf_push(&rb, i));
    }
    /* The eighth slot is deliberately unusable: it is what keeps full and
     * empty distinguishable without a shared counter. */
    CHECK(!ringbuf_push(&rb, 99u));
    CHECK_EQ(ringbuf_count(&rb), 7);
    CHECK_EQ(rb.dropped, 1);

    TT_CASE("wraps without losing order");
    CHECK_EQ(ringbuf_pop(&rb), 0);
    CHECK_EQ(ringbuf_pop(&rb), 1);
    CHECK(ringbuf_push(&rb, 0xAAu));
    CHECK(ringbuf_push(&rb, 0xBBu));
    for (uint8_t i = 2u; i < 7u; i++) {
        CHECK_EQ(ringbuf_pop(&rb), i);
    }
    CHECK_EQ(ringbuf_pop(&rb), 0xAA);
    CHECK_EQ(ringbuf_pop(&rb), 0xBB);
    CHECK(ringbuf_empty(&rb));

    TT_CASE("reset drops pending data");
    CHECK(ringbuf_push(&rb, 1u));
    CHECK(ringbuf_push(&rb, 2u));
    ringbuf_reset(&rb);
    CHECK(ringbuf_empty(&rb));
    CHECK_EQ(rb.dropped, 0);
}
