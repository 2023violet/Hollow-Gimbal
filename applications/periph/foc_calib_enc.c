/**
 * @file    foc_calib_enc.c
 * @brief   FOC Encoder Calibration Module (Final Stable)
 * @version v1.4.0 (Fix: Calc/Flush Separation)
 * @date    2026.01.31
 *
 * Copyright (c) 2026 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

#include "foc_calib_enc.h"
#include "foc_ctrl.h"
#include "motor_drv.h"
#include "foc_param.h"
#include "vofa.h"
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Private function prototypes ------------------------------------------------------------------ */
static void  _calib_reset_ctx(struct foc_enc_calib_ctx *ctx);
static float _calib_get_norm_mech_pos(struct motor_dev *motor);

/* State Handler Prototypes */
static void _state_pp_start(struct motor_dev *motor, struct foc_enc_calib_ctx *ctx);
static void _state_pp_calc(struct motor_dev *motor, struct foc_enc_calib_ctx *ctx);
static void _state_lut_start(struct motor_dev *motor, struct foc_enc_calib_ctx *ctx);
static void _state_lut_cw(struct motor_dev *motor, struct foc_enc_calib_ctx *ctx);
static void _state_lut_ccw(struct motor_dev *motor, struct foc_enc_calib_ctx *ctx);
static void _state_lut_calc(struct motor_dev *motor, struct foc_enc_calib_ctx *ctx);
static void _state_lut_flush(struct motor_dev *motor, struct foc_enc_calib_ctx *ctx);
static void _state_lut_end(struct motor_dev *motor, struct foc_enc_calib_ctx *ctx);

/* Public functions ----------------------------------------------------------------------------- */

/**
 * @brief  Initialize the encoder calibration module
 * * @param  motor  Pointer to the motor device structure
 */
void foc_calib_enc_init(struct motor_dev *motor)
{
    if (motor == NULL)
        return;

    struct foc_enc_calib_ctx *ctx = &motor->calib_state.enc_ctx;

    /* Bind debug variables */
#if (VOFA_PROTOCOL == VOFA_PROTO_JUSTFLOAT)
    vofa_bind(0, &ctx->pos, VOFA_FLOAT);
    vofa_bind(1, &ctx->pr_set, VOFA_FLOAT);
    vofa_bind(2, &ctx->pos_err, VOFA_FLOAT);
    vofa_bind(3, &motor->enc_pri.state.enc_lut, VOFA_FLOAT);
    vofa_bind(4, &ctx->pn, VOFA_UINT8);
    vofa_bind(5, &ctx->lut[0], VOFA_FLOAT);
    vofa_bind(6, &ctx->r_off, VOFA_FLOAT);
    vofa_bind(7, &motor->hw_config.phase_order, VOFA_UINT8);
    vofa_bind(8, &motor->enc_pri.config->dir, VOFA_UINT8);
    vofa_bind(9, &motor->enc_pri.config->mech_offset, VOFA_FLOAT);
#endif

    _calib_reset_ctx(ctx);

    /* Force clear outputs to remove "Ghost NaNs" from previous runs */
    motor->enc_pri.state.calib_valid   = false;
    motor->enc_pri.config->mech_offset = 0.0f;
    motor->enc_pri.state.enc_lut       = 0.0f;
}

/**
 * @brief  Execute encoder calibration state machine
 * * @param  motor  Pointer to the motor device structure
 */
void foc_calib_enc(struct motor_dev *motor)
{
    if (motor == NULL)
        return;

    struct foc_enc_calib_ctx *ctx = &motor->calib_state.enc_ctx;

    ctx->lut_num = CALIB_LUT_SIZE;
    ctx->pos     = _calib_get_norm_mech_pos(motor);
    ctx->raw     = motor->enc_pri.state.raw_value;
    ctx->bit     = ENCODER_BITS;

    foc_ctrl_volt(motor, ctx->vd_ref, 0.0f, ctx->pe_set);

    switch (motor->calib_state.enc_state) {
    case ENC_IDLE:
        motor->calib_state.enc_state = ENC_PP_START;
        break;

    case ENC_PP_START:
        _state_pp_start(motor, ctx);
        break;

    case ENC_PP_CALC:
        _state_pp_calc(motor, ctx);
        break;

    case ENC_LUT_START:
        _state_lut_start(motor, ctx);
        break;

    case ENC_LUT_CW:
        _state_lut_cw(motor, ctx);
        break;

    case ENC_LUT_CCW:
        _state_lut_ccw(motor, ctx);
        break;

    /* Merge old states into CALC, then go to FLUSH */
    case ENC_LUT_CALC:
        _state_lut_calc(motor, ctx);
        break;

    case ENC_LUT_FLUSH:
        _state_lut_flush(motor, ctx);
        break;

    case ENC_LUT_END:
        _state_lut_end(motor, ctx);
        break;

    default:
        break;
    }
}

