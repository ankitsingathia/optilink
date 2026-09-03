#include "optilink/link.h"

#include <stddef.h>

#include "optilink/ddm.h"
#include "optilink/hal.h"

/* Power supplies and laser bias need settling time before anything
 * downstream should believe a reading. These are the numbers the sequencing
 * is built around. */
#define MODULE_PWR_UP_MS        50u
#define MODULE_FAULT_HOLDOFF_MS 200u
#define MODULE_MAX_RETRIES      3u

#define DP_INIT_MS          20u
#define DP_TX_TURN_ON_MS    30u
#define DP_FAULT_HOLDOFF_MS 100u
#define DP_MAX_RETRIES      5u

typedef struct {
    bool    stable;
    bool    candidate;
    uint8_t agree;
} debounce_t;

typedef struct {
    dp_state_t   state;
    bool         enabled;
    uint32_t     entered_ms;
    uint32_t     retries;
    debounce_t   los;
    debounce_t   tx_fault;
    debounce_t   cdr_lol;
    lane_stats_t stats;
} lane_t;

static module_state_t s_module;
static uint32_t       s_module_entered_ms;
static uint32_t       s_module_retries;
static uint32_t       s_module_faults;
static lane_t         s_lane[OPTILINK_LANES];

/* Returns true when the debounced value changed on this pass. */
static bool debounce_update(debounce_t *d, bool raw)
{
    if (raw == d->stable) {
        d->agree     = 0u;
        d->candidate = raw;
        return false;
    }
    if (raw != d->candidate) {
        d->candidate = raw;
        d->agree     = 1u;
        return false;
    }
    d->agree++;
    if (d->agree >= OPTILINK_DEBOUNCE_SAMPLES) {
        d->stable = raw;
        d->agree  = 0u;
        return true;
    }
    return false;
}

static void debounce_init(debounce_t *d, bool initial)
{
    d->stable    = initial;
    d->candidate = initial;
    d->agree     = 0u;
}

static void module_enter(module_state_t s, uint32_t now_ms)
{
    if (s == s_module) {
        return;
    }
    s_module            = s;
    s_module_entered_ms = now_ms;
    if (s == MODULE_FAULT) {
        s_module_faults++;
    }
}

static void lane_enter(uint8_t l, dp_state_t s, uint32_t now_ms, lane_fault_t why)
{
    lane_t *ln = &s_lane[l];

    if (s == ln->state) {
        return;
    }
    ln->state            = s;
    ln->entered_ms       = now_ms;
    ln->stats.entered_ms = now_ms;
    ln->stats.transitions++;

    if (s == DP_FAULT) {
        ln->stats.faults++;
        ln->stats.last_fault = why;
        /* Dark the laser the instant we decide the lane is bad. Leaving a
         * faulted transmitter on is both a link hazard and an eye safety
         * one. */
        hal_tx_enable(l, false);
    } else if (s == DP_DEACTIVATED) {
        hal_tx_enable(l, false);
    }
}

void link_init(uint32_t now_ms)
{
    s_module            = MODULE_RESET;
    s_module_entered_ms = now_ms;
    s_module_retries    = 0u;
    s_module_faults     = 0u;

    for (uint8_t l = 0u; l < OPTILINK_LANES; l++) {
        s_lane[l].state             = DP_DEACTIVATED;
        s_lane[l].enabled           = true;
        s_lane[l].entered_ms        = now_ms;
        s_lane[l].retries           = 0u;
        s_lane[l].stats.transitions = 0u;
        s_lane[l].stats.faults      = 0u;
        s_lane[l].stats.retries     = 0u;
        s_lane[l].stats.entered_ms  = now_ms;
        s_lane[l].stats.last_fault  = LANE_FAULT_NONE;
        debounce_init(&s_lane[l].los, false);
        debounce_init(&s_lane[l].tx_fault, false);
        debounce_init(&s_lane[l].cdr_lol, false);
        hal_tx_enable(l, false);
    }
}

