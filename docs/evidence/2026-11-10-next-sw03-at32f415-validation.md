# NEXT-SW03 AT32F415 reference firmware validation

The local source baseline is HEAD `1f542521189d5e0ef25e64e6530608ece8d6e137`
with the existing uncommitted V2 work. Board connections, clock tree, IRQ
allocation and reproduction commands are in the
[reference firmware README](../../firmware/at32f415/README.md).

## Cross-build evidence

The independent Ninja build in `build/sw03-at32-final` used Arm GNU Toolchain
14.3.1 with `-Os -mcpu=cortex-m4 -mthumb -mfloat-abi=soft`. ELF attributes
reported ARMv7E-M, Thumb-2 and EABI5 soft-float. The vector table starts at
`0x08000000`. Each vector word was compared with its resolved Thumb symbol.

| Vector | Word |
| --- | ---: |
| Reset | `0x08000f01` |
| ADC1 | `0x08000d3d` |
| TMR1_CH | `0x08000cd5` |
| SPI1 | `0x08000ead` |
| TMR5_GLOBAL | `0x08000da9` |

One `main`, four strong IRQ handlers and zero undefined symbols. Symbol and
source checks found no Legacy startup, FreeRTOS, Shell, allocation, watchdog
setup or TMR1 output-enable calls. TMR1 receives a peripheral reset and its
CH4 compare value. The six bridge inputs use GPIO analog mode and have no
timer mux assignment. The Startup and output paths establish a software lock.

| Item | Bytes |
| --- | ---: |
| ELF / BIN / HEX / MAP | 54,088 / 28,608 / 80,550 / 208,425 |
| Flash used / capacity | 28,608 / 131,072 |
| Linker RAM used / capacity | 2,512 / 32,768 |
| `.text` / `.rodata` | 26,860 / 1,280 |
| `.data` / `.bss` | 84 / 1,400 |
| Static Runtime | 972 |
| Vendor `._user_heap_stack` including padding | 1,028 |
| RAM gap from `_ebss` to `_estack` | 31,284 |

BIN SHA-256 `e7781606381c7e7679cfdf76603dd094300774691ea8b7718c1ed1b5b03b395b`.

| Selected linked code | Bytes |
| --- | ---: |
| H0 / H1 entry | 1,356 / 744 |
| Current / velocity / position step | 684 / 576 / 256 |
| Pipeline guarded step / Safety gate | 752 / 190 |
| Device map / locked Board1 submit | 436 / 220 |

GCC `.su` frames include 32 B main, 528 B Runtime initialization, 384 B
Pipeline initialization, 24 B ADC IRQ, 64 B ADC completion, 216 B H0, 312 B
Pipeline step, 144 B current step, 24 B TMR5 IRQ, 64 B encoder completion, 240 B
H1 and 216 B foreground poll. The initialization call chain alone includes at
least 1,064 B of owned frames. The running budget considers foreground + H1 + H0,
two hardware exception frames, nested helpers and library calls. The independent
linker assertion requires at least 4 KiB above statics for MSP. It is a
provision, and the reported `.su` values are individual frames.

The image links software trigonometry, square root and float division. Target
execution cost arguments remain zero in this port. The 200/500 µs scheduling
configuration, stack high-water mark, and real deadline margins need target
measurements. GNU ld reported pre-GC newlib-nano syscall stub warnings and the
vendor script's RWE Flash load segment. The final symbol table has no undefined
references and no allocation entry point.

## Test and quality evidence

| Lane | Result |
| --- | --- |
| Board Port | 1/1. Fake time, ADC completion/overlap, SPI events and continuity loss, phase mapping, and PI rollback through the actual locked adapter. |
| Core / Runtime / G3-NP / G3-SAFE | 5/5, 4/4, 2/2, 1/1. Runtime covers fault, timeout, restart, snapshot interleaving and outer-loop acknowledgement. |
| Legacy | Host 45/45 and a fresh Bench build using Cortex-M3 soft-float. |
| V2 source and style | 34 C/H files, five CMake and README entries in the source contract. clang-format and strict formatting of the added CMake file pass. |
| FalconFOC scoped gate / Firmware CLI contract | Both pass. The CLI contract ran separately with Git Bash. |
| Full gate | Stopped at the existing root CMake format check after the V2 source contract. The installed formatter's extensionless config loading ignored the Python sections. The standalone CMake check used a byte-identical `.py` copy of the repository configuration. |

The Board1 encoder is the full-angle SKU without a wire checksum. Transport
errors, frame handling, timeouts and implausible displacement have Host evidence.
An in-range data-bit error or stuck data value can escape detection. The
requested wire checksum rejection subcase has no evidence on this SKU.

Physical reset state, the nFAULT/nSLEEP/R7 and Break paths, WDT, SPI/ADC waveforms,
CSA validity and physical bridge shutdown remain open under the hardware gate.
