/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The reference engine ECU on the NUCLEO-G431KB (Level 2, v0.4), built with
 * CMake. The logic is portable/engine_ecu (opencrank/ecu.h); this file only
 * sets up the timers, the interrupt, CAN and the UART (docs/porting.md, "The
 * engine ECU"). Pins: platforms/stm32g4/WIRING-L2.md.
 *
 * - TIM2, 32 bits at 170 MHz, free-running: CH1 (PA0) captures every rising
 *   crank edge, CH2 (PA1) every rising cam edge, both with a pull-down and a
 *   47 ns input filter. Its interrupt calls ecu_wheel_tooth() and
 *   ecu_wheel_cam(), and ecu_wheel_poll() every 50 ms from a CH3 compare: the
 *   wheel's whole context. TRGO is "Compare Pulse" (MMS = 011): one pulse at
 *   every CH1 capture, that is at every crank edge.
 * - Injectors 1-6 on TIM1 CH1-3 (PA8, PA9, PA10), TIM3 CH1-2 (PB4, PB5) and
 *   TIM4 CH1 (PB6): one-pulse mode, PWM mode 2, slave in trigger mode on
 *   ITR1 = tim2_trgo (RM0440 tables 267 and 291). When ecu_wheel_tooth()
 *   returns an EcuArm, the interrupt programs that injector's timer (delay
 *   in CCRx, delay + width in ARR, a prescaler so it fits 16 bits) and
 *   enables its trigger: the next crank edge starts the counter in hardware,
 *   and the compare match and the update set both edges. No injector edge is
 *   set by software, so interrupt latency does not move an injection. At
 *   that next edge the interrupt disables the trigger again, so a timer
 *   fires once per arming; it counts the arming as fired if the counter is
 *   running then, and as missed if it is not.
 * - Every 10 ms, the main loop: received frames (FDCAN1, as the demo) to
 *   ecu_handle_frame(); the hand-over with TIM2's interrupt masked
 *   (ecu_wheel_status(), then ecu_wheel_set_width()); ecu_step() and its
 *   EEC1; the IWDG (board/watchdog.h, as the demo).
 * - USART2 (the ST-LINK's virtual COM port, 115200 8N1): at start a line
 *   "# oc-stm32g4-ecu ...", then a CSV header and one row every 100 ms
 *   (decision 11 of the Level 2 design). Sent from a buffer without blocking.
 *
 * Built with OC_ECU_FIXED_FUEL (the separate target oc-stm32g4-ecu-fixedfuel,
 * never the reference firmware), the width handed to the wheel is fixed at
 * 20 % fuel (520 us) instead of the governor's, so that injections can be
 * checked at a constant bridge speed, where the governor would cut fuel.
 *
 * Registers through CMSIS, beyond board_init()'s HAL clock and FDCAN set-up.
 * No CubeMX code.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <opencrank/ecu.h>
#include <opencrank/j1939.h>

#include "board.h"
#include "watchdog.h"

#define CLK_HZ 170000000u
#define UART_BAUD 115200u
#define ECU_PERIOD_MS 10u
#define REPORT_PERIODS 10u                 /* one CSV row every 100 ms */
#define POLL_TICKS (CLK_HZ / 20u)          /* ecu_wheel_poll() every 50 ms */
#define OPM_MAX_COUNT 65000u               /* a 16-bit timer's delay + width, in its prescaled ticks */

#ifdef OC_ECU_FIXED_FUEL
#define OC_ECU_BUILD "ecu-fixedfuel"
#define FIXED_FUEL_PCT 20.0f
#else
#define OC_ECU_BUILD "ecu"
#endif

/* ---- the injector timers --------------------------------------------- */

typedef struct {
    TIM_TypeDef *tim;
    uint8_t timer; /* index into tims[] */
    uint8_t ch;    /* 1-4 */
} InjOut;

/* Injector k (cylinder k) at inj[k - 1], WIRING-L2.md's order. */
static const InjOut inj[ECU_CYLINDERS] = {
    { TIM1, 0u, 1u }, { TIM1, 0u, 2u }, { TIM1, 0u, 3u }, { TIM3, 1u, 1u }, { TIM3, 1u, 2u }, { TIM4, 2u, 1u },
};

static TIM_TypeDef *const tims[3] = { TIM1, TIM3, TIM4 };
static volatile bool tim_armed[3];

/* Output-compare modes (OCxM, 4 bits). */
#define OCM_FORCED_LOW 4u
#define OCM_PWM2 7u

/* OCxM of channel ch (1-4) in its CCMR register; bit 3 of the mode is apart. */
static uint32_t ocm_bits(uint32_t ch, uint32_t mode) {
    const uint32_t shift = ((ch - 1u) % 2u) * 8u;
    return ((mode & 7u) << (TIM_CCMR1_OC1M_Pos + shift)) | (((mode >> 3) & 1u) << (16u + shift));
}

/* Channel `active` in PWM mode 2, every other channel of the timer forced low. */
static void tim_modes(TIM_TypeDef *t, uint32_t active) {
    uint32_t m1 = 0u;
    uint32_t m2 = 0u;
    for (uint32_t ch = 1u; ch <= 4u; ch++) {
        const uint32_t bits = ocm_bits(ch, (ch == active) ? OCM_PWM2 : OCM_FORCED_LOW);
        if (ch <= 2u) {
            m1 |= bits;
        } else {
            m2 |= bits;
        }
    }
    t->CCMR1 = m1;
    t->CCMR2 = m2;
}

static void tim_ccr(TIM_TypeDef *t, uint32_t ch, uint32_t v) {
    switch (ch) {
    case 1u: t->CCR1 = v; break;
    case 2u: t->CCR2 = v; break;
    case 3u: t->CCR3 = v; break;
    default: t->CCR4 = v; break;
    }
}

/* ---- the wheel's context: TIM2's interrupt ---------------------------- */

static EcuWheel wheel;
static volatile uint32_t isr_cycles_max;  /* the worst TIM2 interrupt that handled a tooth */
static volatile uint32_t overruns;        /* captures lost (CC1OF, CC2OF) */
static volatile uint32_t fired;           /* armings started by their crank edge */
static volatile uint32_t missed;          /* armings whose edge came before they were ready */
static volatile uint32_t conflicts;       /* armings refused: that timer's last pulse not over */

/* Programs the injector's timer and enables its trigger: the next crank edge starts it. */
static void arm(const EcuArm *a) {
    if ((a->injector < 1u) || (a->injector > ECU_CYLINDERS) || (a->width_ticks == 0u)) {
        return;
    }
    const InjOut *o = &inj[a->injector - 1u];
    TIM_TypeDef *t = o->tim;
    if ((t->CR1 & TIM_CR1_CEN) != 0u) {
        conflicts++;
        return;
    }
    const uint32_t total = a->delay_ticks + a->width_ticks;
    const uint32_t psc = total / OPM_MAX_COUNT;
    const uint32_t div = psc + 1u;
    uint32_t ccr = (a->delay_ticks + div / 2u) / div;
    uint32_t w = (a->width_ticks + div / 2u) / div;
    if (ccr == 0u) {
        ccr = 1u; /* RM0440: 0 < CCRx in one-pulse mode */
    }
    if (w == 0u) {
        w = 1u;
    }
    t->PSC = psc;
    t->ARR = ccr + w - 1u; /* high from CNT = CCRx to ARR: w counts */
    tim_ccr(t, o->ch, ccr);
    tim_modes(t, o->ch);
    t->EGR = TIM_EGR_UG; /* load PSC, CNT = 0 (URS: no update flag) */
    t->SMCR = (1u << TIM_SMCR_TS_Pos) | (6u << TIM_SMCR_SMS_Pos); /* ITR1, trigger mode */
    tim_armed[o->timer] = true;
}

static void on_tooth(uint32_t t) {
    /* the timers armed on the previous edge were started by this one */
    for (uint32_t i = 0u; i < 3u; i++) {
        if (tim_armed[i]) {
            TIM_TypeDef *tm = tims[i];
            tm->SMCR = 0u;
            tim_armed[i] = false;
            if ((tm->CR1 & TIM_CR1_CEN) != 0u) {
                fired++;
            } else {
                missed++;
            }
        }
    }
    EcuArm a;
    if (ecu_wheel_tooth(&wheel, t, &a)) {
        arm(&a);
    }
}

void TIM2_IRQHandler(void) {
    const uint32_t c0 = DWT->CYCCNT;
    const uint32_t sr = TIM2->SR;
    if ((sr & (TIM_SR_CC1OF | TIM_SR_CC2OF)) != 0u) {
        overruns++;
        TIM2->SR = ~(TIM_SR_CC1OF | TIM_SR_CC2OF);
    }
    const bool tooth = (sr & TIM_SR_CC1IF) != 0u;
    bool cam = (sr & TIM_SR_CC2IF) != 0u;
    const uint32_t tt = tooth ? TIM2->CCR1 : 0u; /* reading clears CC1IF */
    const uint32_t tc = cam ? TIM2->CCR2 : 0u;
    if (tooth && cam && ((int32_t)(tc - tt) < 0)) {
        ecu_wheel_cam(&wheel, tc); /* the cam edge came first */
        cam = false;
    }
    if (tooth) {
        on_tooth(tt);
    }
    if (cam) {
        ecu_wheel_cam(&wheel, tc);
    }
    if ((sr & TIM_SR_CC3IF) != 0u) {
        TIM2->SR = ~TIM_SR_CC3IF;
        TIM2->CCR3 += POLL_TICKS;
        ecu_wheel_poll(&wheel, TIM2->CNT);
    }
    if (tooth) {
        const uint32_t dc = DWT->CYCCNT - c0;
        if (dc > isr_cycles_max) {
            isr_cycles_max = dc;
        }
    }
}

/* ---- set-up ----------------------------------------------------------- */

static void pin_mode(GPIO_TypeDef *port, uint32_t pin, uint32_t mode, uint32_t af, uint32_t pull) {
    port->MODER = (port->MODER & ~(3u << (pin * 2u))) | (mode << (pin * 2u));
    port->PUPDR = (port->PUPDR & ~(3u << (pin * 2u))) | (pull << (pin * 2u));
    port->OSPEEDR &= ~(3u << (pin * 2u)); /* low speed */
    port->OTYPER &= ~(1u << pin);         /* push-pull */
    volatile uint32_t *afr = &port->AFR[pin / 8u];
    *afr = (*afr & ~(0xFu << ((pin % 8u) * 4u))) | (af << ((pin % 8u) * 4u));
}

static void injector_timers_init(void) {
    for (uint32_t i = 0u; i < 3u; i++) {
        TIM_TypeDef *t = tims[i];
        t->CR1 = TIM_CR1_OPM | TIM_CR1_URS; /* stopped; one pulse; only an overflow is an update */
        t->SMCR = 0u;
        t->PSC = 0u;
        t->ARR = 0xFFFFu;
        tim_modes(t, 0u); /* every channel forced low */
        t->EGR = TIM_EGR_UG;
        t->SR = 0u;
    }
    TIM1->CCER = TIM_CCER_CC1E | TIM_CCER_CC2E | TIM_CCER_CC3E;
    TIM1->BDTR = TIM_BDTR_MOE; /* the advanced timer's outputs on */
    TIM3->CCER = TIM_CCER_CC1E | TIM_CCER_CC2E;
    TIM4->CCER = TIM_CCER_CC1E;
}

static void io_init(void) {
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN;
    RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN | RCC_APB1ENR1_TIM3EN | RCC_APB1ENR1_TIM4EN | RCC_APB1ENR1_USART2EN;
    RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;
    (void)RCC->APB2ENR;

    injector_timers_init();
    /* injectors: the timers' outputs, low until armed (AF6 for TIM1, AF2 for TIM3 and TIM4) */
    pin_mode(GPIOA, 8u, 2u, 6u, 0u);
    pin_mode(GPIOA, 9u, 2u, 6u, 0u);
    pin_mode(GPIOA, 10u, 2u, 6u, 0u);
    pin_mode(GPIOB, 4u, 2u, 2u, 0u);
    pin_mode(GPIOB, 5u, 2u, 2u, 0u);
    pin_mode(GPIOB, 6u, 2u, 2u, 0u);
    /* crank and cam: TIM2_CH1/CH2 (AF1), pull-down */
    pin_mode(GPIOA, 0u, 2u, 1u, 2u);
    pin_mode(GPIOA, 1u, 2u, 1u, 2u);
    /* USART2_TX on PA2 (AF7), the ST-LINK's VCP */
    pin_mode(GPIOA, 2u, 2u, 7u, 0u);

    /* TIM2: free-running at 170 MHz, 32 bits; CH1/CH2 capture TI1/TI2 on rising
     * edges, filter fCK_INT with N = 8 (47 ns); CH3 a frozen compare for the
     * poll; TRGO a pulse at every CH1 capture */
    TIM2->PSC = 0u;
    TIM2->ARR = 0xFFFFFFFFu;
    TIM2->CCMR1 = (1u << TIM_CCMR1_CC1S_Pos) | (3u << TIM_CCMR1_IC1F_Pos) | (1u << TIM_CCMR1_CC2S_Pos) |
                  (3u << TIM_CCMR1_IC2F_Pos);
    TIM2->CCMR2 = 0u;
    TIM2->CCER = TIM_CCER_CC1E | TIM_CCER_CC2E;
    TIM2->CR2 = 3u << TIM_CR2_MMS_Pos;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->CCR3 = POLL_TICKS;
    TIM2->SR = 0u;
    TIM2->DIER = TIM_DIER_CC1IE | TIM_DIER_CC2IE | TIM_DIER_CC3IE;
    NVIC_SetPriority(TIM2_IRQn, 0u);
    NVIC_EnableIRQ(TIM2_IRQn);
    TIM2->CR1 = TIM_CR1_CEN;

    USART2->BRR = (CLK_HZ + UART_BAUD / 2u) / UART_BAUD;
    USART2->CR1 = USART_CR1_FIFOEN | USART_CR1_TE | USART_CR1_UE;

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* ---- USART2: a buffer drained by the main loop ------------------------ */

#define TX_BUF 512u
static char tx_buf[TX_BUF];
static uint32_t tx_head, tx_tail;
static uint32_t uart_dropped; /* characters that did not fit */

static void uart_putc(char c) {
    const uint32_t next = (tx_head + 1u) % TX_BUF;
    if (next == tx_tail) {
        uart_dropped++;
        return;
    }
    tx_buf[tx_head] = c;
    tx_head = next;
}

static void uart_puts(const char *s) {
    while (*s != '\0') {
        uart_putc(*s);
        s++;
    }
}

static void uart_pump(void) {
    while ((tx_tail != tx_head) && ((USART2->ISR & USART_ISR_TXE_TXFNF) != 0u)) {
        USART2->TDR = (uint8_t)tx_buf[tx_tail];
        tx_tail = (tx_tail + 1u) % TX_BUF;
    }
}

static void uart_u32(uint32_t v) {
    char d[10];
    uint32_t n = 0u;
    do {
        d[n] = (char)('0' + (v % 10u));
        n++;
        v /= 10u;
    } while (v != 0u);
    while (n > 0u) {
        n--;
        uart_putc(d[n]);
    }
}

/* v / 10^decimals, as a decimal with that many digits after the point. */
static void uart_fixed(int32_t v, uint32_t decimals) {
    uint32_t scale = 1u;
    for (uint32_t i = 0u; i < decimals; i++) {
        scale *= 10u;
    }
    uint32_t a = (uint32_t)v;
    if (v < 0) {
        uart_putc('-');
        a = 0u - a;
    }
    uart_u32(a / scale);
    if (decimals != 0u) {
        uart_putc('.');
        uint32_t f = a % scale;
        for (uint32_t s = scale / 10u; s != 0u; s /= 10u) {
            uart_putc((char)('0' + (f / s)));
            f %= s;
        }
    }
}

static int32_t scaled(float v, float scale) {
    const float x = v * scale;
    return (int32_t)((x >= 0.0f) ? (x + 0.5f) : (x - 0.5f));
}

static const char *const SYNC_NAMES[] = { "NONE", "SEEKING", "CRANK", "FULL" };

/* ---- CAN, as the demo -------------------------------------------------- */

static EcuCtx ecu;
static uint32_t tx_dropped;
static volatile uint32_t step_cycles_max; /* worst observed ecu_step, in CPU cycles */

static void can_poll_rx(uint32_t now_ms) {
    FDCAN_RxHeaderTypeDef rx;
    uint8_t d[8];
    while (HAL_FDCAN_GetRxFifoFillLevel(&hfdcan1, FDCAN_RX_FIFO0) > 0U) {
        if (HAL_FDCAN_GetRxMessage(&hfdcan1, FDCAN_RX_FIFO0, &rx, d) != HAL_OK) {
            break;
        }
        if (rx.IdType != FDCAN_EXTENDED_ID) {
            continue; /* J1939 is 29-bit only */
        }
        const uint32_t len = rx.DataLength; /* G4: the byte count */
        if (len > 8U) {
            continue;
        }
        J1939Frame f;
        f.id = rx.Identifier & 0x1FFFFFFFU;
        f.dlc = (uint8_t)len;
        for (uint32_t i = 0U; i < 8U; i++) {
            f.data[i] = (i < len) ? d[i] : 0xFFU;
        }
        ecu_handle_frame(&ecu, &f, now_ms);
    }
}

static void can_send(const J1939Frame *f) {
    if (HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan1) == 0U) {
        tx_dropped++; /* count it; never block */
        return;
    }
    FDCAN_TxHeaderTypeDef tx = { 0 };
    tx.Identifier = f->id;
    tx.IdType = FDCAN_EXTENDED_ID;
    tx.TxFrameType = FDCAN_DATA_FRAME;
    tx.DataLength = f->dlc;
    tx.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    tx.BitRateSwitch = FDCAN_BRS_OFF;
    tx.FDFormat = FDCAN_CLASSIC_CAN;
    tx.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    tx.MessageMarker = 0U;
    uint8_t d[8];
    for (uint32_t i = 0U; i < 8U; i++) {
        d[i] = f->data[i];
    }
    (void)HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &tx, d);
}

