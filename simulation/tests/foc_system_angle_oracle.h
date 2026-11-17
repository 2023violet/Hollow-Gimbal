/**
 * @file    foc_system_angle_oracle.h
 * @brief   Test-only double kinematic, quantization and speed references.
 * @version v0.1.0
 * @date    2026.11.17
 *
 * Design overview
 * ---------------
 * Independent analytic truth for prescribed constant-acceleration trajectories.
 * Never supplies speed or predicted phase to Runtime. Not a motor plant model.
 *
 * Usage example
 * -------------
 * Quantize system_position() and compare the real Encoder output to a separate
 * system_reference_speed() state. All helpers are local to the including test.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Each scenario owns a zero-initialized speed reference. No global mutable state
 * or production interface is added by this test-only header.
 *
 * Failure / recovery
 * ------------------
 * The test supplies increasing signed times and valid CPR/cutoff. Its assertions
 * reject reference mismatches; these helpers do not grant sensor validity.
 */
#pragma once

/* Includes --------------------------------------------------------------------- */

#include <math.h>
#include <stdint.h>

/* Public Types (test translation unit only) ------------------------------------ */

struct system_angle_reference {
    double position_rad;
    double velocity_rad_s;
    double acceleration_rad_s2;
};

struct system_speed_reference {
    int64_t count;
    int64_t stamp_us;
    double raw_rad_s;
    double filtered_rad_s;
    unsigned observations;
};

/* Public Functions (test-local static helpers) --------------------------------- */

static double system_short(double angle)
{
    return remainder(angle, 6.283185307179586476925286766559);
}

static double system_position(const struct system_angle_reference *motion, int64_t time_us)
{
    const double t = (double)time_us / 1000000.0;
    return motion->position_rad + motion->velocity_rad_s * t +
        0.5 * motion->acceleration_rad_s2 * t * t;
}

static double system_velocity(const struct system_angle_reference *motion, int64_t time_us)
{
    return motion->velocity_rad_s + motion->acceleration_rad_s2 * (double)time_us / 1000000.0;
}

static int64_t system_quantize(double position, uint32_t cpr)
{
    return (int64_t)floor(position * (double)cpr / 6.283185307179586476925286766559 + 0.5);
}

static uint32_t system_cycle(int64_t count, uint32_t cpr)
{
    const int64_t remainder_count = count % (int64_t)cpr;
    return (uint32_t)(remainder_count < 0 ? remainder_count + cpr : remainder_count);
}

static void system_reference_speed(struct system_speed_reference *state,
                                   int64_t count,
                                   int64_t stamp_us,
                                   uint32_t cpr,
                                   double cutoff_hz)
{
    if (state->observations != 0U) {
        const double dt = (double)(stamp_us - state->stamp_us) / 1000000.0;
        state->raw_rad_s =
            (double)(count - state->count) * 6.283185307179586476925286766559 / (double)cpr / dt;
        const double gain = dt / (1.0 / (6.283185307179586476925286766559 * cutoff_hz) + dt);
        state->filtered_rad_s = state->observations == 1U
            ? state->raw_rad_s
            : state->filtered_rad_s + gain * (state->raw_rad_s - state->filtered_rad_s);
    }
    state->count = count;
    state->stamp_us = stamp_us;
    ++state->observations;
}

/* ---------------------------------------- end of file ----------------------------------------- */
