/**
 * @file    tm_platform_foc_timing.h
 * @brief   AT32F415 timer-clock and timebase facts for Board1.
 * @version v0.2.0
 * @date    2026.11.24
 *
 * Design overview
 * ---------------
 * Facts fixed by the AT32F415 clock tree (timer input 150 MHz) and the V2
 * 1 MHz timestamp timers. Control cadence stays in foc_board.h.
 *
 * Usage example
 * -------------
 * foc_board.h derives FOC_BOARD_TIMER_HZ and FOC_BOARD_US_HZ from these macros.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Compile-time constants only; no state.
 *
 * Failure / recovery
 * ------------------
 * A clock-tree change must update these values together with main.c's clock setup.
 */
#pragma once

/* Public Types ---------------------------------------------------------------- */

/* None: compile-time constants only. */

/* Public Macros ---------------------------------------------------------------- */

/** Prescaled timer input after the AT32F415 APB timer multiplier, in Hz. */
#define TM_PLATFORM_TMR_CLK_HZ (150000000U)
/** Same timer input expressed in MHz. */
#define TM_PLATFORM_TMR_CLK_MHZ (150U)
/** V2 TMR2/TMR5 timestamp resolution, in ticks per second. */
#define TM_PLATFORM_V2_TIMEBASE_HZ (1000000U)

/* Public Functions ------------------------------------------------------------ */

/* None. */

/* ---------------------------------------- end of file ----------------------------------------- */
