/**
 * @file    main.c
 * @brief   Standalone FalconFOC V2 AT32F415 entry.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Implementation role
 * -------------------
 * Bind the real timer, ADC and SPI events to the static V2 control instance.
 */

/* Includes --------------------------------------------------------------------- */

#include "at32f415.h"
#include "foc_board.h"
#include "foc_board_boot.h"
#include "foc_board_calibrate.h"
#include "foc_board_timing.h"
#include "foc_can.h"
#if defined(FALCONFOC_BOARD_CAPTURE)
#include "foc_board_capture.h"
#endif
#include "foc_serial.h"
#if defined(FALCONFOC_BOARD_PERSISTENCE)
#include "foc_board_store.h"

struct foc_board_store_driver foc_at32_store_driver(void);
#endif

/* Private Macros --------------------------------------------------------------- */

#define FOC_TIMER_TICKS_PER_US (FOC_BOARD_TIMER_HZ / FOC_BOARD_US_HZ)
#define FOC_TIME_HALF_MARK     0x80000000U
#define FOC_TIME_FLAGS         (TMR_OVF_FLAG | TMR_C1_FLAG)
#define FOC_SPI_ERROR_FLAGS    (SPI_MMERR_FLAG | SPI_I2S_ROERR_FLAG | SPI_CCERR_FLAG)
#define FOC_SPI_IRQS           (SPI_I2S_RDBF_INT | SPI_I2S_ERROR_INT)
#define FOC_TMR5_EVENTS        (TMR_OVF_FLAG | TMR_C1_FLAG | TMR_C2_FLAG)
#define FOC_ENCODER_CS         GPIO_PINS_15
#define FOC_STARTUP_POLL_LIMIT 100000U
/* CAN1 on APB1 (75 MHz): 15 time quanta per bit (1 + 12 + 2), sample point 86.7 %. */
#define FOC_CAN_APB1_HZ        75000000U
#define FOC_CAN_QUANTA         15U
/* 96-bit MCU unique ID, folded into the 48-bit CANSimple serial number. */
#define FOC_UID_BASE           0x1FFFF7E8U

/* Check the offline route model against the vendor SDK in every ARM build. */
_Static_assert((int)FOC_BOARD_ADC_TMR1_CH4 == (int)ADC12_PREEMPT_TRIG_TMR1CH4, "ADC CH4 selector");
_Static_assert(
    (int)FOC_BOARD_ADC_TMR1_TRGOUT == (int)ADC12_PREEMPT_TRIG_TMR1TRGOUT,
    "ADC TRGOUT selector"
);
_Static_assert(
    (int)FOC_BOARD_ADC_REMAPPED_CH4 == (int)ADC12_PREEMPT_TRIG_EXINT15_TMR1CH4,
    "ADC remapped CH4 selector"
);
_Static_assert(
    (int)FOC_BOARD_ADC_SOFTWARE == (int)ADC12_PREEMPT_TRIG_SOFTWARE,
    "ADC software selector"
);
_Static_assert(ADC1_ETP_GMUX == 0x30040101U, "ADC1 remap7 bit 4");

/* Private Types ---------------------------------------------------------------- */

enum foc_spi_phase {
    FOC_SPI_IDLE,
    FOC_SPI_SELECTED,
    FOC_SPI_SENT,
    FOC_SPI_RECEIVED,
    FOC_SPI_ERROR,
};

/* Private Variables ------------------------------------------------------------ */

static struct foc_runtime runtime;
static struct foc_board_output output;
static struct foc_board_adc adc;
static struct foc_board_encoder encoder;
static struct foc_board_boot boot;
static enum foc_spi_phase spi_phase;
/* True when the PLL runs from the 8 MHz crystal, false on the HICK fallback (debugger view). */
static volatile bool clock_from_crystal;
static uint16_t spi_word;
static uint32_t spi_rx_us;
static struct foc_uart_buffer uart;
static struct foc_serial serial;
static struct foc_board_calibration calibration;
static struct foc_can_buffer can_buffer;
static struct foc_can can;
static bool can_started;
#if defined(FALCONFOC_BOARD_PERSISTENCE)
static struct foc_board_persistence persistence;
#endif

#if defined(FALCONFOC_BOARD_CAPTURE)
/* These are the only SWD capture control words. No UART view or control
 * request is extended. The record image begins at falconfoc_capture_ram. */
struct foc_board_capture falconfoc_capture_ram;
volatile uint32_t falconfoc_capture_request;
volatile uint32_t falconfoc_capture_cancel;
static uint32_t capture_cs_us;
static uint32_t capture_spi_status;
static bool capture_cs_seen;
static bool capture_rx_seen;
#endif

#if defined(FALCONFOC_RT02_TIMING_TRACE)
struct foc_at32_trace_span {
    uint32_t start;
    uint32_t end;
    uint32_t identity;
    uint32_t detail;
};

/* Each record has one execution-domain owner. The probe is read after the
 * selected scenario; a live multiword read can cross a publication. */
static volatile struct {
    struct foc_at32_trace_span probe;
    struct foc_at32_trace_span trigger;
    struct foc_at32_trace_span adc;
    struct foc_at32_trace_span tmr5[3];
    struct foc_at32_trace_span spi;
    struct foc_at32_trace_span usart;
    struct foc_at32_trace_span supervision;
    struct foc_at32_trace_span serial;
    uint32_t cs_assert_cycles;
    uint32_t tx_cycles;
    uint32_t rx_cycles;
    uint32_t cs_release_cycles;
} rt_trace;
#endif

/* Private Helpers -------------------------------------------------------------- */

#if defined(FALCONFOC_RT02_TIMING_TRACE)
static uint32_t foc_at32_trace_cycles(void)
{
    return DWT->CYCCNT;
}

static uint32_t foc_at32_trace_enter(void)
{
    return adc.trace_cycles != NULL ? foc_at32_trace_cycles() : 0U;
}

static void foc_at32_trace_exit(
    volatile struct foc_at32_trace_span *span,
    uint32_t start,
    uint32_t identity,
    uint32_t detail
)
{
    if (adc.trace_cycles != NULL) {
        const uint32_t end = foc_at32_trace_cycles();
        span->start = start;
        span->identity = identity;
        span->detail = detail;
        span->end = end;
    }
}

