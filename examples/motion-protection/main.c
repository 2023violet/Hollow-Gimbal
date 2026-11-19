/**
 * @file    main.c
 * @brief   Verify motion protection against a deterministic virtual PMSM.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Implementation role
 * -------------------
 * Exercise public Runtime and Virtual Motor APIs. All positions are continuous
 * motor-shaft radians. These scenarios establish Host software behavior.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_current.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>

#define CHECK(expression)                                                                     \
    do {                                                                                      \
        if (!(expression)) {                                                                  \
            fprintf(stderr, "%s:%d condition failed: %s\n", __func__, __LINE__, #expression); \
            return 1;                                                                         \
        }                                                                                     \
    } while (0)

#define CHECK_STATUS(call, expected)                   \
    do {                                               \
        const int actual_status = (call);              \
        if (actual_status != (expected)) {             \
            fprintf(                                   \
                stderr,                                \
                "%s:%d %s returned %d, expected %d\n", \
                __func__,                              \
                __LINE__,                              \
                #call,                                 \
                actual_status,                         \
                (expected)                             \
            );                                         \
            return 1;                                  \
        }                                              \
    } while (0)

struct fixture {
    struct foc_virtual_current vm;
    enum foc_runtime_mode mode;
    uint32_t sequence;
};

struct reference {
    float position_rad;
    float velocity_rad_s;
};

/* Private Helpers -------------------------------------------------------------- */

static const uint32_t frame_id = 81U;

static struct foc_virtual_current_profile
profile_for(float min_rad, float max_rad, double initial_rad, double initial_rad_s)
{
    struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    profile.initial_state.mechanical_position_rad = initial_rad;
    profile.initial_state.mechanical_velocity_rad_s = initial_rad_s;
    profile.runtime.motion.limits = (struct foc_motion_limits_config){
        .position_min_rad = min_rad,
        .position_max_rad = max_rad,
        .tolerance_rad = 0.0005f,
        .mechanical_frame_id = frame_id,
        .enabled = true,
    };
    profile.runtime.motion.impedance = (struct foc_runtime_impedance_config){
        .enabled = true,
        .mechanical_frame_id = frame_id,
        .torque_constant_nm_per_a =
            (float)(1.5 * profile.motor.pole_pairs * profile.motor.psi_f_wb),
        .torque_limit_nm = 0.315f,
        .max_stiffness_nm_per_rad = 1.0f,
        .max_damping_nm_s_per_rad = 0.10f,
        .max_feedforward_nm = 0.30f,
    };
    return profile;
}

static int request(
    struct fixture *fixture,
    enum foc_runtime_action action,
    uint32_t due_us,
    enum foc_safe_state expected_state
)
{
    CHECK_STATUS(foc_virtual_current_publish_lifecycle(&fixture->vm, action, fixture->mode), 0);
    struct foc_runtime_lifecycle_result result;
    CHECK_STATUS(foc_virtual_current_read_lifecycle(&fixture->vm, &result), 0);
    CHECK(result.result == FOC_RUNTIME_REQUEST_PENDING);
    const uint32_t request_sequence = result.request_sequence;
    CHECK_STATUS(foc_virtual_current_run_until(&fixture->vm, due_us), 0);
    CHECK_STATUS(foc_virtual_current_read_lifecycle(&fixture->vm, &result), 0);
    CHECK(
        result.result == FOC_RUNTIME_REQUEST_APPLIED && result.state == expected_state &&
        result.h0_us == due_us && result.request_sequence == request_sequence
    );
    return 0;
}

static struct foc_runtime_command new_impedance(
    struct fixture *fixture,
    uint32_t now_us,
    uint32_t session,
    struct reference reference,
    float feedforward_nm
)
{
    return (struct foc_runtime_command){
        .mode = FOC_RUNTIME_MODE_IMPEDANCE,
        .seq = ++fixture->sequence,
        .produced_us = now_us,
        .target.impedance = {
            .position_rad = reference.position_rad,
            .velocity_rad_s = reference.velocity_rad_s,
            .stiffness_nm_per_rad = 0.65f,
            .damping_nm_s_per_rad = 0.048f,
            .torque_feedforward_nm = feedforward_nm,
            .mechanical_frame_id = frame_id,
            .source_session = session,
        },
    };
}

