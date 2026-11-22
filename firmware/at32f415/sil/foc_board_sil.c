/**
 * @file    foc_board_sil.c
 * @brief   Host-only Board1 software-in-the-loop device over binary stdio.
 * @version v0.1.0
 * @date    2026.11.21
 *
 * Implementation role
 * -------------------
 * Run the production Board1 control path against a virtual motor. The same
 * functions the AT32 IRQ handlers call in main.c run here in the same order:
 *
 *   TMR1 CH4 (slot + 40 us)  -> foc_board_adc_begin + foc_board_adc_publish
 *   ADC1 PCCE                -> foc_board_adc_complete (H0) + publish
 *   TMR5 OVF/C1/SPI RX/C2    -> foc_board_encoder_begin, foc_board_spi_on_time,
 *                               foc_board_spi_close_result, foc_board_encoder_complete (H1)
 *   Foreground loop          -> foc_runtime_foreground_poll + foc_serial_poll
 *
 * Boot uses foc_board_boot_select with the linked build supply, exactly as
 * main.c. Only register access is replaced: ADC ranks and the encoder word
 * come from the plant, and a validated output request reaches the plant
 * through foc_board_output_attach. The plant keeps its own physical facts
 * (sensor offsets, gains, rotor offset); the firmware never reads them.
 *
 * Time runs freely (scaled wall clock) or in lockstep after each received
 * frame. stdout carries protocol bytes only; FALCONF0C_META lines on stderr
 * carry virtual plant observations. Nothing here is physical evidence.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board.h"
#include "foc_board_boot.h"
#include "foc_board_timing.h"
#include "foc_serial.h"
#include "foc_virtual_motor.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <poll.h>
#include <time.h>
#include <unistd.h>
#endif

/* Private Macros --------------------------------------------------------------- */

#define SIL_TICKS_PER_US   ((uint32_t)FOC_BOARD_TIMER_HZ / (uint32_t)FOC_BOARD_US_HZ)
/* Plant-side Board1 hardware facts, kept apart from the firmware constants. */
#define SIL_ADC_MAX        4095.0f
#define SIL_ADC_FULL_SCALE 3.3f
#define SIL_VBUS_DIVIDER   11.0f
#define SIL_CSA_V_PER_A    0.5f
/* Conversion latency after the software trigger, and SPI word duration. */
#define SIL_ADC_CONVERT_US 4U
#define SIL_SPI_WORD_US    4U
/* Foreground loop service interval and plant integration substep. */
#define SIL_FOREGROUND_US  10U
#define SIL_PLANT_STEP_US  5U
#define SIL_H1_FIRST_US    1U

/* Private Types ---------------------------------------------------------------- */

enum sil_spi_phase {
    SIL_SPI_IDLE,
    SIL_SPI_SELECTED,
    SIL_SPI_SENT,
    SIL_SPI_RECEIVED,
    SIL_SPI_ERROR,
};

enum sil_drive {
    SIL_DRIVE_HIGH_Z,
    SIL_DRIVE_UPDATE,
};

struct sil_plant {
    struct foc_virtual_motor motor;
    struct foc_device_adc_config adc;
    struct foc_encoder_config encoder;
    float vbus_v;
    uint32_t time_us;
    enum sil_drive drive;
    struct foc_device_pwm_request active;
    struct foc_device_pwm_request pending;
    bool pending_valid;
    uint32_t applied_updates;
    uint32_t inhibits;
};

struct sil_device {
    /* Production instances, as the static objects in main.c. */
    struct foc_runtime runtime;
    struct foc_board_output output;
    struct foc_board_adc adc;
    struct foc_board_encoder encoder;
    struct foc_board_boot boot;
    struct foc_uart_buffer uart;
    struct foc_serial serial;
    /* Virtual peripherals. */
    struct sil_plant plant;
    uint32_t now_us;
    uint32_t last_read_us;
    uint32_t last_read_slot;
    bool adc_converting;
    uint32_t adc_done_us;
    uint16_t adc_ranks[4];
    uint32_t adc_sequence;
    /* Plant truth at the conversion aperture, and for the frame H0 last used. */
    double aperture_id_a;
    double aperture_iq_a;
    double sample_id_a;
    double sample_iq_a;
    enum sil_spi_phase spi_phase;
    uint16_t spi_code;
    uint16_t spi_word;
    uint32_t spi_rx_us;
    uint32_t tmr5_before_us;
    /* Reporting. */
    uint32_t trace_us;
    uint32_t next_trace_us;
    uint32_t lifecycle_sequence;
    enum foc_runtime_request_result lifecycle_result;
    bool eof;
};

