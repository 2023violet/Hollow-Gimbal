# FalconFOC V2 AT32F415 reference firmware

This standalone C11 target binds the existing Core, Device, Safety, Pipeline and
Runtime to Board1. The image uses a software-locked output adapter. A valid
UPDATE is recorded and rejected before PI state can commit. An INHIBIT
acknowledges the software lock only. ZERO_DUTY and ACTIVE_BRAKE are distinct
rejected actions. Foreground services the [V1 UART protocol](../../protocol/README.md)
after Runtime supervision. ARM/START report OUTPUT_LOCKED. A restricted
command-timeout clear is executed and rechecked by H0.
The optional d/q PMSM feedforward policy remains disabled in the Board1
reference configuration; enabling it requires a separately validated electrical
speed snapshot and motor parameter set.

## Build

Run from the repository root with CMake, Ninja and Arm GNU Toolchain on PATH.

```powershell
cmake -S firmware/at32f415 -B build/sw03-at32 -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE="$($PWD.Path)/tools/cmake/arm-gcc-toolchain.cmake" `
  -DMCU_CPU=cortex-m4 -DMCU_FLOAT=soft -DMCU_FPU=none
cmake --build build/sw03-at32

cmake -S firmware/at32f415 -B build/sw03-board-host -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build/sw03-board-host
ctest --test-dir build/sw03-board-host --output-on-failure
```

The target emits `falconfoc-v2-at32f415.{elf,bin,hex,map}` in the cross-build
directory. The Host test passes fake peripheral observations to the same Board
Port functions called by the MCU handlers.

Run `ctest --test-dir build/sw03-board-host -R '^test-foc-timing$' -V` for the
ARCH-00 event counts and deliberate deadline, ADC, SPI and Runtime fault cases.
Board cases call the production Board Port and Runtime. Carrier, load markers
and costs in the frequency comparison use a synthetic clock.
[Execution ADR](../../docs/architecture/FalconFOC_BareMetal_Execution_ADR.md)

## Timing ownership

AT32F415 timer-clock facts are owned by
`platform/at/at32f415/generated/inc/tm_platform_foc_timing.h` and are consumed
by both the Legacy application configuration and this V2 Board port. The
Legacy `PWM_FREQ_HZ`/current-step, velocity, position, and trajectory values
remain product control policy; `PWM_FREQ_HZ` is not the generated TMR1 carrier
frequency. This split prevents a future platform clock change from silently
changing the Legacy control cadence or the V2 timestamp scale.

## Explicit calibrated initialization

`foc_board_init()` keeps the nominal Board1 values (2048-count ADC offsets,
seven pole pairs, zero electrical offset, and reversed raw encoder direction).
An accepted `foc_calibration_snapshot` may be supplied to
`foc_board_init_with_calibration()` before any producer starts. That entry point
validates the snapshot against the Board1 ADC gain, full scale, phase order, and
encoder facts, assembles local pipeline/runtime/Sensor copies, and commits them
only after the complete initialization succeeds. It rejects a second attempt
to modify an initialized encoder, does not write Flash, and does not unlock
PWM. The calibration source direction and CAL-01 rotor direction are multiplied
once to resolve the Sensor raw-code direction; the Core continues to receive a
positive-pole-pair mechanical coordinate.

## Startup calibration selection

`main()` calls `foc_board_boot_select()` after `foc_at32_lock_output()` and the
clock, and before ADC, SPI, timers, UART and IRQs start. The selection reads a
build-time, read-only supply through `foc_board_calibration_supply()` and the
independent `foc_board_calibration_expectation()` (accepted supply identity and
expected motor pole pairs, set by CMake, never by the supply).

- Default build (`FALCONFOC_BOARD_CALIBRATION_SUPPLY=NONE`): no supply. The
  report is `NOMINAL_CONFIGURATION` + `CALIBRATION_NOT_AVAILABLE`, with the same
  Runtime as `foc_board_init()`.
- A supply carries the existing admission candidates (ADC zero, rotor, offset),
  their limits and the recorded snapshot. Startup checks format, supply identity,
  the presence of a nonzero review reference and the encoder coordinate (only
  `RAW_NO_LUT` is accepted), recomputes
  `foc_calibration_admit()` with Board facts and the expected pole pairs,
  requires the recomputed snapshot to equal the supplied one bit for bit, checks
  runtime direction = source direction × calibration direction and the Board
  window (offsets 1536–2560 counts, 1–64 pole pairs), then calls
  `foc_board_init_with_calibration()`. Success gives `CALIBRATED_CONFIGURATION`.
- Any rejection or initialization failure scrubs the Runtime, output journal
  and encoder and initializes the nominal configuration from scratch:
  `NOMINAL_CONFIGURATION` + `CALIBRATION_REJECTED` and the reason. If nominal
  initialization fails too, nothing is reported ready and `main()` stays in the
  locked `__WFI()` loop with no producer.

The supply identity is matched against the independent CMake expectation. The
review ID is a reference to review material only; it is required to be nonzero
but is not matched, signed or treated as proof of physical provenance. CMake
rejects a verification supply ID outside 0x1–0xFFFFFFFF (aliases such as
0x156464331 included) and pole pairs outside 1–64 before compilation, and
`foc_board_expectation.c` re-checks the un-narrowed literal with static
assertions.

Only the three ADC offsets, encoder direction, pole pairs and rotor electrical
offset are consumed. R/L, flux, Kt, J/B, PI and LUT stay outside Board1 startup
and FCAL is not parsed. UART reads the result with PARAM_READ IDs 32–45 (see
the protocol README). The optional capture recorder takes its CPR, direction,
pole pairs and phase order from the effective configuration and marks the
configuration in the V1 header word `encoder_profile` (1 nominal, 2 calibrated);
see [BOARD_CAPTURE.md](BOARD_CAPTURE.md#configuration-identity).

The synthetic verification image is a separate, explicitly named build:

```powershell
cmake -S firmware/at32f415 -B build/fw-integ-verify -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE="$($PWD.Path)/tools/cmake/arm-gcc-toolchain.cmake" `
  -DMCU_CPU=cortex-m4 -DMCU_FLOAT=soft -DMCU_FPU=none `
  -DFALCONFOC_BOARD_CALIBRATION_SUPPLY=VERIFICATION
