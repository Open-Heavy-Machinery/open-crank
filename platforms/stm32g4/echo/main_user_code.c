/* SPDX-License-Identifier: Apache-2.0 */
/* Echo for the USB-CAN round-trip measurement (v0.2), to paste into a separate STM32CubeIDE project. It does not compile on its own and it is not portable code. */

/* USER CODE PV */
static volatile uint32_t echo_count;    /* ecos enviados */
static volatile uint32_t echo_dropped;  /* peticiones sin eco (cola TX llena) */

/* USER CODE 0 */
#define ECHO_ID_REQUEST 0x18FF2EFCU   /* PGN 0xFF2E, prioridad 6, desde 0xFC */
#define ECHO_ID_REPLY   0x18FF2F27U   /* PGN 0xFF2F, desde 0x27 */

void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs)
{
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
    FDCAN_TxHeaderTypeDef tx = {0};
    tx.Identifier          = ECHO_ID_REPLY;
    tx.IdType              = FDCAN_EXTENDED_ID;
    tx.TxFrameType         = FDCAN_DATA_FRAME;
    tx.DataLength          = 8U;
    tx.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    tx.BitRateSwitch       = FDCAN_BRS_OFF;
    tx.FDFormat            = FDCAN_CLASSIC_CAN;
    tx.TxEventFifoControl  = FDCAN_NO_TX_EVENTS;
    tx.MessageMarker       = 0U;
    if (HAL_FDCAN_AddMessageToTxFifoQ(hfdcan, &tx, d) == HAL_OK) {
      echo_count++;
    } else {
      echo_dropped++;
    }
  }
}


/* USER CODE 2 */
  if (HAL_FDCAN_ConfigGlobalFilter(&hfdcan1, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_ACCEPT_IN_RX_FIFO0,
                                    FDCAN_FILTER_REMOTE, FDCAN_FILTER_REMOTE) != HAL_OK)
   {
     Error_Handler();
   }
   if (HAL_FDCAN_ActivateNotification(&hfdcan1, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0U) != HAL_OK)
   {
     Error_Handler();
   }
   if (HAL_FDCAN_Start(&hfdcan1) != HAL_OK)
   {
     Error_Handler();
   }

/* USER CODE WHILE */
  while (1)
  {
