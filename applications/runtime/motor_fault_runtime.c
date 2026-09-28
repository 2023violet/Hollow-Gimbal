/**
 * @file    motor_fault_runtime.c
 * @brief   FalconFoc fault-policy runtime owner
 * @version v2.0.0
 * @date    2026.08.29
 *
 * The fast path is deliberately small and allocation-free because it runs in
 * the ADC interrupt immediately before the FOC calculation. The task path owns
 * persistence rules that need a millisecond cadence.
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "motor_fault_runtime.h"
#include "motor_drv.h"
#include "config/tm_core_config.h"

#include <math.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Private State -------------------------------------------------------------------------------- */

/* The motor task calls foc_fault_check() once per millisecond. */
static uint32_t                 _warning_elapsed_ms = 0U;
static uint32_t                 _stall_elapsed_ms   = 0U;
static struct motor_fault_context _fault_context;
static bool                     _fault_context_valid = false;
static uint32_t                 _fault_sequence = 0U;
static struct motor_hw_snapshot _hw_snapshot;
static bool                     _hw_snapshot_valid = false;
#if (TM_BENCH_DIAG != 0U)
static struct motor_bench_sample _bench_samples[MOTOR_BENCH_TRACE_DEPTH];
static uint32_t                 _bench_sample_head = 0U;
static uint32_t                 _bench_sample_count = 0U;
static uint32_t                 _bench_sample_sequence = 0U;
_Static_assert(MOTOR_BENCH_TRACE_DEPTH == 64U,
               "motor_bench_sample_ring_depth_must_remain_64");
_Static_assert(sizeof(_bench_samples) <= (5U * 1024U),
               "motor_bench_sample_ring_storage_must_not_exceed_5_kib");
#endif
static struct motor_bench_trace _bench_trace;

/* Private Helpers ------------------------------------------------------------------------------ */

static bool _drive_active(const struct motor_dev *motor)
{
    return (motor != NULL) &&
           ((motor->axis_state == AXIS_STATE_RUNNING) ||
            (motor->axis_state == AXIS_STATE_CALIB));
}

static float _absf(float value)
{
    return fabsf(value);
}

static bool _float_finite_fast(float value)
{
    union {
        float    value;
        uint32_t bits;
    } raw = {0};

    raw.value = value;
    return (raw.bits & 0x7F800000U) != 0x7F800000U;
}

static float _max3(float a, float b, float c)
{
    float max_value = (a > b) ? a : b;
    return (max_value > c) ? max_value : c;
}

static float _fast_current_limit(const struct motor_dev *motor)
{
    float limit = FOC_FAST_CURRENT_TRIP_A;

    if ((motor != NULL) && _float_finite_fast(motor->prot_config.i_peak) &&
        (motor->prot_config.i_peak > 0.0f) &&
        (motor->prot_config.i_peak < limit)) {
        limit = motor->prot_config.i_peak;
    }

    return limit;
}

static bool _protection_config_valid(const struct motor_dev *motor)
{
    if (motor == NULL) {
        return false;
    }

    return _float_finite_fast(motor->prot_config.v_bus_ov_thr) &&
           _float_finite_fast(motor->prot_config.v_bus_uv_thr) &&
           _float_finite_fast(motor->prot_config.vel_over_thr) &&
           _float_finite_fast(motor->prot_config.motor_temp_thr) &&
           _float_finite_fast(motor->prot_config.driver_temp_thr) &&
           (motor->prot_config.v_bus_uv_thr > 0.0f) &&
           (motor->prot_config.v_bus_ov_thr > motor->prot_config.v_bus_uv_thr) &&
           (motor->prot_config.vel_over_thr > 0.0f) &&
           (motor->prot_config.motor_temp_thr > 0.0f) &&
           (motor->prot_config.driver_temp_thr > 0.0f);
}

