/**
 * @file    foc_virtual_calibration_package.c
 * @brief   Encode and validate reviewed Host calibration independently of Config Store.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Implementation role
 * -------------------
 * FCAL v1 is a fixed little-endian word stream with binary32 values and a
 * trailing CRC32. Runtime limits, timing and callbacks come from the selected
 * local profile. CRC checks integrity; review still belongs to the operator.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_calibration_package.h"

#include "crc.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Private Types ---------------------------------------------------------------- */

enum package_word {
    MAGIC,
    VERSION,
    MOTOR,
    ENCODER,
    FRAME,
    FULL_SCALE,
    CURRENT_SCALE,
    CPR,
    DIRECTION,
    ORDER,
    POLES,
    SESSION,
    D_CAPTURE,
    Q_CAPTURE,
    MECHANICAL_CAPTURE,
    KT_CAPTURE,
    ADC_A,
    ADC_B,
    ADC_C,
    OFFSET,
    RS,
    LD,
    LQ,
    FLUX,
    KT,
    INERTIA,
    DAMPING,
    KP_D,
    KP_Q,
    KI,
    KP_VELOCITY,
    KI_VELOCITY,
    KP_POSITION,
    K_IMPEDANCE,
    D_IMPEDANCE,
    COGGING_ENABLED,
    COGGING_TRAIN,
    COGGING_HOLDOUT,
    COGGING_LIMIT,
    COGGING_DATA,
    LUT_LENGTH = COGGING_DATA + FOC_RUNTIME_COGGING_POINTS,
    LUT_DATA,
    CRC = LUT_DATA + FOC_ENCODER_CAL_MAX_NODES,
    WORDS,
};

/* Private Constants ------------------------------------------------------------ */

static const uint32_t FCAL_MAGIC = UINT32_C(0x4C414346);
_Static_assert(
    sizeof(float) == 4U && FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128,
    "Host package requires IEEE binary32"
);

/* Private Helpers -------------------------------------------------------------- */

static uint32_t bits(float value)
{
    uint32_t word;
    memcpy(&word, &value, sizeof word);
    return word;
}

static float number(uint32_t word)
{
    float value;
    memcpy(&value, &word, sizeof value);
    return value;
}

static uint32_t checksum(const uint8_t *bytes, size_t size)
{
    return ~crc32_ieee(UINT32_MAX, bytes, size);
}

static uint32_t get_word(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8U | (uint32_t)bytes[2] << 16U |
        (uint32_t)bytes[3] << 24U;
}

static void put_word(uint8_t *bytes, uint32_t value)
{
    for (unsigned int i = 0U; i < 4U; ++i) {
        bytes[i] = (uint8_t)(value >> (8U * i));
    }
}

static bool bounded(float value, float lower, float upper)
{
    return isfinite(value) && value >= lower && value <= upper;
}

static bool matches_identity(const uint32_t *words, const struct foc_cal_workflow_identity *id)
{
    const struct foc_calibration_identity *acq = &id->acquisition;
    return words[MOTOR] == id->motor_setup_id && words[ENCODER] == id->encoder_id &&
        words[FRAME] == id->mechanical_frame_id &&
        words[FULL_SCALE] == acq->adc_full_scale_counts &&
        words[CPR] == acq->encoder_counts_per_revolution && words[ORDER] == acq->phase_order &&
        words[DIRECTION] == (uint32_t)acq->encoder_direction && words[POLES] == acq->pole_pairs &&
        words[CURRENT_SCALE] == bits(acq->current_a_per_count);
}

static bool distinct_captures(const uint32_t *words)
{
    const uint32_t ids[] = {
        words[D_CAPTURE],
        words[Q_CAPTURE],
        words[MECHANICAL_CAPTURE],
        words[KT_CAPTURE],
        words[COGGING_TRAIN],
        words[COGGING_HOLDOUT]
    };
    const size_t count = words[COGGING_ENABLED] != 0U ? 6U : 4U;
    for (size_t i = 0U; i < count; ++i) {
        if (ids[i] == 0U) {
            return false;
        }
        for (size_t j = 0U; j < i; ++j) {
            if (ids[i] == ids[j]) {
                return false;
            }
        }
    }
    return true;
}

