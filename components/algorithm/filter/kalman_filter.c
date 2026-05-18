/**
 * @file    kalman_filter.c
 * @brief   Dynamic-dimension Kalman filter component implementation
 * @version v1.1.0
 * @date    2026.03.28
 *
 * Design overview
 * ---------------
 * This file keeps the legacy CMSIS-DSP based Kalman filter math while aligning
 * the implementation with the current Themis layering and comment contracts.
 * The filter still supports two modes:
 *
 * - fixed-dimension mode: `H`, `R`, and `z` stay at the configured dimensions
 * - auto-adjust mode: the active measurement subset is rebuilt every cycle
 *
 * Implementation notes
 * --------------------
 * - `MeasuredVector` uses zero as the "no new sample this cycle" sentinel in
 *   auto-adjust mode, so the vector is consumed and cleared every update.
 * - Temporary workspace now sizes against `max(xhatSize, zSize)` so the
 *   measurement path remains safe even when the measurement dimension exceeds
 *   the state dimension.
 * - Invalid `MeasurementMap` entries are ignored during auto-adjustment to
 *   avoid underflow or out-of-range writes into matrix `H`.
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "kalman_filter.h"
#include <stdbool.h>
#include <string.h>
#include "tm_runtime.h"

/* Public Variables ----------------------------------------------------------------------------- */

uint16_t sizeof_float  = sizeof(float);
uint16_t sizeof_double = sizeof(double);

/* Private Helpers ------------------------------------------------------------------------------ */

/*
 * The current firmware build keeps Kalman workspace ownership on the Themis
 * runtime heap, but that choice stays inside the implementation so the public
 * header no longer leaks `tm_runtime.h` to every consumer.
 */
#define KALMAN_FILTER_ALLOC(size) tm_malloc((uint32_t)(size))

static void   _kalman_release_storage(struct Kalman_Filter *kf);
static bool   _kalman_require_ready(struct Kalman_Filter *kf);
static bool   _kalman_record_status(struct Kalman_Filter *kf, arm_status status);
static void   _kalman_copy_prediction_to_state(struct Kalman_Filter *kf);
static size_t _kalman_workspace_span(const struct Kalman_Filter *kf)
{
    if (kf == NULL) {
        return 0U;
    }

    return (kf->xhatSize > kf->zSize) ? (size_t)kf->xhatSize : (size_t)kf->zSize;
}

static void _kalman_init_vector(mat *matrix, uint16_t rows, float *storage)
{
    Matrix_Init(matrix, rows, 1U, storage);
}

static void _kalman_reset_control_views(struct Kalman_Filter *kf)
{
    kf->ControlVector = NULL;
    kf->u_data        = NULL;
    Matrix_Init(&kf->u, 0U, 0U, NULL);

    kf->B_data = NULL;
    Matrix_Init(&kf->B, 0U, 0U, NULL);
}

static void _kalman_copy_measurements(struct Kalman_Filter *kf)
{
    memcpy(kf->z_data, kf->MeasuredVector, sizeof_float * kf->zSize);

    /*
     * Auto-adjust mode treats zero as "measurement absent this cycle", so the
     * producer side must refill `MeasuredVector` before the next update.
     */
    memset(kf->MeasuredVector, 0, sizeof_float * kf->zSize);
}

