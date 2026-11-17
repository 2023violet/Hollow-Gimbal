/**
 * @file    falconfoc_virtual_motion.c
 * @brief   Deterministic Host Velocity/Position scenarios for VM-01b.
 * @version v0.1.0
 * @date    2026.11.15
 *
 * Implementation role
 * -------------------
 * Exercise the production motion paths, mode/session boundaries, and fault
 * isolation through the VM-01a virtual motor harness.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_current.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Private Types ---------------------------------------------------------------- */

struct motion_result {
    const char *name;
    enum foc_runtime_mode mode;
    float reference;
    double rmse;
    double tail_rmse;
    double peak_error;
    double overshoot;
    uint32_t settling_us;
    double max_iq;
    double iq_reference;
    double iq_actual;
    uint32_t saturated_updates;
    uint32_t accepted_updates;
    uint32_t rejected_updates;
    uint32_t virtual_time_us;
    double final_position_rad;
    double final_velocity_rad_s;
    enum foc_safe_state state;
    uint32_t faults;
    uint32_t motion_session;
    uint32_t replay_hash;
    bool expected_degraded;
};

/* Private Helpers -------------------------------------------------------------- */

static int check(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "scenario failure: %s\n", message);
        return 1;
    }
    return 0;
}

static int start_current(struct foc_virtual_current *context, float iq_a)
{
    return foc_virtual_current_publish_iq(context, 0.0f, iq_a) == FOC_RUNTIME_OK &&
            foc_virtual_current_run_until(context, 0U) == 0 &&
            foc_virtual_current_queue_action(context, FOC_RUNTIME_ACTION_ARM) == 0 &&
            foc_virtual_current_run_until(context, 200U) == 0 &&
            foc_virtual_current_queue_action(context, FOC_RUNTIME_ACTION_START) == 0 &&
            foc_virtual_current_run_until(context, 400U) == 0
        ? 0
        : 1;
}

static int refresh_current(struct foc_virtual_current *context, uint32_t target_us)
{
    while (context->virtual_time_us < target_us) {
        if (foc_virtual_current_publish_iq(context, 0.0f, 0.35f) != FOC_RUNTIME_OK) {
            return 1;
        }
        uint32_t next = context->virtual_time_us + 1000U;
        if (next > target_us) {
            next = target_us;
        }
        if (foc_virtual_current_run_until(context, next) != 0) {
            return 1;
        }
    }
    return 0;
}

static int
start_motion(struct foc_virtual_current *context, enum foc_runtime_mode mode, float reference)
{
    if (foc_virtual_current_queue_set_mode(context, mode) != 0 ||
        foc_virtual_current_run_until(context, 0U) != 0 ||
        foc_virtual_current_run_until(context, 100U) != 0 ||
        foc_virtual_current_publish_motion(context, mode, reference) != FOC_RUNTIME_OK ||
        foc_virtual_current_run_until(context, 500U) != 0 ||
        foc_virtual_current_queue_action(context, FOC_RUNTIME_ACTION_ARM) != 0 ||
        foc_virtual_current_run_until(context, 600U) != 0 ||
        foc_virtual_current_queue_action(context, FOC_RUNTIME_ACTION_START) != 0 ||
        foc_virtual_current_run_until(context, 800U) != 0) {
        return 1;
    }
    return 0;
}

