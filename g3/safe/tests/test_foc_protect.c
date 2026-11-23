/**
 * @file    test_foc_protect.c
 * @brief   Rule tests for the motor-protection monitor.
 * @version v0.1.0
 * @date    2026.11.22
 *
 * Test role
 * ---------
 * This executable proves each protection rule, its window, its mode scope and
 * the divergence convergence check without touching a hardware path.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_protect.h"

#include <math.h>
#include <stdio.h>

#define TEST_ASSERT(condition)                                                                  \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            fprintf(stderr, "assertion failed: %s at %s:%d\n", #condition, __FILE__, __LINE__); \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)

/* Private Helpers -------------------------------------------------------------- */

static struct foc_protect_config make_config(void)
{
    return (struct foc_protect_config){
        .enabled = true,
        .overcurrent_a = 3.0f,
        .overvoltage_v = 22.0f,
        .undervoltage_v = 5.0f,
        .voltage_samples = 3U,
        .overspeed_rad_s = 15.0f,
        .sustained_current_a = 2.375f,
        .sustained_us = 1000U,
        .stall_current_a = 2.375f,
        .stall_speed_rad_s = 0.6f,
        .stall_us = 200U,
        .divergence_current_a = 2.375f,
        .divergence_us = 300U,
    };
}

static struct foc_protect_input make_input(uint32_t now_us, enum foc_protect_scope scope)
{
    return (struct foc_protect_input){
        .now_us = now_us,
        .scope = scope,
        .phase_current_a = {0.5f, -0.25f, -0.25f},
        .vbus_v = 12.0f,
        .current_magnitude_a = 0.5f,
        .speed_rad_s = 5.0f,
        .speed_ref_rad_s = 5.0f,
        .iq_ref_a = 0.5f,
        .speed_valid = true,
        .reference_valid = true,
    };
}

/* Tests ------------------------------------------------------------------------ */

static int test_validate_and_disabled(void)
{
    struct foc_protect_config config = make_config();
    struct foc_protect_state state = {0};
    TEST_ASSERT(foc_protect_validate(&config));
    config.overvoltage_v = 4.0f;
    TEST_ASSERT(!foc_protect_validate(&config));
    config = make_config();
    config.overcurrent_a = NAN;
    TEST_ASSERT(!foc_protect_validate(&config));
    const struct foc_protect_config disabled = {0};
    TEST_ASSERT(foc_protect_validate(&disabled));
    struct foc_protect_input input = make_input(0U, FOC_PROTECT_SCOPE_MOTION);
    input.phase_current_a.a = 100.0f;
    TEST_ASSERT(foc_protect_step(&disabled, &state, &input) == FOC_SAFE_FAULT_NONE);
    TEST_ASSERT(!foc_protect_validate(NULL));
    return 0;
}

static int test_electrical(void)
{
    const struct foc_protect_config config = make_config();
    struct foc_protect_state state = {0};
    struct foc_protect_input input = make_input(0U, FOC_PROTECT_SCOPE_ELECTRICAL);
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_NONE);
    input.phase_current_a.c = -3.1f;
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_OVER_CURRENT);
    input = make_input(0U, FOC_PROTECT_SCOPE_ELECTRICAL);
    input.vbus_v = 23.0f;
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_NONE);
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_NONE);
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_OVER_VOLTAGE);
    input.vbus_v = 12.0f;
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_NONE);
    input.vbus_v = 4.0f;
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_NONE);
    input.vbus_v = 12.0f;
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_NONE);
    input.vbus_v = NAN;
    (void)foc_protect_step(&config, &state, &input);
    (void)foc_protect_step(&config, &state, &input);
    TEST_ASSERT((foc_protect_step(&config, &state, &input) & FOC_SAFE_FAULT_OVER_VOLTAGE) != 0U);
    /* Electrical scope ignores the speed rules. */
    input = make_input(0U, FOC_PROTECT_SCOPE_ELECTRICAL);
    input.speed_rad_s = 100.0f;
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_NONE);
    return 0;
}

static int test_overspeed_and_sustained(void)
{
    const struct foc_protect_config config = make_config();
    struct foc_protect_state state = {0};
    struct foc_protect_input input = make_input(0U, FOC_PROTECT_SCOPE_TORQUE);
    input.speed_rad_s = -15.5f;
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_OVER_SPEED);
    input.speed_valid = false;
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_NONE);

    input = make_input(0U, FOC_PROTECT_SCOPE_TORQUE);
    input.current_magnitude_a = 2.4f;
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_NONE);
    input.now_us = 999U;
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_NONE);
    input.now_us = 1000U;
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_SUSTAINED_CURRENT);
    /* An interruption restarts the window. */
    input.current_magnitude_a = 1.0f;
    input.now_us = 1100U;
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_NONE);
    input.current_magnitude_a = 2.4f;
    input.now_us = 1200U;
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_NONE);
    input.now_us = 2100U;
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_NONE);
    input.now_us = 2200U;
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_SUSTAINED_CURRENT);
    /* Torque scope never reports stall or divergence. */
    input = make_input(5000U, FOC_PROTECT_SCOPE_TORQUE);
    input.current_magnitude_a = 2.4f;
    input.speed_rad_s = 0.0f;
    foc_protect_reset(&state);
    input.now_us = 5000U;
    (void)foc_protect_step(&config, &state, &input);
    input.now_us = 5500U;
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_NONE);
    return 0;
}

