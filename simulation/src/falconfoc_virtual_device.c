/**
 * @file    falconfoc_virtual_device.c
 * @brief   Host-only Falcon V1 virtual device over binary stdio.
 * @version v0.1.0
 * @date    2026.11.16
 *
 * Implementation role
 * -------------------
 * Reuse the production COBS/CRC/Session/Sequence/Serial Adapter with the
 * VM-01b deterministic Runtime and Plant. stdout is protocol bytes only;
 * lifecycle and virtual-physical metadata are emitted on stderr.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_serial.h"
#include "foc_virtual_current.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/* Private Types ---------------------------------------------------------------- */

struct virtual_device {
    struct foc_virtual_current current;
    struct foc_virtual_current_profile profile;
    struct foc_serial serial;
    struct foc_uart_buffer uart;
    uint32_t advance_us;
    uint32_t metadata_sequence;
    enum foc_runtime_request_result metadata_result;
    uint32_t load_on_us;
    uint32_t load_off_us;
    uint32_t sensor_fault_us;
    double load_torque_nm;
    bool load_applied;
    bool sensor_fault_injected;
    unsigned int startup_frames;
    float motion_ref;
    bool reference_override;
    char profile_id[64];
};

/* Private Helpers -------------------------------------------------------------- */

static uint32_t virtual_clock(void *argument)
{
    const struct virtual_device *device = argument;
    return device->current.virtual_time_us;
}

