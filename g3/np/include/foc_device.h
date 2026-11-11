/**
 * @file    foc_device.h
 * @brief   Host-only Board1 ADC and PWM mapping contract for G3-NP.
 * @version v0.1.0
 * @date    2026.11.09
 *
 * Design overview
 * ---------------
 * This header defines the provisional boundary between the portable G2 Core
 * and a future Board1 Device owner. It converts raw ADC rank snapshots into
 * physical values and converts Core phase duties into TMR1 channel requests.
 * It contains no vendor register access and is not part of the firmware build.
 *
 * Usage example
 * -------------
 * 1. Validate an ADC and PWM configuration.
 * 2. Call foc_device_decode_sample() for a complete raw sample.
 * 3. Pass the resulting VBUS snapshot to foc_step().
 * 4. Call foc_device_map_output() and inspect its explicit action before any
 *    future hardware adapter writes registers.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The caller owns raw samples, decoded measurements, configurations, and
 * requests. Functions are non-blocking, do not retain pointers, and are safe
 * to use from host tests. The production Device layer must own the eventual
 * ADC/PWM register and bridge-enable policy.
 *
 * Failure / recovery
 * ------------------
 * Invalid input returns a status and leaves a deterministic inhibit request or
 * zeroed decoded measurement. `INHIBIT`, `UPDATE`, `ZERO_DUTY`, and
 * `ACTIVE_BRAKE` are distinct actions. A zero compare value never infers a
 * bridge action.
 */
#pragma once

#include "foc_core.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types ---------------------------------------------------------------- */

/** Status returned by a Device conversion or mapping operation. */
enum foc_device_status {
    /** Operation completed successfully. */
    FOC_DEVICE_STATUS_OK = 0,
    /** A required pointer was NULL. */
    FOC_DEVICE_STATUS_ERR_NULL = -1,
    /** A Device configuration was invalid. */
    FOC_DEVICE_STATUS_ERR_INVALID_CONFIG = -2,
    /** A sample was missing, incomplete, or physically invalid. */
    FOC_DEVICE_STATUS_ERR_INVALID_SAMPLE = -3,
    /** A sample was repeated or outside its permitted age. */
    FOC_DEVICE_STATUS_ERR_STALE_SAMPLE = -4,
    /** A caller-supplied time value was invalid. */
    FOC_DEVICE_STATUS_ERR_INVALID_TIME = -5,
    /** A Core output could not be represented as a valid request. */
    FOC_DEVICE_STATUS_ERR_INVALID_OUTPUT = -6,
    /** The simulator or future Device is not in a running state. */
    FOC_DEVICE_STATUS_ERR_NOT_RUNNING = -7,
    /** A latched fault has priority over the requested operation. */
    FOC_DEVICE_STATUS_ERR_FAULT_LATCHED = -8,
    /** The Core rejected the Device-provided snapshot. */
    FOC_DEVICE_STATUS_ERR_CORE = -9,
};

/** Board phase ordering used when interpreting ranks and mapping channels. */
enum foc_device_phase_order {
    /** Rank and channel ordering follows the Board1 ABC contract. */
    FOC_DEVICE_PHASE_ORDER_ABC = 0,
    /** Physical B and C labels are exchanged while A remains fixed. */
    FOC_DEVICE_PHASE_ORDER_ACB = 1,
};

/** Explicit action requested from a future bridge adapter. */
enum foc_device_output_action {
    /** Disable the bridge independently of compare values. */
    FOC_DEVICE_OUTPUT_INHIBIT = 0,
    /** Apply normalized duties through the configured channel mapping. */
    FOC_DEVICE_OUTPUT_UPDATE = 1,
    /** Request an enabled zero-duty behavior, distinct from inhibit. */
    FOC_DEVICE_OUTPUT_ZERO_DUTY = 2,
    /** Request an enabled active-brake behavior, pending hardware policy. */
    FOC_DEVICE_OUTPUT_ACTIVE_BRAKE = 3,
};

/** Validity bits required for one complete injected ADC snapshot. */
enum foc_device_sample_validity {
    /** VBUS rank is valid. */
    FOC_DEVICE_SAMPLE_VBUS_VALID = 1U << 0,
    /** ADC rank 2 is valid. */
    FOC_DEVICE_SAMPLE_RANK2_VALID = 1U << 1,
    /** ADC rank 3 is valid. */
    FOC_DEVICE_SAMPLE_RANK3_VALID = 1U << 2,
    /** ADC rank 4 is valid. */
    FOC_DEVICE_SAMPLE_RANK4_VALID = 1U << 3,
    /** All ranks required by the Board1 conversion are valid. */
    FOC_DEVICE_SAMPLE_ALL_VALID = 0x0FU,
};

