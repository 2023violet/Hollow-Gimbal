/**
 * @file    core_time.c
 * @brief   Themis core-time implementation
 * @version v1.0.0
 * @date    2026.03.02
 *
 * Implementation role
 * -------------------
 * This translation unit stores CPU-frequency calibration state and owns the
 * backend-specific delay math behind the public `core_time.h` contract.
 *
 * Key collaborators
 * -----------------
 * - `core_time.h`: shared time/probe contract
 * - FreeRTOS `task.h`: scheduler-state query and `vTaskDelay()` fallback path
 *
 * Delay math notes
 * ----------------
 * DWT backend:
 * - uses a 32-bit up-counter and unsigned modular subtraction
 * - remains wrap-safe as long as one delay chunk stays within one full counter
 *   period
 *
 * SysTick backend:
 * - uses a 24-bit down-counter
 * - accumulates elapsed ticks across polling steps and compensates for reload
 *   events at each step
 */

/* Includes --------------------------------------------------------------------- */
#include "core_time.h"
#include "FreeRTOS.h"
#include "task.h"

/* Private State ----------------------------------------------------------------- */

/** CPU frequency in hertz, written once by `core_time_init()`. */
static uint32_t s_cpu_freq_hz = 0U;

/** CPU frequency in megahertz for the fast `us * freq_mhz` hot path. */
static uint32_t s_cpu_freq_mhz = 0U;

/* Private Helpers --------------------------------------------------------------- */

static inline bool _core_time_is_in_isr(void)
{
    return ((CORE_TIME_ICSR & CORE_TIME_VECTACTIVE_MASK) != 0U);
}

static void _core_time_delay_ticks(uint64_t total_ticks)
{
    while (total_ticks != 0ULL) {
        uint32_t ticks = (total_ticks > (uint64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)total_ticks;

#if !defined(CORE_TIME_USE_SYSTICK)
        uint32_t start = CORE_TIME_DWT_CYCCNT;

        while ((CORE_TIME_DWT_CYCCNT - start) < ticks) {
            /* spin */
        }
#else
        uint32_t start   = CORE_TIME_SYST_CVR & CORE_TIME_SYST_MASK;
        uint32_t elapsed = 0U;

        while (elapsed < ticks) {
            uint32_t now = CORE_TIME_SYST_CVR & CORE_TIME_SYST_MASK;

            if (start >= now) {
                elapsed += start - now;
            } else {
                elapsed += start + (CORE_TIME_SYST_RVR & CORE_TIME_SYST_MASK) - now;
            }

            start = now;
        }
#endif

        total_ticks -= (uint64_t)ticks;
    }
}

/* Public Functions ------------------------------------------------------------- */

int core_time_init(uint32_t cpu_freq_hz)
{
    if (cpu_freq_hz == 0U) {
        return -EINVAL;
    }
    if (cpu_freq_hz < CORE_TIME_MIN_CPU_FREQ_HZ) {
        return -ERANGE;
    }

    s_cpu_freq_hz  = cpu_freq_hz;
    s_cpu_freq_mhz = cpu_freq_hz / 1000000U;

#if !defined(CORE_TIME_USE_SYSTICK)
    /*
     * DWT backend for Cortex-M3 / M4 / M7 / M33:
     * - enable CoreSight tracing first
     * - then enable the cycle counter
     * - do not reset CYCCNT because other in-flight probes may rely on the
     *   shared monotonic time base
     */
    CORE_TIME_DEMCR |= CORE_TIME_TRCENA;
    CORE_TIME_DWT_CTRL |= CORE_TIME_CYCCNTENA;
#else
    /*
     * SysTick backend for Cortex-M0 / M0+ / M23:
     * - if SysTick is already running, leave the existing owner untouched
     * - otherwise configure a free-running 24-bit counter without enabling the
     *   interrupt path
     */
    if ((CORE_TIME_SYST_CSR & CORE_TIME_SYST_CSR_ENABLE) == 0U) {
        CORE_TIME_SYST_RVR = CORE_TIME_SYST_MASK;
        CORE_TIME_SYST_CVR = 0U;
        CORE_TIME_SYST_CSR = CORE_TIME_SYST_CSR_CLKSOURCE | CORE_TIME_SYST_CSR_ENABLE;
    }
#endif

    return 0;
}

void core_time_delay_us(uint32_t us)
{
    if ((us == 0U) || (s_cpu_freq_mhz == 0U)) {
        return;
    }

    _core_time_delay_ticks((uint64_t)us * (uint64_t)s_cpu_freq_mhz);
}

void core_time_delay_ms(uint32_t ms)
{
    if (ms == 0U) {
        return;
    }

    /*
     * Task context uses `vTaskDelay()` once the scheduler is running so we do
     * not burn CPU in a long spin wait. ISR or pre-scheduler paths stay on the
     * spin-delay fallback because `vTaskDelay()` is illegal there.
     */
    if (!_core_time_is_in_isr() && (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)) {
        while (ms != 0U) {
            uint32_t chunk_ms = (ms > 60000U) ? 60000U : ms;

            vTaskDelay(pdMS_TO_TICKS(chunk_ms));
            ms -= chunk_ms;
        }
        return;
    }

    while (ms != 0U) {
        uint32_t chunk_ms = (ms > (UINT32_MAX / 1000U)) ? (UINT32_MAX / 1000U) : ms;

        core_time_delay_us(chunk_ms * 1000U);
        ms -= chunk_ms;
    }
}

float core_time_perf_get_us(const struct core_time_perf *ctx)
{
    if ((ctx == NULL) || (s_cpu_freq_hz == 0U)) {
        return 0.0f;
    }

    /*
     * Cast to float before multiply so the intermediate product does not
     * overflow a 32-bit integer.
     */
    return ((float)ctx->delta_cycles * 1.0e6f) / (float)s_cpu_freq_hz;
}

/* ---------------------------------------- end of file ----------------------------------------- */
