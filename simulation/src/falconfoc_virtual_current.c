/**
 * @file    falconfoc_virtual_current.c
 * @brief   Deterministic Host executable for VM-01a Current scenarios.
 * @version v0.1.0
 * @date    2026.11.15
 *
 * Implementation role
 * -------------------
 * Exercise the production Runtime and Current PI through a virtual PMSM
 * without changing the locked Board1 target.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_current.h"

#include <stdio.h>
#include <string.h>

/* Private Types ---------------------------------------------------------------- */

struct scenario_report {
    const char *name;
    struct foc_virtual_current_observation observation;
    uint32_t hash;
};

/* Private Helpers -------------------------------------------------------------- */

static int check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "scenario failure: %s\n", message);
        return 1;
    }
    return 0;
}

static int start_current(struct foc_virtual_current *context, float iq_a)
{
    if (foc_virtual_current_publish_iq(context, 0.0f, iq_a) != FOC_RUNTIME_OK ||
        foc_virtual_current_run_until(context, 0U) != 0 ||
        foc_virtual_current_queue_action(context, FOC_RUNTIME_ACTION_ARM) != 0 ||
        foc_virtual_current_run_until(context, 200U) != 0 ||
        foc_virtual_current_queue_action(context, FOC_RUNTIME_ACTION_START) != 0 ||
        foc_virtual_current_run_until(context, 400U) != 0) {
        return 1;
    }
    return 0;
}