static int
publish(struct fixture *fixture, uint32_t at_us, struct reference reference, float feedforward_nm)
{
    if (fixture->mode == FOC_RUNTIME_MODE_IMPEDANCE) {
        struct foc_runtime_snapshot snapshot;
        if (foc_virtual_current_snapshot(&fixture->vm, &snapshot) != FOC_RUNTIME_OK) {
            return FOC_RUNTIME_ERR_BUSY;
        }
        const struct foc_runtime_command command =
            new_impedance(fixture, at_us, snapshot.motion_session, reference, feedforward_nm);
        return foc_runtime_publish_impedance_command(&fixture->vm.runtime, &command, at_us);
    }
    const struct foc_runtime_command command = {
        .mode = fixture->mode,
        .seq = ++fixture->sequence,
        .produced_us = at_us,
        .target.motion_ref = fixture->mode == FOC_RUNTIME_MODE_POSITION ? reference.position_rad
                                                                        : reference.velocity_rad_s,
    };
    return foc_runtime_publish_motion_command(&fixture->vm.runtime, &command, at_us);
}

static int start(
    struct fixture *fixture,
    const struct foc_virtual_current_profile *profile,
    enum foc_runtime_mode mode,
    struct reference initial_reference
)
{
    fixture->mode = mode;
    fixture->sequence = 0U;
    CHECK_STATUS(foc_virtual_current_init(&fixture->vm, profile), 0);
    CHECK_STATUS(foc_virtual_current_run_until(&fixture->vm, 0U), 0);
    CHECK(request(fixture, FOC_RUNTIME_ACTION_SET_MODE, 200U, FOC_SAFE_STATE_DISABLED) == 0);
    CHECK_STATUS(foc_virtual_current_run_until(&fixture->vm, 201U), 0);
    CHECK_STATUS(publish(fixture, 201U, initial_reference, 0.0f), FOC_RUNTIME_OK);
    CHECK_STATUS(foc_virtual_current_run_until(&fixture->vm, 500U), 0);
    CHECK(request(fixture, FOC_RUNTIME_ACTION_ARM, 600U, FOC_SAFE_STATE_READY) == 0);
    CHECK(request(fixture, FOC_RUNTIME_ACTION_START, 800U, FOC_SAFE_STATE_RUNNING) == 0);
    CHECK_STATUS(foc_virtual_current_run_until(&fixture->vm, 999U), 0);
    return 0;
}

static int validate_inhibit(struct fixture *fixture, uint32_t old_session, uint32_t updates)
{
    struct foc_runtime_snapshot snapshot;
    CHECK_STATUS(foc_virtual_current_snapshot(&fixture->vm, &snapshot), 0);
    CHECK(snapshot.state != FOC_SAFE_STATE_RUNNING && snapshot.motion_session != old_session);
    CHECK(
        snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT && snapshot.output_confirmed &&
        !snapshot.inhibit_pending && snapshot.updates == updates
    );
    return 0;
}

