/**
 * @file    foc_param.h
 * @brief   FalconFoc application-owned parameter persistence contract
 * @version v3.3.0
 * @date    2026.05.12
 *
 * Design overview
 * ---------------
 * FalconFoc keeps its parameter persistence inside the application layer on
 * purpose:
 * - backend = `tiny_param + bsp_flash`
 * - ownership = `applications/algo/foc_param.*`
 * - runtime save trigger = product task context only
 *
 * This product does not adopt the full Themis `device/` + `drivers/` runtime
 * storage path, but it must still follow the same formal persistence semantics:
 * - blank slot is a normal first-boot state
 * - version mismatch means "load defaults, then migrate or re-save"
 * - CRC / flash errors are isolated per parameter group
 * - flash writes are deferred out of ISR and completed in RTOS task context
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * - `motor`, `enc`, and `sys` are three independent A/B parameter groups.
 * - Save APIs are task-context only. Callers must never erase/write from ISR.
 * - Load APIs may be used during bring-up before task creation.
 * - `PARAM_SAVE_*` flags are the ISR-to-task handoff contract.
 *
 * Failure / recovery
 * ------------------
 * - `PARAM_EMPTY`: normal first boot, apply compiled defaults.
 * - `PARAM_ERR_VERSION`: schema mismatch, apply defaults and re-save or run a
 *   future migration path.
 * - `PARAM_ERR_CRC` / `PARAM_ERR_FLASH`: keep other groups independent and do
 *   not block system bring-up.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_common.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Constants ----------------------------------------------------------------------------- */

/**
 * @brief  Deferred save request flags.
 *
 * These flags are the formal ISR-to-task persistence handoff:
 * - producer: ISR / calibration callback / shell command
 * - consumer: safe RTOS task that performs the real flash write
 */
#define PARAM_SAVE_NONE   (0x00U) /* No pending save */
#define PARAM_SAVE_ENC    (0x01U) /* Encoder calibration data pending save */
#define PARAM_SAVE_MOTOR  (0x02U) /* Motor + PID parameters pending save */
#define PARAM_SAVE_SYS    (0x04U) /* System configuration pending save */
#define PARAM_SAVE_ALL    (PARAM_SAVE_ENC | PARAM_SAVE_MOTOR | PARAM_SAVE_SYS)

/** Operation completed successfully. */
#define PARAM_OK          (0)
/** Header magic mismatch or slot content belongs to another payload. */
#define PARAM_ERR_MAGIC   (1)
/** Stored payload version mismatches the current schema. */
#define PARAM_ERR_VERSION (2)
/** CRC verification failed; stored data must be treated as corrupted. */
#define PARAM_ERR_CRC     (3)
/** Backend flash read/write/erase path failed. */
#define PARAM_ERR_FLASH   (4)
/** Slot is blank and has never been persisted before. */
#define PARAM_EMPTY       (5)

/* Public types --------------------------------------------------------------------------------- */

/**
 * @brief  Status snapshot for one parameter group.
 *
 * Returned by `foc_param_check_all()` for shell diagnostics without touching
 * the live control state.
 */
struct param_slot_status {
    uint32_t addr;     /* Slot base address */
    uint32_t magic;    /* Magic word read from Flash (0xFFFFFFFF if empty) */
    uint32_t version;  /* Version read from Flash */
    uint32_t data_len; /* Payload length stored in header */
    int      status;   /* Result of header + CRC check */
};

/**
 * @brief  Aggregated status for all three parameter groups.
 */
struct param_status_all {
    struct param_slot_status motor; /* SLOT_MOTOR status */
    struct param_slot_status enc;   /* SLOT_ENC status   */
    struct param_slot_status sys;   /* SLOT_SYS status   */
};

/* Public functions ----------------------------------------------------------------------------- */

/**
 * @brief  Save the motor parameter group.
 *
 * Payload includes:
 * - hardware and protection configuration
 * - PID tuning configuration only, not PID runtime integrator state
 * - ramp / trajectory / bandwidth settings
 *
 * @note   Task context only. Do not call from ISR.
 * @param  motor  Caller-owned motor instance.
 * @return `PARAM_OK` on success, otherwise one `PARAM_ERR_*` code.
 */
int foc_param_save_motor(const struct motor_dev *motor);

/**
 * @brief  Load the motor parameter group.
 *
 * On success, static configuration is restored and PID objects are re-created
 * through `pid_init()` so runtime state always starts clean.
 *
 * @param  motor  Caller-owned motor instance.
 * @return `PARAM_OK`, `PARAM_EMPTY`, `PARAM_ERR_VERSION`, or other error code.
 */
int foc_param_load_motor(struct motor_dev *motor);

/**
 * @brief  Save the encoder calibration parameter group.
 *
 * @note   Task context only. Typical producer path is:
 *         `param_save_req |= PARAM_SAVE_ENC` -> safe task drains request.
 * @param  motor  Caller-owned motor instance.
 * @return `PARAM_OK` on success, otherwise one `PARAM_ERR_*` code.
 */
int foc_param_save_enc(const struct motor_dev *motor);

/**
 * @brief  Load the encoder calibration parameter group.
 *
 * @param  motor  Caller-owned motor instance.
 * @return `PARAM_OK`, `PARAM_EMPTY`, or another persistence status code.
 */
int foc_param_load_enc(struct motor_dev *motor);

/**
 * @brief  Save the system default parameter group.
 *
 * Current payload stores the default control mode and input mode only.
 *
 * @note   Task context only. Do not call from ISR.
 * @param  motor  Caller-owned motor instance.
 * @return `PARAM_OK` on success, otherwise one `PARAM_ERR_*` code.
 */
int foc_param_save_sys(const struct motor_dev *motor);

/**
 * @brief  Load the system default parameter group.
 *
 * @param  motor  Caller-owned motor instance.
 * @return `PARAM_OK`, `PARAM_EMPTY`, or another persistence status code.
 */
int foc_param_load_sys(struct motor_dev *motor);

/**
 * @brief  Inspect all parameter groups without modifying live runtime state.
 *
 * @param  out  Caller-owned destination snapshot.
 */
void foc_param_check_all(struct param_status_all *out);

/**
 * @brief  Erase all parameter groups.
 *
 * @note   Irreversible and task-context only.
 */
void foc_param_erase_all(void);

/**
 * @brief  Translate one public persistence status code to text.
 *
 * @param  st  Public `PARAM_*` status code.
 * @return Stable static string literal.
 */
const char *foc_param_status_str(int st);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