static void _kalman_release_storage(struct Kalman_Filter *kf)
{
    if (kf == NULL) {
        return;
    }

    tm_free(kf->MeasurementValidFlags);
    tm_free(kf->MeasurementMap);
    tm_free(kf->MeasurementDegree);
    tm_free(kf->MatR_DiagonalElements);
    tm_free(kf->StateMinVariance);
    tm_free(kf->temp);
    tm_free(kf->FilteredValue);
    tm_free(kf->MeasuredVector);
    tm_free(kf->ControlVector);
    tm_free(kf->xhat_data);
    tm_free(kf->xhatminus_data);
    tm_free(kf->u_data);
    tm_free(kf->z_data);
    tm_free(kf->P_data);
    tm_free(kf->Pminus_data);
    tm_free(kf->F_data);
    tm_free(kf->FT_data);
    tm_free(kf->B_data);
    tm_free(kf->H_data);
    tm_free(kf->HT_data);
    tm_free(kf->Q_data);
    tm_free(kf->R_data);
    tm_free(kf->K_data);
    tm_free(kf->S_data);
    tm_free(kf->temp_matrix_data);
    tm_free(kf->temp_matrix_data1);
    tm_free(kf->temp_vector_data);
    tm_free(kf->temp_vector_data1);
    memset(kf, 0, sizeof(*kf));
}

static bool _kalman_require_ready(struct Kalman_Filter *kf)
{
    if (kf == NULL) {
        return false;
    }

    if ((kf->xhatSize == 0U) || (kf->zSize == 0U) || (kf->FilteredValue == NULL) ||
        (kf->MeasuredVector == NULL) || (kf->MeasurementValidFlags == NULL) ||
        (kf->MeasurementMap == NULL) || (kf->MeasurementDegree == NULL) ||
        (kf->MatR_DiagonalElements == NULL) || (kf->StateMinVariance == NULL) ||
        (kf->temp == NULL) || (kf->xhat_data == NULL) || (kf->xhatminus_data == NULL) ||
        (kf->z_data == NULL) || (kf->P_data == NULL) || (kf->Pminus_data == NULL) ||
        (kf->F_data == NULL) || (kf->FT_data == NULL) || (kf->H_data == NULL) ||
        (kf->HT_data == NULL) || (kf->Q_data == NULL) || (kf->R_data == NULL) ||
        (kf->K_data == NULL) || (kf->S_data == NULL) || (kf->temp_matrix_data == NULL) ||
        (kf->temp_matrix_data1 == NULL) || (kf->temp_vector_data == NULL) ||
        (kf->temp_vector_data1 == NULL)) {
        kf->MatStatus = (int8_t)ARM_MATH_ARGUMENT_ERROR;
        return false;
    }

    if ((kf->uSize > 0U) &&
        ((kf->ControlVector == NULL) || (kf->u_data == NULL) || (kf->B_data == NULL))) {
        kf->MatStatus = (int8_t)ARM_MATH_ARGUMENT_ERROR;
        return false;
    }

    return true;
}

static bool _kalman_record_status(struct Kalman_Filter *kf, arm_status status)
{
    if (kf == NULL) {
        return false;
    }

    kf->MatStatus = (int8_t)status;
    return (status == ARM_MATH_SUCCESS);
}

static void _kalman_copy_prediction_to_state(struct Kalman_Filter *kf)
{
    if ((kf == NULL) || (kf->xhat_data == NULL) || (kf->xhatminus_data == NULL) ||
        (kf->P_data == NULL) || (kf->Pminus_data == NULL)) {
        return;
    }

    memcpy(kf->xhat_data, kf->xhatminus_data, sizeof_float * kf->xhatSize);
    memcpy(kf->P_data, kf->Pminus_data, sizeof_float * kf->xhatSize * kf->xhatSize);
}

static void H_K_R_Adjustment(struct Kalman_Filter *kf);

/* Public Functions ----------------------------------------------------------------------------- */

