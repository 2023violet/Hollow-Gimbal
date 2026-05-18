/**
 * @file ntc_sensor.c
 * @brief  NTC Thermistor Lookup Table & Conversion
 * @version v1.0.0
 * @date 2025.12.14
 * 
 * Copyright (c) 2025 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

#include "ntc_sensor.h"

/** 
 * NTC: 10k B3950
 * Circuit: Pull-up 10k to 3.3V
 * Range: -20°C (Index 0) to 105°C (Index 125)
 * Step: 1°C
 * Order: Descending ADC values
 */
static const uint16_t _ntc_table[] = {
    3738, 3719, 3698, 3677, 3655, 3631, 3607, 3582, 3556, 3530, /* -20..-11 */
    3502, 3473, 3443, 3412, 3381, 3348, 3314, 3280, 3244, 3208, /* -10..-1 */
    3170, 3132, 3093, 3053, 3012, 2970, 2928, 2885, 2842, 2797, /* 0..9 */
    2752, 2707, 2661, 2615, 2568, 2522, 2474, 2427, 2379, 2332, /* 10..19 */
    2284, 2237, 2189, 2142, 2095, 2048, 2001, 1954, 1908, 1863, /* 20..29 */
    1818, 1773, 1729, 1685, 1642, 1600, 1558, 1517, 1477, 1437, /* 30..39 */
    1398, 1360, 1323, 1286, 1250, 1215, 1180, 1147, 1114, 1082, /* 40..49 */
    1051, 1020, 990,  961,  933,  905,  878,  852,  827,  802,  /* 50..59 */
    778,  755,  732,  710,  688,  668,  647,  628,  609,  590,  /* 60..69 */
    572,  555,  538,  522,  506,  491,  476,  461,  447,  434,  /* 70..79 */
    421,  408,  395,  384,  372,  361,  350,  340,  330,  320,  /* 80..89 */
    311,  302,  293,  284,  276,  268,  261,  253,  246,  239,  /* 90..99 */
    233,  226,  220,  214,  209,  203                           /* 100..105 */
};

#define TABLE_SIZE  (sizeof(_ntc_table) / sizeof(_ntc_table[0]))
#define TEMP_OFFSET (-20) /* Temperature at index 0 */

float ntc_to_temp(uint16_t adc_val)
{
    /* 1. Boundary Checks */
    /* Table is descending: _ntc_table[0] is Max ADC (Min Temp) */
    if (adc_val >= _ntc_table[0])
        return (float)TEMP_OFFSET;
    if (adc_val <= _ntc_table[TABLE_SIZE - 1])
        return (float)(TEMP_OFFSET + TABLE_SIZE - 1);

    /* 2. Binary Search (O(log N)) */
    /* We are looking for the interval [val_low, val_high] that contains adc_val */
    int low  = 0;
    int high = TABLE_SIZE - 1;
    int mid;

    while (high - low > 1) {
        mid = (low + high) >> 1;
        /* Note: Values are descending */
        if (adc_val <= _ntc_table[mid]) {
            /* Target is in the right half (lower ADC values / higher temps) */
            low = mid;
        } else {
            /* Target is in the left half (higher ADC values / lower temps) */
            high = mid;
        }
    }

    /* 3. Linear Interpolation */
    /* Found interval: _ntc_table[low] >= adc_val >= _ntc_table[high] */
    /* Indices are: low and (low + 1) */

    uint16_t adc0 = _ntc_table[low];     /* Higher ADC value */
    uint16_t adc1 = _ntc_table[low + 1]; /* Lower ADC value */

    /* Calculate fraction (0.0 to 1.0) */
    /* Since ADC drops as temp increases, closer to adc1 means higher temp */
    float frac = (float)(adc0 - adc_val) / (float)(adc0 - adc1);

    /* Base temperature */
    float temp_base = (float)(low + TEMP_OFFSET);

    return temp_base + frac;
}