/* Private Helpers -------------------------------------------------------------- */

static uint32_t sil_slot_index(uint32_t now_us)
{
    return now_us / (uint32_t)FOC_BOARD_H0_US;
}

/* Integrate the plant to now_us with the drive that was in effect. */
static int sil_plant_advance(struct sil_plant *plant, uint32_t now_us)
{
    while (plant->time_us < now_us) {
        uint32_t step = now_us - plant->time_us;
        if (step > SIL_PLANT_STEP_US) {
            step = SIL_PLANT_STEP_US;
        }
        const double dt_s = (double)step * 1.0e-6;
        const int status = plant->drive == SIL_DRIVE_UPDATE
            ? foc_virtual_motor_apply_duty(&plant->motor, &plant->active, plant->vbus_v, dt_s)
            : foc_virtual_motor_step(&plant->motor, 0.0, 0.0, dt_s);
        if (status != 0) {
            return status;
        }
        plant->time_us += step;
    }
    return 0;
}

/* Output backend: UPDATE loads at the next TMR1 update event (preload);
 * INHIBIT removes drive at once, as clearing MOE does. */
static bool sil_output_backend(void *argument, const struct foc_device_pwm_request *request)
{
    struct sil_device *device = argument;
    struct sil_plant *plant = &device->plant;
    if (request->action == FOC_DEVICE_OUTPUT_UPDATE) {
        plant->pending = *request;
        plant->pending_valid = true;
        return true;
    }
    if (request->action == FOC_DEVICE_OUTPUT_INHIBIT) {
        (void)sil_plant_advance(plant, device->now_us);
        plant->drive = SIL_DRIVE_HIGH_Z;
        plant->pending_valid = false;
        plant->inhibits++;
        return true;
    }
    return false;
}

/* Equivalent of foc_at32_adc_event(): sticky witnesses since the last read. */
static struct foc_board_adc_event sil_adc_event(struct sil_device *device)
{
    const uint32_t now = device->now_us;
    const uint32_t slot = sil_slot_index(now);
    unsigned int flags = 0U;
    if (now < device->last_read_us) {
        flags |= FOC_BOARD_TIME_WRAP;
    }
    if ((device->last_read_us & 0x80000000U) == 0U && (now & 0x80000000U) != 0U) {
        flags |= FOC_BOARD_TIME_HALF;
    }
    if (slot != device->last_read_slot) {
        flags |= FOC_BOARD_SLOT_WRAP;
    }
    device->last_read_us = now;
    device->last_read_slot = slot;
    return (struct foc_board_adc_event){
        .before_us = now,
        .now_us = now,
        .phase_ticks = (now - slot * (uint32_t)FOC_BOARD_H0_US) * SIL_TICKS_PER_US,
        .flags = flags,
        .coherent = true,
    };
}

static void sil_tmr1_update(struct sil_device *device)
{
    struct sil_plant *plant = &device->plant;
    if (plant->pending_valid) {
        plant->active = plant->pending;
        plant->pending_valid = false;
        plant->drive = SIL_DRIVE_UPDATE;
        plant->applied_updates++;
    }
}

/* TMR1_CH_IRQHandler: compare 4 starts one conversion. Ranks are sampled now. */
static void sil_tmr1_compare(struct sil_device *device)
{
    struct foc_board_adc_event event = sil_adc_event(device);
    /* No PCCE is ever left unserviced here, so none is pending. */
    if (foc_board_adc_begin(&device->runtime, &device->adc, &event, false)) {
        struct foc_device_adc_raw raw;
        if (foc_virtual_motor_make_adc(
                &device->plant.motor,
                &device->plant.adc,
                device->plant.vbus_v,
                device->adc_sequence++,
                device->now_us,
                &raw
            ) == 0) {
            device->adc_ranks[0] = raw.vbus_raw;
            device->adc_ranks[1] = raw.rank2_raw;
            device->adc_ranks[2] = raw.rank3_raw;
            device->adc_ranks[3] = raw.rank4_raw;
            struct foc_virtual_motor_observation truth;
            if (foc_virtual_motor_observe(&device->plant.motor, &truth) == 0) {
                device->aperture_id_a = truth.id_a;
                device->aperture_iq_a = truth.iq_a;
            }
            device->adc_converting = true;
            device->adc_done_us = device->now_us + SIL_ADC_CONVERT_US;
        }
    }
    foc_board_adc_publish(&device->adc);
}

