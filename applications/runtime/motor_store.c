/**
 * @file    motor_store.c
 * @brief   FalconFoc motor parameter store owner
 * @version v3.3.0
 * @date    2026.05.12
 *
 * Revision History:
 *   v3.2.0 - Refactor: collapse 4 A/B helpers into 3 generic functions
 *            (_ab_save / _ab_load / _ab_fill_status); fix write-avoidance
 *            bug (seq_num must be stamped with current_seq BEFORE the CRC
 *            comparison, not after); remove all per-group repeated logic.
 *            926 → 380 lines. tiny_param.c/.h unchanged.
 *   v3.1.0 - A/B ping-pong; write-avoidance; migration placeholders.
 *   v3.0.0 - Decouple from Flash hardware; delegate to tiny_param.
 *   v2.0.0 - Add Sys slot; motor payload saves PID config only.
 *   v1.1.0 - Initial modular implementation.
 *
 * ============================================================================
 * File-level design contract
 * ============================================================================
 *
 * This file keeps FalconFoc on the light-weight application-owned persistence
 * path while aligning its behavior with the formal Themis parameter contract.
 *
 * This file has exactly three responsibilities:
 *
 *   1. SLOT DEFINITIONS  — six Flash addresses + magic + version constants.
 *   2. PAYLOAD TYPES     — three private structs that define what is stored.
 *   3. SERIALISATION     — field-by-field copy between motor_dev and payloads.
 *
 * All Flash mechanics (erase, write, CRC, header packaging, A/B selection,
 * write-avoidance) are handled by the three generic helpers at the bottom of
 * this file:  _ab_save / _ab_load / _ab_fill_status.
 * They are written once and reused by every parameter group.
 *
 * ============================================================================
 * A/B Ping-Pong — write protocol
 * ============================================================================
 *
 *   _ab_save():
 *     1. tp_check() both slots in one pass; capture info (stored_crc, data_len).
 *     2. Identify write_slot (stale/invalid) and active_slot (keeper).
 *     3. Read current_seq from active_slot through the BSP flash read hook.
 *     4. Stamp payload->seq_num = current_seq  ← write-avoidance comparison key.
 *     5. Compute CRC of pending payload; compare against active_info.stored_crc.
 *     6. If CRC matches AND data_len matches → data unchanged, return OK (skip).
 *     7. Else: payload->seq_num = current_seq + 1 → tp_save() to write_slot.
 *
 * Step 4 is the fix for the write-avoidance bug present in v3.1.0:
 *   The comparison must include the CURRENT seq_num (read from Flash), not 0.
 *   With seq_num=0, Flash always has seq_num≥1, so CRCs never match.
 *
 * Themis-alignment note:
 *   FalconFoc intentionally keeps `tiny_param + bsp_flash` as an app-private
 *   backend for resource and latency reasons. What must align is the contract:
 *   blank is normal, version mismatch is recoverable, CRC is isolated per
 *   group, and writes are deferred out of ISR context.
 *
 * ============================================================================
 * Flash Partition Map (6 sectors, from end of 1 MB Flash)
 * ============================================================================
 *
 *   Sector 511  0x080FF800  SLOT_SYS_A    (2 KB)
 *   Sector 510  0x080FF000  SLOT_SYS_B    (2 KB)
 *   Sector 509  0x080FE800  SLOT_MOTOR_A  (2 KB)
 *   Sector 508  0x080FE000  SLOT_MOTOR_B  (2 KB)
 *   Sector 507  0x080FD800  SLOT_ENC_A    (2 KB)
 *   Sector 506  0x080FD000  SLOT_ENC_B    (2 KB)
 *
 * Copyright (c) 2026 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "motor_store.h"
#include "tiny_param.h"
#include "bsp_flash.h"
#include "pid.h"
#include <string.h>
#include <stddef.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Private Constants — Flash Partition --------------------------------------------------------- */