void Kalman_Filter_Init(struct Kalman_Filter *kf, uint8_t xhatSize, uint8_t uSize, uint8_t zSize)
{
    size_t workspace_span;
    size_t workspace_matrix_bytes;
    size_t workspace_vector_bytes;

    if ((kf == NULL) || (xhatSize == 0U) || (zSize == 0U)) {
        return;
    }

    memset(kf, 0, sizeof(*kf));
    kf->xhatSize            = xhatSize;
    kf->uSize               = uSize;
    kf->zSize               = zSize;
    kf->MeasurementValidNum = 0U;

    workspace_span         = _kalman_workspace_span(kf);
    workspace_matrix_bytes = sizeof_float * workspace_span * workspace_span;
    workspace_vector_bytes = sizeof_float * workspace_span;

    kf->MeasurementValidFlags = (uint8_t *)tm_calloc(zSize, sizeof(uint8_t));
    if (kf->MeasurementValidFlags == NULL) {
        goto init_failed;
    }

    kf->MeasurementMap = (uint8_t *)tm_calloc(zSize, sizeof(uint8_t));
    if (kf->MeasurementMap == NULL) {
        goto init_failed;
    }

    kf->MeasurementDegree = (float *)tm_calloc(zSize, sizeof_float);
    if (kf->MeasurementDegree == NULL) {
        goto init_failed;
    }

    kf->MatR_DiagonalElements = (float *)tm_calloc(zSize, sizeof_float);
    if (kf->MatR_DiagonalElements == NULL) {
        goto init_failed;
    }

    kf->StateMinVariance = (float *)tm_calloc(xhatSize, sizeof_float);
    if (kf->StateMinVariance == NULL) {
        goto init_failed;
    }

    kf->temp = (uint8_t *)tm_calloc(zSize, sizeof(uint8_t));
    if (kf->temp == NULL) {
        goto init_failed;
    }

    kf->FilteredValue = (float *)tm_calloc(xhatSize, sizeof_float);
    if (kf->FilteredValue == NULL) {
        goto init_failed;
    }

    kf->MeasuredVector = (float *)tm_calloc(zSize, sizeof_float);
    if (kf->MeasuredVector == NULL) {
        goto init_failed;
    }

    kf->xhat_data = (float *)tm_calloc(xhatSize, sizeof_float);
    if (kf->xhat_data == NULL) {
        goto init_failed;
    }
    _kalman_init_vector(&kf->xhat, kf->xhatSize, kf->xhat_data);

    kf->xhatminus_data = (float *)tm_calloc(xhatSize, sizeof_float);
    if (kf->xhatminus_data == NULL) {
        goto init_failed;
    }
    _kalman_init_vector(&kf->xhatminus, kf->xhatSize, kf->xhatminus_data);

    if (uSize > 0U) {
        kf->ControlVector = (float *)tm_calloc(uSize, sizeof_float);
        if (kf->ControlVector == NULL) {
            goto init_failed;
        }

        kf->u_data = (float *)tm_calloc(uSize, sizeof_float);
        if (kf->u_data == NULL) {
            goto init_failed;
        }
        _kalman_init_vector(&kf->u, kf->uSize, kf->u_data);

        kf->B_data = (float *)tm_calloc((uint32_t)xhatSize * (uint32_t)uSize, sizeof_float);
        if (kf->B_data == NULL) {
            goto init_failed;
        }
        Matrix_Init(&kf->B, kf->xhatSize, kf->uSize, kf->B_data);
    } else {
        _kalman_reset_control_views(kf);
    }

    kf->z_data = (float *)tm_calloc(zSize, sizeof_float);
    if (kf->z_data == NULL) {
        goto init_failed;
    }
    _kalman_init_vector(&kf->z, kf->zSize, kf->z_data);

    kf->P_data = (float *)tm_calloc((uint32_t)xhatSize * (uint32_t)xhatSize, sizeof_float);
    if (kf->P_data == NULL) {
        goto init_failed;
    }
    Matrix_Init(&kf->P, kf->xhatSize, kf->xhatSize, kf->P_data);

    kf->Pminus_data = (float *)tm_calloc((uint32_t)xhatSize * (uint32_t)xhatSize, sizeof_float);
    if (kf->Pminus_data == NULL) {
        goto init_failed;
    }
    Matrix_Init(&kf->Pminus, kf->xhatSize, kf->xhatSize, kf->Pminus_data);

    kf->F_data = (float *)tm_calloc((uint32_t)xhatSize * (uint32_t)xhatSize, sizeof_float);
    if (kf->F_data == NULL) {
        goto init_failed;
    }
    Matrix_Init(&kf->F, kf->xhatSize, kf->xhatSize, kf->F_data);

    kf->FT_data = (float *)tm_calloc((uint32_t)xhatSize * (uint32_t)xhatSize, sizeof_float);
    if (kf->FT_data == NULL) {
        goto init_failed;
    }
    Matrix_Init(&kf->FT, kf->xhatSize, kf->xhatSize, kf->FT_data);

    kf->H_data = (float *)tm_calloc((uint32_t)zSize * (uint32_t)xhatSize, sizeof_float);
    if (kf->H_data == NULL) {
        goto init_failed;
    }
    Matrix_Init(&kf->H, kf->zSize, kf->xhatSize, kf->H_data);

    kf->HT_data = (float *)tm_calloc((uint32_t)xhatSize * (uint32_t)zSize, sizeof_float);
    if (kf->HT_data == NULL) {
        goto init_failed;
    }
    Matrix_Init(&kf->HT, kf->xhatSize, kf->zSize, kf->HT_data);

    kf->Q_data = (float *)tm_calloc((uint32_t)xhatSize * (uint32_t)xhatSize, sizeof_float);
    if (kf->Q_data == NULL) {
        goto init_failed;
    }
    Matrix_Init(&kf->Q, kf->xhatSize, kf->xhatSize, kf->Q_data);

    kf->R_data = (float *)tm_calloc((uint32_t)zSize * (uint32_t)zSize, sizeof_float);
    if (kf->R_data == NULL) {
        goto init_failed;
    }
    Matrix_Init(&kf->R, kf->zSize, kf->zSize, kf->R_data);

    kf->K_data = (float *)tm_calloc((uint32_t)xhatSize * (uint32_t)zSize, sizeof_float);
    if (kf->K_data == NULL) {
        goto init_failed;
    }
    Matrix_Init(&kf->K, kf->xhatSize, kf->zSize, kf->K_data);

    /*
     * The legacy implementation reuses one generic matrix/vector workspace
     * across both prediction and measurement stages, so size it against the
     * widest runtime dimension instead of assuming `zSize <= xhatSize`.
     */
    kf->S_data = (float *)tm_calloc(1U, (uint32_t)workspace_matrix_bytes);
    if (kf->S_data == NULL) {
        goto init_failed;
    }
    Matrix_Init(&kf->S, (uint16_t)workspace_span, (uint16_t)workspace_span, kf->S_data);

    kf->temp_matrix_data = (float *)tm_calloc(1U, (uint32_t)workspace_matrix_bytes);
    if (kf->temp_matrix_data == NULL) {
        goto init_failed;
    }
    Matrix_Init(&kf->temp_matrix,
                (uint16_t)workspace_span,
                (uint16_t)workspace_span,
                kf->temp_matrix_data);

    kf->temp_matrix_data1 = (float *)tm_calloc(1U, (uint32_t)workspace_matrix_bytes);
    if (kf->temp_matrix_data1 == NULL) {
        goto init_failed;
    }
    Matrix_Init(&kf->temp_matrix1,
                (uint16_t)workspace_span,
                (uint16_t)workspace_span,
                kf->temp_matrix_data1);

    kf->temp_vector_data = (float *)tm_calloc(1U, (uint32_t)workspace_vector_bytes);
    if (kf->temp_vector_data == NULL) {
        goto init_failed;
    }
    _kalman_init_vector(&kf->temp_vector, (uint16_t)workspace_span, kf->temp_vector_data);

    kf->temp_vector_data1 = (float *)tm_calloc(1U, (uint32_t)workspace_vector_bytes);
    if (kf->temp_vector_data1 == NULL) {
        goto init_failed;
    }
    _kalman_init_vector(&kf->temp_vector1, (uint16_t)workspace_span, kf->temp_vector_data1);

    kf->SkipEq1 = 0U;
    kf->SkipEq2 = 0U;
    kf->SkipEq3 = 0U;
    kf->SkipEq4 = 0U;
    kf->SkipEq5 = 0U;
    kf->MatStatus = (int8_t)ARM_MATH_SUCCESS;
    return;

init_failed:
    _kalman_release_storage(kf);
}