static void write_metadata(const struct virtual_device *device)
{
    struct foc_runtime_lifecycle_result lifecycle;
    if (foc_serial_read_lifecycle_result(
            &device->serial,
            (struct foc_runtime *)&device->current.runtime,
            &lifecycle
        ) == FOC_RUNTIME_OK &&
        (lifecycle.request_sequence != device->metadata_sequence ||
         lifecycle.result != device->metadata_result)) {
        const char *result = lifecycle.result == FOC_RUNTIME_REQUEST_PENDING ? "PENDING"
            : lifecycle.result == FOC_RUNTIME_REQUEST_APPLIED                ? "APPLIED"
            : lifecycle.result == FOC_RUNTIME_REQUEST_REJECTED               ? "REJECTED"
            : lifecycle.result == FOC_RUNTIME_REQUEST_CANCELLED              ? "CANCELLED"
                                                                             : "NONE";
        fprintf(
            stderr,
            "FALCONF0C_META {\"kind\":\"lifecycle\",\"result\":\"%s\","
            "\"request_sequence\":%u,\"action\":%d,\"state\":%d,\"faults\":%u,"
            "\"h0_us\":%u,\"pipeline_status\":%d}\n",
            result,
            lifecycle.request_sequence,
            lifecycle.action,
            lifecycle.state,
            lifecycle.faults,
            lifecycle.h0_us,
            lifecycle.pipeline_status
        );
        ((struct virtual_device *)device)->metadata_sequence = lifecycle.request_sequence;
        ((struct virtual_device *)device)->metadata_result = lifecycle.result;
    }
    struct foc_virtual_current_observation observation;
    if (foc_virtual_current_observe(&device->current, &observation) == 0) {
        struct foc_runtime_snapshot snapshot;
        (void)foc_virtual_current_snapshot(&device->current, &snapshot);
        const bool velocity_mode = observation.mode == FOC_RUNTIME_MODE_VELOCITY;
        const bool position_mode = observation.mode == FOC_RUNTIME_MODE_POSITION;
        fprintf(
            stderr,
            "FALCONF0C_META {\"kind\":\"virtual\",\"simulated\":true,"
            "\"profile_id\":\"%s\","
            "\"virtual_time_us\":%u,\"control_mode\":%d,\"command_sequence\":%u,"
            "\"motion_session\":%u,\"iq_target_a\":%.9g,\"id_actual_a\":%.9g,"
            "\"iq_actual_a\":%.9g,\"velocity_target_rad_s\":%.9g,"
            "\"velocity_actual_rad_s\":%.9g,\"position_target_rad\":%.9g,"
            "\"position_actual_rad\":%.9g,\"vbus_v\":%.9g,"
            "\"voltage_saturated\":%s,\"saturation_count\":%u,"
            "\"virtual_update_accepted\":%u,\"virtual_update_rejected\":%u,"
            "\"simulated_current_valid\":%s,\"simulated_sensor_valid\":%s,"
            "\"physical_applied\":false,\"mode\":%d,\"state\":%d,"
            "\"faults\":%u,\"inhibit_pending\":%s,\"output_action\":%d,"
            "\"load_torque_nm\":%.9g,"
            "\"fault_injection_us\":%d,\"sensor_failure_observed_us\":%d,"
            "\"h1_invalid_observation_us\":%d,\"h0_failure_observed_us\":%d,"
            "\"inhibit_requested_us\":%d,\"inhibit_accepted_us\":%d,"
            "\"fault_latched_us\":%d,\"last_accepted_update_us\":%d,"
            "\"updates_at_injection\":%u,\"updates_at_sensor_failure\":%u,"
            "\"updates_at_h1_invalid\":%u,\"updates_at_h0_failure\":%u,"
            "\"updates_at_inhibit_requested\":%u,\"updates_at_inhibit_accepted\":%u,"
            "\"updates_at_fault_latched\":%u,"
            "\"replay_hash\":%u}\n",
            device->profile_id,
            observation.virtual_time_us,
            observation.mode,
            snapshot.command_seq,
            observation.motion_session,
            observation.iq_ref_a,
            observation.id_a,
            observation.iq_a,
            velocity_mode ? device->motion_ref : 0.0f,
            observation.mechanical_velocity_rad_s,
            position_mode ? device->motion_ref : 0.0f,
            observation.mechanical_position_rad,
            device->current.profile.vbus_v,
            observation.saturated_updates > 0U ? "true" : "false",
            observation.saturated_updates,
            observation.accepted_updates,
            observation.rejected_updates,
            observation.safety_state == FOC_SAFE_STATE_RUNNING && observation.accepted_updates > 0U
                ? "true"
                : "false",
            observation.safety_state == FOC_SAFE_STATE_RUNNING ? "true" : "false",
            observation.mode,
            observation.safety_state,
            observation.faults,
            snapshot.inhibit_pending ? "true" : "false",
            snapshot.action,
            device->current.motor.load_torque_nm,
            device->current.fault_events.injection_seen
                ? (int)device->current.fault_events.injection_us
                : -1,
            device->current.fault_events.sensor_failure_seen
                ? (int)device->current.fault_events.sensor_failure_us
                : -1,
            device->current.fault_events.h1_invalid_seen
                ? (int)device->current.fault_events.h1_invalid_us
                : -1,
            device->current.fault_events.h0_failure_seen
                ? (int)device->current.fault_events.h0_failure_us
                : -1,
            device->current.fault_events.inhibit_requested_seen
                ? (int)device->current.fault_events.inhibit_requested_us
                : -1,
            device->current.fault_events.inhibit_accepted_seen
                ? (int)device->current.fault_events.inhibit_accepted_us
                : -1,
            device->current.fault_events.fault_latched_seen
                ? (int)device->current.fault_events.fault_latched_us
                : -1,
            device->current.fault_events.last_update_seen
                ? (int)device->current.fault_events.last_accepted_update_us
                : -1,
            device->current.fault_events.updates_at_injection,
            device->current.fault_events.updates_at_sensor_failure,
            device->current.fault_events.updates_at_h1_invalid,
            device->current.fault_events.updates_at_h0_failure,
            device->current.fault_events.updates_at_inhibit_requested,
            device->current.fault_events.updates_at_inhibit_accepted,
            device->current.fault_events.updates_at_fault_latched,
            foc_virtual_current_hash(&device->current)
        );
    }
}

static int drain_tx(struct virtual_device *device)
{
    uint8_t byte;
    while (foc_uart_tx_pop(&device->uart, &byte)) {
        if (putchar((int)byte) == EOF) {
            return -1;
        }
    }
    fflush(stdout);
    return 0;
}

