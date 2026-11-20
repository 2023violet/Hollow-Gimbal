/**
 * @file    test_foc_pwm_port.c
 * @brief   Exercise the isolated Board1 PWM transaction and register model.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Test role
 * ---------
 * Inject interleavings at each shadow write and at the simulated load boundary.
 */

/* Includes --------------------------------------------------------------------- */
#include "foc_board_pwm_model.h"

#include <math.h>
#include <stdio.h>

/* Private Helpers -------------------------------------------------------------- */
#define CHECK(e)                                            \
    do {                                                    \
        if (!(e)) {                                         \
            fprintf(stderr, "line %d: %s\n", __LINE__, #e); \
            return 1;                                       \
        }                                                   \
    } while (0)

struct fixture {
    struct foc_board_pwm_model port;
    struct foc_board_pwm_id id;
    struct foc_board_adc_transaction token;
    struct foc_board_load_plan plan;
    struct foc_board_shadow_attempt timing;
    struct foc_device_pwm_config config;
    struct foc_device_pwm_request request;
};

static struct fixture fixture_for(uint32_t cycle, uint32_t period_ns, uint32_t load_cycle)
{
    struct fixture fixture = {
        .id =
            {.session = 17U,
             .submission = UINT32_MAX,
             .slot = cycle,
             .generation = UINT32_MAX,
             .command = 40U},
        .token =
            {.slot_id = cycle,
             .generation = UINT32_MAX,
             .state = FOC_BOARD_ADC_COMPLETED,
             .adc_valid = true},
        .plan = {.period_ns = period_ns, .setup_ns = 2000U, .load = {load_cycle, 0U}},
        .timing =
            {.sample_cycle = cycle,
             .generation = UINT32_MAX,
             .eoc = {cycle, 17000U},
             .h0_start = {cycle, 18000U},
             .h0_finish = {cycle, 29000U},
             .shadow_write = {cycle, 34000U},
             .window_valid = true},
        .config = {.period_counts = 3750U, .phase_order = FOC_DEVICE_PHASE_ORDER_ABC},
    };
    const struct foc_output output = {
        .output_enable = 1U,
        .svpwm = {.duty_a = 0.2f, .duty_b = 0.4f, .duty_c = 0.6f}
    };
    (void)foc_device_map_output(&output, &fixture.config, &fixture.request);
    return fixture;
}

static enum foc_board_pwm_stage accept(struct fixture *f, bool guard)
{
    return foc_board_pwm_accept(
        &f->port,
        &f->id,
        &f->token,
        &f->plan,
        &f->timing,
        &f->config,
        &f->request,
        guard
    );
}

static enum foc_board_pwm_stage write_channel(struct fixture *f, unsigned int channel)
{
    const uint32_t values[] =
        {f->request.channel1_compare, f->request.channel2_compare, f->request.channel3_compare};
    return foc_board_pwm_write(
        &f->port,
        &f->id,
        channel,
        values[channel - 1U],
        f->timing.shadow_write
    );
}

static int test_complete(void)
{
    struct fixture f = fixture_for(UINT32_MAX, 50000U, 0U);
    CHECK(!foc_board_pwm_rearm(&f.port, 17U, false));
    CHECK(accept(&f, true) == FOC_BOARD_PWM_INHIBITED);
    CHECK(foc_board_pwm_rearm(&f.port, 17U, true));
    CHECK(f.request.channel1_compare > f.request.channel2_compare);
    CHECK(f.request.channel2_compare > f.request.channel3_compare);
    CHECK(accept(&f, true) == FOC_BOARD_PWM_UPDATE_ACCEPTED);
    CHECK(write_channel(&f, 1U) == FOC_BOARD_PWM_UPDATE_ACCEPTED);
    CHECK(write_channel(&f, 2U) == FOC_BOARD_PWM_UPDATE_ACCEPTED);
    CHECK(write_channel(&f, 3U) == FOC_BOARD_PWM_SHADOW_WRITTEN);
    CHECK(!f.port.active);
    CHECK(
        foc_board_pwm_load(&f.port, &f.id, f.plan.load, true, true) == FOC_BOARD_PWM_MODEL_ACTIVE
    );
    CHECK(f.port.active && f.port.active_id.command == 40U);
    CHECK(
        f.port.compare[0] == f.request.channel1_compare &&
        f.port.compare[2] == f.request.channel3_compare
    );

    f.id.slot = f.token.slot_id = f.timing.sample_cycle = 0U;
    f.id.generation = f.token.generation = f.timing.generation = 0U;
    f.id.submission = 0U;
    f.timing.eoc.cycle = f.timing.h0_start.cycle = f.timing.h0_finish.cycle =
        f.timing.shadow_write.cycle = 0U;
    f.plan.load.cycle = 1U;
    CHECK(accept(&f, true) == FOC_BOARD_PWM_UPDATE_ACCEPTED);
    CHECK(f.port.active && f.port.active_id.submission == UINT32_MAX);
    return 0;
}

static int test_revocation(void)
{
    for (unsigned int written = 0U; written <= 3U; ++written) {
        struct fixture f = fixture_for(8U, 50000U, 9U);
        CHECK(foc_board_pwm_rearm(&f.port, 17U, true));
        CHECK(accept(&f, true) == FOC_BOARD_PWM_UPDATE_ACCEPTED);
        for (unsigned int channel = 1U; channel <= written; ++channel) {
            CHECK(write_channel(&f, channel) != FOC_BOARD_PWM_INHIBITED);
        }
        if (written == 3U) {
            CHECK(
                foc_board_pwm_load(&f.port, &f.id, f.plan.load, true, true) ==
                FOC_BOARD_PWM_MODEL_ACTIVE
            );
        }
        foc_board_pwm_inhibit(&f.port);
        CHECK(!f.port.active && !f.port.session_open && f.port.written == 0U);
        CHECK(f.port.compare[0] == 0U && f.port.shadow[0] == 0U);
        CHECK(write_channel(&f, 1U) == FOC_BOARD_PWM_INHIBITED);
        CHECK(
            foc_board_pwm_load(&f.port, &f.id, f.plan.load, true, true) == FOC_BOARD_PWM_INHIBITED
        );
        CHECK(!foc_board_pwm_rearm(&f.port, 17U, true));
        CHECK(!foc_board_pwm_rearm(&f.port, 18U, false));
        CHECK(foc_board_pwm_rearm(&f.port, 18U, true));
        CHECK(accept(&f, true) == FOC_BOARD_PWM_INHIBITED);
    }
    return 0;
}

static int test_invalid(void)
{
    for (unsigned int fault = 0U; fault < 11U; ++fault) {
        struct fixture f = fixture_for(100U, 50000U, 101U);
        CHECK(foc_board_pwm_rearm(&f.port, 17U, true));
        switch (fault) {
        case 0U:
            f.request.channel2_compare++;
            break;
        case 1U:
            f.request.duty_a = NAN;
            break;
        case 2U:
            f.request.output_enable_request = 0U;
            break;
        case 3U:
            f.id.session = 0U;
            break;
        case 4U:
            f.id.generation--;
            break;
        case 5U:
            f.plan.load.cycle += UINT32_C(0x80000000);
            break;
        case 6U:
            f.timing.window_valid = false;
            break;
        case 7U:
            f.timing.shadow_write.phase_ns = 48000U;
            break;
        case 8U:
            f.token.adc_valid = false;
            break;
        case 9U:
            f.request.action = FOC_DEVICE_OUTPUT_ACTIVE_BRAKE;
            break;
        default:
            break;
        }
        CHECK(accept(&f, fault != 10U) == FOC_BOARD_PWM_INHIBITED);
        CHECK(!f.port.session_open && !f.port.active);
    }
    return 0;
}

static int test_load_interference(void)
{
    for (unsigned int fault = 0U; fault < 10U; ++fault) {
        struct fixture f = fixture_for(120U, fault == 9U ? 100000U : 50000U, 121U);
        CHECK(foc_board_pwm_rearm(&f.port, 17U, true));
        CHECK(accept(&f, true) == FOC_BOARD_PWM_UPDATE_ACCEPTED);
        if (fault == 0U) {
            CHECK(write_channel(&f, 2U) == FOC_BOARD_PWM_INHIBITED);
        } else if (fault == 1U) {
            CHECK(
                foc_board_pwm_write(
                    &f.port,
                    &f.id,
                    1U,
                    f.request.channel1_compare + 1U,
                    f.timing.shadow_write
                ) == FOC_BOARD_PWM_INHIBITED
            );
        } else if (fault == 2U) {
            CHECK(write_channel(&f, 1U) == FOC_BOARD_PWM_UPDATE_ACCEPTED);
            CHECK(
                foc_board_pwm_load(&f.port, &f.id, f.plan.load, true, true) ==
                FOC_BOARD_PWM_INHIBITED
            );
        } else if (fault == 3U) {
            CHECK(write_channel(&f, 1U) == FOC_BOARD_PWM_UPDATE_ACCEPTED);
            f.timing.shadow_write.phase_ns = 48000U;
            CHECK(write_channel(&f, 2U) == FOC_BOARD_PWM_INHIBITED);
        } else {
            CHECK(write_channel(&f, 1U) == FOC_BOARD_PWM_UPDATE_ACCEPTED);
            CHECK(write_channel(&f, 2U) == FOC_BOARD_PWM_UPDATE_ACCEPTED);
            CHECK(write_channel(&f, 3U) == FOC_BOARD_PWM_SHADOW_WRITTEN);
            f.id.submission += fault == 4U ? 1U : 0U;
            f.plan.load.phase_ns += fault == 5U ? 1U : 0U;
            if (fault == 8U) {
                CHECK(accept(&f, true) == FOC_BOARD_PWM_INHIBITED);
            } else {
                CHECK(
                    foc_board_pwm_load(&f.port, &f.id, f.plan.load, fault != 6U, fault != 7U) ==
                    (fault == 9U ? FOC_BOARD_PWM_MODEL_ACTIVE : FOC_BOARD_PWM_INHIBITED)
                );
            }
        }
        CHECK(f.port.active == (fault == 9U));
    }
    return 0;
}

static int test_runtime_stop_at_load(void)
{
    const uint32_t reasons[] = {
        0U,
        FOC_SAFE_FAULT_SAMPLE_INVALID,
        FOC_SAFE_FAULT_FAST_HEARTBEAT,
        FOC_SAFE_FAULT_WDT,
    };
    for (unsigned int i = 0U; i < sizeof reasons / sizeof reasons[0]; ++i) {
        struct foc_runtime runtime;
        struct foc_board_output output;
        CHECK(foc_board_init(&runtime, &output, 0U) == FOC_RUNTIME_OK);
        struct fixture f = fixture_for(11U, 50000U, 12U);
        CHECK(foc_board_pwm_rearm(&f.port, f.id.session, true));
        CHECK(accept(&f, true) == FOC_BOARD_PWM_UPDATE_ACCEPTED);
        CHECK(write_channel(&f, 1U) == FOC_BOARD_PWM_UPDATE_ACCEPTED);
        CHECK(write_channel(&f, 2U) == FOC_BOARD_PWM_UPDATE_ACCEPTED);
        CHECK(write_channel(&f, 3U) == FOC_BOARD_PWM_SHADOW_WRITTEN);
        foc_runtime_request_inhibit(&runtime, reasons[i]);
        const bool allowed =
            atomic_load_explicit(&runtime.stop_requests, memory_order_acquire) == 0U;
        CHECK(!allowed);
        CHECK(
            foc_board_pwm_load(&f.port, &f.id, f.plan.load, true, allowed) ==
            FOC_BOARD_PWM_INHIBITED
        );
        CHECK(!f.port.active && !f.port.session_open && f.port.written == 0U);
    }
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */
int main(void)
{
    CHECK(test_complete() == 0);
    CHECK(test_revocation() == 0);
    CHECK(test_invalid() == 0);
    CHECK(test_load_interference() == 0);
    CHECK(test_runtime_stop_at_load() == 0);
    puts("Board1 host-only PWM transaction: shadow, load, rollover and inhibit pass");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
