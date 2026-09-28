/**
 * @file motor_drv.h.c
 * @brief  FOC Control Utilities & Constants - Driver
 * @version v1.0.0
 * @date 2025.12.23
 * 
 * Copyright (c) 2025 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "motor_drv.h"

#include "ntc_sensor.h"

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
static uint8_t  _bench_pwm_map       = MOTOR_BENCH_MAP_DEFAULT;
static uint8_t  _bench_adc_map       = MOTOR_BENCH_MAP_DEFAULT;
static uint16_t _bench_adc_trigger   = MOTOR_BENCH_ADC_TRIGGER_DEFAULT;
#endif

/* private functions ---------------------------------------------------------------------------- */

/**
 * @brief  Initialize all FOC PID controllers (id/iq/vel/pos)
 * 
 * @param  motor  Pointer to motor instance structure
 */
void foc_pid_init(struct motor_dev *motor)
{
    /* --- 1. Current Loop (Inner Loop - Fast) --- */
    struct pid_config curr_cfg = {
        .mode = PID_POSITION,
        .kp = DEFAULT_CURR_PID_KP,
        .ki = DEFAULT_CURR_PID_KI,
        .kd = DEFAULT_CURR_PID_KD,
        .max_output = DEFAULT_CURR_MAX_OUT,
        .integral_limit = DEFAULT_CURR_MAX_OUT,
        .dead_band = 0.0f,
        .output_lpf_hz = 0.0f,
        .deriv_lpf_hz = 0.0f,
        .improve = PID_INTEGRAL_LIMIT,
    };
    pid_init(&motor->ctrl_config.id_pid, &curr_cfg);
    pid_init(&motor->ctrl_config.iq_pid, &curr_cfg);

    /* --- 2. Velocity Loop (Middle Loop) --- */
    struct pid_config vel_cfg = {
        .mode = PID_POSITION,
        .kp = DEFAULT_VEL_PID_KP,
        .ki = DEFAULT_VEL_PID_KI,
        .kd = DEFAULT_VEL_PID_KD,
        .max_output = DEFAULT_VEL_MAX_OUT,
        .integral_limit = DEFAULT_VEL_MAX_OUT,
        .dead_band = 0.0f,
        .output_lpf_hz = 0.0f,
        .deriv_lpf_hz = 0.0f,
        .improve = PID_INTEGRAL_LIMIT,
    };
    pid_init(&motor->ctrl_config.vel_pid, &vel_cfg);

    /* --- 3. Position Loop (Outer Loop - Slow) --- */
    struct pid_config pos_cfg = {
        .mode = PID_POSITION,
        .kp = DEFAULT_POS_PID_KP,
        .ki = DEFAULT_POS_PID_KI,
        .kd = DEFAULT_POS_PID_KD,
        .max_output = DEFAULT_POS_MAX_OUT,
        .integral_limit = DEFAULT_POS_MAX_OUT,
        .dead_band = 0.0f,
        .output_lpf_hz = 0.0f,
        .deriv_lpf_hz = 0.0f,
        .improve = PID_INTEGRAL_LIMIT,
    };
    pid_init(&motor->ctrl_config.pos_pid, &pos_cfg);

    /* --- 4. Voltage-mode Velocity Loop (diagnostic fallback) --- */
    struct pid_config vvel_cfg = {
        .mode = PID_POSITION,
        .kp = DEFAULT_VVEL_PID_KP,
        .ki = DEFAULT_VVEL_PID_KI,
        .kd = DEFAULT_VVEL_PID_KD,
        .max_output = DEFAULT_VVEL_MAX_OUT,
        .integral_limit = DEFAULT_VVEL_MAX_OUT,
        .dead_band = 0.0f,
        .output_lpf_hz = 0.0f,
        .deriv_lpf_hz = 0.0f,
        .improve = PID_INTEGRAL_LIMIT,
    };
    pid_init(&motor->ctrl_config.vvel_pid, &vvel_cfg);

    motor->ctrl_config.id_pid.dt = CURR_LOOP_TS;
    motor->ctrl_config.iq_pid.dt = CURR_LOOP_TS;
    motor->ctrl_config.vel_pid.dt = VEL_LOOP_TS;
    motor->ctrl_config.pos_pid.dt = POS_LOOP_TS;
    motor->ctrl_config.vvel_pid.dt = VEL_LOOP_TS;
}