static int configuration_and_mode(void)
{
    struct foc_virtual_current vm;
    struct foc_virtual_current_profile profile = profile_for(0.10f, 0.70f, 0.30, 0.0);
    struct foc_virtual_current_profile invalid = profile;
    invalid.runtime.motion.limits.position_min_rad = 0.80f;
    CHECK_STATUS(foc_virtual_current_init(&vm, &invalid), -1);
    invalid = profile;
    invalid.runtime.motion.limits.position_max_rad = NAN;
    CHECK_STATUS(foc_virtual_current_init(&vm, &invalid), -1);
    invalid = profile;
    invalid.runtime.motion.limits.mechanical_frame_id = frame_id + 1U;
    CHECK_STATUS(foc_virtual_current_init(&vm, &invalid), -1);
    invalid = profile;
    invalid.runtime.motion.following_error = (struct foc_following_error_config){
        .enabled = true,
        .threshold_rad = 0.02f,
        .duration_us = 0U,
        .max_gap_us = 1000U,
    };
    CHECK_STATUS(foc_virtual_current_init(&vm, &invalid), -1);

    CHECK_STATUS(foc_virtual_current_init(&vm, &profile), 0);
    CHECK_STATUS(foc_virtual_current_run_until(&vm, 0U), 0);
    CHECK_STATUS(foc_virtual_current_publish_iq(&vm, 0.0f, 0.10f), FOC_RUNTIME_ERR_MOTION_MODE);
    struct foc_runtime_snapshot snapshot;
    CHECK_STATUS(foc_virtual_current_snapshot(&vm, &snapshot), 0);
    CHECK(
        snapshot.mode == FOC_RUNTIME_MODE_CURRENT && snapshot.updates == 0U &&
        snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT && snapshot.output_confirmed
    );
    printf(
        "CONFIG invalid_interval/NaN/frame/time all rejected; CURRENT soft-limits unsupported\n"
    );
    return 0;
}

static int bounded_position(enum foc_runtime_mode mode, bool multi_turn)
{
    struct fixture fixture;
    const float min_rad = multi_turn ? 6.0f : 0.1f;
    const float max_rad = multi_turn ? 6.7f : 0.7f;
    const float start_rad = multi_turn ? 6.15f : 0.3f;
    const float target_rad = multi_turn ? 6.55f : 0.45f;
    const struct foc_virtual_current_profile profile =
        profile_for(min_rad, max_rad, start_rad, 0.0);
    const struct reference hold = {.position_rad = start_rad};
    CHECK(start(&fixture, &profile, mode, hold) == 0);

    CHECK_STATUS(
        publish(&fixture, 999U, (struct reference){max_rad + 0.02f, 0.0f}, 0.0f),
        FOC_RUNTIME_ERR_MOTION_LIMIT
    );
    CHECK_STATUS(
        publish(&fixture, 999U, (struct reference){min_rad - 0.02f, 0.0f}, 0.0f),
        FOC_RUNTIME_ERR_MOTION_LIMIT
    );
    const struct reference target = {.position_rad = target_rad};
    CHECK_STATUS(publish(&fixture, 999U, target, 0.0f), FOC_RUNTIME_OK);
    const uint32_t test_duration_us = multi_turn ? 1200000U : 900000U;
    for (uint32_t at_us = 1999U; at_us < test_duration_us; at_us += 1000U) {
        CHECK_STATUS(foc_virtual_current_run_until(&fixture.vm, at_us), 0);
        CHECK_STATUS(publish(&fixture, at_us, target, 0.0f), FOC_RUNTIME_OK);
    }
    CHECK_STATUS(foc_virtual_current_run_until(&fixture.vm, test_duration_us), 0);
    struct foc_runtime_snapshot snapshot;
    CHECK_STATUS(foc_virtual_current_snapshot(&fixture.vm, &snapshot), 0);
    CHECK(
        snapshot.state == FOC_SAFE_STATE_RUNNING && snapshot.sensor_valid &&
        snapshot.motion_rejection == FOC_MOTION_REJECT_NONE && snapshot.updates > 100U
    );
    CHECK(fabsf(snapshot.mechanical_rad - target_rad) < 0.02f);
    if (multi_turn) {
        CHECK(snapshot.mechanical_rad > 6.283185f);
    }
    printf(
        "%s mode=%u high/low targets rejected, position_rad=%.6f target_rad=%.3f updates=%" PRIu32
        "\n",
        multi_turn ? "MULTI_TURN" : "IN_BOUNDS",
        (unsigned)mode,
        (double)snapshot.mechanical_rad,
        (double)target_rad,
        snapshot.updates
    );
    return 0;
}

