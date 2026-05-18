/**
 * @file    foc_param.h
 * @brief   Host shim for motor_fsm and shell-store unit coverage
 * @version v1.1.0
 * @date    2026.05.14
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PARAM_OK          (0)
#define PARAM_ERR_MAGIC   (1)
#define PARAM_ERR_VERSION (2)
#define PARAM_ERR_CRC     (3)
#define PARAM_ERR_FLASH   (4)
#define PARAM_EMPTY       (5)

#define PARAM_SAVE_NONE   (0x00U)
#define PARAM_SAVE_ENC    (0x01U)
#define PARAM_SAVE_MOTOR  (0x02U)
#define PARAM_SAVE_SYS    (0x04U)
#define PARAM_SAVE_ALL    (PARAM_SAVE_ENC | PARAM_SAVE_MOTOR | PARAM_SAVE_SYS)

struct param_slot_status {
    uint32_t addr;
    uint32_t magic;
    uint32_t version;
    uint32_t data_len;
    int      status;
};

struct param_status_all {
    struct param_slot_status motor;
    struct param_slot_status enc;
    struct param_slot_status sys;
};

int         foc_param_save_motor(const struct motor_dev *motor);
int         foc_param_load_motor(struct motor_dev *motor);
int         foc_param_save_enc(const struct motor_dev *motor);
int         foc_param_load_enc(struct motor_dev *motor);
int         foc_param_save_sys(const struct motor_dev *motor);
int         foc_param_load_sys(struct motor_dev *motor);
void        foc_param_check_all(struct param_status_all *out);
void        foc_param_erase_all(void);
const char *foc_param_status_str(int st);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
