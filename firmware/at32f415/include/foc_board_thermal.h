/**
 * @file    foc_board_thermal.h
 * @brief   Board NTC polling: filtered temperatures, derating and trips for H0.
 * @version v0.1.0
 * @date    2026.11.25
 *
 * Design overview
 * ---------------
 * Foreground polls the FET and motor NTC channels every FOC_BOARD_THERMAL_PERIOD_US
 * through a thin reader port, filters and derates them with g3/safe foc_thermal
 * using the UART settings (IDs 138–148), writes the temperatures back for IDs 119
 * and 147, and publishes the larger derating share and the trip bits as atomics.
 * The ADC completion passes those to Runtime H0 in its fast input.
 *
 * Usage example
 * -------------
 * Initialize with the board's reader (NULL when the board has no NTC channel), set
 * foc_board_adc.thermal before producers start, then call foc_board_thermal_poll()
 * from every Foreground pass.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Foreground owns the filter state; the ADC IRQ only loads the two atomics.
 *
 * Failure / recovery
 * ------------------
 * An enabled channel without a usable reading (no reader, open or shorted sensor)
 * derates fully and trips after FOC_THERMAL_INVALID_SAMPLES polls. A disabled
 * channel reads NaN and neither derates nor trips.
 */
#pragma once

#include "foc_thermal.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros ---------------------------------------------------------------- */

/** Foreground NTC poll period in µs. */
#define FOC_BOARD_THERMAL_PERIOD_US 10000U
/** Per-stage temperature filter coefficient at that period (about 0.1 s per stage). */
#define FOC_BOARD_THERMAL_ALPHA     0.1f

/* Public Types ----------------------------------------------------------------- */

struct foc_serial;

/** NTC channels. */
enum foc_board_ntc {
    FOC_BOARD_NTC_FET = 0,
    FOC_BOARD_NTC_MOTOR = 1,
    FOC_BOARD_NTC_COUNT = 2,
};

/** Board port: the channel's ADC reading over full scale; false when unavailable. */
typedef bool (*foc_board_ntc_reader)(void *argument, enum foc_board_ntc channel, float *ratio);

struct foc_board_thermal {
    foc_board_ntc_reader read;
    void *argument;
    struct foc_thermal_sensor sensors[FOC_BOARD_NTC_COUNT];
    uint32_t last_us;
    bool started;
    /** Published share of the current to remove (float word) and FOC_SAFE_FAULT_* bits. */
    atomic_uint derate_word;
    atomic_uint faults;
};

/* Public Functions ------------------------------------------------------------- */

/** Reset with an optional reader; nothing derates or trips until the first poll. */
void foc_board_thermal_init(struct foc_board_thermal *thermal,
                            foc_board_ntc_reader read,
                            void *argument);

/** Foreground. Poll when due with the serial settings and publish the results. */
void foc_board_thermal_poll(struct foc_board_thermal *thermal,
                            struct foc_serial *serial,
                            uint32_t now_us);

/** Any context. Latest derating share, 0 none to 1 all. */
float foc_board_thermal_derate(struct foc_board_thermal *thermal);

/** Any context. Latest FOC_SAFE_FAULT_OVER_TEMPERATURE_* bits. */
uint32_t foc_board_thermal_faults(struct foc_board_thermal *thermal);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
