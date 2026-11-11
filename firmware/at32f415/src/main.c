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
#include "foc_serial.h"

/* Private Macros --------------------------------------------------------------- */

#define FOC_TIMER_TICKS_PER_US (FOC_BOARD_TIMER_HZ / FOC_BOARD_US_HZ)
#define FOC_ADC_TRIGGER_US     40U
#define FOC_ADC_LATEST_US      110U
#define FOC_SPI_SEND_US        20U
#define FOC_SPI_FINISH_US      70U
#define FOC_SPI_LATEST_US      100U
#define FOC_SPI_ERROR_FLAGS    (SPI_MMERR_FLAG | SPI_I2S_ROERR_FLAG | SPI_CCERR_FLAG)
#define FOC_SPI_IRQS           (SPI_I2S_RDBF_INT | SPI_I2S_ERROR_INT)
#define FOC_TMR5_EVENTS        (TMR_OVF_FLAG | TMR_C1_FLAG | TMR_C2_FLAG)
#define FOC_ENCODER_CS         GPIO_PINS_15
#define FOC_STARTUP_POLL_LIMIT 100000U

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
static enum foc_spi_phase spi_phase;
static uint16_t spi_word;
static uint32_t spi_rx_us;
static struct foc_uart_buffer uart;
static struct foc_serial serial;

/* Private Helpers -------------------------------------------------------------- */