void Kalman_Filter_Measure(struct Kalman_Filter *kf)
{
    if (!_kalman_require_ready(kf)) {
        return;
    }

    if (kf->UseAutoAdjustment != 0U) {
        H_K_R_Adjustment(kf);
    } else {
        _kalman_copy_measurements(kf);
        memset(kf->MeasurementValidFlags, 0, sizeof(uint8_t) * kf->zSize);
    }

    /*
     * Control input is level-triggered rather than edge-triggered in the
     * current component contract, so keep the caller buffer intact after copy.
     */
    if ((kf->uSize > 0U) && (kf->u_data != NULL) && (kf->ControlVector != NULL)) {
        memcpy(kf->u_data, kf->ControlVector, sizeof_float * kf->uSize);
    }
}

void Kalman_Filter_xhatMinusUpdate(struct Kalman_Filter *kf)
{
    if (!_kalman_require_ready(kf) || (kf->SkipEq1 != 0U)) {
        return;
    }

    if (kf->uSize > 0U) {
        kf->temp_vector.numRows = kf->xhatSize;
        kf->temp_vector.numCols = 1U;
        if (!_kalman_record_status(kf, Matrix_Multiply(&kf->F, &kf->xhat, &kf->temp_vector))) {
            return;
        }

        kf->temp_vector1.numRows = kf->xhatSize;
        kf->temp_vector1.numCols = 1U;
        if (!_kalman_record_status(kf, Matrix_Multiply(&kf->B, &kf->u, &kf->temp_vector1))) {
            return;
        }

        (void)_kalman_record_status(kf,
                                    Matrix_Add(&kf->temp_vector, &kf->temp_vector1, &kf->xhatminus));
    } else {
        (void)_kalman_record_status(kf, Matrix_Multiply(&kf->F, &kf->xhat, &kf->xhatminus));
    }
}

