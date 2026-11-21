/**
 * @file    foc_virtual_calibration_reuse.c
 * @brief   Check reviewed Host calibration against its actual measurement dependencies.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Implementation role
 * -------------------
 * Evaluate a fixed set of physical prerequisites in dependency order and
 * retain the reason for each unavailable parameter.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_calibration_reuse.h"

#include <math.h>
#include <stddef.h>

/* Private Constants ------------------------------------------------------------ */

#define PARAM(value) FOC_CAL_REUSE_BIT(FOC_CAL_REUSE_##value)
#define ID(value)    FOC_CAL_REUSE_BIT(FOC_CAL_REUSE_ID_##value)
#define RANGE(value) FOC_CAL_REUSE_BIT(FOC_CAL_REUSE_RANGE_##value)

struct reuse_rule {
    uint32_t identities;
    uint32_t dependencies;
    uint32_t point_dimensions;
};

static const struct reuse_rule rules[FOC_CAL_REUSE_COUNT] = {
    [FOC_CAL_REUSE_ADC] = {ID(ADC_CHAIN) | ID(PHASE), 0U, RANGE(TEMP)},
    [FOC_CAL_REUSE_RS] = {ID(MOTOR), 0U, RANGE(TEMP) | RANGE(CURRENT)},
    [FOC_CAL_REUSE_RL] =
        {ID(MOTOR) | ID(PHASE), 0U, RANGE(TEMP) | RANGE(CURRENT) | RANGE(FREQUENCY)},
    [FOC_CAL_REUSE_LUT] = {ID(ENCODER) | ID(ASSEMBLY) | ID(LUT_REFERENCE), 0U, 0U},
    [FOC_CAL_REUSE_POLES] = {ID(MOTOR), 0U, 0U},
    [FOC_CAL_REUSE_OFFSET] =
        {ID(MOTOR) | ID(ENCODER) | ID(PHASE) | ID(ASSEMBLY) | ID(OFFSET_REFERENCE),
         PARAM(POLES),
         0U},
    [FOC_CAL_REUSE_HOME] = {ID(MECH_FRAME), 0U, 0U},
    [FOC_CAL_REUSE_FLUX] =
        {ID(MOTOR) | ID(KT_REFERENCE), PARAM(POLES), RANGE(TEMP) | RANGE(SPEED) | RANGE(CURRENT)},
    [FOC_CAL_REUSE_JB] =
        {ID(MOTOR) | ID(AXIS_LOAD) | ID(TORQUE_SOURCE), 0U, RANGE(SPEED) | RANGE(LOAD)},
    [FOC_CAL_REUSE_CURRENT_PI] =
        {ID(TIMING) | ID(CONTROL_LIMITS), PARAM(RS) | PARAM(RL), RANGE(CURRENT) | RANGE(FREQUENCY)},
    [FOC_CAL_REUSE_SERVO] =
        {ID(MECH_FRAME) | ID(TIMING) | ID(CONTROL_LIMITS),
         PARAM(FLUX) | PARAM(JB) | PARAM(CURRENT_PI) | PARAM(LIMITS),
         RANGE(SPEED) | RANGE(CURRENT) | RANGE(LOAD)},
    [FOC_CAL_REUSE_COGGING] =
        {ID(MOTOR) | ID(AXIS_LOAD) | ID(MECH_FRAME) | ID(TORQUE_SOURCE) | ID(CONTROL_LIMITS),
         PARAM(FLUX),
         RANGE(SPEED) | RANGE(LOAD)},
    [FOC_CAL_REUSE_LIMITS] =
        {ID(AXIS_LOAD) | ID(MECH_FRAME) | ID(CONTROL_LIMITS), 0U, RANGE(SPEED) | RANGE(LOAD)},
};

/* Every parameter appears once, after the measurements it can reference. */
static const enum foc_cal_reuse_parameter evaluation_order[FOC_CAL_REUSE_COUNT] = {
    FOC_CAL_REUSE_ADC,
    FOC_CAL_REUSE_RS,
    FOC_CAL_REUSE_LUT,
    FOC_CAL_REUSE_POLES,
    FOC_CAL_REUSE_OFFSET,
    FOC_CAL_REUSE_RL,
    FOC_CAL_REUSE_HOME,
    FOC_CAL_REUSE_FLUX,
    FOC_CAL_REUSE_JB,
    FOC_CAL_REUSE_LIMITS,
    FOC_CAL_REUSE_CURRENT_PI,
    FOC_CAL_REUSE_SERVO,
    FOC_CAL_REUSE_COGGING,
};

