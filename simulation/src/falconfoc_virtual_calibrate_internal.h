/**
 * @file    falconfoc_virtual_calibrate_internal.h
 * @brief   Private foreground seams of the Host calibration executable.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Design overview
 * ---------------
 * Share the established application, closed-loop verification and FCAL
 * adapters with the interactive reuse owner.
 *
 * Usage example
 * -------------
 * Preview with the draft helper, then verify the reviewed candidate with a
 * fresh initializer and the existing virtual control trials.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The Host command loop serializes review and application. The initializer's
 * sensor storage must outlive its initialized Runtime.
 *
 * Failure / recovery
 * ------------------
 * A rejected request never becomes the active reuse history.
 */

#pragma once

/* Includes --------------------------------------------------------------------- */

#include "foc_pmsm_reachability.h"
#include "foc_virtual_calibration.h"
#include "foc_virtual_cogging.h"

/* Public Types ----------------------------------------------------------------- */

struct calibrated_init {
    const struct foc_cal_application *application;
    const struct foc_cal_application_tuning_policy *policy;
    const struct foc_cal_application_candidate *candidate;
    const struct foc_encoder_lut *restored_lut;
    struct foc_cal_application_sensor sensor;
};

enum foc_virtual_cal_cli_stage {
    CAL_CLI_ADC = FOC_VIRTUAL_CAL_ADC,
    CAL_CLI_LUT = FOC_VIRTUAL_CAL_LUT,
    CAL_CLI_ROTOR = FOC_VIRTUAL_CAL_ROTOR,
    CAL_CLI_OFFSET = FOC_VIRTUAL_CAL_OFFSET,
    CAL_CLI_ELECTRICAL_D = FOC_VIRTUAL_CAL_ELECTRICAL_D,
    CAL_CLI_ELECTRICAL_Q = FOC_VIRTUAL_CAL_ELECTRICAL_Q,
    CAL_CLI_FLUX = FOC_VIRTUAL_CAL_FLUX,
    CAL_CLI_MECHANICAL = FOC_VIRTUAL_CAL_MECHANICAL,
    CAL_CLI_TUNE = FOC_VIRTUAL_CAL_TUNE,
    CAL_CLI_BOOTSTRAP_ADC,
    CAL_CLI_BOOTSTRAP_SCAN,
    CAL_CLI_BOOTSTRAP_FIT,
    CAL_CLI_LIVE_ADC,
    CAL_CLI_LIVE_ENCODER,
    CAL_CLI_REUSE_PLAN,
    CAL_CLI_REUSE_RUNTIME,
    CAL_CLI_QUIESCENT,
    CAL_CLI_COGGING,
    CAL_CLI_GAIN_CANDIDATE,
    CAL_CLI_REVIEW,
    CAL_CLI_APPLICATION,
    CAL_CLI_CONTROL_DIAGNOSTIC,
    CAL_CLI_CONTROL_SAFETY,
    CAL_CLI_CONTROL_HOLDOUT,
    CAL_CLI_POST_APPLY_QUIESCENT,
    CAL_CLI_FCAL_SAVE,
    CAL_CLI_FCAL_LOAD,
    CAL_CLI_STAGE_COUNT,
};

enum foc_virtual_cal_cli_stage_status {
    CAL_CLI_PENDING,
    CAL_CLI_RUNNING,
    CAL_CLI_COMPLETED,
    CAL_CLI_FAILED,
    CAL_CLI_REUSED,
    CAL_CLI_SKIPPED,
};

#define CAL_CLI_BIT(stage) (UINT32_C(1) << (stage))
#define CAL_CLI_CAPTURE_STAGES (CAL_CLI_BIT(CAL_CLI_TUNE + 1) - 1U)
#define CAL_CLI_BOOTSTRAP_STAGES \
    (CAL_CLI_BIT(CAL_CLI_BOOTSTRAP_ADC) | CAL_CLI_BIT(CAL_CLI_BOOTSTRAP_SCAN) | \
     CAL_CLI_BIT(CAL_CLI_BOOTSTRAP_FIT))
#define CAL_CLI_APPLY_STAGES \
    (CAL_CLI_BIT(CAL_CLI_REVIEW) | CAL_CLI_BIT(CAL_CLI_APPLICATION) | \
     CAL_CLI_BIT(CAL_CLI_CONTROL_SAFETY) | CAL_CLI_BIT(CAL_CLI_CONTROL_HOLDOUT))