void Kalman_Filter_PminusUpdate(struct Kalman_Filter *kf)
{
    if (!_kalman_require_ready(kf) || (kf->SkipEq2 != 0U)) {
        return;
    }

    if (!_kalman_record_status(kf, Matrix_Transpose(&kf->F, &kf->FT))) {
        return;
    }
    if (!_kalman_record_status(kf, Matrix_Multiply(&kf->F, &kf->P, &kf->Pminus))) {
        return;
    }

    kf->temp_matrix.numRows = kf->Pminus.numRows;
    kf->temp_matrix.numCols = kf->FT.numCols;
    if (!_kalman_record_status(kf, Matrix_Multiply(&kf->Pminus, &kf->FT, &kf->temp_matrix))) {
        return;
    }

    (void)_kalman_record_status(kf, Matrix_Add(&kf->temp_matrix, &kf->Q, &kf->Pminus));
}

void Kalman_Filter_SetK(struct Kalman_Filter *kf)
{
    if (!_kalman_require_ready(kf) || (kf->SkipEq3 != 0U)) {
        return;
    }

    if (!_kalman_record_status(kf, Matrix_Transpose(&kf->H, &kf->HT))) {
        return;
    }

    kf->temp_matrix.numRows = kf->H.numRows;
    kf->temp_matrix.numCols = kf->Pminus.numCols;
    if (!_kalman_record_status(kf, Matrix_Multiply(&kf->H, &kf->Pminus, &kf->temp_matrix))) {
        return;
    }

    kf->temp_matrix1.numRows = kf->temp_matrix.numRows;
    kf->temp_matrix1.numCols = kf->HT.numCols;
    if (!_kalman_record_status(kf,
                               Matrix_Multiply(&kf->temp_matrix, &kf->HT, &kf->temp_matrix1))) {
        return;
    }

    kf->S.numRows = kf->R.numRows;
    kf->S.numCols = kf->R.numCols;
    if (!_kalman_record_status(kf, Matrix_Add(&kf->temp_matrix1, &kf->R, &kf->S))) {
        return;
    }

    if (!_kalman_record_status(kf, Matrix_Inverse(&kf->S, &kf->temp_matrix1))) {
        return;
    }

    kf->temp_matrix.numRows = kf->Pminus.numRows;
    kf->temp_matrix.numCols = kf->HT.numCols;
    if (!_kalman_record_status(kf, Matrix_Multiply(&kf->Pminus, &kf->HT, &kf->temp_matrix))) {
        return;
    }

    (void)_kalman_record_status(kf, Matrix_Multiply(&kf->temp_matrix, &kf->temp_matrix1, &kf->K));
}

