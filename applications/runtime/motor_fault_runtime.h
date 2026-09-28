/**
 * @file    motor_fault_runtime.h
 * @brief   FalconFoc fault-policy runtime owner
 * @version v1.0.0
 * @date    2026.05.21
 *
 * Design overview
 * ---------------
 * This header owns the product-side fault policy wrapper. The hardware-coupled
 * FOC engines only provide measurements; runtime decides when a fault is
 * latched and how the axis is forced safe.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Post-stop Diagnostic Context ----------------------------------------------------------------- */

/**
 * @brief First-fault snapshot captured by the protection path.
 *
 * The context is a single-motor, RAM-only diagnostic record.  It is captured
 * before PWM is disabled and is intended to be read after the motor has
 * stopped.  The capture path does not format, print, persist, or transmit
 * this structure.
 */
struct motor_fault_context {
    uint32_t sequence;
    uint32_t fault_code;

    uint32_t tmr1_cval;
    uint32_t tmr1_pr;
    uint32_t tmr1_ccr1;
    uint32_t tmr1_ccr2;
    uint32_t tmr1_ccr3;
    uint32_t tmr1_ccr4;
    uint32_t tmr1_cctrl;
    uint32_t tmr1_brk;
    uint32_t tmr1_ists;
    uint32_t adc_sts;

    uint8_t axis_state;
    uint8_t ctrl_mode;
    uint8_t sector;
    uint8_t tmr1_count_dir;

    float ia_raw;
    float ib_raw;
    float ic_raw;

    float ia;
    float ib;
    float ic;

    float v_bus_raw;
    float v_bus;

    float id;
    float iq;

    float theta_sim;
    float theta_elec;
    float we_volt_now;
    float we_volt_ramp;

    float vd;
    float vq;
    float v_alpha;
    float v_beta;

    float dtc_a;
    float dtc_b;
    float dtc_c;
};

struct motor_hw_snapshot {
    uint32_t tmr1_cval;
    uint32_t tmr1_pr;
    uint32_t tmr1_ccr1;
    uint32_t tmr1_ccr2;
    uint32_t tmr1_ccr3;
    uint32_t tmr1_ccr4;
    uint32_t tmr1_cctrl;
    uint32_t tmr1_brk;
    uint32_t tmr1_ists;
    uint32_t adc_sts;
    uint8_t  tmr1_count_dir;
};

#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
#define MOTOR_BENCH_TRACE_DEPTH                 (64U)
#define MOTOR_BENCH_SAMPLE_RING_DEPTH           MOTOR_BENCH_TRACE_DEPTH
#define MOTOR_BENCH_SAMPLE_FLAG_CONVERSION      (1U << 0)
#define MOTOR_BENCH_SAMPLE_FLAG_IF_ENABLE       (1U << 1)

/**
 * @brief Fixed-point RAM sample retained only by the bench diagnostic image.
 */
struct motor_bench_sample {
    uint32_t sequence;

    uint16_t tmr1_cval;
    uint16_t tmr1_pr;
    uint16_t tmr1_ccr1;
    uint16_t tmr1_ccr2;
    uint16_t tmr1_ccr3;
    uint16_t tmr1_ccr4;
    uint32_t tmr1_cctrl;
    uint32_t tmr1_brk;
    uint32_t tmr1_ists;
    union {
        uint32_t adc_ists;
        uint32_t adc_sts;
    };

    union {
        uint16_t adc_vbus_raw;
        uint16_t vbus_raw;
    };
    union {
        uint16_t adc_ic_raw;
        uint16_t ic_raw;
    };
    union {
        uint16_t adc_ib_raw;
        uint16_t ib_raw;
    };
    union {
        uint16_t adc_ia_raw;
        uint16_t ia_raw;
    };

    union {
        int16_t ia_mA;
        int16_t ia_ma;
    };
    union {
        int16_t ib_mA;
        int16_t ib_ma;
    };
    union {
        int16_t ic_mA;
        int16_t ic_ma;
    };
    int16_t theta_sim_mrad;
    int16_t theta_elec_mrad;
    int16_t we_now_mrad_s;
    int16_t we_ramp_mrad_s;
    union {
        int16_t vd_mV;
        int16_t vd_mv;
    };
    union {
        int16_t vq_mV;
        int16_t vq_mv;
    };

    union {
        uint16_t vbus_mV;
        uint16_t vbus_mv;
    };
    uint16_t duty_a_permille;
    uint16_t duty_b_permille;
    uint16_t duty_c_permille;

    uint8_t sector;
    uint8_t axis_state;
    uint8_t flags;
    uint8_t tmr1_count_dir;
    uint32_t fault_code;
};
#endif

