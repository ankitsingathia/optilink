/*
 * link.h - the layer 1 state machines.
 *
 * Two levels, mirroring how CMIS splits the problem. A single module state
 * machine owns power sequencing for the whole part, and each lane then runs
 * its own data path state machine underneath it. Splitting them matters
 * because lanes fail independently: one dirty fibre should dark one lane,
 * not drop the other three.
 *
 * Every digital input is debounced before either machine looks at it. An
 * optical receiver sitting near its assert threshold will toggle LOS for as
 * long as you care to watch, and a state machine driven straight off that
 * pin will flap the link instead of reporting a marginal one.
 */
#ifndef OPTILINK_LINK_H
#define OPTILINK_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "optilink/optilink.h"

typedef enum {
    MODULE_RESET = 0,
    MODULE_LOW_PWR,
    MODULE_PWR_UP,
    MODULE_READY,
    MODULE_FAULT,
    MODULE_STATE_COUNT
} module_state_t;

typedef enum {
    DP_DEACTIVATED = 0,
    DP_INIT,
    DP_TX_TURN_ON,
    DP_ACTIVATED,
    DP_FAULT,
    DP_STATE_COUNT
} dp_state_t;

typedef enum {
    LANE_FAULT_NONE = 0,
    LANE_FAULT_TX,      /* laser driver asserted TX_FAULT */
    LANE_FAULT_LOS,     /* receiver lost the optical signal */
    LANE_FAULT_LOL,     /* clock recovery lost lock */
    LANE_FAULT_DDM      /* a monitor latched an alarm */
} lane_fault_t;

typedef struct {
    uint32_t     transitions;
    uint32_t     faults;
    uint32_t     retries;
    uint32_t     entered_ms;
    lane_fault_t last_fault;
} lane_stats_t;

void link_init(uint32_t now_ms);

/* One pass of both machines. Driven by the link task at 10 ms, which with a
 * three sample debounce puts input qualification at 30 ms. */
void link_service(uint32_t now_ms);

module_state_t link_module_state(void);
dp_state_t     link_lane_state(uint8_t lane);
bool           link_lane_enabled(uint8_t lane);
void           link_lane_enable(uint8_t lane, bool on);

/* Bit per lane, set while that lane is carrying traffic. */
uint8_t link_active_lane_mask(void);
bool    link_is_up(void);

/* Debounced digital fault state, one bit per lane. These are live, not
 * latched: cmis.c owns the latching, because the latch exists for the
 * benefit of a slow host poll rather than for the state machines. */
uint8_t link_los_mask(void);
uint8_t link_tx_fault_mask(void);

const lane_stats_t *link_lane_stats(uint8_t lane);
uint32_t            link_module_faults(void);

const char *link_module_state_name(module_state_t s);
const char *link_dp_state_name(dp_state_t s);
const char *link_fault_name(lane_fault_t f);

#endif /* OPTILINK_LINK_H */
