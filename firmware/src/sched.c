#include "optilink/sched.h"

#include <stddef.h>

#include "optilink/optilink.h"

static sched_task_t s_tasks[SCHED_MAX_TASKS];
static uint8_t      s_count;

/* Dispatch order is precomputed at registration time so that the hot path is
 * a straight walk instead of a priority search every tick. */
static uint8_t s_order[SCHED_MAX_TASKS];

void sched_init(uint32_t now_ms)
{
    (void)now_ms;
    for (uint8_t i = 0u; i < SCHED_MAX_TASKS; i++) {
        s_tasks[i].used = false;
        s_order[i]      = 0u;
    }
    s_count = 0u;
}

static void order_insert(uint8_t id)
{
    uint8_t pos = s_count;

    /* Insertion sort by priority, stable so that equal priorities keep
     * registration order. The table is eight entries; nothing cleverer is
     * worth the code space. */
    while (pos > 0u && s_tasks[s_order[pos - 1u]].priority > s_tasks[id].priority) {
        s_order[pos] = s_order[pos - 1u];
        pos--;
    }
    s_order[pos] = id;
}

int sched_register(const char *name, sched_fn_t fn, void *ctx,
                   uint32_t period_ms, uint8_t priority)
{
    uint8_t id;

    if (fn == NULL || name == NULL || s_count >= SCHED_MAX_TASKS) {
        return -1;
    }
    if (period_ms == 0u || (period_ms % OPTILINK_TICK_MS) != 0u) {
        return -1;
    }

    id                       = s_count;
    s_tasks[id].name         = name;
    s_tasks[id].fn           = fn;
    s_tasks[id].ctx          = ctx;
    s_tasks[id].period_ms    = period_ms;
    s_tasks[id].due_ms       = period_ms;
    s_tasks[id].priority     = priority;
    s_tasks[id].used         = true;
    s_tasks[id].runs         = 0u;
    s_tasks[id].overruns     = 0u;
    s_tasks[id].max_latency_ms = 0u;

    order_insert(id);
    s_count++;
    return (int)id;
}

uint32_t sched_service(uint32_t now_ms)
{
    uint32_t dispatched = 0u;

    for (uint8_t i = 0u; i < s_count; i++) {
        sched_task_t *t = &s_tasks[s_order[i]];
        uint32_t      late;

        if (!t->used) {
            continue;
        }
        /* Unsigned wrap makes this comparison correct across the 49 day
         * millisecond rollover, which a plain (now < due) test would not be. */
        if ((int32_t)(now_ms - t->due_ms) < 0) {
            continue;
        }

        late = now_ms - t->due_ms;
        if (late > t->max_latency_ms) {
            t->max_latency_ms = late;
        }

        t->fn(t->ctx);
        t->runs++;
        dispatched++;

        if (late >= t->period_ms) {
            /* We lost at least one whole period. Running the backlog now
             * would monopolise the loop and starve everything below us, so
             * drop the missed slots, resync to the current instant, and
             * record that it happened. */
            t->overruns++;
            t->due_ms = now_ms + t->period_ms;
        } else {
            t->due_ms += t->period_ms;
        }
    }
    return dispatched;
}

uint8_t sched_task_count(void)
{
    return s_count;
}

const sched_task_t *sched_task(uint8_t id)
{
    if (id >= s_count) {
        return NULL;
    }
    return &s_tasks[id];
}
