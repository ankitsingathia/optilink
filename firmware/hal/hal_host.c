/*
 * hal_host.c - the desktop port.
 *
 * Presents the same peripherals as the Cortex-M port, backed by ordinary
 * memory and a stdio pipe instead of silicon. The firmware sources above the
 * HAL are compiled unmodified, so what the tests exercise is the shipping
 * logic and not a reimplementation of it.
 *
 * The clock is virtual. Time only moves when the harness says so, which is
 * what turns "wait 200 ms and hope the lane came up" into an exact
 * assertion.
 */
#include "optilink/hal.h"

#if !defined(OPTILINK_HOST_SIM) || !OPTILINK_HOST_SIM
#error "hal_host.c requires -DOPTILINK_HOST_SIM=1"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal_sim.h"
#include "optilink/ringbuf.h"

/* Counts that land every monitor comfortably inside its warning band, so a
 * module with nothing injected into it comes up clean. Derived from the
 * calibration in ddm.c: roughly 40 C, 3.30 V, 35 mA bias, 1.0 mW out and
 * 0.8 mW in. */
#define DEFAULT_TEMP_COUNTS     1985u
#define DEFAULT_VCC_COUNTS      3754u
#define DEFAULT_BIAS_COUNTS     1093u
#define DEFAULT_TX_POWER_COUNTS 625u
#define DEFAULT_RX_POWER_COUNTS 500u

static uint32_t s_millis;
static uint32_t s_pending_ticks;

static bool     s_gpio[HAL_PIN_COUNT];
static uint16_t s_adc[HAL_ADC_COUNT];
static bool     s_los[OPTILINK_LANES];
static bool     s_txf[OPTILINK_LANES];
static bool     s_lol[OPTILINK_LANES];
static bool     s_tx_on[OPTILINK_LANES];

static ringbuf_t s_rx;
static uint8_t   s_rx_store[4096];

void hal_init(void)
{
    s_millis        = 0u;
    s_pending_ticks = 0u;

    memset(s_gpio, 0, sizeof s_gpio);
    /* A module that is plugged in, out of reset, and not being held in low
     * power. Note ModPrsL and ResetL are active low on the connector; the
     * HAL presents them already normalised. */
    s_gpio[HAL_PIN_MODULE_PRESENT] = true;
    s_gpio[HAL_PIN_RESET_N]        = true;
    s_gpio[HAL_PIN_LPMODE]         = false;
    s_gpio[HAL_PIN_INT_N]          = true;

    s_adc[HAL_ADC_MODULE_TEMP] = DEFAULT_TEMP_COUNTS;
    s_adc[HAL_ADC_MODULE_VCC]  = DEFAULT_VCC_COUNTS;
    for (uint8_t l = 0u; l < OPTILINK_LANES; l++) {
        s_adc[HAL_ADC_TX_BIAS_0 + l]  = DEFAULT_BIAS_COUNTS;
        s_adc[HAL_ADC_TX_POWER_0 + l] = DEFAULT_TX_POWER_COUNTS;
        s_adc[HAL_ADC_RX_POWER_0 + l] = DEFAULT_RX_POWER_COUNTS;
        s_los[l]                      = false;
        s_txf[l]                      = false;
        s_lol[l]                      = false;
        s_tx_on[l]                    = false;
    }

    (void)ringbuf_init(&s_rx, s_rx_store, (uint16_t)sizeof s_rx_store);
}

uint32_t hal_millis(void)
{
    return s_millis;
}

void hal_wait_for_tick(void)
{
    int c = 0;

    if (s_pending_ticks > 0u) {
        s_pending_ticks--;
        s_millis += OPTILINK_TICK_MS;
        return;
    }

    /* Block until a whole console line has arrived, then hand it to the UART
     * driver in one go. Reading a line at a time rather than a byte at a
     * time means a command never straddles a tick boundary, which keeps
     * test timing reproducible. */
    while ((c = fgetc(stdin)) != EOF) {
        (void)ringbuf_push(&s_rx, (uint8_t)c);
        if (c == '\n') {
            break;
        }
    }
    if (c == EOF) {
        fflush(stdout);
        exit(0);
    }
    s_millis += OPTILINK_TICK_MS;
}

bool hal_gpio_read(hal_pin_t pin)
{
    return (pin < HAL_PIN_COUNT) ? s_gpio[pin] : false;
}

void hal_gpio_write(hal_pin_t pin, bool level)
{
    if (pin < HAL_PIN_COUNT) {
        s_gpio[pin] = level;
    }
}

bool hal_lane_pins_read(uint8_t lane, hal_lane_pins_t *out)
{
    if (lane >= OPTILINK_LANES || out == NULL) {
        return false;
    }
    out->rx_los   = s_los[lane];
    out->tx_fault = s_txf[lane];
    out->cdr_lol  = s_lol[lane];
    return true;
}

uint16_t hal_adc_read(hal_adc_ch_t ch)
{
    return (ch < HAL_ADC_COUNT) ? s_adc[ch] : 0u;
}

void hal_uart_init(uint32_t baud)
{
    (void)baud; /* a pipe has no line rate */
}

void hal_uart_write(const uint8_t *buf, size_t len)
{
    bool newline = false;

    for (size_t i = 0u; i < len; i++) {
        (void)fputc((int)buf[i], stdout);
        if (buf[i] == (uint8_t)'\n') {
            newline = true;
        }
    }
    /* Flushing per byte would be correct and unusably slow, since the
     * formatter emits one character at a time. Flushing per line is enough
     * for the harness, which never reads a partial line. */
    if (newline) {
        (void)fflush(stdout);
    }
}

int hal_uart_read_byte(void)
{
    return ringbuf_pop(&s_rx);
}

void hal_tx_enable(uint8_t lane, bool on)
{
    if (lane < OPTILINK_LANES) {
        s_tx_on[lane] = on;
    }
}

/* --- injection ----------------------------------------------------------- */

void hal_sim_set_adc(hal_adc_ch_t ch, uint16_t counts)
{
    if (ch < HAL_ADC_COUNT) {
        s_adc[ch] = counts;
    }
}

void hal_sim_set_gpio(hal_pin_t pin, bool level)
{
    if (pin < HAL_PIN_COUNT) {
        s_gpio[pin] = level;
    }
}

void hal_sim_set_los(uint8_t lane, bool v)
{
    if (lane < OPTILINK_LANES) {
        s_los[lane] = v;
    }
}

void hal_sim_set_tx_fault(uint8_t lane, bool v)
{
    if (lane < OPTILINK_LANES) {
        s_txf[lane] = v;
    }
}

void hal_sim_set_cdr_lol(uint8_t lane, bool v)
{
    if (lane < OPTILINK_LANES) {
        s_lol[lane] = v;
    }
}

bool hal_sim_tx_enabled(uint8_t lane)
{
    return (lane < OPTILINK_LANES) ? s_tx_on[lane] : false;
}

void hal_sim_advance_ticks(uint32_t n)
{
    s_pending_ticks = n;
}
