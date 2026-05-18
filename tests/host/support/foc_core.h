/**
 * @file    foc_core.h
 * @brief   Minimal host shim for FOC runtime variable struct
 * @version v1.0.0
 * @date    2026.05.14
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdint.h>

struct foc_vars {
    float   v_bus;
    float   inv_vbus;
    float   i_a;
    float   i_b;
    float   i_c;
    float   v_a;
    float   v_b;
    float   v_c;
    float   theta;
    float   sin_val;
    float   cos_val;
    float   i_alpha;
    float   i_beta;
    float   i_d;
    float   i_q;
    float   i_d_filter;
    float   i_q_filter;
    float   v_d;
    float   v_q;
    float   v_alpha;
    float   v_beta;
    float   dtc_a;
    float   dtc_b;
    float   dtc_c;
    uint8_t sector;
};

/* ---------------------------------------- end of file ----------------------------------------- */
