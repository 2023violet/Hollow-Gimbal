[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot
)

$ErrorActionPreference = 'Stop'

$root = (Resolve-Path -LiteralPath $RepoRoot).Path
$failures = [System.Collections.Generic.List[string]]::new()

function Add-Failure {
    param([string]$Message)
    [void]$failures.Add($Message)
    Write-Output "[FAIL] $Message"
}

function Test-Contract {
    param(
        [string]$RelativePath,
        [string]$Name,
        [string[]]$Patterns
    )

    $path = Join-Path $root $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Add-Failure "${Name}: missing file ${RelativePath}"
        return
    }

    $content = [System.IO.File]::ReadAllText($path)
    foreach ($pattern in $Patterns) {
        if (-not [System.Text.RegularExpressions.Regex]::IsMatch(
                $content,
                $pattern,
                [System.Text.RegularExpressions.RegexOptions]::Singleline)) {
            Add-Failure "${Name}: pattern not found in ${RelativePath}: ${pattern}"
        }
    }
}

Test-Contract `
    'platform/at/at32f415/generated/src/at32f415_wk_config.c' `
    'clock/NVIC' `
    @(
        'sclk\s*=\s*150000000',
        'crm_pll_config2\(CRM_PLL_SOURCE_HICK,\s*150,\s*1,\s*CRM_PLL_FR_4\)',
        'crm_apb2_div_set\(CRM_APB2_DIV_2\)',
        'crm_apb1_div_set\(CRM_APB1_DIV_2\)',
        'nvic_irq_enable\(ADC1_IRQn,\s*5,\s*0\)',
        'nvic_irq_enable\(USART1_IRQn,\s*7,\s*0\)',
        'nvic_irq_enable\(USART2_IRQn,\s*8,\s*0\)',
        'nvic_irq_enable\(TMR5_GLOBAL_IRQn,\s*6,\s*0\)'
    )

Test-Contract `
    'platform/at/at32f415/generated/src/wk_adc.c' `
    'ADC' `
    @(
        'crm_adc_clock_div_set\(CRM_ADC_DIV_4\)',
        'adc_preempt_channel_length_set\(ADC1,\s*4\)',
        'adc_preempt_channel_set\(ADC1,\s*ADC_CHANNEL_1,\s*1,\s*ADC_SAMPLETIME_7_5\)',
        'adc_preempt_channel_set\(ADC1,\s*ADC_CHANNEL_4,\s*2,\s*ADC_SAMPLETIME_7_5\)',
        'adc_preempt_channel_set\(ADC1,\s*ADC_CHANNEL_5,\s*3,\s*ADC_SAMPLETIME_7_5\)',
        'adc_preempt_channel_set\(ADC1,\s*ADC_CHANNEL_6,\s*4,\s*ADC_SAMPLETIME_7_5\)',
        'adc_preempt_conversion_trigger_set\(ADC1,\s*ADC12_PREEMPT_TRIG_TMR1CH4,\s*TRUE\)'
    )

Test-Contract `
    'platform/at/at32f415/generated/inc/at32f415_wk_config.h' `
    'ADC pin map' `
    @(
        '#define\s+VBUS_PIN\s+GPIO_PINS_1',
        '#define\s+VBUS_GPIO_PORT\s+GPIOA',
        '#define\s+IC_PIN\s+GPIO_PINS_4',
        '#define\s+IC_GPIO_PORT\s+GPIOA',
        '#define\s+IB_PIN\s+GPIO_PINS_5',
        '#define\s+IB_GPIO_PORT\s+GPIOA',
        '#define\s+IA_PIN\s+GPIO_PINS_6',
        '#define\s+IA_GPIO_PORT\s+GPIOA'
    )

Test-Contract `
    'platform/at/at32f415/generated/src/wk_tmr.c' `
    'TMR1 PWM/trigger' `
    @(
        'tmr_cnt_dir_set\(TMR1,\s*TMR_COUNT_TWO_WAY_1\)',
        'tmr_base_init\(TMR1,\s*3749,\s*0\)',
        'tmr_output_channel_config\(TMR1,\s*TMR_SELECT_CHANNEL_1,\s*&tmr_output_struct\)',
        'tmr_output_channel_config\(TMR1,\s*TMR_SELECT_CHANNEL_2,\s*&tmr_output_struct\)',
        'tmr_output_channel_config\(TMR1,\s*TMR_SELECT_CHANNEL_3,\s*&tmr_output_struct\)',
        'tmr_output_struct\.oc_output_state\s*=\s*TRUE;[\s\S]{0,300}tmr_output_struct\.occ_output_state\s*=\s*TRUE;[\s\S]{0,300}tmr_output_channel_config\(TMR1,\s*TMR_SELECT_CHANNEL_1',
        'tmr_output_struct\.oc_output_state\s*=\s*TRUE;[\s\S]{0,300}tmr_output_struct\.occ_output_state\s*=\s*TRUE;[\s\S]{0,300}tmr_output_channel_config\(TMR1,\s*TMR_SELECT_CHANNEL_2',
        'tmr_output_struct\.oc_output_state\s*=\s*TRUE;[\s\S]{0,300}tmr_output_struct\.occ_output_state\s*=\s*TRUE;[\s\S]{0,300}tmr_output_channel_config\(TMR1,\s*TMR_SELECT_CHANNEL_3',
        'tmr_output_struct\.oc_output_state\s*=\s*TRUE;[\s\S]{0,300}tmr_output_struct\.occ_output_state\s*=\s*FALSE;[\s\S]{0,300}tmr_output_channel_config\(TMR1,\s*TMR_SELECT_CHANNEL_4',
        'tmr_channel_value_set\(TMR1,\s*TMR_SELECT_CHANNEL_4,\s*2700\)',
        'tmr_brkdt_struct\.brk_enable\s*=\s*FALSE',
        'tmr_brkdt_struct\.deadtime\s*=\s*0',
        'tmr_brkdt_struct\.auto_output_enable\s*=\s*FALSE'
    )

