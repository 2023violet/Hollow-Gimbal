# Timing and Blocking Baseline

- Audit date: 2026-11-08
- Scope: G1 read-only source and historical-evidence audit
- No control code changed.
- No powered timing measurement performed.

## Fast path call graph

Current Legacy path:

```text
TMR1 CC4 interrupt
    -> adc_preempt_software_trigger_enable(ADC1, TRUE)
    -> ADC injected conversion
    -> ADC1_IRQHandler
        -> adc_read_phases()
        -> fast hardware snapshot
        -> foc_fault_check_fast()
        -> bench watchdog/energy checks
        -> divider gate
        -> foc_current_loop_step()
```

The current source uses `_adc_sw_trig_armed` to allow one software-triggered injected sequence per PWM period. The armed flag is cleared by TMR1 CC4 and re-armed when ADC conversion-complete service begins.

The consequence is architectural and already recorded in the maintenance log: if ADC completion service is delayed beyond a later CC4 event, the effective sample cadence can change even while the nominal PWM and compile-time loop frequencies remain unchanged.

## H1 candidate call graph

Current Legacy path:

```text
TMR5 overflow interrupt
    -> encoder_update(&_motor_ins.enc_pri)
    -> foc_outer_loop_step(&_motor_ins)
```

The current `encoder_update()` path uses blocking SPI polling when `ENCODER_SPI_DMA_ENABLE == 0U`:

- wait for SPI transmit buffer empty
- write SPI data
- wait for SPI receive buffer full
- read SPI data

These loops have no timeout. This is acceptable as a recorded Legacy behavior because the current firmware is known to run, but it is not accepted for the V2 H1 ISR path.

V2 H1 requires a bounded, non-blocking encoder transaction. The preferred design is a DMA transaction state machine with a complete-sample publication step.

## H0/H1 state ownership

| Object or state | Current Legacy location | V2 owner | Current audit status |
| --- | --- | --- | --- |
| Current PI integrators | `motor_dev.ctrl_config` / current control path | H0 | Must be separated from H1 |
| Current filters | `motor_dev.ctrl_state` | H0 | Must be separated from H1 |
| Modulation and duty state | `motor_dev.foc_calc` | H0 | Must be separated from H1 |
| Encoder raw and corrected state | `motor_dev.enc_pri` | H1 sensor state | Current object is shared through `motor_dev` |
| Velocity estimator state | encoder PLL inside encoder object | H1 | Current update runs from TMR5 path |
| Velocity loop state | motor control config/runtime | H1 | Must publish target snapshot to H0 |
| Position loop state | motor control config/runtime | H1 | Must publish target snapshot to H0 |
| Calibration state | `motor_dev.calib_state` | H1/Device calibration service | Must not be written by H0 |
| Command target | runtime command fields | Device -> validated target snapshot | Current fields are embedded in `motor_dev` |
| Fast fault latch | motor fault runtime | H0/safety domain | Final output inhibit must be H0-owned |
| Telemetry snapshot | runtime snapshot structures | H0 publishes, foreground reads consistently | Needs sequence-checked copy |

Migration blocker: the current `motor_dev` aggregate is not a valid V2 shared state boundary. G2/G3 must create explicit H0/H1 snapshots before moving control state.

## Configured timing

| Function | Current configured source | Current nominal value | Status |
| --- | --- | ---: | --- |
| TMR1 carrier | `wk_tmr.c`, ARR 3749, two-way count | approximately 20 kHz | Legacy configured fact |
| Bench FOC model | `foc_config.h` | 5 kHz | Compile-time model |
| Product FOC model | `foc_config.h` | 10 kHz | Compile-time model |
| Bench ADC divider | `motor_task_config.h` | 4 | Compile-time divider |
| Product ADC divider | `motor_task_config.h` | 2 | Compile-time divider |
| TMR5 | `wk_tmr.c`, ARR 1999, prescaler 14 | nominal 5 kHz target | Static configuration only |
| Velocity loop | `foc_config.h` | 5 kHz legacy value | Not accepted as actual timing |
| Position loop | `foc_config.h` | 1 kHz legacy value | Not accepted as actual timing |

## Historical measured timing

The latest maintenance log records the following historical board measurements:

- injected ADC completion near 13.35 kHz
- bench FOC step near 3.34 kHz
- compile-time bench control period based on 5 kHz

These are historical evidence tied to a particular image, board, power state, and experiment. They are not remeasured by G1. They are sufficient to require a new timing contract and instrumentation before V2 control migration.

## Blocking operations

Confirmed or suspected blocking paths requiring V2 treatment:

- encoder SPI transmit status polling
- encoder SPI receive status polling
- any future DMA completion wait
- UART send paths that use polling fallback
- Flash erase/program operations
- protocol loops that consume unbounded input

V2 rules:

- fast and H1 ISR paths cannot wait for SPI, DMA, UART, Flash, or foreground state
- every front-end service has a work bound
- Flash only runs in a safe foreground transaction
- protocol parsing is bounded by bytes and frames per iteration

## Timing facts that require future instrumentation

- ADC trigger timestamp to injected conversion completion
- injected conversion completion to `foc_fast_tick()` entry
- `foc_fast_tick()` WCET
- PWM register write to effective timer update
- sample-to-PWM latency
- TMR5 ISR WCET
- encoder DMA transaction latency
- velocity and position loop deadline miss
- foreground scheduler budget and starvation
- UART RX overflow and protocol dispatch budget
- fault response latency

## G1 decision

The current source audit confirms the need for H0/H1 state separation, bounded non-blocking encoder acquisition, and explicit timing instrumentation. G1 does not modify these paths. G2/G3 implementation is blocked until the ownership and timing contracts are represented in new Core and Runtime interfaces.
