/**
 * @file    stat_welford.h
 * @brief   Numerically stable online mean and variance accumulator
 * @version v1.0.0
 * @date    2026.04.24
 *
 * Design overview
 * ---------------
 * `stat_welford` implements Welford's online mean/variance algorithm so
 * streaming statistics remain numerically stable even when the signal mean is
 * much larger than its spread.
 *
 * @warning Contains floating-point division. Extremely slow on Cortex-M0
 *          without FPU. Evaluate fixed-point alternatives for bare-metal
 *          targets.
 *
 * Usage example
 * -------------
 * struct stat_welford cal = {0};
 *
 * stat_welford_init(&cal);
 * stat_welford_push(&cal, sample);
 * noise = stat_welford_stddev(&cal);
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * - The caller owns one `struct stat_welford` per independent sample stream.
 * - The component keeps no global mutable state and no external references.
 * - One execution context must own each instance unless callers serialize
 *   access explicitly.
 * - @note Not thread-safe for a shared mutable instance. Caller must ensure
 *   external serialization. Not ISR-safe.
 *
 * Blocking / allocation
 * ---------------------
 * - No blocking, no allocation, and no callback re-entry.
 * - Every update is O(1).
 *
 * Failure / recovery
 * ------------------
 * - Invalid contexts fail with `-EINVAL`.
 * - Variance and standard-deviation helpers clamp low-sample states to `0.0f`
 *   instead of propagating NaN.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types --------------------------------------------------------------------------------- */

/**
 * @brief  Welford online statistics context.
 *
 * All state is contained in this struct. Declare as many independent
 * instances as needed.
 */
struct stat_welford {
    /**
     * @brief  Number of samples accumulated so far.
     */
    uint32_t count;

    /**
     * @brief  Current running mean.
     */
    float mean;

    /**
     * @brief  Sum of squared deviations from the running mean.
     *
     * This is the `M2` term in Welford's recurrence.
     */
    float m2;
};

/* Public Functions ----------------------------------------------------------------------------- */

/* Lifecycle ------------------------------------------------------------------------------------ */

/**
 * @brief  Initialize or reset a statistics context.
 *
 * Sets `count = 0`, `mean = 0.0f`, and `m2 = 0.0f`.
 *
 * @param  ctx  Context to initialize. Must not be NULL.
 *
 * @retval 0        Initialization succeeded.
 * @retval -EINVAL  @p ctx is NULL.
 */
int stat_welford_init(struct stat_welford *ctx);

/* Runtime Update ------------------------------------------------------------------------------- */

/**
 * @brief  Push one sample into the running statistics.
 *
 * The update step is:
 * - `count += 1`
 * - `delta  = value - mean`
 * - `mean  += delta / count`
 * - `delta2 = value - mean`
 * - `m2    += delta * delta2`
 *
 * Tracking squared deviation around the running mean avoids the catastrophic
 * precision loss that appears in the naive sum-of-squares formula.
 *
 * @param  ctx    Initialized context. Must not be NULL.
 * @param  value  New sample value.
 *
 * @retval 0        Update succeeded.
 * @retval -EINVAL  @p ctx is NULL or @p value is not finite.
 * @retval -EOVERFLOW  Sample count already reached `UINT32_MAX`.
 */
int stat_welford_push(struct stat_welford *ctx, float value);

/* Snapshot Helpers ----------------------------------------------------------------------------- */

/**
 * @brief  Return the current sample mean.
 *
 * @param  ctx  Statistics context. Must not be NULL.
 *
 * @return Current mean, or `0.0f` when @p ctx is NULL or no sample has been
 *         pushed yet.
 */
float stat_welford_mean(const struct stat_welford *ctx);

/**
 * @brief  Return the unbiased sample variance.
 *
 * Uses Bessel's correction: `M2 / (count - 1)`.
 *
 * @param  ctx  Statistics context. Must not be NULL.
 *
 * @return Sample variance, or `0.0f` when @p ctx is NULL or fewer than two
 *         samples are available.
 */
float stat_welford_var(const struct stat_welford *ctx);

/**
 * @brief  Return the unbiased sample standard deviation.
 *
 * Equivalent to `sqrtf(stat_welford_var(ctx))`.
 *
 * @param  ctx  Statistics context. Must not be NULL.
 *
 * @return Sample standard deviation, or `0.0f` when @p ctx is NULL or fewer
 *         than two samples are available.
 */
float stat_welford_stddev(const struct stat_welford *ctx);

/* Inline Helpers ------------------------------------------------------------------------------- */

/**
 * @brief  Return the number of accumulated samples.
 *
 * @param  ctx  Statistics context. May be NULL.
 *
 * @return Sample count, or 0 when @p ctx is NULL.
 */
static inline uint32_t stat_welford_count(const struct stat_welford *ctx)
{
    return (ctx != NULL) ? ctx->count : 0U;
}

/**
 * @brief  Return whether at least @p n samples have been accumulated.
 *
 * @param  ctx  Statistics context. May be NULL.
 * @param  n    Required minimum sample count.
 *
 * @return `true` when the sample count is at least @p n; otherwise `false`.
 */
static inline bool stat_welford_ready(const struct stat_welford *ctx, uint32_t n)
{
    return (ctx != NULL) && (ctx->count >= n);
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
