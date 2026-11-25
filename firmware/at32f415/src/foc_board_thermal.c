/**
 * @file    foc_board_thermal.c
 * @brief   Board NTC polling through the reader port and g3/safe foc_thermal.
 * @version v0.1.0
 * @date    2026.11.25
 *
 * Implementation role
 * -------------------
 * - pairs with `foc_board_thermal.h`
 * - converts the UART thermal settings to foc_thermal configurations each poll
 * - publishes the derating share and trip bits for the ADC completion
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board_thermal.h"

#include "foc_safe.h"
#include "foc_serial.h"

#include <stddef.h>
#include <string.h>

/* Private Helpers -------------------------------------------------------------- */

static uint32_t thermal_word(float value)
{
    uint32_t word;
    memcpy(&word, &value, sizeof word);
    return word;
}

/* Public Functions ------------------------------------------------------------- */

void foc_board_thermal_init(
    struct foc_board_thermal *thermal,
    foc_board_ntc_reader read,
    void *argument
)
{
    if (thermal == NULL) {
        return;
    }
    thermal->read = read;
    thermal->argument = argument;
    for (unsigned int index = 0U; index < FOC_BOARD_NTC_COUNT; ++index) {
        foc_thermal_sensor_reset(&thermal->sensors[index]);
    }
    thermal->last_us = 0U;
    thermal->started = false;
    atomic_init(&thermal->derate_word, thermal_word(0.0f));
    atomic_init(&thermal->faults, 0U);
}

void foc_board_thermal_poll(
    struct foc_board_thermal *thermal,
    struct foc_serial *serial,
    uint32_t now_us
)
{
    if (thermal == NULL || serial == NULL ||
        (thermal->started && now_us - thermal->last_us < FOC_BOARD_THERMAL_PERIOD_US)) {
        return;
    }
    thermal->started = true;
    thermal->last_us = now_us;
    const struct foc_serial_thermal_settings *settings = &serial->thermal;
    const struct foc_thermal_ntc ntc = {
        settings->beta_k,
        settings->r25_ohm,
        settings->pullup_ohm,
        settings->topology == 1U ? FOC_THERMAL_NTC_TO_SUPPLY : FOC_THERMAL_NTC_TO_GROUND,
    };
    const struct foc_serial_thermal_sensor *channels[FOC_BOARD_NTC_COUNT] = {
        &settings->fet,
        &settings->motor,
    };
    static const uint32_t trips[FOC_BOARD_NTC_COUNT] = {
        FOC_SAFE_FAULT_OVER_TEMPERATURE_FET,
        FOC_SAFE_FAULT_OVER_TEMPERATURE_MOTOR,
    };
    float celsius[FOC_BOARD_NTC_COUNT];
    float derate = 0.0f;
    uint32_t faults = 0U;
    for (unsigned int index = 0U; index < FOC_BOARD_NTC_COUNT; ++index) {
        const struct foc_thermal_sensor_config config = {
            channels[index]->enabled != 0U,
            channels[index]->derate_c,
            channels[index]->limit_c,
        };
        float ratio = 0.0f;
        const bool sampled = config.enabled && thermal->read != NULL &&
            thermal->read(thermal->argument, (enum foc_board_ntc)index, &ratio);
        celsius[index] = foc_thermal_sensor_update(
            &config,
            &ntc,
            &thermal->sensors[index],
            sampled,
            ratio,
            FOC_BOARD_THERMAL_ALPHA
        );
        const float share = foc_thermal_derate(&config, celsius[index]);
        if (share > derate) {
            derate = share;
        }
        if (thermal->sensors[index].tripped) {
            faults |= trips[index];
        }
    }
    serial->temperature.fet_c = celsius[FOC_BOARD_NTC_FET];
    serial->temperature.motor_c = celsius[FOC_BOARD_NTC_MOTOR];
    atomic_store_explicit(&thermal->derate_word, thermal_word(derate), memory_order_relaxed);
    atomic_store_explicit(&thermal->faults, faults, memory_order_relaxed);
}

float foc_board_thermal_derate(struct foc_board_thermal *thermal)
{
    const uint32_t word = atomic_load_explicit(&thermal->derate_word, memory_order_relaxed);
    float value;
    memcpy(&value, &word, sizeof value);
    return value;
}

uint32_t foc_board_thermal_faults(struct foc_board_thermal *thermal)
{
    return atomic_load_explicit(&thermal->faults, memory_order_relaxed);
}

/* ---------------------------------------- end of file ----------------------------------------- */