static bool _measurements_finite(const struct motor_dev *motor)
{
    return _float_finite_fast(motor->meas_data.a) && _float_finite_fast(motor->meas_data.b) &&
           _float_finite_fast(motor->meas_data.c) && _float_finite_fast(motor->meas_data.v_bus) &&
           _float_finite_fast(motor->meas_data.out_vel_rads) &&
           _float_finite_fast(motor->meas_data.motor_temp) &&
           _float_finite_fast(motor->meas_data.driver_temp) &&
           _float_finite_fast(motor->ctrl_state.id_set) &&
           _float_finite_fast(motor->ctrl_state.iq_set) &&
           _float_finite_fast(motor->ctrl_state.vd_set) &&
           _float_finite_fast(motor->ctrl_state.vq_set) &&
           _float_finite_fast(motor->ctrl_state.we_volt_ramp) &&
           _float_finite_fast(motor->ctrl_state.we_volt_now) &&
           _float_finite_fast(motor->ctrl_state.if_iq_target);
}

static bool _foc_current_finite(const struct motor_dev *motor)
{
    return _float_finite_fast(motor->foc_calc.i_d) && _float_finite_fast(motor->foc_calc.i_q);
}

#if (TM_BENCH_DIAG != 0U)
static uint16_t _round_to_u16(float value)
{
    if (!_float_finite_fast(value) || (value <= 0.0f)) {
        return 0U;
    }
    if (value >= 65535.0f) {
        return UINT16_MAX;
    }
    return (uint16_t)(value + 0.5f);
}

static float _theta_elec_for_trace(const struct motor_dev *motor)
{
    if ((motor != NULL) && _float_finite_fast(motor->enc_pri.state.angle_elec_rad)) {
        return motor->enc_pri.state.angle_elec_rad;
    }

    return (motor != NULL) ? motor->meas_data.theta_elec : 0.0f;
}
#endif

static void _copy_hw_snapshot(struct motor_hw_snapshot *out)
{
    if (out == NULL) {
        return;
    }

    if (!_hw_snapshot_valid) {
        memset(out, 0, sizeof(*out));
        return;
    }

    memcpy(out, &_hw_snapshot, sizeof(*out));
}

static void _copy_hw_snapshot_to_fault_context(struct motor_fault_context *context)
{
    struct motor_hw_snapshot hw;

    if (context == NULL) {
        return;
    }

    _copy_hw_snapshot(&hw);
    context->tmr1_cval      = hw.tmr1_cval;
    context->tmr1_pr        = hw.tmr1_pr;
    context->tmr1_ccr1      = hw.tmr1_ccr1;
    context->tmr1_ccr2      = hw.tmr1_ccr2;
    context->tmr1_ccr3      = hw.tmr1_ccr3;
    context->tmr1_ccr4      = hw.tmr1_ccr4;
    context->tmr1_cctrl     = hw.tmr1_cctrl;
    context->tmr1_brk       = hw.tmr1_brk;
    context->tmr1_ists      = hw.tmr1_ists;
    context->adc_sts        = hw.adc_sts;
    context->tmr1_count_dir = hw.tmr1_count_dir;
}

#if (TM_BENCH_DIAG != 0U)
static void _bench_flag_conversion(uint8_t *flags)
{
    if (flags != NULL) {
        *flags |= MOTOR_BENCH_SAMPLE_FLAG_CONVERSION;
    }
}

static uint16_t _bench_u32_to_u16(uint32_t value, uint8_t *flags)
{
    if (value > UINT16_MAX) {
        _bench_flag_conversion(flags);
        return UINT16_MAX;
    }

    return (uint16_t)value;
}