static int return_from_outside(bool upper)
{
    struct fixture fixture;
    const double initial = upper ? 0.62 : 0.13;
    const enum foc_runtime_mode mode =
        upper ? FOC_RUNTIME_MODE_POSITION : FOC_RUNTIME_MODE_VELOCITY;
    struct foc_virtual_current_profile profile = profile_for(0.20f, 0.50f, initial, 0.0);
    /* Use a recovery profile that maintains inward q while approaching the range. */
    profile.load_torque_nm = upper ? -0.025 : 0.008;
    profile.runtime.motion.velocity.kp_a_per_rad_s = 0.5f;
    profile.runtime.motion.velocity.ki_a_per_rad = 0.0f;
    const struct reference inward = {.position_rad = 0.34f, .velocity_rad_s = upper ? 0.0f : 0.3f};
    CHECK(start(&fixture, &profile, mode, inward) == 0);
    CHECK_STATUS(publish(&fixture, 999U, inward, 0.0f), FOC_RUNTIME_OK);

    struct foc_runtime_snapshot initial_snapshot;
    CHECK_STATUS(foc_virtual_current_run_until(&fixture.vm, 1000U), 0);
    CHECK_STATUS(foc_virtual_current_snapshot(&fixture.vm, &initial_snapshot), 0);
    CHECK(
        initial_snapshot.updates == 1U && initial_snapshot.output_confirmed &&
        (upper ? initial_snapshot.mechanical_rad > 0.50f : initial_snapshot.mechanical_rad < 0.20f)
    );

    const uint32_t until_us = 500000U;
    for (uint32_t at_us = 1999U; at_us < until_us; at_us += 1000U) {
        CHECK_STATUS(foc_virtual_current_run_until(&fixture.vm, at_us), 0);
        struct foc_runtime_snapshot stage;
        CHECK_STATUS(foc_virtual_current_snapshot(&fixture.vm, &stage), 0);
        if (stage.state != FOC_SAFE_STATE_RUNNING) {
            struct foc_virtual_current_observation observation;
            CHECK_STATUS(foc_virtual_current_observe(&fixture.vm, &observation), 0);
            fprintf(
                stderr,
                "return lost at %lu: state=%d reason=%d pipeline=%d faults=%lx pos=%.8f vel=%.8f "
                "updates=%lu q=%.8f\n",
                (unsigned long)at_us,
                stage.state,
                stage.motion_rejection,
                stage.pipeline_status,
                (unsigned long)stage.faults,
                (double)stage.mechanical_rad,
                (double)stage.mechanical_rad_s,
                (unsigned long)stage.updates,
                (double)observation.iq_ref_a
            );
        }
        CHECK(stage.state == FOC_SAFE_STATE_RUNNING);
        CHECK_STATUS(publish(&fixture, at_us, inward, 0.0f), FOC_RUNTIME_OK);
    }
    CHECK_STATUS(foc_virtual_current_run_until(&fixture.vm, until_us), 0);
    struct foc_runtime_snapshot snapshot;
    CHECK_STATUS(foc_virtual_current_snapshot(&fixture.vm, &snapshot), 0);
    CHECK(
        snapshot.state == FOC_SAFE_STATE_RUNNING &&
        snapshot.motion_session == initial_snapshot.motion_session &&
        snapshot.motion_rejection == FOC_MOTION_REJECT_NONE && snapshot.updates > 100U
    );
    CHECK(snapshot.mechanical_rad > 0.20f && snapshot.mechanical_rad < 0.50f);
    printf(
        "INWARD_RETURN mode=%u start_rad=%.5f final_rad=%.5f updates=%" PRIu32 "\n",
        (unsigned)mode,
        (double)initial_snapshot.mechanical_rad,
        (double)snapshot.mechanical_rad,
        snapshot.updates
    );
    return 0;
}