static void
foc_at32_trace_hook(struct foc_runtime *instance, enum foc_runtime_hook_point point, void *argument)
{
    (void)instance;
    (void)argument;
    struct foc_board_adc_trace *trace = &adc.live.transaction.trace;
    switch (point) {
    case FOC_RUNTIME_HOOK_H0_INPUTS:
        trace->h0_inputs_cycles = foc_at32_trace_cycles();
        trace->points |= FOC_BOARD_TRACE_H0_INPUTS;
        break;
    case FOC_RUNTIME_HOOK_BEFORE_SUBMIT:
        trace->h0_guard_cycles = foc_at32_trace_cycles();
        trace->points |= FOC_BOARD_TRACE_H0_GUARD;
        break;
    case FOC_RUNTIME_HOOK_AFTER_CONTROL:
        trace->h0_control_done_cycles = foc_at32_trace_cycles();
        trace->points |= FOC_BOARD_TRACE_H0_CONTROL_DONE;
        break;
    case FOC_RUNTIME_HOOK_SENSOR_WRITING:
        encoder.trace.publish_proxy_cycles = foc_at32_trace_cycles();
        encoder.trace.publishing = true;
        break;
    default:
        break;
    }
}

static void foc_at32_trace_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) == 0U) {
        DWT->CYCCNT = 0U;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
        if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U) {
            adc.trace_cycles = foc_at32_trace_cycles;
            encoder.trace_cycles = foc_at32_trace_cycles;
            runtime.config.hook = foc_at32_trace_hook;
            foc_at32_trace_exit(&rt_trace.probe, foc_at32_trace_enter(), 0U, 0U);
        }
    }
}
#endif

static uint32_t foc_at32_now_us(void)
{
    return tmr_counter_value_get(TMR2);
}

#if defined(FALCONFOC_BOARD_CAPTURE)
static void foc_at32_capture_foreground(void)
{
    struct foc_board_capture *capture = &falconfoc_capture_ram;
    const unsigned int state = atomic_load_explicit(&capture->state, memory_order_relaxed);
    if (state == FOC_CAPTURE_IDLE) {
        const uint32_t request = falconfoc_capture_request;
        struct foc_capture_profile profile;
        /* Metadata follows the effective boot configuration, not the nominal constants. */
        if (request != 0U &&
            foc_board_boot_capture_profile(&boot, FOC_CAPTURE_BUILD_TAG, &profile)) {
            (void)foc_board_capture_start(
                capture,
                request,
                FOC_CAPTURE_BOARD,
                foc_at32_now_us() + 1000U,
                &profile
            );
        }
    } else if (state != FOC_CAPTURE_READY) {
        const uint32_t now = state == FOC_CAPTURE_ACTIVE ? foc_at32_now_us() : 0U;
        foc_board_capture_poll(capture, now, falconfoc_capture_cancel);
    }
}

static void foc_at32_capture_adc(
    const uint16_t ranks[4],
    const struct foc_board_adc_event *event,
    bool pending_token,
    bool pcce_drained,
    int result
)
{
    struct foc_capture_adc *row = foc_board_capture_adc(&falconfoc_capture_ram, event->now_us);
    if (row == NULL) {
        return;
    }
    const struct foc_board_adc_transaction *token = &adc.live.transaction;
    row->ranks12 = ranks[0] | ((uint32_t)ranks[1] << 16);
    row->ranks34 = ranks[2] | ((uint32_t)ranks[3] << 16);
    row->flags = pcce_drained ? FOC_CAPTURE_ADC_CLEAR : 0U;
    row->missing_slots = adc.live.missing_slots;
    row->late_count = adc.live.late_completions;
    row->rejection = pending_token ? token->rejection : adc.live.last_rejection;
    if (adc.live.clock_valid && event->coherent) {
        row->flags |= FOC_CAPTURE_ADC_SLOT;
        row->slot = adc.live.slot_id;
        row->slot_start_us = adc.slot_start_us;
    }
    if (pending_token) {
        row->flags |= FOC_CAPTURE_ADC_TOKEN;
        row->generation = token->generation;
        row->token_slot = token->slot_id;
        row->requested_us = token->requested_us;
        if (token->adc_valid) {
            row->flags |= FOC_CAPTURE_ADC_VALID;
        }
        if (token->h0_called) {
            row->flags |= FOC_CAPTURE_ADC_H0;
            row->h0_us = event->now_us;
            row->h0_return_us = foc_at32_now_us();
            if (token->h0_consumed) {
                row->flags |= FOC_CAPTURE_ADC_CONSUMED;
            }
            if (token->h0_consumed && runtime.observed_sensor.valid) {
                row->flags |= FOC_CAPTURE_ADC_SENSOR;
                row->sensor_sequence = runtime.observed_sensor.seq;
                row->sensor_us = runtime.observed_sensor.captured_us;
            }
#if defined(FALCONFOC_RT02_TIMING_TRACE)
            if ((token->trace.points & FOC_BOARD_TRACE_H0) != 0U) {
                row->flags |= FOC_CAPTURE_ADC_DWT;
                row->h0_cycles = token->trace.h0_finish_cycles - token->trace.h0_start_cycles;
            }
#endif
        }
    }
    const struct foc_safe_context *safe = &runtime.pipeline.safety;
    const struct foc_pipeline_mock *mock = &runtime.pipeline.mock;
    if (safe->command_seen) {
        row->flags |= FOC_CAPTURE_ADC_COMMAND;
        row->command_sequence = safe->command_seq;
    }
    const bool stop = atomic_load_explicit(&runtime.stop_requests, memory_order_relaxed) != 0U;
    row->safety = FOC_CAPTURE_OUTPUT_LOCKED | (uint32_t)mock->last_request.action |
        ((uint32_t)safe->state << 4) |
        (mock->output_confirmed && !stop ? FOC_CAPTURE_OUTPUT_CONFIRMED : 0U) |
        (stop ? FOC_CAPTURE_STOP_PENDING : 0U);
    row->session = runtime.motion_session;
    row->faults = safe->latched_faults;
    row->h0_result = result;
}

static void
foc_at32_capture_encoder(uint32_t closed_us, enum foc_board_encoder_result transport, int h1)
{
    struct foc_capture_encoder *row = foc_board_capture_encoder(&falconfoc_capture_ram, closed_us);
    if (row == NULL) {
        return;
    }
    row->encoder_sequence = encoder.seq - 1U;
    row->h1_return_us = foc_at32_now_us();
    row->flags = (capture_cs_seen ? FOC_CAPTURE_ENCODER_CS : 0U) |
        (capture_rx_seen ? FOC_CAPTURE_ENCODER_RAW : 0U) |
        ((encoder.result == FOC_BOARD_ENCODER_OK &&
          (encoder.feedback.quality == FOC_ENCODER_QUALITY_VALID ||
           encoder.feedback.quality == FOC_ENCODER_QUALITY_UNINITIALIZED))
             ? FOC_CAPTURE_ENCODER_POSITION
             : 0U) |
        ((encoder.result == FOC_BOARD_ENCODER_OK && encoder.feedback.speed_valid)
             ? FOC_CAPTURE_ENCODER_SPEED
             : 0U) |
        (encoder.continuity_lost ? FOC_CAPTURE_ENCODER_CONTINUITY_LOST : 0U) |
        (h1 == FOC_RUNTIME_OK ? FOC_CAPTURE_ENCODER_H1 : 0U);
    row->cs_us = capture_cs_seen ? capture_cs_us : 0U;
    row->rx_us = capture_rx_seen ? spi_rx_us : 0U;
    row->raw_word = capture_rx_seen ? spi_word : 0U;
    row->transport = transport;
    row->result = encoder.result;
    row->losses = encoder.losses;
    row->h1_status = h1;
    row->spi_status = capture_spi_status;
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    if (encoder.trace_cycles != NULL) {
        row->flags |= FOC_CAPTURE_ENCODER_DWT;
        row->h1_cycles = encoder.trace.h1_finish_cycles - encoder.trace.h1_start_cycles;
    }
#endif
}
#endif

