/**
 * @file    test_foc_virtual_cogging.c
 * @brief   Virtual shaft acquisition, independent holdout, and Runtime A/B.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Test role
 * ---------
 * Compare the independently qualified knots against a virtual shaft oracle.
 * Run the normal Runtime and accepted output path with compensation toggled.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_cogging.h"
#include "foc_virtual_current.h"

#include <math.h>
#include <stdio.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

/* Private Constants ------------------------------------------------------------ */

static const double two_pi = 6.28318530717958647692;
static const double amplitude_nm = 0.031;
static const double phase_rad = 0.37;
static const unsigned int cycles_per_turn = 4U;
static const uint32_t frame_id = 71U;

/* Private Types ---------------------------------------------------------------- */

struct tracking_metrics {
    double velocity_rms_rad_s;
    double velocity_peak_error_rad_s;
    double net_torque_rms_nm;
    uint32_t accepted_updates;
};

/* Private Helpers -------------------------------------------------------------- */

static int calibrate(struct foc_virtual_cogging_result *result)
{
    const struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    struct foc_virtual_motor plant;
    CHECK(foc_virtual_motor_init(&plant, &profile.motor, NULL) == 0);
    CHECK(foc_virtual_motor_set_load(&plant, 0.019) == 0);
    CHECK(foc_virtual_motor_set_load_gradient(&plant, -0.004) == 0);
    CHECK(foc_virtual_motor_set_cogging(&plant, amplitude_nm, cycles_per_turn, phase_rad) == 0);
    const struct foc_virtual_cogging_scan_config config = {
        .motor_id = 42U,
        .mechanical_frame_id = frame_id,
        .training_capture_id = 51U,
        .holdout_capture_id = 52U,
        .speed_rad_s = 8.0,
        .max_holdout_rms_nm = 0.005,
    };

    CHECK(foc_virtual_cogging_calibrate(&plant, &config, result) == 0);
    CHECK(!plant.fixture_active && plant.state.mechanical_position_rad == 0.0);
    CHECK(
        result->lut.qualified && result->lut.motor_id == config.motor_id &&
        result->lut.mechanical_frame_id == config.mechanical_frame_id &&
        result->lut.training_capture_id == config.training_capture_id &&
        result->lut.holdout_capture_id == config.holdout_capture_id
    );
    CHECK(fabs(result->training_load_bias_nm - 0.019) < 1.0e-6);
    CHECK(fabs(result->training_load_slope_nm_per_rad + 0.004) < 1.0e-6);
    CHECK(result->holdout_compensated_rms_nm < result->holdout_baseline_rms_nm * 0.10);
    for (unsigned int i = 0U; i < FOC_RUNTIME_COGGING_POINTS; ++i) {
        const double angle = two_pi * i / FOC_RUNTIME_COGGING_POINTS;
        const double oracle = -amplitude_nm * sin(cycles_per_turn * angle + phase_rad);
        CHECK(fabs((double)result->lut.torque_nm[i] - oracle) < 1.0e-6);
    }

    struct foc_virtual_cogging_result rejected;
    struct foc_virtual_cogging_scan_config invalid = config;
    invalid.holdout_capture_id = invalid.training_capture_id;
    CHECK(
        foc_virtual_cogging_calibrate(&plant, &invalid, &rejected) == FOC_VIRTUAL_COGGING_ERR_INPUT
    );
    CHECK(!rejected.lut.qualified);
    CHECK(foc_virtual_motor_set_cogging(&plant, amplitude_nm, 32U, phase_rad) == 0);
    CHECK(
        foc_virtual_cogging_calibrate(&plant, &config, &rejected) == FOC_VIRTUAL_COGGING_ERR_HOLDOUT
    );
    CHECK(!rejected.lut.qualified);
    return 0;
}

static int start_velocity(struct foc_virtual_current *vm, float reference)
{
    CHECK(foc_virtual_current_run_until(vm, 0U) == 0);
    CHECK(foc_virtual_current_queue_set_mode(vm, FOC_RUNTIME_MODE_VELOCITY) == 0);
    CHECK(foc_virtual_current_run_until(vm, 201U) == 0);
    CHECK(foc_virtual_current_publish_motion(vm, FOC_RUNTIME_MODE_VELOCITY, reference) == 0);
    CHECK(foc_virtual_current_run_until(vm, 500U) == 0);
    CHECK(foc_virtual_current_queue_action(vm, FOC_RUNTIME_ACTION_ARM) == 0);
    CHECK(foc_virtual_current_run_until(vm, 600U) == 0);
    CHECK(foc_virtual_current_queue_action(vm, FOC_RUNTIME_ACTION_START) == 0);
    CHECK(foc_virtual_current_run_until(vm, 800U) == 0);
    return 0;
}