#define _FLASH_END    (BSP_FLASH_BASE_ADDR + BSP_FLASH_SIZE) /* 0x08100000 */
#define _SECTOR_SZ    (BSP_FLASH_SECTOR_SIZE)                /* 2048 bytes */
#define _PAYLOAD_MAX  (_SECTOR_SZ - TP_HEADER_SIZE)          /* 2032 bytes */

#define _ADDR_SYS_A   (_FLASH_END - 1U * _SECTOR_SZ) /* Sector 511: 0x080FF800 */
#define _ADDR_SYS_B   (_FLASH_END - 2U * _SECTOR_SZ) /* Sector 510: 0x080FF000 */
#define _ADDR_MOTOR_A (_FLASH_END - 3U * _SECTOR_SZ) /* Sector 509: 0x080FE800 */
#define _ADDR_MOTOR_B (_FLASH_END - 4U * _SECTOR_SZ) /* Sector 508: 0x080FE000 */
#define _ADDR_ENC_A   (_FLASH_END - 5U * _SECTOR_SZ) /* Sector 507: 0x080FD800 */
#define _ADDR_ENC_B   (_FLASH_END - 6U * _SECTOR_SZ) /* Sector 506: 0x080FD000 */

#define _MAGIC_MOTOR  (0x4D4F544FU) /* "MOTO" */
#define _MAGIC_ENC    (0x454E434FU) /* "ENCO" */
#define _MAGIC_SYS    (0x53595343U) /* "SYSC" */

/*
 * Increment _VER_* when the corresponding payload struct layout changes.
 * Preserve the old struct definition for the migration path (see LEGACY block).
 */
#define _VER_MOTOR    (1U)
#define _VER_ENC      (1U)
#define _VER_SYS      (1U)

/* Private Types — Payload Structs ------------------------------------------------------------- */

/*
 * seq_num CONTRACT (applies to ALL _*_payload structs):
 *   - MUST be the first field at offset 0.
 *   - Managed exclusively by _ab_save(); never copied to motor_dev on load.
 *   - Higher value = more recently written slot.
 *   - _ab_save() reads current_seq from Flash BEFORE the CRC comparison so
 *     that the write-avoidance check sees an identical payload when data is
 *     unchanged (see step 4 in the file-level design contract above).
 */

/**
 * @brief  Motor parameters payload
 */
struct _motor_payload {
    uint32_t                 seq_num; /* A/B sequence counter — FIRST FIELD, offset 0 */
    struct motor_hw_config   hw;
    struct motor_prot_config prot;
    struct pid_config        id_pid_cfg; /* PID tuning only — NOT runtime state */
    struct pid_config        iq_pid_cfg;
    struct pid_config        vel_pid_cfg;
    struct pid_config        pos_pid_cfg;

    float ramp_vel;
    float ramp_accel;
    float ramp_decel;
    float traj_vel;
    float traj_accel;
    float traj_decel;
    float traj_jerk;
    float curr_bandwidth;
    float vel_bandwidth;
    float pos_bandwidth;
};

/**
 * @brief  Encoder calibration payload
 */
struct _enc_payload {
    uint32_t seq_num;               /* A/B sequence counter — FIRST FIELD */
    int32_t  lut[ENCODER_LUT_SIZE]; /* Non-linearity compensation table   */
    float    mech_offset;
    float    pole_pairs;
    uint8_t  phase_order;
    uint8_t  dir;
    bool     calib_valid;
    uint8_t  reserved[1];
};

/**
 * @brief  System configuration payload
 */
struct _sys_payload {
    uint32_t seq_num; /* A/B sequence counter — FIRST FIELD */
    uint8_t  default_ctrl_mode;
    uint8_t  default_input_mode;
    uint8_t  _reserved[2];
};