static int decode(
    const uint32_t *w,
    const struct foc_virtual_current_profile *profile,
    const struct foc_cal_workflow_identity *identity,
    const struct foc_cal_application_tuning_policy *policy,
    struct foc_virtual_calibration_package *package
)
{
    if (!matches_identity(w, identity) || w[SESSION] == 0U || !distinct_captures(w)) {
        return FOC_VIRTUAL_PACKAGE_IDENTITY;
    }
    if (w[LUT_LENGTH] != 32U || w[LUT_LENGTH] > FOC_ENCODER_CAL_MAX_NODES ||
        w[COGGING_ENABLED] > 1U || !bounded(number(w[ADC_A]), 32.0f, 4063.0f) ||
        !bounded(number(w[ADC_B]), 32.0f, 4063.0f) || !bounded(number(w[ADC_C]), 32.0f, 4063.0f) ||
        !bounded(number(w[OFFSET]), -3.141593f, 3.141593f) ||
        !bounded(number(w[RS]), 0.01f, 5.0f) || !bounded(number(w[LD]), 0.00005f, 0.05f) ||
        !bounded(number(w[LQ]), 0.00005f, 0.05f) || !bounded(number(w[FLUX]), 0.001f, 0.25f) ||
        !bounded(number(w[INERTIA]), 0.0005f, 0.1f) || !bounded(number(w[DAMPING]), 0.0f, 0.1f) ||
        fabsf(number(w[KT]) - 1.5f * (float)w[POLES] * number(w[FLUX])) > 1.0e-5f ||
        !bounded(number(w[KT]), 0.001f, 2.0f)) {
        return FOC_VIRTUAL_PACKAGE_RANGE;
    }
    struct foc_virtual_calibration_package next = {
        .candidate =
            {
                .pipeline =
                    {.adc = profile->adc,
                     .current = profile->current,
                     .pwm = profile->pwm,
                     .safety = profile->safety},
                .runtime.config = profile->runtime,
                .encoder = profile->encoder,
                .has_lut = true,
                .gains =
                    {
                        .identity = *identity,
                        .session = w[SESSION],
                        .d_capture = w[D_CAPTURE],
                        .q_capture = w[Q_CAPTURE],
                        .mechanical_capture = w[MECHANICAL_CAPTURE],
                        .kt_capture = w[KT_CAPTURE],
                        .model = {number(w[RS]), number(w[LD]), number(w[LQ])},
                        .torque_constant_nm_per_a = number(w[KT]),
                        .outer_tuned = true,
                    },
            },
        .lut =
            {.length = w[LUT_LENGTH],
             .counts_per_revolution = w[CPR],
             .encoder_identity = w[ENCODER]},
        .flux_wb = number(w[FLUX]),
        .inertia_kg_m2 = number(w[INERTIA]),
        .damping_nm_s = number(w[DAMPING]),
    };
    struct foc_cal_application_candidate *candidate = &next.candidate;
    struct foc_cal_application_gains *gains = &candidate->gains;
    struct foc_runtime_motion_config *motion = &candidate->runtime.config.motion;
    if (foc_motor_pi_design(&gains->model, &policy->current, &gains->current) != FOC_MOTOR_ID_OK ||
        fabsf(gains->current.kp_d_v_per_a - number(w[KP_D])) > 1.0e-6f ||
        fabsf(gains->current.kp_q_v_per_a - number(w[KP_Q])) > 1.0e-6f ||
        fabsf(gains->current.ki_v_per_a_s - number(w[KI])) > 1.0e-4f ||
        !bounded(number(w[KP_D]), 0.0f, 10.0f) || !bounded(number(w[KP_Q]), 0.0f, 10.0f) ||
        !bounded(number(w[KI]), 0.0f, 1500.0f) || !bounded(number(w[KP_VELOCITY]), 0.0f, 3.0f) ||
        !bounded(number(w[KI_VELOCITY]), 0.0f, 60.0f) ||
        !bounded(number(w[KP_POSITION]), 0.0f, 10.0f) ||
        !bounded(number(w[K_IMPEDANCE]), 0.0f, motion->impedance.max_stiffness_nm_per_rad) ||
        !bounded(number(w[D_IMPEDANCE]), 0.0f, motion->impedance.max_damping_nm_s_per_rad)) {
        return FOC_VIRTUAL_PACKAGE_RANGE;
    }
    candidate->pipeline.adc.phase_a_offset_count = number(w[ADC_A]);
    candidate->pipeline.adc.phase_b_offset_count = number(w[ADC_B]);
    candidate->pipeline.adc.phase_c_offset_count = number(w[ADC_C]);
    candidate->pipeline.current.kp_v_per_a =
        (struct foc_vector_dq){number(w[KP_D]), number(w[KP_Q])};
    candidate->pipeline.current.ki_v_per_a_s = (struct foc_vector_dq){number(w[KI]), number(w[KI])};
    motion->rotor.pole_pairs = (uint16_t)w[POLES];
    motion->rotor.electrical_offset_rad = number(w[OFFSET]);
    motion->impedance.torque_constant_nm_per_a = gains->torque_constant_nm_per_a;
    struct foc_impedance_tuning_input impedance = foc_virtual_calibration_impedance_policy(profile);
    impedance.model_id = w[MOTOR];
    impedance.inertia_kg_m2 = next.inertia_kg_m2;
    impedance.viscous_damping_nm_s_per_rad = next.damping_nm_s;
    impedance.torque_constant_nm_per_a = gains->torque_constant_nm_per_a;
    if (!foc_cal_application_design_outer(
            policy,
            &candidate->runtime.config,
            next.inertia_kg_m2,
            next.damping_nm_s,
            gains->torque_constant_nm_per_a,
            &motion->velocity,
            &motion->position
        ) ||
        foc_impedance_tuning_design(&impedance, &gains->impedance) != FOC_IMPEDANCE_TUNING_OK ||
        fabsf(motion->velocity.kp_a_per_rad_s - number(w[KP_VELOCITY])) > 1.0e-6f ||
        fabsf(motion->velocity.ki_a_per_rad - number(w[KI_VELOCITY])) > 1.0e-5f ||
        fabsf(motion->position.kp_per_s - number(w[KP_POSITION])) > 1.0e-6f ||
        fabsf(gains->impedance.stiffness_nm_per_rad - number(w[K_IMPEDANCE])) > 1.0e-6f ||
        fabsf(gains->impedance.damping_nm_s_per_rad - number(w[D_IMPEDANCE])) > 1.0e-6f) {
        return FOC_VIRTUAL_PACKAGE_RANGE;
    }
    gains->velocity = motion->velocity;
    gains->position = motion->position;
    for (uint32_t i = 0U; i < next.lut.length; ++i) {
        if (w[LUT_DATA + i] > UINT16_MAX) {
            return FOC_VIRTUAL_PACKAGE_RANGE;
        }
        const uint32_t word = w[LUT_DATA + i];
        next.offsets[i] = word > INT16_MAX ? (int16_t)((int32_t)word - 65536) : (int16_t)word;
    }
    motion->cogging = (struct foc_runtime_cogging_config){0};
    if (w[COGGING_ENABLED] != 0U) {
        motion->cogging = (struct foc_runtime_cogging_config){
            .enabled = true,
            .motor_id = w[MOTOR],
            .mechanical_frame_id = w[FRAME],
            .torque_constant_nm_per_a = gains->torque_constant_nm_per_a,
            .iq_limit_a = number(w[COGGING_LIMIT]),
            .lut = {
                .motor_id = w[MOTOR],
                .mechanical_frame_id = w[FRAME],
                .qualified = true,
                .training_capture_id = w[COGGING_TRAIN],
                .holdout_capture_id = w[COGGING_HOLDOUT]
            },
        };
        if (!bounded(motion->cogging.iq_limit_a, 0.001f, motion->iq_limit_a)) {
            return FOC_VIRTUAL_PACKAGE_RANGE;
        }
        for (size_t i = 0U; i < FOC_RUNTIME_COGGING_POINTS; ++i) {
            const float torque = number(w[COGGING_DATA + i]);
            if (fabsf(torque / gains->torque_constant_nm_per_a) > motion->cogging.iq_limit_a ||
                !bounded(
                    torque,
                    -motion->impedance.max_feedforward_nm,
                    motion->impedance.max_feedforward_nm
                )) {
                return FOC_VIRTUAL_PACKAGE_RANGE;
            }
            motion->cogging.lut.torque_nm[i] = torque;
        }
        gains->cogging = motion->cogging;
    }
    next.lut.offset_counts = next.offsets;
    if (foc_encoder_lut_validate(&next.lut, w[CPR], w[ENCODER]) != FOC_ENCODER_STATUS_OK) {
        return FOC_VIRTUAL_PACKAGE_RANGE;
    }
    *package = next;
    package->lut.offset_counts = package->offsets;
    return FOC_VIRTUAL_PACKAGE_OK;
}

