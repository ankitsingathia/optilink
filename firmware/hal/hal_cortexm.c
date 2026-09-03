/*
 * hal_cortexm.c - the target port.
 *
 * A Cortex-M4 with STM32F4 style peripherals: SysTick for the millisecond
 * base, USART2 for the debug console, ADC1 for the analog monitors, GPIOA
 * and GPIOB for the module and lane discretes, and I2C1 as the target the
 * host talks management over.
 *
 * The register maps are declared here rather than pulled from a vendor
 * headline package so the build has no external dependency: one compiler and
 * a linker script is the entire toolchain requirement.
 *
 * Two things run in interrupt context and nothing else does. The UART RX
 * interrupt pushes bytes into a lock free ring that the console task drains,
 * and the I2C event interrupt drives the target state machine directly,
 * because a host clocking a burst read will not wait for a task to be
 * scheduled. Everything else is polled from the main loop.
 */
#include "optilink/hal.h"

#if defined(OPTILINK_HOST_SIM) && OPTILINK_HOST_SIM
#error "hal_cortexm.c must be built with -DOPTILINK_HOST_SIM=0"
#endif

#include "cortexm_irq.h"
#include "optilink/i2c.h"
#include "optilink/ringbuf.h"

/* --- register maps ------------------------------------------------------- */

typedef struct {
    volatile uint32_t MODER, OTYPER, OSPEEDR, PUPDR, IDR, ODR, BSRR, LCKR, AFR[2];
} gpio_regs_t;

typedef struct {
    volatile uint32_t SR, DR, BRR, CR1, CR2, CR3, GTPR;
} usart_regs_t;

typedef struct {
    volatile uint32_t SR, CR1, CR2, SMPR1, SMPR2, JOFR[4], HTR, LTR;
    volatile uint32_t SQR1, SQR2, SQR3, JSQR, JDR[4], DR;
} adc_regs_t;

typedef struct {
    volatile uint32_t CR1, CR2, OAR1, OAR2, DR, SR1, SR2, CCR, TRISE, FLTR;
} i2c_regs_t;

typedef struct {
    volatile uint32_t CR, PLLCFGR, CFGR, CIR, AHB1RSTR, AHB2RSTR, AHB3RSTR;
    volatile uint32_t RESERVED0, APB1RSTR, APB2RSTR, RESERVED1[2];
    volatile uint32_t AHB1ENR, AHB2ENR, AHB3ENR, RESERVED2, APB1ENR, APB2ENR;
} rcc_regs_t;

typedef struct {
    volatile uint32_t CTRL, LOAD, VAL, CALIB;
} systick_regs_t;

#define RCC     ((rcc_regs_t *)0x40023800u)
#define GPIOA   ((gpio_regs_t *)0x40020000u)
#define GPIOB   ((gpio_regs_t *)0x40020400u)
#define USART2  ((usart_regs_t *)0x40004400u)
#define ADC1    ((adc_regs_t *)0x40012000u)
#define I2C1    ((i2c_regs_t *)0x40005400u)
#define SYSTICK ((systick_regs_t *)0xE000E010u)
#define NVIC_ISER ((volatile uint32_t *)0xE000E100u)

/* RCC enable bits. */
#define RCC_AHB1ENR_GPIOA (1u << 0)
#define RCC_AHB1ENR_GPIOB (1u << 1)
#define RCC_APB1ENR_USART2 (1u << 17)
#define RCC_APB1ENR_I2C1  (1u << 21)
#define RCC_APB2ENR_ADC1  (1u << 8)

/* USART bits. */
#define USART_SR_TXE  (1u << 7)
#define USART_SR_RXNE (1u << 5)
#define USART_CR1_RE     (1u << 2)
#define USART_CR1_TE     (1u << 3)
#define USART_CR1_RXNEIE (1u << 5)
#define USART_CR1_UE     (1u << 13)

/* ADC bits. */
#define ADC_SR_EOC   (1u << 1)
#define ADC_CR2_ADON (1u << 0)
#define ADC_CR2_SWSTART (1u << 30)

/* I2C bits. */
#define I2C_CR1_PE   (1u << 0)
#define I2C_CR1_ACK  (1u << 10)
#define I2C_CR2_ITERREN (1u << 8)
#define I2C_CR2_ITEVTEN (1u << 9)
#define I2C_CR2_ITBUFEN (1u << 10)
#define I2C_SR1_ADDR (1u << 1)
#define I2C_SR1_STOPF (1u << 4)
#define I2C_SR1_RXNE (1u << 6)
#define I2C_SR1_TXE  (1u << 7)
#define I2C_SR2_TRA  (1u << 2)

#define IRQ_USART2  38u
#define IRQ_I2C1_EV 31u
#define IRQ_I2C1_ER 32u

/* The part runs from a 16 MHz HSI with no PLL. Slow, but this controller
 * spends its life waiting on a 1 ms tick and a 100 kHz management bus, and
 * the lower clock keeps the module's own power budget down. */
#define SYSCLK_HZ 16000000u
#define PCLK1_HZ  16000000u

/* --- state --------------------------------------------------------------- */