static int collect_motion(
    struct foc_virtual_current *context,
    enum foc_runtime_mode mode,
    float reference,
    uint32_t target_us,
    struct motion_result *result
)
{
    const uint32_t start_us = context->virtual_time_us;
    const uint32_t duration_us = target_us - start_us;
    double sum_sq = 0.0;
    double tail_sum_sq = 0.0;
    uint32_t samples = 0U;
    uint32_t tail_samples = 0U;
    uint32_t settling_candidate = 0U;
    const double tolerance = mode == FOC_RUNTIME_MODE_POSITION ? 0.05 : 0.5;
    double max_iq = 0.0;
    double peak_error = 0.0;
    double overshoot = 0.0;

    while (context->virtual_time_us < target_us) {
        if (foc_virtual_current_publish_motion(context, mode, reference) != FOC_RUNTIME_OK) {
            return 1;
        }
        uint32_t next = context->virtual_time_us + 1000U;
        if (next > target_us) {
            next = target_us;
        }
        if (foc_virtual_current_run_until(context, next) != 0) {
            return 1;
        }
        struct foc_virtual_current_observation observation;
        if (foc_virtual_current_observe(context, &observation) != 0) {
            return 1;
        }
        const double actual = mode == FOC_RUNTIME_MODE_POSITION
            ? observation.mechanical_position_rad
            : observation.mechanical_velocity_rad_s;
        const double error = (double)reference - actual;
        const double absolute_error = fabs(error);
        sum_sq += error * error;
        ++samples;
        if (context->virtual_time_us - start_us >= duration_us * 3U / 4U) {
            tail_sum_sq += error * error;
            ++tail_samples;
        }
        peak_error = fmax(peak_error, absolute_error);
        max_iq = fmax(max_iq, fabs((double)observation.iq_a));
        if ((reference >= 0.0f && actual > reference) || (reference < 0.0f && actual < reference)) {
            overshoot = fmax(overshoot, fabs(actual - (double)reference));
        }
        if (absolute_error <= tolerance) {
            if (settling_candidate == 0U) {
                settling_candidate = context->virtual_time_us;
            }
        } else {
            settling_candidate = 0U;
        }
    }

    struct foc_virtual_current_observation final;
    if (foc_virtual_current_observe(context, &final) != 0) {
        return 1;
    }
    *result = (struct motion_result){
        .mode = mode,
        .reference = reference,
        .rmse = sqrt(sum_sq / fmax(1U, samples)),
        .tail_rmse = sqrt(tail_sum_sq / fmax(1U, tail_samples)),
        .peak_error = peak_error,
        .overshoot = overshoot,
        .settling_us = settling_candidate == 0U ? 0U : settling_candidate - start_us,
        .max_iq = max_iq,
        .iq_reference = final.iq_ref_a,
        .iq_actual = final.iq_a,
        .saturated_updates = final.saturated_updates,
        .accepted_updates = final.accepted_updates,
        .rejected_updates = final.rejected_updates,
        .virtual_time_us = final.virtual_time_us,
        .final_position_rad = final.mechanical_position_rad,
        .final_velocity_rad_s = final.mechanical_velocity_rad_s,
        .state = final.safety_state,
        .faults = final.faults,
        .motion_session = final.motion_session,
        .replay_hash = foc_virtual_current_hash(context),
        .expected_degraded =
            fabs(
                (mode == FOC_RUNTIME_MODE_POSITION ? final.mechanical_position_rad
                                                   : final.mechanical_velocity_rad_s) -
                (double)reference
            ) > tolerance,
    };
    return 0;
}

static int run_motion_case(
    const struct foc_virtual_current_profile *profile,
    const char *name,
    enum foc_runtime_mode mode,
    float reference,
    uint32_t duration_us,
    double load_torque_nm,
    struct motion_result *result
)
{
    struct foc_virtual_current context;
    if (foc_virtual_current_init(&context, profile) != 0 ||
        start_motion(&context, mode, reference) != 0 ||
        foc_virtual_motor_set_load(&context.motor, load_torque_nm) != 0 ||
        collect_motion(&context, mode, reference, context.virtual_time_us + duration_us, result) !=
            0) {
        return 1;
    }
    result->name = name;
    return check(result->state == FOC_SAFE_STATE_RUNNING, "motion case left RUNNING");
}

static int
run_reverse_case(const struct foc_virtual_current_profile *profile, struct motion_result *result)
{
    struct foc_virtual_current context;
    if (foc_virtual_current_init(&context, profile) != 0 ||
        start_motion(&context, FOC_RUNTIME_MODE_VELOCITY, 4.0f) != 0 ||
        collect_motion(
            &context,
            FOC_RUNTIME_MODE_VELOCITY,
            4.0f,
            context.virtual_time_us + 80000U,
            result
        ) != 0 ||
        foc_virtual_current_publish_motion(&context, FOC_RUNTIME_MODE_VELOCITY, -4.0f) !=
            FOC_RUNTIME_OK ||
        collect_motion(
            &context,
            FOC_RUNTIME_MODE_VELOCITY,
            -4.0f,
            context.virtual_time_us + 120000U,
            result
        ) != 0) {
        return 1;
    }
    result->name = "V02-velocity-reverse";
    result->reference = -4.0f;
    return check(result->state == FOC_SAFE_STATE_RUNNING, "reverse case left RUNNING");
}