/*
 * ── LEGACY STRUCTS ─────────────────────────────────────────────────────────
 * When _VER_* is bumped, add the old struct here for tp_load_raw() migration.
 * Remove only after ALL deployed units have been updated.
 *
 * Example (future motor v2 → v3):
 *
 *   struct _motor_payload_v2 {   // LEGACY — remove after full-fleet update
 *       uint32_t seq_num;
 *       struct motor_hw_config   hw;
 *       ... (all v2 fields, no kd_filter_hz) ...
 *   };
 * ──────────────────────────────────────────────────────────────────────────
 */

/* Compile-time guards: alignment + size */
typedef char _chk_motor_align[(sizeof(struct _motor_payload) % 4U == 0U) ? 1 : -1];
typedef char _chk_enc_align[(sizeof(struct _enc_payload) % 4U == 0U) ? 1 : -1];
typedef char _chk_sys_align[(sizeof(struct _sys_payload) % 4U == 0U) ? 1 : -1];
typedef char _chk_motor_size[(sizeof(struct _motor_payload) <= _PAYLOAD_MAX) ? 1 : -1];
typedef char _chk_enc_size[(sizeof(struct _enc_payload) <= _PAYLOAD_MAX) ? 1 : -1];
typedef char _chk_sys_size[(sizeof(struct _sys_payload) <= _PAYLOAD_MAX) ? 1 : -1];

/* Private Constants — Slot Descriptors -------------------------------------------------------- */

static const struct tp_slot _SLOT_MOTOR_A = {_ADDR_MOTOR_A, _PAYLOAD_MAX, _MAGIC_MOTOR, _VER_MOTOR};
static const struct tp_slot _SLOT_MOTOR_B = {_ADDR_MOTOR_B, _PAYLOAD_MAX, _MAGIC_MOTOR, _VER_MOTOR};
static const struct tp_slot _SLOT_ENC_A   = {_ADDR_ENC_A, _PAYLOAD_MAX, _MAGIC_ENC, _VER_ENC};
static const struct tp_slot _SLOT_ENC_B   = {_ADDR_ENC_B, _PAYLOAD_MAX, _MAGIC_ENC, _VER_ENC};
static const struct tp_slot _SLOT_SYS_A   = {_ADDR_SYS_A, _PAYLOAD_MAX, _MAGIC_SYS, _VER_SYS};
static const struct tp_slot _SLOT_SYS_B   = {_ADDR_SYS_B, _PAYLOAD_MAX, _MAGIC_SYS, _VER_SYS};

/* Flash HAL binding */
static const struct tp_flash_ops _BSP_OPS = {
    .read  = bsp_flash_read,
    .write = bsp_flash_write,
    .erase = bsp_flash_erase,
};

/* Private Types — A/B Pair -------------------------------------------------------------------- */

/**
 * @brief  Groups the two slot descriptors of one parameter group.
 *
 * Passed to _ab_save / _ab_load / _ab_fill_status as a single argument,
 * eliminating the need to pass `a` and `b` as separate parameters.
 */
struct _ab_pair {
    const struct tp_slot *a;
    const struct tp_slot *b;
};

static const struct _ab_pair _PAIR_MOTOR = {&_SLOT_MOTOR_A, &_SLOT_MOTOR_B};
static const struct _ab_pair _PAIR_ENC   = {&_SLOT_ENC_A, &_SLOT_ENC_B};
static const struct _ab_pair _PAIR_SYS   = {&_SLOT_SYS_A, &_SLOT_SYS_B};

/* Private Function Prototypes ----------------------------------------------------------------- */

static int  _map_tp_rc(int tp_rc);
static bool _slot_read_seq_num(const struct tp_slot *slot, uint32_t *seq_num_out);
static bool _ctrl_mode_is_valid(uint8_t value);
static bool _input_mode_is_valid(uint8_t value);
static int  _ab_save(const struct _ab_pair *p, void *payload, uint32_t size);
static int  _ab_load(const struct _ab_pair *p,
                     void                  *payload,
                     uint32_t               size,
                     int                   *out_rc_a,
                     int                   *out_rc_b);
