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
 *
 * With --can <serial>, stdin and stdout carry records instead of raw UART bytes:
 * channel u8, length u8, payload. Channel 0 is UART bytes, channel 1 one CAN frame
 * (id u16, flags u8 with bit 0 RTR, length u8, 8 data bytes), channel 2 a time
 * advance (u32 µs, lockstep only), acknowledged with a channel 2 record carrying the
 * virtual time. In lockstep, UART records advance time as before (per frame delimiter);
 * CAN records do not, so a Host can step several nodes alike.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board.h"
#include "foc_board_boot.h"
#include "foc_board_calibrate.h"
#include "foc_board_store.h"
#include "foc_board_thermal.h"
#include "foc_board_timing.h"
#include "foc_can.h"
#include "foc_serial.h"
#include "foc_virtual_bootstrap_instrument.h"
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
/* Multiplexed stdio record channels (--can). */
#define SIL_CHANNEL_UART   0U
#define SIL_CHANNEL_CAN    1U
#define SIL_CHANNEL_TICK   2U
#define SIL_CAN_RECORD     12U

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
    /* Constant external load torque from plant time load_on_us (0 never). */
    double load_torque_nm;
    /* Inverter dead-time and switch-drop error: each phase loses dead_time_v x sign(i). */
    double dead_time_v;
    uint32_t load_on_us;
    bool load_applied;
};

/* Two flash slots of erased 0xFF bytes; optionally mirrored to a file so that
 * restarting the process with the same file behaves as a power cycle. */
