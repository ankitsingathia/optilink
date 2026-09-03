/*
 * sched.h - fixed priority cooperative tick scheduler.
 *
 * There is no preemption and no per task stack: tasks are run to completion
 * from the main loop, so the whole firmware needs exactly one stack and no
 * mutexes. That is the right trade for a module controller, where every job
 * is a short periodic poll and the worst case cost of a missed deadline is a
 * late telemetry sample, not a dropped packet.
 *
 * What the scheduler does owe the caller is honesty about lateness, so every
 * task carries its own dispatch latency and overrun counters and the CLI can
 * read them back.
 */
#ifndef OPTILINK_SCHED_H
#define OPTILINK_SCHED_H

#include <stdbool.h>
#include <stdint.h>

#define SCHED_MAX_TASKS 8

typedef void (*sched_fn_t)(void *ctx);

typedef struct {
    const char *name;
    sched_fn_t  fn;
    void       *ctx;
    uint32_t    period_ms;
    uint32_t    due_ms;      /* absolute time this task should next run */
    uint8_t     priority;    /* 0 is highest */
    bool        used;

    uint32_t    runs;
    uint32_t    overruns;    /* fell a whole period behind and was resynced */
    uint32_t    max_latency_ms;
} sched_task_t;

void sched_init(uint32_t now_ms);

/* Returns the task id, or -1 if the table is full or the arguments are
 * unusable. period_ms must be a non-zero multiple of OPTILINK_TICK_MS. */
int sched_register(const char *name, sched_fn_t fn, void *ctx,
                   uint32_t period_ms, uint8_t priority);

/* Runs every task that is due at now_ms, highest priority first. Call this
 * once per tick from the main loop. Returns how many tasks it dispatched. */
uint32_t sched_service(uint32_t now_ms);

uint8_t                 sched_task_count(void);
const sched_task_t     *sched_task(uint8_t id);

#endif /* OPTILINK_SCHED_H */
