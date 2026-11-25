/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The independent watchdog and the reset cause. See watchdog.h.
 * Key values and the start-up sequence: RM0440, "Independent watchdog (IWDG)".
 */
#include "watchdog.h"

#include "stm32g4xx.h"

#define IWDG_KEY_START 0xCCCCU  /* starts the IWDG (and the LSI) */
#define IWDG_KEY_ACCESS 0x5555U /* unlocks PR and RLR */
#define IWDG_KEY_RELOAD 0xAAAAU /* reloads the counter */

#define IWDG_PRESCALER_32 0x3U  /* PR: LSI / 32 = 1 kHz, one count per ms */

bool oc_wdg_reset_was_watchdog(void) {
    const bool watchdog = (RCC->CSR & RCC_CSR_IWDGRSTF) != 0U;
    RCC->CSR |= RCC_CSR_RMVF; /* clear every reset flag */
    return watchdog;
}

void oc_wdg_start(void) {
    DBGMCU->APB1FZR1 |= DBGMCU_APB1FZR1_DBG_IWDG_STOP; /* a halted core is not a hang */
    IWDG->KR = IWDG_KEY_START;
    IWDG->KR = IWDG_KEY_ACCESS;
    IWDG->PR = IWDG_PRESCALER_32;
    IWDG->RLR = OC_WDG_TIMEOUT_MS - 1U;
    while ((IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU | IWDG_SR_WVU)) != 0U) {
        /* wait until the new prescaler and reload are in the LSI domain */
    }
    IWDG->KR = IWDG_KEY_RELOAD;
}

void oc_wdg_kick(void) {
    IWDG->KR = IWDG_KEY_RELOAD;
}