static uint16_t _bench_float_to_u16(float value, float scale, uint8_t *flags)
{
    float scaled;

    if (!_float_finite_fast(value) || !_float_finite_fast(scale) || (scale <= 0.0f)) {
        _bench_flag_conversion(flags);
        return 0U;
    }

    scaled = value * scale;
    if (!_float_finite_fast(scaled) || (scaled > (float)UINT16_MAX)) {
        _bench_flag_conversion(flags);
        return UINT16_MAX;
    }
    if (scaled <= 0.0f) {
        if (scaled < 0.0f) {
            _bench_flag_conversion(flags);
        }
        return 0U;
    }

    return (uint16_t)(scaled + 0.5f);
}

static int16_t _bench_float_to_i16(float value, float scale, uint8_t *flags)
{
    float scaled;

    if (!_float_finite_fast(value) || !_float_finite_fast(scale) || (scale <= 0.0f)) {
        _bench_flag_conversion(flags);
        return 0;
    }

    scaled = value * scale;
    if (!_float_finite_fast(scaled) || (scaled > (float)INT16_MAX)) {
        _bench_flag_conversion(flags);
        return INT16_MAX;
    }
    if (scaled < (float)INT16_MIN) {
        _bench_flag_conversion(flags);
        return INT16_MIN;
    }

    return (scaled >= 0.0f) ? (int16_t)(scaled + 0.5f) : (int16_t)(scaled - 0.5f);
}

static uint16_t _bench_duty_to_permille(float duty, uint8_t *flags)
{
    if (!_float_finite_fast(duty)) {
        _bench_flag_conversion(flags);
        return 0U;
    }
    if (duty <= 0.0f) {
        if (duty < 0.0f) {
            _bench_flag_conversion(flags);
        }
        return 0U;
    }
    if (duty >= 1.0f) {
        if (duty > 1.0f) {
            _bench_flag_conversion(flags);
        }
        return 1000U;
    }

    return (uint16_t)(duty * 1000.0f + 0.5f);
}

static float _bench_abs(float value)
{
    return _absf(value);
}

static void _bench_update_raw_range(uint16_t value, uint16_t *min_value, uint16_t *max_value)
{
    if (value < *min_value) {
        *min_value = value;
    }
    if (value > *max_value) {
        *max_value = value;
    }
}

static void _bench_update_float_range(float value, float *min_value, float *max_value)
{
    if (value < *min_value) {
        *min_value = value;
    }
    if (value > *max_value) {
        *max_value = value;
    }
}

static void _copy_hw_snapshot_to_sample(struct motor_bench_sample *sample)
{
    struct motor_hw_snapshot hw;

    if (sample == NULL) {
        return;
    }

    _copy_hw_snapshot(&hw);
    sample->tmr1_cval      = _bench_u32_to_u16(hw.tmr1_cval, &sample->flags);
    sample->tmr1_pr        = _bench_u32_to_u16(hw.tmr1_pr, &sample->flags);
    sample->tmr1_ccr1      = _bench_u32_to_u16(hw.tmr1_ccr1, &sample->flags);
    sample->tmr1_ccr2      = _bench_u32_to_u16(hw.tmr1_ccr2, &sample->flags);
    sample->tmr1_ccr3      = _bench_u32_to_u16(hw.tmr1_ccr3, &sample->flags);
    sample->tmr1_ccr4      = _bench_u32_to_u16(hw.tmr1_ccr4, &sample->flags);
    sample->tmr1_cctrl     = hw.tmr1_cctrl;
    sample->tmr1_brk       = hw.tmr1_brk;
    sample->tmr1_ists      = hw.tmr1_ists;
    sample->adc_ists       = hw.adc_sts;
    sample->tmr1_count_dir = hw.tmr1_count_dir;
}