static float _foc_lpf_cutoff_hz(const struct motor_dev *motor)
{
    float curr_bandwidth_hz = DEFAULT_CURR_BW_HZ;

    if ((motor != NULL) && isfinite(motor->ctrl_config.curr_bandwidth) &&
        (motor->ctrl_config.curr_bandwidth > 0.0f)) {
        curr_bandwidth_hz = motor->ctrl_config.curr_bandwidth;
    }

    return curr_bandwidth_hz * (DEFAULT_CUTOFF_HZ / DEFAULT_CURR_BW_HZ);
}

static float _foc_lpf_alpha(const struct motor_dev *motor)
{
    float cutoff_hz = _foc_lpf_cutoff_hz(motor);
    float wc        = FOC_HZ_TO_RADS(cutoff_hz);

    return wc / (wc + DEFAULT_SAMPLING_HZ);
}

/**
 * @brief  Initialize current-loop low-pass filters
 * 
 * @param  motor  Pointer to motor instance structure
 */
void foc_lpf_init(struct motor_dev *motor)
{
    float cutoff_hz;

    if (motor == NULL) {
        return;
    }

    cutoff_hz = _foc_lpf_cutoff_hz(motor);
    lpf_init(&motor->ctrl_state.iq_filter, cutoff_hz, DEFAULT_SAMPLING_HZ);
    lpf_init(&motor->ctrl_state.id_filter, cutoff_hz, DEFAULT_SAMPLING_HZ);
    // lpf_init(&motor->ctrl_state.vq_filter, DEFAULT_CUTOFF_HZ, DEFAULT_SAMPLING_HZ);
    // lpf_init(&motor->ctrl_state.vd_filter, DEFAULT_CUTOFF_HZ, DEFAULT_SAMPLING_HZ);
    // lpf_init(&motor->ctrl_state.v_bus_filter, DEFAULT_CUTOFF_HZ, DEFAULT_SAMPLING_HZ);
    // lpf_init(&motor->ctrl_state.vel_filter, DEFAULT_CUTOFF_HZ, DEFAULT_SAMPLING_HZ);
    // lpf_init(&motor->ctrl_state.pos_filter, DEFAULT_CUTOFF_HZ, DEFAULT_SAMPLING_HZ);
}

/**
 * @brief  Refresh current-loop low-pass coefficients without clearing output
 *
 * @param  motor  Pointer to motor instance structure
 */
void foc_lpf_refresh(struct motor_dev *motor)
{
    float alpha;

    if (motor == NULL) {
        return;
    }

    alpha = _foc_lpf_alpha(motor);
    motor->ctrl_state.iq_filter.alpha = alpha;
    motor->ctrl_state.id_filter.alpha = alpha;
}

/**
 * @brief  Enable PWM Outputs
 */
void pwm_out_enable(void)
{
    tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_1, 0);
    tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_2, 0);
    tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_3, 0);

    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_1, TRUE);
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_1C, TRUE);
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_2, TRUE);
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_2C, TRUE);
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_3, TRUE);
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_3C, TRUE);
}

/**
 * @brief  Disable PWM Outputs
 */
void pwm_out_disable(void)
{
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_1, FALSE);
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_1C, FALSE);
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_2, FALSE);
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_2C, FALSE);
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_3, FALSE);
    tmr_channel_enable(TMR1, TMR_SELECT_CHANNEL_3C, FALSE);
}

/**
 * @brief  Set PWM duty cycles
 * 
 * @param  motor  Pointer to motor instance structure
 */
void pwm_out_set_duty(struct motor_dev *motor)
{
#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
    uint32_t duty_a = (uint32_t)(motor->foc_calc.dtc_a * TMR1->pr);
    uint32_t duty_b = (uint32_t)(motor->foc_calc.dtc_b * TMR1->pr);
    uint32_t duty_c = (uint32_t)(motor->foc_calc.dtc_c * TMR1->pr);

    switch (motor_bench_pwm_channel_for_map(_bench_pwm_map, MOTOR_BENCH_PHASE_A)) {
    case 1U:
        tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_1, duty_a);
        break;
    case 2U:
        tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_2, duty_a);
        break;
    case 3U:
        tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_3, duty_a);
        break;
    default:
        break;
    }
    switch (motor_bench_pwm_channel_for_map(_bench_pwm_map, MOTOR_BENCH_PHASE_B)) {
    case 1U:
        tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_1, duty_b);
        break;
    case 2U:
        tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_2, duty_b);
        break;
    case 3U:
        tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_3, duty_b);
        break;
    default:
        break;
    }
    switch (motor_bench_pwm_channel_for_map(_bench_pwm_map, MOTOR_BENCH_PHASE_C)) {
    case 1U:
        tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_1, duty_c);
        break;
    case 2U:
        tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_2, duty_c);
        break;
    case 3U:
        tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_3, duty_c);
        break;
    default:
        break;
    }
    return;