static int veto_outward(bool use_feedforward)
{
    struct fixture fixture;
    const enum foc_runtime_mode mode =
        use_feedforward ? FOC_RUNTIME_MODE_IMPEDANCE : FOC_RUNTIME_MODE_VELOCITY;
    const struct foc_virtual_current_profile profile = profile_for(0.20f, 0.50f, 0.58, 0.0);
    const struct reference inward = {
        .position_rad = 0.43f,
        .velocity_rad_s = use_feedforward ? 0.0f : -0.3f
    };
    CHECK(start(&fixture, &profile, mode, inward) == 0);

    /* Position alone points inward. +0.3 N*m exceeds the inward spring term. */
    const struct reference outward = {
        .position_rad = 0.43f,
        .velocity_rad_s = use_feedforward ? 0.0f : 0.3f
    };
    struct foc_runtime_snapshot prior;
    CHECK_STATUS(foc_virtual_current_snapshot(&fixture.vm, &prior), 0);
    CHECK_STATUS(publish(&fixture, 999U, outward, use_feedforward ? 0.30f : 0.0f), FOC_RUNTIME_OK);
    CHECK_STATUS(foc_virtual_current_run_until(&fixture.vm, 1000U), 0);
    CHECK(validate_inhibit(&fixture, prior.motion_session, prior.updates) == 0);
    struct foc_runtime_snapshot stopped;
    CHECK_STATUS(foc_virtual_current_snapshot(&fixture.vm, &stopped), 0);
    CHECK(stopped.motion_rejection == FOC_MOTION_REJECT_DIRECTION && stopped.faults == 0U);
    printf(
        "OUTWARD_VETO request=%s update_delta=%" PRIu32 " inhibit_confirmed=%u\n",
        use_feedforward ? "net_q_with_feedforward" : "velocity",
        stopped.updates - prior.updates,
        (unsigned)stopped.output_confirmed
    );
    return 0;
}

static int inertial_crossing(void)
{
    struct fixture fixture;
    const float upper = 0.52f;
    const struct foc_virtual_current_profile profile = profile_for(0.2f, upper, 0.5, 6.0);
    const struct reference target = {.position_rad = upper, .velocity_rad_s = 4.0f};
    CHECK(start(&fixture, &profile, FOC_RUNTIME_MODE_IMPEDANCE, target) == 0);
    struct foc_runtime_snapshot prior;
    CHECK_STATUS(foc_virtual_current_snapshot(&fixture.vm, &prior), 0);
    uint32_t confirmed_updates = 0U;
    uint32_t stop_us = 0U;
    float stop_position = 0.0f;
    float stop_velocity = 0.0f;
    for (uint32_t at_us = 999U; at_us < 9999U; at_us += 200U) {
        CHECK_STATUS(foc_virtual_current_run_until(&fixture.vm, at_us), 0);
        CHECK_STATUS(publish(&fixture, at_us, target, 0.25f), FOC_RUNTIME_OK);
        CHECK_STATUS(foc_virtual_current_run_until(&fixture.vm, at_us + 1U), 0);
        struct foc_runtime_snapshot snapshot;
        CHECK_STATUS(foc_virtual_current_snapshot(&fixture.vm, &snapshot), 0);
        if (snapshot.motion_rejection == FOC_MOTION_REJECT_DIRECTION) {
            CHECK(snapshot.updates == confirmed_updates);
            stop_us = snapshot.trigger_us;
            stop_position = snapshot.mechanical_rad;
            stop_velocity = snapshot.mechanical_rad_s;
            break;
        }
        CHECK(snapshot.state == FOC_SAFE_STATE_RUNNING);
        confirmed_updates = snapshot.updates;
    }
    CHECK(stop_us > 1000U && confirmed_updates != 0U && stop_velocity > 0.0f);
    CHECK(validate_inhibit(&fixture, prior.motion_session, confirmed_updates) == 0);
    CHECK_STATUS(foc_virtual_current_run_until(&fixture.vm, stop_us + 4000U), 0);
    struct foc_virtual_current_observation observation;
    CHECK_STATUS(foc_virtual_current_observe(&fixture.vm, &observation), 0);
    CHECK(
        observation.mechanical_position_rad > upper &&
        observation.accepted_updates == confirmed_updates
    );
    printf(
        "INERTIAL_CROSSING upper_rad=%.3f inhibit_us=%" PRIu32
        " at_inhibit_rad=%.6f velocity_rad_s=%.4f after_4ms_rad=%.6f updates=%" PRIu32 "\n",
        (double)upper,
        stop_us,
        (double)stop_position,
        (double)stop_velocity,
        observation.mechanical_position_rad,
        confirmed_updates
    );
    return 0;
}

