/**
 * @file    test_app_shell_store_contract.c
 * @brief   Host contract tests for FalconFoc `store` shell behavior
 * @version v1.0.0
 * @date    2026.05.14
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_shell_commands.h"
#include "foc_param.h"
#include "tm_shell.h"

#define TEST_ASSERT(expr)                                                                \
    do {                                                                                 \
        if (!(expr)) {                                                                   \
            fprintf(stderr,                                                              \
                    "[test_app_shell_store_contract] assertion failed: %s (%s:%d)\n",   \
                    #expr,                                                               \
                    __FILE__,                                                            \
                    __LINE__);                                                           \
            exit(1);                                                                     \
        }                                                                                \
    } while (0)

struct motor_dev _motor_ins;

static int  g_save_motor_calls;
static int  g_save_enc_calls;
static int  g_save_sys_calls;
static int  g_load_motor_calls;
static int  g_load_enc_calls;
static int  g_load_sys_calls;
static int  g_erase_all_calls;
static int  g_restore_apply_calls;
static char g_output[1024];
static size_t g_output_len;

static void test_env_reset(void)
{
    memset(&_motor_ins, 0, sizeof(_motor_ins));
    g_save_motor_calls = 0;
    g_save_enc_calls   = 0;
    g_save_sys_calls   = 0;
    g_load_motor_calls = 0;
    g_load_enc_calls   = 0;
    g_load_sys_calls   = 0;
    g_erase_all_calls  = 0;
    g_restore_apply_calls = 0;
    memset(g_output, 0, sizeof(g_output));
    g_output_len = 0U;
}

const char *app_shell_axis_state_name(uint8_t state)
{
    switch (state) {
    case AXIS_STATE_IDLE:
        return "idle";
    case AXIS_STATE_CALIB:
        return "calib";
    case AXIS_STATE_RUNNING:
        return "running";
    case AXIS_STATE_FAULT:
        return "fault";
    default:
        return "unknown";
    }
}

void shell_printf(const char *fmt, ...)
{
    va_list args;
    int     written;

    TEST_ASSERT(fmt != NULL);
    TEST_ASSERT(g_output_len < sizeof(g_output));

    va_start(args, fmt);
    written = vsnprintf(&g_output[g_output_len], sizeof(g_output) - g_output_len, fmt, args);
    va_end(args);

    TEST_ASSERT(written >= 0);
    TEST_ASSERT((size_t)written < (sizeof(g_output) - g_output_len));
    g_output_len += (size_t)written;
}

int foc_param_save_motor(const struct motor_dev *motor)
{
    (void)motor;
    g_save_motor_calls++;
    return PARAM_OK;
}

int foc_param_load_motor(struct motor_dev *motor)
{
    (void)motor;
    g_load_motor_calls++;
    return PARAM_OK;
}

int foc_param_save_enc(const struct motor_dev *motor)
{
    (void)motor;
    g_save_enc_calls++;
    return PARAM_OK;
}

int foc_param_load_enc(struct motor_dev *motor)
{
    (void)motor;
    g_load_enc_calls++;
    return PARAM_OK;
}

int foc_param_save_sys(const struct motor_dev *motor)
{
    (void)motor;
    g_save_sys_calls++;
    return PARAM_OK;
}

int foc_param_load_sys(struct motor_dev *motor)
{
    (void)motor;
    g_load_sys_calls++;
    return PARAM_OK;
}

void foc_param_check_all(struct param_status_all *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->motor.status = PARAM_EMPTY;
    out->enc.status   = PARAM_EMPTY;
    out->sys.status   = PARAM_EMPTY;
}

void foc_param_erase_all(void)
{
    g_erase_all_calls++;
}

void motor_restore_after_param_load(struct motor_dev *motor)
{
    (void)motor;
    g_restore_apply_calls++;
}

const char *foc_param_status_str(int st)
{
    switch (st) {
    case PARAM_OK:
        return "OK";
    case PARAM_EMPTY:
        return "EMPTY";
    case PARAM_ERR_FLASH:
        return "FLASH";
    case PARAM_ERR_VERSION:
        return "VERSION";
    case PARAM_ERR_CRC:
        return "CRC";
    default:
        return "ERR";
    }
}

static void test_store_save_sys_is_deferred_in_idle(void)
{
    char *argv[] = {"store", "save", "sys"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;

    TEST_ASSERT(app_shell_store_command(3, argv) == SHELL_OK);
    TEST_ASSERT(_motor_ins.calib_state.param_save_req == PARAM_SAVE_SYS);
    TEST_ASSERT(g_save_sys_calls == 0);
    TEST_ASSERT(g_save_motor_calls == 0);
    TEST_ASSERT(g_save_enc_calls == 0);
    TEST_ASSERT(g_restore_apply_calls == 0);
}

static void test_store_save_motor_is_rejected_while_running(void)
{
    char *argv[] = {"store", "save", "motor"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;

    TEST_ASSERT(app_shell_store_command(3, argv) == SHELL_OK);
    TEST_ASSERT(_motor_ins.calib_state.param_save_req == PARAM_SAVE_NONE);
    TEST_ASSERT(g_save_motor_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=save result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "axis=running") != NULL);
    TEST_ASSERT(g_restore_apply_calls == 0);
}

static void test_store_save_all_sets_combined_deferred_mask(void)
{
    char *argv[] = {"store", "save", "all"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;

    TEST_ASSERT(app_shell_store_command(3, argv) == SHELL_OK);
    TEST_ASSERT(_motor_ins.calib_state.param_save_req == PARAM_SAVE_ALL);
    TEST_ASSERT(g_save_motor_calls == 0);
    TEST_ASSERT(g_save_enc_calls == 0);
    TEST_ASSERT(g_save_sys_calls == 0);
    TEST_ASSERT(strstr(g_output, "mode=deferred") != NULL);
    TEST_ASSERT(g_restore_apply_calls == 0);
}

static void test_store_load_enc_is_allowed_while_running(void)
{
    char *argv[] = {"store", "load", "enc"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;

    TEST_ASSERT(app_shell_store_command(3, argv) == SHELL_OK);
    TEST_ASSERT(g_load_enc_calls == 1);
    TEST_ASSERT(g_load_motor_calls == 0);
    TEST_ASSERT(g_load_sys_calls == 0);
    TEST_ASSERT(g_restore_apply_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=load result=ok target=enc") != NULL);
}

static void test_store_load_sys_applies_runtime_restore_path(void)
{
    char *argv[] = {"store", "load", "sys"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;

    TEST_ASSERT(app_shell_store_command(3, argv) == SHELL_OK);
    TEST_ASSERT(g_load_sys_calls == 1);
    TEST_ASSERT(g_load_motor_calls == 0);
    TEST_ASSERT(g_load_enc_calls == 0);
    TEST_ASSERT(g_restore_apply_calls == 1);
}

int main(void)
{
    test_store_save_sys_is_deferred_in_idle();
    test_store_save_motor_is_rejected_while_running();
    test_store_save_all_sets_combined_deferred_mask();
    test_store_load_enc_is_allowed_while_running();
    test_store_load_sys_applies_runtime_restore_path();

    puts("[test_app_shell_store_contract] passed");
    return 0;
}