cmake --build build/fw-integ-verify
```

It emits `falconfoc-v2-at32f415-calibration-verification.{elf,bin,hex,map}`
and links `src/foc_board_supply_verification.c`, which is synthetic test data,
not a board or motor measurement. Both images keep the same output lock.
`test-foc-board-boot` and `test-foc-board-boot-default` link the same supply
units on the Host.

## Periodic phase source

Encoder completion publishes the same Feedback's directed integer
`mechanical_phase_count`, Sensor-instance CPR and source capture time through
the Core phase helper, using this Runtime's immutable rotor pole pairs/offset.
Motion-enabled H1 requires `electrical_phase_valid` as well as valid continuous
position/speed. The first position-only Encoder frame remains invalid for Board
publication. Rejected frames never borrow an old phase. Continuous position
still goes to Position/Trajectory/telemetry without wrapping. No timer, ADC,
SPI, Safety, UART V1 or locked output admission changes accompany this mapping.

## Board1 mapping

| Resource | V2 binding |
| --- | --- |
| U3 | AT32F415KBU7-4, QFN32, 128 KiB Flash and 32 KiB SRAM. Uses the vendor xB script and startup vector at `0x08000000`. |
| Clocks | HICK 8 MHz → PLL 150 MHz, AHB 150 MHz, APB1/2 75 MHz. Prescaled APB timers receive 150 MHz. |
| TMR2 | 32-bit plus mode, divider 150, 1 µs per count, free-running with no IRQ. All domains share this epoch. |
| TMR1 / ADC1 | TMR1 CH4 internal compare at nominal +40 µs in a 200 µs slot. Its IRQ launches one software-triggered injected conversion and timestamps it. ADC1 group completion is H0. No PWM GPIO mux or active channel is configured. |
| ADC injected group | Rank 1 PA1 / ADC1_IN1 = VBUS. Rank 2 PA4 / IN4 = driver IC → logical A. Rank 3 PA5 / IN5 = driver IB → logical B. Rank 4 PA6 / IN6 = driver IA → logical C. ADCCLK 12.5 MHz. |
| Nominal scaling | 12-bit raw. VBUS = count × 3.3 × 11 / 4095 V. I = (count − 2048) × 3.3 / (4095 × 0.5) A. Gains, offsets and valid CSA conduction windows need measurement. |
| Logical PWM requests | A → CH3 PA10/PB1 (driver C), B → CH2 PA9/PB0 (driver B), C → CH1 PA8/PA7 (driver A). Compare is truncated duty × 29999, inclusive, for diagnostics only. |
| TMR5 / SPI1 | TMR5 overflow begins a nominal 500 µs slot and asserts PA15 CS. CH1 compare at +20 µs starts one SPI transfer and CH2 at +70 µs closes the bounded window. SPI1 RX/error IRQ captures the result. PB3 SCK, PB4 MISO, PB5 MOSI. |
| Encoder | KTH7823-X-N-QN16, active-low CS, SPI mode 3, MSB first, APB2/16, one zero TX word and one 16-bit angle word. No wire CRC. Source time is the CS assertion proxy. `sensors/foc_encoder` owns direction, bounded uint32 timestamp arithmetic, integer-backed unwrapping, raw difference speed, variable-`dt` LPF speed, alpha-beta comparison speed, and quality classification. Board1 publishes the LPF speed into the existing `foc_runtime_sensor`; the raw and tracked estimates remain local diagnostics. The absolute sensor zero and seven pole pairs are nominal. Electrical offset zero is uncalibrated. |
| IRQ priority | NVIC grouping 0, four preemption bits. ADC1 #18 and TMR1_CH #27 use priority 0. TMR5_GLOBAL #50 and SPI1 #35 use priority 1. Same-priority pairs serialize their own acquisition state. H0 may preempt H1. |
| USART1 | PB6 TX, PB7 RX, 115200 8N1, byte IRQ priority 2. Bounded RX ring and single complete TX buffer. |
| Other resources | DMA1/2, SysTick and both watchdogs are unused. No Flash persistence backend. |

ADC consumes one outstanding trigger only. An overlapping trigger invalidates
that token and requests stop. A late group must be drained before accepting
another. The Runtime computes control intervals from consecutive accepted source
timestamps. H1 drops failed observations and requests inhibition even if an
invalid snapshot cannot be published. After an encoder gap beyond 2.5 ms, the
continuous position remains invalid for the session. At startup two timely
encoder observations are needed to estimate velocity. The software latch
threshold allows the initial ADC cycles with no velocity estimate.

RT-01 assigns each ADC token a 200-µs logical TMR1 slot from phase and TMR2
time witnesses. The port rejects ambiguous time and cross-slot completions.
An optional single foreground reader can request a coherent Board snapshot
with `foc_board_adc_read`. A BUSY result leaves its destination unchanged
while the IRQ owner services the request. This API adds no UART V1 operation.
The diagnostic load timestamp identifies the logical next slot boundary.
[RT-01 timing and evidence](../../docs/evidence/2026-11-19-next-rt-01.md)

RT-02 adds opt-in local timestamps with `-DFALCONFOC_RT02_TIMING_TRACE=ON`.
The single ADC transaction carries the expected CH4 slot identity, IRQ entry
counter samples, pre-PCSWTRG proxy, PCCE IRQ entry, H0 elapsed cycles, and
adapter attempt/return, including a declined UPDATE before INHIBIT.
Read the same coherent Board snapshot with `foc_board_adc_read`. On ARM the
probe uses DWT CYCCNT when enabled and supported. Markers describe software
execution. ADC synchronization, signal aperture and physical PWM load
require independent event witnesses.

`foc_board_timing` encodes offline ADC trigger candidates and checks nominal
rank apertures and future carrier-specific shadow deadlines. See
[RT-02 timing and evidence](../../docs/evidence/2026-11-19-next-rt-02.md)
for timing assumptions, the Host A/B model, and OFF/ON build results.

RT-03 extends the same option with per-domain DWT observations of the ADC,
encoder/SPI, H0/H1 and foreground paths. The observations are latest records
owned by their interrupt or by foreground. A debugger read needs a stable
record, and elapsed cycles can include preemption.

The SPI phase check uses a TMR2 time bracket around TMR5 service and rejects a
transaction when the CS/TX or close action proxy exceeds its window. A
contended H1 snapshot permits one fresh copy attempt. Supervision defers a
contended read without claiming a successful heartbeat. The ARM cost inputs
remain zero-valued Host budget placeholders. See
[RT-03 evidence](../../docs/evidence/2026-11-19-next-rt-03.md).

## Locked Board1 capture

Optional `FALCONFOC_BOARD_CAPTURE=ON` records ADC ranks, SPI words and
Runtime/Safety observations in a one-shot SRAM image. Read the frozen image
over the controlled SWD workflow and check it offline with the Host tools.
See [Board1 capture and SOFT-06 import](BOARD_CAPTURE.md).
See [locked timing observations](LOCKED_TIMING.md) for the DWT and SRAM comparison.

## Evidence limits

The lock is applied in the standalone entry before peripheral producer setup.
The six bridge input pins stay unmuxed and TMR1 only counts and compares
internally. Physical output and reset behavior, oscillator accuracy, SPI/ADC
timing, CSA validity, IRQ latency, execution budgets and stack high-water
measurements remain open. The Board1 Break, `nFAULT`, `nSLEEP`, R7 and independent
watchdog questions remain governed by the separate hardware safety gate.

References for the peripheral and encoder contracts

- [AT32F415 reference manual](https://www.arterytek.com/download/RM/RM_AT32F415_V2.05_EN.pdf)
- [AT32F415 datasheet](https://arterytek.com/download/DS/DS_AT32F415_V2.02_EN.pdf)
- [KTH7823 manufacturer selection table](https://www.conntek.com.cn/html/web/product/magneticCurrentLinearSensingChip/hshrMagneticCodedAngleSensor/index.html)
