/**
 * @file    tm_driver_config.h
 * @brief   Themis Driver Configuration
 * @version v1.2.0
 * @date    2026.03.24
 * 
 * Driver-config ownership rules
 * --------------------------------------------------------------------------
 * 1. This file is the platform capability mirror of the current Themis port.
 *    It keeps the full macro surface for supported AT32 peripheral classes,
 *    not only the resources enabled on the current board.
 * 2. Toggle-style capability macros use `// #define XXX` when disabled.
 *    This keeps IDE comment/uncomment workflow smooth in Keil / IAR / VS Code.
 * 3. Numeric tuning macros may stay defined even when the corresponding
 *    instance is disabled. They act as per-instance defaults.
 * 4. Real board truth still belongs to `include/config/tm_driver_config.h`.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Optional Board-Test Overrides --------------------------------------------------------------- */

#ifndef TM_UART1_RX_BUFSIZE_OVERRIDE
#define TM_UART1_RX_BUFSIZE_OVERRIDE 0U
#endif

#ifndef TM_UART1_TX_BUFSIZE_OVERRIDE
#define TM_UART1_TX_BUFSIZE_OVERRIDE 0U
#endif

#ifndef TM_UART1_DMA_PING_BUFSIZE_OVERRIDE
#define TM_UART1_DMA_PING_BUFSIZE_OVERRIDE 0U
#endif

/* Profile-Tuned Runtime Budgets --------------------------------------------------------------- */

#if TM_DEVDRV_PROFILE_TINY
#define TM_UART_PROFILE_RX_BUFSIZE       (32U)
#define TM_UART_PROFILE_TX_BUFSIZE       (32U)
#define TM_UART_PROFILE_DMA_PING_BUFSIZE (16U)
#elif TM_DEVDRV_PROFILE_FULL_DIAG
#define TM_UART_PROFILE_RX_BUFSIZE       (128U)
#define TM_UART_PROFILE_TX_BUFSIZE       (128U)
#define TM_UART_PROFILE_DMA_PING_BUFSIZE (64U)
#else
#define TM_UART_PROFILE_RX_BUFSIZE       (64U)
#define TM_UART_PROFILE_TX_BUFSIZE       (64U)
#define TM_UART_PROFILE_DMA_PING_BUFSIZE (32U)
#endif

/* Current Board Truth ------------------------------------------------------------------------- */
/*
 * Current AT32 Workbench configuration enables:
 * - ADC1 + DMA1 Channel7
 * - I2C1 + DMA1 Channel5/Channel6
 * - USART1 + DMA1 Channel1/Channel2
 * - USART7 + DMA1 Channel3/Channel4
 * - TMR2 CH2 (buzzer) and TMR4 CH4 (PWM output)
 * - On-chip internal flash
 */

/* ADC ----------------------------------------------------------------------------------------- */

#define BSP_USING_ADC
#define BSP_USING_ADC1
#define BSP_ADC1_RX_USING_DMA
// #define BSP_USING_ADC2
// #define BSP_ADC2_RX_USING_DMA
// #define BSP_USING_ADC3
// #define BSP_ADC3_RX_USING_DMA

/* DAC ----------------------------------------------------------------------------------------- */

// #define BSP_USING_DAC
// #define BSP_USING_DAC1

/* I2C ----------------------------------------------------------------------------------------- */

#define BSP_USING_I2C
#define BSP_USING_I2C1
/*
 * Keep I2C1 DMA enabled for the OLED path, but pair it with the board-local
 * display transport policy in `bsp_display.c`: short SSD1316 command bursts
 * stay on the lightweight non-DMA path, while full-page framebuffer writes
 * continue to use DMA.
 */
