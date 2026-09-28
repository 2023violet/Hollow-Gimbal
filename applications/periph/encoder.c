/**
 * @file    encoder.c
 * @brief   KTH7823 Magnetic Encoder Driver with PLL Estimator
 * @version v1.3.0
 * @date    2026.02.22
 *
 * Revision History
 * ──────────────────
 * v1.3.0 - Update: pll_enc_update() call no longer passes dt argument.
 *          dt is now precomputed internally by the PLL in pll_enc_init(),
 *          matching the change in pll_enc v3.2.0 (Opt #5).
 *
 * v1.2.0 - Fix #1: encoder_init: sample_rate_hz now correctly assigned as
 *          _PLL_SAMPLE_RATE (Hz), not _PLL_TS (seconds). Wrong unit caused
 *          the velocity LPF alpha to overflow to 1.0, disabling all filtering.
 *
 *        - Fix #2: encoder_update: pll_enc_get_angle_elec_rad now receives
 *          enc->config->mech_offset. Without this, the calibrated encoder
 *          zero-offset had no effect on the electrical angle fed to the FOC
 *          current loop, causing the motor to be unable to rotate after
 *          encoder calibration (Park transform frame misaligned with rotor flux).
 *
 * Copyright (c) 2026 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "encoder.h"
#include <string.h>

/* Private constants ---------------------------------------------------------------------------- */

/** Dummy SPI transmit word to clock out encoder data (KTH7823 ignores TX content) */
#define _ENCODER_READ_CMD (0x0000U)

/** PWM switching period (seconds), e.g. 50e-6 at 20 kHz */
#define _PLL_TS           (PWM_DT)

/** PLL / encoder update rate (Hz). encoder_update runs in the 5 kHz
 *  motor soft-IRQ (TMR5), not the 20 kHz ADC ISR, on soft-float SoCs. */
#define _PLL_SAMPLE_RATE  (VEL_LOOP_FREQ_HZ)

/* Private functions ---------------------------------------------------------------------------- */

#if ENCODER_SPI_DMA_ENABLE

static void _encoder_dma_start(struct encoder_dev *enc)
{
    spi_type *spi = enc->config->spi_instance;

    enc->dma_tx_buffer = _ENCODER_READ_CMD;

    dma_channel_enable(DMA1_CHANNEL3, FALSE);
    dma_channel_enable(DMA1_CHANNEL4, FALSE);

    dma_flag_clear(DMA1_FDT3_FLAG);
    dma_flag_clear(DMA1_FDT4_FLAG);

    wk_dma_channel_config(DMA1_CHANNEL3, (uint32_t)&spi->dt, (uint32_t)&enc->dma_rx_buffer, 1);

    wk_dma_channel_config(DMA1_CHANNEL4, (uint32_t)&spi->dt, (uint32_t)&enc->dma_tx_buffer, 1);

    dma_channel_enable(DMA1_CHANNEL3, TRUE);
    dma_channel_enable(DMA1_CHANNEL4, TRUE);

    enc->dma_active = true;
}

static bool _encoder_dma_finish(struct encoder_dev *enc, uint16_t *data)
{
    while (dma_flag_get(DMA1_FDT3_FLAG) == RESET || dma_flag_get(DMA1_FDT4_FLAG) == RESET) {
    }

    dma_flag_clear(DMA1_FDT3_FLAG);
    dma_flag_clear(DMA1_FDT4_FLAG);

    dma_channel_enable(DMA1_CHANNEL3, FALSE);
    dma_channel_enable(DMA1_CHANNEL4, FALSE);

    enc->dma_active = false;

    *data = enc->dma_rx_buffer;

    if (*data == 0x0000U || *data == 0xFFFFU) {
        return false;
    }
    return true;
}

#else

static bool _encoder_read_raw(struct encoder_dev *enc, uint16_t *data)
{
    gpio_bits_reset(enc->config->cs_port, enc->config->cs_pin);

    while (spi_i2s_flag_get(enc->config->spi_instance, SPI_I2S_TDBE_FLAG) == RESET)
        ;
    spi_i2s_data_transmit(enc->config->spi_instance, _ENCODER_READ_CMD);

    while (spi_i2s_flag_get(enc->config->spi_instance, SPI_I2S_RDBF_FLAG) == RESET)
        ;
    *data = spi_i2s_data_receive(enc->config->spi_instance);

    gpio_bits_set(enc->config->cs_port, enc->config->cs_pin);

    if (*data == 0x0000U || *data == 0xFFFFU) {
        return false;
    }
    return true;
}

#endif

