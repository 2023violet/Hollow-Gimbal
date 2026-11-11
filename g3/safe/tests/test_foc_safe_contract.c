/**
 * @file    test_foc_safe_contract.c
 * @brief   Fault-injection tests for the no-power G3-SAFE contract.
 * @version v0.1.0
 * @date    2026.11.09
 *
 * Test role
 * ---------
 * This executable proves state, latch, freshness, heartbeat, WDT, Flash, and
 * output-inhibit behavior without touching a real hardware path.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_safe.h"

#include <stdio.h>

#define TEST_ASSERT(condition)                                                                  \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            fprintf(stderr, "assertion failed: %s at %s:%d\n", #condition, __FILE__, __LINE__); \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)

/* Private Helpers -------------------------------------------------------------- */

static struct foc_safe_config make_config(void)
{
    return (struct foc_safe_config){
        .command_timeout_us = 100U,
        .sample_timeout_us = 100U,
        .fast_heartbeat_timeout_us = 50U,
        .foreground_heartbeat_timeout_us = 80U,
        .invalid_sample_latch_after = 2U,
    };
}

static struct foc_device_pwm_config make_pwm_config(void)
{
    return (struct foc_device_pwm_config){
        .period_counts = 3749U,
        .phase_order = FOC_DEVICE_PHASE_ORDER_ABC,
    };
}

static struct foc_device_pwm_request make_update_request(void)
{
    return (struct foc_device_pwm_request){
        .action = FOC_DEVICE_OUTPUT_UPDATE,
        .output_enable_request = 1U,
        .duty_a = 0.50f,
        .duty_b = 0.45f,
        .duty_c = 0.55f,
        .channel1_compare = 1874U,
        .channel2_compare = 1687U,
        .channel3_compare = 2061U,
    };
}

static void publish_healthy_observations(
    struct foc_safe_context *context,
    uint32_t now_us,
    uint32_t command_seq,
    uint32_t sample_seq
)
{
    (void)foc_safe_observe_command(context, now_us, command_seq);
    (void)foc_safe_observe_sample(context, now_us, sample_seq, true);
    (void)foc_safe_heartbeat_fast(context, now_us);
    (void)foc_safe_heartbeat_foreground(context, now_us);
}

static int decision_is_inhibited(const struct foc_safe_output_decision *decision)
{
    return decision->permitted == 0U && decision->request.action == FOC_DEVICE_OUTPUT_INHIBIT &&
        decision->request.output_enable_request == 0U;
}

/* Test Entry ------------------------------------------------------------------ */