static struct foc_board_adc_event foc_at32_adc_event(void)
{
    struct foc_board_adc_event event = {0};
    for (unsigned int attempt = 0U; attempt < 2U; ++attempt) {
        const uint32_t marks = TMR2->ists & FOC_TIME_FLAGS;
        const uint32_t period = TMR1->ists & TMR_OVF_FLAG;
        event.before_us = TMR2->cval;
        event.phase_ticks = TMR1->cval;
        event.now_us = TMR2->cval;
        if (marks != (TMR2->ists & FOC_TIME_FLAGS) || period != (TMR1->ists & TMR_OVF_FLAG)) {
            continue;
        }
        if (marks != 0U) {
            tmr_flag_clear(TMR2, marks);
        }
        if (period != 0U) {
            tmr_flag_clear(TMR1, TMR_OVF_FLAG);
            /* A second overflow during W0C can erase the next witness. */
            if (TMR1->cval < event.phase_ticks) {
                return event;
            }
        }
        event.flags = ((marks & TMR_OVF_FLAG) != 0U ? FOC_BOARD_TIME_WRAP : 0U) |
            ((marks & TMR_C1_FLAG) != 0U ? FOC_BOARD_TIME_HALF : 0U) |
            (period != 0U ? FOC_BOARD_SLOT_WRAP : 0U);
        event.coherent = TMR1->ctrl1_bit.tmren != 0U && TMR1->ctrl1_bit.cnt_dir == TMR_COUNT_UP &&
            TMR1->pr == FOC_BOARD_COMPARE_MAX && TMR1->div == 0U &&
            TMR1->c4dt == FOC_BOARD_ADC_COMPARE_US * FOC_TIMER_TICKS_PER_US &&
            TMR2->ctrl1_bit.tmren != 0U && TMR2->ctrl1_bit.cnt_dir == TMR_COUNT_UP &&
            TMR2->ctrl1_bit.pmen != 0U && TMR2->pr == UINT32_MAX &&
            TMR2->div == FOC_TIMER_TICKS_PER_US - 1U && TMR2->c1dt == FOC_TIME_HALF_MARK;
        return event;
    }
    return event;
}

/* Hold the H0/H1 producers while the calibration manager re-initializes the instance. */
static void foc_at32_quiesce(void *argument, bool hold)
{
    (void)argument;
    const IRQn_Type producers[] = {TMR1_CH_IRQn, ADC1_IRQn, TMR5_GLOBAL_IRQn, SPI1_IRQn};
    for (unsigned int index = 0U; index < sizeof producers / sizeof producers[0]; ++index) {
        if (hold) {
            NVIC_DisableIRQ(producers[index]);
        } else {
            NVIC_EnableIRQ(producers[index]);
        }
    }
    __DSB();
    __ISB();
}

static uint32_t foc_at32_serial_now(void *argument)
{
    (void)argument;
    return foc_at32_now_us();
}

static void foc_at32_uart_init(void)
{
    crm_periph_clock_enable(CRM_USART1_PERIPH_CLOCK, TRUE);
    gpio_pin_remap_config(USART1_GMUX_0001, TRUE);
    gpio_init_type pins;
    gpio_default_para_init(&pins);
    pins.gpio_mode = GPIO_MODE_MUX;
    pins.gpio_pins = GPIO_PINS_6;
    gpio_init(GPIOB, &pins);
    pins.gpio_mode = GPIO_MODE_INPUT;
    pins.gpio_pull = GPIO_PULL_UP;
    pins.gpio_pins = GPIO_PINS_7;
    gpio_init(GPIOB, &pins);
    usart_init(USART1, 115200, USART_DATA_8BITS, USART_STOP_1_BIT);
    usart_transmitter_enable(USART1, TRUE);
    usart_receiver_enable(USART1, TRUE);
    usart_parity_selection_config(USART1, USART_PARITY_NONE);
    usart_hardware_flow_control_set(USART1, USART_HARDWARE_FLOW_NONE);
    usart_interrupt_enable(USART1, USART_RDBF_INT, TRUE);
    usart_interrupt_enable(USART1, USART_ERR_INT, TRUE);
    usart_enable(USART1, TRUE);
}

static bool foc_at32_can_send(void *context, const struct foc_can_frame *frame)
{
    (void)context;
    can_tx_message_type message = {
        .standard_id = frame->id,
        .id_type = CAN_ID_STANDARD,
        .frame_type = frame->rtr ? CAN_TFT_REMOTE : CAN_TFT_DATA,
        .dlc = frame->length,
    };
    for (unsigned int index = 0U; index < frame->length && index < 8U; ++index) {
        message.data[index] = frame->data[index];
    }
    return can_message_transmit(CAN1, &message) != CAN_TX_STATUS_NO_EMPTY;
}

static uint32_t foc_at32_can_state(const void *context)
{
    (void)context;
    if (!can_started) {
        return clock_from_crystal ? FOC_CAN_STATE_OFF : FOC_CAN_STATE_NO_CRYSTAL;
    }
    return can_flag_get(CAN1, CAN_BOF_FLAG) == SET ? FOC_CAN_STATE_BUS_OFF
        : can_flag_get(CAN1, CAN_EPF_FLAG) == SET  ? FOC_CAN_STATE_PASSIVE
                                                   : FOC_CAN_STATE_ACTIVE;
}

static void foc_at32_can_reboot(void *context)
{
    (void)context;
    NVIC_SystemReset();
}

/* CAN1 on PA11 (RX) / PA12 (TX) through the SIT3051. The internal RC oscillator is too
 * inaccurate for CAN, so the HICK fallback leaves CAN off (ID 126 reads NO_CRYSTAL). */
