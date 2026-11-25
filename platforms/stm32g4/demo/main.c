/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Reference controller on the NUCLEO-G431KB, built with CMake (no CubeIDE).
 * The behaviour is that of demo/main_user_code.c, the CubeIDE entry point:
 * every frame into RX FIFO 0, mc_step every 10 ms, TX never blocks, and the
 * DWT cycle counter keeps the worst observed mc_step in mc_step_cycles_max.
 * Keep the two in step when either changes.
 *
 * Since v0.3.1 (CMake build only, not yet in main_user_code.c): the IWDG
 * (board/watchdog.h, 100 ms) is kicked every control period, and after a
 * watchdog reset the controller starts in FAULT (mc_init_after_watchdog,
 * docs/safety.md SR-11). Built with OC_WDG_HANG_TEST (the separate target
 * oc-stm32g4-demo-hangtest, never the reference firmware), the board hangs
 * on purpose when it receives the test frame OC_TEST_HANG_ID, so that the
 * watchdog can be tested on the bus.
 */
#include <stddef.h>
#include <stdint.h>

#include <opencrank/j1939.h>
#include <opencrank/machine_ctrl.h>

#include "board.h"
#include "watchdog.h"

#define MC_PERIOD_MS 10U /* TSC1 and pump command every 10 ms */

static McCtx mc;
static uint32_t tx_dropped;
static volatile uint32_t mc_step_cycles_max; /* worst observed mc_step, in CPU cycles */
static volatile bool started_after_watchdog;   /* for a debugger: this start followed an IWDG reset */

#ifdef OC_WDG_HANG_TEST
/* Test build only. Bench test frame, from the bench (0xFC), PGN 0xFF2D: the
 * same value as OC_TEST_HANG_ID in sim/runtime/src/bench.c. */
#define OC_TEST_HANG_ID 0x18FF2DFCUL

/* Stops the control loop without kicking the watchdog. */
static void __attribute__((noreturn, noinline)) oc_test_hang(void) {
    for (;;) {
        __NOP();
    }
}
#endif

static void can_poll_rx(uint32_t now_ms) {
    FDCAN_RxHeaderTypeDef rx;
    uint8_t d[8];

    /* Drain the FIFO: one frame per loop would lose frames at 100 frames/s+ */
    while (HAL_FDCAN_GetRxFifoFillLevel(&hfdcan1, FDCAN_RX_FIFO0) > 0U) {
        if (HAL_FDCAN_GetRxMessage(&hfdcan1, FDCAN_RX_FIFO0, &rx, d) != HAL_OK) {
            break;
        }
        if (rx.IdType != FDCAN_EXTENDED_ID) {
            continue; /* J1939 is 29-bit only */
        }
        /* G4: DataLength is the byte count. (On H7 it is shifted 16 bits.) */
        const uint32_t len = rx.DataLength;
        if (len > 8U) {
            continue;
        }
        J1939Frame f;
        f.id = rx.Identifier & 0x1FFFFFFFU;
#ifdef OC_WDG_HANG_TEST
        if (f.id == OC_TEST_HANG_ID) {
            oc_test_hang();
        }
#endif
        f.dlc = (uint8_t)len;
        for (uint32_t i = 0U; i < 8U; i++) {
            f.data[i] = (i < len) ? d[i] : 0xFFU;
        }
        mc_handle_frame(&mc, &f, now_ms);
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
    tx.DataLength = f->dlc; /* G4: plain byte count */
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

int main(void) {
    /* Read the reset cause first; this also clears the reset flags (RMVF). */
    started_after_watchdog = oc_wdg_reset_was_watchdog();
    board_init(false);

    /* Accept every frame into RX FIFO 0: the controller decides what it needs. */
    if (HAL_FDCAN_ConfigGlobalFilter(&hfdcan1, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_ACCEPT_IN_RX_FIFO0,
                                     FDCAN_FILTER_REMOTE, FDCAN_FILTER_REMOTE) != HAL_OK) {
        board_fail();
    }
    if (HAL_FDCAN_Start(&hfdcan1) != HAL_OK) {
        board_fail();
    }

    /* DWT cycle counter: worst observed execution time, not a proven bound. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    if (started_after_watchdog) {
        mc_init_after_watchdog(&mc, &MC_DEFAULT_PARAMS); /* FAULT until the operator's fault reset */
    } else {
        mc_init(&mc, &MC_DEFAULT_PARAMS);
    }
    oc_wdg_start();
    uint32_t next = HAL_GetTick() + MC_PERIOD_MS;

    for (;;) {
        const uint32_t now = HAL_GetTick();

        can_poll_rx(now);

        /* Wrap-safe: true once `now` has reached `next`. */
        if ((int32_t)(now - next) >= 0) {
            next += MC_PERIOD_MS;

            J1939Frame out[4];
            const uint32_t c0 = DWT->CYCCNT;
            const size_t n = mc_step(&mc, now, out, sizeof(out) / sizeof(out[0]));
            const uint32_t dc = DWT->CYCCNT - c0; /* wrap-safe */
            if (dc > mc_step_cycles_max) {
                mc_step_cycles_max = dc;
            }
            for (size_t k = 0U; k < n; k++) {
                can_send(&out[k]);
            }
            oc_wdg_kick(); /* once per control period: a stuck loop stops kicking */
        }
    }
}
