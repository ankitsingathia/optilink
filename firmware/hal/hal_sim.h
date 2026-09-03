/*
 * hal_sim.h - injection hooks, host port only.
 *
 * Everything here is behind OPTILINK_HOST_SIM and is not compiled into the
 * target image. That is deliberate: a build that can be told to fake a laser
 * fault is not a build that should ever be able to ship, and keeping the
 * seam at compile time rather than behind a runtime flag makes that
 * guarantee something the linker enforces instead of something a reviewer
 * has to notice.
 */
#ifndef OPTILINK_HAL_SIM_H
#define OPTILINK_HAL_SIM_H

#if defined(OPTILINK_HOST_SIM) && OPTILINK_HOST_SIM

#include <stdbool.h>
#include <stdint.h>

#include "optilink/hal.h"

void hal_sim_set_adc(hal_adc_ch_t ch, uint16_t counts);
void hal_sim_set_gpio(hal_pin_t pin, bool level);
void hal_sim_set_los(uint8_t lane, bool v);
void hal_sim_set_tx_fault(uint8_t lane, bool v);
void hal_sim_set_cdr_lol(uint8_t lane, bool v);

/* True while the data path has the laser on for that lane. The tests assert
 * on this to prove a fault actually squelched the transmitter. */
bool hal_sim_tx_enabled(uint8_t lane);

/* Runs n scheduler ticks without reading the console, so a test can advance
 * time by an exact amount instead of sleeping and hoping. */
void hal_sim_advance_ticks(uint32_t n);

#endif /* OPTILINK_HOST_SIM */
#endif /* OPTILINK_HAL_SIM_H */