#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
static void _encoder_bench_trace_update(const struct encoder_dev *enc,
                                        int32_t corrected_count,
                                        uint8_t error_flags)
{
    struct encoder_bench_input input;

    if (enc == NULL) {
        return;
    }

    input.raw_word = enc->state.raw_value;
    input.angle_mech_rad = enc->state.angle_mech_rad;
    input.angle_elec_rad = enc->state.angle_elec_rad;
    input.velocity_rads = enc->state.velocity_rads;
    input.total_enc = enc->state.total_enc;
    encoder_bench_trace_update(&input, corrected_count, error_flags);
}
#endif

/* Public functions ----------------------------------------------------------------------------- */

/**
 * @brief  Initialize the encoder driver and the internal PLL estimator.
 *
 * @param  enc  Encoder instance
 * @param  cfg  Hardware and algorithm configuration (must remain valid forever)
 */
void encoder_init(struct encoder_dev *enc, struct encoder_config *cfg)
{
    if (!enc || !cfg) {
        return;
    }

    enc->config = cfg;

    /* Clear runtime state and LUT */
    memset(&enc->state, 0, sizeof(struct encoder_pll_state));
    memset(enc->state.enc_table, 0, sizeof(enc->state.enc_table));

#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
    encoder_bench_trace_clear();
#endif

#if ENCODER_SPI_DMA_ENABLE
    enc->dma_rx_buffer = 0x0000U;
    enc->dma_tx_buffer = 0x0000U;
    enc->dma_active    = false;
#endif

    /* Ensure CS is de-asserted at power-on */
    gpio_bits_set(enc->config->cs_port, enc->config->cs_pin);

    /* Force the CS pin to plain GPIO output.
     *
     * SPI1_GMUX_0001 maps PA15 as the SPI1 NSS (hardware) pin, which leaves it
     * in mux mode; with software CS (SWCSEN=1) the NSS pin function is not
     * needed, so we reconfigure it as a push-pull GPIO output so the encoder
     * chip-select can actually be driven. */
    {
        gpio_init_type gpio_init_struct;
        gpio_default_para_init(&gpio_init_struct);
        gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
        gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
        gpio_init_struct.gpio_mode = GPIO_MODE_OUTPUT;
        gpio_init_struct.gpio_pins = enc->config->cs_pin;
        gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
        gpio_init(enc->config->cs_port, &gpio_init_struct);
        gpio_bits_set(enc->config->cs_port, enc->config->cs_pin);
    }

    /* ── Configure PLL ────────────────────────────────────────────────────
     * sample_rate_hz MUST be in Hz (e.g. 20000.0f = 20 kHz).
     * pll_enc_init() uses this to precompute dt = 1/sample_rate_hz.
     *
     * Fix #1 (v1.2.0): was incorrectly passing _PLL_TS (seconds) here,
     * which caused lpf_init() to compute alpha > 1.0, disabling the LPF. */
    enc->pll_enc_cfg.cpr               = (float)ENCODER_CPR;
    enc->pll_enc_cfg.pll_bandwidth     = enc->config->pll_bandwidth;
    enc->pll_enc_cfg.vel_lpf_bandwidth = enc->config->vel_lpf_bandwidth;
    // enc->pll_enc_cfg.interp_lpf_bandwidth = ENCODER_INTERP_LPF_BW_HZ;
    enc->pll_enc_cfg.snap_factor       = ENCODER_SNAP_FACTOR;
    enc->pll_enc_cfg.sample_rate_hz    = _PLL_SAMPLE_RATE; /* Hz — NOT seconds */

    /* Initialize PLL (precomputes kp, ki, snap_thr, dt internally) */
    pll_enc_init(&enc->pll_enc_ins, &enc->pll_enc_cfg);

#if ENCODER_SPI_DMA_ENABLE
    gpio_bits_reset(enc->config->cs_port, enc->config->cs_pin);
    _encoder_dma_start(enc);
#endif

    enc->initialized = true;
}

/**
 * @brief  Main encoder update — read → direction correct → LUT compensate → PLL update.
 *
 * Call at 20 kHz from the ADC ISR, before foc_para_calc().
 * On return, enc->state.(angle_mech_rad, angle_elec_rad, velocity_rads, total_enc)
 * all contain fresh values derived from the latest PLL estimate.
 *
 * [ISR-SAFE] — no RTOS primitives, no dynamic allocation.
 */