static uint32_t foc_at32_now_us(void)
{
    return tmr_counter_value_get(TMR2);
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

static bool foc_at32_clock_init(void)
{
    /* Flash access wait states only. This path never erases or programs Flash. */
    flash_psr_set(FLASH_WAIT_CYCLE_4);
    crm_pll_config2(CRM_PLL_SOURCE_HICK, 150, 1, CRM_PLL_FR_4);
    crm_clock_source_enable(CRM_CLOCK_SOURCE_PLL, TRUE);
    uint32_t remaining = FOC_STARTUP_POLL_LIMIT;
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

static bool foc_at32_adc_init(void)
{
    gpio_init_type pins;
    gpio_default_para_init(&pins);
    pins.gpio_mode = GPIO_MODE_ANALOG;
    pins.gpio_pins = GPIO_PINS_1 | GPIO_PINS_4 | GPIO_PINS_5 | GPIO_PINS_6;
    gpio_init(GPIOA, &pins);

    crm_periph_clock_enable(CRM_ADC1_PERIPH_CLOCK, TRUE);
    adc_reset(ADC1);
    crm_adc_clock_div_set(CRM_ADC_DIV_6);
    adc_base_config_type config;
    adc_base_default_para_init(&config);
    config.sequence_mode = TRUE;
    adc_base_config(ADC1, &config);
    adc_preempt_channel_length_set(ADC1, 4);
    /* VBUS, IC, IB, IA in physical driver phase order. */
    adc_preempt_channel_set(ADC1, ADC_CHANNEL_1, 1, ADC_SAMPLETIME_7_5);
    adc_preempt_channel_set(ADC1, ADC_CHANNEL_4, 2, ADC_SAMPLETIME_7_5);
    adc_preempt_channel_set(ADC1, ADC_CHANNEL_5, 3, ADC_SAMPLETIME_7_5);
    adc_preempt_channel_set(ADC1, ADC_CHANNEL_6, 4, ADC_SAMPLETIME_7_5);
    adc_preempt_conversion_trigger_set(ADC1, ADC12_PREEMPT_TRIG_SOFTWARE, TRUE);
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

    /* Only channel 4 compare events. No GPIO mux, channel enable, or MOE. */
    tmr_base_init(TMR1, FOC_BOARD_COMPARE_MAX, 0U);
    TMR1->c4dt = FOC_ADC_TRIGGER_US * FOC_TIMER_TICKS_PER_US;
    tmr_flag_clear(TMR1, TMR_C4_FLAG);
    tmr_interrupt_enable(TMR1, TMR_C4_INT, TRUE);

    tmr_base_init(TMR5, FOC_BOARD_H1_US - 1U, FOC_TIMER_TICKS_PER_US - 1U);
    tmr_channel_value_set(TMR5, TMR_SELECT_CHANNEL_1, FOC_SPI_SEND_US);
    tmr_channel_value_set(TMR5, TMR_SELECT_CHANNEL_2, FOC_SPI_FINISH_US);
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
    NVIC_ClearPendingIRQ(TMR1_CH_IRQn);
    NVIC_ClearPendingIRQ(ADC1_IRQn);
    NVIC_ClearPendingIRQ(TMR5_GLOBAL_IRQn);
    NVIC_ClearPendingIRQ(SPI1_IRQn);
    NVIC_ClearPendingIRQ(USART1_IRQn);
    NVIC_EnableIRQ(TMR1_CH_IRQn);
    NVIC_EnableIRQ(ADC1_IRQn);
    NVIC_EnableIRQ(TMR5_GLOBAL_IRQn);
    NVIC_EnableIRQ(SPI1_IRQn);
    NVIC_EnableIRQ(USART1_IRQn);
}

static void foc_at32_spi_finish(uint32_t now_us, enum foc_board_encoder_result result)
{
    spi_i2s_interrupt_enable(SPI1, FOC_SPI_IRQS, FALSE);
    gpio_bits_set(GPIOA, FOC_ENCODER_CS);
    spi_phase = FOC_SPI_IDLE;
    if (result != FOC_BOARD_ENCODER_OK) {
        foc_at32_spi_reset();
    }
    (void)foc_board_encoder_complete(&runtime, &encoder, now_us, result, spi_word);
}

/* Public Functions ------------------------------------------------------------- */

int main(void)
{
    __disable_irq();
    foc_at32_lock_output();
    if (foc_at32_clock_init() && foc_board_init(&runtime, &output, 0U) == FOC_RUNTIME_OK &&
        foc_at32_adc_init()) {
        foc_at32_spi_init();
        foc_at32_timers_init();
        foc_serial_init(&serial, &runtime);
        foc_at32_uart_init();
        foc_at32_irq_enable();
        tmr_counter_enable(TMR2, TRUE);
        tmr_counter_enable(TMR5, TRUE);
        tmr_counter_enable(TMR1, TRUE);
        __enable_irq();
        struct foc_runtime_foreground_result result;
        for (;;) {
            (void)foc_runtime_foreground_poll(&runtime, foc_at32_now_us(), 0U, 0U, &result);
            foc_serial_poll(&serial, &uart, &runtime, foc_at32_serial_now, NULL);
            if (!foc_uart_tx_available(&uart)) {
                usart_interrupt_enable(USART1, USART_TDBE_INT, TRUE);
            }
        }
    }
    for (;;) {
        __WFI();
    }
}

void TMR1_CH_IRQHandler(void)
{
    if (tmr_flag_get(TMR1, TMR_C4_FLAG) == RESET) {
        return;
    }
    tmr_flag_clear(TMR1, TMR_C4_FLAG);
    const uint32_t count = tmr_counter_value_get(TMR1);
    if (count < FOC_ADC_TRIGGER_US * FOC_TIMER_TICKS_PER_US ||
        count > FOC_ADC_LATEST_US * FOC_TIMER_TICKS_PER_US) {
        foc_runtime_request_inhibit(&runtime, FOC_SAFE_FAULT_FAST_HEARTBEAT);
        return;
    }
    if (foc_board_adc_begin(&runtime, &adc, foc_at32_now_us())) {
        adc_preempt_software_trigger_enable(ADC1, TRUE);
    }
}

void ADC1_IRQHandler(void)
{
    if (adc_flag_get(ADC1, ADC_PCCE_FLAG) == RESET) {
        return;
    }
    const uint32_t now_us = foc_at32_now_us();
    const uint16_t ranks[] = {
        adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_1),
        adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_2),
        adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_3),
        adc_preempt_conversion_data_get(ADC1, ADC_PREEMPT_CHANNEL_4),
    };
    adc_flag_clear(ADC1, ADC_PCCE_FLAG);
    (void)foc_board_adc_complete(&runtime, &adc, ranks, sizeof ranks / sizeof ranks[0], now_us);
}