#else
    switch (motor->hw_config.phase_order) {
    case PHASE_ORDER_ABC: {
        tmr_channel_value_set(
            TMR1,
            TMR_SELECT_CHANNEL_3,
            (uint32_t)(motor->foc_calc.dtc_a * TMR1->pr)
        );
        tmr_channel_value_set(
            TMR1,
            TMR_SELECT_CHANNEL_2,
            (uint32_t)(motor->foc_calc.dtc_b * TMR1->pr)
        );
        tmr_channel_value_set(
            TMR1,
            TMR_SELECT_CHANNEL_1,
            (uint32_t)(motor->foc_calc.dtc_c * TMR1->pr)
        );
    } break;

    case PHASE_ORDER_ACB: {
        tmr_channel_value_set(
            TMR1,
            TMR_SELECT_CHANNEL_3,
            (uint32_t)(motor->foc_calc.dtc_a * TMR1->pr)
        );
        tmr_channel_value_set(
            TMR1,
            TMR_SELECT_CHANNEL_2,
            (uint32_t)(motor->foc_calc.dtc_c * TMR1->pr)
        );
        tmr_channel_value_set(
            TMR1,
            TMR_SELECT_CHANNEL_1,
            (uint32_t)(motor->foc_calc.dtc_b * TMR1->pr)
        );
    } break;
    }
#endif
}

/* public functions adc ------------------------------------------------------------------------- */

/**
 * @brief  Read ADC channel blocking
 * 
 * @param  channel  ADC channel select type
 * 
 * @return uint16_t ADC conversion data
 */
uint16_t adc_read_channel_blocking(adc_channel_select_type channel)
{
    adc_flag_clear(ADC1, ADC_CCE_FLAG);
    adc_ordinary_channel_set(ADC1, channel, 1, ADC_SAMPLETIME_239_5);
    adc_ordinary_software_trigger_enable(ADC1, TRUE);

    uint32_t timeout = 0;
    while ((adc_flag_get(ADC1, ADC_CCE_FLAG) == RESET) && timeout < 0xFFFF) {
        timeout++;
    }

    return adc_ordinary_conversion_data_get(ADC1);
}

/**
 * @brief  Read VBUS and NTC voltages
 * 
 * @param  motor  Pointer to motor instance structure
 */
/**
 * @brief  Read ordinary ADC channels (VBAT) using DMA.
 *
 * This avoids blocking the caller while ADC completes conversion.
 * Assumes ADC1 ordinary sequence is configured for one channel and
 * DMA1_CHANNEL3 is mapped to ADC1 by the generated initialization code.
 *
 * @param  motor  Pointer to motor instance structure
 */
void adc_read_ordinary_dma(struct motor_dev *motor)
{
    (void)(motor);
    // /* Number of ordinary conversions configured in wk_adc1_init(). */
    // const uint16_t channel_count = 1U;

    // /* Static DMA buffer aligned for halfword transfers. */
    // static uint16_t dma_buffer[1];

    // /* Clear DMA flags before transfer. */
    // dma_flag_clear(DMA1_FDT3_FLAG);
    // dma_flag_clear(DMA1_DTERR3_FLAG);

    // /* Configure DMA source (ADC data register) and destination (local buffer). */
    // DMA1_CHANNEL3->paddr = (uint32_t)&ADC1->odt;
    // DMA1_CHANNEL3->maddr = (uint32_t)dma_buffer;
    // DMA1_CHANNEL3->dtcnt = channel_count;

    // /* Enable DMA channel and trigger ADC conversion. */
    // dma_channel_enable(DMA1_CHANNEL3, TRUE);
    // adc_ordinary_software_trigger_enable(ADC1, TRUE);

    // /* Wait for transfer complete with timeout. */
    // uint32_t timeout = 0U;
    // while ((dma_flag_get(DMA1_FDT3_FLAG) == RESET) && (timeout < 0xFFFFU)) {
    //     timeout++;
    // }

    // /* Disable DMA channel after transfer. */
    // dma_channel_enable(DMA1_CHANNEL3, FALSE);

    // /* Convert VBAT raw sample to voltage. */
    // motor->meas_data.v_bus_raw = (float)dma_buffer[0];
    // motor->meas_data.v_bus = FAC_VBUS(motor->meas_data.v_bus_raw);
}

void adc_read_ordinary(struct motor_dev *motor)
{
    adc_read_ordinary_dma(motor);
}