#ifdef OC_ECU_FIXED_FUEL
/* Test build only: the injector width of FIXED_FUEL_PCT through the injector model, in ticks. */
uint32_t __attribute__((noinline)) oc_test_fixed_fuel_ticks(const EcuParams *p);
uint32_t __attribute__((noinline)) oc_test_fixed_fuel_ticks(const EcuParams *p) {
    const float us = p->injector_dead_us + ((p->injector_full_us * FIXED_FUEL_PCT) / 100.0f);
    return (uint32_t)(us * ((float)p->tick_hz / 1000000.0f));
}
#endif

/* ---- the report --------------------------------------------------------- */

typedef struct {
    EcuWheelStatus ws;
    uint32_t isr_cycles_max, overruns, fired, missed, conflicts;
} Snapshot;

static void report_header(bool after_watchdog) {
    uart_puts("# oc-stm32g4-ecu start; build=" OC_ECU_BUILD "; reset=");
    uart_puts(after_watchdog ? "watchdog" : "other");
    uart_puts("; tick_hz=170000000\r\n");
    uart_puts("ms,sync,rpm,target_rpm,fuel_pct,width_us,sync_losses,gaps,injections,arms,missed,conflicts,"
              "teeth,cam,stops,overruns,isr_cycles_max,step_cycles_max,tx_dropped\r\n");
}