static void foc_at32_can_init(uint32_t bitrate)
{
    const volatile uint32_t *uid = (const volatile uint32_t *)FOC_UID_BASE;
    const uint64_t folded =
        (uint64_t)((uid[1] ^ (uid[2] >> 16)) & 0xFFFFU) << 32 | (uint64_t)(uid[0] ^ uid[2]);
    const struct foc_can_port port = {
        .send = foc_at32_can_send,
        .state = foc_at32_can_state,
        .reboot = foc_at32_can_reboot,
        .serial_number = folded != 0U ? folded : 1U,
    };
    foc_can_init(&can, &can_buffer, &port);
    const struct foc_serial_can_link link = {&can, foc_can_link_status};
    foc_serial_attach_can_link(&serial, &link);
    if (!clock_from_crystal || bitrate == 0U) {
        return;
    }
    crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_CAN1_PERIPH_CLOCK, TRUE);
    gpio_init_type pins;
    gpio_default_para_init(&pins);
    pins.gpio_mode = GPIO_MODE_MUX;
    pins.gpio_pins = GPIO_PINS_12;
    gpio_init(GPIOA, &pins);
    pins.gpio_mode = GPIO_MODE_INPUT;
    pins.gpio_pull = GPIO_PULL_UP;
    pins.gpio_pins = GPIO_PINS_11;
    gpio_init(GPIOA, &pins);
    can_base_type base;
    can_default_para_init(&base);
    base.mode_selection = CAN_MODE_COMMUNICATE;
    base.aebo_enable = TRUE;
    base.mdrsel_selection = CAN_DISCARDING_FIRST_RECEIVED;
    base.mmssr_selection = CAN_SENDING_BY_ID;
    can_baudrate_type timing = {
        .baudrate_div = (uint16_t)(FOC_CAN_APB1_HZ / (FOC_CAN_QUANTA * bitrate)),
        .rsaw_size = CAN_RSAW_1TQ,
        .bts1_size = CAN_BTS1_12TQ,
        .bts2_size = CAN_BTS2_2TQ,
    };
    if (can_base_init(CAN1, &base) != SUCCESS || can_baudrate_set(CAN1, &timing) != SUCCESS) {
        return;
    }
    /* One 32-bit mask filter that accepts everything: the IRQ drops extended frames. */
    can_filter_init_type filter;
    can_filter_default_para_init(&filter);
    filter.filter_activate_enable = TRUE;
    filter.filter_mode = CAN_FILTER_MODE_ID_MASK;
    filter.filter_fifo = CAN_FILTER_FIFO0;
    filter.filter_number = 0U;
    filter.filter_bit = CAN_FILTER_32BIT;
    can_filter_init(CAN1, &filter);
    can_interrupt_enable(CAN1, CAN_RF0MIEN_INT, TRUE);
    can_started = true;
}

static void foc_at32_lock_output(void)
{
    crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_TMR1_PERIPH_CLOCK, TRUE);
    /* Reset clears MOE and all channel enables. Keep the bridge inputs unmuxed. */
    crm_periph_reset(CRM_TMR1_PERIPH_RESET, TRUE);
    crm_periph_reset(CRM_TMR1_PERIPH_RESET, FALSE);
    gpio_init_type pins;
    gpio_default_para_init(&pins);
    pins.gpio_mode = GPIO_MODE_ANALOG;
    pins.gpio_pins = GPIO_PINS_7 | GPIO_PINS_8 | GPIO_PINS_9 | GPIO_PINS_10;
    gpio_init(GPIOA, &pins);
    pins.gpio_pins = GPIO_PINS_0 | GPIO_PINS_1;
    gpio_init(GPIOB, &pins);
}

/* Board facts that are not producers: phase-voltage dividers (VC PA1, VB PA2, VA PA3) stay
 * analog until they are sampled; NFAULT (PB2) is an input with the board's 10 kOhm pull-up to
 * DRV_3V3; unconnected PB8 is pulled down. VBUS and phase currents are set in ADC init. */
static void foc_at32_board_pins_init(void)
{
    gpio_init_type pins;
    gpio_default_para_init(&pins);
    pins.gpio_mode = GPIO_MODE_ANALOG;
    pins.gpio_pins = GPIO_PINS_1 | GPIO_PINS_2 | GPIO_PINS_3;
    gpio_init(GPIOA, &pins);
    pins.gpio_mode = GPIO_MODE_INPUT;
    pins.gpio_pull = GPIO_PULL_NONE;
    pins.gpio_pins = GPIO_PINS_2;
    gpio_init(GPIOB, &pins);
    pins.gpio_pull = GPIO_PULL_DOWN;
    pins.gpio_pins = GPIO_PINS_8;
    gpio_init(GPIOB, &pins);
}

/* Coast check phase voltages, foreground only, bridge off: three single software-triggered
 * ordinary conversions (the injected H0 group preempts them), logical phases A, B, C =
 * driver C (PA1/IN1), B (PA2/IN2), A (PA3/IN3), the order of the currents and PWM. */
static bool foc_at32_phase_voltage_read(void *argument, uint16_t counts[3], uint32_t *sample_us)
{
    (void)argument;
    static const adc_channel_select_type channels[3] =
        {ADC_CHANNEL_1, ADC_CHANNEL_2, ADC_CHANNEL_3};
    for (unsigned int index = 0U; index < 3U; ++index) {
        adc_ordinary_channel_set(ADC1, channels[index], 1, ADC_SAMPLETIME_28_5);
        adc_flag_clear(ADC1, ADC_CCE_FLAG);
        adc_ordinary_software_trigger_enable(ADC1, TRUE);
        uint32_t remaining = 1000U;
        while (adc_flag_get(ADC1, ADC_CCE_FLAG) == RESET && remaining != 0U) {
            remaining--;
        }
        if (remaining == 0U) {
            return false;
        }
        counts[index] = adc_ordinary_conversion_data_get(ADC1);
        if (index == 1U) {
            *sample_us = foc_at32_now_us();
        }
    }
    return true;
}

/* DRV8311 nFAULT (PB2, open drain with the board pull-up): asserted when low. */
static bool foc_at32_nfault_asserted(void *argument)
{
    (void)argument;
    return gpio_input_data_bit_read(GPIOB, GPIO_PINS_2) == RESET;
}

/* PLL from the board's 8 MHz crystal: 8 MHz x 75 / 4 = 150 MHz. If the crystal does not
 * start, HICK / 2 (4 MHz x 150 / 4) gives the same 150 MHz so the board still boots and
 * answers on USART1; clock_from_crystal records which source runs. */