static volatile uint32_t s_ticks;
static ringbuf_t         s_rx;
static uint8_t           s_rx_store[256];

/* Module discretes on GPIOA, lane discretes on GPIOB: three inputs and one
 * transmit enable per lane, packed four lanes to a port. */
static const uint8_t PIN_MAP[HAL_PIN_COUNT] = { 0u, 1u, 2u, 3u };
#define LANE_LOS_PIN(l)      (uint8_t)(0u + (l))
#define LANE_TXFAULT_PIN(l)  (uint8_t)(4u + (l))
#define LANE_LOL_PIN(l)      (uint8_t)(8u + (l))
#define LANE_TXEN_PIN(l)     (uint8_t)(12u + (l))

/* --- clock --------------------------------------------------------------- */

void SysTick_Handler(void)
{
    s_ticks++;
}

uint32_t hal_millis(void)
{
    return s_ticks;
}

void hal_wait_for_tick(void)
{
    uint32_t entry = s_ticks;

    /* Sleep rather than spin. The core wakes on any interrupt, so the loop
     * re-checks: a UART byte arriving mid-tick must not be mistaken for the
     * tick itself. */
    while (s_ticks == entry) {
        __asm volatile("wfi");
    }
}

/* --- gpio ---------------------------------------------------------------- */

bool hal_gpio_read(hal_pin_t pin)
{
    if (pin >= HAL_PIN_COUNT) {
        return false;
    }
    return (GPIOA->IDR & (1u << PIN_MAP[pin])) != 0u;
}

void hal_gpio_write(hal_pin_t pin, bool level)
{
    if (pin >= HAL_PIN_COUNT) {
        return;
    }
    /* BSRR is write only and atomic: set in the low half, reset in the high
     * half. Using it avoids the read-modify-write race against an interrupt
     * that touches another pin on the same port. */
    GPIOA->BSRR = level ? (1u << PIN_MAP[pin])
                        : (1u << (PIN_MAP[pin] + 16u));
}

bool hal_lane_pins_read(uint8_t lane, hal_lane_pins_t *out)
{
    uint32_t idr;

    if (lane >= OPTILINK_LANES || out == NULL) {
        return false;
    }
    /* One read for all three signals, so the debouncer sees a coherent
     * sample rather than three taken microseconds apart. */
    idr           = GPIOB->IDR;
    out->rx_los   = (idr & (1u << LANE_LOS_PIN(lane))) != 0u;
    out->tx_fault = (idr & (1u << LANE_TXFAULT_PIN(lane))) != 0u;
    out->cdr_lol  = (idr & (1u << LANE_LOL_PIN(lane))) != 0u;
    return true;
}

void hal_tx_enable(uint8_t lane, bool on)
{
    if (lane >= OPTILINK_LANES) {
        return;
    }
    GPIOB->BSRR = on ? (1u << LANE_TXEN_PIN(lane))
                     : (1u << (LANE_TXEN_PIN(lane) + 16u));
}

/* --- adc ----------------------------------------------------------------- */

uint16_t hal_adc_read(hal_adc_ch_t ch)
{
    uint32_t timeout = 10000u;

    if (ch >= HAL_ADC_COUNT) {
        return 0u;
    }
    ADC1->SQR3 = (uint32_t)ch;   /* single conversion, this channel */
    ADC1->CR2 |= ADC_CR2_SWSTART;

    /* Bounded wait. A conversion that never completes must not hang the
     * whole module: returning zero trips the low alarm, which is the safe
     * reading to report for a monitor we cannot actually see. */
    while ((ADC1->SR & ADC_SR_EOC) == 0u) {
        if (--timeout == 0u) {
            return 0u;
        }
    }
    return (uint16_t)(ADC1->DR & 0x0FFFu);
}

/* --- uart ---------------------------------------------------------------- */

void USART2_IRQHandler(void)
{
    while ((USART2->SR & USART_SR_RXNE) != 0u) {
        /* Reading DR clears RXNE. A full ring drops the byte and bumps a
         * counter rather than blocking here, because blocking in an ISR to
         * wait on a task is how a console stalls a link. */
        (void)ringbuf_push(&s_rx, (uint8_t)(USART2->DR & 0xFFu));
    }
}

void hal_uart_init(uint32_t baud)
{
    (void)ringbuf_init(&s_rx, s_rx_store, (uint16_t)sizeof s_rx_store);

    /* PA2 and PA3 to alternate function 7. */
    GPIOA->MODER &= ~((3u << (2u * 2u)) | (3u << (3u * 2u)));
    GPIOA->MODER |= (2u << (2u * 2u)) | (2u << (3u * 2u));
    GPIOA->AFR[0] &= ~((0xFu << (2u * 4u)) | (0xFu << (3u * 4u)));
    GPIOA->AFR[0] |= (7u << (2u * 4u)) | (7u << (3u * 4u));

    USART2->BRR = PCLK1_HZ / baud;
    USART2->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;
    NVIC_ISER[IRQ_USART2 / 32u] = 1u << (IRQ_USART2 % 32u);
}