/* Private functions ---------------------------------------------------------------------------- */

static void _calib_reset_ctx(struct foc_enc_calib_ctx *ctx)
{
    if (ctx != NULL) {
        memset(ctx, 0, sizeof(struct foc_enc_calib_ctx));
    }
}

static float _calib_get_norm_mech_pos(struct motor_dev *motor)
{
    int32_t raw_adj;
    if (motor->enc_pri.config->dir == DIR_CW) {
        raw_adj = (int32_t)motor->enc_pri.state.raw_value;
    } else {
        raw_adj = ENCODER_CPR - (int32_t)motor->enc_pri.state.raw_value;
    }
    raw_adj &= (ENCODER_CPR - 1);

    float pos = (float)raw_adj * (FOC_2PI / (float)ENCODER_CPR);
    return foc_wrap_0_2pi(pos);
}

/* State Handlers ------------------------------------------------------------------------------- */

/* ... (Keep _state_pp_start, _state_pp_calc, _state_lut_start, _state_lut_cw, _state_lut_ccw AS IS) ... */
/* NOTE: Just copy them from the previous code block, they were correct. */
static void _state_pp_start(struct motor_dev *motor, struct foc_enc_calib_ctx *ctx)
{
    memset(ctx->lut, 0, sizeof(ctx->lut)); /* Critical Clear */
    if (++ctx->cnt_i < CALIB_ALIGN_DURATION) {
        ctx->vd_ref                = CALIB_VOLTAGE;
        ctx->pe_set                = 0.0f;
        ctx->pe_acc                = CALIB_ACCEL;
        ctx->pos_start             = ctx->pos;
        motor->enc_pri.config->dir = DIR_CCW;
    } else {
        ctx->pe_set += ctx->pe_acc;
        if (ctx->pe_set > CALIB_CHECK_ANGLE_MIN) {
            if (ctx->pe_set < CALIB_CHECK_ANGLE_MAX)
                ctx->pos_dir_end = ctx->pos;
            if (fabsf(ctx->pos_start - ctx->pos) < CALIB_CHECK_POS_TH)
                motor->calib_state.enc_state = ENC_PP_CALC;
        }
    }
}

static void _state_pp_calc(struct motor_dev *motor, struct foc_enc_calib_ctx *ctx)
{
    ctx->pn = (uint8_t)(round(ctx->pe_set / FOC_2PI));
    if (ctx->pn == 0)
        ctx->pn = 1;
    motor->enc_pri.config->pole_pairs = ctx->pn;
    float pos_dif                     = ctx->pos_dir_end - ctx->pos_start;
    pos_dif                           = foc_wrap_pm_pi(pos_dif);
    if (pos_dif < 0) {
        motor->hw_config.phase_order =
            (motor->hw_config.phase_order == PHASE_ORDER_ABC) ? PHASE_ORDER_ACB : PHASE_ORDER_ABC;
        motor->enc_pri.config->dir = DIR_CW;
    } else {
        motor->enc_pri.config->dir = DIR_CCW;
    }
    motor->calib_state.enc_state = ENC_LUT_START;
}