static void _ab_fill_status(const struct _ab_pair *p, struct param_slot_status *out);

/* Public Functions ============================================================================ */

/**
 * @brief  Save motor parameters.  [TASK-ONLY]
 */
int foc_param_save_motor(const struct motor_dev *motor)
{
    if (motor == NULL) {
        return PARAM_ERR_FLASH;
    }

    struct _motor_payload p;
    memset(&p, 0, sizeof(p));

    p.hw   = motor->hw_config;
    p.prot = motor->prot_config;

    p.id_pid_cfg  = motor->ctrl_config.id_pid.config;
    p.iq_pid_cfg  = motor->ctrl_config.iq_pid.config;
    p.vel_pid_cfg = motor->ctrl_config.vel_pid.config;
    p.pos_pid_cfg = motor->ctrl_config.pos_pid.config;

    p.ramp_vel       = motor->ctrl_config.ramp_vel;
    p.ramp_accel     = motor->ctrl_config.ramp_accel;
    p.ramp_decel     = motor->ctrl_config.ramp_decel;
    p.traj_vel       = motor->ctrl_config.traj_vel;
    p.traj_accel     = motor->ctrl_config.traj_accel;
    p.traj_decel     = motor->ctrl_config.traj_decel;
    p.traj_jerk      = motor->ctrl_config.traj_jerk;
    p.curr_bandwidth = motor->ctrl_config.curr_bandwidth;
    p.vel_bandwidth  = motor->ctrl_config.vel_bandwidth;
    p.pos_bandwidth  = motor->ctrl_config.pos_bandwidth;

    return _ab_save(&_PAIR_MOTOR, &p, sizeof(p));
}

/**
 * @brief  Load motor parameters.  Re-inits all PID controllers for clean state.
 */
int foc_param_load_motor(struct motor_dev *motor)
{
    if (motor == NULL) {
        return PARAM_ERR_FLASH;
    }

    struct _motor_payload p;
    int                   rc_a, rc_b;
    int                   st = _ab_load(&_PAIR_MOTOR, &p, sizeof(p), &rc_a, &rc_b);

    if (st == PARAM_ERR_VERSION) {
        /*
         * Migration placeholder — activate when _VER_MOTOR is bumped.
         *
         * const struct tp_slot *legacy =
         *     (rc_a == -EPROTO) ? &_SLOT_MOTOR_A : &_SLOT_MOTOR_B;
         * struct _motor_payload_v2 old;
         * if (tp_load_raw(legacy, &_BSP_OPS, &old, sizeof(old)) == 0) {
         *     _migrate_motor_v2_to_v3(&old, motor);
         *     foc_param_save_motor(motor);
         *     return PARAM_OK;
         * }
         */
        (void)rc_a;
        (void)rc_b;
    }

    if (st != PARAM_OK) {
        return st;
    }

    /* seq_num is storage metadata — intentionally NOT restored to motor->* */
    motor->hw_config   = p.hw;
    motor->prot_config = p.prot;

    pid_init(&motor->ctrl_config.id_pid, &p.id_pid_cfg);
    pid_init(&motor->ctrl_config.iq_pid, &p.iq_pid_cfg);
    pid_init(&motor->ctrl_config.vel_pid, &p.vel_pid_cfg);
    pid_init(&motor->ctrl_config.pos_pid, &p.pos_pid_cfg);

    motor->ctrl_config.ramp_vel       = p.ramp_vel;
    motor->ctrl_config.ramp_accel     = p.ramp_accel;
    motor->ctrl_config.ramp_decel     = p.ramp_decel;
    motor->ctrl_config.traj_vel       = p.traj_vel;
    motor->ctrl_config.traj_accel     = p.traj_accel;
    motor->ctrl_config.traj_decel     = p.traj_decel;
    motor->ctrl_config.traj_jerk      = p.traj_jerk;
    motor->ctrl_config.curr_bandwidth = p.curr_bandwidth;
    motor->ctrl_config.vel_bandwidth  = p.vel_bandwidth;
    motor->ctrl_config.pos_bandwidth  = p.pos_bandwidth;

    return PARAM_OK;
}