static int overloaded_following_stop(void)
{
    struct fixture fixture;
    struct foc_virtual_current_profile profile = profile_for(0.10f, 0.90f, 0.40, 0.0);
    const uint32_t duration_us = 20000U;
    profile.runtime.motion.following_error = (struct foc_following_error_config){
        .enabled = true,
        .threshold_rad = 0.02f,
        .duration_us = duration_us,
        .max_gap_us = 1000U,
    };
    /* Constant load exceeds the 0.315 N*m command cap. */
    profile.load_torque_nm = 0.55;
    CHECK(
        start(&fixture, &profile, FOC_RUNTIME_MODE_IMPEDANCE, (struct reference){0.40f, 0.0f}) == 0
    );
    struct foc_runtime_snapshot prior;
    CHECK_STATUS(foc_virtual_current_snapshot(&fixture.vm, &prior), 0);

    uint32_t first_excess_source_us = 0U;
    uint32_t confirmed_updates = 0U;
    struct foc_runtime_snapshot stopped = {0};
    const struct reference target = {.position_rad = 0.68f};
    for (uint32_t at_us = 999U; at_us < 60000U; at_us += 200U) {
        CHECK_STATUS(foc_virtual_current_run_until(&fixture.vm, at_us), 0);
        CHECK_STATUS(publish(&fixture, at_us, target, 0.0f), FOC_RUNTIME_OK);
        CHECK_STATUS(foc_virtual_current_run_until(&fixture.vm, at_us + 1U), 0);
        struct foc_runtime_snapshot snapshot;
        CHECK_STATUS(foc_virtual_current_snapshot(&fixture.vm, &snapshot), 0);
        if (snapshot.motion_rejection == FOC_MOTION_REJECT_FOLLOWING) {
            CHECK(snapshot.following_state == FOC_FOLLOWING_TRIGGERED);
            CHECK(snapshot.updates == confirmed_updates);
            stopped = snapshot;
            break;
        }
        if (snapshot.following_state == FOC_FOLLOWING_PENDING && first_excess_source_us == 0U) {
            first_excess_source_us = snapshot.sensor_us;
        }
        CHECK(snapshot.state == FOC_SAFE_STATE_RUNNING);
        confirmed_updates = snapshot.updates;
    }
    CHECK(
        first_excess_source_us != 0U && stopped.trigger_us >= first_excess_source_us + duration_us
    );
    CHECK(stopped.motion_rejection == FOC_MOTION_REJECT_FOLLOWING && stopped.faults == 0U);
    CHECK(validate_inhibit(&fixture, prior.motion_session, confirmed_updates) == 0);
    const uint32_t after_us = stopped.trigger_us + 2000U;
    CHECK_STATUS(foc_virtual_current_run_until(&fixture.vm, after_us), 0);
    const struct foc_runtime_command stale =
        new_impedance(&fixture, after_us, prior.motion_session, target, 0.0f);
    CHECK_STATUS(
        foc_runtime_publish_impedance_command(&fixture.vm.runtime, &stale, after_us),
        FOC_RUNTIME_ERR_ORDER
    );
    CHECK(validate_inhibit(&fixture, prior.motion_session, confirmed_updates) == 0);
    printf(
        "FOLLOWING_STOP load_Nm=%.2f cap_Nm=%.3f first_excess_source_us=%" PRIu32
        " inhibit_us=%" PRIu32 " threshold_time_us=%" PRIu32 " updates=%" PRIu32
        " old_session_rejected=true faults=0x%" PRIx32 "\n",
        profile.load_torque_nm,
        (double)profile.runtime.motion.impedance.torque_limit_nm,
        first_excess_source_us,
        stopped.trigger_us,
        duration_us,
        confirmed_updates,
        stopped.faults
    );
    return 0;
}

