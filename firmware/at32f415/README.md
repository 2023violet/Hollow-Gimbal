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
| Clocks | 8 MHz crystal (HEXT, 20 pF load) → PLL × 75 / 4 = 150 MHz, AHB 150 MHz, APB1/2 75 MHz. Prescaled APB timers receive 150 MHz. If the crystal does not start within the startup poll, HICK / 2 × 150 / 4 gives the same 150 MHz so the board still boots; `clock_from_crystal` records the source for a debugger. |
| TMR2 | 32-bit plus mode, divider 150, 1 µs per count, free-running with no IRQ. All domains share this epoch. |
| TMR1 / ADC1 | TMR1 CH4 internal compare at nominal +40 µs in a 200 µs slot. Its IRQ launches one software-triggered injected conversion and timestamps it. ADC1 group completion is H0. No PWM GPIO mux or active channel is configured. |
| ADC injected group | Rank 1 PA0 / ADC1_IN0 = VBUS. Rank 2 PA4 / IN4 = driver IC → logical A. Rank 3 PA5 / IN5 = driver IB → logical B. Rank 4 PA6 / IN6 = driver IA → logical C. ADCCLK 12.5 MHz. VDDA is the DRV8311 LDO (DRV_3V3). |
| Nominal scaling | 12-bit raw. VBUS = count × 3.3 × 11 / 4095 V (10 kΩ / 1 kΩ). I = (count − 2048) × 3.3 / (4095 × 0.5) A: DRV8311H GAIN 47 kΩ to GND is 0.5 V/A, and CSAREF on DRV_3V3 puts zero current at VREF / 2 (SOx through 330 Ω and 22 pF). Gains, offsets and valid CSA conduction windows need measurement. |
| Logical PWM requests | A → CH3 PA10/PB1 (driver C), B → CH2 PA9/PB0 (driver B), C → CH1 PA8/PA7 (driver A). Compare is truncated duty × 29999, inclusive, for diagnostics only. |
| TMR5 / SPI1 | TMR5 overflow begins a nominal 500 µs slot and asserts PA15 CS. CH1 compare at +20 µs starts one SPI transfer and CH2 at +70 µs closes the bounded window. SPI1 RX/error IRQ captures the result. PB3 SCK, PB4 MISO, PB5 MOSI. |
| Encoder | KTH7823-X-N-QN16, active-low CS, SPI mode 3, MSB first, APB2/16, one zero TX word and one 16-bit angle word. No wire CRC. Source time is the CS assertion proxy. `sensors/foc_encoder` owns direction, bounded uint32 timestamp arithmetic, integer-backed unwrapping, raw difference speed, variable-`dt` LPF speed, alpha-beta comparison speed, and quality classification. Board1 publishes the LPF speed into the existing `foc_runtime_sensor`; the raw and tracked estimates remain local diagnostics. The absolute sensor zero and seven pole pairs are nominal. Electrical offset zero is uncalibrated. |
| IRQ priority | NVIC grouping 0, four preemption bits. ADC1 #18 and TMR1_CH #27 use priority 0. TMR5_GLOBAL #50 and SPI1 #35 use priority 1. Same-priority pairs serialize their own acquisition state. H0 may preempt H1. |
| USART1 | PB6 TX, PB7 RX, 115200 8N1, byte IRQ priority 2. Bounded RX ring and single complete TX buffer. |
| Phase voltage | PA1 / IN1 = VC → logical A, PA2 / IN2 = VB → logical B, PA3 / IN3 = VA → logical C, through 10 kΩ / 1 kΩ (11:1). Not in the H0 group (the injected group holds four channels): the coast check reads them in the foreground with three software-triggered ordinary conversions, 28.5 cycles each, while the bridge is off. |
| Driver | DRV8311H: nSLEEP pulled up (always enabled, no GPIO), MODE 47 kΩ = 6x PWM with 5 A OCP, SLEW 47 kΩ = 75 V/µs. NFAULT on PB2 is an input with the board's 10 kΩ pull-up (and LED3); H0 reads it each cycle and two low samples in a row latch DRIVER_FAULT (fault bit 16), cleared by FAULT_CLEAR once the line is high. `sil-demo --sil-scenario driver-fault` injects it with `--driver-fault-us on_us[,off_us]`. |
| CAN1 | PA11 RX / PA12 TX through the SIT3051, classic CAN, ID 122 bit rate (1 Mbit/s), RX FIFO 0 IRQ priority 2, three TX mailboxes filled by Foreground. Off on the HICK fallback clock. CANSimple adapter `foc_can.c` (protocol README, contract 25). |
| Unused pins | PB8 not connected (pulled down), BOOT0 10 kΩ to GND. |
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

