#include "optilink/cli.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "optilink/cmis.h"
#include "optilink/ddm.h"
#include "optilink/eth.h"
#include "optilink/hal.h"
#include "optilink/i2c.h"
#include "optilink/link.h"
#include "optilink/sched.h"
#include "optilink/uart.h"

#include "../hal/hal_sim.h"

/* One scratch frame buffer. Static rather than automatic because this runs
 * on the main stack and a 1.5 KB local would be most of it.
 *
 * Deliberately larger than the largest frame the parser accepts, so that an
 * oversize frame can still be handed to eth_parse and rejected there. If the
 * console enforced the length itself it would be duplicating the parser's
 * policy, and the parser's own reject path would be unreachable from the
 * outside and therefore untested. */
#define CLI_FRAME_CAP (ETH_MAX_FRAME + 64u)
static uint8_t s_frame[CLI_FRAME_CAP];

static const char *s_cursor;

static void reply_ok(void)
{
    uart_puts("ok\n");
}

static void reply_err(const char *why)
{
    uart_printf("err %s\n", why);
}

/* --- tokenising ---------------------------------------------------------- */

static void tok_begin(const char *line)
{
    s_cursor = line;
}

/* Returns a pointer into the line at the next token and its length, without
 * copying. Used for the frame hex string, which is far too long to want a
 * scratch copy of on a part with this much RAM. */
static const char *tok_next_raw(size_t *len)
{
    const char *start;

    while (*s_cursor == ' ' || *s_cursor == '\t') {
        s_cursor++;
    }
    if (*s_cursor == '\0') {
        return NULL;
    }
    start = s_cursor;
    while (*s_cursor != '\0' && *s_cursor != ' ' && *s_cursor != '\t') {
        s_cursor++;
    }
    *len = (size_t)(s_cursor - start);
    return start;
}

/* Returns the next whitespace delimited token into dst, or false at the end
 * of the line. */
static bool tok_next(char *dst, size_t cap)
{
    size_t n = 0u;

    while (*s_cursor == ' ' || *s_cursor == '\t') {
        s_cursor++;
    }
    if (*s_cursor == '\0') {
        return false;
    }
    while (*s_cursor != '\0' && *s_cursor != ' ' && *s_cursor != '\t') {
        if (n < (cap - 1u)) {
            dst[n++] = *s_cursor;
        }
        s_cursor++;
    }
    dst[n] = '\0';
    return true;
}

static bool parse_u32(const char *s, uint32_t *out)
{
    uint32_t v    = 0u;
    bool     any  = false;
    uint8_t  base = 10u;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16u;
        s += 2;
    }
    while (*s != '\0') {
        uint32_t d;

        if (*s >= '0' && *s <= '9') {
            d = (uint32_t)(*s - '0');
        } else if (base == 16u && *s >= 'a' && *s <= 'f') {
            d = (uint32_t)(*s - 'a') + 10u;
        } else if (base == 16u && *s >= 'A' && *s <= 'F') {
            d = (uint32_t)(*s - 'A') + 10u;
        } else {
            return false;
        }
        if (d >= base) {
            return false;
        }
        v = (v * base) + d;
        any = true;
        s++;
    }
    *out = v;
    return any;
}

static bool next_u32(uint32_t *out)
{
    char t[16];

    return tok_next(t, sizeof t) && parse_u32(t, out);
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return (c - 'a') + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return (c - 'A') + 10;
    }
    return -1;
}

/* --- command handlers ---------------------------------------------------- */

static void cmd_id(void)
{
    uart_printf("fw=optilink version=%s lanes=%u tick_ms=%u\n",
                OPTILINK_VERSION_STR, (uint32_t)OPTILINK_LANES,
                (uint32_t)OPTILINK_TICK_MS);
    reply_ok();
}

static void cmd_mon(void)
{
    uart_printf("temp=%d vcc=%d\n", ddm_value(DDM_IDX_TEMP), ddm_value(DDM_IDX_VCC));
    for (uint8_t l = 0u; l < OPTILINK_LANES; l++) {
        uart_printf("lane%u bias=%d txpower=%d rxpower=%d\n", (uint32_t)l,
                    ddm_value(DDM_IDX_TX_BIAS(l)), ddm_value(DDM_IDX_TX_POWER(l)),
                    ddm_value(DDM_IDX_RX_POWER(l)));
    }
    reply_ok();
}

