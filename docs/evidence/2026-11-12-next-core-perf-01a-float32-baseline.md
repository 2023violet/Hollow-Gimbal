# FalconFOC V2 NEXT-CORE-PERF-01A｜Float32 control-path baseline

## Scope and identity

This is a read-only performance and numerical baseline for the existing
Float32 control path. No production algorithm, Runtime, Safety, Board, or
Protocol implementation was changed. The only source changes in this round
are the two standalone baseline test targets and their test files:

- `core/tests/test_foc_baseline.c`
- `core/tests/test_legacy_sincos_baseline.c`
- `core/CMakeLists.txt` test registration only

The worktree was on branch `codex/falconfoc-v2-rc0` at
`46bc963f5e5849e4440d1714a845fe485d338837` before the baseline test files were
added. Existing worktree changes were preserved. Source fingerprints for the
inputs and test witnesses are in `build/perf01a/source-fingerprints.txt`.

Host tools:

```text
Clang 22.1.8, target x86_64-pc-windows-msvc
CMake 4.4.2
Ninja 1.13.2
Arm GNU Toolchain 14.3.1
Python 3.13.15
```

The ordinary Host baseline was configured as a Release Ninja build. The
instrumented call-count build used Clang `-O0 -g -fno-inline -finstrument-functions`
and is explicitly excluded from timing results.

## Runtime call chain

The current reference firmware path is:

```text
TMR1/ADC completion
  -> foc_runtime_h0_step
    -> foc_runtime_control
      -> foc_pipeline_step_guarded
        -> foc_current_step
          -> Clarke
          -> measured Park
          -> current-vector limit
          -> inverse Park
          -> SVPWM
          -> duty-to-voltage reconstruction
          -> applied-voltage Park
          -> optional anti-windup admission and second modulation
        -> foc_device_map_output
        -> final Safety gate
```

When motion mode is enabled, H1 additionally performs `foc_rotor_electrical`
and the outer-loop work before publishing the target consumed by H0. The
foreground and H1 calls in the reference firmware currently pass simulated
cost `0U`; these values are not real MCU execution measurements.

## Static call-site baseline

The complete source inventory is in `build/perf01a/static-call-sites.txt`.
The important current-loop locations are:

| Operation | Static locations in one current step | Meaning |
| --- | ---: | --- |
| `fmodf` | 1 helper location | Reached by each Park/inverse-Park angle wrap |
| `sinf` / `cosf` | 1 location in Park and 1 in inverse Park | Same angle is calculated independently |
| `atan2f` | 1 in SVPWM | Used only for sector metadata; duty generation uses phase/common-mode values |
| `sqrtf` | current-vector limit, SVPWM magnitude, and voltage-vector magnitude | Some calls are path dependent |
| `hypotf` | 1 in applied-voltage reconstruction | Used for voltage-limit verification |
| `foc_current_modulate` | 1 normal path, 2 when anti-windup correction remodulates | Second pass is conditional |
| `foc_config_validate` | 1 through SVPWM per modulation pass | Static PWM configuration is rechecked in the hot path |

## Dynamic call-count baseline

The instrumented executable writes `build/perf01a/calls.csv`. The columns are
`fmodf`, `sinf`, `cosf`, `atan2f`, `sqrtf`, `hypotf`, `Clarke`, `Park`,
`inverse Park`, `SVPWM`, `config_validate`, `foc_current_modulate`,
`foc_current_limit_vector`, `foc_current_admit_integral`, and
`foc_current_step`.

| Path | `fmodf` | `sinf` | `cosf` | `atan2f` | `sqrtf` | `hypotf` | Park | inverse Park | SVPWM | config | modulate | limit | admit |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Normal | 3 | 3 | 3 | 1 | 3 | 1 | 2 | 1 | 1 | 1 | 1 | 2 | 0 |
| Low VBUS / vector or duty saturation with correction | 5 | 5 | 5 | 2 | 5 | 2 | 3 | 2 | 2 | 2 | 2 | 3 | 2 |
| Saturation without changed integral admission | 3 | 3 | 3 | 1 | 3 | 1 | 2 | 1 | 1 | 1 | 1 | 2 | 2 |

The counts are current-loop counts only. They are not WCET and they do not
include interrupt entry, ADC, SPI, Runtime scheduling, Safety, or UART work.

## Golden Float32 output baseline

The ordinary executable was run as:

```text
cmake -S core -B build/perf01a/cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/perf01a/cmake --parallel 4
build/perf01a/cmake/test-foc-baseline.exe --golden
```

The output is a deterministic CSV containing, for each angle sweep, boundary,
normal, low-VBUS, vector-limit, duty-limit, saturation, reversal, release, and
invalid-input case:

- input configuration and phase samples;
- electrical angle and d/q target;
- status and output-enable result;
- target-limit and saturation flags;
- sector and Device phase compare values;
- measured d/q current;
- reconstructed d/q voltage;
- duties;
- integral state before and after the step.

The baseline contains 4,127 CSV lines and its SHA-256 is:

```text
AC74F39988D5FECBA870A58F9059E6701832683D6175EBCE02C57DFE71284A27
```

The raw file is `build/perf01a/golden-cmake.csv`; it is intentionally kept in
the ignored build evidence area rather than added as a production data file.
The same test is registered as `test-foc-baseline --golden` in the Core CTest
project.

The invalid-input cases return the existing deterministic errors and leave the
controller state/output unchanged. No new tolerance or control behavior was
introduced by this baseline.

## Legacy fast sine/cosine comparison