/**
 * @brief RAM-only aggregate trace for a bench FOC run window.
 *
 * The trace is intentionally a fixed-size first/last/min/max record.  It is
 * updated from the bench FOC path and read only after the axis is stopped;
 * it never formats, persists, or transmits data from the interrupt path.
 */
struct motor_bench_trace {
    uint32_t sequence;
    uint32_t sample_count;
    uint8_t  valid;
    uint8_t  first_sector;
    uint8_t  last_sector;

    uint16_t ia_raw_min;
    uint16_t ia_raw_max;
    uint16_t ib_raw_min;
    uint16_t ib_raw_max;
    uint16_t ic_raw_min;
    uint16_t ic_raw_max;

    float ia_peak_abs;
    float ib_peak_abs;
    float ic_peak_abs;

    float ia_first;
    float ia_last;
    float ib_first;
    float ib_last;
    float ic_first;
    float ic_last;

    float v_bus_min;
    float v_bus_max;

    float theta_sim_first;
    float theta_sim_last;
    float theta_elec_first;
    float theta_elec_last;

    float we_volt_now_first;
    float we_volt_now_last;
    float we_volt_ramp_first;
    float we_volt_ramp_last;

    float vd_first;
    float vd_last;
    float vq_first;
    float vq_last;

    float dtc_a_min;
    float dtc_a_max;
    float dtc_b_min;
    float dtc_b_max;
    float dtc_c_min;
    float dtc_c_max;
};

/**
 * @brief Read the first-fault context without changing it.
 *
 * @param[out] out Caller-owned destination.
 * @retval true  A valid latched context was copied.
 * @retval false No context is latched, or `out == NULL`.
 */
bool motor_fault_context_read(struct motor_fault_context *out);

/**
 * @brief Clear the RAM-only first-fault context.
 *
 * This is a lifecycle/diagnostic operation and must not be called from the
 * ADC ISR.  It does not write Flash.
 */
void motor_fault_context_clear(void);

/**
 * @brief Read the last bench trace without changing it.
 *
 * @param[out] out Caller-owned destination.
 * @retval true  A valid trace window was copied.
 * @retval false No bench samples have been captured, or `out == NULL`.
 */
bool motor_bench_trace_read(struct motor_bench_trace *out);

/**
 * @brief Clear the RAM-only bench trace window.
 *
 * This is a stopped/diagnostic operation and must not be called from the FOC
 * interrupt path.
 */
void motor_bench_trace_clear(void);

/**
 * @brief Stage one RAM-only hardware snapshot captured at the FOC sample boundary.
 *
 * The ADC ISR calls this before any fast fault latch or FOC computation so the
 * same register image can back both first-fault context and completed bench
 * samples. Passing `NULL` clears the staged snapshot.
 */
void motor_fault_runtime_stage_hw_snapshot(const struct motor_hw_snapshot *snapshot);

#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
/**
 * @brief Produce one bench-only sample after a completed FOC calculation.
 *
 * This helper is bench-only and must not be called by normal product images.
 */
void motor_bench_trace_update(const struct motor_dev *motor);

/**
 * @brief Read one retained bench sample by chronological index.
 *
 * Index `0` is the oldest retained sample. Invalid indices or `out == NULL`
 * fail without modifying the output buffer.
 */
bool motor_bench_trace_sample_read(uint32_t index, struct motor_bench_sample *out);

/**
 * @brief Return the number of retained bench samples.
 */
uint32_t motor_bench_trace_sample_count(void);

/**
 * @brief Clear the retained bench sample ring and reset its sequence.
 */
void motor_bench_trace_sample_clear(void);
#endif

/* Runtime Policy ------------------------------------------------------------------------ */

/**
 * @brief  Evaluate live motor measurements and latch FalconFoc product faults.
 */
void foc_fault_check(struct motor_dev *motor);

/**
 * @brief  Execute the constant-time ADC-ISR protection check.
 *
 * This function may be called immediately after the injected ADC sample is
 * converted and before any FOC calculation.  It only latches a fault and
 * disables PWM; state-machine ownership remains in `motor_fsm`.
 */
void foc_fault_check_fast(struct motor_dev *motor);

/**
 * @brief  Latch the bench diagnostic runtime budget fault from an ISR.
 *
 * Bench-only callers use this as a last-resort energy bound when the shell
 * task cannot service a stop command quickly enough.  It performs no I/O,
 * logging, Flash access, or FSM dispatch.
 */
void foc_fault_bench_timeout(struct motor_dev *motor);

/**
 * @brief  Reset task-level persistence counters after a safe lifecycle edge.
 */
void foc_fault_runtime_reset(void);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