static void cmd_flags(void)
{
    /* Peeks rather than clears. Clearing is what an I2C read of the flag
     * bytes does, and a test needs to be able to look without disturbing
     * that. */
    uart_printf("temp=0x%02x vcc=0x%02x\n", (uint32_t)ddm_flags_peek(DDM_IDX_TEMP),
                (uint32_t)ddm_flags_peek(DDM_IDX_VCC));
    for (uint8_t l = 0u; l < OPTILINK_LANES; l++) {
        uart_printf("lane%u bias=0x%02x txpower=0x%02x rxpower=0x%02x\n", (uint32_t)l,
                    (uint32_t)ddm_flags_peek(DDM_IDX_TX_BIAS(l)),
                    (uint32_t)ddm_flags_peek(DDM_IDX_TX_POWER(l)),
                    (uint32_t)ddm_flags_peek(DDM_IDX_RX_POWER(l)));
    }
    uart_printf("any_alarm=%u\n", (uint32_t)(ddm_any_alarm() ? 1u : 0u));
    reply_ok();
}

static void cmd_link(void)
{
    uart_printf("module=%s faults=%u active_mask=0x%02x up=%u\n",
                link_module_state_name(link_module_state()), link_module_faults(),
                (uint32_t)link_active_lane_mask(),
                (uint32_t)(link_is_up() ? 1u : 0u));
    for (uint8_t l = 0u; l < OPTILINK_LANES; l++) {
        const lane_stats_t *st = link_lane_stats(l);

        uart_printf("lane%u state=%s enabled=%u tx_on=%u transitions=%u faults=%u "
                    "retries=%u last_fault=%s\n",
                    (uint32_t)l, link_dp_state_name(link_lane_state(l)),
                    (uint32_t)(link_lane_enabled(l) ? 1u : 0u),
#if defined(OPTILINK_HOST_SIM) && OPTILINK_HOST_SIM
                    (uint32_t)(hal_sim_tx_enabled(l) ? 1u : 0u),
#else
                    0u,
#endif
                    st->transitions, st->faults, st->retries,
                    link_fault_name(st->last_fault));
    }
    reply_ok();
}

static void cmd_stats(void)
{
    const eth_stats_t *e = eth_stats();
    const i2c_stats_t *i = i2c_target_stats();

    uart_printf("eth frames=%u bytes=%u fcs_err=%u undersize=%u oversize=%u "
                "truncated=%u vlan=%u qinq=%u uc=%u mc=%u bc=%u\n",
                e->frames, e->bytes, e->fcs_errors, e->undersize, e->oversize,
                e->truncated, e->vlan_tagged, e->qinq, e->unicast, e->multicast,
                e->broadcast);
    uart_printf("i2c txn=%u rd=%u wr=%u nack=%u\n", i->transactions, i->bytes_read,
                i->bytes_written, i->nacks);
    uart_printf("uart dropped=%u\n", uart_lines_dropped());

    for (uint8_t t = 0u; t < sched_task_count(); t++) {
        const sched_task_t *st = sched_task(t);

        uart_printf("task %s period=%u runs=%u overruns=%u max_latency=%u\n",
                    st->name, st->period_ms, st->runs, st->overruns,
                    st->max_latency_ms);
    }
    uart_printf("uptime_ms=%u\n", hal_millis());
    reply_ok();
}

static void cmd_i2cr(void)
{
    uint32_t off, len;

    if (!next_u32(&off) || !next_u32(&len)) {
        reply_err("usage: i2cr <offset> <len>");
        return;
    }
    if (off > 0xFFu || len == 0u || len > 256u) {
        reply_err("range");
        return;
    }

    /* Drive the real target state machine rather than reading cmis directly,
     * so the tests exercise the offset auto-increment and the read-to-clear
     * behaviour exactly as a host would hit them. */
    (void)i2c_target_start(OPTILINK_I2C_ADDR, false);
    (void)i2c_target_write((uint8_t)off);
    (void)i2c_target_start(OPTILINK_I2C_ADDR, true);
    for (uint32_t n = 0u; n < len; n++) {
        uart_printf("%02x", (uint32_t)i2c_target_read());
    }
    i2c_target_stop();
    uart_puts("\n");
    reply_ok();
}

