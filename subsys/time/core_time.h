/**
 * @file    core_time.h
 * @brief   Themis core-time services and zero-overhead performance probes
 * @version v1.0.0
 * @date    2026.03.02
 *
 * Design overview
 * ---------------
 * `core_time` exposes two architecture-transparent services behind one API:
 * - blocking delay helpers: `core_time_delay_us()` / `core_time_delay_ms()`
 * - zero-overhead probes: `core_time_perf_start()` / `core_time_perf_stop()`
 *
 * Backend selection
 * -----------------
 * - Cortex-M3 / M4 / M7 / M33 class targets use DWT `CYCCNT`
 * - Cortex-M0 / M0+ / M23 baseline targets use SysTick `CVR`
 *
 * Callers never need to know which backend is active. Both backends keep the
 * same public function names and semantics.
 *
 * Register model
 * --------------
 * DWT backend:
 * - `DEMCR` bit 24 `TRCENA` enables the CoreSight trace block
 * - `DWT_CTRL` bit 0 `CYCCNTENA` enables the free-running cycle counter
 *
 * SysTick backend:
 * - `SYST_CSR` is the control/status register
 * - `SYST_RVR` is the 24-bit reload register
 * - `SYST_CVR` is the 24-bit down-counting current-value register
 *
 * Overflow / wrap rules
 * ---------------------
 * - DWT elapsed-time math uses unsigned 32-bit subtraction and stays correct
 *   across wrap as long as the measured interval is shorter than one full
 *   counter period.
 * - SysTick elapsed-time math compensates for the 24-bit down-counter reload
 *   path on every measurement.
 */

#pragma once

/* Includes --------------------------------------------------------------------- */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tmdef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros ----------------------------------------------------------------- */

#if defined(__ARM_ARCH_6M__) || defined(__ARM_ARCH_8M_BASE__)
#define CORE_TIME_USE_SYSTICK 1
#else
#undef CORE_TIME_USE_SYSTICK
#endif

/** @brief DEMCR: Debug and Exception Monitor Control. Bit 24 = TRCENA. */
#if !defined(CORE_TIME_DEMCR)
#define CORE_TIME_DEMCR (*((volatile uint32_t *)0xE000EDFCU))
#endif

/** @brief DWT Control Register. Bit 0 = CYCCNTENA. */
#if !defined(CORE_TIME_DWT_CTRL)
#define CORE_TIME_DWT_CTRL (*((volatile uint32_t *)0xE0001000U))
#endif

/** @brief DWT cycle-count register: 32-bit free-running up-counter. */
#if !defined(CORE_TIME_DWT_CYCCNT)
#define CORE_TIME_DWT_CYCCNT (*((volatile uint32_t *)0xE0001004U))
#endif

/** @brief DEMCR bit 24: master CoreSight trace enable. */
#if !defined(CORE_TIME_TRCENA)
#define CORE_TIME_TRCENA TM_BIT(24U)
#endif

/** @brief DWT_CTRL bit 0: cycle counter enable. */
#if !defined(CORE_TIME_CYCCNTENA)
#define CORE_TIME_CYCCNTENA TM_BIT(0U)
#endif

/** @brief ICSR active exception bits. Zero means thread context. */
#if !defined(CORE_TIME_VECTACTIVE_MASK)
#define CORE_TIME_VECTACTIVE_MASK (0x000001FFUL)
#endif

/** @brief Interrupt Control and State Register. */
#if !defined(CORE_TIME_ICSR)
#define CORE_TIME_ICSR (*((volatile uint32_t *)0xE000ED04U))
#endif

/** @brief SysTick Control and Status Register. */
#if !defined(CORE_TIME_SYST_CSR)
#define CORE_TIME_SYST_CSR (*((volatile uint32_t *)0xE000E010U))
#endif

/** @brief SysTick enable bit in SYST_CSR. */
#if !defined(CORE_TIME_SYST_CSR_ENABLE)
#define CORE_TIME_SYST_CSR_ENABLE TM_BIT(0U)
#endif

/** @brief SysTick processor clock select bit in SYST_CSR. */
#if !defined(CORE_TIME_SYST_CSR_CLKSOURCE)
#define CORE_TIME_SYST_CSR_CLKSOURCE TM_BIT(2U)
#endif

/** @brief SysTick reload-value register (24-bit). */
#if !defined(CORE_TIME_SYST_RVR)
#define CORE_TIME_SYST_RVR (*((volatile uint32_t *)0xE000E014U))
#endif

/** @brief SysTick current-value register (24-bit, counts down). */
#if !defined(CORE_TIME_SYST_CVR)
#define CORE_TIME_SYST_CVR (*((volatile uint32_t *)0xE000E018U))
#endif

/** @brief Mask for 24-bit SysTick fields. */
#if !defined(CORE_TIME_SYST_MASK)
#define CORE_TIME_SYST_MASK (0x00FFFFFFU)
#endif

/** @brief Minimum supported CPU clock for microsecond-delay math. */
#if !defined(CORE_TIME_MIN_CPU_FREQ_HZ)
#define CORE_TIME_MIN_CPU_FREQ_HZ (1000000U)
#endif

/* Public Types ------------------------------------------------------------------ */