static void _bench_update_aggregate(const struct motor_dev *motor)
{
    uint16_t ia_raw;
    uint16_t ib_raw;
    uint16_t ic_raw;

    ia_raw = _round_to_u16(motor->meas_data.ia_raw);
    ib_raw = _round_to_u16(motor->meas_data.ib_raw);
    ic_raw = _round_to_u16(motor->meas_data.ic_raw);

    if (_bench_trace.sample_count == 0U) {
        _bench_trace.valid              = 1U;
        _bench_trace.first_sector       = motor->foc_calc.sector;
        _bench_trace.ia_raw_min         = ia_raw;
        _bench_trace.ia_raw_max         = ia_raw;
        _bench_trace.ib_raw_min         = ib_raw;
        _bench_trace.ib_raw_max         = ib_raw;
        _bench_trace.ic_raw_min         = ic_raw;
        _bench_trace.ic_raw_max         = ic_raw;
        _bench_trace.ia_peak_abs        = _bench_abs(motor->meas_data.a);
        _bench_trace.ib_peak_abs        = _bench_abs(motor->meas_data.b);
        _bench_trace.ic_peak_abs        = _bench_abs(motor->meas_data.c);
        _bench_trace.ia_first           = motor->meas_data.a;
        _bench_trace.ib_first           = motor->meas_data.b;
        _bench_trace.ic_first           = motor->meas_data.c;
        _bench_trace.v_bus_min          = motor->meas_data.v_bus;
        _bench_trace.v_bus_max          = motor->meas_data.v_bus;
        _bench_trace.theta_sim_first    = motor->ctrl_state.theta_sim;
        _bench_trace.theta_elec_first   = _theta_elec_for_trace(motor);
        _bench_trace.we_volt_now_first  = motor->ctrl_state.we_volt_now;
        _bench_trace.we_volt_ramp_first = motor->ctrl_state.we_volt_ramp;
        _bench_trace.vd_first           = motor->foc_calc.v_d;
        _bench_trace.vq_first           = motor->foc_calc.v_q;
        _bench_trace.dtc_a_min          = motor->foc_calc.dtc_a;
        _bench_trace.dtc_a_max          = motor->foc_calc.dtc_a;
        _bench_trace.dtc_b_min          = motor->foc_calc.dtc_b;
        _bench_trace.dtc_b_max          = motor->foc_calc.dtc_b;
        _bench_trace.dtc_c_min          = motor->foc_calc.dtc_c;
        _bench_trace.dtc_c_max          = motor->foc_calc.dtc_c;
    } else {
        _bench_update_raw_range(ia_raw, &_bench_trace.ia_raw_min, &_bench_trace.ia_raw_max);
        _bench_update_raw_range(ib_raw, &_bench_trace.ib_raw_min, &_bench_trace.ib_raw_max);
        _bench_update_raw_range(ic_raw, &_bench_trace.ic_raw_min, &_bench_trace.ic_raw_max);
        if (_bench_abs(motor->meas_data.a) > _bench_trace.ia_peak_abs) {
            _bench_trace.ia_peak_abs = _bench_abs(motor->meas_data.a);
        }
        if (_bench_abs(motor->meas_data.b) > _bench_trace.ib_peak_abs) {
            _bench_trace.ib_peak_abs = _bench_abs(motor->meas_data.b);
        }
        if (_bench_abs(motor->meas_data.c) > _bench_trace.ic_peak_abs) {
            _bench_trace.ic_peak_abs = _bench_abs(motor->meas_data.c);
        }
        _bench_update_float_range(motor->meas_data.v_bus,
                                  &_bench_trace.v_bus_min,
                                  &_bench_trace.v_bus_max);
        _bench_update_float_range(motor->foc_calc.dtc_a,
                                  &_bench_trace.dtc_a_min,
                                  &_bench_trace.dtc_a_max);
        _bench_update_float_range(motor->foc_calc.dtc_b,
                                  &_bench_trace.dtc_b_min,
                                  &_bench_trace.dtc_b_max);
        _bench_update_float_range(motor->foc_calc.dtc_c,
                                  &_bench_trace.dtc_c_min,
                                  &_bench_trace.dtc_c_max);
    }

    _bench_trace.sample_count++;
    _bench_trace.last_sector       = motor->foc_calc.sector;
    _bench_trace.ia_last           = motor->meas_data.a;
    _bench_trace.ib_last           = motor->meas_data.b;
    _bench_trace.ic_last           = motor->meas_data.c;
    _bench_trace.theta_sim_last    = motor->ctrl_state.theta_sim;
    _bench_trace.theta_elec_last   = _theta_elec_for_trace(motor);
    _bench_trace.we_volt_now_last  = motor->ctrl_state.we_volt_now;
    _bench_trace.we_volt_ramp_last = motor->ctrl_state.we_volt_ramp;
    _bench_trace.vd_last           = motor->foc_calc.v_d;
    _bench_trace.vq_last           = motor->foc_calc.v_q;
}