void hal_uart_write(const uint8_t *buf, size_t len)
{
    for (size_t i = 0u; i < len; i++) {
        while ((USART2->SR & USART_SR_TXE) == 0u) {
            /* The console is a diagnostic path and blocking here costs at
             * most a character time at 115200, which is inside the 1 ms
             * budget of the task that calls it. */
        }
        USART2->DR = buf[i];
    }
}

int hal_uart_read_byte(void)
{
    return ringbuf_pop(&s_rx);
}

/* --- i2c target ---------------------------------------------------------- */

void I2C1_EV_IRQHandler(void)
{
    uint32_t sr1 = I2C1->SR1;

    if ((sr1 & I2C_SR1_ADDR) != 0u) {
        uint32_t sr2 = I2C1->SR2; /* reading SR1 then SR2 clears ADDR */

        (void)i2c_target_start(OPTILINK_I2C_ADDR, (sr2 & I2C_SR2_TRA) != 0u);
        return;
    }
    if ((sr1 & I2C_SR1_RXNE) != 0u) {
        if (!i2c_target_write((uint8_t)(I2C1->DR & 0xFFu))) {
            /* Refuse the byte by dropping ACK for it, which is how a target
             * tells the host that offset was not writable. */
            I2C1->CR1 &= ~I2C_CR1_ACK;
            I2C1->CR1 |= I2C_CR1_ACK;
        }
        return;
    }
    if ((sr1 & I2C_SR1_TXE) != 0u) {
        I2C1->DR = i2c_target_read();
        return;
    }
    if ((sr1 & I2C_SR1_STOPF) != 0u) {
        I2C1->CR1 |= I2C_CR1_PE; /* writing CR1 after reading SR1 clears STOPF */
        i2c_target_stop();
    }
}

void I2C1_ER_IRQHandler(void)
{
    /* Clear every error flag and let the host retry. A management bus glitch
     * must not leave the target wedged and the module unreadable. */
    I2C1->SR1 = 0u;
    i2c_target_stop();
}

static void i2c_init(void)
{
    /* PB6 and PB7 to alternate function 4, open drain. */
    GPIOB->MODER &= ~((3u << (6u * 2u)) | (3u << (7u * 2u)));
    GPIOB->MODER |= (2u << (6u * 2u)) | (2u << (7u * 2u));
    GPIOB->OTYPER |= (1u << 6u) | (1u << 7u);
    GPIOB->AFR[0] &= ~((0xFu << (6u * 4u)) | (0xFu << (7u * 4u)));
    GPIOB->AFR[0] |= (4u << (6u * 4u)) | (4u << (7u * 4u));

    I2C1->CR1 = 0u;
    I2C1->CR2 = (PCLK1_HZ / 1000000u) | I2C_CR2_ITEVTEN | I2C_CR2_ITBUFEN |
                I2C_CR2_ITERREN;
    /* Address is shifted up one: bit 0 of the byte on the wire is the
     * direction, not part of the address. */
    I2C1->OAR1 = (uint32_t)(OPTILINK_I2C_ADDR << 1);
    I2C1->CCR  = PCLK1_HZ / (2u * 100000u); /* 100 kHz standard mode */
    I2C1->TRISE = (PCLK1_HZ / 1000000u) + 1u;
    I2C1->CR1  = I2C_CR1_PE | I2C_CR1_ACK;

    NVIC_ISER[IRQ_I2C1_EV / 32u] = 1u << (IRQ_I2C1_EV % 32u);
    NVIC_ISER[IRQ_I2C1_ER / 32u] = 1u << (IRQ_I2C1_ER % 32u);
}

/* --- bring-up ------------------------------------------------------------ */

void hal_init(void)
{
    s_ticks = 0u;

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOA | RCC_AHB1ENR_GPIOB;
    RCC->APB1ENR |= RCC_APB1ENR_USART2 | RCC_APB1ENR_I2C1;
    RCC->APB2ENR |= RCC_APB2ENR_ADC1;

    /* Lane transmit enables are outputs; everything else on GPIOB stays an
     * input. MODER is two bits per pin, 01 for output. */
    for (uint8_t l = 0u; l < OPTILINK_LANES; l++) {
        uint32_t shift = (uint32_t)LANE_TXEN_PIN(l) * 2u;

        GPIOB->MODER &= ~(3u << shift);
        GPIOB->MODER |= (1u << shift);
    }
    /* Lasers off until the data path machine says otherwise. */
    GPIOB->BSRR = 0x0000F000u << 16;

    /* INT_N is an output on GPIOA; the other module pins are inputs. */
    GPIOA->MODER &= ~(3u << ((uint32_t)PIN_MAP[HAL_PIN_INT_N] * 2u));
    GPIOA->MODER |= (1u << ((uint32_t)PIN_MAP[HAL_PIN_INT_N] * 2u));

    ADC1->CR2 = ADC_CR2_ADON;

    i2c_init();

    /* SysTick at 1 kHz: reload is one tick's worth of core clocks minus one,
     * because the counter reloads on the transition through zero. */
    SYSTICK->LOAD = (SYSCLK_HZ / 1000u) - 1u;
    SYSTICK->VAL  = 0u;
    SYSTICK->CTRL = 0x07u; /* core clock, interrupt enabled, running */
}
