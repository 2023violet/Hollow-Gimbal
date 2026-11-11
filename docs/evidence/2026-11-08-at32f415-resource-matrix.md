# AT32F415 Resource Matrix

- Audit date: 2026-11-08
- Scope: G1 read-only source audit
- No generated source, CMake, or board configuration changed.
- No debugger, flash, UART command, PWM enable, or powered-board action performed.

## Current startup ownership

`platform/at/at32f415/generated/src/main.c` initializes the following sequence before `app_main_bootstrap()`:

1. system clock
2. peripheral clocks
3. debug
4. NVIC
5. timebase
6. GPIO
7. ADC1
8. DMA1 channels 1-4
9. USART1
10. SPI1
11. TMR1
12. TMR5
13. TMR3
14. USART2 is not called from the current main path
15. `app_main_bootstrap()`

TMR2 is not initialized by the current generated startup chain.

## Timer ownership

| Timer | Current configuration | Current owner | V2 status |
| --- | --- | --- | --- |
| TMR1 | Two-way counter, ARR 3749, CH1-3 complementary PWM, CH4 compare at 3740, CC4 interrupt, software ADC trigger path, break disabled, deadtime 0 | Generated platform plus `motor_task.c` ISR | Keep current Legacy path. V2 fast-domain candidate |
| TMR3 | Up counter, ARR 7499, overflow primary trigger, CH4 output compare 3689 | Generated platform synchronization source | Current source. Do not repurpose before trigger audit |
| TMR5 | Up counter, ARR 1999, prescaler 14, overflow interrupt | `motor_task.c` soft IRQ and outer path | H1 time-boundary candidate. Current ISR path requires blocking audit |
| TMR2 | Register base and IRQ are present in the AT32F415 SDK headers, but no generated init exists | None | Candidate 1 MHz monotonic source. Exact counter width, clock, overflow and board conflict still require audit |
| TMR11 | Used by generated timebase support | Platform timebase | Do not assume available for V2 without audit |
| WDT/WWDT | Generated configuration leaves `WDT_MODULE_ENABLED` and `WWDT_MODULE_ENABLED` commented | No current hardware watchdog owner found in the audited generated path | G3-SAFE blocker until watchdog selection and reset behavior are explicitly audited |

## ADC and DMA ownership

ADC1 injected sequence is:

| Rank | Channel | Logical meaning in current application |
| ---: | --- | --- |
| 1 | ADC channel 1 | VBUS |
| 2 | ADC channel 4 | IC |
| 3 | ADC channel 5 | IB |
| 4 | ADC channel 6 | IA |

Current ADC injected trigger is software-triggered from the TMR1 CC4 ISR. ADC conversion-complete interrupt is enabled.

DMA1 mappings in generated code:

| DMA channel | Current generated request | Current use |
| --- | --- | --- |
| DMA1 Channel 1 | UART1 RX | USART1 transport |
| DMA1 Channel 2 | UART1 TX | USART1 transport |
| DMA1 Channel 3 | UART2 RX | Generated UART2 path, not enabled by current main path |
| DMA1 Channel 4 | UART2 TX | Generated UART2 path, not enabled by current main path |
| DMA1 Channel 5-7 | No current generated assignment in the current UART setup | Candidate space only. Exact interrupt and request configuration require SDK audit |

The current generated DMA header leaves buffer-size and memory-base macros at zero. Application code configures the active UART DMA path directly through `wk_dma_channel_config()`.

### SPI DMA feasibility

Current `ENCODER_SPI_DMA_ENABLE` is `0U`, so the active encoder path is blocking SPI polling. DMA1 Channels 3 and 4 are currently mapped to generated UART2 RX/TX requests. A V2 SPI1 DMA implementation cannot claim those channels without changing the generated request mapping and proving that UART2 is not needed on the target board.

Status: `candidate`, not allocated.

## UART and SPI ownership

### USART1

- 115200 baud
- 8 data bits
- 1 stop bit
- no parity
- no hardware flow control
- RX DMA enabled
- TX DMA enabled
- USART IDLE interrupt enabled
- current application transport owner: `bsp_console.c` and `shell_task.c`

### USART2

Generated initialization exists, but current `main.c` does not call `wk_usart2_init()`. The presence of generated USART2 code does not prove an external PCB connection.

### SPI1

- full duplex master
- 16-bit frames
- MSB first
- clock polarity high
- second clock edge
- software chip select
- clock divider 16
- current application owner: encoder path
- active current encoder path: blocking polling

## IRQ priority map

| IRQ | Priority | Current owner |
| --- | ---: | --- |
| ADC1 | 5 | `motor_task.c` ADC ISR |
| TMR1 CH | 4 | `motor_task.c` CC4 ISR |
| TMR5 global | 6 | `motor_task.c` soft IRQ |
| USART1 | 7 | `shell_task.c` |
| USART2 | 8 | Generated path, current main path does not initialize USART2 |
| SysTick | 15 | CMSIS/FreeRTOS timebase |
| PendSV | 15 | CMSIS/FreeRTOS context switching |

The current priority arrangement is a Legacy fact. V2 must define its own interrupt ownership only after the G1 timing and resource evidence is accepted.

## H0/H1 candidate execution boundaries

| Domain | Candidate owner | Candidate state |
| --- | --- | --- |
| H0 | ADC completion / fast control domain | current-loop integrators, filters, modulation, PWM output, fast safety latch |
| H1 | TMR5 sensor/outer boundary | encoder transaction state, sensor quality, velocity loop, position loop, calibration state |
| Foreground | main loop and static jobs | UART, protocol, Device Supervisor, persistence, telemetry, diagnostics |

No current Legacy object is automatically accepted as a V2 shared object. Task 3 must identify every writer and reader before Core migration.

## TMR2 monotonic-time audit

SDK evidence confirms flexible DMA request identifiers for `DMA_FLEXIBLE_SPI1_RX` and `DMA_FLEXIBLE_SPI1_TX`, and flexible channels 1-7 exist. Current generated code uses channels 1-4 for UART1 and UART2 request mappings. This makes channels 5-7 the first candidate space for SPI1, but it does not yet prove request priority, interrupt availability, or board-level timing.

Current status: `candidate`, not allocated.

Required before use:

- confirm timer width and clock source for the exact AT32F415 package
- confirm interrupt and read behavior
- confirm no conflict with generated timebase or board function
- confirm 1 MHz configuration and overflow handling
- add a separate ADR amendment if TMR2 is unavailable

## SPI DMA feasibility audit

Current status: `candidate`, not allocated.

Blocking facts:

- current encoder polling path waits on SPI status flags
- current DMA1 Channels 3 and 4 are mapped to generated UART2 requests
- `ENCODER_SPI_DMA_ENABLE` is disabled
- no V2 DMA request reassignment is authorized by G1

## Resources that require an ADR before allocation

- TMR2 or any replacement monotonic timer
- SPI1 RX/TX DMA channels
- any change to TMR5 ownership
- any change to ADC/TMR1 trigger ownership
- any change to USART1 DMA channels
- any change to IRQ priorities used by the Legacy path

## G1 decision

The current source audit is complete for the visible startup, timer, ADC, DMA, UART, SPI, and IRQ ownership. TMR2 and SPI DMA remain candidates. No resource allocation change is authorized until the SDK-level timer/DMA facts and the H0/H1 timing measurements are recorded.
