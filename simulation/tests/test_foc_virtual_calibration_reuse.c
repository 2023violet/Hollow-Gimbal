/**
 * @file    test_foc_virtual_calibration_reuse.c
 * @brief   Selective calibration reuse and rejected provenance tests.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Test role
 * ---------
 * Exercise retained and invalidated records under changed sources, device
 * identities and operating conditions.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_calibration_reuse.h"

#include <math.h>
#include <stdio.h>

/* Private Helpers -------------------------------------------------------------- */

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

#define BIT(parameter) FOC_CAL_REUSE_BIT(FOC_CAL_REUSE_##parameter)

static struct foc_cal_reuse_setup setup(void)
{
    struct foc_cal_reuse_setup value = {
        .temperature_c = 25.0f,
        .speed_rad_s = 10.0f,
        .current_a = 1.0f,
        .frequency_hz = 20.0f,
        .load_nm = 0.1f,
    };
    for (unsigned int id = 0U; id < FOC_CAL_REUSE_ID_COUNT; ++id) {
        value.id[id] = 100U + id;
    }
    return value;
}

static struct foc_cal_reuse_record record(enum foc_cal_reuse_parameter parameter)
{
    struct foc_cal_reuse_record value = {
        .session = 20U,
        .capture = 100U + (uint32_t)parameter,
        .revision = 1U,
        .source = FOC_CAL_REUSE_SOURCE_HOST_VIRTUAL,
        .reviewed = true,
        .uses_adc = parameter == FOC_CAL_REUSE_RS || parameter == FOC_CAL_REUSE_RL ||
            parameter == FOC_CAL_REUSE_FLUX,
        .uses_mapped_angle = parameter == FOC_CAL_REUSE_POLES || parameter == FOC_CAL_REUSE_OFFSET,
        .uses_rl = parameter == FOC_CAL_REUSE_FLUX,
    };
    if (parameter == FOC_CAL_REUSE_RS) {
        value.range_mask = FOC_CAL_REUSE_BIT(FOC_CAL_REUSE_RANGE_TEMP);
        value.range[FOC_CAL_REUSE_RANGE_TEMP] = (struct foc_cal_reuse_range){25.0f, 25.0f};
    }
    return value;
}

static int complete_history(struct foc_cal_reuse_history *history)
{
    *history = (struct foc_cal_reuse_history){.setup = setup()};
    const enum foc_cal_reuse_parameter acquisition_order[] = {
        FOC_CAL_REUSE_ADC,
        FOC_CAL_REUSE_RS,
        FOC_CAL_REUSE_RL,
        FOC_CAL_REUSE_LUT,
        FOC_CAL_REUSE_POLES,
        FOC_CAL_REUSE_OFFSET,
        FOC_CAL_REUSE_HOME,
        FOC_CAL_REUSE_FLUX,
        FOC_CAL_REUSE_JB,
        FOC_CAL_REUSE_LIMITS,
        FOC_CAL_REUSE_CURRENT_PI,
        FOC_CAL_REUSE_SERVO,
        FOC_CAL_REUSE_COGGING,
    };
    for (unsigned int i = 0U; i < FOC_CAL_REUSE_COUNT; ++i) {
        const enum foc_cal_reuse_parameter parameter = acquisition_order[i];
        const struct foc_cal_reuse_record item = record(parameter);
        CHECK(foc_cal_reuse_bind_record(history, parameter, &item));
    }
    return 0;
}