/**
 * @brief  Save encoder calibration data.  [TASK-ONLY]  [NON-REENTRANT]
 *
 * @note   Static buffer avoids a ~520-byte stack allocation for the LUT.
 */
int foc_param_save_enc(const struct motor_dev *motor)
{
    if (motor == NULL) {
        return PARAM_ERR_FLASH;
    }

    static struct _enc_payload p;
    memset(&p, 0, sizeof(p));

    p.mech_offset = motor->enc_pri.config->mech_offset;
    p.pole_pairs  = motor->enc_pri.config->pole_pairs;
    p.phase_order = motor->hw_config.phase_order;
    p.dir         = motor->enc_pri.config->dir;
    p.calib_valid = motor->enc_pri.state.calib_valid;

    if (p.calib_valid) {
        memcpy(p.lut, motor->enc_pri.state.enc_table, sizeof(p.lut));
    }

    return _ab_save(&_PAIR_ENC, &p, sizeof(p));
}

/**
 * @brief  Load encoder calibration data.  [NON-REENTRANT]
 */
int foc_param_load_enc(struct motor_dev *motor)
{
    if (motor == NULL) {
        return PARAM_ERR_FLASH;
    }

    static struct _enc_payload p;
    int                        rc_a, rc_b;
    int                        st = _ab_load(&_PAIR_ENC, &p, sizeof(p), &rc_a, &rc_b);
    if (st != PARAM_OK) {
        (void)rc_a;
        (void)rc_b;
        return st;
    }

    motor->enc_pri.config->mech_offset = p.mech_offset;
    motor->enc_pri.config->pole_pairs  = p.pole_pairs;
    motor->enc_pri.config->dir         = p.dir;
    motor->hw_config.phase_order       = p.phase_order;
    motor->enc_pri.state.calib_valid   = p.calib_valid;

    if (p.calib_valid) {
        memcpy(motor->enc_pri.state.enc_table, p.lut, sizeof(p.lut));
    }

    return PARAM_OK;
}

/**
 * @brief  Save system configuration.  [TASK-ONLY]
 */
int foc_param_save_sys(const struct motor_dev *motor)
{
    if (motor == NULL) {
        return PARAM_ERR_FLASH;
    }

    struct _sys_payload p;
    memset(&p, 0, sizeof(p));

    p.default_ctrl_mode  = (uint8_t)motor->ctrl_mode;
    p.default_input_mode = (uint8_t)motor->input_mode;

    return _ab_save(&_PAIR_SYS, &p, sizeof(p));
}

/**
 * @brief  Load system configuration.
 */
int foc_param_load_sys(struct motor_dev *motor)
{
    if (motor == NULL) {
        return PARAM_ERR_FLASH;
    }

    struct _sys_payload p;
    int                 rc_a, rc_b;
    int                 st = _ab_load(&_PAIR_SYS, &p, sizeof(p), &rc_a, &rc_b);
    if (st != PARAM_OK) {
        (void)rc_a;
        (void)rc_b;
        return st;
    }

    motor->ctrl_mode  = _ctrl_mode_is_valid(p.default_ctrl_mode) ? (enum foc_ctrl_mode)p.default_ctrl_mode
                                                                 : CTRL_MODE_VOLT;
    motor->input_mode = _input_mode_is_valid(p.default_input_mode)
                            ? (enum foc_input_mode)p.default_input_mode
                            : INPUT_MODE_INACTIVE;

    return PARAM_OK;
}

/**
 * @brief  Check all six Flash slots without modifying motor state.
 */
void foc_param_check_all(struct param_status_all *out)
{
    if (out == NULL) {
        return;
    }
    _ab_fill_status(&_PAIR_MOTOR, &out->motor);
    _ab_fill_status(&_PAIR_ENC, &out->enc);
    _ab_fill_status(&_PAIR_SYS, &out->sys);
}