static int
run_transition_case(const struct foc_virtual_current_profile *profile, struct motion_result *result)
{
    struct foc_virtual_current context;
    if (foc_virtual_current_init(&context, profile) != 0 || start_current(&context, 0.35f) != 0) {
        return 1;
    }
    if (refresh_current(&context, 5000U) != 0)
        return 1;
    if (foc_virtual_current_request_stop(&context) != 0)
        return 1;
    if (foc_virtual_current_run_until(&context, 5200U) != 0)
        return 1;
    if (foc_virtual_current_queue_set_mode(&context, FOC_RUNTIME_MODE_VELOCITY) != 0)
        return 1;
    if (foc_virtual_current_run_until(&context, 5400U) != 0)
        return 1;
    if (foc_virtual_current_run_until(&context, 5450U) != 0)
        return 1;
    if (foc_virtual_current_publish_motion(&context, FOC_RUNTIME_MODE_VELOCITY, 3.0f) !=
        FOC_RUNTIME_OK)
        return 1;
    if (foc_virtual_current_run_until(&context, 5500U) != 0)
        return 1;
    if (foc_virtual_current_queue_action(&context, FOC_RUNTIME_ACTION_ARM) != 0)
        return 1;
    if (foc_virtual_current_run_until(&context, 5600U) != 0)
        return 1;
    if (foc_virtual_current_queue_action(&context, FOC_RUNTIME_ACTION_START) != 0)
        return 1;
    if (foc_virtual_current_run_until(&context, 5800U) != 0)
        return 1;
    if (collect_motion(&context, FOC_RUNTIME_MODE_VELOCITY, 3.0f, 30000U, result) != 0)
        return 1;
    result->name = "S01-mode-transition";
    return check(result->state == FOC_SAFE_STATE_RUNNING, "transition did not re-enter RUNNING");
}

static int
run_fault_case(const struct foc_virtual_current_profile *profile, struct motion_result *result)
{
    struct foc_virtual_current timeout_context;
    struct foc_virtual_current sensor_context;
    struct foc_virtual_current_observation timeout_observation;
    struct foc_virtual_current_observation sensor_observation;
    uint32_t sensor_updates_before;
    if (foc_virtual_current_init(&timeout_context, profile) != 0 ||
        start_motion(&timeout_context, FOC_RUNTIME_MODE_VELOCITY, 3.0f) != 0 ||
        foc_virtual_current_run_until(&timeout_context, 5000U) != 0 ||
        foc_virtual_current_observe(&timeout_context, &timeout_observation) != 0 ||
        timeout_observation.safety_state != FOC_SAFE_STATE_FAULT_LATCHED ||
        foc_virtual_current_init(&sensor_context, profile) != 0 ||
        start_motion(&sensor_context, FOC_RUNTIME_MODE_VELOCITY, 3.0f) != 0 ||
        collect_motion(&sensor_context, FOC_RUNTIME_MODE_VELOCITY, 3.0f, 3400U, result) != 0 ||
        foc_virtual_current_fail_next_sensor(&sensor_context) != 0) {
        return 1;
    }
    sensor_updates_before = sensor_context.output.accepted_updates;
    if (foc_virtual_current_run_until(&sensor_context, 3800U) != 0 ||
        foc_virtual_current_observe(&sensor_context, &sensor_observation) != 0) {
        return 1;
    }
    if (sensor_observation.safety_state == FOC_SAFE_STATE_RUNNING ||
        sensor_observation.accepted_updates > sensor_updates_before) {
        return 1;
    }
    *result = (struct motion_result){
        .name = "S02-fault-old-command-isolation",
        .mode = timeout_observation.mode,
        .reference = 3.0f,
        .virtual_time_us = timeout_observation.virtual_time_us,
        .final_position_rad = timeout_observation.mechanical_position_rad,
        .final_velocity_rad_s = timeout_observation.mechanical_velocity_rad_s,
        .state = timeout_observation.safety_state,
        .faults = timeout_observation.faults,
        .motion_session = timeout_observation.motion_session,
        .accepted_updates = timeout_observation.accepted_updates,
        .rejected_updates = timeout_observation.rejected_updates,
        .saturated_updates = timeout_observation.saturated_updates,
        .iq_reference = timeout_observation.iq_ref_a,
        .iq_actual = timeout_observation.iq_a,
        .replay_hash = foc_virtual_current_hash(&timeout_context),
    };
    return check(
        result->state == FOC_SAFE_STATE_FAULT_LATCHED,
        "command timeout did not latch the fault"
    );
}

static void print_result(const struct motion_result *result)
{
    printf(
        "%s mode=%d ref=%g rmse=%g tail_rmse=%g peak=%g overshoot=%g settling_us=%u "
        "max_iq=%g iq_ref=%g iq=%g saturated=%u updates=%u rejected=%u time_us=%u "
        "pos=%g speed=%g state=%d faults=%u session=%u hash=%u%s\n",
        result->name,
        result->mode,
        result->reference,
        result->rmse,
        result->tail_rmse,
        result->peak_error,
        result->overshoot,
        result->settling_us,
        result->max_iq,
        result->iq_reference,
        result->iq_actual,
        result->saturated_updates,
        result->accepted_updates,
        result->rejected_updates,
        result->virtual_time_us,
        result->final_position_rad,
        result->final_velocity_rad_s,
        result->state,
        result->faults,
        result->motion_session,
        result->replay_hash,
        result->expected_degraded ? " EXPECTED_DEGRADED" : ""
    );
}

