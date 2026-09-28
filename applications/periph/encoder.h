/**
 * @file    encoder.h
 * @brief   KTH7823 Magnetic Encoder Driver with PLL Estimator
 * @version v1.1.0  // Updated to use external PLL
 * @date    2025.12.23
 *
 * Copyright (c) 2025 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "tm_common.h"
#include "pll_enc.h"
#include "foc_config.h"
#include "wk_gpio.h"
#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
#include "encoder_bench_trace.h"
#endif
#if ENCODER_SPI_DMA_ENABLE
#include "wk_dma.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Data Structures ------------------------------------------------------------------------------ */

/**
 * @brief Encoder Hardware Configuration (ROM)
 */
struct encoder_config {
    /* Hardware Interface */
    spi_type  *spi_instance; /* AT32 SPI Peripheral (e.g. SPI1) */
    gpio_type *cs_port;      /* Chip Select Port */
    uint16_t   cs_pin;       /* Chip Select Pin */

    /* Physical Properties */
    uint8_t dir;         /* Mounting dir (CW/CCW) */
    float   mech_offset; /* Mechanical Zero Offset (rad) */
    float   pole_pairs;  /* Motor Pole Pairs */

    /* PLL Parameters */
    float pll_bandwidth;     /* Bandwidth for velocity estimation (suggest 100-500 Hz) */
    float vel_lpf_bandwidth; /* Bandwidth for velocity low-pass filter (suggest 1-5 Hz) */
};

/**
 * @brief Encoder State (RAM)
 * @note  PLL state moved to external PLL instance
 */
struct encoder_pll_state {
    /* Raw Data */
    uint16_t raw_value;   /* Current raw SPI value (0-65535) */
    bool     calib_valid; /* Calibration valid flag */

    /* LUT for nonlinearity compensation */
    float   enc_lut;
    int32_t enc_table[ENCODER_LUT_SIZE]; /* Lookup table */

    /* Output Cache (from PLL) */
    float angle_mech_rad; /* Mechanical Angle (0 - 2PI) */
    float angle_elec_rad; /* Electrical Angle (0 - 2PI) */
    float velocity_rads;  /* Velocity (rad/s) */
    float total_enc;      /* Total Encoder Count (interpolated, including turns) */
};

/**
 * @brief Encoder Object Instance (RAM)
 */
struct encoder_dev {
    struct encoder_config   *config;      /* Hardware Config */
    struct encoder_pll_state state;       /* Internal State */
    struct pll_enc_dev       pll_enc_ins; /* External PLL instance */
    struct pll_enc_config    pll_enc_cfg; /* Permanent storage for PLL config */

    /* Runtime Flags */
    bool initialized;
    bool spi_error;

#if ENCODER_SPI_DMA_ENABLE
    uint16_t dma_rx_buffer; /* DMA receive buffer (16-bit raw angle) */
    uint16_t dma_tx_buffer; /* DMA transmit buffer (dummy = 0x0000) */
    bool     dma_active;    /* DMA transfer in-progress flag */
#endif
};

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief  Initialize the encoder driver
 * 
 * @param  enc   Encoder instance
 * @param  cfg   Configuration
 */
void encoder_init(struct encoder_dev *enc, struct encoder_config *cfg);

/**
 * @brief  Update encoder state (Call in High-Freq Loop)
 * @note   Performs SPI transaction + LUT correction + PLL update
 */
void encoder_update(struct encoder_dev *enc);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