int main(void)
{
    const struct foc_safe_config config = make_config();
    const struct foc_device_pwm_config pwm_config = make_pwm_config();
    const struct foc_device_pwm_request update_request = make_update_request();
    struct foc_safe_context context = {0};
    struct foc_safe_output_decision decision = {0};

    TEST_ASSERT(foc_safe_init(&context, &config) == FOC_SAFE_STATUS_OK);
    TEST_ASSERT(context.state == FOC_SAFE_STATE_BOOT);
    TEST_ASSERT(
        foc_safe_gate_output(&context, 0U, &pwm_config, &update_request, &decision) ==
        FOC_SAFE_STATUS_OK
    );
    TEST_ASSERT(decision_is_inhibited(&decision));

    TEST_ASSERT(foc_safe_boot_complete(&context) == FOC_SAFE_STATUS_OK);
    TEST_ASSERT(
        foc_safe_gate_output(&context, 0U, &pwm_config, &update_request, &decision) ==
        FOC_SAFE_STATUS_OK
    );
    TEST_ASSERT(decision_is_inhibited(&decision));
    TEST_ASSERT(foc_safe_arm(&context) == FOC_SAFE_STATUS_OK);
    TEST_ASSERT(
        foc_safe_gate_output(&context, 0U, &pwm_config, &update_request, &decision) ==
        FOC_SAFE_STATUS_OK
    );
    TEST_ASSERT(decision_is_inhibited(&decision));
    TEST_ASSERT(foc_safe_start(&context) == FOC_SAFE_STATUS_OK);

    publish_healthy_observations(&context, 0U, 1U, 1U);
    TEST_ASSERT(foc_safe_supervisor_tick(&context, 10U) == FOC_SAFE_STATUS_OK);
    TEST_ASSERT(
        foc_safe_gate_output(&context, 10U, &pwm_config, &update_request, &decision) ==
        FOC_SAFE_STATUS_OK
    );
    TEST_ASSERT(decision.permitted != 0U);
    TEST_ASSERT(decision.request.action == FOC_DEVICE_OUTPUT_UPDATE);
    TEST_ASSERT(foc_safe_watchdog_permitted(&context, 10U) == FOC_SAFE_STATUS_OK);

    {
        struct foc_safe_context timeout_context = {0};

        TEST_ASSERT(foc_safe_init(&timeout_context, &config) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_boot_complete(&timeout_context) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_arm(&timeout_context) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_start(&timeout_context) == FOC_SAFE_STATUS_OK);
        publish_healthy_observations(&timeout_context, 0U, 1U, 1U);
        TEST_ASSERT(foc_safe_observe_sample(&timeout_context, 90U, 2U, true) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_heartbeat_fast(&timeout_context, 90U) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_heartbeat_foreground(&timeout_context, 90U) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(
            foc_safe_supervisor_tick(&timeout_context, 101U) == FOC_SAFE_STATUS_ERR_TIMEOUT
        );
        TEST_ASSERT(timeout_context.state == FOC_SAFE_STATE_FAULT_LATCHED);
        TEST_ASSERT((timeout_context.latched_faults & FOC_SAFE_FAULT_COMMAND_TIMEOUT) != 0U);
    }

    {
        struct foc_safe_context timeout_context = {0};

        TEST_ASSERT(foc_safe_init(&timeout_context, &config) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_boot_complete(&timeout_context) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_arm(&timeout_context) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_start(&timeout_context) == FOC_SAFE_STATUS_OK);
        publish_healthy_observations(&timeout_context, 0U, 1U, 1U);
        TEST_ASSERT(foc_safe_observe_command(&timeout_context, 90U, 2U) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_heartbeat_fast(&timeout_context, 90U) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_heartbeat_foreground(&timeout_context, 90U) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(
            foc_safe_supervisor_tick(&timeout_context, 101U) == FOC_SAFE_STATUS_ERR_TIMEOUT
        );
        TEST_ASSERT((timeout_context.latched_faults & FOC_SAFE_FAULT_SAMPLE_TIMEOUT) != 0U);
    }

    {
        struct foc_safe_context heartbeat_context = {0};

        TEST_ASSERT(foc_safe_init(&heartbeat_context, &config) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_boot_complete(&heartbeat_context) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_arm(&heartbeat_context) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_start(&heartbeat_context) == FOC_SAFE_STATUS_OK);
        publish_healthy_observations(&heartbeat_context, 0U, 1U, 1U);
        TEST_ASSERT(foc_safe_heartbeat_foreground(&heartbeat_context, 60U) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(
            foc_safe_supervisor_tick(&heartbeat_context, 60U) == FOC_SAFE_STATUS_ERR_TIMEOUT
        );
        TEST_ASSERT((heartbeat_context.latched_faults & FOC_SAFE_FAULT_FAST_HEARTBEAT) != 0U);
    }

    {
        struct foc_safe_context heartbeat_context = {0};

        TEST_ASSERT(foc_safe_init(&heartbeat_context, &config) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_boot_complete(&heartbeat_context) == FOC_SAFE_STATUS_OK);
        publish_healthy_observations(&heartbeat_context, 0U, 1U, 1U);
        TEST_ASSERT(foc_safe_heartbeat_fast(&heartbeat_context, 90U) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(
            foc_safe_watchdog_permitted(&heartbeat_context, 90U) ==
            FOC_SAFE_STATUS_ERR_NOT_PERMITTED
        );
        TEST_ASSERT((heartbeat_context.latched_faults & FOC_SAFE_FAULT_FOREGROUND_HEARTBEAT) != 0U);
    }

    {
        struct foc_safe_context watchdog_context = {0};

        TEST_ASSERT(foc_safe_init(&watchdog_context, &config) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_boot_complete(&watchdog_context) == FOC_SAFE_STATUS_OK);
        publish_healthy_observations(&watchdog_context, 0U, 1U, 1U);
        TEST_ASSERT(
            foc_safe_latch_fault(&watchdog_context, FOC_SAFE_FAULT_WDT) == FOC_SAFE_STATUS_OK
        );
        TEST_ASSERT(foc_safe_watchdog_permitted(&watchdog_context, 10U) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(
            foc_safe_latch_fault(&watchdog_context, FOC_SAFE_FAULT_FATAL_RESET_REQUIRED) ==
            FOC_SAFE_STATUS_OK
        );
        TEST_ASSERT(
            foc_safe_watchdog_permitted(&watchdog_context, 10U) == FOC_SAFE_STATUS_ERR_NOT_PERMITTED
        );
    }

    {
        struct foc_safe_context invalid_sample_context = {0};

        TEST_ASSERT(foc_safe_init(&invalid_sample_context, &config) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_boot_complete(&invalid_sample_context) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(
            foc_safe_observe_sample(&invalid_sample_context, 0U, 1U, false) ==
            FOC_SAFE_STATUS_ERR_INVALID_INPUT
        );
        TEST_ASSERT(invalid_sample_context.state != FOC_SAFE_STATE_FAULT_LATCHED);
        TEST_ASSERT(
            foc_safe_observe_sample(&invalid_sample_context, 1U, 2U, false) ==
            FOC_SAFE_STATUS_ERR_INVALID_INPUT
        );
        TEST_ASSERT(invalid_sample_context.state == FOC_SAFE_STATE_FAULT_LATCHED);
        TEST_ASSERT((invalid_sample_context.latched_faults & FOC_SAFE_FAULT_SAMPLE_INVALID) != 0U);
        TEST_ASSERT(foc_safe_arm(&invalid_sample_context) == FOC_SAFE_STATUS_ERR_FAULT_LATCHED);
        TEST_ASSERT(foc_safe_clear_fault(&invalid_sample_context) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(invalid_sample_context.state == FOC_SAFE_STATE_DISABLED);
        TEST_ASSERT(foc_safe_start(&invalid_sample_context) == FOC_SAFE_STATUS_ERR_STATE);
    }

    {
        struct foc_safe_context immediate_inhibit_context = {0};

        TEST_ASSERT(foc_safe_init(&immediate_inhibit_context, &config) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_boot_complete(&immediate_inhibit_context) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_arm(&immediate_inhibit_context) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_start(&immediate_inhibit_context) == FOC_SAFE_STATUS_OK);
        publish_healthy_observations(&immediate_inhibit_context, 0U, 1U, 1U);
        TEST_ASSERT(
            foc_safe_observe_sample(&immediate_inhibit_context, 10U, 2U, false) ==
            FOC_SAFE_STATUS_ERR_INVALID_INPUT
        );
        TEST_ASSERT(
            foc_safe_gate_output(
                &immediate_inhibit_context,
                10U,
                &pwm_config,
                &update_request,
                &decision
            ) == FOC_SAFE_STATUS_OK
        );
        TEST_ASSERT(decision_is_inhibited(&decision));
        TEST_ASSERT(immediate_inhibit_context.state != FOC_SAFE_STATE_FAULT_LATCHED);
    }

    {
        struct foc_safe_context wrap_context = {0};
        const uint32_t near_wrap = UINT32_MAX - 20U;

        TEST_ASSERT(foc_safe_init(&wrap_context, &config) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_boot_complete(&wrap_context) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(
            foc_safe_observe_command(&wrap_context, near_wrap, UINT32_MAX - 2U) ==
            FOC_SAFE_STATUS_OK
        );
        TEST_ASSERT(
            foc_safe_observe_sample(&wrap_context, near_wrap, UINT32_MAX - 2U, true) ==
            FOC_SAFE_STATUS_OK
        );
        TEST_ASSERT(foc_safe_heartbeat_fast(&wrap_context, near_wrap) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_heartbeat_foreground(&wrap_context, near_wrap) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_observe_command(&wrap_context, 10U, 1U) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_observe_sample(&wrap_context, 10U, 1U, true) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_watchdog_permitted(&wrap_context, 10U) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(
            foc_safe_observe_command(&wrap_context, 5U, 2U) == FOC_SAFE_STATUS_ERR_INVALID_INPUT
        );
    }

    {
        struct foc_safe_context recovery_context = {0};

        TEST_ASSERT(foc_safe_init(&recovery_context, &config) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_boot_complete(&recovery_context) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_arm(&recovery_context) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_start(&recovery_context) == FOC_SAFE_STATUS_OK);
        publish_healthy_observations(&recovery_context, 0U, 1U, 1U);
        TEST_ASSERT(
            foc_safe_latch_fault(&recovery_context, FOC_SAFE_FAULT_COMMAND_TIMEOUT) ==
            FOC_SAFE_STATUS_OK
        );
        TEST_ASSERT(foc_safe_clear_fault(&recovery_context) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_arm(&recovery_context) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_start(&recovery_context) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(
            foc_safe_gate_output(&recovery_context, 10U, &pwm_config, &update_request, &decision) ==
            FOC_SAFE_STATUS_OK
        );
        TEST_ASSERT(decision_is_inhibited(&decision));
    }

    {
        struct foc_safe_context commit_context = {0};

        TEST_ASSERT(foc_safe_init(&commit_context, &config) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_boot_complete(&commit_context) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_arm(&commit_context) == FOC_SAFE_STATUS_OK);
        TEST_ASSERT(foc_safe_start(&commit_context) == FOC_SAFE_STATUS_OK);
        publish_healthy_observations(&commit_context, 0U, 1U, 1U);
        TEST_ASSERT(
            foc_safe_gate_output(&commit_context, 10U, &pwm_config, &update_request, &decision) ==
            FOC_SAFE_STATUS_OK
        );
        TEST_ASSERT(decision.permitted != 0U);
        TEST_ASSERT(
            foc_safe_latch_fault(&commit_context, FOC_SAFE_FAULT_SAMPLE_TIMEOUT) ==
            FOC_SAFE_STATUS_OK
        );
        TEST_ASSERT(
            foc_safe_gate_output(&commit_context, 11U, &pwm_config, &update_request, &decision) ==
            FOC_SAFE_STATUS_OK
        );
        TEST_ASSERT(decision_is_inhibited(&decision));
    }

    TEST_ASSERT(foc_safe_disable(&context) == FOC_SAFE_STATUS_OK);
    TEST_ASSERT(
        foc_safe_flash_permitted(&context, FOC_DEVICE_OUTPUT_INHIBIT) == FOC_SAFE_STATUS_OK
    );
    TEST_ASSERT(
        foc_safe_flash_permitted(&context, FOC_DEVICE_OUTPUT_UPDATE) ==
        FOC_SAFE_STATUS_ERR_NOT_PERMITTED
    );
    TEST_ASSERT(foc_safe_arm(&context) == FOC_SAFE_STATUS_OK);
    TEST_ASSERT(
        foc_safe_flash_permitted(&context, FOC_DEVICE_OUTPUT_INHIBIT) ==
        FOC_SAFE_STATUS_ERR_NOT_PERMITTED
    );

    {
        struct foc_device_pwm_request invalid_action = update_request;

        invalid_action.action = FOC_DEVICE_OUTPUT_ACTIVE_BRAKE;
        TEST_ASSERT(foc_safe_start(&context) == FOC_SAFE_STATUS_OK);
        publish_healthy_observations(&context, 200U, 2U, 2U);
        TEST_ASSERT(
            foc_safe_gate_output(&context, 200U, &pwm_config, &invalid_action, &decision) ==
            FOC_SAFE_STATUS_OK
        );
        TEST_ASSERT(decision_is_inhibited(&decision));
        TEST_ASSERT((decision.inhibit_reasons & FOC_SAFE_FAULT_INVALID_STATE) != 0U);

        invalid_action = update_request;
        invalid_action.duty_a = 2.0f;
        TEST_ASSERT(
            foc_safe_gate_output(&context, 200U, &pwm_config, &invalid_action, &decision) ==
            FOC_SAFE_STATUS_OK
        );
        TEST_ASSERT(decision_is_inhibited(&decision));
    }

    TEST_ASSERT(foc_safe_latch_fault(&context, FOC_SAFE_FAULT_WDT) == FOC_SAFE_STATUS_OK);
    TEST_ASSERT(foc_safe_start(&context) == FOC_SAFE_STATUS_ERR_FAULT_LATCHED);
    TEST_ASSERT(foc_safe_clear_fault(&context) == FOC_SAFE_STATUS_OK);
    TEST_ASSERT(context.state == FOC_SAFE_STATE_DISABLED);

    puts("test_foc_safe_contract passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