static int run_with_refresh(struct foc_virtual_current *context, uint32_t target_us, float iq_a)
{
    while (context->virtual_time_us < target_us) {
        if (foc_virtual_current_publish_iq(context, 0.0f, iq_a) != FOC_RUNTIME_OK) {
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

static int observe(struct foc_virtual_current *context, struct scenario_report *report)
{
    if (foc_virtual_current_observe(context, &report->observation) != 0) {
        return 1;
    }
    report->hash = foc_virtual_current_hash(context);
    return 0;
}

static void print_report(const struct scenario_report *report)
{
    const struct foc_virtual_current_observation *observation = &report->observation;
    printf(
        "%s time_us=%u state=%d faults=%u id_ref=%g iq_ref=%g id=%g iq=%g "
        "updates=%u rejected=%u position=%g speed=%g hash=%u\n",
        report->name,
        observation->virtual_time_us,
        observation->safety_state,
        observation->faults,
        observation->id_ref_a,
        observation->iq_ref_a,
        observation->id_a,
        observation->iq_a,
        observation->accepted_updates,
        observation->rejected_updates,
        observation->mechanical_position_rad,
        observation->mechanical_velocity_rad_s,
        report->hash
    );
}

static void write_csv_header(FILE *file)
{
    fputs(
        "scenario,virtual_time_us,id_ref_a,iq_ref_a,id_a,iq_a,duty_a,duty_b,duty_c,"
        "v_alpha_v,v_beta_v,accepted_updates,rejected_updates,safety_state,faults,"
        "mechanical_position_rad,mechanical_velocity_rad_s,replay_hash\n",
        file
    );
}

static void write_csv_row(FILE *file, const struct scenario_report *report)
{
    const struct foc_virtual_current_observation *observation = &report->observation;
    fprintf(
        file,
        "%s,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%u,%d,%u,%.9g,%.9g,%u\n",
        report->name,
        observation->virtual_time_us,
        observation->id_ref_a,
        observation->iq_ref_a,
        observation->id_a,
        observation->iq_a,
        observation->duty_a,
        observation->duty_b,
        observation->duty_c,
        observation->v_alpha_v,
        observation->v_beta_v,
        observation->accepted_updates,
        observation->rejected_updates,
        observation->safety_state,
        observation->faults,
        observation->mechanical_position_rad,
        observation->mechanical_velocity_rad_s,
        report->hash
    );
}

static int scenario_current_step(
    const struct foc_virtual_current_profile *profile,
    struct scenario_report *report
)
{
    struct foc_virtual_current context;
    if (foc_virtual_current_init(&context, profile) != 0 || start_current(&context, 0.45f) != 0 ||
        run_with_refresh(&context, 8000U, 0.45f) != 0 || observe(&context, report) != 0) {
        return 1;
    }
    report->name = "CASE-01-current-step";
    return check(
        report->observation.accepted_updates > 0U &&
            report->observation.safety_state == FOC_SAFE_STATE_RUNNING,
        "current step did not remain running with accepted updates"
    );
}

static int scenario_current_reverse(
    const struct foc_virtual_current_profile *profile,
    struct scenario_report *report
)
{
    struct foc_virtual_current context;
    if (foc_virtual_current_init(&context, profile) != 0 || start_current(&context, 0.45f) != 0 ||
        run_with_refresh(&context, 4000U, 0.45f) != 0 ||
        foc_virtual_current_publish_iq(&context, 0.0f, -0.45f) != FOC_RUNTIME_OK ||
        run_with_refresh(&context, 9000U, -0.45f) != 0 || observe(&context, report) != 0) {
        return 1;
    }
    report->name = "CASE-02-current-reverse";
    return check(report->observation.accepted_updates > 0U, "reverse case has no accepted updates");
}

static int scenario_rejected_update(
    const struct foc_virtual_current_profile *profile,
    struct scenario_report *report
)
{
    struct foc_virtual_current context;
    if (foc_virtual_current_init(&context, profile) != 0 || start_current(&context, 0.45f) != 0 ||
        run_with_refresh(&context, 3000U, 0.45f) != 0 ||
        foc_virtual_current_reject_next_update(&context) != 0 ||
        foc_virtual_current_publish_iq(&context, 0.0f, 0.55f) != FOC_RUNTIME_OK ||
        foc_virtual_current_run_until(&context, 3200U) != 0 || observe(&context, report) != 0) {
        return 1;
    }
    report->name = "CASE-03-update-reject";
    return check(
        report->observation.rejected_updates == 1U &&
            report->observation.safety_state == FOC_SAFE_STATE_DISABLED,
        "rejected update did not inhibit the virtual Runtime"
    );
}

static int
scenario_stop(const struct foc_virtual_current_profile *profile, struct scenario_report *report)
{
    struct foc_virtual_current context;
    if (foc_virtual_current_init(&context, profile) != 0 || start_current(&context, 0.45f) != 0 ||
        run_with_refresh(&context, 3000U, 0.45f) != 0 ||
        foc_virtual_current_request_stop(&context) != 0 ||
        foc_virtual_current_run_until(&context, 3200U) != 0 || observe(&context, report) != 0) {
        return 1;
    }
    report->name = "CASE-04-stop";
    return check(
        report->observation.safety_state == FOC_SAFE_STATE_DISABLED,
        "stop did not reach disabled state"
    );
}

static int
scenario_replay(const struct foc_virtual_current_profile *profile, struct scenario_report *report)
{
    struct foc_virtual_current first;
    struct foc_virtual_current second;
    struct scenario_report second_report;
    if (foc_virtual_current_init(&first, profile) != 0 ||
        foc_virtual_current_init(&second, profile) != 0 || start_current(&first, 0.45f) != 0 ||
        start_current(&second, 0.45f) != 0 || run_with_refresh(&first, 8000U, 0.45f) != 0 ||
        run_with_refresh(&second, 8000U, 0.45f) != 0 || observe(&first, report) != 0 ||
        observe(&second, &second_report) != 0) {
        return 1;
    }
    report->name = "CASE-05-replay";
    return check(report->hash == second_report.hash, "replay fingerprints differ");
}

/* Public Functions ------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    FILE *csv = NULL;
    if (argc == 3 && strcmp(argv[1], "--csv") == 0) {
#ifdef _WIN32
        if (fopen_s(&csv, argv[2], "w") != 0) {
            csv = NULL;
#else
        csv = fopen(argv[2], "w");
        if (csv == NULL) {
#endif
            fprintf(stderr, "cannot open CSV path: %s\n", argv[2]);
            return 1;
        }
        write_csv_header(csv);
    } else if (argc != 1) {
        fprintf(stderr, "usage: falconfoc-virtual-current [--csv path]\n");
        return 2;
    }

    struct scenario_report reports[5] = {
        {.name = "CASE-01-current-step"},
        {.name = "CASE-02-current-reverse"},
        {.name = "CASE-03-update-reject"},
        {.name = "CASE-04-stop"},
        {.name = "CASE-05-replay"},
    };
    if (scenario_current_step(&profile, &reports[0]) != 0 ||
        scenario_current_reverse(&profile, &reports[1]) != 0 ||
        scenario_rejected_update(&profile, &reports[2]) != 0 ||
        scenario_stop(&profile, &reports[3]) != 0 || scenario_replay(&profile, &reports[4]) != 0) {
        if (csv != NULL) {
            fclose(csv);
        }
        return 1;
    }
    puts(
        "profile=GENERIC_RESEARCH_MOTOR vbus_v=12 h0_us=200 h1_us=500 "
        "output=virtual-high-z-on-inhibit"
    );
    for (unsigned int index = 0U; index < 5U; ++index) {
        print_report(&reports[index]);
        if (csv != NULL) {
            write_csv_row(csv, &reports[index]);
        }
    }
    if (csv != NULL) {
        fclose(csv);
    }
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