## Board1 SIL (M0)

`falconfoc-board1-sil` is a Host-only build of this firmware's control path
against a virtual motor. It runs the same `foc_board_boot_select`,
`foc_board_adc_begin/complete` (H0), `foc_board_encoder_begin/complete` (H1),
`foc_board_spi_on_time/close_result`, Runtime supervision and `foc_serial_poll`
calls that the IRQ handlers and foreground loop in `src/main.c` make, in the
same order and at the same TMR1/TMR5 phases. It links the default build
supply. Only register access is replaced:

| main.c on AT32 | SIL |
| --- | --- |
| ADC1 ranks after the CH4 software trigger | plant currents quantized at the trigger aperture |
| SPI1 word after CS | plant angle latched at CS |
| TMR1 compare registers (not written; output locked) | `foc_board_output_attach` backend: UPDATE loads at the next TMR1 update, INHIBIT removes drive at once |
| TMR2 counter | virtual microsecond clock |
| USART1 RX/TX IRQs | stdin/stdout bytes |

The plant keeps its own sensor facts (ADC offsets, gains, rotor offset); the
firmware decodes with its own constants and never reads them. Time runs freely
(`--speed`, virtual/wall ratio) or in lockstep after each received frame
(`--lockstep-us`). Without frames the free-running device keeps time, so the
command timeout stops the drive exactly as on the target.

```powershell
cmake --build build/sw03-board-host --target falconfoc-board1-sil
python tools/falconfoc-cli/falconfoc_cli.py --simulator build/sw03-board-host/falconfoc-board1-sil.exe sil-demo
ctest --test-dir build/sw03-board-host -R '^test-foc-board1-sil$' -V
```

The ARM image attaches no output backend, so every valid UPDATE remains
`FOC_BOARD_UPDATE_NOT_APPLIED` there. A SIL pass is software evidence: it does
not model switching, dead time, ADC aperture error, CSA noise, MCU timing or
the power stage. The AT32 TMR1 output backend is separate work.

### M1 product control over UART

The same SIL exercises the M1 control surface: COMMAND_SET_MODE and its
LIFECYCLE_READ receipt, controller tuning through the H0 tuning mailbox
(`foc_runtime_publish_tuning`, applied only while DISABLED), and fault clear and
re-run. `sil-demo --sil-scenario current|velocity|position|tuning|recover` runs
each one; `test-foc-board1-sil` runs all of them in lockstep. The locked ARM
image accepts SET_MODE and tuning as well, since neither touches output policy.

### M2 persistent settings

`src/foc_board_store.c` implements the Board1 settings record ("FBP1", 108
bytes, layout in the file header): controller tuning and UART RAM policies,
bound to the effective motor configuration, in two slots with the
configuration-store transaction (erase inactive slot, program and verify body,
program and verify the commit word). `foc_board_persistence_restore()` applies
a matching, valid record at boot before any producer; the UART adapter gains
PARAM_SAVE, CONFIG_ERASE and readback IDs 48–50.