void Kalman_Filter_xhatUpdate(struct Kalman_Filter *kf)
{
    if (!_kalman_require_ready(kf) || (kf->SkipEq4 != 0U)) {
        return;
    }

    kf->temp_vector.numRows = kf->H.numRows;
    kf->temp_vector.numCols = 1U;
    if (!_kalman_record_status(kf, Matrix_Multiply(&kf->H, &kf->xhatminus, &kf->temp_vector))) {
        return;
    }

    kf->temp_vector1.numRows = kf->z.numRows;
    kf->temp_vector1.numCols = 1U;
    if (!_kalman_record_status(kf, Matrix_Subtract(&kf->z, &kf->temp_vector, &kf->temp_vector1))) {
        return;
    }

    kf->temp_vector.numRows = kf->K.numRows;
    kf->temp_vector.numCols = 1U;
    if (!_kalman_record_status(kf, Matrix_Multiply(&kf->K, &kf->temp_vector1, &kf->temp_vector))) {
        return;
    }

    (void)_kalman_record_status(kf, Matrix_Add(&kf->xhatminus, &kf->temp_vector, &kf->xhat));
}

void Kalman_Filter_P_Update(struct Kalman_Filter *kf)
{
    if (!_kalman_require_ready(kf) || (kf->SkipEq5 != 0U)) {
        return;
    }

    kf->temp_matrix.numRows  = kf->K.numRows;
    kf->temp_matrix.numCols  = kf->H.numCols;
    kf->temp_matrix1.numRows = kf->temp_matrix.numRows;
    kf->temp_matrix1.numCols = kf->Pminus.numCols;

    if (!_kalman_record_status(kf, Matrix_Multiply(&kf->K, &kf->H, &kf->temp_matrix))) {
        return;
    }
    if (!_kalman_record_status(kf,
                               Matrix_Multiply(&kf->temp_matrix, &kf->Pminus, &kf->temp_matrix1))) {
        return;
    }
    (void)_kalman_record_status(kf, Matrix_Subtract(&kf->Pminus, &kf->temp_matrix1, &kf->P));
}

