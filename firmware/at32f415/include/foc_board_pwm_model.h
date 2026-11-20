/**
 * @file    foc_board_pwm_model.h
 * @brief   Board1 PWM output transaction contract for isolated Host tests.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Design overview
 * ---------------
 * Stage an admitted request, witness all three writes, then accept only the
 * designated timer load. No MCU headers, registers or electrical actions.
 *
 * Usage example
 * -------------
 * Open a simulated session, admit one request, witness C1/C2/C3, then load.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Serialized Host event model. H0 owns request and write events. The test
 * supplies final Guard and timer witnesses and can interleave inhibition.
 *
 * Failure / recovery
 * ------------------
 * A failed transaction clears both banks and revokes the simulated session.
 * Recovery requires an explicit qualified rearm with a newer session.
 */
#pragma once

#include "foc_board_timing.h"

/* Public Types ---------------------------------------------------------------- */
enum foc_board_pwm_stage {
    FOC_BOARD_PWM_INHIBITED,
    FOC_BOARD_PWM_UPDATE_ACCEPTED,
    FOC_BOARD_PWM_SHADOW_WRITTEN,
    FOC_BOARD_PWM_MODEL_ACTIVE,
};

struct foc_board_pwm_id {
    uint32_t session;
    uint32_t submission;
    uint32_t slot;
    uint32_t generation;
    uint32_t command;
};

struct foc_board_pwm_model {
    struct foc_board_pwm_id latest_id;
    struct foc_board_pwm_id active_id;
    struct foc_board_load_plan plan;
    struct foc_board_shadow_attempt timing;
    uint32_t latest_session;
    uint32_t requested[3];
    uint32_t shadow[3];
    uint32_t compare[3];
    unsigned int written;
    enum foc_board_pwm_stage stage;
    bool session_open;
    bool identity_seen;
    bool active;
};

/* Public Functions ------------------------------------------------------------- */

/** Starts a new model session after an explicit recovery/arming witness. */
bool foc_board_pwm_rearm(struct foc_board_pwm_model *port, uint32_t session, bool recovery_allowed);

/** Accept a proposed full write after the final guard and compare validation.
 * The projected last-write time is admitted here and checked at each write. */
enum foc_board_pwm_stage foc_board_pwm_accept(struct foc_board_pwm_model *port,
                                              const struct foc_board_pwm_id *id,
                                              const struct foc_board_adc_transaction *token,
                                              const struct foc_board_load_plan *plan,
                                              const struct foc_board_shadow_attempt *timing,
                                              const struct foc_device_pwm_config *config,
                                              const struct foc_device_pwm_request *request,
                                              bool guard_allowed);

/** Witness a single model register write in C1/C2/C3 order. */
enum foc_board_pwm_stage foc_board_pwm_write(struct foc_board_pwm_model *port,
                                             const struct foc_board_pwm_id *id,
                                             unsigned int channel,
                                             uint32_t compare,
                                             struct foc_board_carrier_stamp observed);

/** Consume the qualified load once. Reports model Active, never physical torque. */
enum foc_board_pwm_stage foc_board_pwm_load(struct foc_board_pwm_model *port,
                                            const struct foc_board_pwm_id *id,
                                            struct foc_board_carrier_stamp observed,
                                            bool timer_witness_valid,
                                            bool guard_allowed);

/** Common STOP/FAULT/reset event in the model. Retires all outstanding writes. */
void foc_board_pwm_inhibit(struct foc_board_pwm_model *port);

/* ---------------------------------------- end of file ----------------------------------------- */