static bool foc_at32_clock_init(void)
{
    /* Flash access wait states only. This path never erases or programs Flash. */
    flash_psr_set(FLASH_WAIT_CYCLE_4);
    crm_clock_source_enable(CRM_CLOCK_SOURCE_HEXT, TRUE);
    uint32_t remaining = FOC_STARTUP_POLL_LIMIT;
    while (crm_flag_get(CRM_HEXT_STABLE_FLAG) != SET && remaining != 0U) {
        remaining--;
    }
    clock_from_crystal = remaining != 0U;
    if (clock_from_crystal) {
        crm_pll_config2(CRM_PLL_SOURCE_HEXT, 75, 1, CRM_PLL_FR_4);
    } else {
        crm_clock_source_enable(CRM_CLOCK_SOURCE_HEXT, FALSE);
        crm_pll_config2(CRM_PLL_SOURCE_HICK, 150, 1, CRM_PLL_FR_4);
    }
    crm_clock_source_enable(CRM_CLOCK_SOURCE_PLL, TRUE);
    remaining = FOC_STARTUP_POLL_LIMIT;
    while (crm_flag_get(CRM_PLL_STABLE_FLAG) != SET && remaining != 0U) {
        remaining--;
    }
    if (remaining == 0U) {
        return false;
    }
    crm_ahb_div_set(CRM_AHB_DIV_1);
    crm_apb1_div_set(CRM_APB1_DIV_2);
    crm_apb2_div_set(CRM_APB2_DIV_2);
    crm_auto_step_mode_enable(TRUE);
    crm_sysclk_switch(CRM_SCLK_PLL);
    remaining = FOC_STARTUP_POLL_LIMIT;
    while (crm_sysclk_switch_status_get() != CRM_SCLK_PLL && remaining != 0U) {
        remaining--;
    }
    crm_auto_step_mode_enable(FALSE);
    system_core_clock_update();
    return remaining != 0U && system_core_clock == FOC_BOARD_TIMER_HZ;
}

/* Select the configuration after the output lock and before any producer or IRQ. */
static bool foc_at32_boot(void)
{
    const struct foc_board_calibration_expectation expected = foc_board_calibration_expectation();
    return foc_board_boot_select(
               &boot,
               &runtime,
               &output,
               &encoder,
               foc_board_calibration_supply(),
               &expected,
               0U
           ) == FOC_RUNTIME_OK;
}

static bool foc_at32_adc_init(void)
{
    gpio_init_type pins;
    gpio_default_para_init(&pins);
    pins.gpio_mode = GPIO_MODE_ANALOG;
    pins.gpio_pins = GPIO_PINS_0 | GPIO_PINS_4 | GPIO_PINS_5 | GPIO_PINS_6;
    gpio_init(GPIOA, &pins);

    crm_periph_clock_enable(CRM_ADC1_PERIPH_CLOCK, TRUE);
    adc_reset(ADC1);
    crm_adc_clock_div_set(CRM_ADC_DIV_6);
    adc_base_config_type config;
    adc_base_default_para_init(&config);
    config.sequence_mode = TRUE;
    config.ordinary_channel_length = 1;
    adc_base_config(ADC1, &config);
    adc_preempt_channel_length_set(ADC1, 4);
    /* VBUS (PA0), IC (PA4), IB (PA5), IA (PA6) in physical driver phase order. */
    adc_preempt_channel_set(ADC1, ADC_CHANNEL_0, 1, ADC_SAMPLETIME_7_5);
    adc_preempt_channel_set(ADC1, ADC_CHANNEL_4, 2, ADC_SAMPLETIME_7_5);
    adc_preempt_channel_set(ADC1, ADC_CHANNEL_5, 3, ADC_SAMPLETIME_7_5);
    adc_preempt_channel_set(ADC1, ADC_CHANNEL_6, 4, ADC_SAMPLETIME_7_5);
    adc_preempt_conversion_trigger_set(ADC1, ADC12_PREEMPT_TRIG_SOFTWARE, TRUE);
    /* Ordinary group: one software-triggered phase-voltage channel at a time (coast check). */
    adc_ordinary_conversion_trigger_set(ADC1, ADC12_ORDINARY_TRIG_SOFTWARE, TRUE);
    adc_enable(ADC1, TRUE);

    adc_calibration_init(ADC1);
    uint32_t remaining = FOC_STARTUP_POLL_LIMIT;
    while (adc_calibration_init_status_get(ADC1) && remaining != 0U) {
        remaining--;
    }
    if (remaining == 0U) {
        return false;
    }
    adc_calibration_start(ADC1);
    remaining = FOC_STARTUP_POLL_LIMIT;
    while (adc_calibration_status_get(ADC1) && remaining != 0U) {
        remaining--;
    }
    adc_flag_clear(ADC1, ADC_PCCE_FLAG);
    adc_interrupt_enable(ADC1, ADC_PCCE_INT, TRUE);
    return remaining != 0U;
}

static void foc_at32_spi_reset(void)
{
    crm_periph_reset(CRM_SPI1_PERIPH_RESET, TRUE);
    crm_periph_reset(CRM_SPI1_PERIPH_RESET, FALSE);
    spi_init_type config;
    spi_default_para_init(&config);
    config.transmission_mode = SPI_TRANSMIT_FULL_DUPLEX;
    config.master_slave_mode = SPI_MODE_MASTER;
    config.frame_bit_num = SPI_FRAME_16BIT;
    config.first_bit_transmission = SPI_FIRST_BIT_MSB;
    config.mclk_freq_division = SPI_MCLK_DIV_16;
    config.clock_polarity = SPI_CLOCK_POLARITY_HIGH;
    config.clock_phase = SPI_CLOCK_PHASE_2EDGE;
    config.cs_mode_selection = SPI_CS_SOFTWARE_MODE;
    spi_init(SPI1, &config);
    spi_software_cs_internal_level_set(SPI1, SPI_SWCS_INTERNAL_LEVEL_HIGHT);
    spi_enable(SPI1, TRUE);
}

static void foc_at32_spi_init(void)
{
    crm_periph_clock_enable(CRM_IOMUX_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_SPI1_PERIPH_CLOCK, TRUE);
    gpio_pin_remap_config(SWJTAG_GMUX_010, TRUE);
    gpio_pin_remap_config(SPI1_GMUX_0001, TRUE);
    gpio_bits_set(GPIOA, FOC_ENCODER_CS);
    gpio_init_type pins;
    gpio_default_para_init(&pins);
    pins.gpio_mode = GPIO_MODE_OUTPUT;
    pins.gpio_pins = FOC_ENCODER_CS;
    gpio_init(GPIOA, &pins);
    pins.gpio_mode = GPIO_MODE_MUX;
    pins.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    pins.gpio_pins = GPIO_PINS_3 | GPIO_PINS_5;
    gpio_init(GPIOB, &pins);
    pins.gpio_mode = GPIO_MODE_INPUT;
    pins.gpio_pull = GPIO_PULL_UP;
    pins.gpio_pins = GPIO_PINS_4;
    gpio_init(GPIOB, &pins);
    foc_at32_spi_reset();
}

