# Themis Components Layer

## Purpose

`components/` owns reusable building blocks that should stay narrower and more
portable than `subsys/`, `device/`, `drivers/`, `bsp/`, and `applications/`.

The layer is intended for:

- protocol, parsing, control, canvas, and utility logic
- caller-owned data flow and lifecycle
- code that can be verified with host-side tests whenever dependencies allow

The layer must not quietly become a second framework layer.

## Purity Contract

Components should follow these rules by default:

- no direct `bsp/`, `device/`, or `drivers/` includes
- no RTOS API ownership in public headers
- no automatic boot/export registration owned by the component itself
- no hidden product policy in the public API surface
- explicit CMake dependencies that match the headers consumers really see

## Current Exceptions

Some components still carry justified framework-facing contracts:

- `components/algorithm/pid` publicly exposes `core_time.h`
- `components/algorithm/*` publicly exposes `arm_math.h`
- `components/algorithm/filter/kalman_filter.c` still allocates from the
  Themis runtime heap inside the implementation

These exceptions are allowed only when the dependency is part of the current
public contract or when removing it would be a behavior-changing refactor.

## 2026-04 Audit Outcome

The first-round closeout focused on safe, low-risk changes:

- `vofa` no longer includes `tm_runtime.h` in its public header
- `vofa` no longer owns `SERVICE_EXPORT(vofa_init)`
- `protocol` is host-buildable, so `test_vofa` now runs in the normal host gate
- `kalman_filter.h` no longer leaks `tm_runtime.h`; allocator ownership stays
  inside the `.c` implementation for now

The second-round closeout removed the last obvious product-policy sink from the
component layer:

- `vofa.c` no longer compiles alternate behavior behind `TM_APP_FEATURE_VOFA`
- firmware no longer whole-archives `protocol`; higher layers now own whether
  VOFA code is pulled into the final image
- host-side `test_vofa` no longer needs `subsys/export` or `subsys/runtime`
  include paths just to compile the component

The next round can revisit deeper topics such as allocator injection for
Kalman and further algorithm host coverage, but `components/` should now be
treated as boundary-stable unless a new reusable module is introduced.

The newest reusable module follows that same boundary rule:

- `components/protocol/` is now VOFA-only for FalconFoc live runtime.
- `components/utils/atomic`, `components/utils/bitmap`, and
  `components/utils/mpsc_queue` now provide reusable low-level primitives
  without pulling RTOS ownership into the component layer.
