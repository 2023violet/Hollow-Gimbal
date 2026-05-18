/**
 * @file    stat_welford.c
 * @brief   Numerically stable streaming mean and variance implementation
 * @version v1.0.0
 * @date    2026.04.24
 *
 * Implementation role
 * -------------------
 * - pairs with `stat_welford.h`
 * - owns the two-delta Welford update and low-sample clamping policy
 * - keeps all invariants local to one caller-owned statistics context
 *
 * Key collaborators
 * -----------------
 * - `stat_welford.h`: public lifecycle and snapshot contract
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "stat_welford.h"
#include <errno.h>
#include <math.h>

/* Public Functions ----------------------------------------------------------------------------- */

int stat_welford_init(struct stat_welford *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }

    ctx->count = 0U;
    ctx->mean  = 0.0f;
    ctx->m2    = 0.0f;

    return 0;
}

int stat_welford_push(struct stat_welford *ctx, float value)
{
    float delta;
    float delta2;

    if (ctx == NULL) {
        return -EINVAL;
    }
    if (!isfinite(value)) {
        return -EINVAL;
    }
    if (ctx->count == UINT32_MAX) {
        return -EOVERFLOW;
    }

    ctx->count++;

    /*
     * Welford two-delta update:
     * 1. Measure deviation from the old mean.
     * 2. Move the mean toward the new sample by delta / N.
     * 3. Measure deviation from the new mean.
     * 4. Accumulate delta * delta2 into M2.
     *
     * This avoids the catastrophic cancellation that appears in the naive
     * sum-of-squares formulation when the signal mean is much larger than its
     * spread.
     */
    delta = value - ctx->mean;
    ctx->mean += delta / (float)ctx->count;
    delta2 = value - ctx->mean;

    ctx->m2 += delta * delta2;

    return 0;
}

float stat_welford_mean(const struct stat_welford *ctx)
{
    if (ctx == NULL || ctx->count == 0U) {
        return 0.0f;
    }

    return ctx->mean;
}

float stat_welford_var(const struct stat_welford *ctx)
{
    float var;

    if (ctx == NULL || ctx->count < 2U) {
        /*
         * Sample variance is undefined below two samples because the unbiased
         * form divides by (count - 1). Returning 0.0f gives callers a stable
         * sentinel and avoids propagating NaN into downstream math.
         */
        return 0.0f;
    }

    /*
     * Bessel-corrected sample variance.
     *
     * The final clamp guards against rare negative sub-ULP drift from floating
     * point rounding so sqrtf() never sees a negative input.
     */
    var = ctx->m2 / (float)(ctx->count - 1U);
    return (var >= 0.0f) ? var : 0.0f;
}

float stat_welford_stddev(const struct stat_welford *ctx)
{
    /* stat_welford_var() guarantees a finite non-negative input for sqrtf(). */
    return sqrtf(stat_welford_var(ctx));
}

/* ---------------------------------------- end of file ----------------------------------------- */
