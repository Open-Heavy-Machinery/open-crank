/* SPDX-License-Identifier: Apache-2.0 */
/*
 * HAL configuration for open-crank's STM32G4 firmware, written for this
 * repository (not ST's template). It enables only the HAL modules the
 * firmware uses; stm32g4xx_hal.h includes this file.
 */
#ifndef OC_STM32G4XX_HAL_CONF_H
#define OC_STM32G4XX_HAL_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

/* Modules used: core, clocks, power, flash latency, GPIO, NVIC/SysTick, FDCAN. */
#define HAL_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED   /* needed by the RCC and GPIO headers' types */
#define HAL_EXTI_MODULE_ENABLED
#define HAL_FDCAN_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED

/* No callback registration: the firmware overrides the weak HAL callbacks. */
#define USE_HAL_FDCAN_REGISTER_CALLBACKS 0U

/* Oscillators. The Nucleo's 24 MHz HSE crystal is not used (see README.md). */
#define HSE_VALUE            24000000UL
#define HSE_STARTUP_TIMEOUT  100UL
#define HSI_VALUE            16000000UL
#define HSI48_VALUE          48000000UL
#define LSI_VALUE            32000UL
#define LSE_VALUE            32768UL
#define LSE_STARTUP_TIMEOUT  5000UL
#define EXTERNAL_CLOCK_VALUE 48000UL

#define VDD_VALUE                3300UL
#define TICK_INT_PRIORITY        0x0FUL /* SysTick: lowest priority */
#define USE_RTOS                 0U
#define PREFETCH_ENABLE          0U
#define INSTRUCTION_CACHE_ENABLE 1U
#define DATA_CACHE_ENABLE        1U

#define USE_SPI_CRC 0U

#include "stm32g4xx_hal_rcc.h"
#include "stm32g4xx_hal_gpio.h"
#include "stm32g4xx_hal_dma.h"
#include "stm32g4xx_hal_cortex.h"
#include "stm32g4xx_hal_exti.h"
#include "stm32g4xx_hal_fdcan.h"
#include "stm32g4xx_hal_flash.h"
#include "stm32g4xx_hal_pwr.h"

/* Parameter checks are off, as in a release build of the HAL. */
#define assert_param(expr) ((void)0U)

#ifdef __cplusplus
}
#endif

#endif /* OC_STM32G4XX_HAL_CONF_H */