static void report_row(uint32_t now_ms, const Snapshot *s, uint32_t width_ticks) {
    const EcuWheelStatus *ws = &s->ws;
    uart_u32(now_ms);
    uart_putc(',');
    uart_puts(SYNC_NAMES[(uint32_t)ws->sync & 3u]);
    uart_putc(',');
    uart_fixed(scaled(ws->rpm, 10.0f), 1u);
    uart_putc(',');
    uart_fixed(scaled(ecu.target_rpm, 10.0f), 1u);
    uart_putc(',');
#ifdef OC_ECU_FIXED_FUEL
    uart_fixed(scaled((width_ticks != 0u) ? FIXED_FUEL_PCT : 0.0f, 100.0f), 2u);
#else
    uart_fixed(scaled(ecu.fuel_pct, 100.0f), 2u);
#endif
    uart_putc(',');
    uart_fixed((int32_t)((width_ticks * 10u + (CLK_HZ / 1000000u) / 2u) / (CLK_HZ / 1000000u)), 1u);
    const uint32_t f[] = { ws->sync_losses, ws->gaps, s->fired, ws->arms, s->missed, s->conflicts, ws->teeth,
                           ws->cam_edges, ws->stops, s->overruns, s->isr_cycles_max, step_cycles_max, tx_dropped };
    for (uint32_t i = 0u; i < (uint32_t)(sizeof(f) / sizeof(f[0])); i++) {
        uart_putc(',');
        uart_u32(f[i]);
    }
    uart_puts("\r\n");
}

