# UART and DMA Audit

- Audit date: 2026-11-08
- Scope: G1 read-only source audit
- Current Board1 external control path: USART1
- Current generated configuration: 115200 8N1, RX/TX DMA, USART IDLE interrupt

## Current USART1 configuration

Source facts:

- USART1 is initialized at 115200 baud.
- Data format is 8 data bits, one stop bit, no parity.
- Hardware flow control is disabled.
- USART1 RX DMA is enabled.
- USART1 TX DMA is enabled.
- USART1 IDLE interrupt is enabled.
- DMA1 Channel 1 is mapped to UART1 RX.
- DMA1 Channel 2 is mapped to UART1 TX.

USART2 also has generated configuration, but the current `main.c` path does not call `wk_usart2_init()`. Generated code alone does not prove an external Board1 connection.

## RX path

Current ownership:

```text
USART1 IDLE IRQ
    -> shell_task_rx_notify()
    -> shell task wake
    -> bsp_console_shell_rx_position()
    -> _process_dma_rx()
    -> line buffer
    -> tm_shell_task_submit_line()
    -> app_shell_task queue
    -> shell_process_line()
```

Current implementation is optimized for line-oriented Shell input. It uses a DMA ring position and an IDLE notification to move received bytes into the line parser.

## TX path

Current ownership:

```text
shell_printf / async print
    -> bounded TX frame or ring buffer
    -> shell task drain
    -> bsp_console_write_poll() or bsp_console_write_dma()
    -> USART1 TX
```

The current transport already has useful bounded buffering and a separate transport/session split. This should be reused as a transport foundation rather than replaced with a new UART driver.

## Continuous-stream risks

The current path requires explicit V2 tests for:

- continuous binary data with no IDLE gap
- DMA counter wraparound
- producer overtaking consumer
- partial COBS frame
- corrupted frame followed by a valid frame
- RX overflow
- multiple complete frames in one DMA drain
- one frame split across multiple drains
- TX queue saturation
- telemetry traffic competing with control traffic

IDLE alone cannot be the only forward-progress mechanism for a continuous binary stream. V2 must add DMA half-transfer, transfer-complete, or bounded periodic polling support, subject to the resource audit.

## Traffic-class policy

| Traffic | Queue policy | Loss policy |
| --- | --- | --- |
| `DISABLE`, `STOP`, fault event | independent safety path | must not be blocked by telemetry |
| latest setpoint | single latest-wins mailbox | stale targets are discarded |
| parameter/calibration request | bounded FIFO | return `BUSY` when full |
| status telemetry | bounded best-effort queue | old samples may be dropped |
| debug log | optional low-priority queue | may be dropped or disabled |

Protocol parsing never runs in a control ISR. UART only transports bytes. Device validates the message and publishes a safe command object.

## Required Protocol v1 behavior

UART Protocol v1 must define:

- bounded maximum frame length
- COBS or equivalent resynchronizable framing
- CRC16 for accidental corruption
- version and message type
- sequence number
- payload length
- command freshness
- duplicate request handling
- ACK/NACK behavior
- RX overflow behavior
- telemetry backpressure
- command timeout
- distinction between STOP, DISABLE, BRAKE, and FAULT_CLEAR

CRC detects accidental corruption. It is not an authentication mechanism.

## G1 decision

The existing USART1 DMA transport is a usable migration foundation. The V2 work must separate its byte transport from Shell line parsing, add continuous-stream progress and overflow detection, and preserve an independent safety command path. No UART implementation is changed during G1.