static int encode(
    const struct foc_virtual_calibration *session,
    const struct foc_cal_application_candidate *candidate,
    uint8_t bytes[4U * WORDS]
)
{
    const struct foc_cal_workflow_bundle *bundle = &session->workflow.result;
    if (!bundle->complete || !bundle->applicable || !candidate->has_lut ||
        !candidate->gains.outer_tuned) {
        return FOC_VIRTUAL_PACKAGE_IDENTITY;
    }
    const struct foc_cal_workflow_identity *id = &bundle->request.identity;
    const struct foc_calibration_identity *acq = &id->acquisition;
    const struct foc_cal_application_gains *g = &candidate->gains;
    const struct foc_device_adc_config *adc = &candidate->pipeline.adc;
    const struct foc_runtime_motion_config *m = &candidate->runtime.config.motion;
    uint32_t words[WORDS] = {
        [MAGIC] = FCAL_MAGIC,
        [VERSION] = 1U,
        [MOTOR] = id->motor_setup_id,
        [ENCODER] = id->encoder_id,
        [FRAME] = id->mechanical_frame_id,
        [FULL_SCALE] = acq->adc_full_scale_counts,
        [CURRENT_SCALE] = bits(acq->current_a_per_count),
        [CPR] = acq->encoder_counts_per_revolution,
        [DIRECTION] = (uint32_t)acq->encoder_direction,
        [ORDER] = acq->phase_order,
        [POLES] = m->rotor.pole_pairs,
        [SESSION] = g->session,
        [D_CAPTURE] = g->d_capture,
        [Q_CAPTURE] = g->q_capture,
        [MECHANICAL_CAPTURE] = g->mechanical_capture,
        [KT_CAPTURE] = g->kt_capture,
        [ADC_A] = bits(adc->phase_a_offset_count),
        [ADC_B] = bits(adc->phase_b_offset_count),
        [ADC_C] = bits(adc->phase_c_offset_count),
        [OFFSET] = bits(m->rotor.electrical_offset_rad),
        [RS] = bits(g->model.rs_ohm),
        [LD] = bits(g->model.ld_h),
        [LQ] = bits(g->model.lq_h),
        [FLUX] = bits(session->flux.flux_wb),
        [KT] = bits(g->torque_constant_nm_per_a),
        [INERTIA] = bits(bundle->mechanical.inertia_kg_m2),
        [DAMPING] = bits(bundle->mechanical.damping_nm_s),
        [KP_D] = bits(g->current.kp_d_v_per_a),
        [KP_Q] = bits(g->current.kp_q_v_per_a),
        [KI] = bits(g->current.ki_v_per_a_s),
        [KP_VELOCITY] = bits(g->velocity.kp_a_per_rad_s),
        [KI_VELOCITY] = bits(g->velocity.ki_a_per_rad),
        [KP_POSITION] = bits(g->position.kp_per_s),
        [K_IMPEDANCE] = bits(g->impedance.stiffness_nm_per_rad),
        [D_IMPEDANCE] = bits(g->impedance.damping_nm_s_per_rad),
        [COGGING_ENABLED] = m->cogging.enabled,
        [COGGING_TRAIN] = m->cogging.lut.training_capture_id,
        [COGGING_HOLDOUT] = m->cogging.lut.holdout_capture_id,
        [COGGING_LIMIT] = bits(m->cogging.iq_limit_a),
        [LUT_LENGTH] = bundle->lut.nodes,
    };
    for (uint32_t i = 0U; i < bundle->lut.nodes; ++i) {
        words[LUT_DATA + i] = (uint16_t)bundle->lut.offsets[i];
    }
    for (size_t i = 0U; i < FOC_RUNTIME_COGGING_POINTS; ++i) {
        words[COGGING_DATA + i] = bits(m->cogging.lut.torque_nm[i]);
    }
    for (size_t i = 0U; i < CRC; ++i) {
        put_word(&bytes[4U * i], words[i]);
    }
    put_word(&bytes[4U * CRC], checksum(bytes, 4U * CRC));
    return FOC_VIRTUAL_PACKAGE_OK;
}