static void write_csv_header(FILE *file)
{
    fputs(
        "scenario,mode,reference,rmse,tail_rmse,peak_error,overshoot,settling_us,max_iq,"
        "iq_reference_a,iq_actual_a,saturated_updates,accepted_updates,rejected_updates,"
        "virtual_time_us,position_rad,velocity_rad_s,state,faults,motion_session,replay_hash,"
        "expected_degraded\n",
        file
    );
}

static void write_csv_row(FILE *file, const struct motion_result *result)
{
    fprintf(
        file,
        "%s,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%.9g,%.9g,%.9g,%u,%u,%u,%u,%.9g,%.9g,%d,%u,%u,%u,%u\n",
        result->name,
        result->mode,
        result->reference,
        result->rmse,
        result->tail_rmse,
        result->peak_error,
        result->overshoot,
        result->settling_us,
        result->max_iq,
        result->iq_reference,
        result->iq_actual,
        result->saturated_updates,
        result->accepted_updates,
        result->rejected_updates,
        result->virtual_time_us,
        result->final_position_rad,
        result->final_velocity_rad_s,
        result->state,
        result->faults,
        result->motion_session,
        result->replay_hash,
        result->expected_degraded ? 1U : 0U
    );
}

static bool selected(const char *selection, const char *name)
{
    return strcmp(selection, "all") == 0 || strcmp(selection, name) == 0;
}

/* Public Functions ------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *selection = "all";
    FILE *csv = NULL;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--scenario") == 0 && index + 1 < argc) {
            selection = argv[++index];
        } else if (strcmp(argv[index], "--csv") == 0 && index + 1 < argc) {
#ifdef _WIN32
            if (fopen_s(&csv, argv[++index], "w") != 0) {
                csv = NULL;
#else
            csv = fopen(argv[++index], "w");
            if (csv == NULL) {
#endif
                fprintf(stderr, "cannot open CSV output\n");
                return 1;
            }
        } else {
            fprintf(stderr, "usage: falconfoc-virtual-motion [--scenario name] [--csv path]\n");
            return 2;
        }
    }

    if (csv != NULL) {
        write_csv_header(csv);
    }
    const struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    struct motion_result result;
    const struct {
        const char *name;
        enum foc_runtime_mode mode;
        float reference;
        uint32_t duration_us;
        double load_torque_nm;
    } basic[] = {
        {"V01-velocity-step", FOC_RUNTIME_MODE_VELOCITY, 4.0f, 80000U, 0.0},
        {"V03-velocity-load", FOC_RUNTIME_MODE_VELOCITY, 4.0f, 100000U, 0.03},
        {"P01-position-step", FOC_RUNTIME_MODE_POSITION, 1.0f, 200000U, 0.0},
        {"P02-position-multiturn", FOC_RUNTIME_MODE_POSITION, 7.0f, 800000U, 0.0},
    };
    for (unsigned int index = 0U; index < sizeof(basic) / sizeof(basic[0]); ++index) {
        if (selected(selection, basic[index].name) &&
            run_motion_case(
                &profile,
                basic[index].name,
                basic[index].mode,
                basic[index].reference,
                basic[index].duration_us,
                basic[index].load_torque_nm,
                &result
            ) != 0) {
            return 1;
        }
        if (selected(selection, basic[index].name)) {
            print_result(&result);
            if (csv != NULL) {
                write_csv_row(csv, &result);
            }
        }
    }
    if (selected(selection, "V02-velocity-reverse") && run_reverse_case(&profile, &result) != 0) {
        return 1;
    }
    if (selected(selection, "V02-velocity-reverse")) {
        print_result(&result);
        if (csv != NULL) {
            write_csv_row(csv, &result);
        }
    }
    if (selected(selection, "S01-mode-transition") && run_transition_case(&profile, &result) != 0) {
        return 1;
    }
    if (selected(selection, "S01-mode-transition")) {
        print_result(&result);
        if (csv != NULL) {
            write_csv_row(csv, &result);
        }
    }
    if (selected(selection, "S02-fault-old-command-isolation") &&
        run_fault_case(&profile, &result) != 0) {
        return 1;
    }
    if (selected(selection, "S02-fault-old-command-isolation")) {
        print_result(&result);
        if (csv != NULL) {
            write_csv_row(csv, &result);
        }
    }
    if (csv != NULL) {
        fclose(csv);
    }
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