/* ADC1_IRQHandler: PCCE retires the token and runs H0. */
static void sil_adc_complete(struct sil_device *device)
{
    device->adc_converting = false;
    struct foc_board_adc_event event = sil_adc_event(device);
    (void)foc_board_adc_complete(
        &device->runtime,
        &device->adc,
        &device->output,
        device->adc_ranks,
        4U,
        &event,
        true
    );
    device->sample_id_a = device->aperture_id_a;
    device->sample_iq_a = device->aperture_iq_a;
    foc_board_adc_publish(&device->adc);
}

/* foc_at32_spi_finish() */
static void
sil_spi_finish(struct sil_device *device, uint32_t phase_us, enum foc_board_encoder_result result)
{
    const uint32_t closed_us = device->now_us;
    if (result == FOC_BOARD_ENCODER_OK &&
        !foc_board_spi_on_time(FOC_BOARD_SPI_CLOSE, phase_us, device->tmr5_before_us, closed_us)) {
        result = FOC_BOARD_ENCODER_TIMEOUT;
    }
    device->spi_phase = SIL_SPI_IDLE;
    (void)foc_board_encoder_complete(
        &device->runtime,
        &device->encoder,
        closed_us,
        result,
        device->spi_word
    );
}

/* foc_at32_tmr5_service() for one edge at the given TMR5 phase. */
static void sil_tmr5(struct sil_device *device, uint32_t phase_us)
{
    const uint32_t now = device->now_us;
    if (phase_us == 0U) {
        device->tmr5_before_us = now;
        if (device->encoder.pending) {
            (void)foc_board_encoder_begin(&device->encoder, now);
            sil_spi_finish(device, phase_us, FOC_BOARD_ENCODER_OVERLAP);
            return;
        }
        if (foc_board_encoder_begin(&device->encoder, now)) {
            if (foc_board_spi_on_time(FOC_BOARD_SPI_CS, phase_us, now, now)) {
                /* Chip select falls: the sensor latches its angle here. */
                (void)sil_plant_advance(&device->plant, now);
                if (foc_virtual_motor_make_encoder_code(
                        &device->plant.motor,
                        &device->plant.encoder,
                        &device->spi_code
                    ) == 0) {
                    device->encoder.captured_us = now;
                    device->spi_phase = SIL_SPI_SELECTED;
                    return;
                }
            }
            sil_spi_finish(device, phase_us, FOC_BOARD_ENCODER_TIMEOUT);
        }
    } else if (phase_us == FOC_BOARD_SPI_SEND_US) {
        device->tmr5_before_us = now;
        if (device->spi_phase != SIL_SPI_SELECTED) {
            return;
        }
        device->spi_phase = foc_board_spi_on_time(FOC_BOARD_SPI_SEND, phase_us, now, now)
            ? SIL_SPI_SENT
            : SIL_SPI_ERROR;
    } else if (phase_us == FOC_BOARD_SPI_SEND_US + SIL_SPI_WORD_US) {
        /* SPI1_IRQHandler: one received word. */
        if (device->spi_phase == SIL_SPI_SENT) {
            device->spi_word = device->spi_code;
            device->spi_rx_us = now;
            device->spi_phase = SIL_SPI_RECEIVED;
        } else if (device->spi_phase == SIL_SPI_SELECTED) {
            device->spi_phase = SIL_SPI_ERROR;
        }
    } else if (phase_us == FOC_BOARD_SPI_CLOSE_US && device->encoder.pending) {
        device->tmr5_before_us = now;
        const enum foc_board_encoder_result result = foc_board_spi_close_result(
            phase_us,
            now,
            now,
            device->spi_rx_us,
            device->spi_phase == SIL_SPI_RECEIVED,
            false,
            device->spi_phase == SIL_SPI_ERROR
        );
        sil_spi_finish(device, phase_us, result);
    }
}

