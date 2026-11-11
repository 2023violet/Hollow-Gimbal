# NEXT-SW04 UART protocol and Host tool validation

The implementation extends the existing uncommitted V2 baseline at HEAD
`1f542521189d5e0ef25e64e6530608ece8d6e137`. The
[wire specification](../../protocol/README.md) freezes all layouts and the
[CLI README](../../tools/falconfoc-cli/README.md) provides tool reproduction.

## Delivered behavior

The independent C11 codec reuses repository COBS and implements CCITT-FALSE.
Raw frames occupy at most 114 bytes, wire frames 116 bytes. The adapter owns
boot-local sessions, modulo sequences and three Foreground RAM policies.
Setpoints publish once with the MCU validation time. Their ACK carries the
Runtime publication identity, queryable through the H0 snapshot.

STOP revokes the command and uses an independent Runtime inhibit request. A
reserved response slot allows it through ordinary TX congestion. Isolated
command-timeout clear is queued to H0 and revalidated against sensing, time,
Foreground progress and inhibition. Other latched fault masks are rejected.
ARM and START return OUTPUT_LOCKED. Physical applied stays false.

The three writable policies are RX bytes per poll, remote current-vector cap
and absolute position-reference cap. Updates require the DISABLED software
inhibit view. Controller settings are read only. Parameter metadata marks
nominal pole pairs and electrical offset as requiring calibration.

USART1 uses PB6/PB7 remap, 115200 8N1 and priority 2. DMA1 CH1 RX and CH2 TX
were cross-checked against the SDK and existing console owner, and remain free
in this image. The byte IRQ has at most one RX/error operation and one TX
operation. H0 remains priority 0 and H1 priority 1. Protocol parsing runs after
Runtime supervision, with a cap of 8–32 bytes and one completed frame per poll.
The nominal event-rate bound is 23,040 full-duplex byte events/s.

The Python codec and C simulator exchange actual framed pipe bytes. Physical
serial selection is implemented using the locally installed pyserial 3.5,
with no dependency-manifest change. The simulator clock is fixed at zero.
Advancing-time and overload checks use C Host tests.

## Reproduction

```powershell
cmake -S protocol -B build/sw04-protocol -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build/sw04-protocol
ctest --test-dir build/sw04-protocol --output-on-failure

cmake -S firmware/at32f415 -B build/sw04-host -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build/sw04-host
ctest --test-dir build/sw04-host --output-on-failure

cmake -S firmware/at32f415 -B build/sw04-at32 -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=D:/study/Software/Hollow-Gimbal/tools/cmake/arm-gcc-toolchain.cmake `
  -DMCU_CPU=cortex-m4 -DMCU_FLOAT=soft -DMCU_FPU=none