static struct reference bounded_transition(uint32_t at_us, uint32_t start_us, float from, float to)
{
    /* Same 400 ms quintic profile as the existing Servo example, shifted on this shaft. */
    const float duration_s = 0.4f;
    const float u = fminf((float)(at_us - start_us) * 1.0e-6f / duration_s, 1.0f);
    const float delta = to - from;
    const float tail = 1.0f - u;
    return (struct reference){
        .position_rad = from + delta * u * u * u * (10.0f + u * (-15.0f + 6.0f * u)),
        .velocity_rad_s = delta * 30.0f * u * u * tail * tail / duration_s,
    };
}

static int transient_and_reverse(void)
{
    struct fixture fixture;
    struct foc_virtual_current_profile profile = profile_for(0.10f, 0.90f, 0.35, 0.0);
    profile.runtime.motion.following_error = (struct foc_following_error_config){
        .enabled = true,
        .threshold_rad = 0.02f,
        .duration_us = 300000U,
        .max_gap_us = 1000U,
    };
    CHECK(
        start(&fixture, &profile, FOC_RUNTIME_MODE_IMPEDANCE, (struct reference){0.35f, 0.0f}) == 0
    );
    unsigned pending_count = 0U;
    float max_error = 0.0f;
    struct foc_runtime_snapshot snapshot = {0};
    for (uint32_t at_us = 999U; at_us < 1750000U; at_us += 1000U) {
        struct reference reference = {.position_rad = 0.35f};
        if (at_us >= 900000U) {
            reference = bounded_transition(at_us, 900000U, 0.57f, 0.25f);
        } else if (at_us >= 300000U) {
            reference = bounded_transition(at_us, 300000U, 0.35f, 0.57f);
        }
        CHECK_STATUS(foc_virtual_current_run_until(&fixture.vm, at_us), 0);
        CHECK_STATUS(publish(&fixture, at_us, reference, 0.0f), FOC_RUNTIME_OK);
        CHECK_STATUS(foc_virtual_current_run_until(&fixture.vm, at_us + 1U), 0);
        CHECK_STATUS(foc_virtual_current_snapshot(&fixture.vm, &snapshot), 0);
        CHECK(
            snapshot.state == FOC_SAFE_STATE_RUNNING &&
            snapshot.motion_rejection == FOC_MOTION_REJECT_NONE &&
            snapshot.following_state != FOC_FOLLOWING_TRIGGERED
        );
        if (snapshot.following_state == FOC_FOLLOWING_PENDING) {
            pending_count++;
        }
        max_error = fmaxf(max_error, fabsf(reference.position_rad - snapshot.mechanical_rad));
    }
    CHECK(pending_count != 0U && snapshot.following_state == FOC_FOLLOWING_TRACKING);
    CHECK(fabsf(snapshot.mechanical_rad - 0.25f) < 0.01f);
    printf(
        "BOUNDED_REVERSAL pending_observations=%u max_error_rad=%.6f final_rad=%.6f no_trip=true\n",
        pending_count,
        (double)max_error,
        (double)snapshot.mechanical_rad
    );
    return 0;
}

enum fault_case {
    INVALID_SENSOR,
    REJECTED_ADAPTER,
    EXTERNAL_STOP,
};