static void sil_drain_tx(struct sil_device *device)
{
    uint8_t buffer[256];
    size_t length = 0U;
    while (length < sizeof buffer && foc_uart_tx_pop(&device->uart, &buffer[length])) {
        ++length;
    }
    if (length != 0U) {
        (void)fwrite(buffer, 1U, length, stdout);
        (void)fflush(stdout);
    }
}

static uint32_t sil_serial_clock(void *argument)
{
    return ((const struct sil_device *)argument)->now_us;
}

static const char *sil_lifecycle_name(enum foc_runtime_request_result result)
{
    return result == FOC_RUNTIME_REQUEST_PENDING  ? "PENDING"
        : result == FOC_RUNTIME_REQUEST_APPLIED   ? "APPLIED"
        : result == FOC_RUNTIME_REQUEST_REJECTED  ? "REJECTED"
        : result == FOC_RUNTIME_REQUEST_CANCELLED ? "CANCELLED"
                                                  : "NONE";
}

static void sil_report(struct sil_device *device)
{
    struct foc_runtime_lifecycle_result lifecycle;
    if (foc_serial_read_lifecycle_result(&device->serial, &device->runtime, &lifecycle) ==
            FOC_RUNTIME_OK &&
        (lifecycle.request_sequence != device->lifecycle_sequence ||
         lifecycle.result != device->lifecycle_result)) {
        device->lifecycle_sequence = lifecycle.request_sequence;
        device->lifecycle_result = lifecycle.result;
        fprintf(
            stderr,
            "FALCONF0C_META {\"kind\":\"lifecycle\",\"result\":\"%s\",\"request_sequence\":%u,"
            "\"action\":%d,\"state\":%d,\"faults\":%u,\"h0_us\":%u,\"pipeline_status\":%d}\n",
            sil_lifecycle_name(lifecycle.result),
            lifecycle.request_sequence,
            lifecycle.action,
            lifecycle.state,
            lifecycle.faults,
            lifecycle.h0_us,
            lifecycle.pipeline_status
        );
    }
    if (device->trace_us == 0U || device->now_us < device->next_trace_us) {
        return;
    }
    device->next_trace_us = device->now_us + device->trace_us;
    struct foc_runtime_snapshot view;
    if (foc_runtime_read_snapshot(&device->runtime, &view) != FOC_RUNTIME_OK) {
        return;
    }
    struct foc_virtual_motor_observation plant;
    (void)sil_plant_advance(&device->plant, device->now_us);
    if (foc_virtual_motor_observe(&device->plant.motor, &plant) != 0) {
        return;
    }
    const struct foc_board_adc_diagnostics *adc = &device->adc.live;
    fprintf(
        stderr,
        "FALCONF0C_META {\"kind\":\"sil\",\"simulated\":true,\"physical_applied\":false,"
        "\"virtual_time_us\":%u,\"state\":%d,\"mode\":%d,\"faults\":%u,"
        "\"plant_id_a\":%.9g,\"plant_iq_a\":%.9g,\"plant_velocity_rad_s\":%.9g,"
        "\"plant_position_rad\":%.9g,\"sample_plant_id_a\":%.9g,\"sample_plant_iq_a\":%.9g,"
        "\"firmware_id_a\":%.9g,\"firmware_iq_a\":%.9g,"
        "\"firmware_current_valid\":%s,\"firmware_position_rad\":%.9g,"
        "\"firmware_velocity_rad_s\":%.9g,\"drive\":\"%s\",\"board_applied\":%u,"
        "\"board_declined\":%u,\"plant_loaded\":%u,\"inhibits\":%u,"
        "\"adc_observed_slots\":%u,\"adc_missing_slots\":%u,\"adc_h0_consumed\":%u,"
        "\"adc_timing_rejections\":%u,\"encoder_seq\":%u,\"encoder_losses\":%u}\n",
        device->now_us,
        view.state,
        view.mode,
        view.faults,
        plant.id_a,
        plant.iq_a,
        plant.mechanical_velocity_rad_s,
        plant.mechanical_position_rad,
        device->sample_id_a,
        device->sample_iq_a,
        view.measured_id_a,
        view.measured_iq_a,
        view.current_valid ? "true" : "false",
        view.mechanical_rad,
        view.mechanical_rad_s,
        device->plant.drive == SIL_DRIVE_UPDATE ? "UPDATE" : "HIGH_Z",
        device->output.applied_updates,
        device->output.declined_updates,
        device->plant.applied_updates,
        device->plant.inhibits,
        adc->observed_slots,
        adc->missing_slots,
        adc->h0_consumed,
        adc->timing_rejections,
        device->encoder.seq,
        device->encoder.losses
    );
}

