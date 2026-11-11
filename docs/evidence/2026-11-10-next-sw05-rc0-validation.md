# NEXT-SW05 FalconFOC V2 RC0 software acceptance

The acceptance baseline is the local worktree at HEAD
1f542521189d5e0ef25e64e6530608ece8d6e137. Existing uncommitted V2 assets were
preserved. build/rc0/baseline.json stores compiler versions, worktree status
and source hashes captured before the RC0 additions.

## Reproduction environment

The host used Clang 22.1.8, GCC 16.2.0 from MinGW, CMake 4.4.2, Ninja 1.13.2,
Python 3.13.15 and Arm GNU Toolchain 14.3.1. The target build used
-mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os and the existing AT32F415 xB
linker script with a 4 KiB stack-headroom assertion.

The completed lanes were Core 5/5, Runtime 6/6, G3-NP 2/2, G3-SAFE 1/1,
Protocol 1/1, Board/UART/Python 3/3, Legacy Host 45/45, Legacy Bench and the
independent V2 cross-build. The FalconFOC scoped quality gate passed. During
this RC0 recheck the root CMakeLists.txt formatter issue was cleared without a
semantic CMake change; the full all gate then exposed additional formatter
differences in existing shared CMake files and the new standalone V2 manifests.
Those files were not reformatted because this task authorizes only the root
format correction and forbids unrelated Legacy CMake changes.

## Reliability evidence

The protocol parser now runs a deterministic 10,000-iteration random input
stream in the C test. The UART pressure test injects bytes at a deterministic
87 microsecond interval, mixes valid, CRC-bad and oversize frames, forces RX
quarantine and slows protocol polling. Its output is:

    uart pressure crc=29 dropped=88 rx_loss=29 updates=0 h0_late=0 h1_late=0 fg_late=0

The stream never produces a control UPDATE, never latches a fault and keeps
H0, H1 and Foreground deadlines healthy. The TX congestion test proves that
STOP is published before the ordinary response is drained and that its
reserved acknowledgement can be sent afterward.

The long Runtime executable replays 100,000 control cycles at 100 microseconds.
It produces the same result twice:

    long replay hash=49dea950 updates=99998 due=100001 missed=0 late=0 budget=0
    long replay hash=49dea950 updates=99998 due=100001 missed=0 late=0 budget=0

Existing PMSM model output remains within its approved gates. Runtime speed +10
rad/s settles in 0.4543 s, speed -10 rad/s settles in 0.5134 s, position +7
rad settles in 1.2569 s and position -3 rad settles in 1.5079 s. The integrated
model replay fingerprint is 884b005e. The complete trace is in
build/rc0/motion-metrics.log.

## AC01 through AC18

| AC | Result | Evidence |
| --- | --- | --- |
| AC01 independent builds | PASS | All Core, Runtime, Device, Safety, Protocol and Board lanes build and run. |
| AC02 continuous UART load | PASS | 87 us byte stream, mixed corruption, oversize input, ring wrap and slow polling. |
| AC03 malformed frames | PASS | Random C parser stream, Python corruption/recovery, oversize and incomplete COBS. |
| AC04 STOP priority | PASS | Reserved STOP publication and acknowledgement under TX congestion. |
| AC05 Session and sequence | PASS | Zero, wrap, replay, half-range ambiguity, old session and new HELLO. |
| AC06 parameter safety | PASS | Whitelist, read-only, finite/range checks, state guard and RAM-only dirty state. |
| AC07 long Runtime | PASS | 100,000 cycles, 0 missed, 0 late and 0 budget overrun. |
| AC08 deterministic replay | PASS | Long-stream hash and existing model replay are identical. |
| AC09 transaction commit | PASS | Current, velocity and position rejection preserve inner and outer state. |
| AC10 control quality | PASS | Existing current, speed and position model gates pass. |
| AC11 memory safety | PASS | LLVM Clang 22.1.8 with `-fsanitize=address,undefined` built and ran Core 5/5, G3-NP 2/2, G3-SAFE 1/1, Runtime 6/6, Protocol 1/1 and Board/UART/Python 3/3 (18/18 total). The LLVM dynamic runtime directory was supplied only through the test-process `PATH`; no production target was changed. |
| AC12 target firmware | PASS | Final ELF includes Protocol, UART and IRQ paths with no undefined symbols. |
| AC13 resource report | PASS | Final ELF, MAP, symbols and su frames recorded below. |
| AC14 software output lock | PASS | ARM/START locked, UPDATE unapplied, no PWM unlock path. |
| AC15 CLI | PASS | Complete simulator flow for HELLO, capability, status, fault, parameters, setpoint, lock and STOP. |
| AC16 complete regression | PASS for requested software lanes | Core, Runtime, NP, Safety, Protocol, Board/UART/Python, Legacy Host, Legacy Bench and V2. |
| AC17 evidence | PASS | This report, baseline hashes, logs, vectors and commands are stored. |
| AC18 scoped changes | PASS | Changes are acceptance tests, direct robustness coverage, and evidence/log updates. |

