/**
 * @file    bsp_config.h
 * @brief   Board facts for the Themis-aligned Falcon FOC AT32 image
 * @version v1.0.0
 * @date    2026.05.11
 *
 * Design overview
 * ---------------
 * This header is the single compile-time owner of board-level facts for the
 * current Falcon FOC target. Unlike the full Themis runtime profile, this FOC
 * product does not assemble the `device/` and `drivers/` abstraction layers,
 * so this header intentionally stays small and only records board truth that is
 * still useful to product-owned code and compatibility shims.
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - Board facts belong here instead of being scattered through APP/task code.
 * - Runtime object creation, ISR wiring, and peripheral register setup remain
 *   owned by `bsp/board/` and `platform/at/at32f415/`.
 * - If the board routing changes, update these facts in the same change rather
 *   than introducing a second source of truth.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "config/tm_core_config.h"

/* Board Identity ------------------------------------------------------------------------------- */

/** Human-readable board identifier used by diagnostics and build metadata. */
#define BOARD_NAME                "falcon-foc-at32"

/* Board Default Resource Names ----------------------------------------------------------------- */

/** Default UART resource name used by the console-first shell transport path. */
#define BOARD_CONSOLE_UART_NAME   "uart1"
/** Default telemetry UART resource name when diagnostics explicitly opt in. */
#define BOARD_TELEMETRY_UART_NAME BOARD_CONSOLE_UART_NAME
/** Default ADC resource name used by the current motor-sense path. */
#define BOARD_MAIN_ADC_NAME       "adc1"
/** Default SPI resource name used by the current driver-side gate path. */
#define BOARD_MAIN_SPI_NAME       "spi1"
/** Default auxiliary SPI resource name used by the current encoder-side path. */
#define BOARD_AUX_SPI_NAME        "spi3"

/* ---------------------------------------- end of file ----------------------------------------- */
