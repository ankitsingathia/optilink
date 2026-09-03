#include "optilink/sched.h"

#include "optilink/optilink.h"
#include "suites.h"
#include "tinytest.h"

static unsigned s_order[16];
static unsigned s_order_len;
static unsigned s_runs_a;
static unsigned s_runs_b;

static void record(void *ctx)
{
    if (s_order_len < 16u) {
        s_order[s_order_len++] = (unsigned)(uintptr_t)ctx;
    }
}

static void bump_a(void *ctx)
{
    (void)ctx;
    s_runs_a++;
}

static void bump_b(void *ctx)
{
    (void)ctx;
    s_runs_b++;
}

void suite_sched(void)
{
    TT_CASE("register validates arguments");
    sched_init(0u);
    CHECK_EQ(sched_register("null", NULL, NULL, 1u, 0u), -1);
    CHECK_EQ(sched_register("zero", bump_a, NULL, 0u, 0u), -1);
    CHECK_EQ(sched_register("noname", bump_a, NULL, 1u, 0u) >= 0, 1);
    /* The tick is the quantum, so a period that is not a multiple of it is
     * refused rather than silently rounded. With a 1 ms tick every period
     * qualifies, so this only bites if the tick is ever coarsened. */
    CHECK_EQ(OPTILINK_TICK_MS, 1);

    TT_CASE("table is finite");
    sched_init(0u);
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        CHECK_EQ(sched_register("t", bump_a, NULL, 1u, 0u), i);
    }
    CHECK_EQ(sched_register("overflow", bump_a, NULL, 1u, 0u), -1);
    CHECK_EQ(sched_task_count(), SCHED_MAX_TASKS);
    CHECK(sched_task(SCHED_MAX_TASKS) == NULL);

    TT_CASE("higher priority dispatches first");
    sched_init(0u);
    s_order_len = 0u;
    /* Registered low priority first, so ordering can only come from the
     * priority sort and not from registration order. */
    CHECK(sched_register("low", record, (void *)(uintptr_t)3u, 1u, 9u) >= 0);
    CHECK(sched_register("high", record, (void *)(uintptr_t)1u, 1u, 0u) >= 0);
    CHECK(sched_register("mid", record, (void *)(uintptr_t)2u, 1u, 5u) >= 0);
    CHECK_EQ(sched_service(1u), 3);
    CHECK_EQ(s_order_len, 3);
    CHECK_EQ(s_order[0], 1);
    CHECK_EQ(s_order[1], 2);
    CHECK_EQ(s_order[2], 3);

    TT_CASE("periods are honoured");
    sched_init(0u);
    s_runs_a = 0u;
    s_runs_b = 0u;
    CHECK(sched_register("fast", bump_a, NULL, 1u, 0u) >= 0);
    CHECK(sched_register("slow", bump_b, NULL, 10u, 1u) >= 0);
    for (uint32_t t = 1u; t <= 100u; t++) {
        (void)sched_service(t);
    }
    CHECK_EQ(s_runs_a, 100);
    CHECK_EQ(s_runs_b, 10);

    TT_CASE("a lost period is counted, not replayed");
    sched_init(0u);
    s_runs_b = 0u;
    CHECK(sched_register("slow", bump_b, NULL, 10u, 0u) >= 0);
    /* Jump 500 ms in one service call, as a long blocking task would cause.
     * The scheduler must run the task once and resync, not run it fifty
     * times and starve everything below it. */
    CHECK_EQ(sched_service(500u), 1);
    CHECK_EQ(s_runs_b, 1);
    CHECK_EQ(sched_task(0)->overruns, 1);
    CHECK_EQ(sched_task(0)->max_latency_ms, 490);
    /* Resynced to now + period, so the next due time is 510. */
    CHECK_EQ(sched_service(509u), 0);
    CHECK_EQ(sched_service(510u), 1);
    CHECK_EQ(sched_task(0)->overruns, 1);
}