static int tracking(
    const struct foc_runtime_cogging_config *config,
    uint32_t producer_frame,
    float reference,
    struct tracking_metrics *metrics
)
{
    const uint32_t end_us = 6000000U;
    struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    profile.runtime.motion.impedance.mechanical_frame_id = producer_frame;
    profile.runtime.motion.cogging = *config;
    struct foc_virtual_current vm;
    CHECK(foc_virtual_current_init(&vm, &profile) == 0);
    CHECK(foc_virtual_motor_set_cogging(&vm.motor, amplitude_nm, cycles_per_turn, phase_rad) == 0);
    CHECK(start_velocity(&vm, reference) == 0);

    *metrics = (struct tracking_metrics){0};
    double speed_sq = 0.0;
    double torque_sq = 0.0;
    double previous_speed = vm.motor.state.mechanical_velocity_rad_s;
    unsigned int samples = 0U;
    for (uint32_t time = 1000U; time <= end_us; time += 1000U) {
        CHECK(foc_virtual_current_publish_motion(&vm, FOC_RUNTIME_MODE_VELOCITY, reference) == 0);
        CHECK(foc_virtual_current_run_until(&vm, time) == 0);
        const double speed = vm.motor.state.mechanical_velocity_rad_s;
        if (time > 500000U) {
            const double speed_error = speed - reference;
            const double net_torque =
                profile.motor.inertia_kg_m2 * (speed - previous_speed) / 0.001;
            speed_sq += speed_error * speed_error;
            torque_sq += net_torque * net_torque;
            metrics->velocity_peak_error_rad_s =
                fmax(metrics->velocity_peak_error_rad_s, fabs(speed_error));
            samples++;
        }
        previous_speed = speed;
    }
    CHECK(samples > 0U);
    metrics->velocity_rms_rad_s = sqrt(speed_sq / samples);
    metrics->net_torque_rms_nm = sqrt(torque_sq / samples);
    metrics->accepted_updates = vm.output.accepted_updates;
    struct foc_runtime_snapshot snapshot;
    CHECK(foc_virtual_current_snapshot(&vm, &snapshot) == 0);
    CHECK(
        snapshot.state == FOC_SAFE_STATE_RUNNING && snapshot.action == FOC_DEVICE_OUTPUT_UPDATE &&
        snapshot.faults == 0U && snapshot.accepted_outer_seen && snapshot.output_confirmed
    );
    CHECK(vm.output.rejected_updates == 0U && metrics->accepted_updates > 0U);
    CHECK(foc_virtual_current_request_stop(&vm) == 0);
    CHECK(foc_virtual_current_run_until(&vm, end_us + 200U) == 0);
    CHECK(foc_virtual_current_snapshot(&vm, &snapshot) == 0);
    CHECK(snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT && snapshot.output_confirmed);
    CHECK(vm.output.accepted_updates == metrics->accepted_updates);
    return 0;
}

static int accepted_iq(struct foc_virtual_current *vm, float *iq)
{
    struct foc_runtime_snapshot snapshot;
    CHECK(foc_virtual_current_snapshot(vm, &snapshot) == 0);
    CHECK(
        snapshot.faults == 0U && snapshot.action == FOC_DEVICE_OUTPUT_UPDATE &&
        snapshot.accepted_outer_seen
    );
    const unsigned int slot = atomic_load_explicit(&vm->runtime.sensor_slot, memory_order_acquire);
    CHECK(
        slot < 2U && vm->runtime.sensor_slots[slot].target_valid &&
        vm->runtime.sensor_slots[slot].sample.seq == snapshot.accepted_outer_seq
    );
    *iq = vm->runtime.sensor_slots[slot].target.target_a.q;
    return 0;
}

static int test_cogging_cannot_drive_out_of_motion_limits(struct foc_runtime_cogging_config config)
{
    struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    profile.runtime.motion.cogging = config;
    profile.runtime.motion.limits = (struct foc_motion_limits_config){
        .enabled = true,
        .mechanical_frame_id = frame_id,
        .position_min_rad = 0.0f,
        .position_max_rad = 1.0f,
    };
    struct foc_virtual_current vm;
    CHECK(foc_virtual_current_init(&vm, &profile) == 0);
    CHECK(vm.runtime.config.motion.cogging.enabled && config.lut.torque_nm[0] < 0.0f);
    CHECK(foc_virtual_motor_set_fixture_speed(&vm.motor, 0.0) == 0);
    CHECK(start_velocity(&vm, 0.0f) == 0);
    CHECK(foc_virtual_current_run_until(&vm, 2000U) == 0);
    float iq;
    CHECK(accepted_iq(&vm, &iq) == 0);
    CHECK(iq == 0.0f);
    return 0;
}