/* Private Helpers -------------------------------------------------------------- */

static bool
provenance_valid(enum foc_cal_reuse_parameter parameter, const struct foc_cal_reuse_record *record)
{
    const uint32_t bit = FOC_CAL_REUSE_BIT(parameter);
    const uint32_t electrical = PARAM(RS) | PARAM(RL);
    const uint32_t adc = electrical | PARAM(FLUX) | PARAM(JB);
    const uint32_t mapped = PARAM(RL) | PARAM(POLES) | PARAM(OFFSET) | PARAM(HOME) | PARAM(FLUX) |
        PARAM(JB) | PARAM(COGGING) | PARAM(LIMITS);
    if (record->source < FOC_CAL_REUSE_SOURCE_HOST_VIRTUAL ||
        record->source > FOC_CAL_REUSE_SOURCE_DERIVED || (record->uses_adc && (bit & adc) == 0U) ||
        (record->uses_mapped_angle && (bit & mapped) == 0U) ||
        (record->uses_kt && parameter != FOC_CAL_REUSE_JB) ||
        (record->uses_rl && parameter != FOC_CAL_REUSE_FLUX) ||
        (parameter == FOC_CAL_REUSE_JB && record->uses_adc && !record->uses_kt)) {
        return false;
    }
    if (record->source == FOC_CAL_REUSE_SOURCE_HOST_VIRTUAL) {
        if (((bit & electrical) != 0U || parameter == FOC_CAL_REUSE_FLUX) && !record->uses_adc) {
            return false;
        }
        if (parameter == FOC_CAL_REUSE_FLUX && !record->uses_rl) {
            return false;
        }
    }
    return true;
}

static struct reuse_rule
rule_for(enum foc_cal_reuse_parameter parameter, const struct foc_cal_reuse_record *record)
{
    struct reuse_rule rule = rules[parameter];
    if (record->uses_adc) {
        rule.dependencies |= PARAM(ADC);
    }
    if (record->uses_mapped_angle) {
        rule.identities |= ID(ENCODER) | ID(ASSEMBLY);
        rule.dependencies |= PARAM(LUT);
        if (parameter == FOC_CAL_REUSE_RL || parameter == FOC_CAL_REUSE_FLUX || record->uses_kt) {
            rule.dependencies |= PARAM(OFFSET);
        }
        if (parameter == FOC_CAL_REUSE_RL || parameter == FOC_CAL_REUSE_FLUX) {
            rule.identities |= ID(ANGLE_REFERENCE);
        }
        if (parameter == FOC_CAL_REUSE_JB) {
            rule.identities |= ID(MECH_FRAME);
        }
        if (parameter == FOC_CAL_REUSE_LIMITS) {
            rule.dependencies |= PARAM(HOME);
        }
    } else if (
        parameter == FOC_CAL_REUSE_RL || record->uses_rl || record->uses_kt ||
        (parameter == FOC_CAL_REUSE_POLES && record->source != FOC_CAL_REUSE_SOURCE_MANUFACTURER)
    ) {
        /* The constrained Host bench uses an independent plant magnetic witness. */
        rule.identities |= ID(ANGLE_REFERENCE);
    }
    if ((parameter == FOC_CAL_REUSE_RS || parameter == FOC_CAL_REUSE_RL) &&
        record->source != FOC_CAL_REUSE_SOURCE_MANUFACTURER) {
        rule.identities |= ID(VOLTAGE_SOURCE);
    }
    if (record->uses_rl) {
        rule.identities |= ID(PHASE) | ID(VOLTAGE_SOURCE);
        rule.dependencies |= PARAM(RS) | PARAM(RL);
    }
    if (record->uses_kt) {
        rule.dependencies |= PARAM(FLUX);
        rule.point_dimensions |= RANGE(CURRENT);
    }
    return rule;
}

