/**
 * @file    test_foc_board_boot_default.c
 * @brief   Formal Board1 startup selection with the default (absent) supply.
 * @version v0.1.0
 * @date    2026.11.21
 *
 * Test role
 * ---------
 * Link the same supply and expectation translation units as the default ARM
 * image and prove NOMINAL_CONFIGURATION + CALIBRATION_NOT_AVAILABLE, the UART
 * readback of that state, and that the default expectation accepts no supply.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board_boot_test.h"

#include <stdio.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

/* Private Variables ------------------------------------------------------------ */

static struct foc_boot_test fixture;

/* Private Helpers -------------------------------------------------------------- */

static int test_default_boot(void)
{
    const struct foc_board_calibration_expectation expected = foc_board_calibration_expectation();
    CHECK(foc_board_calibration_supply() == NULL);
    CHECK(expected.supply_id == 0U && expected.pole_pairs == 7U);
    CHECK(
        foc_boot_test_select(&fixture, foc_board_calibration_supply(), &expected) == FOC_RUNTIME_OK
    );
    CHECK(fixture.boot.configuration == FOC_BOARD_NOMINAL_CONFIGURATION);
    CHECK(fixture.boot.calibration == FOC_BOARD_CALIBRATION_NOT_AVAILABLE);
    CHECK(fixture.boot.rejection == FOC_BOARD_REJECTION_NONE && fixture.boot.supply_id == 0U);

    /* Selection must be identical to the previous default foc_board_init(). */
    static struct foc_runtime legacy;
    struct foc_board_output legacy_output;
    CHECK(foc_board_init(&legacy, &legacy_output, 0U) == FOC_RUNTIME_OK);
    CHECK(
        memcmp(
            &legacy.pipeline.adc_config,
            &fixture.runtime.pipeline.adc_config,
            sizeof legacy.pipeline.adc_config
        ) == 0
    );
    CHECK(legacy.config.motion.rotor.pole_pairs == fixture.runtime.config.motion.rotor.pole_pairs);
    CHECK(
        legacy.config.motion.rotor.electrical_offset_rad ==
        fixture.runtime.config.motion.rotor.electrical_offset_rad
    );
    CHECK(!fixture.encoder.config_valid);

    struct foc_boot_test_parameter parameter;
    struct foc_protocol_frame response;
    CHECK(foc_boot_test_hello(&fixture) == 0);
    CHECK(foc_boot_test_read(&fixture, 32U, &parameter) == FOC_PROTO_OK);
    CHECK(parameter.value == FOC_BOARD_NOMINAL_CONFIGURATION);
    CHECK(foc_boot_test_read(&fixture, 33U, &parameter) == FOC_PROTO_OK);
    CHECK(parameter.value == FOC_BOARD_CALIBRATION_NOT_AVAILABLE);
    CHECK(foc_boot_test_read(&fixture, 34U, &parameter) == FOC_PROTO_OK && parameter.value == 0U);
    CHECK(foc_boot_test_read(&fixture, 38U, &parameter) == FOC_PROTO_OK);
    CHECK(foc_boot_test_float(parameter.value) == 2048.0f);
    CHECK(foc_boot_test_read(&fixture, 42U, &parameter) == FOC_PROTO_OK && parameter.value == 2U);
    CHECK(foc_boot_test_read(&fixture, 44U, &parameter) == FOC_PROTO_OK && parameter.value == 0U);
    CHECK(foc_boot_test_read(&fixture, 45U, &parameter) == FOC_PROTO_OK && parameter.value == 0U);
    CHECK(foc_boot_test_read(&fixture, 16U, &parameter) == FOC_PROTO_OK && parameter.value == 7U);
    CHECK(
        foc_boot_test_call(&fixture, FOC_MSG_ARM, NULL, 0U, &response) == FOC_PROTO_OUTPUT_LOCKED
    );
    CHECK(
        foc_boot_test_call(&fixture, FOC_MSG_START, NULL, 0U, &response) == FOC_PROTO_OUTPUT_LOCKED
    );
    CHECK(
        foc_boot_test_call(&fixture, FOC_MSG_PARAM_SAVE, NULL, 0U, &response) ==
        FOC_PROTO_NOT_SUPPORTED
    );
    CHECK(fixture.output.declined_updates == 0U);
    printf("default boot: NOMINAL_CONFIGURATION + CALIBRATION_NOT_AVAILABLE\n");
    return 0;
}

static int test_default_expectation_accepts_no_supply(void)
{
    const struct foc_board_calibration_expectation expected = foc_board_calibration_expectation();
    const struct foc_board_calibration_supply candidate = {
        .format = FOC_BOARD_CALIBRATION_SUPPLY_FORMAT,
        .supply_id = 0x56464331U,
        .coordinate = FOC_BOARD_COORDINATE_RAW_NO_LUT,
    };
    CHECK(
        foc_board_boot_qualify(&candidate, &expected, NULL, NULL) ==
        FOC_BOARD_REJECTION_SUPPLY_IDENTITY
    );
    const struct foc_board_calibration_supply unnamed = {
        .format = FOC_BOARD_CALIBRATION_SUPPLY_FORMAT,
        .coordinate = FOC_BOARD_COORDINATE_RAW_NO_LUT,
    };
    CHECK(
        foc_board_boot_qualify(&unnamed, &expected, NULL, NULL) ==
        FOC_BOARD_REJECTION_SUPPLY_IDENTITY
    );
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    return test_default_boot() || test_default_expectation_accepts_no_supply();
}

/* ---------------------------------------- end of file ----------------------------------------- */