/**
 * @brief  Performance probe context for one measured code region.
 *
 * @code
 * static struct core_time_perf g_foc_perf;
 *
 * core_time_perf_start(&g_foc_perf);
 * foc_run(motor);
 * core_time_perf_stop(&g_foc_perf);
 *
 * float us = core_time_perf_get_us(&g_foc_perf);
 * @endcode
 */
struct core_time_perf {
    /** Counter snapshot captured at `core_time_perf_start()`. */
    uint32_t start_cycles;
    /** Elapsed ticks recorded at `core_time_perf_stop()`. */
    uint32_t delta_cycles;
};

/* Public Inline Helpers --------------------------------------------------------- */

#if defined(CORE_TIME_USE_SYSTICK)

/**
 * @brief  Capture one start timestamp from SysTick CVR.
 *
 * @param[in,out] ctx  Non-NULL probe context.
 */
static inline void core_time_perf_start(struct core_time_perf *ctx)
{
    ctx->start_cycles = CORE_TIME_SYST_CVR & CORE_TIME_SYST_MASK;
}

/**
 * @brief  Latch elapsed ticks with SysTick reload compensation.
 *
 * @details
 * SysTick counts down. If `start >= now`, no reload happened between the two
 * samples and the delta is `start - now`. Otherwise the counter reloaded once
 * and the delta becomes `start + RVR - now`.
 *
 * @param[in,out] ctx  Non-NULL probe context passed to `core_time_perf_start()`.
 */
static inline void core_time_perf_stop(struct core_time_perf *ctx)
{
    uint32_t now   = CORE_TIME_SYST_CVR & CORE_TIME_SYST_MASK;
    uint32_t start = ctx->start_cycles;

    if (start >= now) {
        ctx->delta_cycles = start - now;
    } else {
        ctx->delta_cycles = start + (CORE_TIME_SYST_RVR & CORE_TIME_SYST_MASK) - now;
    }
}

#else

/**
 * @brief  Capture one start timestamp from DWT CYCCNT.
 *
 * @param[in,out] ctx  Non-NULL probe context.
 */
static inline void core_time_perf_start(struct core_time_perf *ctx)
{
    ctx->start_cycles = CORE_TIME_DWT_CYCCNT;
}

/**
 * @brief  Latch elapsed cycles using unsigned modular subtraction.
 *
 * @details
 * Unsigned subtraction keeps the elapsed-cycle result correct across one
 * counter wrap, provided the measured interval is shorter than one full
 * 32-bit counter period.
 *
 * @param[in,out] ctx  Non-NULL probe context passed to `core_time_perf_start()`.
 */
static inline void core_time_perf_stop(struct core_time_perf *ctx)
{
    ctx->delta_cycles = CORE_TIME_DWT_CYCCNT - ctx->start_cycles;
}

#endif /* CORE_TIME_USE_SYSTICK */

/* Public Functions ------------------------------------------------------------- */

/**
 * @brief  Initialize the core-time subsystem.
 *
 * @details
 * Stores `cpu_freq_hz` and `cpu_freq_mhz` in module-private globals.
 *
 * DWT mode:
 * - sets `DEMCR.TRCENA` and `DWT_CTRL.CYCCNTENA` via idempotent `|=`
 * - intentionally does not reset `DWT_CYCCNT`
 *
 * SysTick mode:
 * - does nothing if SysTick is already running
 * - otherwise configures a free-running 24-bit counter at full CPU clock
 *
 * @param[in] cpu_freq_hz  CPU core clock in hertz. Must be greater than zero.
 *
 * @retval 0         Initialization succeeded.
 * @retval -EINVAL   `cpu_freq_hz` is zero.
 * @retval -ERANGE   `cpu_freq_hz` is below 1 MHz and cannot support
 *                   `core_time_delay_us()`.
 */
int core_time_init(uint32_t cpu_freq_hz);

/**
 * @brief  Busy-wait for the requested microsecond duration.
 *
 * @details
 * DWT mode spins on `(DWT_CYCCNT - start) < us * freq_mhz`.
 * SysTick mode uses cumulative elapsed counting with reload compensation so
 * 24-bit counter wrap stays correct across longer waits.
 *
 * Safe to call from ISR context or before the RTOS scheduler starts.
 *
 * @param[in] us  Delay duration in microseconds.
 */
void core_time_delay_us(uint32_t us);

/**
 * @brief  Delay for the requested millisecond duration with RTOS awareness.
 *
 * @details
 * When the FreeRTOS scheduler is running in thread context, the helper yields
 * with `vTaskDelay()` in bounded chunks. Otherwise it falls back to bounded
 * microsecond spin waits for pre-scheduler or ISR-safe operation.
 *
 * @param[in] ms  Delay duration in milliseconds.
 */
void core_time_delay_ms(uint32_t ms);

/**
 * @brief  Convert one probe measurement to microseconds.
 *
 * @details
 * Uses the module-private CPU-frequency calibration stored by
 * `core_time_init()`. This helper performs floating-point division and is
 * intended for non-real-time diagnostics or logging paths.
 *
 * @param[in] ctx  Non-NULL measured probe context.
 *
 * @return Elapsed time in microseconds, or `0.0f` when `ctx` is NULL.
 */
float core_time_perf_get_us(const struct core_time_perf *ctx);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