static void foc_at32_timers_init(void)
{
    crm_periph_clock_enable(CRM_TMR2_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_TMR5_PERIPH_CLOCK, TRUE);
    tmr_32_bit_function_enable(TMR2, TRUE);
    tmr_base_init(TMR2, UINT32_MAX, FOC_TIMER_TICKS_PER_US - 1U);
    /* Internal sticky midpoint and overflow witnesses, without a TMR2 IRQ. */
    TMR2->c1dt = FOC_TIME_HALF_MARK;
    tmr_counter_value_set(TMR2, 0U);
    tmr_flag_clear(TMR2, FOC_TIME_FLAGS);

    /* Only channel 4 compare events. No GPIO mux, channel enable, or MOE. */
    tmr_base_init(TMR1, FOC_BOARD_COMPARE_MAX, 0U);
    TMR1->c4dt = FOC_BOARD_ADC_COMPARE_US * FOC_TIMER_TICKS_PER_US;
    tmr_counter_value_set(TMR1, 0U);
    tmr_flag_clear(TMR1, TMR_C4_FLAG | TMR_OVF_FLAG);
    tmr_interrupt_enable(TMR1, TMR_C4_INT, TRUE);

    tmr_base_init(TMR5, FOC_BOARD_H1_US - 1U, FOC_TIMER_TICKS_PER_US - 1U);
    tmr_channel_value_set(TMR5, TMR_SELECT_CHANNEL_1, FOC_BOARD_SPI_SEND_US);
    tmr_channel_value_set(TMR5, TMR_SELECT_CHANNEL_2, FOC_BOARD_SPI_CLOSE_US);
    tmr_counter_value_set(TMR5, FOC_BOARD_H1_US - 1U);
    tmr_flag_clear(TMR5, FOC_TMR5_EVENTS);
    tmr_interrupt_enable(TMR5, TMR_OVF_INT | TMR_C1_INT | TMR_C2_INT, TRUE);
}

static void foc_at32_irq_enable(void)
{
    /* Four priority bits, all used for preemption. Pairs serialize their own state. */
    NVIC_SetPriorityGrouping(0U);
    NVIC_SetPriority(TMR1_CH_IRQn, 0U);
    NVIC_SetPriority(ADC1_IRQn, 0U);
    NVIC_SetPriority(TMR5_GLOBAL_IRQn, 1U);
    NVIC_SetPriority(SPI1_IRQn, 1U);
    NVIC_SetPriority(USART1_IRQn, 2U);
    NVIC_SetPriority(CAN1_RX0_IRQn, 2U);
    NVIC_ClearPendingIRQ(TMR1_CH_IRQn);
    NVIC_ClearPendingIRQ(ADC1_IRQn);
    NVIC_ClearPendingIRQ(TMR5_GLOBAL_IRQn);
    NVIC_ClearPendingIRQ(SPI1_IRQn);
    NVIC_ClearPendingIRQ(USART1_IRQn);
    NVIC_ClearPendingIRQ(CAN1_RX0_IRQn);
    NVIC_EnableIRQ(TMR1_CH_IRQn);
    NVIC_EnableIRQ(ADC1_IRQn);
    NVIC_EnableIRQ(TMR5_GLOBAL_IRQn);
    NVIC_EnableIRQ(SPI1_IRQn);
    NVIC_EnableIRQ(USART1_IRQn);
    if (can_started) {
        NVIC_EnableIRQ(CAN1_RX0_IRQn);
    }
}

static void foc_at32_spi_finish(
    uint32_t phase_us,
    uint32_t before_phase_us,
    enum foc_board_encoder_result result
)
{
    spi_i2s_interrupt_enable(SPI1, FOC_SPI_IRQS, FALSE);
    gpio_bits_set(GPIOA, FOC_ENCODER_CS);
    const uint32_t closed_us = foc_at32_now_us();
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    rt_trace.cs_release_cycles = foc_at32_trace_enter();
#endif
    if (result == FOC_BOARD_ENCODER_OK &&
        !foc_board_spi_on_time(FOC_BOARD_SPI_CLOSE, phase_us, before_phase_us, closed_us)) {
        result = FOC_BOARD_ENCODER_TIMEOUT;
    }
    spi_phase = FOC_SPI_IDLE;
    if (result != FOC_BOARD_ENCODER_OK) {
        foc_at32_spi_reset();
    }
#if defined(FALCONFOC_BOARD_CAPTURE)
    const int h1 = foc_board_encoder_complete(&runtime, &encoder, closed_us, result, spi_word);
    foc_at32_capture_encoder(closed_us, result, h1);
#else
    (void)foc_board_encoder_complete(&runtime, &encoder, closed_us, result, spi_word);
#endif
}

/* Public Functions ------------------------------------------------------------- */

int main(void)
{
    __disable_irq();
    foc_at32_lock_output();
    foc_at32_board_pins_init();
    if (foc_at32_clock_init() && foc_at32_boot() && foc_at32_adc_init()) {
        adc.driver_fault_line = foc_at32_nfault_asserted;
        foc_at32_spi_init();
        foc_at32_timers_init();
        foc_serial_init(&serial, &runtime);
        foc_serial_attach_boot(&serial, foc_board_boot_serial_readback, &boot);
        /* The locked image answers CALIBRATE_START with OUTPUT_LOCKED: CAL-2 must drive. */
        foc_board_calibration_bind(
            &calibration,
            &runtime,
            &output,
            &encoder,
            &adc,
            &serial,
            foc_at32_quiesce,
            NULL
        );
        const struct foc_serial_calibration calibrate = foc_board_calibration_serial(&calibration);
        foc_serial_attach_calibration(&serial, &calibrate);
        foc_board_calibration_attach_phase_voltage(&calibration, foc_at32_phase_voltage_read, NULL);
        /* The boot configuration and tuning CONFIG_ERASE returns to, before any restore. */
        foc_board_calibration_capture_boot(&calibration, &boot);
#if defined(FALCONFOC_BOARD_PERSISTENCE)
        /* Restore saved settings before any producer or IRQ starts. */
        persistence.driver = foc_at32_store_driver();
        foc_board_persistence_restore(&persistence, &boot, &runtime, &output, &encoder, &serial);
        const struct foc_serial_persistence store = foc_board_persistence_serial(&persistence);
        foc_serial_attach_persistence(&serial, &store);
        if (persistence.calibration_restored) {
            foc_board_calibration_adopt(&calibration, &persistence.restored_calibration);
        }
        persistence.calibration_source = &calibration.snapshot;
#endif
        foc_at32_uart_init();
        /* After the restore: the saved bit rate applies from this boot. */
        foc_at32_can_init(serial.can.bitrate);
        foc_at32_irq_enable();
#if defined(FALCONFOC_RT02_TIMING_TRACE)
        foc_at32_trace_init();
#endif
        tmr_counter_enable(TMR2, TRUE);
        tmr_counter_enable(TMR5, TRUE);
        const uint32_t initial_phase = TMR1->cval;
        const uint32_t epoch_before = foc_at32_now_us();
        tmr_counter_enable(TMR1, TRUE);
        if (foc_board_adc_init_timing(&adc, epoch_before, foc_at32_now_us(), initial_phase)) {
            __enable_irq();
            struct foc_runtime_foreground_result result;
            for (;;) {
#if defined(FALCONFOC_RT02_TIMING_TRACE)
                const uint32_t supervision_start = foc_at32_trace_enter();
                const int supervision_status =
                    foc_runtime_foreground_poll(&runtime, foc_at32_now_us(), 0U, 0U, &result);
                if (supervision_status != FOC_RUNTIME_IDLE) {
                    foc_at32_trace_exit(
                        &rt_trace.supervision,
                        supervision_start,
                        runtime.supervision.due_count,
                        (uint32_t)supervision_status
                    );
                }
                const uint32_t serial_start = foc_at32_trace_enter();
#else
                (void)foc_runtime_foreground_poll(&runtime, foc_at32_now_us(), 0U, 0U, &result);
#endif
                foc_serial_poll(&serial, &uart, &runtime, foc_at32_serial_now, NULL);
                if (can_started) {
                    foc_can_poll(&can, &serial, &runtime, foc_at32_now_us());
                }
                foc_board_calibration_poll(&calibration, foc_at32_now_us());
                if (!foc_uart_tx_available(&uart)) {
                    usart_interrupt_enable(USART1, USART_TDBE_INT, TRUE);
                }
#if defined(FALCONFOC_RT02_TIMING_TRACE)
                foc_at32_trace_exit(&rt_trace.serial, serial_start, 0U, 0U);
#endif
#if defined(FALCONFOC_BOARD_CAPTURE)
                foc_at32_capture_foreground();
#endif
            }
        }
    }
    for (;;) {
        __WFI();
    }
}

