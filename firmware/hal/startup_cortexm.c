/*
 * startup_cortexm.c - vector table and reset path.
 *
 * Written in C rather than assembly because on Cortex-M there is nothing
 * here that needs assembly: the core loads the stack pointer from the first
 * vector entry itself, so by the time Reset_Handler runs there is already a
 * usable stack and ordinary C is safe.
 */
#include <stdint.h>

#include "cortexm_irq.h"

/* Provided by the linker script. Their addresses are the values, which is
 * why they are declared as arrays rather than as scalars. */
extern uint32_t _sidata[]; /* .data initialisers, in flash */
extern uint32_t _sdata[];
extern uint32_t _edata[];
extern uint32_t _sbss[];
extern uint32_t _ebss[];
extern uint32_t _estack[];

int main(void);

void Reset_Handler(void)
{
    uint32_t *src = _sidata;
    uint32_t *dst = _sdata;

    /* Copy initialised statics out of flash, then zero the rest. Until both
     * are done no global is trustworthy, so nothing above may be called. */
    while (dst < _edata) {
        *dst++ = *src++;
    }
    for (dst = _sbss; dst < _ebss; dst++) {
        *dst = 0u;
    }

    (void)main();

    /* main never returns. If it somehow does, stop here rather than running
     * off the end of the vector table. */
    for (;;) {
    }
}

void Default_Handler(void)
{
    /* Spin so a debugger can see which vector fired. Silently returning
     * would turn a wiring mistake into an intermittent field fault. */
    for (;;) {
    }
}

void NMI_Handler(void)
{
    for (;;) {
    }
}

void HardFault_Handler(void)
{
    for (;;) {
    }
}

typedef void (*vector_t)(void);

/* The first entry is the initial stack pointer, not a function. The union of
 * the two is what the hardware expects at address zero of the image. */
__attribute__((section(".isr_vector"), used)) const vector_t g_vectors[] = {
    (vector_t)_estack,
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    Default_Handler, /* MemManage */
    Default_Handler, /* BusFault */
    Default_Handler, /* UsageFault */
    0, 0, 0, 0,      /* reserved */
    Default_Handler, /* SVCall */
    Default_Handler, /* DebugMonitor */
    0,               /* reserved */
    Default_Handler, /* PendSV */
    SysTick_Handler,

    /* External interrupts 0..40. Only the three this port uses are named. */
    [16 + 31] = I2C1_EV_IRQHandler,
    [16 + 32] = I2C1_ER_IRQHandler,
    [16 + 38] = USART2_IRQHandler,
    [16 + 40] = Default_Handler,
};