`test_legacy_sincos_baseline` scans 1,048,576 uniformly spaced angles over
`[0, 2π)`, plus values immediately around quadrant boundaries. It compares
`foc_fast_sincos` from `applications/algo/foc_math.h` with host `sinf/cosf`.
The output is in `build/perf01a/legacy-sincos.csv` and its SHA-256 is:

```text
814370FE48CAD50C8EA3D250694801D1F3A6DC535B88700E574F4CA6F6AFBE6C
```

Measured results:

| Metric | Result |
| --- | ---: |
| Scan step | `5.99211262e-06 rad` |
| Maximum component absolute error | `5.99211262e-06` |
| RMS component error | `3.63588333e-06` |
| Maximum `sin² + cos² - 1` error | `7.37137005e-07` |
| Maximum Park component error | `7.27176666e-06` |
| Maximum inverse-Park component error | `2.98023224e-06` |
| Maximum SVPWM duty propagation difference | `2.56299973e-06` |
| Sector mismatches over the scan | `0` |

The measured maximum is higher than the Legacy header comment's approximate
`3.5e-6` statement. That comment is therefore not accepted as the numerical
baseline. The current V2 implementation was not replaced with this function.

The boundary scan shows the expected approximation error near zero and the
quadrant transitions; the wrap checks return `0`, `0`, `0`, and `0.5` for
`-2π`, `0`, `2π`, and `4π+0.5`.

## Host timing baseline

The uninstrumented Release executable uses a fixed 256-input dataset, 15
recorded batches after one warm-up batch, and 200,000 current-loop calls per
batch. The benchmark carries a volatile checksum through the result so the
compiler cannot remove the work. Windows QueryPerformanceCounter is used for
the elapsed time.

`build/perf01a/benchmark.csv` SHA-256:

```text
C10CCA43A7736630A14CFD4400947B5BF280F7F679A28B6D41BFF6F68190E510
```

| Path | Samples | Minimum ns/step | Median ns/step | Maximum ns/step |
| --- | ---: | ---: | ---: | ---: |
| Normal | 15 | 140.2025 | 141.2125 | 153.6465 |
| Saturated / correction candidate | 15 | 257.2410 | 257.9015 | 260.3480 |

These numbers are host-only timing, not AT32F415 cycles and not WCET.
Different compiler, OS scheduling, CPU frequency, and standard-library
implementations will change them.

## AT32F415 static target evidence

The existing V2 target was checked with:

```text
cmake --build build/rc0/baseline-arm --parallel 4
arm-none-eabi-size -A build/rc0/baseline-arm/falconfoc-v2-at32f415.elf
arm-none-eabi-nm -S --size-sort ...
arm-none-eabi-objdump -d -S ...
```

The build was already up to date and returned exit code 0. The current binary
SHA-256 remains:

```text
B2743F8FE788B3EA923FD094C4AE40AE3C93326C11E76B35BCBD1F680E763EB7
```

Selected static results:

| Item | Result |
| --- | ---: |
| `.text` | 31,976 B |
| `.rodata` | 1,324 B |
| `.data` | 84 B |
| `.bss` | 2,268 B |
| `foc_current_step` stack frame | 144 B |
| `foc_current_modulate` stack frame | 56 B |
| `foc_svpwm` stack frame | 56 B |
| `foc_park` / `foc_inverse_park` stack frame | 32 B each |
| Undefined symbols | 0 |

The target contains software floating-point helper and libm symbols including
`__aeabi_fadd`, `__aeabi_fsub`, `__aeabi_fmul`, `__aeabi_fdiv`, `sinf`,
`cosf`, `atan2f`, `fmodf`, `sqrtf`, and `hypotf`. This proves software-float
dependency and symbol/code presence; it does not prove per-call cycles.

Full size, symbol, ELF-header, and disassembly outputs are stored in
`build/perf01a/arm-size-A.txt`, `build/perf01a/arm-symbols-hot.txt`,
`build/perf01a/arm-elf-header.txt`, and `build/perf01a/arm-disassembly.txt`.

## Verification

Fresh checks for this round:

| Check | Result |
| --- | --- |
| Core CTest | 7/7 PASS |
| Runtime CTest | 6/6 PASS |
| G3-NP CTest | 2/2 PASS |
| G3-SAFE CTest | 1/1 PASS |
| Board/UART/Python CTest | 3/3 PASS |
| Legacy Host | 45/45 PASS |
| FalconFOC source contract | PASS, 46 C/H files |
| New baseline clang-format | PASS |
| ARM V2 build | PASS, already up to date |
| Physical actions | None |

## PERF-01B candidates and risk boundary

The evidence supports the following candidates for the next separately
authorized stage:

1. Cache one angle reduction and one sin/cos pair for the current cycle only
   while the Park and inverse-Park angles are equal.
2. Remove `atan2f` from the duty-generation fast path while preserving the
   public sector field through a separately validated diagnostic path.
3. Move immutable PWM configuration validation out of the trusted hot path,
   while retaining dynamic VBUS, finite-input, timing, duty and final safety
   checks.
4. Preserve the applied-voltage reconstruction and anti-windup second pass
   until a state/output equivalence test proves any replacement correct.

No PERF-01B optimization was implemented here. No Q31 code, numeric generic
framework, API change, Runtime change, Safety change, Board change, or
Protocol change was made.

## Unverified target metrics

The following remain unverified and must not be inferred from this report:

- AT32F415 cycle counts or WCET for normal/saturated H0;
- ADC interrupt, SPI encoder, USART IRQ, Runtime and Safety arbitration cost;
- stack high-water mark;
- PWM update latency or physical output timing;
- equivalence of a future fast-math implementation on the real motor;
- any Q31 performance or precision advantage.