static int equipment_changes_propagate_selectively(void)
{
    struct foc_cal_reuse_history history;
    CHECK(complete_history(&history) == 0);
    CHECK(foc_cal_reuse_evaluate(&history, &history.setup, NULL) == BIT(COUNT) - 1U);
    static const struct {
        enum foc_cal_reuse_identity change;
        uint32_t retained;
    } cases[] = {
        {FOC_CAL_REUSE_ID_ENCODER,
         BIT(ADC) | BIT(RS) | BIT(RL) | BIT(HOME) | BIT(JB) | BIT(CURRENT_PI) | BIT(LIMITS)},
        {FOC_CAL_REUSE_ID_MOTOR, BIT(ADC) | BIT(LUT) | BIT(HOME) | BIT(LIMITS)},
        {FOC_CAL_REUSE_ID_AXIS_LOAD,
         BIT(ADC) | BIT(RS) | BIT(RL) | BIT(LUT) | BIT(POLES) | BIT(OFFSET) | BIT(HOME) |
             BIT(FLUX) | BIT(CURRENT_PI)},
        {FOC_CAL_REUSE_ID_ADC_CHAIN,
         BIT(LUT) | BIT(POLES) | BIT(OFFSET) | BIT(HOME) | BIT(JB) | BIT(LIMITS)},
        {FOC_CAL_REUSE_ID_PHASE, BIT(LUT) | BIT(POLES) | BIT(HOME) | BIT(JB) | BIT(LIMITS)},
        {FOC_CAL_REUSE_ID_OFFSET_REFERENCE, (BIT(COUNT) - 1U) & ~BIT(OFFSET)},
        {FOC_CAL_REUSE_ID_MECH_FRAME,
         BIT(ADC) | BIT(RS) | BIT(RL) | BIT(LUT) | BIT(POLES) | BIT(OFFSET) | BIT(FLUX) | BIT(JB) |
             BIT(CURRENT_PI)},
        {FOC_CAL_REUSE_ID_KT_REFERENCE,
         BIT(ADC) | BIT(RS) | BIT(RL) | BIT(LUT) | BIT(POLES) | BIT(OFFSET) | BIT(HOME) | BIT(JB) |
             BIT(CURRENT_PI) | BIT(LIMITS)},
    };
    for (unsigned int i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        struct foc_cal_reuse_setup next = history.setup;
        ++next.id[cases[i].change];
        CHECK(foc_cal_reuse_evaluate(&history, &next, NULL) == cases[i].retained);
    }
    return 0;
}

static int revised_mapping_and_torque_do_not_relabel_old_fits(void)
{
    struct foc_cal_reuse_history history;
    CHECK(complete_history(&history) == 0);
    struct foc_cal_reuse_record item = record(FOC_CAL_REUSE_JB);
    item.revision = 2U;
    item.uses_kt = true;
    item.uses_adc = true;
    item.uses_mapped_angle = true;
    CHECK(foc_cal_reuse_bind_record(&history, FOC_CAL_REUSE_JB, &item));
    struct foc_cal_reuse_setup next = history.setup;
    ++next.id[FOC_CAL_REUSE_ID_KT_REFERENCE];
    enum foc_cal_reuse_reason reasons[FOC_CAL_REUSE_COUNT];
    uint32_t retained = foc_cal_reuse_evaluate(&history, &next, reasons);
    CHECK((retained & (BIT(JB) | BIT(FLUX))) == 0U);
    CHECK(reasons[FOC_CAL_REUSE_JB] == FOC_CAL_REUSE_REASON_DEPENDENCY_INVALID);

    item = record(FOC_CAL_REUSE_OFFSET);
    item.revision = 2U;
    CHECK(foc_cal_reuse_bind_record(&history, FOC_CAL_REUSE_OFFSET, &item));
    retained = foc_cal_reuse_evaluate(&history, &history.setup, reasons);
    CHECK(
        (retained & (BIT(JB) | BIT(OFFSET) | BIT(RL) | BIT(FLUX))) ==
        (BIT(OFFSET) | BIT(RL) | BIT(FLUX))
    );
    CHECK(reasons[FOC_CAL_REUSE_JB] == FOC_CAL_REUSE_REASON_DEPENDENCY_CHANGED);

    item = record(FOC_CAL_REUSE_LUT);
    CHECK(!foc_cal_reuse_bind_record(&history, FOC_CAL_REUSE_LUT, &item));
    item.revision = 2U;
    CHECK(foc_cal_reuse_bind_record(&history, FOC_CAL_REUSE_LUT, &item));
    retained = foc_cal_reuse_evaluate(&history, &history.setup, reasons);
    CHECK((retained & (BIT(OFFSET) | BIT(POLES) | BIT(JB))) == 0U);
    CHECK((retained & (BIT(RS) | BIT(RL))) == (BIT(RS) | BIT(RL)));
    CHECK(reasons[FOC_CAL_REUSE_POLES] == FOC_CAL_REUSE_REASON_DEPENDENCY_CHANGED);
    return 0;
}