struct sil_flash {
    uint8_t slots[2][FOC_BOARD_STORE_SLOT_BYTES];
    const char *path;
    uint32_t erases;
    uint32_t programs;
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
    struct foc_board_persistence persistence;
    struct foc_board_calibration calibration;
    struct foc_can_buffer can_buffer;
    struct foc_can can;
    /* --can: multiplexed stdio records and the record being received. */
    bool mux;
    uint8_t record[2U + 255U];
    unsigned int record_have;
    /* A time-advance record was received: acknowledge it with the virtual time. */
    bool tick_ack;
    /* Virtual peripherals. */
    struct sil_flash flash;
    struct sil_plant plant;
    uint32_t now_us;
    uint32_t last_read_us;
    uint32_t last_read_slot;
    bool adc_converting;
    uint32_t adc_done_us;
    uint16_t adc_ranks[4];
    uint32_t adc_sequence;
    /* Optional phase-rank noise in counts rms (deterministic xorshift32). */
    double adc_noise_counts;
    uint32_t noise_state;
    /* Fault injection: the DRV8311 nFAULT line is low in [driver_fault_on_us, _off_us). */
    uint32_t driver_fault_on_us;
    uint32_t driver_fault_off_us;
    /* NTC plant: the winding follows ambient + Rth x 1.5 Rs i^2 with time constant tau, the
     * power stage half that rise at tau / 2. Both read through a 10 kOhm beta 3950 NTC to
     * ground with a 10 kOhm pull-up; in [override_on_us, _off_us) both read override_c. */
    struct foc_board_thermal thermal;
    double rs_ohm;
    double ambient_c;
    double rth_k_per_w;
    double thermal_tau_s;
    double motor_c;
    double fet_c;
    uint32_t thermal_us;
    uint32_t ntc_override_on_us;
    uint32_t ntc_override_off_us;
    double ntc_override_c;
    /* Fault injection: from adc_step_us on, add adc_step_counts to the first current rank. */
    uint32_t adc_step_us;
    double adc_step_counts;
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

/* Average phase voltages minus dead_time_v x sign(i) per phase (smoothed over 20 mA),
 * reduced to alpha/beta as foc_virtual_motor_apply_duty() does. */
static int sil_plant_step_dead_time(struct sil_plant *plant, double dt_s)
{
    struct foc_virtual_motor_observation seen;
    if (foc_virtual_motor_observe(&plant->motor, &seen) != 0) {
        return -1;
    }
    const double theta = seen.electrical_position_rad;
    const double i_alpha = cos(theta) * seen.id_a - sin(theta) * seen.iq_a;
    const double i_beta = sin(theta) * seen.id_a + cos(theta) * seen.iq_a;
    const double current[3] = {
        i_alpha,
        -0.5 * i_alpha + 0.8660254037844386 * i_beta,
        -0.5 * i_alpha - 0.8660254037844386 * i_beta,
    };
    const double duty[3] = {plant->active.duty_a, plant->active.duty_b, plant->active.duty_c};
    double v[3];
    for (unsigned int index = 0U; index < 3U; ++index) {
        v[index] = plant->vbus_v * duty[index] - plant->dead_time_v * tanh(current[index] / 0.02);
    }
    const double alpha = (2.0 * v[0] - v[1] - v[2]) / 3.0;
    const double beta = (v[1] - v[2]) / 1.7320508075688772;
    return foc_virtual_motor_step(&plant->motor, alpha, beta, dt_s);
}

/* Integrate the plant to now_us with the drive that was in effect. */
static int sil_plant_advance(struct sil_plant *plant, uint32_t now_us)
{
    while (plant->time_us < now_us) {
        if (!plant->load_applied && plant->load_torque_nm != 0.0 &&
            plant->time_us >= plant->load_on_us) {
            if (foc_virtual_motor_set_load(&plant->motor, plant->load_torque_nm) != 0) {
                return -1;
            }
            plant->load_applied = true;
        }
        uint32_t step = now_us - plant->time_us;
        if (step > SIL_PLANT_STEP_US) {
            step = SIL_PLANT_STEP_US;
        }
        const double dt_s = (double)step * 1.0e-6;
        /* HIGH_Z: all bridge switches off. Winding current has already decayed
         * through the diodes; the open winding carries none and the shaft coasts. */
        const int status = plant->drive != SIL_DRIVE_UPDATE
            ? foc_virtual_motor_step_open_circuit(&plant->motor, 0.0, 0.0, dt_s)
            : plant->dead_time_v > 0.0
            ? sil_plant_step_dead_time(plant, dt_s)
            : foc_virtual_motor_apply_duty(&plant->motor, &plant->active, plant->vbus_v, dt_s);
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
        /* Diode freewheeling ends within about L*I/VBUS (tens of µs), far below one slot. */
        plant->motor.state.id_a = 0.0;
        plant->motor.state.iq_a = 0.0;
        plant->motor.v_alpha_v = 0.0;
        plant->motor.v_beta_v = 0.0;
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

/* Sum of four uniforms (variance 1/3), scaled to the requested rms and rounded. */
static uint16_t sil_adc_noisy(struct sil_device *device, uint16_t raw)
{
    if (!(device->adc_noise_counts > 0.0)) {
        return raw;
    }
    double sum = 0.0;
    for (unsigned int index = 0U; index < 4U; ++index) {
        uint32_t x = device->noise_state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        device->noise_state = x;
        sum += (double)x / 4294967296.0 - 0.5;
    }
    const double value = (double)raw + sum * device->adc_noise_counts / 0.5773502691896258;
    return (uint16_t)fmin(fmax(floor(value + 0.5), 0.0), 4095.0);
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
            device->adc_ranks[1] = sil_adc_noisy(device, raw.rank2_raw);
            if (device->adc_step_counts != 0.0 && device->now_us >= device->adc_step_us) {
                const double stepped = (double)device->adc_ranks[1] + device->adc_step_counts;
                device->adc_ranks[1] = (uint16_t)(stepped < 0.0          ? 0.0
                                                      : stepped > 4095.0 ? 4095.0
                                                                         : stepped);
            }
            device->adc_ranks[2] = sil_adc_noisy(device, raw.rank3_raw);
            device->adc_ranks[3] = sil_adc_noisy(device, raw.rank4_raw);
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

static void sil_write_record(uint8_t channel, const uint8_t *payload, size_t length)
{
    const uint8_t header[2] = {channel, (uint8_t)length};
    (void)fwrite(header, 1U, sizeof header, stdout);
    (void)fwrite(payload, 1U, length, stdout);
    (void)fflush(stdout);
}

static void sil_drain_tx(struct sil_device *device)
{
    uint8_t buffer[255];
    size_t length = 0U;
    while (length < sizeof buffer && foc_uart_tx_pop(&device->uart, &buffer[length])) {
        ++length;
    }
    if (length == 0U) {
        return;
    }
    if (device->mux) {
        sil_write_record(SIL_CHANNEL_UART, buffer, length);
        return;
    }
    (void)fwrite(buffer, 1U, length, stdout);
    (void)fflush(stdout);
}

/* The CAN port of main.c: every frame leaves at once on the virtual bus. */
static bool sil_can_send(void *context, const struct foc_can_frame *frame)
{
    (void)context;
    uint8_t payload[SIL_CAN_RECORD] = {
        (uint8_t)frame->id,
        (uint8_t)(frame->id >> 8),
        frame->rtr ? 1U : 0U,
        frame->length,
    };
    memcpy(payload + 4, frame->data, sizeof frame->data);
    sil_write_record(SIL_CHANNEL_CAN, payload, sizeof payload);
    return true;
}

static uint32_t sil_can_state(const void *context)
{
    const struct sil_device *device = context;
    return device->mux ? FOC_CAN_STATE_ACTIVE : FOC_CAN_STATE_OFF;
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

/* main.c foc_at32_phase_voltage_read(): the open-winding EMF as terminal voltages through
 * the 11:1 dividers. The floating neutral is taken at its worst: the lowest terminal sits on a
 * body-diode clamp 0.3 V below ground, which the ADC reads as zero. */
static bool sil_phase_voltage_read(void *argument, uint16_t counts[3], uint32_t *sample_us)
{
    struct sil_device *device = argument;
    struct sil_plant *plant = &device->plant;
    double alpha = 0.0;
    double beta = 0.0;
    if (sil_plant_advance(plant, device->now_us) != 0 || plant->drive == SIL_DRIVE_UPDATE ||
        foc_virtual_motor_measure_open_circuit(&plant->motor, &alpha, &beta) != 0) {
        return false;
    }
    const double emf[3] = {
        alpha,
        -0.5 * alpha + 0.8660254037844386 * beta,
        -0.5 * alpha - 0.8660254037844386 * beta,
    };
    const double lowest = fmin(emf[0], fmin(emf[1], emf[2]));
    const double per_count = SIL_ADC_FULL_SCALE * SIL_VBUS_DIVIDER / SIL_ADC_MAX;
    for (unsigned int index = 0U; index < 3U; ++index) {
        const double count = round((emf[index] - lowest - 0.3) / per_count);
        counts[index] = (uint16_t)fmin(fmax(count, 0.0), SIL_ADC_MAX);
    }
    *sample_us = device->now_us;
    return true;
}

/* main.c foc_at32_nfault_asserted(): the injected nFAULT window on virtual time. */
static bool sil_driver_fault_line(void *argument)
{
    const struct sil_device *device = argument;
    return device->now_us >= device->driver_fault_on_us &&
        device->now_us < device->driver_fault_off_us;
}

/* Advance the NTC plant to now with the present copper loss. */
static void sil_thermal_advance(struct sil_device *device)
{
    struct foc_virtual_motor_observation seen;
    const double dt_s = (double)(device->now_us - device->thermal_us) * 1.0e-6;
    device->thermal_us = device->now_us;
    if (foc_virtual_motor_observe(&device->plant.motor, &seen) != 0 || !(dt_s > 0.0)) {
        return;
    }
    const double rise = device->rth_k_per_w * 1.5 * device->rs_ohm *
        (seen.id_a * seen.id_a + seen.iq_a * seen.iq_a);
    const double motor = 1.0 - exp(-dt_s / device->thermal_tau_s);
    const double fet = 1.0 - exp(-2.0 * dt_s / device->thermal_tau_s);
    device->motor_c += motor * (device->ambient_c + rise - device->motor_c);
    device->fet_c += fet * (device->ambient_c + 0.5 * rise - device->fet_c);
}

/* Board port: the channel's divider ratio from the plant temperature. */
static bool sil_ntc_read(void *argument, enum foc_board_ntc channel, float *ratio)
{
    const struct sil_device *device = argument;
    const bool override = device->now_us >= device->ntc_override_on_us &&
        device->now_us < device->ntc_override_off_us;
    const double celsius = override    ? device->ntc_override_c
        : channel == FOC_BOARD_NTC_FET ? device->fet_c
                                       : device->motor_c;
    const double r = 10000.0 * exp(3950.0 * (1.0 / (celsius + 273.15) - 1.0 / 298.15));
    *ratio = (float)(r / (r + 10000.0));
    return true;
}

/* main.c foreground loop body. */
static void sil_foreground(struct sil_device *device)
{
    struct foc_runtime_foreground_result result;
    (void)foc_runtime_foreground_poll(&device->runtime, device->now_us, 0U, 0U, &result);
    foc_serial_poll(&device->serial, &device->uart, &device->runtime, sil_serial_clock, device);
    if (device->mux) {
        foc_can_poll(&device->can, &device->serial, &device->runtime, device->now_us);
    }
    foc_board_calibration_poll(&device->calibration, device->now_us);
    sil_thermal_advance(device);
    foc_board_thermal_poll(&device->thermal, &device->serial, device->now_us);
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

/* One complete --can record: returns the lockstep advance it asks for in µs. */
static uint32_t sil_record(struct sil_device *device, uint32_t lockstep_us)
{
    const uint8_t channel = device->record[0];
    const uint8_t length = device->record[1];
    const uint8_t *payload = device->record + 2;
    uint32_t advance = 0U;
    if (channel == SIL_CHANNEL_UART) {
        for (unsigned int index = 0U; index < length; ++index) {
            foc_uart_rx_push(&device->uart, payload[index], false);
            advance += payload[index] == 0U ? lockstep_us : 0U;
        }
    } else if (channel == SIL_CHANNEL_CAN && length == SIL_CAN_RECORD) {
        struct foc_can_frame frame = {
            .id = (uint16_t)((payload[0] | payload[1] << 8) & 0x7FF),
            .rtr = (payload[2] & 1U) != 0U,
            .length = payload[3] > 8U ? 8U : payload[3],
        };
        memcpy(frame.data, payload + 4, sizeof frame.data);
        foc_can_rx_push(&device->can_buffer, &frame);
    } else if (channel == SIL_CHANNEL_TICK && length == 4U) {
        advance = (uint32_t)payload[0] | (uint32_t)payload[1] << 8 | (uint32_t)payload[2] << 16 |
            (uint32_t)payload[3] << 24;
        device->tick_ack = true;
    }
    return advance;
}

/* Feed one received byte: raw UART, or part of a --can record. Returns the advance. */
static uint32_t sil_receive(struct sil_device *device, uint8_t byte, uint32_t lockstep_us)
{
    if (!device->mux) {
        foc_uart_rx_push(&device->uart, byte, false);
        return byte == 0U ? lockstep_us : 0U;
    }
    device->record[device->record_have++] = byte;
    if (device->record_have < 2U || device->record_have < 2U + device->record[1]) {
        return 0U;
    }
    device->record_have = 0U;
    return sil_record(device, lockstep_us);
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
            (void)sil_receive(device, buffer[index], 0U);
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
        "          [--flash file] [--adc-offset a,b,c] [--adc-noise rms] [--vbus-v v]\n"
        "          [--adc-step us,counts]\n"
        "          [--motor-rs-ohm r] "
        "[--motor-ld-h l]\n"
        "          [--motor-lq-h l] [--motor-psi-f-wb psi] [--motor-pole-pairs n]\n"
        "          [--motor-offset-rad rad] [--motor-inertia j] [--motor-damping b]\n"
        "          [--load-torque-nm t[,on_us]] [--driver-fault-us on_us[,off_us]]\n"
        "          [--dead-time-v v] [--can serial] [--thermal ambient_c,rth_k_per_w,tau_s]\n"
        "          [--ntc-override-us on_us,off_us,celsius]\n",
        name
    );
}

/* Flash device ------------------------------------------------------------------ */

static int sil_flash_sync(const struct sil_flash *flash)
{
    if (flash->path == NULL) {
        return 0;
    }
    FILE *file = fopen(flash->path, "wb");
    if (file == NULL) {
        return -1;
    }
    const size_t written = fwrite(flash->slots, 1U, sizeof flash->slots, file);
    return fclose(file) == 0 && written == sizeof flash->slots ? 0 : -1;
}

static void sil_flash_open(struct sil_flash *flash, const char *path)
{
    memset(flash->slots, 0xFF, sizeof flash->slots);
    flash->path = path;
    if (path == NULL) {
        return;
    }
    FILE *file = fopen(path, "rb");
    if (file != NULL) {
        uint8_t image[sizeof flash->slots];
        if (fread(image, 1U, sizeof image, file) == sizeof image) {
            memcpy(flash->slots, image, sizeof image);
        }
        (void)fclose(file);
    }
}

static int
sil_flash_read(void *context, unsigned int slot, size_t offset, uint8_t *bytes, size_t size)
{
    const struct sil_flash *flash = context;
    if (slot > 1U || offset > sizeof flash->slots[0] || size > sizeof flash->slots[0] - offset) {
        return -1;
    }
    memcpy(bytes, &flash->slots[slot][offset], size);
    return 0;
}

static int sil_flash_erase(void *context, unsigned int slot)
{
    struct sil_flash *flash = context;
    if (slot > 1U) {
        return -1;
    }
    memset(flash->slots[slot], 0xFF, sizeof flash->slots[slot]);
    flash->erases++;
    return sil_flash_sync(flash);
}

/* NOR semantics: aligned words, bits only cleared. */
static int sil_flash_program(
    void *context,
    unsigned int slot,
    size_t offset,
    const uint8_t *bytes,
    size_t size
)
{
    struct sil_flash *flash = context;
    if (slot > 1U || offset % FOC_BOARD_STORE_PROGRAM_BYTES != 0U ||
        size % FOC_BOARD_STORE_PROGRAM_BYTES != 0U || offset > sizeof flash->slots[0] ||
        size > sizeof flash->slots[0] - offset) {
        return -1;
    }
    for (size_t index = 0U; index < size; ++index) {
        flash->slots[slot][offset + index] &= bytes[index];
    }
    flash->programs++;
    return sil_flash_sync(flash);
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
    double load_torque_nm = 0.0;
    uint32_t load_on_us = 0U;
    const char *flash_path = NULL;
    struct foc_virtual_motor_params motor = sil_default_motor();
    device.trace_us = 500U;
    device.ambient_c = 25.0;
    device.rth_k_per_w = 10.0;
    device.thermal_tau_s = 30.0;

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
        } else if (strcmp(option, "--adc-noise") == 0) {
            device.adc_noise_counts = strtod(value, NULL);
            device.noise_state = UINT32_C(0x9E3779B9);
        } else if (strcmp(option, "--adc-step") == 0) {
            char *rest = NULL;
            device.adc_step_us = (uint32_t)strtoul(value, &rest, 0);
            device.adc_step_counts = rest != NULL && *rest == ',' ? strtod(rest + 1, NULL) : 0.0;
        } else if (strcmp(option, "--flash") == 0) {
            flash_path = value;
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
        } else if (strcmp(option, "--can") == 0) {
            /* Multiplexed stdio with a virtual CAN bus; the value is the 48-bit serial. */
            device.mux = true;
            device.can.port.serial_number = strtoull(value, NULL, 0) & 0xFFFFFFFFFFFFULL;
        } else if (strcmp(option, "--dead-time-v") == 0) {
            /* Inverter voltage error per phase, opposing its current. */
            device.plant.dead_time_v = strtod(value, NULL);
        } else if (strcmp(option, "--driver-fault-us") == 0) {
            /* DRV8311 nFAULT held low from on_us until off_us (default: never released). */
            char *rest = NULL;
            device.driver_fault_on_us = (uint32_t)strtoul(value, &rest, 0);
            device.driver_fault_off_us =
                rest != NULL && *rest == ',' ? (uint32_t)strtoul(rest + 1, NULL, 0) : UINT32_MAX;
        } else if (strcmp(option, "--thermal") == 0) {
            /* NTC plant: ambient in degC, winding thermal resistance in K/W, time constant s. */
            char *rest = NULL;
            device.ambient_c = strtod(value, &rest);
            device.rth_k_per_w = rest != NULL && *rest == ',' ? strtod(rest + 1, &rest) : -1.0;
            device.thermal_tau_s = rest != NULL && *rest == ',' ? strtod(rest + 1, NULL) : -1.0;
        } else if (strcmp(option, "--ntc-override-us") == 0) {
            /* Both NTC channels read celsius from on_us until off_us. */
            char *rest = NULL;
            device.ntc_override_on_us = (uint32_t)strtoul(value, &rest, 0);
            device.ntc_override_off_us =
                rest != NULL && *rest == ',' ? (uint32_t)strtoul(rest + 1, &rest, 0) : 0U;
            device.ntc_override_c = rest != NULL && *rest == ',' ? strtod(rest + 1, NULL) : NAN;
        } else if (strcmp(option, "--load-torque-nm") == 0) {
            /* Constant external load torque at the rotor, opposing positive motor torque,
             * applied from plant time on_us (default 0). */
            char *rest = NULL;
            load_torque_nm = strtod(value, &rest);
            load_on_us = rest != NULL && *rest == ',' ? (uint32_t)strtoul(rest + 1, NULL, 0) : 0U;
        } else {
            sil_usage(argv[0]);
            return 2;
        }
    }
    if (!(speed > 0.0) || !isfinite(speed) || !(vbus_v > 0.0f) || !isfinite(device.ambient_c) ||
        !(device.rth_k_per_w >= 0.0) || !(device.thermal_tau_s > 0.0) ||
        (device.ntc_override_off_us > device.ntc_override_on_us &&
         !isfinite(device.ntc_override_c))) {
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
    plant->load_torque_nm = load_torque_nm;
    plant->load_on_us = load_on_us;
    if (foc_virtual_motor_init(&plant->motor, &motor, &initial) != 0 || !isfinite(load_torque_nm)) {
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
    if (device.driver_fault_on_us != 0U) {
        device.adc.driver_fault_line = sil_driver_fault_line;
        device.adc.driver_fault_argument = &device;
    }
    device.rs_ohm = motor.rs_ohm;
    device.motor_c = device.ambient_c;
    device.fet_c = device.ambient_c;
    foc_board_thermal_init(&device.thermal, sil_ntc_read, &device);
    device.adc.thermal = &device.thermal;
    /* Single-threaded SIL: no producer runs while the manager reconfigures. */
    foc_board_calibration_bind(
        &device.calibration,
        &device.runtime,
        &device.output,
        &device.encoder,
        &device.adc,
        &device.serial,
        NULL,
        NULL
    );
    foc_board_calibration_attach_phase_voltage(
        &device.calibration,
        sil_phase_voltage_read,
        &device
    );
    const struct foc_serial_calibration calibration =
        foc_board_calibration_serial(&device.calibration);
    foc_serial_attach_calibration(&device.serial, &calibration);
    /* The boot configuration and tuning CONFIG_ERASE returns to, before any restore. */
    foc_board_calibration_capture_boot(&device.calibration, &device.boot);
    /* Boot restore before any producer, as an image with storage does on the target. */
    sil_flash_open(&device.flash, flash_path);
    device.persistence.driver = (struct foc_board_store_driver){
        .context = &device.flash,
        .read = sil_flash_read,
        .erase = sil_flash_erase,
        .program = sil_flash_program,
    };
    foc_board_persistence_restore(
        &device.persistence,
        &device.boot,
        &device.runtime,
        &device.output,
        &device.encoder,
        &device.serial
    );
    const struct foc_serial_persistence persistence =
        foc_board_persistence_serial(&device.persistence);
    foc_serial_attach_persistence(&device.serial, &persistence);
    /* main.c foc_at32_can_init(): the port, after the restore. */
    const struct foc_can_port can_port = {
        .context = &device,
        .send = sil_can_send,
        .state = sil_can_state,
        .serial_number = device.can.port.serial_number,
    };
    foc_can_init(&device.can, &device.can_buffer, &can_port);
    const struct foc_serial_can_link can_link = {&device.can, foc_can_link_status};
    foc_serial_attach_can_link(&device.serial, &can_link);
    /* A calibration restored from storage is the manager's applied result; saves keep it. */
    if (device.persistence.calibration_restored) {
        foc_board_calibration_adopt(&device.calibration, &device.persistence.restored_calibration);
    }
    device.persistence.calibration_source = &device.calibration.snapshot;
    if (!foc_board_adc_init_timing(&device.adc, 0U, 0U, 0U)) {
        fprintf(stderr, "FALCONF0C_META {\"kind\":\"error\",\"reason\":\"adc timing\"}\n");
        return 1;
    }
    fprintf(
        stderr,
        "FALCONF0C_META {\"kind\":\"sil_boot\",\"simulated\":true,\"physical_applied\":false,"
        "\"mode\":\"%s\",\"speed\":%.6g,\"lockstep_us\":%u,\"plant_pole_pairs\":%u,"
        "\"plant_offset_rad\":%.9g,\"plant_adc_offset\":[%.6g,%.6g,%.6g],"
        "\"store_state\":%u,\"store_loaded_sequence\":%u,\"flash_file\":%s}\n",
        lockstep_us != 0U ? "lockstep" : "free-run",
        speed,
        lockstep_us,
        motor.pole_pairs,
        motor.electrical_offset_rad,
        offsets[0],
        offsets[1],
        offsets[2],
        (unsigned int)device.persistence.state,
        device.persistence.loaded_sequence,
        flash_path != NULL ? "true" : "false"
    );

    if (lockstep_us != 0U) {
        /* Deterministic: time moves only after each complete received frame. */
        for (;;) {
            const int byte = getchar();
            if (byte == EOF) {
                break;
            }
            const uint32_t advance = sil_receive(&device, (uint8_t)byte, lockstep_us);
            if (advance != 0U && sil_advance_to(&device, device.now_us + advance) != 0) {
                return 1;
            }
            if (device.tick_ack) {
                /* Every frame of the advanced interval is already on stdout. */
                const uint8_t now[4] = {
                    (uint8_t)device.now_us,
                    (uint8_t)(device.now_us >> 8),
                    (uint8_t)(device.now_us >> 16),
                    (uint8_t)(device.now_us >> 24),
                };
                device.tick_ack = false;
                sil_write_record(SIL_CHANNEL_TICK, now, sizeof now);
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
