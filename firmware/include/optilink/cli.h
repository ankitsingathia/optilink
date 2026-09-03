/*
 * cli.h - the debug console.
 *
 * Line in, lines out, and every response ends with either "ok" or
 * "err <reason>". That sentinel is what lets an automated harness know a
 * reply is complete without guessing at timeouts, and it is the single
 * reason the Python tests can run deterministically rather than sleeping
 * between commands.
 */
#ifndef OPTILINK_CLI_H
#define OPTILINK_CLI_H

#include <stdint.h>

void cli_init(void);

/* Handles at most one complete line per call. Registered as the highest
 * priority task so console latency stays at one tick. */
void cli_task(void *ctx);

#endif /* OPTILINK_CLI_H */