static int advance_virtual_time(struct virtual_device *device)
{
    const uint32_t target_us = device->current.virtual_time_us + device->advance_us;
    while (device->current.virtual_time_us < target_us) {
        uint32_t next_us = device->current.virtual_time_us + 100U;
        if (next_us > target_us) {
            next_us = target_us;
        }
        if (!device->load_applied && device->load_on_us > device->current.virtual_time_us &&
            device->load_on_us < next_us) {
            next_us = device->load_on_us;
        }
        if (device->load_applied && device->load_off_us > device->current.virtual_time_us &&
            device->load_off_us < next_us) {
            next_us = device->load_off_us;
        }
        if (!device->sensor_fault_injected &&
            device->sensor_fault_us > device->current.virtual_time_us &&
            device->sensor_fault_us < next_us) {
            next_us = device->sensor_fault_us;
        }
        if (!device->sensor_fault_injected && next_us == device->sensor_fault_us) {
            if (foc_virtual_current_fail_next_sensor(&device->current) != 0) {
                return -1;
            }
            if (foc_virtual_current_mark_sensor_fault(&device->current, next_us) != 0) {
                return -1;
            }
            device->sensor_fault_injected = true;
        }
        if (foc_virtual_current_run_until(&device->current, next_us) != 0) {
            return -1;
        }
        if (!device->load_applied && device->load_on_us != 0U &&
            device->current.virtual_time_us == device->load_on_us) {
            if (foc_virtual_motor_set_load(&device->current.motor, device->load_torque_nm) != 0) {
                return -1;
            }
            device->load_applied = true;
        }
        if (device->load_applied && device->load_off_us != 0U &&
            device->current.virtual_time_us == device->load_off_us) {
            if (foc_virtual_motor_set_load(&device->current.motor, 0.0) != 0) {
                return -1;
            }
            device->load_applied = false;
        }
    }
    write_metadata(device);
    return 0;
}

static int initialize_device(struct virtual_device *device, enum foc_runtime_mode mode)
{
    if (foc_virtual_current_init(&device->current, &device->profile) != 0) {
        return -1;
    }
    device->metadata_result = FOC_RUNTIME_REQUEST_NONE;
    if (!device->reference_override) {
        device->motion_ref = mode == FOC_RUNTIME_MODE_POSITION ? 7.0f
            : mode == FOC_RUNTIME_MODE_VELOCITY                ? 3.0f
                                                               : 0.4f;
    }
    if (mode != FOC_RUNTIME_MODE_CURRENT &&
        foc_virtual_current_publish_lifecycle(
            &device->current,
            FOC_RUNTIME_ACTION_SET_MODE,
            mode
        ) != FOC_RUNTIME_OK) {
        return -1;
    }
    if (foc_virtual_current_run_until(&device->current, 0U) != 0) {
        return -1;
    }
    foc_serial_init_with_policy(
        &device->serial,
        &device->current.runtime,
        FOC_SERIAL_TARGET_VIRTUAL_ONLY
    );
    write_metadata(device);
    return 0;
}

static enum foc_runtime_mode parse_mode(const char *value)
{
    if (strcmp(value, "current") == 0) {
        return FOC_RUNTIME_MODE_CURRENT;
    }
    if (strcmp(value, "velocity") == 0) {
        return FOC_RUNTIME_MODE_VELOCITY;
    }
    if (strcmp(value, "position") == 0) {
        return FOC_RUNTIME_MODE_POSITION;
    }
    return (enum foc_runtime_mode) - 1;
}

/* Public Functions ------------------------------------------------------------- */

