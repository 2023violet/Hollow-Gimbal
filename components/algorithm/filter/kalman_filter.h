/**
 * @file    kalman_filter.h
 * @brief   Dynamic-dimension Kalman filter component built on CMSIS-DSP matrices
 * @version v1.0.0
 * @date    2026.03.28
 *
 * Design overview
 * ---------------
 * `Kalman_Filter` is a reusable matrix-based filter component that owns state,
 * covariance, and temporary workspaces for one Kalman filter instance. It
 * supports a fixed-dimension mode and an auto-adjustment mode where the active
 * measurement subset can change at runtime.
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - The caller owns each `struct Kalman_Filter` instance.
 * - `Kalman_Filter_Init()` allocates all internal storage through the
 *   implementation-owned allocator path and binds the CMSIS-DSP matrix views
 *   onto that storage.
 * - The component currently has no deinit/free API, so instances are expected
 *   to live for the whole runtime once initialized.
 * - `MeasuredVector` and `ControlVector` are caller-owned update points inside
 *   the allocated object and are consumed/reset by `Kalman_Filter_Update()`.
 * - In auto-adjust mode, callers may set `MeasurementValidFlags[i] = 1` before
 *   `Kalman_Filter_Update()` to mark an explicit zero-valued measurement as
 *   valid for that cycle. The flags are consumed and cleared during the update.
 * - Task-context only by default. No internal locking is provided.
 *
 * Usage example
 * -------------
 * @code
 *   struct Kalman_Filter kf;
 *
 *   Kalman_Filter_Init(&kf, 3U, 0U, 3U);
 *   fill_initial_matrices(&kf);
 *   kf.MeasuredVector[0] = sample0;
 *   Kalman_Filter_Update(&kf);
 * @endcode
 *
 * Copyright (c) 2025 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdint.h>
#include "arm_math.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros -------------------------------------------------------------------------------- */

/** Matrix type used by the current float32 implementation. */
#define mat              arm_matrix_instance_f32
/** CMSIS-DSP matrix init helper used by the current float32 implementation. */
#define Matrix_Init      arm_mat_init_f32
/** CMSIS-DSP matrix add helper used by the current float32 implementation. */
#define Matrix_Add       arm_mat_add_f32
/** CMSIS-DSP matrix subtract helper used by the current float32 implementation. */
#define Matrix_Subtract  arm_mat_sub_f32
/** CMSIS-DSP matrix multiply helper used by the current float32 implementation. */
#define Matrix_Multiply  arm_mat_mult_f32
/** CMSIS-DSP matrix transpose helper used by the current float32 implementation. */
#define Matrix_Transpose arm_mat_trans_f32
/** CMSIS-DSP matrix inverse helper used by the current float32 implementation. */
#define Matrix_Inverse   arm_mat_inverse_f32

/* Public Types --------------------------------------------------------------------------------- */

/** One dynamic-dimension Kalman filter instance and all of its matrix workspaces. */
struct Kalman_Filter {
    /** Filtered state vector exposed to the caller after each update. */
    float *FilteredValue;
    /** Measurement input vector to be filled by the caller before update. */
    float *MeasuredVector;
    /** Control input vector to be filled by the caller before update. */
    float *ControlVector;

    /** State vector dimension. */
    uint8_t xhatSize;
    /** Control vector dimension. */
    uint8_t uSize;
    /** Measurement vector dimension. */
    uint8_t zSize;

    /** Non-zero enables runtime measurement-subset auto adjustment. */
    uint8_t  UseAutoAdjustment;
    /** Number of currently valid measurements after adjustment. */
    uint8_t  MeasurementValidNum;
    /** Per-measurement valid flags consumed by auto-adjust mode. */
    uint8_t *MeasurementValidFlags;