cmake --build build/sw04-at32
```

Artifacts are `build/sw04-at32/falconfoc-v2-at32f415.{elf,bin,hex,map}`.
Arm GCC 14.3.1 uses -Os, Thumb-2, ARMv7E-M and EABI5 soft-float.
The BIN SHA-256 is
`b2743f8fe788b3ea923fd094c4ae40ae3c93326c11e76b35bcbd1f680e763eb7`.
The compiler build log is `build/sw04-at32/build.log`. Each CTest lane retains
its last run under `Testing/Temporary/LastTest.log`.

## Acceptance

| AC | Result | Evidence |
| --- | --- | --- |
| 01 | PASS | Standalone protocol library and CTest 1/1, no Runtime/MCU dependency. |
| 02 | PASS | C/Python stream tests cover zeros, empty delimiters, maximum frames, oversize, malformed COBS, split and joined frames, and resynchronization. |
| 03 | PASS | 0x29B1 check, 22 fixed vectors, and all eight single-bit flips at each raw business-frame byte, including CRC. |
| 04 | PASS | Explicit LE fields, magic, version, reserved, length, flags and unknown type tests. |
| 05 | PASS | All V1 IDs have codec vectors and dispatch outcomes. Response vectors additionally pass the Python business decoder. |
| 06 | PASS | Zero, wrap, duplicate, stale/half-range sequence, wrong session, HELLO retry and old-session invalidation. |
| 07 | PASS | Accepted command retains its MCU publication time through repeated/CRC-bad traffic until the real Runtime raises COMMAND_TIMEOUT. |
| 08 | PASS | Status, fault and telemetry responses carry mode, lock, seen flags, source times and validity. |
| 09 | PASS | ARM/START locked, congestion STOP reserve, fault restrictions and clear completion through H0. |
| 10 | PASS | Typed ID reads include units, range and applied/calibration attributes. |
| 11 | PASS | Whitelist, range, NaN, read-only and RUNNING/pending-stop rejection; RAM-only writes report APPLIED. |
| 12 | PASS | PARAM_SAVE reports NOT_SUPPORTED, with no Flash erase/program symbol. |
| 13 | PASS | RX position wrap, full queue, gap recovery, UART error, TX immutable ownership and reserved STOP response. |
| 14 | PASS | Python 4/4 tests compare both codecs to the shared vectors and exchange messages with the real C adapter. |
| 15 | PASS | Runtime publication, revoke and restricted recovery retain H0/H1 ownership. |
| 16 | PASS | 100 simulated H0 periods under byte traffic and TX congestion, per-poll cap preserved, supervision first, no simulated missed H0 period. |
| 17 | PASS | Actual linked resource comparison and stack-frame records below. |
| 18 | PASS for requested lanes | Core 5/5, Runtime 5/5, G3-NP 2/2, G3-SAFE 1/1, Board/Transport/Python CTest 3/3, Protocol 1/1, Legacy Host 45/45, fresh Legacy Bench and independent V2 build. |

The FalconFOC quality scope, source contract for 43 C/H files and six CMake/
README entries, clang-format, strict touched CMake formatting and diff checks
pass. Strict CMake checks use a byte-identical .py copy of .cmake-format to
load the repository's Python configuration correctly. The full `all` gate
stops at the existing root CMake format difference.

A read-only review found two stale response vectors. Adding business-decoder
validation reproduced BAD_RESPONSE, updating the two payloads fixed it, and
the resulting four Python tests passed. The reviewer found no additional
actionable concurrency or software safety defect in the inspected paths.

## Resource comparison

The SW03 column comes from the preserved ELF in `build/sw03-at32-final`.

| Item | SW03 B | SW04 B | Delta B |
| --- | ---: | ---: | ---: |
| Flash including initialized data | 28,608 | 33,768 | +5,160 |
| Linker RAM including reserved section | 2,512 | 3,376 | +864 |
| .text | 26,860 | 31,976 | +5,116 |
| .rodata | 1,280 | 1,324 | +44 |
| .data | 84 | 84 | 0 |
| .bss | 1,400 | 2,268 | +868 |
| Static Runtime | 972 | 1,020 | +48 |

The static UART context is 652 B, including the 512 B RX ring and 116 B TX
frame buffer. The protocol/session/RAM-policy context is 168 B, including the
reserved STOP metadata. The ROM growth stays within a 16 KiB round budget and
static RAM growth within 2 KiB.

Selected linked symbols are UART IRQ 100 B, UART RX push 72 B, TX pop 62 B,
protocol CRC 56 B, encode 198 B, decode 230 B, serial poll including inlined
dispatch 1,880 B, and Runtime timeout-clear request 94 B.

GCC stack frames are main 32 B, serial poll 424 B, protocol encode 160 B,
decode 144 B, COBS 20 B, UART IRQ 16 B, H0 entry 96 B, H0 control 200 B,
H1 entry 264 B, foreground poll 240 B and coherent snapshot copy 184 B.
Interrupt nesting now includes UART below H1/H0. The 4 KiB stack-headroom
assertion passes, and the gap from statics to MSP top is 30,416 B. Individual
frames plus the provision do not establish measured stack high-water or WCET.

Resolved vector words were checked against the actual symbols, including
USART1 at `0x08000fe9`. Undefined symbols and forbidden Legacy, allocation,
PWM-enable, WDT and Flash erase/program symbols were absent. The existing
vendor-linker RWE load-segment and newlib-nano pre-GC syscall warnings remain
visible in the build log.

## Limits and next evidence

The writable policies affect Foreground admission only. V1 exposes no remote
control-mode switch and starts in CURRENT. The restricted recovery command
handles an isolated software command-timeout latch, with other causes held.
Streaming telemetry and Flash persistence explicitly report NOT_SUPPORTED.
Current telemetry is marked invalid in the locked image, and mechanical
feedback retains its uncalibrated/source-time qualification.

Physical UART waveforms, DMA/IRQ timing, baud-rate error, ISR latency/WCET,
control jitter under UART load, Flash persistence, motor output and DRV8311H
behavior have no new hardware evidence. G3-SAFE hardware and G3-PWR remain
governed by their existing gate.