#define BSP_I2C1_RX_USING_DMA
#define BSP_I2C1_TX_USING_DMA
// #define BSP_USING_I2C2
// #define BSP_I2C2_RX_USING_DMA
// #define BSP_I2C2_TX_USING_DMA
// #define BSP_USING_I2C3
// #define BSP_I2C3_RX_USING_DMA
// #define BSP_I2C3_TX_USING_DMA
// #define BSP_USING_I2C4
// #define BSP_I2C4_RX_USING_DMA
// #define BSP_I2C4_TX_USING_DMA

/* I2C Bit-Bang -------------------------------------------------------------------------------- */

// #define BSP_USING_I2C_BITBANG
// #define BSP_USING_I2C_BITBANG1
// #define BSP_USING_I2C_BITBANG2
// #define BSP_USING_I2C_BITBANG3
// #define BSP_USING_I2C_BITBANG4

/* GPIO ---------------------------------------------------------------------------------------- */

#define BSP_USING_PIN

/* PWM ----------------------------------------------------------------------------------------- */

// #define BSP_USING_PWM1_CH1
// #define BSP_USING_PWM1_CH2
// #define BSP_USING_PWM1_CH3
// #define BSP_USING_PWM1_CH4
// #define BSP_USING_PWM1_CH1N
// #define BSP_USING_PWM1_CH2N
// #define BSP_USING_PWM1_CH3N

// #define BSP_USING_PWM2_CH1
#define BSP_USING_PWM2_CH2
// #define BSP_USING_PWM2_CH3
// #define BSP_USING_PWM2_CH4

// #define BSP_USING_PWM3_CH1
// #define BSP_USING_PWM3_CH2
// #define BSP_USING_PWM3_CH3
// #define BSP_USING_PWM3_CH4

// #define BSP_USING_PWM4_CH1
// #define BSP_USING_PWM4_CH2
// #define BSP_USING_PWM4_CH3
#define BSP_USING_PWM4_CH4

// #define BSP_USING_PWM8_CH1
// #define BSP_USING_PWM8_CH2
// #define BSP_USING_PWM8_CH3
// #define BSP_USING_PWM8_CH4
// #define BSP_USING_PWM8_CH1N
// #define BSP_USING_PWM8_CH2N
// #define BSP_USING_PWM8_CH3N

// #define BSP_USING_PWM9_CH1
// #define BSP_USING_PWM9_CH2
// #define BSP_USING_PWM10_CH1
// #define BSP_USING_PWM11_CH1
// #define BSP_USING_PWM12_CH1
// #define BSP_USING_PWM12_CH2
// #define BSP_USING_PWM13_CH1
// #define BSP_USING_PWM14_CH1

/* Legacy PWM bridge --------------------------------------------------------------------------- */

#if defined(BSP_USING_TIM1_CH1) && !defined(BSP_USING_PWM1_CH1)
#define BSP_USING_PWM1_CH1
#endif
#if defined(BSP_USING_TIM1_CH2) && !defined(BSP_USING_PWM1_CH2)
#define BSP_USING_PWM1_CH2
#endif
#if defined(BSP_USING_TIM1_CH3) && !defined(BSP_USING_PWM1_CH3)
#define BSP_USING_PWM1_CH3
#endif
#if defined(BSP_USING_TIM1_CH4) && !defined(BSP_USING_PWM1_CH4)
#define BSP_USING_PWM1_CH4
#endif

#if defined(BSP_USING_TIM2_CH1) && !defined(BSP_USING_PWM2_CH1)
#define BSP_USING_PWM2_CH1
#endif
#if defined(BSP_USING_TIM2_CH2) && !defined(BSP_USING_PWM2_CH2)
#define BSP_USING_PWM2_CH2
#endif
#if defined(BSP_USING_TIM2_CH3) && !defined(BSP_USING_PWM2_CH3)
#define BSP_USING_PWM2_CH3
#endif
#if defined(BSP_USING_TIM2_CH4) && !defined(BSP_USING_PWM2_CH4)
#define BSP_USING_PWM2_CH4
#endif