struct foc_virtual_cal_cli_timing {
    const char *mode;
    uint32_t planned;
    enum foc_virtual_cal_cli_stage_status states[CAL_CLI_STAGE_COUNT];
    uint64_t host_wall_start_us;
    uint64_t host_compute_start_us;
};

struct foc_virtual_calibration_package;

/* Public Functions ------------------------------------------------------------- */

extern const struct foc_cal_application_tuning_policy foc_virtual_cal_cli_tuning;

void foc_virtual_cal_cli_timing_reset(struct foc_virtual_cal_cli_timing *timing, uint32_t planned);

void foc_virtual_cal_cli_timing_start(struct foc_virtual_cal_cli_timing *timing,
                                     enum foc_virtual_cal_cli_stage stage);

/** A negative simulated duration means the interval was unavailable. */
void foc_virtual_cal_cli_timing_finish(struct foc_virtual_cal_cli_timing *timing,
                                      enum foc_virtual_cal_cli_stage stage,
                                      bool success,
                                      int64_t simulated_motor_us);

/** Report configured workflow stages that need no new execution. */
void foc_virtual_cal_cli_timing_omit(struct foc_virtual_cal_cli_timing *timing,
                                    uint32_t stages,
                                    enum foc_virtual_cal_cli_stage_status status);

int foc_virtual_cal_cli_error(const char *reason, const char *stage, int native);

int foc_virtual_cal_cli_initialize(void *argument,
                                   struct foc_runtime *runtime,
                                   struct foc_encoder_context *encoder,
                                   const struct foc_pipeline_config *pipeline,
                                   const struct foc_virtual_current_profile *profile);

bool foc_virtual_cal_cli_readback(struct foc_virtual_current *driver,
                                  const struct foc_cal_application_candidate *candidate,
                                  bool emit);

int foc_virtual_cal_cli_verify(struct foc_virtual_current_profile *profile,
                               struct calibrated_init *init,
                               struct foc_virtual_calibration *session,
                               const char *save_config,
                               const char *current_study_path,
                               bool startup_study,
                               struct foc_virtual_cal_cli_timing *timing);

void foc_virtual_cal_cli_preview(const struct foc_cal_application_candidate *candidate,
                                 uint32_t review_id);

bool foc_virtual_cal_cli_review_matches(const char *approved, uint32_t review_id);

int foc_virtual_cal_cli_draft(const struct foc_virtual_calibration *session,
                              const struct foc_cal_application_tuning_policy *policy,
                              struct foc_cal_application_candidate *candidate);

void foc_virtual_cal_cli_cogging_fixture(struct foc_virtual_current_profile *profile);

int foc_virtual_cal_cli_cogging_capture(const struct foc_virtual_current_profile *profile,
                                        struct foc_virtual_calibration *session,
                                        struct foc_virtual_cogging_result *capture);

int foc_virtual_cal_cli_restore(const char *path,
                                struct foc_virtual_current_profile *profile,
                                const struct foc_cal_workflow_identity *identity,
                                const char *approved,
                                const struct foc_pmsm_operating_point *diagnostic,
                                struct foc_virtual_cal_cli_timing *timing,
                                const char *quick_scenario);

void foc_virtual_calibration_schedule(struct foc_virtual_calibration *session);

int foc_virtual_cal_cli_reuse_session(const char *name,
                                      bool cogging,
                                      const char *bootstrap,
                                      struct foc_virtual_cal_cli_timing *timing);

int foc_virtual_cal_cli_bootstrap(const char *mode,
                                  const char *scenario,
                                  const char *trace,
                                  struct foc_virtual_current_profile *profile,
                                  struct foc_cal_workflow_request *request,
                                  uint32_t *signature);

int foc_virtual_cal_cli_bootstrap_timed(const char *mode,
                                        const char *scenario,
                                        const char *trace,
                                        struct foc_virtual_current_profile *profile,
                                        struct foc_cal_workflow_request *request,
                                        uint32_t *signature,
                                        struct foc_virtual_cal_cli_timing *timing);

int foc_virtual_cal_cli_quick_check(const struct foc_virtual_current_profile *profile,
                                    const struct foc_cal_workflow_identity *identity,
                                    const struct foc_virtual_calibration_package *package,
                                    const char *scenario,
                                    struct foc_virtual_cal_cli_timing *timing);

struct foc_flux_id_result;

void foc_virtual_cal_cli_flux_preview(const struct foc_flux_id_result *evidence);
uint32_t foc_virtual_cal_cli_flux_signature(const struct foc_flux_id_result *evidence);

/* ---------------------------------------- end of file ----------------------------------------- */