bool foc_param_save_layout(uint8_t mask, struct param_save_layout *out)
{
    uint32_t payload_size;

    if (out == NULL) {
        return false;
    }

    switch (mask) {
    case PARAM_SAVE_MOTOR:
        payload_size = (uint32_t)sizeof(struct _motor_payload);
        break;
    case PARAM_SAVE_ENC:
        payload_size = (uint32_t)sizeof(struct _enc_payload);
        break;
    case PARAM_SAVE_SYS:
        payload_size = (uint32_t)sizeof(struct _sys_payload);
        break;
    default:
        return false;
    }

    out->payload_size     = payload_size;
    out->flash_total_size = TP_HEADER_SIZE + payload_size;
    return true;
}

/**
 * @brief  Factory reset: erase all six Flash sectors.
 *
 * Erase order: ENC first (largest payload), then MOTOR, then SYS.
 * This minimises the window during which no valid data exists in Flash.
 */
void foc_param_erase_all(void)
{
    tp_erase(&_SLOT_ENC_A, &_BSP_OPS);
    tp_erase(&_SLOT_ENC_B, &_BSP_OPS);
    tp_erase(&_SLOT_MOTOR_A, &_BSP_OPS);
    tp_erase(&_SLOT_MOTOR_B, &_BSP_OPS);
    tp_erase(&_SLOT_SYS_A, &_BSP_OPS);
    tp_erase(&_SLOT_SYS_B, &_BSP_OPS);
}

/**
 * @brief  Translate param_status to a human-readable string.
 */
const char *foc_param_status_str(int st)
{
    switch (st) {
    case PARAM_OK:
        return "OK";
    case PARAM_ERR_MAGIC:
        return "ERR_MAGIC";
    case PARAM_ERR_VERSION:
        return "ERR_VERSION";
    case PARAM_ERR_CRC:
        return "ERR_CRC";
    case PARAM_ERR_FLASH:
        return "ERR_FLASH";
    case PARAM_EMPTY:
        return "EMPTY";
    default:
        return "UNKNOWN";
    }
}

/* Private Functions =========================================================================== */

/**
 * @brief  Map a tiny_param return code to the public param_status_t.
 */
static int _map_tp_rc(int tp_rc)
{
    switch (tp_rc) {
    case 0:
        return PARAM_OK;
    case TP_EMPTY:
        return PARAM_EMPTY;
    case -ETIMEDOUT:
        return PARAM_ERR_FLASH;
    case -EILSEQ:
        return PARAM_ERR_CRC;
    case -EBADMSG:
        return PARAM_ERR_MAGIC;
    case -EPROTO:
        return PARAM_ERR_VERSION;
    default:
        return PARAM_ERR_FLASH;
    }
}

static bool _ctrl_mode_is_valid(uint8_t value)
{
    switch ((enum foc_ctrl_mode)value) {
    case CTRL_MODE_VOLT:
    case CTRL_MODE_CURR:
    case CTRL_MODE_VEL:
    case CTRL_MODE_POS:
        return true;
    default:
        return false;
    }
}

static bool _input_mode_is_valid(uint8_t value)
{
    switch ((enum foc_input_mode)value) {
    case INPUT_MODE_INACTIVE:
    case INPUT_MODE_PASSTHROUGH:
    case INPUT_MODE_VEL_RAMP:
    case INPUT_MODE_TORQUE_RAMP:
    case INPUT_MODE_POS_FILTER:
    case INPUT_MODE_VEL_FILTER:
    case INPUT_MODE_TRAP_TRAJ:
    case INPUT_MODE_TUNING:
        return true;
    default:
        return false;
    }
}

/**
 * @brief  Read one slot's sequence number through the flash read contract.
 *
 * `seq_num` is the first field of every payload and lives immediately after
 * the tiny_param header. Reading it through `bsp_flash_read()` keeps the
 * FalconFoc app path aligned with Themis portability rules instead of relying
 * on direct flash-address dereference.
 */