static int test_impedance_feedforward_cap(struct foc_runtime_cogging_config config)
{
    const float maximum_feedforward_nm = 0.02f;
    struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    profile.runtime.motion.cogging = config;
    profile.runtime.motion.impedance = (struct foc_runtime_impedance_config){
        .enabled = true,
        .mechanical_frame_id = frame_id,
        .torque_constant_nm_per_a = config.torque_constant_nm_per_a,
        .torque_limit_nm = 0.15f,
        .max_stiffness_nm_per_rad = 1.0f,
        .max_damping_nm_s_per_rad = 0.1f,
        .max_feedforward_nm = maximum_feedforward_nm,
    };
    struct foc_virtual_current vm;
    CHECK(foc_virtual_current_init(&vm, &profile) == 0);
    CHECK(foc_virtual_motor_set_fixture_speed(&vm.motor, 0.0) == 0);
    CHECK(foc_virtual_current_run_until(&vm, 0U) == 0);
    CHECK(
        foc_virtual_current_publish_lifecycle(
            &vm,
            FOC_RUNTIME_ACTION_SET_MODE,
            FOC_RUNTIME_MODE_IMPEDANCE
        ) == 0
    );
    CHECK(foc_virtual_current_run_until(&vm, 201U) == 0);
    struct foc_runtime_snapshot snapshot;
    CHECK(foc_virtual_current_snapshot(&vm, &snapshot) == 0);
    const struct foc_runtime_command command = {
        .mode = FOC_RUNTIME_MODE_IMPEDANCE,
        .seq = 1U,
        .produced_us = vm.virtual_time_us,
        .target.impedance = {
            .mechanical_frame_id = frame_id,
            .source_session = snapshot.motion_session,
            .torque_feedforward_nm = -maximum_feedforward_nm,
        },
    };
    CHECK(foc_runtime_publish_impedance_command(&vm.runtime, &command, vm.virtual_time_us) == 0);
    CHECK(foc_virtual_current_run_until(&vm, 500U) == 0);
    CHECK(foc_virtual_current_queue_action(&vm, FOC_RUNTIME_ACTION_ARM) == 0);
    CHECK(foc_virtual_current_run_until(&vm, 600U) == 0);
    CHECK(foc_virtual_current_queue_action(&vm, FOC_RUNTIME_ACTION_START) == 0);
    CHECK(foc_virtual_current_run_until(&vm, 2000U) == 0);
    float iq;
    CHECK(accepted_iq(&vm, &iq) == 0);
    CHECK(fabsf(iq + maximum_feedforward_nm / config.torque_constant_nm_per_a) < 1.0e-7f);
    return 0;
}

static int check_improvement(
    const char *direction,
    const struct tracking_metrics *off,
    const struct tracking_metrics *on
)
{
    printf(
        "cogging %s velocity RMS off=%.8g on=%.8g rad/s, "
        "net shaft torque RMS off=%.8g on=%.8g N*m\n",
        direction,
        off->velocity_rms_rad_s,
        on->velocity_rms_rad_s,
        off->net_torque_rms_nm,
        on->net_torque_rms_nm
    );
    CHECK(
        on->velocity_rms_rad_s < 0.55 * off->velocity_rms_rad_s &&
        on->velocity_peak_error_rad_s < off->velocity_peak_error_rad_s &&
        on->net_torque_rms_nm < 0.55 * off->net_torque_rms_nm &&
        on->accepted_updates == off->accepted_updates
    );
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    struct foc_virtual_cogging_result calibration;
    CHECK(calibrate(&calibration) == 0);
    struct foc_runtime_cogging_config config = {
        .enabled = false,
        .motor_id = calibration.lut.motor_id,
        .mechanical_frame_id = frame_id,
        .torque_constant_nm_per_a = 0.15f,
        .iq_limit_a = 0.3f,
        .lut = calibration.lut,
    };
    const float speed_rad_s = 1.4f;
    struct tracking_metrics off, on, reverse_off, reverse_on;
    CHECK(tracking(&config, frame_id, speed_rad_s, &off) == 0);
    CHECK(tracking(&config, frame_id, -speed_rad_s, &reverse_off) == 0);
    config.enabled = true;
    CHECK(tracking(&config, frame_id, speed_rad_s, &on) == 0);
    CHECK(tracking(&config, frame_id, -speed_rad_s, &reverse_on) == 0);
    CHECK(test_cogging_cannot_drive_out_of_motion_limits(config) == 0);
    CHECK(test_impedance_feedforward_cap(config) == 0);
    printf(
        "cogging holdout off=%.8g compensated=%.8g N*m\n",
        calibration.holdout_baseline_rms_nm,
        calibration.holdout_compensated_rms_nm
    );
    CHECK(check_improvement("positive", &off, &on) == 0);
    CHECK(check_improvement("negative", &reverse_off, &reverse_on) == 0);

    struct tracking_metrics mismatched_motor, mismatched_frame, invalid_lut;
    CHECK(tracking(&config, frame_id + 1U, speed_rad_s, &mismatched_frame) == 0);
    config.motor_id++;
    CHECK(tracking(&config, frame_id, speed_rad_s, &mismatched_motor) == 0);
    config.motor_id--;
    config.lut.torque_nm[0] = NAN;
    CHECK(tracking(&config, frame_id, speed_rad_s, &invalid_lut) == 0);
    CHECK(
        mismatched_frame.velocity_rms_rad_s == off.velocity_rms_rad_s &&
        mismatched_motor.velocity_rms_rad_s == off.velocity_rms_rad_s &&
        invalid_lut.velocity_rms_rad_s == off.velocity_rms_rad_s
    );
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