void TMR1_CH_IRQHandler(void)
{
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    const uint32_t entry_cycles = foc_at32_trace_enter();
    const uint32_t entry_phase = adc.trace_cycles != NULL ? TMR1->cval : 0U;
#endif
    if (tmr_flag_get(TMR1, TMR_C4_FLAG) == RESET) {
#if defined(FALCONFOC_RT02_TIMING_TRACE)
        foc_at32_trace_exit(&rt_trace.trigger, entry_cycles, 0U, 0U);
#endif
        return;
    }
    struct foc_board_adc_event event = foc_at32_adc_event();
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    event.irq_entry_cycles = entry_cycles;
    event.irq_entry_phase_ticks = entry_phase;
#endif
    /* Acknowledge after the phase read so a compare within its window
     * cannot be issued once as stale work and again as a new IRQ. */
    tmr_flag_clear(TMR1, TMR_C4_FLAG);
    if (foc_board_adc_begin(&runtime, &adc, &event, adc_flag_get(ADC1, ADC_PCCE_FLAG) != RESET)) {
#if defined(FALCONFOC_RT02_TIMING_TRACE)
        if (adc.trace_cycles != NULL) {
            adc.live.transaction.trace.points |= FOC_BOARD_TRACE_SOFTWARE_TRIGGER;
            adc.live.transaction.trace.trigger_proxy_cycles = foc_at32_trace_cycles();
        }
#endif
        adc_preempt_software_trigger_enable(ADC1, TRUE);
#if defined(FALCONFOC_RT02_TIMING_TRACE)
        adc.live.transaction.trace.trigger_write_return_cycles = foc_at32_trace_enter();
#endif
    }
    foc_board_adc_publish(&adc);
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    foc_at32_trace_exit(&rt_trace.trigger, entry_cycles, adc.live.slot_id, adc.live.last_rejection);
#endif
}

void ADC1_IRQHandler(void)
{
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    const uint32_t entry_cycles = foc_at32_trace_enter();
#endif
    if (adc_flag_get(ADC1, ADC_PCCE_FLAG) == RESET) {
#if defined(FALCONFOC_RT02_TIMING_TRACE)
        foc_at32_trace_exit(&rt_trace.adc, entry_cycles, 0U, 0U);
#endif
        return;
    }
    struct foc_board_adc_event event = foc_at32_adc_event();
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    event.irq_entry_cycles = entry_cycles;
#endif
    const uint16_t ranks[] = {
        adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_1),
        adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_2),
        adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_3),
        adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_4),
    };
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    event.ranks_ready_cycles = foc_at32_trace_enter();
#endif
    adc_flag_clear(ADC1, ADC_PCCE_FLAG);
#if defined(FALCONFOC_BOARD_CAPTURE)
    const enum foc_board_adc_state token_state = adc.live.transaction.state;
    const bool drained = adc_flag_get(ADC1, ADC_PCCE_FLAG) == RESET;
    const int result = foc_board_adc_complete(
        &runtime,
        &adc,
        &output,
        ranks,
        sizeof ranks / sizeof ranks[0],
        &event,
        drained
    );
    foc_at32_capture_adc(
        ranks,
        &event,
        token_state == FOC_BOARD_ADC_PENDING || token_state == FOC_BOARD_ADC_DRAIN,
        drained,
        result
    );
#else
    (void)foc_board_adc_complete(
        &runtime,
        &adc,
        &output,
        ranks,
        sizeof ranks / sizeof ranks[0],
        &event,
        adc_flag_get(ADC1, ADC_PCCE_FLAG) == RESET
    );
#endif
    foc_board_adc_publish(&adc);
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    foc_at32_trace_exit(&rt_trace.adc, entry_cycles, adc.live.slot_id, adc.live.last_rejection);
#endif
}