Test-Contract `
    'platform/at/at32f415/generated/src/wk_spi.c' `
    'SPI1' `
    @(
        'gpio_init_struct\.gpio_pins\s*=\s*GPIO_PINS_3;[\s\S]{0,500}gpio_init\(GPIOB,\s*&gpio_init_struct\)',
        'gpio_init_struct\.gpio_pins\s*=\s*GPIO_PINS_4;[\s\S]{0,500}gpio_init\(GPIOB,\s*&gpio_init_struct\)',
        'gpio_init_struct\.gpio_pins\s*=\s*GPIO_PINS_5;[\s\S]{0,500}gpio_init\(GPIOB,\s*&gpio_init_struct\)',
        'spi_init_struct\.transmission_mode\s*=\s*SPI_TRANSMIT_FULL_DUPLEX',
        'spi_init_struct\.master_slave_mode\s*=\s*SPI_MODE_MASTER',
        'spi_init_struct\.frame_bit_num\s*=\s*SPI_FRAME_16BIT',
        'spi_init_struct\.first_bit_transmission\s*=\s*SPI_FIRST_BIT_MSB',
        'spi_init_struct\.clock_polarity\s*=\s*SPI_CLOCK_POLARITY_HIGH',
        'spi_init_struct\.clock_phase\s*=\s*SPI_CLOCK_PHASE_2EDGE',
        'spi_init_struct\.cs_mode_selection\s*=\s*SPI_CS_SOFTWARE_MODE'
    )

Test-Contract `
    'platform/at/at32f415/generated/inc/at32f415_wk_config.h' `
    'SPI1 chip select' `
    @(
        '#define\s+SPI1_CS_PIN\s+GPIO_PINS_15',
        '#define\s+SPI1_CS_GPIO_PORT\s+GPIOA'
    )

Test-Contract `
    'applications/periph/motor_drv.c' `
    'FOC hardware mapping' `
    @(
        'tmr_channel_value_set\(\s*TMR1,\s*TMR_SELECT_CHANNEL_3,\s*\(uint32_t\)\(motor->foc_calc\.dtc_a',
        'tmr_channel_value_set\(\s*TMR1,\s*TMR_SELECT_CHANNEL_2,\s*\(uint32_t\)\(motor->foc_calc\.dtc_b',
        'tmr_channel_value_set\(\s*TMR1,\s*TMR_SELECT_CHANNEL_1,\s*\(uint32_t\)\(motor->foc_calc\.dtc_c',
        'motor->meas_data\.ia_raw\s*=\s*adc_preempt_conversion_data_get\(ADC1,\s*ADC_PREEMPT_CHANNEL_2\)',
        'motor->meas_data\.ib_raw\s*=\s*adc_preempt_conversion_data_get\(ADC1,\s*ADC_PREEMPT_CHANNEL_3\)',
        'motor->meas_data\.ic_raw\s*=\s*adc_preempt_conversion_data_get\(ADC1,\s*ADC_PREEMPT_CHANNEL_4\)',
        'motor->meas_data\.ic_raw\s*=\s*adc_preempt_conversion_data_get\(ADC1,\s*ADC_PREEMPT_CHANNEL_3\)',
        'motor->meas_data\.ib_raw\s*=\s*adc_preempt_conversion_data_get\(ADC1,\s*ADC_PREEMPT_CHANNEL_4\)',
        'meas_data\.c\s*=\s*FAC_CURR\(\(motor->meas_data\.ic_raw\s*-\s*motor->meas_data\.ic_offset\)\)',
        'meas_data\.b\s*=\s*FAC_CURR\(\(motor->meas_data\.ib_raw\s*-\s*motor->meas_data\.ib_offset\)\)',
        'meas_data\.a\s*=\s*FAC_CURR\(\(motor->meas_data\.ia_raw\s*-\s*motor->meas_data\.ia_offset\)\)'
    )

Test-Contract `
    'applications/periph/foc_ctrl.c' `
    'VBUS numeric guard' `
    @(
        'foc_vbus_numeric_valid\(v_bus\)\s*\?\s*\(1\.0f\s*/\s*v_bus\)\s*:\s*0\.0f',
        'if\s*\(!foc_vbus_numeric_valid\(motor->meas_data\.v_bus\)\)',
        'pwm_out_disable\(\);[\s\S]{0,80}return;'
    )

if ($failures.Count -gt 0) {
    Write-Output ("FOC_PERIPHERAL_CONTRACT=FAIL count={0}" -f $failures.Count)
    exit 1
}

Write-Output 'FOC_PERIPHERAL_CONTRACT=PASS'
exit 0