/** Physical conversion factors and phase-order facts for raw ADC samples. */
struct foc_device_adc_config {
    /** VBUS volts per ADC count. */
    float vbus_v_per_count;
    /** Current amperes per ADC count after amplifier scaling. */
    float current_a_per_count;
    /** Minimum accepted decoded VBUS in volts. */
    float vbus_min_v;
    /** Physical phase-A raw offset in ADC counts. */
    float phase_a_offset_count;
    /** Physical phase-B raw offset in ADC counts. */
    float phase_b_offset_count;
    /** Physical phase-C raw offset in ADC counts. */
    float phase_c_offset_count;
    /** Rank-to-phase ordering used for this board configuration. */
    enum foc_device_phase_order phase_order;
};

/** Raw injected ADC snapshot copied from a producer-owned buffer. */
struct foc_device_adc_raw {
    /** VBUS rank-1 raw ADC value. */
    uint16_t vbus_raw;
    /** Injected ADC rank-2 raw value. */
    uint16_t rank2_raw;
    /** Injected ADC rank-3 raw value. */
    uint16_t rank3_raw;
    /** Injected ADC rank-4 raw value. */
    uint16_t rank4_raw;
    /** Validity bits that must contain FOC_DEVICE_SAMPLE_ALL_VALID. */
    uint32_t valid_mask;
    /** Producer sequence number used for freshness checks. */
    uint32_t sample_seq;
    /** Producer timestamp in the caller's monotonic tick domain. */
    uint32_t sample_tick;
};

/** Physical measurement snapshot produced without retaining raw input pointers. */
struct foc_device_measurement {
    /** VBUS snapshot passed to the G2 Core. */
    struct foc_measurement core;
    /** Reconstructed physical phase currents in amperes. */
    struct foc_vector_abc phase_current_a;
    /** Sequence copied from the accepted raw sample. */
    uint32_t sample_seq;
    /** Timestamp copied from the accepted raw sample. */
    uint32_t sample_tick;
};

/** Timer compare range and phase-to-channel mapping facts. */
struct foc_device_pwm_config {
    /** Timer period in compare counts. */
    uint32_t period_counts;
    /** Phase-to-channel ordering used by the future bridge adapter. */
    enum foc_device_phase_order phase_order;
};

/** Complete action and compare request emitted by the Device mapper. */
struct foc_device_pwm_request {
    /** Explicit bridge action. */
    enum foc_device_output_action action;
    /** Non-zero only for actions that request an enabled behavior. */
    unsigned int output_enable_request;
    /** Phase-A normalized duty before channel mapping. */
    float duty_a;
    /** Phase-B normalized duty before channel mapping. */
    float duty_b;
    /** Phase-C normalized duty before channel mapping. */
    float duty_c;
    /** Compare value for TMR1 channel 1. */
    uint32_t channel1_compare;
    /** Compare value for TMR1 channel 2. */
    uint32_t channel2_compare;
    /** Compare value for TMR1 channel 3. */
    uint32_t channel3_compare;
};

/* Public Functions ------------------------------------------------------------- */

/* Configuration and conversion ----------------------------------------------- */

/** Validate physical ADC conversion factors and phase order. */
int foc_device_adc_config_validate(const struct foc_device_adc_config *config);

/** Validate timer period and phase-to-channel mapping configuration. */
int foc_device_pwm_config_validate(const struct foc_device_pwm_config *config);

/** Decode physical quantities. All sequence values are copied without judging order. */
int foc_device_decode_sample(const struct foc_device_adc_raw *raw,
                             const struct foc_device_adc_config *config,
                             struct foc_device_measurement *measurement);

/* Output actions ---------------------------------------------------------------- */

/** Build a bridge-inhibit request with deterministic zero compare metadata. */
int foc_device_output_inhibit(const struct foc_device_pwm_config *config,
                              struct foc_device_pwm_request *request);

/** Build an explicit zero-duty request without changing it into an inhibit. */
int foc_device_output_zero_duty(const struct foc_device_pwm_config *config,
                                struct foc_device_pwm_request *request);

/** Build an explicit active-brake request pending production hardware policy. */
int foc_device_output_active_brake(const struct foc_device_pwm_config *config,
                                   struct foc_device_pwm_request *request);

/** Map a successful Core output into Board1 timer channels or inhibit it. */
int foc_device_map_output(const struct foc_output *core_output,
                          const struct foc_device_pwm_config *config,
                          struct foc_device_pwm_request *request);

/* ---------------------------------------- end of file ----------------------------------------- */