void encoder_update(struct encoder_dev *enc)
{
    if (enc == NULL || enc->config == NULL || !enc->initialized) {
        return;
    }

    /* ── Step 1: SPI read ───────────────────────────────────────────────── */
#if ENCODER_SPI_DMA_ENABLE
    gpio_bits_set(enc->config->cs_port, enc->config->cs_pin);

    if (_encoder_dma_finish(enc, &enc->state.raw_value)) {
        enc->spi_error = false;
    } else {
        enc->spi_error = true;
        gpio_bits_reset(enc->config->cs_port, enc->config->cs_pin);
        _encoder_dma_start(enc);
#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
        _encoder_bench_trace_update(enc, -1, ENCODER_BENCH_TRACE_ERROR_SPI);
#endif
        return;
    }
#else
    if (_encoder_read_raw(enc, &enc->state.raw_value)) {
        enc->spi_error = false;
    } else {
        enc->spi_error = true;
#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
        _encoder_bench_trace_update(enc, -1, ENCODER_BENCH_TRACE_ERROR_SPI);
#endif
        return;
    }
#endif

    /* ── Step 2: Direction correction ─────────────────────────────────────
     *   DIR_CW  → use raw count as-is (encoder positive = motor CW)
     *   DIR_CCW → mirror count so that increasing count always means CW    */
    int32_t raw_adjusted;
    if (enc->config->dir == DIR_CW) {
        raw_adjusted = (int32_t)enc->state.raw_value;
    } else {
        raw_adjusted = ENCODER_CPR - (int32_t)enc->state.raw_value;
    }

    /* Mask to [0, CPR-1]: handles the DIR_CCW edge case where raw_value==0
     * would otherwise produce raw_adjusted = ENCODER_CPR (out of range).   */
    raw_adjusted &= (ENCODER_CPR - 1);

    /* ── Step 3: LUT non-linearity compensation ────────────────────────────
     *
     * The LUT stores per-sector encoder error in counts (int32_t).
     * Interpolate between adjacent LUT entries for sub-sector accuracy.
     *
     * off_interp is ADDED (not subtracted) to raw_adjusted: the LUT contains
     * the correction to apply to bring the raw reading to the ideal position. */
    int32_t off_interp = 0;

    if (enc->state.calib_valid) {
        const uint8_t  bit_shift = ENCODER_BITS - ENCODER_LUT_BITS;
        const uint32_t idx       = (uint32_t)raw_adjusted >> bit_shift;
        const uint32_t next_idx  = (idx + 1U) % ENCODER_LUT_SIZE;
        const int32_t  off_1     = enc->state.enc_table[idx];
        const int32_t  off_2     = enc->state.enc_table[next_idx];
        const uint32_t frac      = (uint32_t)raw_adjusted & ((1U << bit_shift) - 1U);

        off_interp = off_1 + ((off_2 - off_1) * (int32_t)frac >> bit_shift);
    }

    /* Apply correction and wrap result into [0, CPR) */
    int32_t cnt = raw_adjusted + off_interp;

    if (cnt >= ENCODER_CPR) {
        cnt -= ENCODER_CPR;
    } else if (cnt < 0) {
        cnt += ENCODER_CPR;
    }

    /* ── Step 4: PLL update ────────────────────────────────────────────────
     *
     * v1.3.0: dt parameter removed — pll_enc_update() now uses its internally
     * precomputed dt (= 1/sample_rate_hz), eliminating any possible time-base
     * mismatch between the PLL and its velocity LPF.                         */
    if (pll_enc_update(&enc->pll_enc_ins, cnt) != 0) {
#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
        _encoder_bench_trace_update(enc, cnt, ENCODER_BENCH_TRACE_ERROR_PLL);
#endif
#if ENCODER_SPI_DMA_ENABLE
        gpio_bits_reset(enc->config->cs_port, enc->config->cs_pin);
        _encoder_dma_start(enc);
#endif
        return;
    }

    /* ── Step 5: Retrieve outputs from PLL ───────────────────────────────
     *
     * Fix #2 (v1.2.0): pll_enc_get_angle_elec_rad receives mech_offset.
     *
     * The electrical angle formula is:
     *     theta_elec = (theta_mech_raw − mech_offset) × pole_pairs
     *
     * mech_offset is the encoder-to-electrical-zero phase difference found
     * by foc_calib_enc and stored in enc->config->mech_offset.
     *
     * Before calibration: mech_offset == 0.0f → no visible error (OK).
     * After calibration:  mech_offset != 0.0f → must be applied; omitting
     *   it misaligns the Park frame with the rotor flux axis → motor stalls. */
    enc->state.angle_mech_rad =
        pll_enc_get_angle_mech_rad(&enc->pll_enc_ins, enc->config->mech_offset);

    enc->state.angle_elec_rad = pll_enc_get_angle_elec_rad(&enc->pll_enc_ins,
                                                           enc->config->mech_offset,
                                                           enc->config->pole_pairs);

    enc->state.velocity_rads = pll_enc_get_velocity_rads(&enc->pll_enc_ins);
    enc->state.total_enc     = pll_enc_get_total_enc(&enc->pll_enc_ins);

#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
    _encoder_bench_trace_update(enc, cnt, 0U);
#endif

#if ENCODER_SPI_DMA_ENABLE
    gpio_bits_reset(enc->config->cs_port, enc->config->cs_pin);
    _encoder_dma_start(enc);
#endif
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