static void module_service(uint32_t now_ms)
{
    bool     present = hal_gpio_read(HAL_PIN_MODULE_PRESENT);
    bool     reset_n = hal_gpio_read(HAL_PIN_RESET_N);
    bool     lpmode  = hal_gpio_read(HAL_PIN_LPMODE);
    uint32_t held    = now_ms - s_module_entered_ms;

    /* Reset and removal are asynchronous and outrank whatever state we
     * thought we were in. */
    if (!reset_n || !present) {
        if (s_module != MODULE_RESET) {
            for (uint8_t l = 0u; l < OPTILINK_LANES; l++) {
                lane_enter(l, DP_DEACTIVATED, now_ms, LANE_FAULT_NONE);
            }
            module_enter(MODULE_RESET, now_ms);
        }
        return;
    }

    switch (s_module) {
    case MODULE_RESET:
        s_module_retries = 0u;
        module_enter(MODULE_LOW_PWR, now_ms);
        break;

    case MODULE_LOW_PWR:
        if (!lpmode) {
            module_enter(MODULE_PWR_UP, now_ms);
        }
        break;

    case MODULE_PWR_UP:
        if (held >= MODULE_PWR_UP_MS) {
            module_enter(MODULE_READY, now_ms);
        }
        break;

    case MODULE_READY:
        if (lpmode) {
            for (uint8_t l = 0u; l < OPTILINK_LANES; l++) {
                lane_enter(l, DP_DEACTIVATED, now_ms, LANE_FAULT_NONE);
            }
            module_enter(MODULE_LOW_PWR, now_ms);
        } else if (ddm_any_alarm()) {
            for (uint8_t l = 0u; l < OPTILINK_LANES; l++) {
                lane_enter(l, DP_FAULT, now_ms, LANE_FAULT_DDM);
            }
            module_enter(MODULE_FAULT, now_ms);
        }
        break;

    case MODULE_FAULT:
        /* Back off before retrying, and stop retrying entirely after a few
         * attempts. A module that cannot come up should stay down and
         * visible rather than cycling its lasers forever. */
        if (held >= MODULE_FAULT_HOLDOFF_MS && !ddm_any_alarm()) {
            if (s_module_retries < MODULE_MAX_RETRIES) {
                s_module_retries++;
                module_enter(MODULE_LOW_PWR, now_ms);
            }
        }
        break;

    default:
        module_enter(MODULE_RESET, now_ms);
        break;
    }
}

static void lane_service(uint8_t l, uint32_t now_ms)
{
    lane_t         *ln = &s_lane[l];
    hal_lane_pins_t pins;
    uint32_t        held = now_ms - ln->entered_ms;
    bool            los, txf, lol;

    if (!hal_lane_pins_read(l, &pins)) {
        return;
    }
    (void)debounce_update(&ln->los, pins.rx_los);
    (void)debounce_update(&ln->tx_fault, pins.tx_fault);
    (void)debounce_update(&ln->cdr_lol, pins.cdr_lol);

    los = ln->los.stable;
    txf = ln->tx_fault.stable;
    lol = ln->cdr_lol.stable;

    if (s_module != MODULE_READY) {
        if (ln->state != DP_DEACTIVATED && ln->state != DP_FAULT) {
            lane_enter(l, DP_DEACTIVATED, now_ms, LANE_FAULT_NONE);
        }
        return;
    }

    switch (ln->state) {
    case DP_DEACTIVATED:
        if (ln->enabled) {
            ln->retries = 0u;
            lane_enter(l, DP_INIT, now_ms, LANE_FAULT_NONE);
        }
        break;

    case DP_INIT:
        if (!ln->enabled) {
            lane_enter(l, DP_DEACTIVATED, now_ms, LANE_FAULT_NONE);
        } else if (txf) {
            lane_enter(l, DP_FAULT, now_ms, LANE_FAULT_TX);
        } else if (held >= DP_INIT_MS) {
            hal_tx_enable(l, true);
            lane_enter(l, DP_TX_TURN_ON, now_ms, LANE_FAULT_NONE);
        }
        break;

    case DP_TX_TURN_ON:
        if (txf) {
            lane_enter(l, DP_FAULT, now_ms, LANE_FAULT_TX);
        } else if (held >= DP_TX_TURN_ON_MS) {
            /* Only declare the lane up once the far end is actually there:
             * light present and the CDR locked to it. */
            if (!los && !lol) {
                lane_enter(l, DP_ACTIVATED, now_ms, LANE_FAULT_NONE);
            } else if (held >= (DP_TX_TURN_ON_MS * 4u)) {
                lane_enter(l, DP_FAULT, now_ms,
                           los ? LANE_FAULT_LOS : LANE_FAULT_LOL);
            }
        }
        break;

    case DP_ACTIVATED:
        if (!ln->enabled) {
            lane_enter(l, DP_DEACTIVATED, now_ms, LANE_FAULT_NONE);
        } else if (txf) {
            lane_enter(l, DP_FAULT, now_ms, LANE_FAULT_TX);
        } else if (los) {
            lane_enter(l, DP_FAULT, now_ms, LANE_FAULT_LOS);
        } else if (lol) {
            lane_enter(l, DP_FAULT, now_ms, LANE_FAULT_LOL);
        }
        break;

    case DP_FAULT:
        if (!ln->enabled) {
            lane_enter(l, DP_DEACTIVATED, now_ms, LANE_FAULT_NONE);
        } else if (held >= DP_FAULT_HOLDOFF_MS && !txf && !los && !lol) {
            if (ln->retries < DP_MAX_RETRIES) {
                ln->retries++;
                ln->stats.retries++;
                lane_enter(l, DP_INIT, now_ms, LANE_FAULT_NONE);
            }
        }
        break;

    default:
        lane_enter(l, DP_DEACTIVATED, now_ms, LANE_FAULT_NONE);
        break;
    }
}