static void cmd_i2cw(void)
{
    uint32_t off, v;
    uint32_t written = 0u;
    bool     nacked  = false;

    if (!next_u32(&off)) {
        reply_err("usage: i2cw <offset> <byte>...");
        return;
    }
    if (off > 0xFFu) {
        reply_err("range");
        return;
    }
    (void)i2c_target_start(OPTILINK_I2C_ADDR, false);
    (void)i2c_target_write((uint8_t)off);
    while (next_u32(&v)) {
        if (v > 0xFFu) {
            i2c_target_stop();
            reply_err("byte range");
            return;
        }
        if (!i2c_target_write((uint8_t)v)) {
            nacked = true;
        }
        written++;
    }
    i2c_target_stop();

    if (written == 0u) {
        reply_err("no data");
        return;
    }
    if (nacked) {
        reply_err("nack read-only");
        return;
    }
    reply_ok();
}

static void cmd_frame(void)
{
    const char  *hex;
    uint16_t     len = 0u;
    eth_frame_t  f;
    eth_result_t r;
    size_t       n = 0u;

    hex = tok_next_raw(&n);
    if (hex == NULL) {
        reply_err("usage: frame <hex>");
        return;
    }
    if ((n % 2u) != 0u) {
        reply_err("odd hex length");
        return;
    }
    if ((n / 2u) > CLI_FRAME_CAP) {
        reply_err("too long for the console buffer");
        return;
    }
    for (size_t i = 0u; i < n; i += 2u) {
        int hi = hex_nibble(hex[i]);
        int lo = hex_nibble(hex[i + 1u]);

        if (hi < 0 || lo < 0) {
            reply_err("bad hex");
            return;
        }
        s_frame[len++] = (uint8_t)((hi << 4) | lo);
    }

    r = eth_parse(s_frame, len, &f);
    uart_printf("result=%s len=%u\n", eth_result_name(r), (uint32_t)len);
    if (r != ETH_ERR_UNDERSIZE && r != ETH_ERR_OVERSIZE && r != ETH_ERR_TRUNCATED) {
        uart_printf("dst=%02x%02x%02x%02x%02x%02x src=%02x%02x%02x%02x%02x%02x\n",
                    (uint32_t)f.dst[0], (uint32_t)f.dst[1], (uint32_t)f.dst[2],
                    (uint32_t)f.dst[3], (uint32_t)f.dst[4], (uint32_t)f.dst[5],
                    (uint32_t)f.src[0], (uint32_t)f.src[1], (uint32_t)f.src[2],
                    (uint32_t)f.src[3], (uint32_t)f.src[4], (uint32_t)f.src[5]);
        uart_printf("ethertype=0x%04x vlan=%u qinq=%u outer_vid=%u outer_pcp=%u "
                    "inner_vid=%u payload_len=%u\n",
                    (uint32_t)f.ethertype, (uint32_t)(f.vlan_tagged ? 1u : 0u),
                    (uint32_t)(f.qinq ? 1u : 0u), (uint32_t)f.outer_vid,
                    (uint32_t)f.outer_pcp, (uint32_t)f.inner_vid,
                    (uint32_t)f.payload_len);
        uart_printf("fcs_frame=0x%08x fcs_calc=0x%08x\n", f.fcs_in_frame,
                    f.fcs_computed);
    }
    reply_ok();
}

static void cmd_lane(void)
{
    uint32_t lane;
    char     onoff[8];

    if (!next_u32(&lane) || !tok_next(onoff, sizeof onoff)) {
        reply_err("usage: lane <n> <on|off>");
        return;
    }
    if (lane >= OPTILINK_LANES) {
        reply_err("lane range");
        return;
    }
    if (strcmp(onoff, "on") == 0) {
        link_lane_enable((uint8_t)lane, true);
    } else if (strcmp(onoff, "off") == 0) {
        link_lane_enable((uint8_t)lane, false);
    } else {
        reply_err("expected on or off");
        return;
    }
    reply_ok();
}

#if defined(OPTILINK_HOST_SIM) && OPTILINK_HOST_SIM

