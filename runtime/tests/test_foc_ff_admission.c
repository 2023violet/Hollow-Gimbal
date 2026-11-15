/**
 * @file    test_foc_ff_admission.c
 * @brief   Host-only PMSM parameter and electrical-speed admission contract.
 * @version v0.1.0
 * @date    2026.11.14
 *
 * Design overview
 * ---------------
 * The current production Runtime validates numeric feedforward settings and
 * H1 freshness, but it does not carry parameter provenance or motor identity
 * metadata. This test keeps the missing admission policy explicit without
 * changing a production public API or enabling Board1 feedforward.
 *
 * Test role
 * ---------
 * Distinguish numeric validity, provenance, electrical-speed quality, and the
 * final Host eligibility decision. Negative cases are part of the contract.
 *
 * Implementation role
 * -------------------
 * Use the existing Safe modular time/sequence helpers and the production Core
 * feedforward equations only where a numeric result is under test.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_current.h"
#include "foc_safe.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Private Types ---------------------------------------------------------------- */

enum ff_source_kind {
    FF_SOURCE_NONE,
    FF_SOURCE_MODEL_ASSUMED,
    FF_SOURCE_IDENTIFIED,
};

enum ff_admission_status {
    FF_ADMISSION_OK,
    FF_ADMISSION_DISABLED,
    FF_ADMISSION_NUMERIC_INVALID,
    FF_ADMISSION_PROVENANCE_INVALID,
    FF_ADMISSION_SPEED_INVALID,
};

struct ff_parameter_record {
    struct foc_current_feedforward_model model;
    float rs_ohm;
    uint16_t pole_pairs;
    uint32_t motor_identity;
    uint32_t calibration_version;
    enum ff_source_kind source;
};

struct ff_speed_record {
    float omega_e_rad_s;
    uint32_t source_us;
    uint32_t source_seq;
    uint32_t session;
    uint32_t calibration_version;
    bool valid;
};

/* Private Helpers -------------------------------------------------------------- */

static bool ff_numeric_valid(const struct ff_parameter_record *parameters)
{
    return parameters != NULL && isfinite(parameters->rs_ohm) && parameters->rs_ohm > 0.0f &&
        isfinite(parameters->model.ld_h) && parameters->model.ld_h > 0.0f &&
        isfinite(parameters->model.lq_h) && parameters->model.lq_h > 0.0f &&
        isfinite(parameters->model.psi_f_wb) && parameters->model.psi_f_wb >= 0.0f &&
        parameters->pole_pairs > 0U;
}

static enum ff_admission_status ff_admit(
    const struct ff_parameter_record *parameters,
    const struct ff_speed_record *speed,
    uint32_t now_us,
    uint32_t expected_motor_identity,
    uint32_t expected_calibration_version,
    uint32_t expected_session,
    uint32_t previous_speed_seq,
    bool speed_seen,
    bool enabled,
    bool allow_model_assumed
)
{
    if (!enabled) {
        return FF_ADMISSION_DISABLED;
    }
    if (!ff_numeric_valid(parameters)) {
        return FF_ADMISSION_NUMERIC_INVALID;
    }
    if ((parameters->source != FF_SOURCE_IDENTIFIED &&
         !(allow_model_assumed && parameters->source == FF_SOURCE_MODEL_ASSUMED)) ||
        parameters->motor_identity != expected_motor_identity ||
        parameters->calibration_version != expected_calibration_version) {
        return FF_ADMISSION_PROVENANCE_INVALID;
    }
    if (speed == NULL || !speed->valid || !isfinite(speed->omega_e_rad_s) ||
        speed->session != expected_session ||
        speed->calibration_version != expected_calibration_version ||
        (speed_seen && !foc_safe_sequence_newer(speed->source_seq, previous_speed_seq, true)) ||
        !foc_safe_timestamp_fresh(now_us, speed->source_us, 700U)) {
        return FF_ADMISSION_SPEED_INVALID;
    }
    return FF_ADMISSION_OK;
}

static int check(bool condition, const char *name)
{
    if (!condition) {
        fprintf(stderr, "feedforward admission failure: %s\n", name);
        return 1;
    }
    return 0;
}

/* Public Functions ------------------------------------------------------------- */