int main(void) {
    const bool after_watchdog = oc_wdg_reset_was_watchdog(); /* also clears the reset flags */
    board_init(false);
    if (HAL_FDCAN_ConfigGlobalFilter(&hfdcan1, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_ACCEPT_IN_RX_FIFO0,
                                     FDCAN_FILTER_REMOTE, FDCAN_FILTER_REMOTE) != HAL_OK) {
        board_fail();
    }
    if (HAL_FDCAN_Start(&hfdcan1) != HAL_OK) {
        board_fail();
    }

    ecu_init(&ecu, &ECU_DEFAULT_PARAMS);
    ecu_wheel_init(&wheel, &ECU_DEFAULT_PARAMS);
    io_init(); /* enables TIM2's interrupt: the wheel is ready */
    report_header(after_watchdog);
    oc_wdg_start();

    uint32_t next = HAL_GetTick() + ECU_PERIOD_MS;
    uint32_t periods = 0u;
    for (;;) {
        const uint32_t now = HAL_GetTick();
        can_poll_rx(now);
        uart_pump();

        if ((int32_t)(now - next) >= 0) {
            next += ECU_PERIOD_MS;

            Snapshot s;
            NVIC_DisableIRQ(TIM2_IRQn);
            ecu_wheel_status(&wheel, &s.ws);
            s.isr_cycles_max = isr_cycles_max;
            s.overruns = overruns;
            s.fired = fired;
            s.missed = missed;
            s.conflicts = conflicts;
            NVIC_EnableIRQ(TIM2_IRQn);

            J1939Frame out[1];
            const uint32_t c0 = DWT->CYCCNT;
            const size_t n = ecu_step(&ecu, &s.ws, now, out, 1u);
            const uint32_t dc = DWT->CYCCNT - c0;
            if (dc > step_cycles_max) {
                step_cycles_max = dc;
            }
#ifdef OC_ECU_FIXED_FUEL
            const uint32_t width = (s.ws.sync == ECU_SYNC_FULL) ? oc_test_fixed_fuel_ticks(&ECU_DEFAULT_PARAMS) : 0u;
#else
            const uint32_t width = ecu_width_ticks(&ecu);
#endif
            NVIC_DisableIRQ(TIM2_IRQn);
            ecu_wheel_set_width(&wheel, width);
            NVIC_EnableIRQ(TIM2_IRQn);

            for (size_t k = 0U; k < n; k++) {
                can_send(&out[k]);
            }
            periods++;
            if ((periods % REPORT_PERIODS) == 0u) {
                report_row(now, &s, width);
            }
            oc_wdg_kick();
        }
    }
}