#if defined(BSP_USING_TIM3_CH1) && !defined(BSP_USING_PWM3_CH1)
#define BSP_USING_PWM3_CH1
#endif
#if defined(BSP_USING_TIM3_CH2) && !defined(BSP_USING_PWM3_CH2)
#define BSP_USING_PWM3_CH2
#endif
#if defined(BSP_USING_TIM3_CH3) && !defined(BSP_USING_PWM3_CH3)
#define BSP_USING_PWM3_CH3
#endif
#if defined(BSP_USING_TIM3_CH4) && !defined(BSP_USING_PWM3_CH4)
#define BSP_USING_PWM3_CH4
#endif

#if defined(BSP_USING_TIM4_CH1) && !defined(BSP_USING_PWM4_CH1)
#define BSP_USING_PWM4_CH1
#endif
#if defined(BSP_USING_TIM4_CH2) && !defined(BSP_USING_PWM4_CH2)
#define BSP_USING_PWM4_CH2
#endif
#if defined(BSP_USING_TIM4_CH3) && !defined(BSP_USING_PWM4_CH3)
#define BSP_USING_PWM4_CH3
#endif
#if defined(BSP_USING_TIM4_CH4) && !defined(BSP_USING_PWM4_CH4)
#define BSP_USING_PWM4_CH4
#endif

#if defined(BSP_USING_TIM8_CH1) && !defined(BSP_USING_PWM8_CH1)
#define BSP_USING_PWM8_CH1
#endif
#if defined(BSP_USING_TIM8_CH2) && !defined(BSP_USING_PWM8_CH2)
#define BSP_USING_PWM8_CH2
#endif
#if defined(BSP_USING_TIM8_CH3) && !defined(BSP_USING_PWM8_CH3)
#define BSP_USING_PWM8_CH3
#endif
#if defined(BSP_USING_TIM8_CH4) && !defined(BSP_USING_PWM8_CH4)
#define BSP_USING_PWM8_CH4
#endif

#if defined(BSP_USING_TIM9_CH1) && !defined(BSP_USING_PWM9_CH1)
#define BSP_USING_PWM9_CH1
#endif
#if defined(BSP_USING_TIM9_CH2) && !defined(BSP_USING_PWM9_CH2)
#define BSP_USING_PWM9_CH2
#endif

#if defined(BSP_USING_TIM10_CH1) && !defined(BSP_USING_PWM10_CH1)
#define BSP_USING_PWM10_CH1
#endif

#if defined(BSP_USING_TIM11_CH1) && !defined(BSP_USING_PWM11_CH1)
#define BSP_USING_PWM11_CH1
#endif

#if defined(BSP_USING_TIM12_CH1) && !defined(BSP_USING_PWM12_CH1)
#define BSP_USING_PWM12_CH1
#endif
#if defined(BSP_USING_TIM12_CH2) && !defined(BSP_USING_PWM12_CH2)
#define BSP_USING_PWM12_CH2
#endif

#if defined(BSP_USING_TIM13_CH1) && !defined(BSP_USING_PWM13_CH1)
#define BSP_USING_PWM13_CH1
#endif

#if defined(BSP_USING_TIM14_CH1) && !defined(BSP_USING_PWM14_CH1)
#define BSP_USING_PWM14_CH1
#endif

#if defined(BSP_USING_PWM1_CH1) || defined(BSP_USING_PWM1_CH2) || defined(BSP_USING_PWM1_CH3) || \
    defined(BSP_USING_PWM1_CH4)
#define BSP_USING_PWM1
#endif

#if defined(BSP_USING_PWM2_CH1) || defined(BSP_USING_PWM2_CH2) || defined(BSP_USING_PWM2_CH3) || \
    defined(BSP_USING_PWM2_CH4)
#define BSP_USING_PWM2
#endif

#if defined(BSP_USING_PWM3_CH1) || defined(BSP_USING_PWM3_CH2) || defined(BSP_USING_PWM3_CH3) || \
    defined(BSP_USING_PWM3_CH4)
#define BSP_USING_PWM3
#endif