/* Public Functions ------------------------------------------------------------- */

int foc_virtual_calibration_package_id(
    const struct foc_virtual_calibration *session,
    const struct foc_cal_application_candidate *candidate,
    uint32_t *review_id
)
{
    uint8_t bytes[4U * WORDS];
    const int status = encode(session, candidate, bytes);
    if (status == FOC_VIRTUAL_PACKAGE_OK) {
        *review_id = get_word(&bytes[4U * CRC]);
    }
    return status;
}

int foc_virtual_calibration_package_save(
    const char *path,
    const struct foc_virtual_calibration *session,
    const struct foc_cal_application_candidate *candidate
)
{
    if (!session->workflow.result.reviewed) {
        return FOC_VIRTUAL_PACKAGE_IDENTITY;
    }
    uint8_t bytes[4U * WORDS];
    const int status = encode(session, candidate, bytes);
    if (status != FOC_VIRTUAL_PACKAGE_OK) {
        return status;
    }
    FILE *output = NULL;
#if defined(_MSC_VER)
    (void)fopen_s(&output, path, "wbx");
#else
    output = fopen(path, "wbx");
#endif
    if (output == NULL) {
        return FOC_VIRTUAL_PACKAGE_IO;
    }
    const bool written = fwrite(bytes, 1U, sizeof bytes, output) == sizeof bytes;
    const int closed = fclose(output);
    return written && closed == 0 ? FOC_VIRTUAL_PACKAGE_OK : FOC_VIRTUAL_PACKAGE_IO;
}

