/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Harness check for the Level 2 wiring (WIRING-L2.md), built with CMake. Not
 * an ECU: it only shows that each wire between the Tang Nano 20K and this
 * board goes where the map says.
 *
 * - Crank (PA0, TIM2_CH1) and cam (PA1, TIM2_CH2): every rising edge is
 *   captured. A tooth interval over 1.5 times the one before is the 60-2
 *   wheel's gap, once per revolution; the time between two gaps gives the
 *   speed.
 * - Injectors 1-6 (PA8, PA9, PA10, PB4, PB5, PB6): every 100 ms, one pulse on
 *   each line in turn, injector k high for k x 100 us. The bridge captures
 *   their widths, so a crossed wire shows as the wrong width on a channel and
 *   an open one as no pulse.
 * - Once a second, over USART2 (the ST-LINK's virtual COM port, 115200 8N1):
 *     wirecheck s=<n> crank=<edges> cam=<edges> gaps=<n> rpm=<r> ovr=<n> bursts=<n>
 *   counts over the last second; ovr counts capture overruns (an edge lost).
 *
 * Registers through CMSIS only: no HAL module beyond board_init()'s.
 */
#include <stdint.h>

#include "board.h"

#define CLK_HZ 170000000u
#define UART_BAUD 115200u
#define BURST_PERIOD_MS 100u
#define PULSE_UNIT_US 100u
#define PULSE_GAP_US 100u

typedef struct {
    GPIO_TypeDef *port;
    uint32_t pin;
} Pin;

/* Injectors 1-6, in WIRING-L2.md's order. */
static const Pin inj[6] = {
    { GPIOA, 8u }, { GPIOA, 9u }, { GPIOA, 10u }, { GPIOB, 4u }, { GPIOB, 5u }, { GPIOB, 6u },
};

static volatile uint32_t crank_edges, cam_edges, gaps, overruns;
static volatile uint32_t rev_ticks; /* TIM2 ticks between the last two gaps */
static uint32_t last_tooth, last_dt, last_gap;
static uint32_t have_tooth, have_gap;

void TIM2_IRQHandler(void) {
    const uint32_t sr = TIM2->SR;
    if ((sr & (TIM_SR_CC1OF | TIM_SR_CC2OF)) != 0u) {
        overruns++;
    }
    if ((sr & TIM_SR_CC1IF) != 0u) {
        const uint32_t t = TIM2->CCR1; /* reading clears CC1IF */
        crank_edges++;
        if (have_tooth != 0u) {
            const uint32_t dt = t - last_tooth;
            if (last_dt != 0u && dt > last_dt + last_dt / 2u) {
                gaps++;
                if (have_gap != 0u) {
                    rev_ticks = t - last_gap;
                }
                last_gap = t;
                have_gap = 1u;
            }
            last_dt = dt;
        }
        last_tooth = t;
        have_tooth = 1u;
    }
    if ((sr & TIM_SR_CC2IF) != 0u) {
        (void)TIM2->CCR2;
        cam_edges++;
    }
    TIM2->SR = ~(TIM_SR_CC1OF | TIM_SR_CC2OF);
}

static void pin_mode(GPIO_TypeDef *port, uint32_t pin, uint32_t mode, uint32_t af, uint32_t pull) {
    port->MODER = (port->MODER & ~(3u << (pin * 2u))) | (mode << (pin * 2u));
    port->PUPDR = (port->PUPDR & ~(3u << (pin * 2u))) | (pull << (pin * 2u));
    port->OSPEEDR &= ~(3u << (pin * 2u)); /* low speed */
    port->OTYPER &= ~(1u << pin);         /* push-pull */
    volatile uint32_t *afr = &port->AFR[pin / 8u];
    *afr = (*afr & ~(0xFu << ((pin % 8u) * 4u))) | (af << ((pin % 8u) * 4u));
}

static void io_init(void) {
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN;
    RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN | RCC_APB1ENR1_USART2EN;
    (void)RCC->APB1ENR1;

    /* injectors: outputs, low */
    for (unsigned k = 0; k < 6u; k++) {
        inj[k].port->BRR = 1u << inj[k].pin;
        pin_mode(inj[k].port, inj[k].pin, 1u, 0u, 0u);
    }
    /* crank and cam: TIM2_CH1/CH2 (AF1), pull-down */
    pin_mode(GPIOA, 0u, 2u, 1u, 2u);
    pin_mode(GPIOA, 1u, 2u, 1u, 2u);
    /* USART2_TX on PA2 (AF7) */
    pin_mode(GPIOA, 2u, 2u, 7u, 0u);

    /* TIM2: free-running at 170 MHz, 32 bits; CH1/CH2 capture TI1/TI2 on
     * rising edges, filter fCK_INT with N = 8 (47 ns) */
    TIM2->PSC = 0u;
    TIM2->ARR = 0xFFFFFFFFu;
    TIM2->CCMR1 = (1u << TIM_CCMR1_CC1S_Pos) | (3u << TIM_CCMR1_IC1F_Pos) | (1u << TIM_CCMR1_CC2S_Pos) |
                  (3u << TIM_CCMR1_IC2F_Pos);
    TIM2->CCER = TIM_CCER_CC1E | TIM_CCER_CC2E;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->SR = 0u;
    TIM2->DIER = TIM_DIER_CC1IE | TIM_DIER_CC2IE;
    NVIC_SetPriority(TIM2_IRQn, 0u);
    NVIC_EnableIRQ(TIM2_IRQn);
    TIM2->CR1 = TIM_CR1_CEN;

    /* USART2: 115200 8N1, transmit only */
    USART2->BRR = (CLK_HZ + UART_BAUD / 2u) / UART_BAUD;
    USART2->CR1 = USART_CR1_TE | USART_CR1_UE;

    /* DWT cycle counter, for the pulse widths */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void uart_puts(const char *s) {
    while (*s != '\0') {
        while ((USART2->ISR & USART_ISR_TXE) == 0u) {
        }
        USART2->TDR = (uint8_t)*s++;
    }
}

static void uart_put_u32(uint32_t v) {
    char d[10];
    unsigned n = 0u;
    do {
        d[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0u);
    char s[11];
    for (unsigned i = 0u; i < n; i++) {
        s[i] = d[n - 1u - i];
    }
    s[n] = '\0';
    uart_puts(s);
}

static void uart_field(const char *name, uint32_t v) {
    uart_puts(name);
    uart_put_u32(v);
}

static void wait_cycles_from(uint32_t start, uint32_t cycles) {
    while (DWT->CYCCNT - start < cycles) {
    }
}

/* Injector k high for (k + 1) x 100 us, one after the other, 100 us apart. */
static void burst(void) {
    const uint32_t per_us = CLK_HZ / 1000000u;
    for (unsigned k = 0; k < 6u; k++) {
        const uint32_t t0 = DWT->CYCCNT;
        inj[k].port->BSRR = 1u << inj[k].pin;
        wait_cycles_from(t0, (k + 1u) * PULSE_UNIT_US * per_us);
        inj[k].port->BRR = 1u << inj[k].pin;
        wait_cycles_from(DWT->CYCCNT, PULSE_GAP_US * per_us);
    }
}

int main(void) {
    board_init(false);
    io_init();
    uart_puts("wirecheck: start\r\n");

    uint32_t next_burst = HAL_GetTick(), next_report = HAL_GetTick() + 1000u;
    uint32_t seconds = 0u, bursts = 0u;
    for (;;) {
        const uint32_t now = HAL_GetTick();
        if ((int32_t)(now - next_burst) >= 0) {
            burst();
            bursts++;
            next_burst += BURST_PERIOD_MS;
        }
        if ((int32_t)(now - next_report) >= 0) {
            next_report += 1000u;
            seconds++;
            __disable_irq();
            const uint32_t c = crank_edges, m = cam_edges, g = gaps, o = overruns, r = rev_ticks;
            crank_edges = cam_edges = gaps = overruns = 0u;
            __enable_irq();
            const uint32_t rpm = (g != 0u && r != 0u) ? (uint32_t)((60ull * CLK_HZ + r / 2u) / r) : 0u;
            uart_field("wirecheck s=", seconds);
            uart_field(" crank=", c);
            uart_field(" cam=", m);
            uart_field(" gaps=", g);
            uart_field(" rpm=", rpm);
            uart_field(" ovr=", o);
            uart_field(" bursts=", bursts);
            uart_puts("\r\n");
            bursts = 0u;
        }
    }
}
