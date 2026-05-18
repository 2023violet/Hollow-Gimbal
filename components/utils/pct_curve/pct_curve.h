/**
 * @file    pct_curve.h
 * @brief   Portable piecewise percent-curve helpers
 * @version v1.0.0
 * @date    2026.04.23
 *
 * Copyright (c) 2026 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 *
 * Design overview
 * ---------------
 * pct_curve maps one logical percent through a caller-owned monotonic anchor
 * table and provides one companion helper for scaling permille duty output.
 *
 * The helper does not own anchor storage and does not know anything about a
 * product's audio path, display path, or RTOS cadence. The caller supplies the
 * anchor table that expresses its own policy, while pct_curve only performs
 * deterministic piecewise interpolation.
 *
 * Usage example
 * -------------
 * @code
 * static const struct pct_curve_anchor anchors[] = {
 *     {0U, 0U},
 *     {50U, 35U},
 *     {100U, 100U},
 * };
 *
 * uint8_t effective = pct_curve_interpolate(anchors, 3U, logical_percent);
 * uint16_t duty_pm = pct_curve_scale_permille(raw_duty_pm, effective);
 * @endcode
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - The caller owns the anchor array storage.
 * - The helper retains no references after each call returns.
 * - All APIs are pure calculations over the given arguments.
 * - Re-entrant, thread-safe, and ISR-safe as long as the caller provides
 *   immutable inputs.
 *
 * Blocking / allocation
 * --------------------
 * - All APIs are bounded and non-blocking.
 * - Dynamic allocation: none.
 * - Hidden state: none.
 *
 * Failure / recovery
 * ------------------
 * - Empty or NULL anchor tables fall back to a clamped passthrough mapping.
 * - Percent domains are clamped to 0..100 before interpolation/scaling.
 * - pct_curve_scale_permille() preserves a non-zero output when both the duty
 *   and effective percent are non-zero but integer truncation would round to 0.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public types --------------------------------------------------------------------------------- */

/**
 * @brief  One monotonic percent-mapping anchor.
 *
 * input_percent and output_percent are both expressed in the closed range
 * 0..100. Callers should keep the table monotonic in both domains.
 */
struct pct_curve_anchor {
    uint8_t input_percent;  /* Logical-domain percent.   */
    uint8_t output_percent; /* Effective-domain percent. */
};

/* Public function declarations ----------------------------------------------------------------- */

/**
 * @brief  Map one logical percent through a piecewise-linear anchor table.
 *
 * The helper clamps @p logical_percent into 0..100, then finds the anchor
 * segment that contains it and linearly interpolates the output percent with
 * integer rounding-to-nearest.
 *
 * @param  anchors           Caller-owned anchor table. May be NULL when
 *                           @p anchor_count is 0.
 * @param  anchor_count      Number of entries in @p anchors.
 * @param  logical_percent   Input percent in the logical domain.
 *
 * @return Effective percent in the range 0..100.
 */
uint8_t pct_curve_interpolate(const struct pct_curve_anchor *anchors,
                              size_t                         anchor_count,
                              uint8_t                        logical_percent);

/**
 * @brief  Scale a permille duty value by an effective percent.
 *
 * @param  duty_permille              Raw duty in permille units.
 * @param  effective_volume_percent   Effective percent in the range 0..100.
 *                                    Values above 100 are clamped.
 *
 * @return Scaled duty in permille units.
 */
uint16_t pct_curve_scale_permille(uint16_t duty_permille, uint8_t effective_volume_percent);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