/**
 * @brief  Read phase B and C currents from ADC injected group, then
 *         reconstruct phase A current via Kirchhoff's current law
 *         (ia + ib + ic = 0  =>  ia = -(ib + ic))
 *
 * @param  motor  Pointer to motor instance structure
 */
#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
static uint16_t _adc_read_phase_rank(uint8_t rank)
{
    switch (rank) {
    case 2U:
        return adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_2);
    case 3U:
        return adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_3);
    case 4U:
        return adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_4);
    default:
        return 0U;
    }
}
#endif

void adc_read_phases(struct motor_dev *motor)
{
    motor->meas_data.v_bus_raw = adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_1);

#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
    motor->meas_data.ia_raw = _adc_read_phase_rank(
        motor_bench_adc_rank_for_map(_bench_adc_map, MOTOR_BENCH_PHASE_A));
    motor->meas_data.ib_raw = _adc_read_phase_rank(
        motor_bench_adc_rank_for_map(_bench_adc_map, MOTOR_BENCH_PHASE_B));
    motor->meas_data.ic_raw = _adc_read_phase_rank(
        motor_bench_adc_rank_for_map(_bench_adc_map, MOTOR_BENCH_PHASE_C));
#else
    /* Rank/phase pairing fixed 2026-08-30 from bench evidence (T1 trigger
     * scan, 2 seeds x 12 trigger points, SnapTest dp vectors): the current of
     * the CH3-driven phase appears on ADC rank2, the CH2-driven phase on
     * rank3 and the CH1-driven phase on rank4. The rank follows the PWM
     * channel, so the ACB branch swaps the B/C labels of ranks 3/4 exactly
     * like pwm_out_set_duty() swaps the CH2/CH1 duties. */
    switch (motor->hw_config.phase_order) {
    case PHASE_ORDER_ABC: {
        motor->meas_data.ia_raw = adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_2);
        motor->meas_data.ib_raw = adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_3);
        motor->meas_data.ic_raw = adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_4);
    } break;

    case PHASE_ORDER_ACB: {
        motor->meas_data.ia_raw = adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_2);
        motor->meas_data.ic_raw = adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_3);
        motor->meas_data.ib_raw = adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_4);
    } break;
    }
#endif

    motor->meas_data.v_bus = FAC_VBUS(motor->meas_data.v_bus_raw);
    /* SOx polarity fixed 2026-08-30 from the same bench evidence: with the
     * corrected pairing the raw counts move POSITIVE for positive
     * motor-convention phase current (dp1+ must show dominant POSITIVE ib,
     * which the former global negation violated), so the samples are used
     * without negation. */
    motor->meas_data.c = FAC_CURR((motor->meas_data.ic_raw - motor->meas_data.ic_offset));
    motor->meas_data.b = FAC_CURR((motor->meas_data.ib_raw - motor->meas_data.ib_offset));
    motor->meas_data.a = FAC_CURR((motor->meas_data.ia_raw - motor->meas_data.ia_offset));
    motor->meas_data.injected_sample_seq++;
}

#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
bool motor_drv_diag_set_pwm_map(uint8_t map_id)
{
    if (map_id >= MOTOR_BENCH_MAP_COUNT) {
        return false;
    }

    _bench_pwm_map = map_id;
    return true;
}

bool motor_drv_diag_set_adc_map(uint8_t map_id)
{
    if (map_id >= MOTOR_BENCH_MAP_COUNT) {
        return false;
    }

    _bench_adc_map = map_id;
    return true;
}

void motor_drv_diag_reset_maps(void)
{
    _bench_pwm_map     = MOTOR_BENCH_MAP_DEFAULT;
    _bench_adc_map     = MOTOR_BENCH_MAP_DEFAULT;
    _bench_adc_trigger = MOTOR_BENCH_ADC_TRIGGER_DEFAULT;
    tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_4, _bench_adc_trigger);
}

uint8_t motor_drv_diag_get_pwm_map(void)
{
    return _bench_pwm_map;
}

uint8_t motor_drv_diag_get_adc_map(void)
{
    return _bench_adc_map;
}

bool motor_drv_diag_set_adc_trigger(uint16_t compare)
{
    if (!motor_bench_adc_trigger_is_valid(compare)) {
        return false;
    }

    _bench_adc_trigger = compare;
    tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_4, compare);
    return true;
}

uint16_t motor_drv_diag_get_adc_trigger(void)
{
    return _bench_adc_trigger;
}
#endif

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