#if defined(BSP_USING_PWM4_CH1) || defined(BSP_USING_PWM4_CH2) || defined(BSP_USING_PWM4_CH3) || \
    defined(BSP_USING_PWM4_CH4)
#define BSP_USING_PWM4
#endif

#if defined(BSP_USING_PWM8_CH1) || defined(BSP_USING_PWM8_CH2) || defined(BSP_USING_PWM8_CH3) || \
    defined(BSP_USING_PWM8_CH4)
#define BSP_USING_PWM8
#endif

#if defined(BSP_USING_PWM9_CH1) || defined(BSP_USING_PWM9_CH2)
#define BSP_USING_PWM9
#endif

#if defined(BSP_USING_PWM10_CH1)
#define BSP_USING_PWM10
#endif

#if defined(BSP_USING_PWM11_CH1)
#define BSP_USING_PWM11
#endif

#if defined(BSP_USING_PWM12_CH1) || defined(BSP_USING_PWM12_CH2)
#define BSP_USING_PWM12
#endif

#if defined(BSP_USING_PWM13_CH1)
#define BSP_USING_PWM13
#endif

#if defined(BSP_USING_PWM14_CH1)
#define BSP_USING_PWM14
#endif

#if defined(BSP_USING_PWM1) || defined(BSP_USING_PWM2) || defined(BSP_USING_PWM3) ||    \
    defined(BSP_USING_PWM4) || defined(BSP_USING_PWM8) || defined(BSP_USING_PWM9) ||    \
    defined(BSP_USING_PWM10) || defined(BSP_USING_PWM11) || defined(BSP_USING_PWM12) || \
    defined(BSP_USING_PWM13) || defined(BSP_USING_PWM14)
#define BSP_USING_PWM
#endif

/* SPI ----------------------------------------------------------------------------------------- */

// #define BSP_USING_SPI
// #define BSP_USING_SPI1
// #define BSP_SPI1_RX_USING_DMA
// #define BSP_SPI1_TX_USING_DMA
// #define BSP_USING_SPI2
// #define BSP_SPI2_RX_USING_DMA
// #define BSP_SPI2_TX_USING_DMA
// #define BSP_USING_SPI3
// #define BSP_SPI3_RX_USING_DMA
// #define BSP_SPI3_TX_USING_DMA

/* UART ---------------------------------------------------------------------------------------- */

#define BSP_USING_UART
#define BSP_USING_UART1
// #define BSP_USING_UART2
// #define BSP_USING_UART3
// #define BSP_USING_UART4
// #define BSP_USING_UART5
// #define BSP_USING_UART6
#define BSP_USING_UART7
// #define BSP_USING_UART8

#define TM_UART_USING_DMA

#define BSP_UART1_RX_USING_DMA
#define BSP_UART1_TX_USING_DMA
// #define BSP_UART2_RX_USING_DMA
// #define BSP_UART2_TX_USING_DMA
// #define BSP_UART3_RX_USING_DMA
// #define BSP_UART3_TX_USING_DMA
// #define BSP_UART4_RX_USING_DMA
// #define BSP_UART4_TX_USING_DMA
// #define BSP_UART5_RX_USING_DMA
// #define BSP_UART5_TX_USING_DMA
// #define BSP_UART6_RX_USING_DMA
// #define BSP_UART6_TX_USING_DMA
#define BSP_UART7_RX_USING_DMA
#define BSP_UART7_TX_USING_DMA
// #define BSP_UART8_RX_USING_DMA
// #define BSP_UART8_TX_USING_DMA

/*
 * `uart1` is the board-default runtime console / validation transport.
 * Keep its RX side wider than the generic standard profile so periodic ADC
 * telemetry, echo traffic, and host-side burst capture do not fight for the
 * same narrow FIFO budget. Tiny still trims this path because release gating
 * now proves the smaller console buffers on the real COM54 board.
 */
