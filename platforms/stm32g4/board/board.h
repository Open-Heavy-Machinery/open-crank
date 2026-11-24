/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The reference board (NUCLEO-G431KB) as the firmware sees it: clocks, the
 * millisecond tick and FDCAN1 at 250 kbit/s on PA11/PA12. Board glue only;
 * the control logic is portable/ (ARCHITECTURE.md, section 2).
 */
#ifndef OC_STM32G4_BOARD_H
#define OC_STM32G4_BOARD_H

#include <stdbool.h>

#include "stm32g4xx_hal.h"

/* FDCAN1, classic CAN at 250 kbit/s (README.md, "FDCAN1"). */
extern FDCAN_HandleTypeDef hfdcan1;

/*
 * HAL, 170 MHz from the HSI, 1 ms SysTick, and FDCAN1 initialised (not started).
 * With rx_irq true, FDCAN1 interrupt line 0 is enabled in the NVIC.
 */
void board_init(bool rx_irq);

/* Unrecoverable set-up error: interrupts off, stop here (visible in a debugger). */
void board_fail(void) __attribute__((noreturn));

#endif /* OC_STM32G4_BOARD_H */