static void _bench_write_sample(const struct motor_dev *motor)
{
    struct motor_bench_sample *sample;
    uint32_t                   slot;

    if (_bench_sample_count < MOTOR_BENCH_TRACE_DEPTH) {
        slot = (_bench_sample_head + _bench_sample_count) % MOTOR_BENCH_TRACE_DEPTH;
        _bench_sample_count++;
    } else {
        slot = _bench_sample_head;
        _bench_sample_head = (_bench_sample_head + 1U) % MOTOR_BENCH_TRACE_DEPTH;
    }

    sample = &_bench_samples[slot];
    memset(sample, 0, sizeof(*sample));
    sample->sequence        = ++_bench_sample_sequence;
    sample->fault_code      = motor->fault_code;
    sample->flags           = (motor->ctrl_state.if_enable != 0U) ? MOTOR_BENCH_SAMPLE_FLAG_IF_ENABLE : 0U;
    sample->adc_vbus_raw    = _bench_float_to_u16(motor->meas_data.v_bus_raw, 1.0f, &sample->flags);
    sample->adc_ic_raw      = _bench_float_to_u16(motor->meas_data.ic_raw, 1.0f, &sample->flags);
    sample->adc_ib_raw      = _bench_float_to_u16(motor->meas_data.ib_raw, 1.0f, &sample->flags);
    sample->adc_ia_raw      = _bench_float_to_u16(motor->meas_data.ia_raw, 1.0f, &sample->flags);
    sample->duty_a_permille = _bench_duty_to_permille(motor->foc_calc.dtc_a, &sample->flags);
    sample->duty_b_permille = _bench_duty_to_permille(motor->foc_calc.dtc_b, &sample->flags);
    sample->duty_c_permille = _bench_duty_to_permille(motor->foc_calc.dtc_c, &sample->flags);
    sample->vbus_mV         = _bench_float_to_u16(motor->meas_data.v_bus, 1000.0f, &sample->flags);
    sample->ia_mA           = _bench_float_to_i16(motor->meas_data.a, 1000.0f, &sample->flags);
    sample->ib_mA           = _bench_float_to_i16(motor->meas_data.b, 1000.0f, &sample->flags);
    sample->ic_mA           = _bench_float_to_i16(motor->meas_data.c, 1000.0f, &sample->flags);
    sample->theta_sim_mrad  = _bench_float_to_i16(motor->ctrl_state.theta_sim, 1000.0f, &sample->flags);
    sample->theta_elec_mrad = _bench_float_to_i16(_theta_elec_for_trace(motor), 1000.0f, &sample->flags);
    sample->we_now_mrad_s   = _bench_float_to_i16(motor->ctrl_state.we_volt_now, 1000.0f, &sample->flags);
    sample->we_ramp_mrad_s  = _bench_float_to_i16(motor->ctrl_state.we_volt_ramp, 1000.0f, &sample->flags);
    sample->vd_mV           = _bench_float_to_i16(motor->foc_calc.v_d, 1000.0f, &sample->flags);
    sample->vq_mV           = _bench_float_to_i16(motor->foc_calc.v_q, 1000.0f, &sample->flags);
    sample->axis_state      = (uint8_t)motor->axis_state;
    sample->sector          = motor->foc_calc.sector;
    _copy_hw_snapshot_to_sample(sample);
    _bench_trace.sequence = sample->sequence;
}
#endif