#if TM_DEVDRV_PROFILE_TINY
#define BSP_UART1_RX_BUFSIZE       (64U)
#define BSP_UART1_DMA_PING_BUFSIZE (32U)
#else
#define BSP_UART1_RX_BUFSIZE       (128U)
#define BSP_UART1_DMA_PING_BUFSIZE (64U)
#endif
#define BSP_UART1_TX_BUFSIZE TM_UART_PROFILE_TX_BUFSIZE

#if (TM_UART1_RX_BUFSIZE_OVERRIDE > 0U)
#undef BSP_UART1_RX_BUFSIZE
#define BSP_UART1_RX_BUFSIZE TM_UART1_RX_BUFSIZE_OVERRIDE
#endif

#if (TM_UART1_TX_BUFSIZE_OVERRIDE > 0U)
#undef BSP_UART1_TX_BUFSIZE
#define BSP_UART1_TX_BUFSIZE TM_UART1_TX_BUFSIZE_OVERRIDE
#endif

#if (TM_UART1_DMA_PING_BUFSIZE_OVERRIDE > 0U)
#undef BSP_UART1_DMA_PING_BUFSIZE
#define BSP_UART1_DMA_PING_BUFSIZE TM_UART1_DMA_PING_BUFSIZE_OVERRIDE
#endif

#define BSP_UART2_RX_BUFSIZE       TM_UART_PROFILE_RX_BUFSIZE
#define BSP_UART2_TX_BUFSIZE       TM_UART_PROFILE_TX_BUFSIZE
#define BSP_UART2_DMA_PING_BUFSIZE TM_UART_PROFILE_DMA_PING_BUFSIZE

#define BSP_UART3_RX_BUFSIZE       TM_UART_PROFILE_RX_BUFSIZE
#define BSP_UART3_TX_BUFSIZE       TM_UART_PROFILE_TX_BUFSIZE
#define BSP_UART3_DMA_PING_BUFSIZE TM_UART_PROFILE_DMA_PING_BUFSIZE

#define BSP_UART4_RX_BUFSIZE       TM_UART_PROFILE_RX_BUFSIZE
#define BSP_UART4_TX_BUFSIZE       TM_UART_PROFILE_TX_BUFSIZE
#define BSP_UART4_DMA_PING_BUFSIZE TM_UART_PROFILE_DMA_PING_BUFSIZE

#define BSP_UART5_RX_BUFSIZE       TM_UART_PROFILE_RX_BUFSIZE
#define BSP_UART5_TX_BUFSIZE       TM_UART_PROFILE_TX_BUFSIZE
#define BSP_UART5_DMA_PING_BUFSIZE TM_UART_PROFILE_DMA_PING_BUFSIZE

#define BSP_UART6_RX_BUFSIZE       TM_UART_PROFILE_RX_BUFSIZE
#define BSP_UART6_TX_BUFSIZE       TM_UART_PROFILE_TX_BUFSIZE
#define BSP_UART6_DMA_PING_BUFSIZE TM_UART_PROFILE_DMA_PING_BUFSIZE

#define BSP_UART7_RX_BUFSIZE       TM_UART_PROFILE_RX_BUFSIZE
#define BSP_UART7_TX_BUFSIZE       TM_UART_PROFILE_TX_BUFSIZE
#define BSP_UART7_DMA_PING_BUFSIZE TM_UART_PROFILE_DMA_PING_BUFSIZE

#define BSP_UART8_RX_BUFSIZE       TM_UART_PROFILE_RX_BUFSIZE
#define BSP_UART8_TX_BUFSIZE       TM_UART_PROFILE_TX_BUFSIZE
#define BSP_UART8_DMA_PING_BUFSIZE TM_UART_PROFILE_DMA_PING_BUFSIZE

/* WDT ----------------------------------------------------------------------------------------- */

#define BSP_USING_WDT

/* Flash --------------------------------------------------------------------------------------- */

#define BSP_USING_FLASH
#define BSP_USING_FLASH_ONCHIP

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