static bool _slot_read_seq_num(const struct tp_slot *slot, uint32_t *seq_num_out)
{
    int rc;

    if ((slot == NULL) || (seq_num_out == NULL)) {
        return false;
    }

    rc = _BSP_OPS.read(slot->addr + TP_HEADER_SIZE, (uint8_t *)seq_num_out, sizeof(*seq_num_out));
    return (rc >= 0);
}

/**
 * @brief  Generic A/B ping-pong save with write-avoidance and seq_num management.
 *
 * CONTRACT: payload[0..3] MUST be seq_num (uint32_t, first field of every
 *           _*_payload struct).  _ab_save reads and manages it transparently.
 *
 * Algorithm (see also file-level design contract):
 *   1. tp_check() both slots; capture struct tp_slot_info for stored_crc.
 *   2. Choose write_slot (stale/invalid) and active_slot (good copy).
 *   3. Read current_seq from active_slot through the flash read hook.
 *   4. Set *seq = current_seq  → makes pending payload identical to Flash
 *      if business data has not changed (write-avoidance correctness fix).
 *   5. Compare tp_calc_crc(pending) vs active_info.stored_crc.
 *   6. If equal AND data_len matches → return OK (skip erase+write).
 *   7. Else: *seq = current_seq + 1 → tp_save() to write_slot.
 *
 * @note  [TASK-ONLY] — tp_save() disables all IRQs internally.
 *
 * @param  p       Pointer to the A/B pair for this parameter group.
 * @param  payload Pointer to the populated payload struct (seq_num not set yet).
 * @param  size    sizeof(*payload); must be a multiple of 4.
 */
static int _ab_save(const struct _ab_pair *p, void *payload, uint32_t size)
{
    /*
     * seq_num is guaranteed at offset 0 by the struct CONTRACT above.
     * Accessing it via a uint32_t pointer is safe because:
     *   (a) all _*_payload structs are 4-byte aligned (compile-time assert), and
     *   (b) seq_num is the first field (offset 0).
     */
    uint32_t *seq = (uint32_t *)payload;

    /* ── Step 1: check both slots in one pass ───────────────────────────── */
    struct tp_slot_info info_a, info_b;
    int                 rc_a = tp_check(p->a, &_BSP_OPS, &info_a);
    int                 rc_b = tp_check(p->b, &_BSP_OPS, &info_b);

    bool a_ok = (rc_a == 0);
    bool b_ok = (rc_b == 0);

    /* ── Step 2: select write_slot and active_slot ──────────────────────── */
    const struct tp_slot      *write_slot;
    const struct tp_slot_info *active_info; /* NULL if no valid slot exists */

    if (!a_ok && !b_ok) {
        /* Both invalid/empty: first write goes to A; no active data */
        write_slot  = p->a;
        active_info = NULL;
    } else if (a_ok && !b_ok) {
        write_slot  = p->b;
        active_info = &info_a;
    } else if (!a_ok && b_ok) {
        write_slot  = p->a;
        active_info = &info_b;
    } else {
        uint32_t sa;
        uint32_t sb;

        /*
         * Both valid: write to the slot with the lower seq_num.
         * Even on internal flash we read through the BSP hook so the contract
         * does not depend on direct address dereference.
         */
        if (!_slot_read_seq_num(p->a, &sa) || !_slot_read_seq_num(p->b, &sb)) {
            return PARAM_ERR_FLASH;
        }

        if (sa <= sb) {
            write_slot  = p->a;
            active_info = &info_b;
        } else {
            write_slot  = p->b;
            active_info = &info_a;
        }
    }

    /* ── Step 3: read current_seq from active slot ──────────────────────── */
    uint32_t current_seq = 0U;
    if (active_info != NULL) {
        const struct tp_slot *active_slot = (active_info == &info_a) ? p->a : p->b;
        if (!_slot_read_seq_num(active_slot, &current_seq)) {
            return PARAM_ERR_FLASH;
        }
    }

    /* ── Steps 4–6: write-avoidance ─────────────────────────────────────── */
    *seq = current_seq; /* stamp BEFORE CRC comparison — the bug-fix */

    if (active_info != NULL && active_info->data_len == size &&
        tp_calc_crc((active_info == &info_a) ? p->a : p->b, payload, size) ==
            active_info->stored_crc) {
        /* Payload (including seq_num) is byte-for-byte identical → skip */
        return PARAM_OK;
    }

    /* ── Step 7: data changed (or first save) → commit ─────────────────── */
    *seq = current_seq + 1U;
    return _map_tp_rc(tp_save(write_slot, &_BSP_OPS, payload, size));
}