int foc_virtual_calibration_package_load(
    const char *path,
    const struct foc_virtual_current_profile *profile,
    const struct foc_cal_workflow_identity *identity,
    const struct foc_cal_application_tuning_policy *policy,
    const struct foc_cal_application_tuning_policy *legacy_policy,
    struct foc_virtual_calibration_package *package
)
{
    FILE *input = NULL;
#if defined(_MSC_VER)
    (void)fopen_s(&input, path, "rb");
#else
    input = fopen(path, "rb");
#endif
    if (input == NULL) {
        return FOC_VIRTUAL_PACKAGE_IO;
    }
    uint8_t bytes[4U * WORDS];
    const bool read = fread(bytes, 1U, sizeof bytes, input) == sizeof bytes &&
        fgetc(input) == EOF && !ferror(input);
    fclose(input);
    if (!read) {
        return FOC_VIRTUAL_PACKAGE_IO;
    }
    if (get_word(bytes) != FCAL_MAGIC || get_word(&bytes[4]) != 1U) {
        return FOC_VIRTUAL_PACKAGE_VERSION;
    }
    if (get_word(&bytes[4U * CRC]) != checksum(bytes, 4U * CRC)) {
        return FOC_VIRTUAL_PACKAGE_CHECKSUM;
    }
    uint32_t words[WORDS];
    for (size_t i = 0U; i < CRC; ++i) {
        words[i] = get_word(&bytes[4U * i]);
    }
    int status = decode(words, profile, identity, policy, package);
    if (status == FOC_VIRTUAL_PACKAGE_RANGE && legacy_policy != NULL) {
        status = decode(words, profile, identity, legacy_policy, package);
        if (status == FOC_VIRTUAL_PACKAGE_OK) {
            package->uses_legacy_policy = true;
        }
    }
    if (status == FOC_VIRTUAL_PACKAGE_OK) {
        package->review_id = get_word(&bytes[4U * CRC]);
    }
    return status;
}

const char *foc_virtual_calibration_package_reason(int status)
{
    static const char *const reasons[] = {
        "OK",
        "PACKAGE_IO_OR_LENGTH",
        "PACKAGE_VERSION",
        "PACKAGE_CHECKSUM",
        "PACKAGE_IDENTITY",
        "PACKAGE_PARAMETER_RANGE"
    };
    return (unsigned int)status < sizeof reasons / sizeof reasons[0] ? reasons[status]
                                                                     : "PACKAGE_UNKNOWN";
}

/* ---------------------------------------- end of file ----------------------------------------- */