int main(void)
{
    const struct ff_parameter_record identified = {
        .model = {.ld_h = 0.001f, .lq_h = 0.0015f, .psi_f_wb = 0.05f},
        .rs_ohm = 0.3f,
        .pole_pairs = 4U,
        .motor_identity = 0x4d4f5441U,
        .calibration_version = 7U,
        .source = FF_SOURCE_IDENTIFIED,
    };
    struct ff_parameter_record assumed = identified;
    struct ff_speed_record speed = {
        .omega_e_rad_s = 120.0f,
        .source_us = 1000U,
        .source_seq = 10U,
        .session = 3U,
        .calibration_version = 7U,
        .valid = true,
    };
    struct ff_parameter_record invalid = identified;
    unsigned int passed = 0U;

    if (check(
            ff_admit(&identified, &speed, 1200U, 0x4d4f5441U, 7U, 3U, 9U, true, true, false) ==
                FF_ADMISSION_OK,
            "identified fresh parameters and speed"
        ) != 0) {
        return 1;
    }
    ++passed;

    assumed.source = FF_SOURCE_MODEL_ASSUMED;
    if (check(
            ff_admit(&assumed, &speed, 1200U, 0x4d4f5441U, 7U, 3U, 9U, true, true, false) ==
                    FF_ADMISSION_PROVENANCE_INVALID &&
                ff_admit(&assumed, &speed, 1200U, 0x4d4f5441U, 7U, 3U, 9U, true, true, true) ==
                    FF_ADMISSION_OK,
            "model-assumed policy separation"
        ) != 0) {
        return 1;
    }
    ++passed;

    invalid.model.ld_h = 0.0f;
    if (check(
            ff_admit(&invalid, &speed, 1200U, 0x4d4f5441U, 7U, 3U, 9U, true, true, false) ==
                FF_ADMISSION_NUMERIC_INVALID,
            "zero inductance rejection"
        ) != 0) {
        return 1;
    }
    invalid = identified;
    invalid.model.psi_f_wb = NAN;
    if (check(
            ff_admit(&invalid, &speed, 1200U, 0x4d4f5441U, 7U, 3U, 9U, true, true, false) ==
                FF_ADMISSION_NUMERIC_INVALID,
            "nonfinite flux rejection"
        ) != 0) {
        return 1;
    }
    ++passed;

    invalid = identified;
    invalid.motor_identity ^= 1U;
    if (check(
            ff_admit(&invalid, &speed, 1200U, 0x4d4f5441U, 7U, 3U, 9U, true, true, false) ==
                FF_ADMISSION_PROVENANCE_INVALID,
            "motor identity mismatch rejection"
        ) != 0) {
        return 1;
    }
    invalid = identified;
    invalid.calibration_version++;
    if (check(
            ff_admit(&invalid, &speed, 1200U, 0x4d4f5441U, 7U, 3U, 9U, true, true, false) ==
                FF_ADMISSION_PROVENANCE_INVALID,
            "calibration version mismatch rejection"
        ) != 0) {
        return 1;
    }
    ++passed;

    {
        struct ff_speed_record first = speed;
        first.valid = false;
        if (check(
                ff_admit(&identified, &first, 1200U, 0x4d4f5441U, 7U, 3U, 9U, true, true, false) ==
                    FF_ADMISSION_SPEED_INVALID,
                "first speed quality rejection"
            ) != 0) {
            return 1;
        }
        first = speed;
        first.source_us = 0U;
        if (check(
                ff_admit(&identified, &first, 1200U, 0x4d4f5441U, 7U, 3U, 9U, true, true, false) ==
                    FF_ADMISSION_SPEED_INVALID,
                "stale speed rejection"
            ) != 0) {
            return 1;
        }
        first = speed;
        first.session = 2U;
        if (check(
                ff_admit(&identified, &first, 1200U, 0x4d4f5441U, 7U, 3U, 9U, true, true, false) ==
                    FF_ADMISSION_SPEED_INVALID,
                "old session speed rejection"
            ) != 0) {
            return 1;
        }
        first = speed;
        first.source_seq = 9U;
        if (check(
                ff_admit(&identified, &first, 1200U, 0x4d4f5441U, 7U, 3U, 9U, true, true, false) ==
                    FF_ADMISSION_SPEED_INVALID,
                "old sequence speed rejection"
            ) != 0) {
            return 1;
        }
        first = speed;
        first.source_us = UINT32_MAX - 200U;
        if (check(
                ff_admit(&identified, &first, 100U, 0x4d4f5441U, 7U, 3U, 9U, true, true, false) ==
                    FF_ADMISSION_OK,
                "timestamp wrap acceptance"
            ) != 0) {
            return 1;
        }
    }
    ++passed;

    {
        struct foc_current_feedforward_context context = {0};
        struct foc_current_feedforward_output output;
        if (check(
                foc_current_feedforward_init(&context, &identified.model, true) == FOC_STATUS_OK &&
                    foc_current_feedforward_compute(
                        &context,
                        120.0f,
                        (struct foc_vector_dq){1.0f, 2.0f},
                        &output
                    ) == FOC_STATUS_OK &&
                    fabsf(output.voltage_d_v + 0.36f) < 1.0e-6f &&
                    fabsf(output.voltage_q_v - 6.12f) < 1.0e-5f,
                "production feedforward equation"
            ) != 0) {
            return 1;
        }
    }
    ++passed;

    if (check(
            ff_admit(&identified, &speed, 1200U, 0x4d4f5441U, 7U, 3U, 9U, true, true, false) ==
                    FF_ADMISSION_OK &&
                ff_admit(&identified, &speed, 1200U, 0x4d4f5441U, 7U, 3U, 9U, true, false, false) ==
                    FF_ADMISSION_DISABLED,
            "explicit enable boundary"
        ) != 0) {
        return 1;
    }
    ++passed;

    printf(
        "test-foc-ff-admission passed matrix=%u numeric=1 provenance=1 speed=1 wrap=1 "
        "feedforward_equation=1 production_wiring=host_only\n",
        passed
    );
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
