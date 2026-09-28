/**
 * @file    encoder.h
 * @brief   Minimal host shim for encoder persistence coverage
 * @version v1.0.0
 * @date    2026.05.14
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>
#include "foc_config.h"
#include "pll_enc.h"
#include "../../../applications/periph/encoder_bench_trace.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct spi_type_ {
    uint32_t reserved;
} spi_type;

typedef struct gpio_type_ {
    uint32_t reserved;
} gpio_type;

extern spi_type  g_host_spi1;
extern gpio_type g_host_gpioa;

#define SPI1        (&g_host_spi1)
#define GPIOA       (&g_host_gpioa)
#define GPIO_PINS_4 (4U)
#define GPIO_PINS_15 (15U)

struct encoder_config {
    spi_type  *spi_instance;
    gpio_type *cs_port;
    uint16_t   cs_pin;
    uint8_t    dir;
    float      mech_offset;
    float      pole_pairs;
    float      pll_bandwidth;
    float      vel_lpf_bandwidth;
};

struct encoder_pll_state {
    uint16_t raw_value;
    bool     calib_valid;
    float    enc_lut;
    int32_t  enc_table[ENCODER_LUT_SIZE];
    float    angle_mech_rad;
    float    angle_elec_rad;
    float    velocity_rads;
    float    total_enc;
};

struct encoder_dev {
    struct encoder_config   *config;
    struct encoder_pll_state state;
    struct pll_enc_dev       pll_enc_ins;
    struct pll_enc_config    pll_enc_cfg;
    bool                     initialized;
    bool                     spi_error;
};

void encoder_init(struct encoder_dev *enc, struct encoder_config *cfg);
void encoder_update(struct encoder_dev *enc);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