    /** Measurement-to-state index map used by auto adjustment. */
    uint8_t *MeasurementMap;
    /** Measurement scaling coefficients used to build matrix H. */
    float   *MeasurementDegree;
    /** Per-measurement diagonal values used to build matrix R. */
    float   *MatR_DiagonalElements;
    /** Lower bound applied to the diagonal of covariance matrix P. */
    float   *StateMinVariance;
    /** Temporary index map used during auto adjustment. */
    uint8_t *temp;

    /** Skip flag for state prediction equation. */
    uint8_t SkipEq1;
    /** Skip flag for covariance prediction equation. */
    uint8_t SkipEq2;
    /** Skip flag for Kalman-gain equation. */
    uint8_t SkipEq3;
    /** Skip flag for state correction equation. */
    uint8_t SkipEq4;
    /** Skip flag for covariance correction equation. */
    uint8_t SkipEq5;

    /** Current state estimate x(k|k). */
    mat xhat;
    /** Predicted state estimate x(k|k-1). */
    mat xhatminus;
    /** Control vector u(k). */
    mat u;
    /** Measurement vector z(k). */
    mat z;
    /** Corrected covariance P(k|k). */
    mat P;
    /** Predicted covariance P(k|k-1). */
    mat Pminus;
    /** State transition matrix F. */
    mat F;
    /** Transpose of F. */
    mat FT;
    /** Control matrix B. */
    mat B;
    /** Measurement matrix H. */
    mat H;
    /** Transpose of H. */
    mat HT;
    /** Process noise covariance Q. */
    mat Q;
    /** Measurement noise covariance R. */
    mat R;
    /** Kalman gain K. */
    mat K;
    /** Innovation covariance S. */
    mat S;
    /** Temporary matrix workspace. */
    mat temp_matrix;
    /** Secondary temporary matrix workspace. */
    mat temp_matrix1;
    /** Temporary vector workspace. */
    mat temp_vector;
    /** Secondary temporary vector workspace. */
    mat temp_vector1;

    /** Most recent CMSIS-DSP matrix operation status. */
    int8_t MatStatus;

    /** Optional user hook executed after measurement collection. */
    void (*User_Func0_f)(struct Kalman_Filter *kf);
    /** Optional user hook executed after state prediction. */
    void (*User_Func1_f)(struct Kalman_Filter *kf);
    /** Optional user hook executed after covariance prediction. */
    void (*User_Func2_f)(struct Kalman_Filter *kf);
    /** Optional user hook executed after gain update. */
    void (*User_Func3_f)(struct Kalman_Filter *kf);
    /** Optional user hook executed after state correction. */
    void (*User_Func4_f)(struct Kalman_Filter *kf);
    /** Optional user hook executed after covariance correction. */
    void (*User_Func5_f)(struct Kalman_Filter *kf);
    /** Optional user hook executed before returning the filtered value. */
    void (*User_Func6_f)(struct Kalman_Filter *kf);

    /** Backing storage for `xhat`. */
    float *xhat_data;
    /** Backing storage for `xhatminus`. */
    float *xhatminus_data;
    /** Backing storage for `u`. */
    float *u_data;
    /** Backing storage for `z`. */
    float *z_data;
    /** Backing storage for `P`. */
    float *P_data;
    /** Backing storage for `Pminus`. */
    float *Pminus_data;
    /** Backing storage for `F`. */
    float *F_data;
    /** Backing storage for `FT`. */
    float *FT_data;
    /** Backing storage for `B`. */
    float *B_data;
    /** Backing storage for `H`. */
    float *H_data;
    /** Backing storage for `HT`. */
    float *HT_data;
    /** Backing storage for `Q`. */
    float *Q_data;
    /** Backing storage for `R`. */
    float *R_data;
    /** Backing storage for `K`. */
    float *K_data;
    /** Backing storage for `S`. */
    float *S_data;
    /** Backing storage for `temp_matrix`. */
    float *temp_matrix_data;
    /** Backing storage for `temp_matrix1`. */
    float *temp_matrix_data1;
    /** Backing storage for `temp_vector`. */
    float *temp_vector_data;
    /** Backing storage for `temp_vector1`. */
    float *temp_vector_data1;
};

