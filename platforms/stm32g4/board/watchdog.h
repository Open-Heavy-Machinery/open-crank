/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The independent watchdog (IWDG) of the STM32G431 and the reset cause.
 * Register level (CMSIS), no HAL module, so the CMake build and a CubeIDE
 * project can use the same file. docs/safety.md, SR-11.
 */
#ifndef OC_STM32G4_WATCHDOG_H
#define OC_STM32G4_WATCHDOG_H

#include <stdbool.h>

/* Watchdog timeout: the main loop must kick it at least this often. */
#define OC_WDG_TIMEOUT_MS 100U

/*
 * True if the last reset was caused by the IWDG (RCC_CSR.IWDGRSTF). Reads the
 * reset flags and then clears all of them (RCC_CSR.RMVF), so a later reset of
 * another kind (power-on, NRST, software) is not reported as a watchdog one.
 * Call it once, at start-up, before oc_wdg_start().
 */
bool oc_wdg_reset_was_watchdog(void);

/*
 * Starts the IWDG with OC_WDG_TIMEOUT_MS (LSI 32 kHz / 32 = 1 kHz) and makes
 * it stop while a debugger halts the core. Once started, only a reset stops it.
 */
void oc_wdg_start(void);

/* Reloads the IWDG counter: the loop is alive. */
void oc_wdg_kick(void);

#endif /* OC_STM32G4_WATCHDOG_H */
