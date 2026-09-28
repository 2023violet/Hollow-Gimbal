/* add user code begin Header */
/**
  **************************************************************************
  * @file     wk_tmr.c
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
#include "wk_tmr.h"

/* add user code begin 0 */

/* add user code end 0 */

/**
  * @brief  init tmr1 function.
  * @param  none
  * @retval none
  */
void wk_tmr1_init(void)
{
  /* add user code begin tmr1_init 0 */

  /* add user code end tmr1_init 0 */

  gpio_init_type gpio_init_struct;
  tmr_output_config_type tmr_output_struct;
  tmr_brkdt_config_type tmr_brkdt_struct;

  gpio_default_para_init(&gpio_init_struct);

  /* add user code begin tmr1_init 1 */

  /* add user code end tmr1_init 1 */

  /* configure the CH1C pin */
  gpio_init_struct.gpio_pins = INLC_PIN;
  gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
  gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(INLC_GPIO_PORT, &gpio_init_struct);

  /* configure the CH2C pin */
  gpio_init_struct.gpio_pins = INLB_PIN;
  gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
  gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(INLB_GPIO_PORT, &gpio_init_struct);

  /* configure the CH3C pin */
  gpio_init_struct.gpio_pins = INLA_PIN;
  gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
  gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(INLA_GPIO_PORT, &gpio_init_struct);

  /* configure the CH1 pin */
  gpio_init_struct.gpio_pins = INHC_PIN;
  gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
  gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(INHC_GPIO_PORT, &gpio_init_struct);

  /* configure the CH2 pin */
  gpio_init_struct.gpio_pins = INHB_PIN;
  gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
  gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(INHB_GPIO_PORT, &gpio_init_struct);

  /* configure the CH3 pin */
  gpio_init_struct.gpio_pins = INHA_PIN;
  gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
  gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(INHA_GPIO_PORT, &gpio_init_struct);

  /* GPIO PIN remap */
  gpio_pin_remap_config(TMR1_GMUX_0001, TRUE); 

  /* configure counter settings */
  tmr_cnt_dir_set(TMR1, TMR_COUNT_TWO_WAY_1);
  tmr_clock_source_div_set(TMR1, TMR_CLOCK_DIV1);
  tmr_repetition_counter_set(TMR1, 0);
  tmr_period_buffer_enable(TMR1, FALSE);
  tmr_base_init(TMR1, 3749, 0);

  /* configure primary mode settings */
  tmr_sub_sync_mode_set(TMR1, TRUE);
  tmr_primary_mode_select(TMR1, TMR_PRIMARY_SEL_RESET);

  /* configure sub-mode */
  tmr_sub_mode_select(TMR1, TMR_SUB_RESET_MODE);

  /* configure sub-mode input */
  tmr_trigger_input_select(TMR1, TMR_SUB_INPUT_SEL_IS2);

  /* configure channel 1 output settings */
  tmr_output_struct.oc_mode = TMR_OUTPUT_CONTROL_PWM_MODE_A;
  tmr_output_struct.oc_output_state = TRUE;
  tmr_output_struct.occ_output_state = TRUE;
  tmr_output_struct.oc_polarity = TMR_OUTPUT_ACTIVE_HIGH;
  tmr_output_struct.occ_polarity = TMR_OUTPUT_ACTIVE_HIGH;
  tmr_output_struct.oc_idle_state = FALSE;
  tmr_output_struct.occ_idle_state = FALSE;
  tmr_output_channel_config(TMR1, TMR_SELECT_CHANNEL_1, &tmr_output_struct);
  tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_1, 0);
  tmr_output_channel_buffer_enable(TMR1, TMR_SELECT_CHANNEL_1, TRUE);

  tmr_output_channel_immediately_set(TMR1, TMR_SELECT_CHANNEL_1, FALSE);

  /* configure channel 2 output settings */
  tmr_output_struct.oc_mode = TMR_OUTPUT_CONTROL_PWM_MODE_A;
  tmr_output_struct.oc_output_state = TRUE;
  tmr_output_struct.occ_output_state = TRUE;
  tmr_output_struct.oc_polarity = TMR_OUTPUT_ACTIVE_HIGH;
  tmr_output_struct.occ_polarity = TMR_OUTPUT_ACTIVE_HIGH;
  tmr_output_struct.oc_idle_state = FALSE;
  tmr_output_struct.occ_idle_state = FALSE;
  tmr_output_channel_config(TMR1, TMR_SELECT_CHANNEL_2, &tmr_output_struct);
  tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_2, 0);
  tmr_output_channel_buffer_enable(TMR1, TMR_SELECT_CHANNEL_2, TRUE);

  tmr_output_channel_immediately_set(TMR1, TMR_SELECT_CHANNEL_2, FALSE);

  /* configure channel 3 output settings */
  tmr_output_struct.oc_mode = TMR_OUTPUT_CONTROL_PWM_MODE_A;
  tmr_output_struct.oc_output_state = TRUE;
  tmr_output_struct.occ_output_state = TRUE;
  tmr_output_struct.oc_polarity = TMR_OUTPUT_ACTIVE_HIGH;
  tmr_output_struct.occ_polarity = TMR_OUTPUT_ACTIVE_HIGH;
  tmr_output_struct.oc_idle_state = FALSE;
  tmr_output_struct.occ_idle_state = FALSE;
  tmr_output_channel_config(TMR1, TMR_SELECT_CHANNEL_3, &tmr_output_struct);
  tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_3, 0);
  tmr_output_channel_buffer_enable(TMR1, TMR_SELECT_CHANNEL_3, TRUE);

  tmr_output_channel_immediately_set(TMR1, TMR_SELECT_CHANNEL_3, FALSE);

  /* configure channel 4 output settings (ADC injected-group trigger).
   * The DRV8311H current sense uses low-side sense FETs, so sampling must be
   * PWM-synchronized.  The trigger fires on the CH4-output rising edge, i.e.
   * the DOWN-crossing of CCR4, so CCR4 places the sample point relative to
   * the counter peak: the all-low-side conduction window of the worst phase
   * is [peak-(1-d)*3749, peak+(1-d)*3749] and CENTERING the 2.13 us
   * four-rank sequence on the peak (a) keeps every rank inside the window up
   * to duty ~0.83 (the empirical CSA settling of ~710 counts then sets the
   * ceiling), whereas the old CCR4=2700 sampled peak+7.0 us, which fell out
   * of the shrinking window already at duty ~0.53 and blinded rank2/ia first
   * (the 2026-08-30 "duty>0.53 rank2 aperture" mystery).
   * CCR4=3740 = 9 counts (60 ns) before the turnaround: effectively AT the
   * peak while avoiding the peak-turnaround compare edge case. */
  tmr_output_struct.oc_mode = TMR_OUTPUT_CONTROL_PWM_MODE_A;
  tmr_output_struct.oc_output_state = TRUE;
  tmr_output_struct.occ_output_state = FALSE;
  tmr_output_struct.oc_polarity = TMR_OUTPUT_ACTIVE_HIGH;
  tmr_output_struct.occ_polarity = TMR_OUTPUT_ACTIVE_HIGH;
  tmr_output_struct.oc_idle_state = FALSE;
  tmr_output_struct.occ_idle_state = FALSE;
  tmr_output_channel_config(TMR1, TMR_SELECT_CHANNEL_4, &tmr_output_struct);
  tmr_channel_value_set(TMR1, TMR_SELECT_CHANNEL_4, 3740);
  tmr_output_channel_buffer_enable(TMR1, TMR_SELECT_CHANNEL_4, FALSE);

  tmr_output_channel_immediately_set(TMR1, TMR_SELECT_CHANNEL_4, FALSE);

  /* CCR4 compare interrupt: at this counter position the CC4 ISR issues the
   * ADC injected-group SOFTWARE trigger (the hardware TMR1CH4 trigger path
   * proved unreliable on this board — the sampled current gain varied with
   * the electrical angle, see the 2026-09-26 gain(theta) map in the
   * maintenance log). Keeping the compare-based timing preserves the
   * bench-validated sample point near the counter peak while removing the
   * mystery trigger path. */
  tmr_interrupt_enable(TMR1, TMR_C4_INT, TRUE);

  /* configure break and dead-time settings */
  tmr_brkdt_struct.brk_enable = FALSE;
  tmr_brkdt_struct.auto_output_enable = FALSE;
  tmr_brkdt_struct.brk_polarity = TMR_BRK_INPUT_ACTIVE_LOW;
  tmr_brkdt_struct.fcsoen_state = FALSE;
  tmr_brkdt_struct.fcsodis_state = FALSE;
  tmr_brkdt_struct.wp_level = TMR_WP_OFF;
  /* Dead-time A/B (2026-09-26 bench): 1.0 us (DTG=0x8B) badly distorts the
   * low-duty phases of 30 rpm open-loop drag (duty 0.19-0.22 loses ~20% of
   * its ON window) and the axis stalls where deadtime=0 pulled through.
   * Reverted to 0: the DRV8311H 6x-PWM input interlock (~450-760 ns handshake)
   * remains the only dead-time, matching the config that achieved the
   * historical 9-turn bidirectional I-f drag. */
  tmr_brkdt_struct.deadtime = 0;
  tmr_brkdt_config(TMR1, &tmr_brkdt_struct);


  tmr_output_enable(TMR1, TRUE);

  tmr_counter_enable(TMR1, TRUE);

  /* add user code begin tmr1_init 2 */

  /* add user code end tmr1_init 2 */
}