/* main.c foreground loop body. */
static void sil_foreground(struct sil_device *device)
{
    struct foc_runtime_foreground_result result;
    (void)foc_runtime_foreground_poll(&device->runtime, device->now_us, 0U, 0U, &result);
    foc_serial_poll(&device->serial, &device->uart, &device->runtime, sil_serial_clock, device);
    sil_drain_tx(device);
    sil_report(device);
}

/* Advance virtual time one microsecond at a time, firing due events in IRQ priority order. */
static int sil_advance_to(struct sil_device *device, uint32_t target_us)
{
    while (device->now_us < target_us) {
        const uint32_t now = ++device->now_us;
        const uint32_t slot_phase = now % (uint32_t)FOC_BOARD_H0_US;
        if (slot_phase == 0U) {
            if (sil_plant_advance(&device->plant, now) != 0) {
                return -1;
            }
            sil_tmr1_update(device);
        }
        if (slot_phase == (uint32_t)FOC_BOARD_ADC_COMPARE_US) {
            if (sil_plant_advance(&device->plant, now) != 0) {
                return -1;
            }
            sil_tmr1_compare(device);
        }
        if (device->adc_converting && now == device->adc_done_us) {
            sil_adc_complete(device);
        }
        if (now >= SIL_H1_FIRST_US) {
            const uint32_t phase = (now - SIL_H1_FIRST_US) % (uint32_t)FOC_BOARD_H1_US;
            if (phase == 0U || phase == FOC_BOARD_SPI_SEND_US ||
                phase == FOC_BOARD_SPI_SEND_US + SIL_SPI_WORD_US ||
                phase == FOC_BOARD_SPI_CLOSE_US) {
                sil_tmr5(device, phase);
            }
        }
        if (now % SIL_FOREGROUND_US == 0U) {
            sil_foreground(device);
        }
    }
    return sil_plant_advance(&device->plant, device->now_us);
}

/* Host I/O --------------------------------------------------------------------- */

static double sil_wall_s(void)
{
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec * 1.0e-9;
#endif
}

static void sil_sleep_ms(unsigned int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec delay = {.tv_sec = 0, .tv_nsec = (long)ms * 1000000L};
    nanosleep(&delay, NULL);
#endif
}

/* Move every byte already waiting on stdin into the USART RX queue. */
static void sil_read_available(struct sil_device *device)
{
    uint8_t buffer[256];
    for (;;) {
        size_t length = 0U;
#ifdef _WIN32
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        DWORD available = 0;
        if (!PeekNamedPipe(input, NULL, 0, NULL, &available, NULL)) {
            device->eof = true;
            return;
        }
        if (available == 0) {
            return;
        }
        DWORD received = 0;
        const DWORD wanted = available < sizeof buffer ? available : (DWORD)sizeof buffer;
        if (!ReadFile(input, buffer, wanted, &received, NULL) || received == 0) {
            device->eof = true;
            return;
        }
        length = received;
#else
        struct pollfd ready = {.fd = STDIN_FILENO, .events = POLLIN};
        if (poll(&ready, 1, 0) <= 0) {
            return;
        }
        const ssize_t received = read(STDIN_FILENO, buffer, sizeof buffer);
        if (received <= 0) {
            device->eof = true;
            return;
        }
        length = (size_t)received;
#endif
        for (size_t index = 0U; index < length; ++index) {
            foc_uart_rx_push(&device->uart, buffer[index], false);
        }
    }
}

/* Plant defaults match the Board1 nominal rotor (7 pole pairs, zero offset). */
static struct foc_virtual_motor_params sil_default_motor(void)
{
    return (struct foc_virtual_motor_params){
        .rs_ohm = 0.6,
        .ld_h = 0.0012,
        .lq_h = 0.0015,
        .psi_f_wb = 0.01,
        .pole_pairs = 7U,
        .electrical_offset_rad = 0.0,
        .inertia_kg_m2 = 0.002,
        .damping_nm_s = 0.006,
    };
}

