/*
 * cortexm_irq.h - interrupt handler names shared between the vector table
 * and the port that implements them.
 *
 * The table in startup_cortexm.c needs the addresses and hal_cortexm.c
 * defines the bodies, so the prototypes live here rather than being declared
 * twice and silently drifting apart.
 */
#ifndef OPTILINK_CORTEXM_IRQ_H
#define OPTILINK_CORTEXM_IRQ_H

void Reset_Handler(void);
void Default_Handler(void);
void NMI_Handler(void);
void HardFault_Handler(void);
void SysTick_Handler(void);
void USART2_IRQHandler(void);
void I2C1_EV_IRQHandler(void);
void I2C1_ER_IRQHandler(void);

#endif /* OPTILINK_CORTEXM_IRQ_H */