void link_service(uint32_t now_ms)
{
    module_service(now_ms);
    for (uint8_t l = 0u; l < OPTILINK_LANES; l++) {
        lane_service(l, now_ms);
    }
    /* INT_N is active low and is the only way a module gets the attention of
     * the host between polls. */
    hal_gpio_write(HAL_PIN_INT_N, !ddm_any_alarm());
}

module_state_t link_module_state(void)
{
    return s_module;
}

uint32_t link_module_faults(void)
{
    return s_module_faults;
}

dp_state_t link_lane_state(uint8_t lane)
{
    return (lane < OPTILINK_LANES) ? s_lane[lane].state : DP_DEACTIVATED;
}

bool link_lane_enabled(uint8_t lane)
{
    return (lane < OPTILINK_LANES) ? s_lane[lane].enabled : false;
}

void link_lane_enable(uint8_t lane, bool on)
{
    if (lane < OPTILINK_LANES) {
        s_lane[lane].enabled = on;
    }
}

uint8_t link_active_lane_mask(void)
{
    uint8_t m = 0u;

    for (uint8_t l = 0u; l < OPTILINK_LANES; l++) {
        if (s_lane[l].state == DP_ACTIVATED) {
            m |= (uint8_t)(1u << l);
        }
    }
    return m;
}

bool link_is_up(void)
{
    return s_module == MODULE_READY && link_active_lane_mask() != 0u;
}

uint8_t link_los_mask(void)
{
    uint8_t m = 0u;

    for (uint8_t l = 0u; l < OPTILINK_LANES; l++) {
        if (s_lane[l].los.stable) {
            m |= (uint8_t)(1u << l);
        }
    }
    return m;
}

uint8_t link_tx_fault_mask(void)
{
    uint8_t m = 0u;

    for (uint8_t l = 0u; l < OPTILINK_LANES; l++) {
        if (s_lane[l].tx_fault.stable) {
            m |= (uint8_t)(1u << l);
        }
    }
    return m;
}

const lane_stats_t *link_lane_stats(uint8_t lane)
{
    return (lane < OPTILINK_LANES) ? &s_lane[lane].stats : NULL;
}

const char *link_module_state_name(module_state_t s)
{
    static const char *const N[MODULE_STATE_COUNT] = {
        "RESET", "LOW_PWR", "PWR_UP", "READY", "FAULT"
    };
    return (s < MODULE_STATE_COUNT) ? N[s] : "?";
}

const char *link_dp_state_name(dp_state_t s)
{
    static const char *const N[DP_STATE_COUNT] = {
        "DEACTIVATED", "INIT", "TX_TURN_ON", "ACTIVATED", "FAULT"
    };
    return (s < DP_STATE_COUNT) ? N[s] : "?";
}

const char *link_fault_name(lane_fault_t f)
{
    switch (f) {
    case LANE_FAULT_NONE: return "NONE";
    case LANE_FAULT_TX:   return "TX_FAULT";
    case LANE_FAULT_LOS:  return "RX_LOS";
    case LANE_FAULT_LOL:  return "CDR_LOL";
    case LANE_FAULT_DDM:  return "DDM_ALARM";
    default:              return "?";
    }
}