static int rejection_or_stop(enum fault_case fault)
{
    struct fixture fixture;
    struct foc_virtual_current_profile profile = profile_for(0.1f, 0.7f, 0.30, 0.0);
    profile.runtime.motion.following_error = (struct foc_following_error_config){
        .enabled = true,
        .threshold_rad = 0.02f,
        .duration_us = 20000U,
        .max_gap_us = 1000U,
    };
    CHECK(
        start(&fixture, &profile, FOC_RUNTIME_MODE_IMPEDANCE, (struct reference){0.30f, 0.0f}) == 0
    );
    CHECK_STATUS(publish(&fixture, 999U, (struct reference){0.40f, 0.0f}, 0.0f), FOC_RUNTIME_OK);
    struct foc_runtime_snapshot prior;
    CHECK_STATUS(foc_virtual_current_snapshot(&fixture.vm, &prior), 0);
    if (fault == INVALID_SENSOR) {
        CHECK_STATUS(foc_virtual_current_fail_next_sensor(&fixture.vm), 0);
    } else if (fault == REJECTED_ADAPTER) {
        CHECK_STATUS(foc_virtual_current_reject_next_update(&fixture.vm), 0);
    } else {
        CHECK_STATUS(foc_virtual_current_request_stop(&fixture.vm), 0);
        struct foc_runtime_snapshot pending;
        CHECK_STATUS(foc_virtual_current_snapshot(&fixture.vm, &pending), 0);
        CHECK(
            pending.inhibit_pending && !pending.output_confirmed && pending.updates == prior.updates
        );
    }
    CHECK_STATUS(foc_virtual_current_run_until(&fixture.vm, 1000U), 0);
    CHECK(validate_inhibit(&fixture, prior.motion_session, prior.updates) == 0);
    struct foc_virtual_current_observation observation;
    CHECK_STATUS(foc_virtual_current_observe(&fixture.vm, &observation), 0);
    CHECK(observation.rejected_updates == (fault == REJECTED_ADAPTER ? 1U : 0U));
    struct foc_runtime_snapshot stopped;
    CHECK_STATUS(foc_virtual_current_snapshot(&fixture.vm, &stopped), 0);
    if (fault == INVALID_SENSOR) {
        CHECK(!stopped.sensor_valid);
    }
    CHECK(stopped.motion_rejection != FOC_MOTION_REJECT_FOLLOWING);
    printf(
        "OUTPUT_GUARD event=%s updates=%" PRIu32 " adapter_rejections=%" PRIu32
        " pending=false confirmed=true\n",
        fault == INVALID_SENSOR
            ? "invalid_sensor"
            : (fault == REJECTED_ADAPTER ? "adapter_reject" : "independent_stop"),
        stopped.updates,
        observation.rejected_updates
    );
    return 0;
}

/* Public Functions ------------------------------------------------------------- */

int main(void)
{
    CHECK(configuration_and_mode() == 0);
    CHECK(bounded_position(FOC_RUNTIME_MODE_POSITION, false) == 0);
    CHECK(bounded_position(FOC_RUNTIME_MODE_IMPEDANCE, false) == 0);
    CHECK(return_from_outside(true) == 0);
    CHECK(return_from_outside(false) == 0);
    CHECK(veto_outward(false) == 0);
    CHECK(veto_outward(true) == 0);
    CHECK(inertial_crossing() == 0);
    CHECK(overloaded_following_stop() == 0);
    CHECK(transient_and_reverse() == 0);
    CHECK(rejection_or_stop(INVALID_SENSOR) == 0);
    CHECK(rejection_or_stop(REJECTED_ADAPTER) == 0);
    CHECK(rejection_or_stop(EXTERNAL_STOP) == 0);
    CHECK(bounded_position(FOC_RUNTIME_MODE_POSITION, true) == 0);
    puts("PASS falconfoc-motion-protection-demo physical_applied=false");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