static int test_stall(void)
{
    struct foc_protect_config config = make_config();
    config.sustained_us = 0U;
    struct foc_protect_state state = {0};
    struct foc_protect_input input = make_input(0U, FOC_PROTECT_SCOPE_MOTION);
    input.current_magnitude_a = 2.4f;
    input.speed_rad_s = 0.3f;
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_NONE);
    input.now_us = 200U;
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_STALL);
    foc_protect_reset(&state);
    input.speed_rad_s = 1.0f;
    input.now_us = 1000U;
    (void)foc_protect_step(&config, &state, &input);
    input.now_us = 2000U;
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_NONE);
    config.stall_us = 0U;
    input.speed_rad_s = 0.0f;
    TEST_ASSERT(foc_protect_step(&config, &state, &input) == FOC_SAFE_FAULT_NONE);
    return 0;
}

/* Feed one 300 µs window of samples every 10 µs; returns the bits of the last sample. */
static uint32_t feed_window(
    const struct foc_protect_config *config,
    struct foc_protect_state *state,
    struct foc_protect_input *input,
    float iq_saturated_share,
    float speed_from,
    float speed_to
)
{
    uint32_t faults = FOC_SAFE_FAULT_NONE;
    for (unsigned int step = 0U; step < 31U; ++step) {
        input->iq_ref_a = (float)step < iq_saturated_share * 31.0f ? 2.5f : 1.0f;
        input->speed_rad_s = speed_from + (speed_to - speed_from) * (float)step / 30.0f;
        faults |= foc_protect_step(config, state, input);
        input->now_us += 10U;
    }
    return faults;
}

static int test_divergence(void)
{
    struct foc_protect_config config = make_config();
    config.sustained_us = 0U;
    config.stall_us = 0U;
    struct foc_protect_state state = {0};
    struct foc_protect_input input = make_input(0U, FOC_PROTECT_SCOPE_MOTION);
    input.speed_ref_rad_s = 5.0f;

    /* Runaway against the reference: the second saturated window is no better. */
    TEST_ASSERT(feed_window(&config, &state, &input, 1.0f, -2.0f, -8.0f) == 0U);
    TEST_ASSERT(
        feed_window(&config, &state, &input, 1.0f, -8.0f, -14.0f) ==
        FOC_SAFE_FAULT_TRACKING_DIVERGENCE
    );

    /* Rocking: saturated for 60% of each window, speed swinging around zero. */
    foc_protect_reset(&state);
    TEST_ASSERT(feed_window(&config, &state, &input, 0.6f, -4.0f, 4.0f) == 0U);
    TEST_ASSERT(
        feed_window(&config, &state, &input, 0.6f, 4.0f, -4.0f) ==
        FOC_SAFE_FAULT_TRACKING_DIVERGENCE
    );

    /* A commanded acceleration or reversal: the mean error falls window after window. */
    foc_protect_reset(&state);
    input.speed_ref_rad_s = -5.0f;
    float speed = 5.0f;
    for (unsigned int window = 0U; window < 6U; ++window) {
        TEST_ASSERT(feed_window(&config, &state, &input, 1.0f, speed, speed - 1.5f) == 0U);
        speed -= 1.5f;
    }

    /* Mostly unsaturated windows clear the baseline; no reference, no rule. */
    foc_protect_reset(&state);
    input.speed_ref_rad_s = 5.0f;
    TEST_ASSERT(feed_window(&config, &state, &input, 1.0f, 0.0f, 0.0f) == 0U);
    TEST_ASSERT(feed_window(&config, &state, &input, 0.3f, 0.0f, 0.0f) == 0U);
    TEST_ASSERT(feed_window(&config, &state, &input, 1.0f, 0.0f, 0.0f) == 0U);
    input.reference_valid = false;
    TEST_ASSERT(feed_window(&config, &state, &input, 1.0f, 0.0f, 0.0f) == 0U);
    TEST_ASSERT(feed_window(&config, &state, &input, 1.0f, 0.0f, 0.0f) == 0U);
    return 0;
}

/* Public Functions ------------------------------------------------------------- */

int main(void)
{
    if (test_validate_and_disabled() || test_electrical() || test_overspeed_and_sustained() ||
        test_stall() || test_divergence()) {
        return 1;
    }
    printf("foc_protect: all rules pass\n");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