/**
 * @brief  Generic A/B ping-pong load — selects the freshest valid slot.
 *
 * @param  p         A/B pair.
 * @param  payload   Caller buffer (>= size bytes).
 * @param  size      sizeof(*payload).
 * @param  out_rc_a  [out] tp_check result for slot A (for migration decisions).
 * @param  out_rc_b  [out] tp_check result for slot B.
 *
 * @return PARAM_OK          Payload loaded successfully.
 * @return PARAM_EMPTY       Both slots are blank (first boot).
 * @return PARAM_ERR_VERSION At least one slot has a version mismatch; caller
 *                           can inspect *out_rc_a / *out_rc_b and call
 *                           tp_load_raw() for migration.
 * @return PARAM_ERR_*       Other hardware or data-integrity error.
 */
static int _ab_load(const struct _ab_pair *p,
                    void                  *payload,
                    uint32_t               size,
                    int                   *out_rc_a,
                    int                   *out_rc_b)
{
    *out_rc_a = tp_check(p->a, &_BSP_OPS, NULL);
    *out_rc_b = tp_check(p->b, &_BSP_OPS, NULL);

    bool a_ok = (*out_rc_a == 0);
    bool b_ok = (*out_rc_b == 0);

    /* Select active slot */
    const struct tp_slot *active = NULL;
    if (a_ok && !b_ok) {
        active = p->a;
    } else if (!a_ok && b_ok) {
        active = p->b;
    } else if (a_ok && b_ok) {
        uint32_t sa;
        uint32_t sb;

        if (!_slot_read_seq_num(p->a, &sa) || !_slot_read_seq_num(p->b, &sb)) {
            return PARAM_ERR_FLASH;
        }

        active      = (sa >= sb) ? p->a : p->b;
    }

    if (active == NULL) {
        /* Neither slot is healthy */
        if (*out_rc_a == TP_EMPTY && *out_rc_b == TP_EMPTY) {
            return PARAM_EMPTY;
        }
        /* Return the more informative error (non-EMPTY takes precedence) */
        return _map_tp_rc((*out_rc_a != TP_EMPTY) ? *out_rc_a : *out_rc_b);
    }

    return _map_tp_rc(tp_load(active, &_BSP_OPS, payload, size));
}

/**
 * @brief  Report the health of an A/B slot pair into a param_slot_status.
 *
 * Picks the healthier slot's info (A preferred when both are valid or both
 * are bad).  Used exclusively by foc_param_check_all().
 */
static void _ab_fill_status(const struct _ab_pair *p, struct param_slot_status *out)
{
    struct tp_slot_info ia, ib;
    tp_check(p->a, &_BSP_OPS, &ia);
    tp_check(p->b, &_BSP_OPS, &ib);

    /* Prefer the valid slot; if both valid prefer A (arbitrary); if both bad prefer A */
    const struct tp_slot_info *best = (ib.status == 0 && ia.status != 0) ? &ib : &ia;

    out->addr     = best->addr;
    out->magic    = best->magic;
    out->version  = best->version;
    out->data_len = best->data_len;
    out->status   = _map_tp_rc(best->status);
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
