/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Board set-up for the NUCLEO-G431KB, written for this repository with ST's
 * HAL (fetched at build time, see CMakeLists.txt). The settings are those of
 * README.md: HSI 16 MHz -> PLL (M 4, N 85, R 2) -> 170 MHz; regulator in
 * Range 1 boost, flash latency 4; FDCAN kernel clock PCLK1 (170 MHz);
 * FDCAN1 nominal prescaler 17, SJW 5, seg1 34, seg2 5: 250 kbit/s, 87.5 %.
 */
#include "board.h"

FDCAN_HandleTypeDef hfdcan1;

void board_fail(void) {
    __disable_irq();
    for (;;) {
    }
}

/* The HAL's time base: SysTick at 1 ms, started by HAL_Init and HAL_RCC_ClockConfig. */
void SysTick_Handler(void) {
    HAL_IncTick();
}

static void clock_init(void) {
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();
    /* PB4/PB6 come out of reset with the USB-C dead-battery pull-downs; release them. */
    HAL_PWREx_DisableUCPDDeadBattery();

    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST) != HAL_OK) {
        board_fail();
    }

    RCC_OscInitTypeDef osc = { 0 };
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState = RCC_HSI_ON;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    osc.PLL.PLLM = RCC_PLLM_DIV4;   /* 16 MHz / 4 = 4 MHz */
    osc.PLL.PLLN = 85U;             /* 4 MHz * 85 = 340 MHz */
    osc.PLL.PLLP = RCC_PLLP_DIV2;
    osc.PLL.PLLQ = RCC_PLLQ_DIV2;
    osc.PLL.PLLR = RCC_PLLR_DIV2;   /* 340 MHz / 2 = 170 MHz */
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        board_fail();
    }

    RCC_ClkInitTypeDef clk = { 0 };
    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_4) != HAL_OK) {
        board_fail();
    }
}

/* Called by HAL_FDCAN_Init: the peripheral's clock and pins. */
void HAL_FDCAN_MspInit(FDCAN_HandleTypeDef *hfdcan) {
    if (hfdcan->Instance != FDCAN1) {
        return;
    }
    RCC_PeriphCLKInitTypeDef pclk = { 0 };
    pclk.PeriphClockSelection = RCC_PERIPHCLK_FDCAN;
    pclk.FdcanClockSelection = RCC_FDCANCLKSOURCE_PCLK1;
    if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK) {
        board_fail();
    }
    __HAL_RCC_FDCAN_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef pins = { 0 };
    pins.Pin = GPIO_PIN_11 | GPIO_PIN_12;   /* PA11 FDCAN1_RX, PA12 FDCAN1_TX */
    pins.Mode = GPIO_MODE_AF_PP;
    pins.Pull = GPIO_NOPULL;
    pins.Speed = GPIO_SPEED_FREQ_LOW;
    pins.Alternate = GPIO_AF9_FDCAN1;
    HAL_GPIO_Init(GPIOA, &pins);
}

static void fdcan1_init(void) {
    hfdcan1.Instance = FDCAN1;
    hfdcan1.Init.ClockDivider = FDCAN_CLOCK_DIV1;
    hfdcan1.Init.FrameFormat = FDCAN_FRAME_CLASSIC;
    hfdcan1.Init.Mode = FDCAN_MODE_NORMAL;
    hfdcan1.Init.AutoRetransmission = ENABLE;
    hfdcan1.Init.TransmitPause = DISABLE;
    hfdcan1.Init.ProtocolException = DISABLE;
    hfdcan1.Init.NominalPrescaler = 17U;      /* 170 MHz / 17 = 10 MHz, 40 tq per bit */
    hfdcan1.Init.NominalSyncJumpWidth = 5U;
    hfdcan1.Init.NominalTimeSeg1 = 34U;
    hfdcan1.Init.NominalTimeSeg2 = 5U;        /* (1 + 34) / 40 = 87.5 % */
    /* The data phase is unused in classic mode; the HAL still checks its range. */
    hfdcan1.Init.DataPrescaler = 1U;
    hfdcan1.Init.DataSyncJumpWidth = 1U;
    hfdcan1.Init.DataTimeSeg1 = 1U;
    hfdcan1.Init.DataTimeSeg2 = 1U;
    hfdcan1.Init.StdFiltersNbr = 0U;
    hfdcan1.Init.ExtFiltersNbr = 0U;
    hfdcan1.Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
    if (HAL_FDCAN_Init(&hfdcan1) != HAL_OK) {
        board_fail();
    }
}

void board_init(bool rx_irq) {
    if (HAL_Init() != HAL_OK) {
        board_fail();
    }
    clock_init();
    fdcan1_init();
    if (rx_irq) {
        HAL_NVIC_SetPriority(FDCAN1_IT0_IRQn, 0U, 0U);
        HAL_NVIC_EnableIRQ(FDCAN1_IT0_IRQn);
    }
}