static void _capture_fault_context(const struct motor_dev *motor, uint32_t fault)
{
    if ((motor == NULL) || _fault_context_valid) {
        return;
    }

    _fault_sequence++;
    memset(&_fault_context, 0, sizeof(_fault_context));
    _fault_context.sequence   = _fault_sequence;
    _fault_context.fault_code = fault;
    _copy_hw_snapshot_to_fault_context(&_fault_context);
    _fault_context.axis_state  = (uint8_t)motor->axis_state;
    _fault_context.ctrl_mode   = (uint8_t)motor->ctrl_mode;
    _fault_context.sector      = motor->foc_calc.sector;
    _fault_context.ia_raw      = motor->meas_data.ia_raw;
    _fault_context.ib_raw      = motor->meas_data.ib_raw;
    _fault_context.ic_raw      = motor->meas_data.ic_raw;
    _fault_context.ia          = motor->meas_data.a;
    _fault_context.ib          = motor->meas_data.b;
    _fault_context.ic          = motor->meas_data.c;
    _fault_context.v_bus_raw   = motor->meas_data.v_bus_raw;
    _fault_context.v_bus       = motor->meas_data.v_bus;
    _fault_context.id          = motor->foc_calc.i_d;
    _fault_context.iq          = motor->foc_calc.i_q;
    _fault_context.theta_sim   = motor->ctrl_state.theta_sim;
    _fault_context.theta_elec  = motor->meas_data.theta_elec;
    _fault_context.we_volt_now = motor->ctrl_state.we_volt_now;
    _fault_context.we_volt_ramp = motor->ctrl_state.we_volt_ramp;
    _fault_context.vd          = motor->foc_calc.v_d;
    _fault_context.vq          = motor->foc_calc.v_q;
    _fault_context.v_alpha     = motor->foc_calc.v_alpha;
    _fault_context.v_beta      = motor->foc_calc.v_beta;
    _fault_context.dtc_a       = motor->foc_calc.dtc_a;
    _fault_context.dtc_b       = motor->foc_calc.dtc_b;
    _fault_context.dtc_c       = motor->foc_calc.dtc_c;
    _fault_context_valid       = true;
}

static void _latch_fault(struct motor_dev *motor, uint32_t fault)
{
    if ((motor == NULL) || (fault == FAULT_NONE) || (motor->fault_code != FAULT_NONE)) {
        return;
    }

    _capture_fault_context(motor, fault);
    motor->fault_code |= fault;
    pwm_out_disable();
}

static bool _drive_command_active(const struct motor_dev *motor)
{
    if ((motor->ctrl_mode == CTRL_MODE_VOLT) &&
        ((_absf(motor->ctrl_state.vd_set) > 0.001f) ||
         (_absf(motor->ctrl_state.vq_set) > 0.001f) ||
         (_absf(motor->ctrl_state.we_volt_ramp) > 0.001f) ||
         (_absf(motor->ctrl_state.we_volt_now) > 0.001f))) {
        return true;
    }

    return (_absf(motor->ctrl_state.iq_set) > 0.001f) ||
           (_absf(motor->ctrl_state.target_iq) > 0.001f) ||
           ((_absf(motor->ctrl_state.if_iq_target) > 0.001f) &&
            (motor->ctrl_state.if_enable != 0U));
}

/* Public Functions ----------------------------------------------------------------------------- */

void foc_fault_runtime_reset(void)
{
    _warning_elapsed_ms = 0U;
    _stall_elapsed_ms   = 0U;
}

bool motor_fault_context_read(struct motor_fault_context *out)
{
    if (out == NULL) {
        return false;
    }

    if (!_fault_context_valid) {
        memset(out, 0, sizeof(*out));
        return false;
    }

    memcpy(out, &_fault_context, sizeof(*out));
    return true;
}

