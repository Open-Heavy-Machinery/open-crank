/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Harness check for the Level 2 wiring (WIRING-L2.md), built with CMake. Not
 * an ECU: it only shows that each wire between the Tang Nano 20K and this
 * board goes where the map says, and measures what the bridge's fidelity file
 * needs (open-tick gateware/tang_nano_20k/fidelity.csv).
 *
 * - Crank (PA0, TIM2_CH1) and cam (PA1, TIM2_CH2): every rising edge is
 *   captured. A tooth interval over 1.5 times the one before is the 60-2
 *   wheel's gap, once per revolution; the time between two gaps gives the
 *   speed. Every other interval is one tooth period, and is summed for its
 *   statistics (count, sum, sum of squares, min, max, in TIM2 ticks).
 * - Injectors 1-6 (PA8, PA9, PA10, PB4, PB5, PB6): on command, a counted
 *   series of bursts. In each burst, one pulse on each line in turn, injector
 *   k high for k x 100 us. The bridge captures their widths, so a crossed
 *   wire shows as the wrong width on a channel, an open one as no pulse, and
 *   a lost capture as a count below the bursts sent.
 * - Commands, over USART2 RX (the ST-LINK's virtual COM port), one per line:
 *     bursts <hz> <n>   send n bursts, hz a second (1-100, 1000 / hz whole);
 *                       answered "wirecheck: bursts hz=<hz> n=<n>", and
 *                       "wirecheck: bursts done n=<n>" after the last one
 *     stop              stop sending bursts
 *   No burst is sent until a command asks for them.
 * - Once a second, over USART2 TX (115200 8N1):
 *     wirecheck s=<n> crank=<edges> cam=<edges> gaps=<n> rpm=<r> ovr=<n> bursts=<n>
 *         tn=<n> ts=<sum> tq=<sum of squares> tmin=<ticks> tmax=<ticks>
 *   (one line) counts over the last second; ovr counts capture overruns (an
 *   edge lost); tn..tmax are the tooth periods' statistics, gap excluded.
 *
 * Registers through CMSIS only: no HAL module beyond board_init()'s.
 */
#include <stdint.h>

#include "board.h"

#define CLK_HZ 170000000u
#define UART_BAUD 115200u
#define PULSE_UNIT_US 100u
#define PULSE_GAP_US 100u
#define MAX_BURST_HZ 100u

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
static volatile uint32_t tooth_n, tooth_min, tooth_max;
static volatile uint64_t tooth_sum, tooth_sq;
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
            } else if (last_dt != 0u) {
                /* one tooth period: the gap and the first interval are not */
                tooth_n++;
                tooth_sum += dt;
                tooth_sq += (uint64_t)dt * dt;
                if (dt < tooth_min) {
                    tooth_min = dt;
                }
                if (dt > tooth_max) {
                    tooth_max = dt;
                }
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
    /* USART2_TX on PA2 and USART2_RX on PA3 (AF7), the ST-LINK's VCP */
    pin_mode(GPIOA, 2u, 2u, 7u, 0u);
    pin_mode(GPIOA, 3u, 2u, 7u, 1u);

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

    /* USART2: 115200 8N1, with its FIFO, so a command arriving during a burst
     * (up to 2.7 ms of busy wait) waits there */
    USART2->BRR = (CLK_HZ + UART_BAUD / 2u) / UART_BAUD;
    USART2->CR1 = USART_CR1_FIFOEN | USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;

    /* DWT cycle counter, for the pulse widths */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void uart_puts(const char *s) {
    while (*s != '\0') {
        while ((USART2->ISR & USART_ISR_TXE_TXFNF) == 0u) {
        }
        USART2->TDR = (uint8_t)*s++;
    }
}

static void uart_put_u64(uint64_t v) {
    char d[20];
    unsigned n = 0u;
    do {
        d[n++] = (char)('0' + (unsigned)(v % 10u));
        v /= 10u;
    } while (v != 0u);
    char s[21];
    for (unsigned i = 0u; i < n; i++) {
        s[i] = d[n - 1u - i];
    }
    s[n] = '\0';
    uart_puts(s);
}

static void uart_field(const char *name, uint64_t v) {
    uart_puts(name);
    uart_put_u64(v);
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

/* Burst series: period in ms, bursts left, bursts sent in this series. */
static uint32_t burst_period_ms, bursts_left, bursts_sent, next_burst;

/* Reads a decimal number at *p, moving *p past it; 0 digits gives 0 and *ok = 0. */
static uint32_t parse_u32(const char **p, int *ok) {
    while (**p == ' ') {
        (*p)++;
    }
    uint32_t v = 0u;
    int digits = 0;
    while (**p >= '0' && **p <= '9' && v < 100000000u) {
        v = v * 10u + (uint32_t)(**p - '0');
        (*p)++;
        digits++;
    }
    if (digits == 0) {
        *ok = 0;
    }
    return v;
}

static int starts_with(const char *s, const char *prefix) {
    while (*prefix != '\0') {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

static void command(const char *line, uint32_t now) {
    if (starts_with(line, "bursts ")) {
        const char *p = line + 7;
        int ok = 1;
        const uint32_t hz = parse_u32(&p, &ok);
        const uint32_t n = parse_u32(&p, &ok);
        if (ok && hz >= 1u && hz <= MAX_BURST_HZ && 1000u % hz == 0u && n >= 1u) {
            burst_period_ms = 1000u / hz;
            bursts_left = n;
            bursts_sent = 0u;
            next_burst = now;
            uart_field("wirecheck: bursts hz=", hz);
            uart_field(" n=", n);
            uart_puts("\r\n");
            return;
        }
    } else if (starts_with(line, "stop")) {
        bursts_left = 0u;
        uart_puts("wirecheck: stopped\r\n");
        return;
    }
    uart_puts("wirecheck: bad command\r\n");
}

/* Collects characters from the receive FIFO into a line; runs a whole line. */
static void poll_commands(uint32_t now) {
    static char line[32];
    static unsigned len;
    if ((USART2->ISR & USART_ISR_ORE) != 0u) {
        USART2->ICR = USART_ICR_ORECF;
        len = 0u;
    }
    while ((USART2->ISR & USART_ISR_RXNE_RXFNE) != 0u) {
        const char c = (char)(USART2->RDR & 0xFFu);
        if (c == '\r' || c == '\n') {
            if (len != 0u) {
                line[len] = '\0';
                command(line, now);
            }
            len = 0u;
        } else if (len < sizeof(line) - 1u) {
            line[len++] = c;
        }
    }
}

int main(void) {
    board_init(false);
    io_init();
    tooth_min = 0xFFFFFFFFu;
    uart_puts("wirecheck: start\r\n");

    uint32_t next_report = HAL_GetTick() + 1000u;
    uint32_t seconds = 0u, bursts = 0u;
    for (;;) {
        const uint32_t now = HAL_GetTick();
        poll_commands(now);
        if (bursts_left != 0u && (int32_t)(now - next_burst) >= 0) {
            burst();
            bursts++;
            bursts_sent++;
            next_burst += burst_period_ms;
            if (--bursts_left == 0u) {
                uart_field("wirecheck: bursts done n=", bursts_sent);
                uart_puts("\r\n");
            }
        }
        if ((int32_t)(now - next_report) >= 0) {
            next_report += 1000u;
            seconds++;
            __disable_irq();
            const uint32_t c = crank_edges, m = cam_edges, g = gaps, o = overruns, r = rev_ticks;
            const uint32_t tn = tooth_n, tmin = tooth_min, tmax = tooth_max;
            const uint64_t ts = tooth_sum, tq = tooth_sq;
            crank_edges = cam_edges = gaps = overruns = 0u;
            tooth_n = 0u;
            tooth_sum = tooth_sq = 0u;
            tooth_min = 0xFFFFFFFFu;
            tooth_max = 0u;
            __enable_irq();
            const uint32_t rpm = (g != 0u && r != 0u) ? (uint32_t)((60ull * CLK_HZ + r / 2u) / r) : 0u;
            uart_field("wirecheck s=", seconds);
            uart_field(" crank=", c);
            uart_field(" cam=", m);
            uart_field(" gaps=", g);
            uart_field(" rpm=", rpm);
            uart_field(" ovr=", o);
            uart_field(" bursts=", bursts);
            uart_field(" tn=", tn);
            uart_field(" ts=", ts);
            uart_field(" tq=", tq);
            uart_field(" tmin=", tn != 0u ? tmin : 0u);
            uart_field(" tmax=", tmax);
            uart_puts("\r\n");
            bursts = 0u;
        }
    }
}