static void foc_at32_tmr5_service(uint32_t events)
{
    tmr_flag_clear(TMR5, events);
    const uint32_t before_phase_us = foc_at32_now_us();
    const uint32_t phase_us = tmr_counter_value_get(TMR5);
    const uint32_t now_us = foc_at32_now_us();
    if ((events & TMR_OVF_FLAG) != 0U) {
        if (encoder.pending) {
            (void)foc_board_encoder_begin(&encoder, now_us);
            foc_at32_spi_finish(phase_us, before_phase_us, FOC_BOARD_ENCODER_OVERLAP);
            return;
        }
        if (foc_board_encoder_begin(&encoder, now_us)) {
#if defined(FALCONFOC_BOARD_CAPTURE)
            capture_cs_seen = false;
            capture_rx_seen = false;
            capture_spi_status = 0U;
#endif
            if (events == TMR_OVF_FLAG &&
                foc_board_spi_on_time(
                    FOC_BOARD_SPI_CS,
                    phase_us,
                    before_phase_us,
                    foc_at32_now_us()
                )) {
                gpio_bits_reset(GPIOA, FOC_ENCODER_CS);
                const uint32_t selected_us = foc_at32_now_us();
#if defined(FALCONFOC_BOARD_CAPTURE)
                capture_cs_seen = true;
                capture_cs_us = selected_us;
#endif
#if defined(FALCONFOC_RT02_TIMING_TRACE)
                rt_trace.cs_assert_cycles = foc_at32_trace_enter();
#endif
                if (foc_board_spi_on_time(
                        FOC_BOARD_SPI_CS,
                        phase_us,
                        before_phase_us,
                        selected_us
                    )) {
                    encoder.captured_us = selected_us;
                    spi_phase = FOC_SPI_SELECTED;
                    return;
                }
            }
            foc_at32_spi_finish(phase_us, before_phase_us, FOC_BOARD_ENCODER_TIMEOUT);
        }
    } else if ((events & TMR_C2_FLAG) != 0U && encoder.pending) {
        const uint32_t status = SPI1->sts;
#if defined(FALCONFOC_BOARD_CAPTURE)
        capture_spi_status |= status;
#endif
        const bool error = spi_phase == FOC_SPI_ERROR ||
            (status & (FOC_SPI_ERROR_FLAGS | SPI_I2S_RDBF_FLAG)) != 0U;
        const enum foc_board_encoder_result result = foc_board_spi_close_result(
            phase_us,
            before_phase_us,
            foc_at32_now_us(),
            spi_rx_us,
            spi_phase == FOC_SPI_RECEIVED,
            (status & SPI_I2S_BF_FLAG) != 0U,
            error
        );
        foc_at32_spi_finish(phase_us, before_phase_us, result);
    } else if ((events & TMR_C1_FLAG) != 0U && spi_phase == FOC_SPI_SELECTED) {
        const uint32_t status = SPI1->sts;
#if defined(FALCONFOC_BOARD_CAPTURE)
        capture_spi_status |= status;
#endif
        if ((status & SPI_I2S_TDBE_FLAG) == 0U ||
            !foc_board_spi_on_time(
                FOC_BOARD_SPI_SEND,
                phase_us,
                before_phase_us,
                foc_at32_now_us()
            ) ||
            (status & (FOC_SPI_ERROR_FLAGS | SPI_I2S_RDBF_FLAG | SPI_I2S_BF_FLAG)) != 0U) {
            spi_phase = FOC_SPI_ERROR;
            return;
        }
        spi_phase = FOC_SPI_SENT;
        spi_i2s_interrupt_enable(SPI1, FOC_SPI_IRQS, TRUE);
        spi_i2s_data_transmit(SPI1, 0U);
#if defined(FALCONFOC_RT02_TIMING_TRACE)
        rt_trace.tx_cycles = foc_at32_trace_enter();
#endif
        if (!foc_board_spi_on_time(
                FOC_BOARD_SPI_SEND,
                phase_us,
                before_phase_us,
                foc_at32_now_us()
            )) {
            spi_phase = FOC_SPI_ERROR;
            spi_i2s_interrupt_enable(SPI1, FOC_SPI_IRQS, FALSE);
        }
    }
}

void TMR5_GLOBAL_IRQHandler(void)
{
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    const uint32_t start = foc_at32_trace_enter();
    const uint32_t identity = encoder.seq;
#endif
    const uint32_t events = TMR5->ists & FOC_TMR5_EVENTS;
    foc_at32_tmr5_service(events);
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    const unsigned int point = (events & TMR_OVF_FLAG) != 0U ? FOC_BOARD_SPI_CS
        : (events & TMR_C2_FLAG) != 0U                       ? FOC_BOARD_SPI_CLOSE
                                                             : FOC_BOARD_SPI_SEND;
    foc_at32_trace_exit(&rt_trace.tmr5[point], start, identity, events);
#endif
}

static void foc_at32_spi_service(uint32_t status)
{
    if ((status & (SPI_I2S_RDBF_FLAG | FOC_SPI_ERROR_FLAGS)) == 0U) {
        return;
    }
#if defined(FALCONFOC_BOARD_CAPTURE)
    capture_spi_status |= status;
#endif
    spi_i2s_interrupt_enable(SPI1, FOC_SPI_IRQS, FALSE);
    if (spi_phase != FOC_SPI_SENT || (status & FOC_SPI_ERROR_FLAGS) != 0U) {
        spi_phase = FOC_SPI_ERROR;
        return;
    }
    spi_word = spi_i2s_data_receive(SPI1);
    spi_rx_us = foc_at32_now_us();
#if defined(FALCONFOC_BOARD_CAPTURE)
    capture_rx_seen = true;
#endif
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    rt_trace.rx_cycles = foc_at32_trace_enter();
#endif
    spi_phase = FOC_SPI_RECEIVED;
}

void SPI1_IRQHandler(void)
{
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    const uint32_t start = foc_at32_trace_enter();
#endif
    const uint32_t status = SPI1->sts;
    foc_at32_spi_service(status);
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    foc_at32_trace_exit(&rt_trace.spi, start, encoder.seq, status);
#endif
}

void USART1_IRQHandler(void)
{
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    const uint32_t start = foc_at32_trace_enter();
#endif
    const uint32_t status = USART1->sts;
    const uint32_t errors = USART_PERR_FLAG | USART_FERR_FLAG | USART_NERR_FLAG | USART_ROERR_FLAG;
    if ((status & (USART_RDBF_FLAG | errors)) != 0U) {
        /* Status then data clears RX errors according to the SDK contract. */
        const uint8_t byte = (uint8_t)usart_data_receive(USART1);
        foc_uart_rx_push(&uart, byte, (status & errors) != 0U);
    }
    if (usart_interrupt_flag_get(USART1, USART_TDBE_FLAG) != RESET) {
        uint8_t byte;
        if (foc_uart_tx_pop(&uart, &byte)) {
            usart_data_transmit(USART1, byte);
        }
        if (foc_uart_tx_available(&uart)) {
            usart_interrupt_enable(USART1, USART_TDBE_INT, FALSE);
        }
    }
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    foc_at32_trace_exit(&rt_trace.usart, start, 0U, status);
#endif
}

/* Move every pending standard frame of FIFO 0 into the RX ring; extended frames are dropped. */
void CAN1_RX0_IRQHandler(void)
{
    while (can_receive_message_pending_get(CAN1, CAN_RX_FIFO0) != 0U) {
        can_rx_message_type message;
        can_message_receive(CAN1, CAN_RX_FIFO0, &message);
        if (message.id_type != CAN_ID_STANDARD) {
            continue;
        }
        struct foc_can_frame frame = {
            .id = (uint16_t)(message.standard_id & 0x7FFU),
            .length = message.dlc > 8U ? 8U : message.dlc,
            .rtr = message.frame_type == CAN_TFT_REMOTE,
        };
        for (unsigned int index = 0U; index < frame.length; ++index) {
            frame.data[index] = message.data[index];
        }
        foc_can_rx_push(&can_buffer, &frame);
    }
}

/* ---------------------------------------- end of file ----------------------------------------- */