static void _state_lut_start(struct motor_dev *motor, struct foc_enc_calib_ctx *ctx)
{
    if (++ctx->cnt_i < CALIB_ALIGN_DURATION) {
        ctx->pe_set = 0.0f;
    } else {
        ctx->n1         = ctx->lut_num * ctx->pn;
        ctx->n2         = CALIB_SCAN_STEPS;
        ctx->pe_acc     = FOC_2PI * ctx->pn / (float)(ctx->n1 * ctx->n2);
        ctx->cnt_i      = 0;
        ctx->cnt_j      = ctx->n2;
        ctx->cnt_k      = 0; /* Reset K! */
        int32_t raw_adj = (motor->enc_pri.config->dir == DIR_CW)
                              ? (int32_t)ctx->raw
                              : (ENCODER_CPR - (int32_t)ctx->raw);
        raw_adj &= (ENCODER_CPR - 1);
        ctx->raw_start_idx           = raw_adj >> (ctx->bit - ENCODER_LUT_BITS);
        motor->calib_state.enc_state = ENC_LUT_CW;
    }
}

static void _state_lut_cw(struct motor_dev *motor, struct foc_enc_calib_ctx *ctx)
{
    if (ctx->cnt_i < (ctx->n1 * ctx->n2)) {
        if (ctx->cnt_j == ctx->n2) {
            ctx->cnt_j   = 0;
            ctx->pos_err = foc_wrap_pm_pi(ctx->pr_set - ctx->pos);
            if (ctx->cnt_k == 0)
                ctx->p_raw[0] = (float)ctx->raw;
            uint32_t step_idx = ctx->cnt_i / ctx->n2;
            uint32_t lut_idx  = (ctx->raw_start_idx + step_idx) % ctx->lut_num;
            ctx->lut[lut_idx] += ctx->pos_err;
            ctx->cnt_k++;
        }
        ctx->pe_set += ctx->pe_acc;
        ctx->pr_set = foc_wrap_0_2pi(ctx->pe_set / (float)ctx->pn);
        ctx->cnt_i++;
        ctx->cnt_j++;
    } else {
        ctx->cnt_i                   = 0;
        ctx->cnt_j                   = ctx->n2;
        motor->calib_state.enc_state = ENC_LUT_CCW;
    }
}

static void _state_lut_ccw(struct motor_dev *motor, struct foc_enc_calib_ctx *ctx)
{
    if (ctx->cnt_i < (ctx->n1 * ctx->n2)) {
        if (ctx->cnt_j == ctx->n2) {
            ctx->cnt_j   = 0;
            ctx->pos_err = foc_wrap_pm_pi(ctx->pr_set - ctx->pos);
            if (ctx->cnt_k == (2 * ctx->n1 - 1))
                ctx->p_raw[1] = (float)ctx->raw;
            uint32_t total_steps  = ctx->n1;
            uint32_t current_step = ctx->cnt_i / ctx->n2;
            uint32_t relative_idx = (total_steps - current_step) % ctx->lut_num;
            uint32_t lut_idx      = (ctx->raw_start_idx + relative_idx) % ctx->lut_num;
            ctx->lut[lut_idx] += ctx->pos_err;
            ctx->cnt_k++;
        }
        ctx->pe_set -= ctx->pe_acc;
        ctx->pr_set = foc_wrap_0_2pi(ctx->pe_set / (float)ctx->pn);
        ctx->cnt_i++;
        ctx->cnt_j++;
    } else {
        ctx->cnt_i                   = 0;
        ctx->cnt_j                   = 0;
        ctx->vd_ref                  = 0.0f;
        motor->calib_state.enc_state = ENC_LUT_CALC;
    }
}

/* -------------------------------------------------------------------------- */
/* NEW: Logic split into CALC (Run Once) and FLUSH (Run Loop) */
/* -------------------------------------------------------------------------- */

static void _state_lut_calc(struct motor_dev *motor, struct foc_enc_calib_ctx *ctx)
{
    /* NaN Protection */
    if (ctx->pn == 0)
        ctx->pn = 1;

    float divisor      = 2.0f * (float)ctx->pn;
    float sum_of_means = 0.0f;

    /* 1. Normalize Accumulator -> Average Error (Radians) */
    for (uint32_t i = 0; i < ctx->lut_num; i++) {
        ctx->lut[i] /= divisor;
        sum_of_means += ctx->lut[i];
    }

    /* 2. Calculate Mean */
    ctx->mean = sum_of_means / (float)ctx->lut_num;

    /* 3. Convert Radians to Ticks & Remove Bias */
    /* Ensure bit is valid before shifting */
    if (ctx->bit > 31)
        ctx->bit = 16;
    float ticks_per_rad = (float)(1UL << ctx->bit) / FOC_2PI;

    for (uint32_t i = 0; i < ctx->lut_num; i++) {
        float error_rad = ctx->lut[i] - ctx->mean;
        ctx->lut[i]     = error_rad * ticks_per_rad;
    }

    /* 4. Calculate Offsets */
    ctx->raw_off = (uint16_t)((ctx->p_raw[0] + ctx->p_raw[1]) * 0.5f);
    ctx->r_off   = fmodf(ctx->mean, FOC_2PI);

    /* 5. Reset index for Flush state */
    ctx->cnt_i = 0;

    /* 6. Transition to FLUSH state immediately */
    motor->calib_state.enc_state = ENC_LUT_FLUSH;
}