## Target ELF resources

Final artifacts are in build/rc0/baseline-arm.

| Section or object | Bytes |
| --- | ---: |
| Flash initialized image | 33,768 |
| .text | 31,976 |
| .rodata | 1,324 |
| .data | 84 |
| .bss | 2,268 |
| Linker RAM statistic | 3,376 / 32 KiB |
| Static Runtime | 1,020 |
| UART context | 652, including 512 B RX and 116 B TX |

The BIN SHA-256 is
B2743F8FE788B3EA923FD094C4AE40AE3C93326C11E76B35BCBD1F680E763EB7.
ELF attributes report ARMv7E-M, Thumb-2 and EABI5 soft-float. The vector table
contains the single Reset, ADC1, TMR1_CH, TMR5_GLOBAL, SPI1 and USART1
handlers. arm-none-eabi-nm -u reports no undefined symbols and the forbidden
Legacy, FreeRTOS, Shell, allocation, PWM-enable, WDT and Flash program symbols
are absent.

Selected linked sizes are UART IRQ 100 B, foc_serial_poll 1,880 B,
foc_protocol_encode 198 B, foc_protocol_decode 230 B, H1 744 B, H0 752 B,
Foreground 552 B and foc_current_step 684 B. GCC su records include 32 B main,
424 B protocol poll, 160 B protocol encode, 144 B decode, 16 B USART IRQ,
264 B H1, 240 B Foreground and 200 B H0 control frames. The gap from _ebss to
_estack is 30,416 B. This is static provision, not measured stack high-water
or WCET.

## Safety and hardware boundary

The final image still has no runtime PWM unlock, WDT setup, Flash erase/program,
Legacy application, FreeRTOS scheduler or Shell dependency. UPDATE remains
software-checked and physically unapplied. Software INHIBIT is reported at the
adapter scope and does not certify bridge shutdown.

Physical UART waveforms, baud error, MCU ISR timing, ADC windows, encoder
electrical errors, reset pin levels, DRV8311H faults, WDT behavior and real
motor performance have no new hardware evidence. G3-SAFE hardware remains
unpassed and G3-PWR remains locked.

## NEXT-SW05R recheck

The two requested software blockers were rechecked against the current
worktree.

| Blocker | Result | Evidence |
| --- | --- | --- |
| AC11 ASan/UBSan | PASS | `build/rc0/sanitizer-final.log` and `build/rc0/sanitizer-summary.txt`; a clean sanitizer probe compiled and ran, then six independent Clang/Ninja Host builds completed with 18/18 CTest tests and no sanitizer report. The compiler was `clang 22.1.8`, target `x86_64-pc-windows-msvc`; the runtime DLL directory was added to the test process `PATH`. |
| `Scope all` | BLOCKED | `build/rc0/scope-all-final.log` and `build/rc0/cmake-format-inventory-final.txt`; root `CMakeLists.txt` now passes the active formatter configuration, then `core/CMakeLists.txt` is rejected. The complete inventory identifies `g3/np/CMakeLists.txt`, `g3/safe/CMakeLists.txt`, `tests/CMakeLists.txt`, `cmake/ThemisBuildAxes.cmake` and `cmake/ThemisContracts.cmake` as not clean under the same formatter. |

The CMake gate tooling was repaired to recognize the repository's extensionless
`.cmake-format` Python configuration and to check Unix line endings through
temporary normalized inputs. This does not rewrite source files. Root
`CMakeLists.txt` was converted to LF only; `git diff -w -- CMakeLists.txt`
contains no semantic change. No control algorithm, protocol behavior, Runtime
logic, board register path or hardware state was changed in this recheck.

The final identity record is `HEAD=1f542521189d5e0ef25e64e6530608ece8d6e137`.
The worktree remains intentionally dirty with the pre-existing V2 files and
new evidence; `build/rc0/status-final.txt` records 98 status entries and
`build/rc0/worktree-sha256.txt` records the 2,288 tracked/untracked source
files included in the final content fingerprint.

Because `Scope all` is not closed, the RC0 software decision remains:

    RC0 SOFTWARE BLOCKED

The remaining blocker is formatter debt outside the authorized root-only scope;
the ASan/UBSan blocker is closed. G3-SAFE hardware and G3-PWR remain locked.
