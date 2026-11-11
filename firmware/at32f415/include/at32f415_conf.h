/**
 * @file    at32f415_conf.h
 * @brief   Vendor SDK configuration for the standalone V2 image.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Design overview
 * ---------------
 * Expose only the SDK modules selected by the standalone board target.
 *
 * Usage example
 * -------------
 * The SDK includes this file through at32f415.h during the V2 build.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * This is a build-time configuration with no runtime state.
 *
 * Failure / recovery
 * ------------------
 * The standalone build supplies these definitions; there is no fallback configuration.
 */
#pragma once

#define HEXT_VALUE           8000000U
#define HEXT_STARTUP_TIMEOUT 0x3000U
#define HICK_VALUE           8000000U
#define ADC_MODULE_ENABLED
#define CRM_MODULE_ENABLED
#define FLASH_MODULE_ENABLED
#define GPIO_MODULE_ENABLED
#define SPI_MODULE_ENABLED
#define TMR_MODULE_ENABLED
#define USART_MODULE_ENABLED

#include "at32f415_adc.h"
#include "at32f415_crm.h"
#include "at32f415_flash.h"
#include "at32f415_gpio.h"
#include "at32f415_spi.h"
#include "at32f415_tmr.h"
#include "at32f415_usart.h"

/* Public Types ---------------------------------------------------------------- */

/* Public Functions ------------------------------------------------------------- */

/* ---------------------------------------- end of file ----------------------------------------- */