static int sil_parse_offsets(const char *text, float offsets[3])
{
    const char *cursor = text;
    for (unsigned int index = 0U; index < 3U; ++index) {
        char *end = NULL;
        const double value = strtod(cursor, &end);
        if (end == cursor || !isfinite(value) || *end != (index < 2U ? ',' : 0)) {
            return -1;
        }
        offsets[index] = (float)value;
        cursor = end + 1;
    }
    return 0;
}

static void sil_usage(const char *name)
{
    fprintf(
        stderr,
        "usage: %s [--speed factor | --lockstep-us us] [--duration-us us] [--trace-us us]\n"
        "          [--adc-offset a,b,c] [--vbus-v v] [--motor-rs-ohm r] [--motor-ld-h l]\n"
        "          [--motor-lq-h l] [--motor-psi-f-wb psi] [--motor-pole-pairs n]\n"
        "          [--motor-offset-rad rad] [--motor-inertia j] [--motor-damping b]\n",
        name
    );
}

/* Public Functions ------------------------------------------------------------- */

int main(int argc, char **argv)
{
#ifdef _WIN32
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
    static struct sil_device device;
    double speed = 0.02;
    uint32_t lockstep_us = 0U;
    uint32_t duration_us = 0U;
    float offsets[3] = {2048.0f, 2048.0f, 2048.0f};
    float vbus_v = 12.0f;
    struct foc_virtual_motor_params motor = sil_default_motor();
    device.trace_us = 500U;

    for (int index = 1; index < argc; ++index) {
        const char *option = argv[index];
        const char *value = index + 1 < argc ? argv[index + 1] : NULL;
        if (value == NULL) {
            sil_usage(argv[0]);
            return 2;
        }
        ++index;
        if (strcmp(option, "--speed") == 0) {
            speed = strtod(value, NULL);
        } else if (strcmp(option, "--lockstep-us") == 0) {
            lockstep_us = (uint32_t)strtoul(value, NULL, 0);
        } else if (strcmp(option, "--duration-us") == 0) {
            duration_us = (uint32_t)strtoul(value, NULL, 0);
        } else if (strcmp(option, "--trace-us") == 0) {
            device.trace_us = (uint32_t)strtoul(value, NULL, 0);
        } else if (strcmp(option, "--adc-offset") == 0) {
            if (sil_parse_offsets(value, offsets) != 0) {
                sil_usage(argv[0]);
                return 2;
            }
        } else if (strcmp(option, "--vbus-v") == 0) {
            vbus_v = (float)strtod(value, NULL);
        } else if (strcmp(option, "--motor-rs-ohm") == 0) {
            motor.rs_ohm = strtod(value, NULL);
        } else if (strcmp(option, "--motor-ld-h") == 0) {
            motor.ld_h = strtod(value, NULL);
        } else if (strcmp(option, "--motor-lq-h") == 0) {
            motor.lq_h = strtod(value, NULL);
        } else if (strcmp(option, "--motor-psi-f-wb") == 0) {
            motor.psi_f_wb = strtod(value, NULL);
        } else if (strcmp(option, "--motor-pole-pairs") == 0) {
            motor.pole_pairs = (unsigned int)strtoul(value, NULL, 0);
        } else if (strcmp(option, "--motor-offset-rad") == 0) {
            motor.electrical_offset_rad = strtod(value, NULL);
        } else if (strcmp(option, "--motor-inertia") == 0) {
            motor.inertia_kg_m2 = strtod(value, NULL);
        } else if (strcmp(option, "--motor-damping") == 0) {
            motor.damping_nm_s = strtod(value, NULL);
        } else {
            sil_usage(argv[0]);
            return 2;
        }
    }
    if (!(speed > 0.0) || !isfinite(speed) || !(vbus_v > 0.0f)) {
        sil_usage(argv[0]);
        return 2;
    }

    /* Plant: its own sensor facts. The firmware decodes with its own constants. */
    struct sil_plant *plant = &device.plant;
    plant->vbus_v = vbus_v;
    plant->adc = (struct foc_device_adc_config){
        .vbus_v_per_count = SIL_ADC_FULL_SCALE * SIL_VBUS_DIVIDER / SIL_ADC_MAX,
        .current_a_per_count = SIL_ADC_FULL_SCALE / (SIL_ADC_MAX * SIL_CSA_V_PER_A),
        .vbus_min_v = 1.0f,
        .phase_a_offset_count = offsets[0],
        .phase_b_offset_count = offsets[1],
        .phase_c_offset_count = offsets[2],
        .phase_order = FOC_DEVICE_PHASE_ORDER_ABC,
    };
    /* Raw sensor mounting: 16-bit word, reversed against the rotor (Board1). */
    plant->encoder = (struct foc_encoder_config){
        .counts_per_revolution = 65536U,
        .direction = FOC_ENCODER_DIRECTION_REVERSED,
        .max_speed_rad_s = 1000.0f,
        .min_dt_us = 1U,
        .max_dt_us = 2500U,
        .speed_lpf_cutoff_hz = 80.0f,
        .tracker_alpha = 0.85f,
        .tracker_beta = 0.2f,
    };
    const struct foc_virtual_motor_state initial = {0};
    if (foc_virtual_motor_init(&plant->motor, &motor, &initial) != 0) {
        fprintf(stderr, "FALCONF0C_META {\"kind\":\"error\",\"reason\":\"plant parameters\"}\n");
        return 2;
    }

    /* main(): boot selection, then attachments, then producers. */
    const struct foc_board_calibration_expectation expected = foc_board_calibration_expectation();
    if (foc_board_boot_select(
            &device.boot,
            &device.runtime,
            &device.output,
            &device.encoder,
            foc_board_calibration_supply(),
            &expected,
            0U
        ) != FOC_RUNTIME_OK ||
        !foc_board_output_attach(&device.output, sil_output_backend, &device)) {
        fprintf(stderr, "FALCONF0C_META {\"kind\":\"error\",\"reason\":\"boot\"}\n");
        return 1;
    }
    foc_serial_init_with_policy(&device.serial, &device.runtime, FOC_SERIAL_TARGET_VIRTUAL_ONLY);
    foc_serial_attach_boot(&device.serial, foc_board_boot_serial_readback, &device.boot);
    if (!foc_board_adc_init_timing(&device.adc, 0U, 0U, 0U)) {
        fprintf(stderr, "FALCONF0C_META {\"kind\":\"error\",\"reason\":\"adc timing\"}\n");
        return 1;
    }
    fprintf(
        stderr,
        "FALCONF0C_META {\"kind\":\"sil_boot\",\"simulated\":true,\"physical_applied\":false,"
        "\"mode\":\"%s\",\"speed\":%.6g,\"lockstep_us\":%u,\"plant_pole_pairs\":%u,"
        "\"plant_offset_rad\":%.9g,\"plant_adc_offset\":[%.6g,%.6g,%.6g]}\n",
        lockstep_us != 0U ? "lockstep" : "free-run",
        speed,
        lockstep_us,
        motor.pole_pairs,
        motor.electrical_offset_rad,
        offsets[0],
        offsets[1],
        offsets[2]
    );

    if (lockstep_us != 0U) {
        /* Deterministic: time moves only after each complete received frame. */
        for (;;) {
            const int byte = getchar();
            if (byte == EOF) {
                break;
            }
            foc_uart_rx_push(&device.uart, (uint8_t)byte, false);
            if (byte == 0 && sil_advance_to(&device, device.now_us + lockstep_us) != 0) {
                return 1;
            }
            if (duration_us != 0U && device.now_us >= duration_us) {
                break;
            }
        }
        return ferror(stdin) ? 1 : 0;
    }

    /* Free-run: virtual time follows the wall clock scaled by speed. */
    const double start_s = sil_wall_s();
    while (!device.eof && (duration_us == 0U || device.now_us < duration_us)) {
        sil_read_available(&device);
        const double target = (sil_wall_s() - start_s) * speed * 1.0e6;
        uint32_t target_us = target >= 4294967295.0 ? UINT32_MAX : (uint32_t)target;
        if (duration_us != 0U && target_us > duration_us) {
            target_us = duration_us;
        }
        if (target_us > device.now_us) {
            /* Short chunks keep received bytes close to their arrival time. */
            const uint32_t chunk =
                device.now_us + 100U < target_us ? device.now_us + 100U : target_us;
            if (sil_advance_to(&device, chunk) != 0) {
                return 1;
            }
        } else {
            sil_sleep_ms(1U);
        }
    }
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