static enum foc_cal_reuse_reason range_reason(
    const struct foc_cal_reuse_setup *previous,
    const struct foc_cal_reuse_setup *next,
    const struct foc_cal_reuse_record *record,
    uint32_t point_dimensions
)
{
    if ((record->range_mask & ~(RANGE(COUNT) - 1U)) != 0U) {
        return FOC_CAL_REUSE_REASON_RANGE_INVALID;
    }
    const float measured[FOC_CAL_REUSE_RANGE_COUNT] = {
        previous->temperature_c,
        previous->speed_rad_s,
        previous->current_a,
        previous->frequency_hz,
        previous->load_nm,
    };
    const float requested[FOC_CAL_REUSE_RANGE_COUNT] = {
        next->temperature_c,
        next->speed_rad_s,
        next->current_a,
        next->frequency_hz,
        next->load_nm,
    };
    for (unsigned int i = 0U; i < FOC_CAL_REUSE_RANGE_COUNT; ++i) {
        const uint32_t bit = FOC_CAL_REUSE_BIT(i);
        if (((record->range_mask | point_dimensions) & bit) == 0U) {
            continue;
        }
        const struct foc_cal_reuse_range range = (record->range_mask & bit) != 0U
            ? record->range[i]
            : (struct foc_cal_reuse_range){measured[i], measured[i]};
        if (!isfinite(range.min) || !isfinite(range.max) || range.min > range.max ||
            !isfinite(measured[i]) || measured[i] < range.min || measured[i] > range.max) {
            return FOC_CAL_REUSE_REASON_RANGE_INVALID;
        }
        if (!isfinite(requested[i]) || requested[i] < range.min || requested[i] > range.max) {
            return FOC_CAL_REUSE_REASON_OUTSIDE_RANGE;
        }
    }
    return FOC_CAL_REUSE_REASON_VALID;
}

static enum foc_cal_reuse_reason reason_for(
    const struct foc_cal_reuse_history *history,
    const struct foc_cal_reuse_setup *next,
    enum foc_cal_reuse_parameter parameter,
    const struct foc_cal_reuse_record *record,
    uint32_t reusable
)
{
    if (record->revision == 0U || record->session == 0U || record->capture == 0U) {
        return FOC_CAL_REUSE_REASON_MISSING;
    }
    if (!record->reviewed) {
        return FOC_CAL_REUSE_REASON_UNREVIEWED;
    }
    if (!provenance_valid(parameter, record)) {
        return FOC_CAL_REUSE_REASON_PROVENANCE;
    }
    const struct reuse_rule rule = rule_for(parameter, record);
    for (unsigned int i = 0U; i < FOC_CAL_REUSE_ID_COUNT; ++i) {
        if ((rule.identities & FOC_CAL_REUSE_BIT(i)) != 0U &&
            (history->setup.id[i] == 0U || next->id[i] != history->setup.id[i])) {
            return (enum foc_cal_reuse_reason)(FOC_CAL_REUSE_REASON_IDENTITY_FIRST + i);
        }
    }
    const enum foc_cal_reuse_reason applicability =
        range_reason(&history->setup, next, record, rule.point_dimensions);
    if (applicability != FOC_CAL_REUSE_REASON_VALID) {
        return applicability;
    }
    for (unsigned int i = 0U; i < FOC_CAL_REUSE_COUNT; ++i) {
        const uint32_t bit = FOC_CAL_REUSE_BIT(i);
        if ((rule.dependencies & bit) == 0U) {
            if (record->dependency[i] != 0U) {
                return FOC_CAL_REUSE_REASON_PROVENANCE;
            }
            continue;
        }
        if (record->dependency[i] == 0U || history->records[i].revision == 0U) {
            return FOC_CAL_REUSE_REASON_DEPENDENCY_MISSING;
        }
        if (record->dependency[i] != history->records[i].revision) {
            return FOC_CAL_REUSE_REASON_DEPENDENCY_CHANGED;
        }
        if ((reusable & bit) == 0U) {
            return FOC_CAL_REUSE_REASON_DEPENDENCY_INVALID;
        }
    }
    return FOC_CAL_REUSE_REASON_VALID;
}