/**
  * @brief  init tmr3 function.
  * @param  none
  * @retval none
  */
void wk_tmr3_init(void)
{
  /* add user code begin tmr3_init 0 */

  /* add user code end tmr3_init 0 */

  tmr_output_config_type tmr_output_struct;

  /* add user code begin tmr3_init 1 */

  /* add user code end tmr3_init 1 */

  /* configure counter settings */
  tmr_cnt_dir_set(TMR3, TMR_COUNT_UP);
  tmr_clock_source_div_set(TMR3, TMR_CLOCK_DIV1);
  tmr_period_buffer_enable(TMR3, FALSE);
  tmr_base_init(TMR3, 7499, 0);

  /* configure primary mode settings */
  tmr_sub_sync_mode_set(TMR3, TRUE);
  tmr_primary_mode_select(TMR3, TMR_PRIMARY_SEL_OVERFLOW);

  /* configure channel 4 output settings */
  tmr_output_struct.oc_mode = TMR_OUTPUT_CONTROL_PWM_MODE_B;
  tmr_output_struct.oc_output_state = TRUE;
  tmr_output_struct.occ_output_state = FALSE;
  tmr_output_struct.oc_polarity = TMR_OUTPUT_ACTIVE_HIGH;
  tmr_output_struct.occ_polarity = TMR_OUTPUT_ACTIVE_HIGH;
  tmr_output_struct.oc_idle_state = FALSE;
  tmr_output_struct.occ_idle_state = FALSE;
  tmr_output_channel_config(TMR3, TMR_SELECT_CHANNEL_4, &tmr_output_struct);
  tmr_channel_value_set(TMR3, TMR_SELECT_CHANNEL_4, 3689);
  tmr_output_channel_buffer_enable(TMR3, TMR_SELECT_CHANNEL_4, FALSE);

  tmr_output_channel_immediately_set(TMR3, TMR_SELECT_CHANNEL_4, FALSE);


  tmr_counter_enable(TMR3, TRUE);

  /* add user code begin tmr3_init 2 */

  /* add user code end tmr3_init 2 */
}

/**
  * @brief  init tmr5 function.
  * @param  none
  * @retval none
  */
void wk_tmr5_init(void)
{
  /* add user code begin tmr5_init 0 */

  /* add user code end tmr5_init 0 */


  /* add user code begin tmr5_init 1 */

  /* add user code end tmr5_init 1 */

  /* configure counter settings */
  tmr_cnt_dir_set(TMR5, TMR_COUNT_UP);
  tmr_clock_source_div_set(TMR5, TMR_CLOCK_DIV1);
  tmr_period_buffer_enable(TMR5, FALSE);
  tmr_base_init(TMR5, 1999, 14);

  /* configure primary mode settings */
  tmr_sub_sync_mode_set(TMR5, FALSE);
  tmr_primary_mode_select(TMR5, TMR_PRIMARY_SEL_RESET);

  tmr_counter_enable(TMR5, TRUE);

  /* enable ovfien interrupt */
  tmr_interrupt_enable(TMR5, TMR_OVF_INT, TRUE);

  /* add user code begin tmr5_init 2 */

  /* add user code end tmr5_init 2 */
}

/* add user code begin 1 */

/* add user code end 1 */