The SIL backs the slots with `--flash <file>`; restarting it on the same file is
a power cycle (`sil-demo --sil-scenario persist`). On the AT32 the slots are the
top two 2 KiB regions (0x0801F000, 0x0801F800); `store-reserve.ld` keeps every
image below them. The Flash driver `src/foc_at32_store.c` is linked only with
`-DFALCONFOC_BOARD_PERSISTENCE=ON`, so the default image still contains no
Flash erase or program code. Target facts still to confirm before enabling it:
the sector size, the instruction-fetch stall during erase while H0/H1 IRQs are
enabled (a save from DISABLED may then miss H0 deadlines), and endurance.

### M3a on-MCU calibration

`src/foc_board_calibrate.c` runs CAL-1 (ADC zero, collected in H0 through the
Board ADC tap) and CAL-2 (direction, pole pairs, electrical offset) on live
firmware data, admits the candidates with the existing `foc_calibration_admit`
and re-initializes the instance with `foc_board_reconfigure`, which keeps the
output backend and the H0/H1 lane schedules of the running producers. CAL-2
drives through the Runtime CALIBRATION mode: a commanded d/q voltage at a
commanded electrical angle (`foc_runtime_publish_calibration_command`), applied
by the Pipeline without the current PI, with the phase current decoded every
cycle and a vector above the current limit stopping the output. The estimators
work in the source sensor frame; admission maps that to the Runtime direction.

`sil-demo --sil-scenario calibrate` gives the SIL a motor the firmware does not
know (11 pole pairs, 1.1 rad offset, ADC zero 2078/2030/2060): velocity control
fails before and tracks after, and a STOP mid-sweep aborts with the old
configuration kept. On the ARM image `main.c` binds the manager; the locked
image answers CALIBRATE_START with OUTPUT_LOCKED, and the quiesce hook masks the
four producer IRQs around reconfiguration. The SIL models an inhibited bridge
as open windings (no current) rather than shorted terminals.

### M3b electrical calibration

