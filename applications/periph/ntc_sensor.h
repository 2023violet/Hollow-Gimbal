/**
 * @file ntc_sensor.h
 * @brief  NTC Thermistor Lookup Table & Conversion
 * @version v1.0.0
 * @date 2025.12.14
 * 
 * Copyright (c) 2025 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief  Convert ADC value (12-bit) to Temperature (Celsius)
 * 
 * @param  adc_val  Raw ADC value (0-4095)
 * 
 * @return Temperature in Celsius
 */
float ntc_to_temp(uint16_t adc_val);

#ifdef __cplusplus
}
#endif