float *Kalman_Filter_Update(struct Kalman_Filter *kf)
{
    if (!_kalman_require_ready(kf)) {
        return NULL;
    }

    kf->MatStatus = (int8_t)ARM_MATH_SUCCESS;
    Kalman_Filter_Measure(kf);
    if (kf->User_Func0_f != NULL) {
        kf->User_Func0_f(kf);
    }

    Kalman_Filter_xhatMinusUpdate(kf);
    if (kf->MatStatus != ARM_MATH_SUCCESS) {
        return NULL;
    }
    if (kf->User_Func1_f != NULL) {
        kf->User_Func1_f(kf);
    }

    Kalman_Filter_PminusUpdate(kf);
    if (kf->MatStatus != ARM_MATH_SUCCESS) {
        return NULL;
    }
    if (kf->User_Func2_f != NULL) {
        kf->User_Func2_f(kf);
    }

    if ((kf->MeasurementValidNum != 0U) || (kf->UseAutoAdjustment == 0U)) {
        Kalman_Filter_SetK(kf);
        if (kf->MatStatus != ARM_MATH_SUCCESS) {
            _kalman_copy_prediction_to_state(kf);
            goto finish_update;
        }
        if (kf->User_Func3_f != NULL) {
            kf->User_Func3_f(kf);
        }

        Kalman_Filter_xhatUpdate(kf);
        if (kf->MatStatus != ARM_MATH_SUCCESS) {
            _kalman_copy_prediction_to_state(kf);
            goto finish_update;
        }
        if (kf->User_Func4_f != NULL) {
            kf->User_Func4_f(kf);
        }

        Kalman_Filter_P_Update(kf);
        if (kf->MatStatus != ARM_MATH_SUCCESS) {
            _kalman_copy_prediction_to_state(kf);
            goto finish_update;
        }
    } else {
        /*
         * When no new measurement arrives in auto-adjust mode, preserve the
         * pure prediction result instead of attempting a zero-row correction.
         */
        _kalman_copy_prediction_to_state(kf);
    }

finish_update:
    if (kf->User_Func5_f != NULL) {
        kf->User_Func5_f(kf);
    }

    /*
     * Clamp the covariance diagonal so the filter does not become numerically
     * overconfident and stop tracking slow state drift.
     */
    for (uint8_t i = 0U; i < kf->xhatSize; ++i) {
        if (kf->P_data[i * kf->xhatSize + i] < kf->StateMinVariance[i]) {
            kf->P_data[i * kf->xhatSize + i] = kf->StateMinVariance[i];
        }
    }

    memcpy(kf->FilteredValue, kf->xhat_data, sizeof_float * kf->xhatSize);

    if (kf->User_Func6_f != NULL) {
        kf->User_Func6_f(kf);
    }

    return kf->FilteredValue;
}

/* Private Helpers ------------------------------------------------------------------------------ */

static void H_K_R_Adjustment(struct Kalman_Filter *kf)
{
    if (kf == NULL) {
        return;
    }

    kf->MeasurementValidNum = 0U;
    _kalman_copy_measurements(kf);

    memset(kf->R_data, 0, sizeof_float * kf->zSize * kf->zSize);
    memset(kf->H_data, 0, sizeof_float * kf->xhatSize * kf->zSize);

    for (uint8_t i = 0U; i < kf->zSize; ++i) {
        uint8_t state_index;
        bool    measurement_valid;

        measurement_valid = (kf->MeasurementValidFlags[i] != 0U) || (kf->z_data[i] != 0.0f);
        if (!measurement_valid) {
            continue;
        }

        state_index = kf->MeasurementMap[i];
        if ((state_index == 0U) || (state_index > kf->xhatSize)) {
            continue;
        }

        kf->z_data[kf->MeasurementValidNum] = kf->z_data[i];
        kf->temp[kf->MeasurementValidNum]   = i;

        kf->H_data[kf->xhatSize * kf->MeasurementValidNum + (state_index - 1U)] =
            kf->MeasurementDegree[i];
        kf->MeasurementValidNum++;
    }

    memset(kf->MeasurementValidFlags, 0, sizeof(uint8_t) * kf->zSize);

    for (uint8_t i = 0U; i < kf->MeasurementValidNum; ++i) {
        kf->R_data[i * kf->MeasurementValidNum + i] = kf->MatR_DiagonalElements[kf->temp[i]];
    }

    kf->H.numRows  = kf->MeasurementValidNum;
    kf->H.numCols  = kf->xhatSize;
    kf->HT.numRows = kf->xhatSize;
    kf->HT.numCols = kf->MeasurementValidNum;
    kf->R.numRows  = kf->MeasurementValidNum;
    kf->R.numCols  = kf->MeasurementValidNum;
    kf->K.numRows  = kf->xhatSize;
    kf->K.numCols  = kf->MeasurementValidNum;
    kf->z.numRows  = kf->MeasurementValidNum;
}

/* ---------------------------------------- end of file ----------------------------------------- */
