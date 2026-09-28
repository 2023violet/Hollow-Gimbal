/* add user code begin Header */
/**
  **************************************************************************
  * @file     wk_adc.c
  * @brief    work bench config program
  **************************************************************************
  * Copyright (c) 2025, Artery Technology, All rights reserved.
  *
  * The software Board Support Package (BSP) that is made available to
  * download from Artery official website is the copyrighted work of Artery.
  * Artery authorizes customers to use, copy, and distribute the BSP
  * software and its related documentation for the purpose of design and
  * development in conjunction with Artery microcontrollers. Use of the
  * software is governed by this copyright notice and the following disclaimer.
  *
  * THIS SOFTWARE IS PROVIDED ON "AS IS" BASIS WITHOUT WARRANTIES,
  * GUARANTEES OR REPRESENTATIONS OF ANY KIND. ARTERY EXPRESSLY DISCLAIMS,
  * TO THE FULLEST EXTENT PERMITTED BY LAW, ALL EXPRESS, IMPLIED OR
  * STATUTORY OR OTHER WARRANTIES, GUARANTEES OR REPRESENTATIONS,
  * INCLUDING BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY,
  * FITNESS FOR A PARTICULAR PURPOSE, OR NON-INFRINGEMENT.
  *
  **************************************************************************
  */
/* add user code end Header */

/* Includes ------------------------------------------------------------------*/
#include "wk_adc.h"

/* add user code begin 0 */

/* add user code end 0 */

/**
  * @brief  init adc1 function.
  * @param  none
  * @retval none
  */
void wk_adc1_init(void)
{
  /* add user code begin adc1_init 0 */

  /* add user code end adc1_init 0 */

  gpio_init_type gpio_init_struct;
  adc_base_config_type adc_base_struct;

  gpio_default_para_init(&gpio_init_struct);

  /* add user code begin adc1_init 1 */

  /* add user code end adc1_init 1 */

  /*gpio--------------------------------------------------------------------*/ 
  /* configure the IN1 pin */
  gpio_init_struct.gpio_mode = GPIO_MODE_ANALOG;
  gpio_init_struct.gpio_pins = VBUS_PIN;
  gpio_init(VBUS_GPIO_PORT, &gpio_init_struct);

  /* configure the IN4 pin */
  gpio_init_struct.gpio_mode = GPIO_MODE_ANALOG;
  gpio_init_struct.gpio_pins = IC_PIN;
  gpio_init(IC_GPIO_PORT, &gpio_init_struct);

  /* configure the IN5 pin */
  gpio_init_struct.gpio_mode = GPIO_MODE_ANALOG;
  gpio_init_struct.gpio_pins = IB_PIN;
  gpio_init(IB_GPIO_PORT, &gpio_init_struct);

  /* configure the IN6 pin */
  gpio_init_struct.gpio_mode = GPIO_MODE_ANALOG;
  gpio_init_struct.gpio_pins = IA_PIN;
  gpio_init(IA_GPIO_PORT, &gpio_init_struct);

  adc_reset(ADC1);
  crm_adc_clock_div_set(CRM_ADC_DIV_4);

  /*adc_settings--------------------------------------------------------------------*/ 
  adc_base_default_para_init(&adc_base_struct);
  adc_base_struct.sequence_mode = TRUE;
  adc_base_struct.repeat_mode = FALSE;
  adc_base_struct.data_align = ADC_RIGHT_ALIGNMENT;
  adc_base_struct.ordinary_channel_length = 1;
  adc_base_config(ADC1, &adc_base_struct);

  /* adc_preempt_conversionmode--------------------------------------------- */
  adc_preempt_channel_length_set(ADC1, 4);

  adc_preempt_channel_set(ADC1, ADC_CHANNEL_1, 1, ADC_SAMPLETIME_7_5);
  adc_preempt_offset_value_set(ADC1, ADC_PREEMPT_CHANNEL_1, 0x0);
  adc_preempt_channel_set(ADC1, ADC_CHANNEL_4, 2, ADC_SAMPLETIME_7_5);
  adc_preempt_offset_value_set(ADC1, ADC_PREEMPT_CHANNEL_2, 0x0);
  adc_preempt_channel_set(ADC1, ADC_CHANNEL_5, 3, ADC_SAMPLETIME_7_5);
  adc_preempt_offset_value_set(ADC1, ADC_PREEMPT_CHANNEL_3, 0x0);
  adc_preempt_channel_set(ADC1, ADC_CHANNEL_6, 4, ADC_SAMPLETIME_7_5);
  adc_preempt_offset_value_set(ADC1, ADC_PREEMPT_CHANNEL_4, 0x0);

  /* When "ADCx_PREEMPT_TRIG_SOFTWARE" is selected, user can only use software trigger. \
  The software trigger function is adc_preempt_software_trigger_enable(ADCx, TRUE); */
  adc_preempt_conversion_trigger_set(ADC1, ADC12_PREEMPT_TRIG_TMR3CH4, TRUE);

  /* enable preempted channels conversion end interrupt */
  adc_interrupt_enable(ADC1, ADC_PCCE_INT, TRUE);

  /* add user code begin adc1_init 2 */

  /* add user code end adc1_init 2 */
  
  adc_enable(ADC1, TRUE);
  
  /* adc calibration-------------------------------------------------------- */
  adc_calibration_init(ADC1);
  while(adc_calibration_init_status_get(ADC1));
  adc_calibration_start(ADC1);
  while(adc_calibration_status_get(ADC1));

  /* add user code begin adc1_init 3 */

  /* add user code end adc1_init 3 */
}

/* add user code begin 1 */

/* add user code end 1 */