static void _state_lut_flush(struct motor_dev *motor, struct foc_enc_calib_ctx *ctx)
{
    /* Flush computed LUT values to motor config one by one */
    if (ctx->cnt_i < ctx->lut_num) {
        motor->enc_pri.state.enc_lut               = ctx->lut[ctx->cnt_i];
        motor->enc_pri.state.enc_table[ctx->cnt_i] = (int32_t)ctx->lut[ctx->cnt_i];
        ctx->cnt_i++;
    } else {
        motor->calib_state.enc_state = ENC_LUT_END;
    }
}

static void _state_lut_end(struct motor_dev *motor, struct foc_enc_calib_ctx *ctx)
{
    motor->enc_pri.config->mech_offset = ctx->r_off;
    motor->enc_pri.state.calib_valid   = true;

    /*
     * Do NOT call foc_param_save_enc() here.
     * This function runs inside the TIM6 interrupt context (slow FOC loop).
     * Flash erase (~20ms) with __disable_irq() would block the higher-priority
     * ADC/FOC ISR and create a real-time safety hazard.
     *
     * Instead: signal motor_task to perform the save in RTOS (task) context.
     */
    motor->calib_state.param_save_req |= PARAM_SAVE_ENC;

    // motor->axis_state            = AXIS_STATE_IDLE;
    // motor->calib_state.enc_state = ENC_IDLE;
}

#if 0
void motor_align_and_print_offset(struct motor_dev *motor)
{
    // /* 1. 强制使用电压开环模式 */
    // motor->ctrl_mode = CTRL_MODE_VOLT;

    /* 2. 强行输出电压矢量：Vd=1.5V, Vq=0, Theta=0 */
    /* 这会将转子死死吸在电角度 0 度的位置 */
    float align_voltage = 1.5f; // 如果电机没反应，可加到 2.0f-3.0f，注意不要过流

    /* 持续 2 秒，确保电机稳定下来 */
    for (int i = 0; i < 2000; i++) {
        /* 注意：这里第三个参数是 Electrical Angle，给 0.0f */
        foc_ctrl_volt(motor, align_voltage, 0.0f, 0.0f);
        osDelay(1);
    }

    /* 3. 读取此时的编码器原始值 */
    encoder_update(&motor->enc_pri); /* 确保读到最新数据 */

    /* 获取调整方向后的原始值 (raw_adjusted) */
    /* 注意：我们需要知道经方向修正后的值，但还没减去 offset 的值 */
    /* 在 encoder.c 中，cnt = raw_adjusted - off_interp; */
    /* 我们可以直接读 encoder 结构体里的 raw_value */
    uint16_t raw = motor->enc_pri.state.raw_value;

    /* 计算当前的逻辑计数值 */
    int32_t current_cnt;
    if (motor->enc_pri.config->dir == 0) { // DIR_CW
        current_cnt = raw;
    } else {                       // DIR_CCW
        current_cnt = 65536 - raw; // 假设 16bit 编码器 (65536)
    }

    /* 4. 这个 current_cnt 就是您的新零点偏移量！ */
    /* 因为此时电角度为 0，所以 MechAngle * Poles = 0 */
    /* 所以我们将当前位置设为 mech_offset */

    motor->enc_pri.config->mech_offset = (float)current_cnt / 65536.0f * 6.2831853f;

    /* 停止输出 */
    pwm_out_disable();

    for (int i = 0; i < 2000; i++) {
        osDelay(1);
    }
    while (1)
        ; // 卡住，让您有时间看打印的参数
}
#endif

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