void TMR5_GLOBAL_IRQHandler(void)
{
    const uint32_t events = TMR5->ists & FOC_TMR5_EVENTS;
    tmr_flag_clear(TMR5, events);
    const uint32_t phase_us = tmr_counter_value_get(TMR5);
    const uint32_t now_us = foc_at32_now_us();
    if ((events & TMR_OVF_FLAG) != 0U) {
        if (encoder.pending) {
            (void)foc_board_encoder_begin(&encoder, now_us);
            foc_at32_spi_finish(now_us, FOC_BOARD_ENCODER_OVERLAP);
            return;
        }
        if (foc_board_encoder_begin(&encoder, now_us)) {
            if (events == TMR_OVF_FLAG && phase_us <= 10U) {
                gpio_bits_reset(GPIOA, FOC_ENCODER_CS);
                spi_phase = FOC_SPI_SELECTED;
            } else {
                foc_at32_spi_finish(now_us, FOC_BOARD_ENCODER_TIMEOUT);
            }
        }
    } else if ((events & TMR_C2_FLAG) != 0U && encoder.pending) {
        const uint32_t status = SPI1->sts;
        const bool error = spi_phase == FOC_SPI_ERROR ||
            (status & (FOC_SPI_ERROR_FLAGS | SPI_I2S_RDBF_FLAG)) != 0U;
        /* At least two counter ticks leave a full microsecond of CS hold. */
        const bool timely = phase_us >= FOC_SPI_FINISH_US && phase_us <= FOC_SPI_LATEST_US &&
            (status & SPI_I2S_BF_FLAG) == 0U && now_us - spi_rx_us >= 2U;
        enum foc_board_encoder_result result = FOC_BOARD_ENCODER_TIMEOUT;
        if (error) {
            result = FOC_BOARD_ENCODER_FRAME;
        } else if (spi_phase == FOC_SPI_RECEIVED && timely) {
            result = FOC_BOARD_ENCODER_OK;
        }
        foc_at32_spi_finish(now_us, result);
    } else if ((events & TMR_C1_FLAG) != 0U && spi_phase == FOC_SPI_SELECTED) {
        const uint32_t status = SPI1->sts;
        if (phase_us < FOC_SPI_SEND_US || phase_us > 30U || (status & SPI_I2S_TDBE_FLAG) == 0U ||
            (status & (FOC_SPI_ERROR_FLAGS | SPI_I2S_RDBF_FLAG | SPI_I2S_BF_FLAG)) != 0U) {
            spi_phase = FOC_SPI_ERROR;
            return;
        }
        spi_phase = FOC_SPI_SENT;
        spi_i2s_interrupt_enable(SPI1, FOC_SPI_IRQS, TRUE);
        spi_i2s_data_transmit(SPI1, 0U);
    }
}

void SPI1_IRQHandler(void)
{
    const uint32_t status = SPI1->sts;
    if ((status & (SPI_I2S_RDBF_FLAG | FOC_SPI_ERROR_FLAGS)) == 0U) {
        return;
    }
    spi_i2s_interrupt_enable(SPI1, FOC_SPI_IRQS, FALSE);
    if (spi_phase != FOC_SPI_SENT || (status & FOC_SPI_ERROR_FLAGS) != 0U) {
        spi_phase = FOC_SPI_ERROR;
        return;
    }
    spi_word = spi_i2s_data_receive(SPI1);
    spi_rx_us = foc_at32_now_us();
    spi_phase = FOC_SPI_RECEIVED;
}

void USART1_IRQHandler(void)
{
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
}

/* ---------------------------------------- end of file ----------------------------------------- */