int main(int argc, char **argv)
{
#ifdef _WIN32
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
    enum foc_runtime_mode mode = FOC_RUNTIME_MODE_CURRENT;
    struct virtual_device device = {.advance_us = 500U, .startup_frames = 0U};
    device.profile = foc_virtual_current_default_profile();
    (void)snprintf(device.profile_id, sizeof device.profile_id, "%s", "GENERIC_RESEARCH_MOTOR");
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--mode") == 0 && index + 1 < argc) {
            mode = parse_mode(argv[++index]);
        } else if (strcmp(argv[index], "--advance-us") == 0 && index + 1 < argc) {
            device.advance_us = (uint32_t)strtoul(argv[++index], NULL, 0);
        } else if (strcmp(argv[index], "--load-on-us") == 0 && index + 1 < argc) {
            device.load_on_us = (uint32_t)strtoul(argv[++index], NULL, 0);
        } else if (strcmp(argv[index], "--load-off-us") == 0 && index + 1 < argc) {
            device.load_off_us = (uint32_t)strtoul(argv[++index], NULL, 0);
        } else if (strcmp(argv[index], "--load-torque-nm") == 0 && index + 1 < argc) {
            device.load_torque_nm = strtod(argv[++index], NULL);
        } else if (strcmp(argv[index], "--sensor-fault-us") == 0 && index + 1 < argc) {
            device.sensor_fault_us = (uint32_t)strtoul(argv[++index], NULL, 0);
        } else if (strcmp(argv[index], "--profile-id") == 0 && index + 1 < argc) {
            (void)snprintf(device.profile_id, sizeof device.profile_id, "%s", argv[++index]);
        } else if (strcmp(argv[index], "--reference") == 0 && index + 1 < argc) {
            device.motion_ref = strtof(argv[++index], NULL);
            device.reference_override = true;
        } else if (strcmp(argv[index], "--current-kp") == 0 && index + 1 < argc) {
            const float value = strtof(argv[++index], NULL);
            device.profile.current.kp_v_per_a.d = value;
            device.profile.current.kp_v_per_a.q = value;
        } else if (strcmp(argv[index], "--current-ki") == 0 && index + 1 < argc) {
            const float value = strtof(argv[++index], NULL);
            device.profile.current.ki_v_per_a_s.d = value;
            device.profile.current.ki_v_per_a_s.q = value;
        } else if (strcmp(argv[index], "--velocity-kp") == 0 && index + 1 < argc) {
            device.profile.runtime.motion.velocity.kp_a_per_rad_s = strtof(argv[++index], NULL);
        } else if (strcmp(argv[index], "--velocity-ki") == 0 && index + 1 < argc) {
            device.profile.runtime.motion.velocity.ki_a_per_rad = strtof(argv[++index], NULL);
        } else if (strcmp(argv[index], "--position-kp") == 0 && index + 1 < argc) {
            device.profile.runtime.motion.position.kp_per_s = strtof(argv[++index], NULL);
        } else if (strcmp(argv[index], "--current-limit-a") == 0 && index + 1 < argc) {
            const float value = strtof(argv[++index], NULL);
            device.profile.current.current_limit_a = value;
            device.profile.runtime.motion.iq_limit_a = value;
        } else if (strcmp(argv[index], "--voltage-limit-v") == 0 && index + 1 < argc) {
            device.profile.current.pwm.voltage_limit_v = strtof(argv[++index], NULL);
        } else if (strcmp(argv[index], "--vbus-v") == 0 && index + 1 < argc) {
            device.profile.vbus_v = strtof(argv[++index], NULL);
        } else if (strcmp(argv[index], "--motor-rs-ohm") == 0 && index + 1 < argc) {
            device.profile.motor.rs_ohm = strtod(argv[++index], NULL);
        } else if (strcmp(argv[index], "--motor-ld-h") == 0 && index + 1 < argc) {
            device.profile.motor.ld_h = strtod(argv[++index], NULL);
        } else if (strcmp(argv[index], "--motor-lq-h") == 0 && index + 1 < argc) {
            device.profile.motor.lq_h = strtod(argv[++index], NULL);
        } else if (strcmp(argv[index], "--motor-psi-f-wb") == 0 && index + 1 < argc) {
            device.profile.motor.psi_f_wb = strtod(argv[++index], NULL);
        } else if (strcmp(argv[index], "--motor-inertia") == 0 && index + 1 < argc) {
            device.profile.motor.inertia_kg_m2 = strtod(argv[++index], NULL);
        } else if (strcmp(argv[index], "--motor-damping") == 0 && index + 1 < argc) {
            device.profile.motor.damping_nm_s = strtod(argv[++index], NULL);
        } else {
            fprintf(
                stderr,
                "usage: falconfoc-virtual-device [--mode current|velocity|position] "
                "[--advance-us us] [--load-on-us us --load-off-us us --load-torque-nm nm] "
                "[--sensor-fault-us us] [--profile-id id] [--reference value] "
                "[--current-kp value --current-ki value --velocity-kp value --velocity-ki value "
                "--position-kp value --current-limit-a value --voltage-limit-v value --vbus-v "
                "value] "
                "[--motor-rs-ohm value --motor-ld-h value --motor-lq-h value "
                "--motor-psi-f-wb value --motor-inertia value --motor-damping value]\n"
            );
            return 2;
        }
    }
    if (mode < FOC_RUNTIME_MODE_CURRENT || mode > FOC_RUNTIME_MODE_POSITION ||
        initialize_device(&device, mode) != 0) {
        return 1;
    }
    for (;;) {
        const int byte = getchar();
        if (byte == EOF) {
            break;
        }
        foc_uart_rx_push(&device.uart, (uint8_t)byte, false);
        foc_serial_poll(
            &device.serial,
            &device.uart,
            &device.current.runtime,
            virtual_clock,
            &device
        );
        if (byte == 0) {
            if (drain_tx(&device) != 0) {
                return 1;
            }
            if (device.startup_frames > 0U) {
                --device.startup_frames;
            } else if (advance_virtual_time(&device) != 0) {
                return 1;
            }
        }
    }
    return ferror(stdin) ? 1 : 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