static int qualified_point_range_is_not_unbounded(void)
{
    struct foc_cal_reuse_history history;
    CHECK(complete_history(&history) == 0);
    struct foc_cal_reuse_setup next = history.setup;
    enum foc_cal_reuse_reason reasons[FOC_CAL_REUSE_COUNT];
    CHECK((foc_cal_reuse_evaluate(&history, &next, reasons) & BIT(RS)) != 0U);
    next.temperature_c = nextafterf(25.0f, INFINITY);
    CHECK((foc_cal_reuse_evaluate(&history, &next, reasons) & (BIT(RS) | BIT(CURRENT_PI))) == 0U);
    CHECK(reasons[FOC_CAL_REUSE_RS] == FOC_CAL_REUSE_REASON_OUTSIDE_RANGE);
    next.temperature_c = NAN;
    CHECK((foc_cal_reuse_evaluate(&history, &next, reasons) & BIT(RS)) == 0U);
    CHECK(reasons[FOC_CAL_REUSE_RS] == FOC_CAL_REUSE_REASON_OUTSIDE_RANGE);

    struct foc_cal_reuse_record item = record(FOC_CAL_REUSE_RS);
    item.revision = 2U;
    item.range[FOC_CAL_REUSE_RANGE_TEMP] = (struct foc_cal_reuse_range){26.0f, 24.0f};
    CHECK(!foc_cal_reuse_bind_record(&history, FOC_CAL_REUSE_RS, &item));
    CHECK(history.records[FOC_CAL_REUSE_RS].revision == 1U);
    return 0;
}

static int rejects_incomplete_and_unreviewed_evidence(void)
{
    struct foc_cal_reuse_history history = {.setup = setup()};
    struct foc_cal_reuse_record item = record(FOC_CAL_REUSE_RS);
    CHECK(!foc_cal_reuse_bind_record(&history, FOC_CAL_REUSE_RS, &item));
    item = record(FOC_CAL_REUSE_ADC);
    item.reviewed = false;
    CHECK(!foc_cal_reuse_bind_record(&history, FOC_CAL_REUSE_ADC, &item));
    item.reviewed = true;
    CHECK(foc_cal_reuse_bind_record(&history, FOC_CAL_REUSE_ADC, &item));
    item = record(FOC_CAL_REUSE_RS);
    item.uses_adc = false;
    CHECK(!foc_cal_reuse_bind_record(&history, FOC_CAL_REUSE_RS, &item));
    item.source = FOC_CAL_REUSE_SOURCE_INSTRUMENT;
    CHECK(foc_cal_reuse_bind_record(&history, FOC_CAL_REUSE_RS, &item));
    struct foc_cal_reuse_setup next = history.setup;
    ++next.id[FOC_CAL_REUSE_ID_ADC_CHAIN];
    CHECK(foc_cal_reuse_evaluate(&history, &next, NULL) == BIT(RS));
    ++next.id[FOC_CAL_REUSE_ID_VOLTAGE_SOURCE];
    CHECK(foc_cal_reuse_evaluate(&history, &next, NULL) == 0U);

    item = record(FOC_CAL_REUSE_RL);
    CHECK(foc_cal_reuse_bind_record(&history, FOC_CAL_REUSE_RL, &item));
    history.records[FOC_CAL_REUSE_RL].dependency[FOC_CAL_REUSE_ADC] = 0U;
    enum foc_cal_reuse_reason reasons[FOC_CAL_REUSE_COUNT];
    CHECK((foc_cal_reuse_evaluate(&history, &history.setup, reasons) & BIT(RL)) == 0U);
    CHECK(reasons[FOC_CAL_REUSE_RL] == FOC_CAL_REUSE_REASON_DEPENDENCY_MISSING);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    return equipment_changes_propagate_selectively() ||
        revised_mapping_and_torque_do_not_relabel_old_fits() ||
        qualified_point_range_is_not_unbounded() || rejects_incomplete_and_unreviewed_evidence();
}

/* ---------------------------------------- end of file ----------------------------------------- */
