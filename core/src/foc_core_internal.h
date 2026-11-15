/**
 * @file    foc_core_internal.h
 * @brief   Private Core helpers shared by the Float32 control translation units.
 * @version v0.1.0
 * @date    2026.11.13
 *
 * Internal role
 * -------------
 * - carries one validated electrical angle and its sin/cos pair through a
 *   single current-control cycle
 * - exposes the trusted SVPWM entry used only after copied configuration was
 *   validated at initialization
 * - is not a public integration header and must not be included by callers
 *
 * Design overview
 * ---------------
 * The cache is created at a control boundary and borrowed by trusted transform
 * helpers until that one calculation completes.
 *
 * Usage example
 * -------------
 * Prepare one cache, pass it to the cached Park helpers, then discard it after
 * the current control step. Public Core APIs do not expose this type.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The caller owns the cache storage and serializes access to one controller
 * instance. It is an internal stack/value object and holds no external pointer.
 *
 * Failure / recovery
 * ------------------
 * Preparation rejects non-finite angles. Trusted SVPWM still rejects dynamic
 * VBUS, vector, duty, and non-finite errors; static configuration validation is
 * the responsibility of the initialization/public boundary.
 */

/* Includes --------------------------------------------------------------------- */

#pragma once

#include "foc_core.h"

#include <math.h>

/* Private Types --------------------------------------------------------------- */

struct foc_angle_cache {
    float angle_rad;
    float sine;
    float cosine;
};

/* Public Types --------------------------------------------------------------- */

/* Internal types are exposed only to sibling Core translation units. */

/* Private Helpers -------------------------------------------------------------- */

static inline int foc_angle_cache_prepare(float angle_rad, struct foc_angle_cache *cache)
{
    const float two_pi = 6.28318530717958647692f;
    float wrapped;

    if (cache == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    *cache = (struct foc_angle_cache){0};
    if (!isfinite(angle_rad)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    wrapped = fmodf(angle_rad, two_pi);
    if (wrapped < 0.0f) {
        wrapped += two_pi;
    }
    cache->angle_rad = wrapped >= two_pi ? 0.0f : wrapped;
    cache->sine = sinf(cache->angle_rad);
    cache->cosine = cosf(cache->angle_rad);
    if (!isfinite(cache->sine) || !isfinite(cache->cosine)) {
        *cache = (struct foc_angle_cache){0};
        return FOC_STATUS_ERR_NONFINITE;
    }
    return FOC_STATUS_OK;
}

static inline int foc_park_cached(const struct foc_vector_alpha_beta *input,
                                  const struct foc_angle_cache *cache,
                                  struct foc_vector_dq *output)
{
    struct foc_vector_dq result;

    if (input == NULL || cache == NULL || output == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    if (!isfinite(input->alpha) || !isfinite(input->beta) || !isfinite(cache->sine) ||
        !isfinite(cache->cosine)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    result.d = input->alpha * cache->cosine + input->beta * cache->sine;
    result.q = input->beta * cache->cosine - input->alpha * cache->sine;
    if (!isfinite(result.d) || !isfinite(result.q)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    *output = result;
    return FOC_STATUS_OK;
}

static inline int foc_inverse_park_cached(const struct foc_vector_dq *input,
                                          const struct foc_angle_cache *cache,
                                          struct foc_vector_alpha_beta *output)
{
    struct foc_vector_alpha_beta result;

    if (input == NULL || cache == NULL || output == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    if (!isfinite(input->d) || !isfinite(input->q) || !isfinite(cache->sine) ||
        !isfinite(cache->cosine)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    result.alpha = input->d * cache->cosine - input->q * cache->sine;
    result.beta = input->d * cache->sine + input->q * cache->cosine;
    if (!isfinite(result.alpha) || !isfinite(result.beta)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    *output = result;
    return FOC_STATUS_OK;
}

/* Trusted Core entry ---------------------------------------------------------- */

/**
 * Calculate SVPWM after the caller has validated the immutable configuration.
 * Dynamic VBUS, vector, duty, and finite-value checks remain inside the entry.
 */
int foc_svpwm_trusted(const struct foc_vector_alpha_beta *input,
                      float vbus_v,
                      const struct foc_config *config,
                      struct foc_svpwm_output *output);

/* Public Functions ------------------------------------------------------------- */

/* Internal sibling entry declared above; no application code should call it. */

/* ---------------------------------------- end of file ----------------------------------------- */