/* Public Functions ------------------------------------------------------------- */

bool foc_cal_reuse_bind_record(
    struct foc_cal_reuse_history *history,
    enum foc_cal_reuse_parameter parameter,
    const struct foc_cal_reuse_record *record
)
{
    if (history == NULL || record == NULL || (unsigned int)parameter >= FOC_CAL_REUSE_COUNT ||
        record->revision <= history->records[parameter].revision) {
        return false;
    }
    struct foc_cal_reuse_record trial = *record;
    const uint32_t dependencies = rule_for(parameter, record).dependencies;
    for (unsigned int i = 0U; i < FOC_CAL_REUSE_COUNT; ++i) {
        if (trial.dependency[i] != 0U) {
            return false;
        }
        if ((dependencies & FOC_CAL_REUSE_BIT(i)) != 0U) {
            trial.dependency[i] = history->records[i].revision;
        }
    }
    const uint32_t reusable = foc_cal_reuse_evaluate(history, &history->setup, NULL);
    if (reason_for(history, &history->setup, parameter, &trial, reusable) !=
        FOC_CAL_REUSE_REASON_VALID) {
        return false;
    }
    history->records[parameter] = trial;
    return true;
}

uint32_t foc_cal_reuse_evaluate(
    const struct foc_cal_reuse_history *history,
    const struct foc_cal_reuse_setup *next,
    enum foc_cal_reuse_reason reasons[FOC_CAL_REUSE_COUNT]
)
{
    uint32_t reusable = 0U;
    for (unsigned int i = 0U; i < FOC_CAL_REUSE_COUNT; ++i) {
        const enum foc_cal_reuse_parameter parameter = evaluation_order[i];
        const enum foc_cal_reuse_reason reason = history == NULL || next == NULL
            ? FOC_CAL_REUSE_REASON_MISSING
            : reason_for(history, next, parameter, &history->records[parameter], reusable);
        if (reasons != NULL) {
            reasons[parameter] = reason;
        }
        if (reason == FOC_CAL_REUSE_REASON_VALID) {
            reusable |= FOC_CAL_REUSE_BIT(parameter);
        }
    }
    return reusable;
}

const char *foc_cal_reuse_parameter_name(enum foc_cal_reuse_parameter parameter)
{
    static const char *const names[FOC_CAL_REUSE_COUNT] = {
        "adc_offset",
        "rs",
        "ld_lq",
        "encoder_lut",
        "pole_pairs",
        "rotor_offset",
        "mechanical_home",
        "flux_kt",
        "jb",
        "current_pi",
        "servo_gains",
        "cogging_lut",
        "mechanical_limits",
    };
    return (unsigned int)parameter < FOC_CAL_REUSE_COUNT ? names[parameter] : "unknown_parameter";
}

const char *foc_cal_reuse_reason_name(enum foc_cal_reuse_reason reason)
{
    static const char *const causes[] = {
        "valid",
        "missing_record",
        "unreviewed_record",
        "invalid_provenance",
        "invalid_range",
        "outside_range",
        "missing_dependency",
        "changed_dependency_revision",
        "invalid_dependency",
    };
    static const char *const identities[FOC_CAL_REUSE_ID_COUNT] = {
        "motor_identity_changed",
        "encoder_identity_changed",
        "adc_chain_changed",
        "phase_definition_changed",
        "assembly_changed",
        "axis_or_load_changed",
        "mechanical_frame_changed",
        "voltage_source_changed",
        "angle_reference_changed",
        "torque_source_changed",
        "control_timing_changed",
        "control_limits_changed",
        "offset_reference_changed",
        "lut_reference_changed",
        "kt_reference_changed",
    };
    if ((unsigned int)reason < sizeof causes / sizeof causes[0]) {
        return causes[reason];
    }
    if (reason >= FOC_CAL_REUSE_REASON_IDENTITY_FIRST &&
        reason <= FOC_CAL_REUSE_REASON_IDENTITY_LAST) {
        return identities[reason - FOC_CAL_REUSE_REASON_IDENTITY_FIRST];
    }
    return "unknown_reason";
}

/* ---------------------------------------- end of file ----------------------------------------- */
