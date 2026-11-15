/**
 * @file    tm_platform_foc_timing.h
 * @brief   AT32F415 timer-clock and peripheral-cadence facts.
 * @version v0.1.0
 * @date    2026.11.15
 *
 * Ownership
 * ---------
 * Keep facts determined by the AT32F415 clock tree and generated timer setup
 * in the platform layer. Product control cadence remains application-owned.
 */
#pragma once

/* Public Macros ---------------------------------------------------------------- */

/** Prescaled timer input after the AT32F415 APB timer multiplier, in Hz. */
#define TM_PLATFORM_TMR_CLK_HZ (150000000U)
/** Same timer input expressed in MHz. */
#define TM_PLATFORM_TMR_CLK_MHZ (150U)
/** Legacy generated TMR1 center-aligned carrier/ADC event rate, in Hz. */
#define TM_PLATFORM_LEGACY_TMR1_CARRIER_HZ (20000U)
/** Legacy generated TMR5 motor soft-IRQ rate, in Hz. */
#define TM_PLATFORM_LEGACY_TMR5_SOFTIRQ_HZ (5000U)
/** V2 TMR2/TMR5 timestamp resolution, in ticks per second. */
#define TM_PLATFORM_V2_TIMEBASE_HZ (1000000U)

/* ---------------------------------------- end of file ----------------------------------------- */
