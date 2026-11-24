/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Echo for the USB-CAN round-trip measurement (tools/can_rtt.py), built with
 * CMake (no CubeIDE). The behaviour is that of echo/main_user_code.c: each
 * request 0x18FF2EFC is answered with 0x18FF2F27 and the same 8 bytes, from
 * the FDCAN1 RX FIFO 0 interrupt. Keep the two in step when either changes.
 */
#include <stdint.h>

#include "board.h"

#define ECHO_ID_REQUEST 0x18FF2EFCU /* PGN 0xFF2E, priority 6, from 0xFC */
#define ECHO_ID_REPLY   0x18FF2F27U /* PGN 0xFF2F, from 0x27 */

static volatile uint32_t echo_count;   /* echoes sent */
static volatile uint32_t echo_dropped; /* requests not echoed (TX queue full) */

void FDCAN1_IT0_IRQHandler(void) {
    HAL_FDCAN_IRQHandler(&hfdcan1);
}

void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs) {
    if ((RxFifo0ITs & FDCAN_IT_RX_FIFO0_NEW_MESSAGE) == 0U) {
        return;
    }
    FDCAN_RxHeaderTypeDef rx;
    uint8_t d[8];
    while (HAL_FDCAN_GetRxFifoFillLevel(hfdcan, FDCAN_RX_FIFO0) > 0U) {
        if (HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0, &rx, d) != HAL_OK) {
            break;
        }
        /* G4: DataLength is the byte count. */
        if ((rx.IdType != FDCAN_EXTENDED_ID) || (rx.Identifier != ECHO_ID_REQUEST) || (rx.DataLength != 8U)) {
            continue;
        }
        if (HAL_FDCAN_GetTxFifoFreeLevel(hfdcan) == 0U) {
            echo_dropped++;
            continue;
        }
        FDCAN_TxHeaderTypeDef tx = { 0 };
        tx.Identifier = ECHO_ID_REPLY;
        tx.IdType = FDCAN_EXTENDED_ID;
        tx.TxFrameType = FDCAN_DATA_FRAME;
        tx.DataLength = 8U;
        tx.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
        tx.BitRateSwitch = FDCAN_BRS_OFF;
        tx.FDFormat = FDCAN_CLASSIC_CAN;
        tx.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
        tx.MessageMarker = 0U;
        if (HAL_FDCAN_AddMessageToTxFifoQ(hfdcan, &tx, d) == HAL_OK) {
            echo_count++;
        } else {
            echo_dropped++;
        }
    }
}

int main(void) {
    board_init(true);

    if (HAL_FDCAN_ConfigGlobalFilter(&hfdcan1, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_ACCEPT_IN_RX_FIFO0,
                                     FDCAN_FILTER_REMOTE, FDCAN_FILTER_REMOTE) != HAL_OK) {
        board_fail();
    }
    if (HAL_FDCAN_ActivateNotification(&hfdcan1, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0U) != HAL_OK) {
        board_fail();
    }
    if (HAL_FDCAN_Start(&hfdcan1) != HAL_OK) {
        board_fail();
    }

    for (;;) {
        __WFI(); /* everything happens in the interrupt */
    }
}