void motor_fault_context_clear(void)
{
    memset(&_fault_context, 0, sizeof(_fault_context));
    _fault_context_valid = false;
}

bool motor_bench_trace_read(struct motor_bench_trace *out)
{
    if (out == NULL) {
        return false;
    }

    memcpy(out, &_bench_trace, sizeof(*out));
    return out->valid != 0U;
}

void motor_bench_trace_clear(void)
{
#if (TM_BENCH_DIAG != 0U)
    motor_bench_trace_sample_clear();
#else
    memset(&_bench_trace, 0, sizeof(_bench_trace));
#endif
}

void motor_fault_runtime_stage_hw_snapshot(const struct motor_hw_snapshot *snapshot)
{
    if (snapshot == NULL) {
        memset(&_hw_snapshot, 0, sizeof(_hw_snapshot));
        _hw_snapshot_valid = false;
        return;
    }

    memcpy(&_hw_snapshot, snapshot, sizeof(_hw_snapshot));
    _hw_snapshot_valid = true;
}

#if (TM_BENCH_DIAG != 0U)
void motor_bench_trace_update(const struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    _bench_update_aggregate(motor);
    _bench_write_sample(motor);
}

bool motor_bench_trace_sample_read(uint32_t index, struct motor_bench_sample *out)
{
    uint32_t slot;

    if ((out == NULL) || (index >= _bench_sample_count)) {
        return false;
    }

    slot = (_bench_sample_head + index) % MOTOR_BENCH_TRACE_DEPTH;
    memcpy(out, &_bench_samples[slot], sizeof(*out));
    return true;
}

uint32_t motor_bench_trace_sample_count(void)
{
    return _bench_sample_count;
}

void motor_bench_trace_sample_clear(void)
{
    memset(&_bench_trace, 0, sizeof(_bench_trace));
    memset(_bench_samples, 0, sizeof(_bench_samples));
    _bench_sample_head     = 0U;
    _bench_sample_count    = 0U;
    _bench_sample_sequence = 0U;
}
#endif

void foc_fault_check_fast(struct motor_dev *motor)
{
    uint32_t fault = FAULT_NONE;
    float    current_limit;
    float    measured_current;
    float    abs_velocity;

    if (!_drive_active(motor) || (motor->fault_code != FAULT_NONE)) {
        return;
    }

    if (!_measurements_finite(motor) || !_foc_current_finite(motor) ||
        !_protection_config_valid(motor)) {
        fault = FAULT_SENSOR_ERROR;
    } else {
        current_limit = _fast_current_limit(motor);
        measured_current = _max3(_absf(motor->meas_data.a),
                                 _absf(motor->meas_data.b),
                                 _absf(motor->meas_data.c));
        if (measured_current > current_limit) {
            fault = FAULT_OVER_CURR;
        }

        if (fault == FAULT_NONE) {
            if (motor->meas_data.v_bus > motor->prot_config.v_bus_ov_thr) {
                fault = FAULT_OVER_VOLT;
            } else if (motor->meas_data.v_bus < motor->prot_config.v_bus_uv_thr) {
                fault = FAULT_UNDER_VOLT;
            } else {
                abs_velocity = _absf(motor->meas_data.out_vel_rads);
                if (abs_velocity > motor->prot_config.vel_over_thr) {
                    fault = FAULT_OVER_VEL;
                } else if ((motor->meas_data.motor_temp > 5.0f) &&
                           (motor->meas_data.motor_temp > motor->prot_config.motor_temp_thr)) {
                    fault = FAULT_MOTOR_OVER_TEMP;
                } else if ((motor->meas_data.driver_temp > 5.0f) &&
                           (motor->meas_data.driver_temp > motor->prot_config.driver_temp_thr)) {
                    fault = FAULT_DRIVER_OVER_TEMP;
                }
            }
        }
    }

    _latch_fault(motor, fault);
}