/* Public Variables ----------------------------------------------------------------------------- */

/** Cached `sizeof(float)` used by the legacy implementation. */
extern uint16_t sizeof_float;
/** Cached `sizeof(double)` used by the legacy implementation. */
extern uint16_t sizeof_double;

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief Allocate and bind all internal storage for one Kalman filter instance.
 *
 * Configures the filter dimensions and allocates the required internal
 * state and covariance matrices.
 *
 * @warning This function may call dynamic memory allocation (`malloc`).
 *          The caller must later call a corresponding cleanup routine to free
 *          these resources if the filter is destroyed.
 *
 * @param[out] kf       Caller-owned Kalman filter instance to initialize.
 * @param[in]  xhatSize Dimension of the state vector (x).
 * @param[in]  uSize    Dimension of the control vector (u).
 * @param[in]  zSize    Dimension of the measurement vector (z).
 */
void Kalman_Filter_Init(struct Kalman_Filter *kf, uint8_t xhatSize, uint8_t uSize, uint8_t zSize);

/**
 * @brief Refresh measurement and control vectors before one update cycle.
 *
 * Copies the external measurement (`Z`) and control (`U`) vectors into
 * the filter's internal working buffers.
 *
 * @note This function is NOT thread-safe.
 *
 * @param[in,out] kf Kalman filter instance.
 */
void Kalman_Filter_Measure(struct Kalman_Filter *kf);

/**
 * @brief Run the state prediction step.
 *
 * Computes `xhat_minus = A * xhat + B * u`.
 *
 * @note This function is NOT thread-safe.
 *
 * @param[in,out] kf Kalman filter instance.
 */
void Kalman_Filter_xhatMinusUpdate(struct Kalman_Filter *kf);

/**
 * @brief Run the covariance prediction step.
 *
 * Computes `P_minus = A * P * A' + Q`.
 *
 * @note This function is NOT thread-safe.
 *
 * @param[in,out] kf Kalman filter instance.
 */
void Kalman_Filter_PminusUpdate(struct Kalman_Filter *kf);

/**
 * @brief Recompute the Kalman gain.
 *
 * Computes `K = P_minus * H' * (H * P_minus * H' + R)^-1`.
 *
 * @note This function is NOT thread-safe.
 *
 * @param[in,out] kf Kalman filter instance.
 */
void Kalman_Filter_SetK(struct Kalman_Filter *kf);

/**
 * @brief Run the state correction step.
 *
 * Computes `xhat = xhat_minus + K * (Z - H * xhat_minus)`.
 *
 * @note This function is NOT thread-safe.
 *
 * @param[in,out] kf Kalman filter instance.
 */
void Kalman_Filter_xhatUpdate(struct Kalman_Filter *kf);

/**
 * @brief Run the covariance correction step.
 *
 * Computes `P = (I - K * H) * P_minus`.
 *
 * @note This function is NOT thread-safe.
 *
 * @param[in,out] kf Kalman filter instance.
 */
void Kalman_Filter_P_Update(struct Kalman_Filter *kf);

/**
 * @brief Run one full update cycle and return the filtered state vector.
 *
 * Executes `Measure` -> `xhatMinus` -> `Pminus` -> `SetK` -> `xhat` -> `P`
 * in sequence.
 *
 * @note This function is NOT thread-safe. It assumes `Kalman_Filter_Measure()`
 *       has populated the internal `Z` and `U` vectors for this cycle.
 *
 * @param[in,out] kf Kalman filter instance.
 *
 * @return Pointer to the internal filtered state vector (`xhat`).
 *         The returned pointer is borrowed and remains valid only until the
 *         next modification of the filter instance.
 */
float *Kalman_Filter_Update(struct Kalman_Filter *kf);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