The same manager continues with CAL-3 (Rs, Ld, Lq through `foc_motor_id`),
CAL-4 (flux and Kt through `foc_flux_id`) and CAL-5 (current PI through
`foc_motor_pi_design`, applied by the H0 tuning mailbox). A post-H0 Board
observer streams each cycle's d/q current, applied d/q voltage and speed on the
slot clock into a 64-entry ring that the Foreground drains. Excitation runs in
voltage-mode FOC on the calibrated rotor angle (`follow_rotor` drive commands),
so no fixture is needed. The q-axis pulses turn a free light shaft, so CAL-3
fits its back-EMF from the rotor angle and takes R from the still d axis
(`fit_back_emf`, `known_rs_ohm`). The encoder samples at the H1 rate, so each
record waits for the encoder sample after its ADC aperture and the angle is
interpolated to the aperture (`runtime->adc.produced_us`); a stale angle would
add Kt x age / J to Rs. The d-axis fit also identifies the inverter dead-time
voltage against each record's phase-current sign vector (`fit_dead_time`); the q
axis and CAL-4 subtract it, and ID 95 reports it (`sil-demo --sil-scenario
dead-time`, with the SIL's `--dead-time-v`). The current loop can compensate it
(`foc_current_config.dead_time_v`, ID 87, store format 7), off by default and
enabled by the Host (`sil-demo --sil-scenario dead-time-comp`). Each CAL-4 level starts its capture once the speed
(mean over 100 ms from the encoder angle) has the level's sign and is within 5 %
of steady, estimated from two successive changes; a heavy rotor takes seconds and
after a reversal still turns the old way. A level waits at most 10 s, then the
run ends TIMEOUT. `sil-demo --sil-scenario rotor-range` identifies Rs, Ld, Lq
and flux from a bare 1e-6 kg·m² rotor to 0.05 and a 3 Ω motor within 0.5 %, and
shows the TIMEOUT of a 0.2 kg·m² rotor. The voltage is declared `COMMANDED` (appended to
`foc_motor_id_voltage_source`), and `foc_flux_id_break()` lets each settled speed
level be its own segment. The manager shares the UART adapter's publication and
lifecycle sequences, so the Host keeps control after a run. `sil-demo --sil-scenario
calibrate` uses a plant whose Rs, L and flux also differ from the Board1 defaults.

### M3c stored calibration

The settings record grows to format 2 (140 bytes): a flags word and the applied
calibration snapshot (calibrated ADC zero, pole pairs, electrical offset,
calibration and Runtime directions). The identity stays the image's boot
configuration, so a calibrated motor still matches. At boot
`foc_board_persistence_restore()` re-initializes the instance with
`foc_board_reconfigure()` before applying the tuning; a refused calibration
restores the boot tuning and reports VALUE_REJECTED. The manager adopts the
restored snapshot (`foc_board_calibration_adopt()`), so later saves keep it and
the electrical stages can rerun alone; without an alignment in this boot CAL-3
first probes its drive voltage. Readback ID 51 reports the restore.
`sil-demo --sil-scenario calibrate-persist` calibrates the hidden plant, saves,
power-cycles, runs velocity without recalibrating, reruns CAL-3/4/5 and erases.

### M3-R calibration correctness

A source audit of M0–M3 found calibration paths that ended FAILED with CAL-1/2
already applied, left the Runtime in CALIBRATION mode, or read back zeros
after a boot restore. The run now commits in two named phases (rotor at APPLY,
electrical at TUNE); a failure after SET_MODE CALIBRATION passes through RESTORE
to CURRENT mode first; CALIBRATE_READ grows to 93 bytes with the committed
phases, the results present, the active source (boot / run / storage), the ring
high-water mark and the active Runtime pole pairs, offset and ADC zero. CAL-1
refuses a zero noisier than 4 counts rms before driving. The CAL-2 sweep runs at
half an electrical turn per second: at one turn per second the reversal rang
on high-Rs motors (back-EMF damping falls with Rs) and CAL-2 admission failed
from about 1 Ω. The SIL gains `--adc-noise <counts rms>`, and
`sil-demo --sil-scenario calibrate-faults` covers the refused noisy zero, CAL-3
failing after CAL-1/2 (save and restore of the committed phase), and an abort
in CAL-3 followed by Host control. Motor identity is still the boot
configuration (see the protocol README).

### Motor protection

H0 evaluates `g3/safe/foc_protect` after every accepted running cycle:
over-current, bus over/under-voltage, over-speed, sustained current, stall and
tracking divergence (protocol README, Motor protection). A trip latches fault bits
9–15 with INHIBIT and a snapshot (FAULT_SNAPSHOT_READ, 0x22); FAULT_CLEAR clears
protection latches. Thresholds are tuning IDs 112–118, RAM-only until the
parameter record grows (contract format 3). `sil-demo --sil-scenario protect`
drives the nominal profile against six SIL motors (same, 17°, 86°, 172° offset,
11 and 14 pole pairs), injects each rule (`--adc-step us,counts`, `--vbus-v`,
lowered limits, `--motor-damping`), clears a latch and checks that a deliberate
+5 → −5 rad/s reversal does not trip. Torque-mode reversal from a wrong offset is
not detected.

### Motor profile and store format 3

The motor model (Rs, Ld, Lq, flux, inertia, damping), the user profile (rated
and peak current, maximum speed, gear ratio) and the current-loop bandwidth
travel inside the Runtime tuning, so H0 applies them with the PI, the limits and
the protection thresholds as one candidate. `foc_board_profile` is the unified
refresh: a model or bandwidth write re-derives the current PI with
`foc_motor_pi_design()` at the H0 period, a peak current or maximum speed
tightens the limits, and CAL-3/4/5 write the identified model through the same
path. Store format 3 (212 bytes) adds the profile and IDs 112–118 after the
format-2 bytes; format 4 (216 bytes) adds the output zero (ID 124), format 5
(224 bytes) the acceleration limits (IDs 106/107) and format 6 (232 bytes) IDs
108/109; older records load with the boot profile.

### Inertia identification (CAL-7)

`CALIBRATE_START` mask 8 (alone, or 9–11 with the rotor/electrical stages) runs
CAL-7 on the MCU: a bang-bang q voltage keeps the rotor within ID 109 of its start
while `foc_mechanical_id` fits J and B on windows a probe sizes to the motion. TUNE
writes IDs 85/86 and `foc_board_profile` derives the velocity PI and position gain
at ID 108; H1 feeds the planned torque forward. It is opt-in because the shaft
turns with its load attached: `falconfoc_cli.py motor-setup --mechanical` or
`request --message calibrate --cal-stages mechanical`. `sil-demo --sil-scenario
inertia` identifies a 2e-5 (CAL-3/4/5 and CAL-7 from nothing), a 0.002 and a
0.01 kg·m² rotor and shows the derived
gains, the travel bound, the feedforward move and the power-cycle restore.

### Trajectory shaping

IDs 106/107 travel in the Runtime tuning: either nonzero enables the H1 planner
with the ID 72 speed limit; POSITION mode then tracks a trapezoid planned on its
own reference. `sil-demo --sil-scenario trajectory` (`--load-torque-nm t,on_us`
is the SIL's constant load) shows the velocity ramps, a 6 rad trapezoid, the
refused values, the power-cycle restore, the erase and a loaded hold without a
position integrator.

### Output frame

`foc_serial` converts at the UART boundary only: velocity and position targets,
TELEMETRY position/velocity and the remote position cap are output-side through
the gear ratio (ID 99) and the zero offset (ID 124); the Runtime stays rotor-side.
SET_ZERO (0x54) makes the present output position zero while DISABLED.
`falconfoc_cli.py request --message set-zero` sends it, and `sil-demo
--sil-scenario frame` shows gear-scaled position and speed, the refusals while
running, SET_ZERO, the power-cycle restore and the erase. `sil-demo
--sil-scenario profile` checks the derived gains, the refused bandwidth and
model, the tightened limits, the rated-current trip, the power-cycle restore and
the calibration write-back.

### Commutation gate and motor replacement

ARM/START require a valid commutation group and an ADC zero measured in this boot
(NOT_CALIBRATED / BUSY otherwise). The calibration manager measures the zero by
itself (zero-only CAL-1, no drive) once a group is valid; COMMUTATION_WRITE (0x53)
installs a group by re-initialization; ID 17 trims its offset; CONFIG_ERASE returns
the running instance to the boot configuration. After any re-initialization H0
keeps INHIBIT without counting angle-less samples toward the invalid-sample latch
until H1 publishes the first sensor (sensor warm-up). `falconfoc_cli.py
motor-setup --rated-current --peak-current --max-speed` runs the replacement
workflow (calibrate, profile, save), and `sil-demo --sil-scenario swap` shows the
gate, motor-setup, a replaced motor on the old profile caught by protection, the
recommissioned motor, an exported-group import, the trim and the erase.

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

### CAN (CANSimple)

`foc_can.c` translates CANSimple frames into the same Foreground requests as UART
(`foc_serial_execute`), so both transports share admission, limits and the lease.
`main.c` starts CAN1 after the settings restore (the saved bit rate applies from that
boot) and before the IRQs; Foreground calls `foc_can_poll` after `foc_serial_poll`.
The SIL takes `--can <serial>` to multiplex UART and CAN frames on stdio; the CLI runs a
virtual bus of SIL nodes (`sil-demo --sil-scenario can` and `can-multi`, lockstep) and
offers `can --can-command scan|assign|state|mode|set|read|write|watch|save` over a SIL
node or, through python-can, a USB-CAN adapter (not exercised without hardware).