void foc_fault_bench_timeout(struct motor_dev *motor)
{
    _latch_fault(motor, FAULT_COMM_ERROR);
}

void foc_fault_check(struct motor_dev *motor)
{
    float current_sq;
    float hard_current_sq;
    float warning_current_sq;
    float stall_current_sq;
    float abs_velocity;
    bool  warning_active;
    bool  stall_active;

    if (motor == NULL) {
        return;
    }

    if (!_drive_active(motor)) {
        foc_fault_runtime_reset();
        return;
    }

    if (motor->fault_code != FAULT_NONE) {
        return;
    }

    if (!_measurements_finite(motor) || !_foc_current_finite(motor) ||
        !_protection_config_valid(motor)) {
        _latch_fault(motor, FAULT_SENSOR_ERROR);
        return;
    }

    current_sq = motor->foc_calc.i_d * motor->foc_calc.i_d +
                 motor->foc_calc.i_q * motor->foc_calc.i_q;
    hard_current_sq    = _fast_current_limit(motor) * _fast_current_limit(motor);
    warning_current_sq = FOC_WARN_CURRENT_A * FOC_WARN_CURRENT_A;
    stall_current_sq   = FOC_STALL_CURRENT_A * FOC_STALL_CURRENT_A;

    if (current_sq > hard_current_sq) {
        _latch_fault(motor, FAULT_OVER_CURR);
        return;
    }

    if (motor->meas_data.v_bus > motor->prot_config.v_bus_ov_thr) {
        _latch_fault(motor, FAULT_OVER_VOLT);
        return;
    }
    if (motor->meas_data.v_bus < motor->prot_config.v_bus_uv_thr) {
        _latch_fault(motor, FAULT_UNDER_VOLT);
        return;
    }

    abs_velocity = _absf(motor->meas_data.out_vel_rads);
    if (abs_velocity > motor->prot_config.vel_over_thr) {
        _latch_fault(motor, FAULT_OVER_VEL);
        return;
    }

    if ((motor->meas_data.motor_temp > 5.0f) &&
        (motor->meas_data.motor_temp > motor->prot_config.motor_temp_thr)) {
        _latch_fault(motor, FAULT_MOTOR_OVER_TEMP);
        return;
    }
    if ((motor->meas_data.driver_temp > 5.0f) &&
        (motor->meas_data.driver_temp > motor->prot_config.driver_temp_thr)) {
        _latch_fault(motor, FAULT_DRIVER_OVER_TEMP);
        return;
    }

    warning_active = current_sq > warning_current_sq;
    if (warning_active) {
        if (_warning_elapsed_ms < FOC_WARNING_TRIP_MS) {
            _warning_elapsed_ms++;
        }
        if (_warning_elapsed_ms >= FOC_WARNING_TRIP_MS) {
            _latch_fault(motor, FAULT_OVER_CURR);
            return;
        }
    } else {
        _warning_elapsed_ms = 0U;
    }

    /* A voltage-open-loop test intentionally commands zero speed for fixed
     * vectors and does not close a speed loop.  Applying the closed-loop
     * stall detector here would turn a valid electrical alignment test into
     * a false over-current fault.  Absolute current and sustained-current
     * protection remain active for CTRL_MODE_VOLT. */
    stall_active = (motor->ctrl_mode != CTRL_MODE_VOLT) &&
                   _drive_command_active(motor) &&
                   (current_sq >= stall_current_sq) &&
                   (abs_velocity <= FOC_RPM_TO_RADS(FOC_STALL_SPEED_RPM));
    if (stall_active) {
        if (_stall_elapsed_ms < FOC_STALL_TRIP_MS) {
            _stall_elapsed_ms++;
        }
        if (_stall_elapsed_ms >= FOC_STALL_TRIP_MS) {
            _latch_fault(motor, FAULT_OVER_CURR);
            return;
        }
    } else {
        _stall_elapsed_ms = 0U;
    }
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