static void cmd_inject(void)
{
    char     what[16];
    uint32_t a, b;

    if (!tok_next(what, sizeof what)) {
        reply_err("usage: inject <signal> [lane] <value>");
        return;
    }

    if (strcmp(what, "temp") == 0 || strcmp(what, "vcc") == 0) {
        if (!next_u32(&a)) {
            reply_err("need counts");
            return;
        }
        hal_sim_set_adc(strcmp(what, "temp") == 0 ? HAL_ADC_MODULE_TEMP
                                                  : HAL_ADC_MODULE_VCC,
                        (uint16_t)a);
        reply_ok();
        return;
    }

    if (strcmp(what, "present") == 0 || strcmp(what, "reset") == 0 ||
        strcmp(what, "lpmode") == 0) {
        if (!next_u32(&a)) {
            reply_err("need value");
            return;
        }
        hal_sim_set_gpio(strcmp(what, "present") == 0  ? HAL_PIN_MODULE_PRESENT
                         : strcmp(what, "reset") == 0  ? HAL_PIN_RESET_N
                                                       : HAL_PIN_LPMODE,
                         a != 0u);
        reply_ok();
        return;
    }

    /* Everything below is per lane. */
    if (!next_u32(&a) || a >= OPTILINK_LANES || !next_u32(&b)) {
        reply_err("usage: inject <signal> <lane> <value>");
        return;
    }

    if (strcmp(what, "bias") == 0) {
        hal_sim_set_adc((hal_adc_ch_t)(HAL_ADC_TX_BIAS_0 + a), (uint16_t)b);
    } else if (strcmp(what, "txpower") == 0) {
        hal_sim_set_adc((hal_adc_ch_t)(HAL_ADC_TX_POWER_0 + a), (uint16_t)b);
    } else if (strcmp(what, "rxpower") == 0) {
        hal_sim_set_adc((hal_adc_ch_t)(HAL_ADC_RX_POWER_0 + a), (uint16_t)b);
    } else if (strcmp(what, "los") == 0) {
        hal_sim_set_los((uint8_t)a, b != 0u);
    } else if (strcmp(what, "txfault") == 0) {
        hal_sim_set_tx_fault((uint8_t)a, b != 0u);
    } else if (strcmp(what, "lol") == 0) {
        hal_sim_set_cdr_lol((uint8_t)a, b != 0u);
    } else {
        reply_err("unknown signal");
        return;
    }
    reply_ok();
}

static void cmd_tick(void)
{
    uint32_t n;

    if (!next_u32(&n)) {
        reply_err("usage: tick <n>");
        return;
    }
    if (n > 1000000u) {
        reply_err("too many");
        return;
    }
    /* Reply before the ticks run. The harness sees "ok", then blocks on the
     * next command until the burst finishes, which is what makes time
     * advance atomically from the test's point of view. */
    reply_ok();
    hal_sim_advance_ticks(n);
}

#endif /* OPTILINK_HOST_SIM */

static void cmd_help(void)
{
    uart_puts("id mon flags link stats i2cr i2cw frame lane help\n");
#if defined(OPTILINK_HOST_SIM) && OPTILINK_HOST_SIM
    uart_puts("inject tick\n");
#endif
    reply_ok();
}

/* --- dispatch ------------------------------------------------------------ */

static void dispatch(const char *line)
{
    char cmd[16];

    tok_begin(line);
    if (!tok_next(cmd, sizeof cmd)) {
        return; /* a bare newline is not an error, and gets no reply */
    }

    if (strcmp(cmd, "id") == 0)          { cmd_id(); }
    else if (strcmp(cmd, "mon") == 0)    { cmd_mon(); }
    else if (strcmp(cmd, "flags") == 0)  { cmd_flags(); }
    else if (strcmp(cmd, "link") == 0)   { cmd_link(); }
    else if (strcmp(cmd, "stats") == 0)  { cmd_stats(); }
    else if (strcmp(cmd, "i2cr") == 0)   { cmd_i2cr(); }
    else if (strcmp(cmd, "i2cw") == 0)   { cmd_i2cw(); }
    else if (strcmp(cmd, "frame") == 0)  { cmd_frame(); }
    else if (strcmp(cmd, "lane") == 0)   { cmd_lane(); }
    else if (strcmp(cmd, "help") == 0)   { cmd_help(); }
#if defined(OPTILINK_HOST_SIM) && OPTILINK_HOST_SIM
    else if (strcmp(cmd, "inject") == 0) { cmd_inject(); }
    else if (strcmp(cmd, "tick") == 0)   { cmd_tick(); }
#endif
    else                                 { reply_err("unknown command"); }
}

void cli_init(void)
{
    /* Nothing to hold: the line buffer belongs to the UART driver and the
     * frame scratch is static. */
}

void cli_task(void *ctx)
{
    (void)ctx;
    uart_poll();
    if (uart_line_ready()) {
        dispatch(uart_line());
        uart_consume_line();
    }
}
