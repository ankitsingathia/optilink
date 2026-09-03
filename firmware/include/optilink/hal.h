/*
 * hal.h - the seam between OptiLink and whatever it is running on.
 *
 * Two ports implement this header: hal_cortexm.c drives real peripherals on
 * an ARM Cortex-M controller, and hal_host.c fakes them in a desktop process
 * so the identical firmware sources can be run under a test harness with no
 * hardware attached. Nothing above this header knows which one it linked.
 */
#ifndef OPTILINK_HAL_H
#define OPTILINK_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "optilink/optilink.h"

/* ---------------------------------------------------------------- clock -- */

void     hal_init(void);
uint32_t hal_millis(void);

/* Blocks until the next scheduler tick boundary. On the Cortex-M port this
 * is WFI woken by SysTick; on the host port it is driven by the harness so
 * tests advance time explicitly instead of sleeping. */
void hal_wait_for_tick(void);

/* ----------------------------------------------------------------- gpio -- */

typedef enum {
    HAL_PIN_MODULE_PRESENT = 0, /* ModPrsL, active low on the connector */
    HAL_PIN_RESET_N,            /* ResetL from the host */
    HAL_PIN_LPMODE,             /* host requests low power */
    HAL_PIN_INT_N,              /* module asserts to interrupt the host */
    HAL_PIN_COUNT
} hal_pin_t;

bool hal_gpio_read(hal_pin_t pin);
void hal_gpio_write(hal_pin_t pin, bool level);

/* Per-lane digital status straight off the optical front end. */
typedef struct {
    bool rx_los;   /* receiver lost the optical signal */
    bool tx_fault; /* laser driver latched a fault */
    bool cdr_lol;  /* clock recovery lost lock */
} hal_lane_pins_t;

bool hal_lane_pins_read(uint8_t lane, hal_lane_pins_t *out);

/* ------------------------------------------------------------------ adc -- */

/* Raw analog monitor channels. Values are unscaled ADC counts; converting
 * them into the fixed-point units CMIS mandates is ddm.c's job, not the
 * port's. */
typedef enum {
    HAL_ADC_MODULE_TEMP = 0,
    HAL_ADC_MODULE_VCC,
    HAL_ADC_TX_BIAS_0,
    HAL_ADC_TX_POWER_0 = HAL_ADC_TX_BIAS_0 + OPTILINK_LANES,
    HAL_ADC_RX_POWER_0 = HAL_ADC_TX_POWER_0 + OPTILINK_LANES,
    HAL_ADC_COUNT      = HAL_ADC_RX_POWER_0 + OPTILINK_LANES
} hal_adc_ch_t;

uint16_t hal_adc_read(hal_adc_ch_t ch);

/* ----------------------------------------------------------------- uart -- */

void hal_uart_init(uint32_t baud);
void hal_uart_write(const uint8_t *buf, size_t len);

/* Non-blocking single byte pop from the driver's RX ring. Returns -1 when
 * the ring is empty. */
int hal_uart_read_byte(void);

/* --------------------------------------------------------------- optics -- */

/* Transmitter enable per lane. Squelching TX is how the data path state
 * machine keeps a faulted lane dark. */
void hal_tx_enable(uint8_t lane, bool on);

#endif /* OPTILINK_HAL_H */
