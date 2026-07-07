# FalconFoc Maintenance Log

## 2026-06-18

### Fix voltage open-loop shell path colliding with current loop

- Fixed the long-standing bug where `diag set vd/vq` in `RUNNING` state only
  wrote `vd_set/vq_set` but the 20 kHz ADC ISR (`foc_current_loop_step`)
  ignored `CTRL_MODE_VOLT` and kept running the current PID, effectively
  locking the rotor.
- Added explicit voltage-mode branch in
  [`applications/periph/foc_ctrl.c`](../../applications/periph/foc_ctrl.c):
  - integrates a simulated electrical angle `theta_sim` at `we_volt_ramp`
  - calls `foc_ctrl_volt()` directly, bypassing the current loop
- Added `theta_sim` and `we_volt_ramp` to `struct motor_ctrl_state` in
  [`applications/algo/foc_common.h`](../../applications/algo/foc_common.h).
- Extended `foc_cmd_voltage_set()` and `motor_runtime_diag_set_voltage()` to
  accept an optional electrical angular velocity `we_ramp` (rad/s).
- Extended shell syntax to `diag set vd <V> [<we>]` / `diag set vq <V> [<we>]`:
  - omit `<we>` for alignment / lock-on-angle test
  - supply `<we>` for continuous spin test to verify the motor can rotate
- Initialized / cleared the new state in `_motor_clear_setpoints()` so IDLE and
  FAULT entry always reset the voltage open-loop simulator.
- Updated host-test stubs and contract tests to match the new signatures.
- Updated [`docs/application/Themis_FalconFoc新版Shell使用手册.md`](../application/Themis_FalconFoc%E6%96%B0%E7%89%88Shell%E4%BD%BF%E7%94%A8%E6%89%8B%E5%86%8C.md)
  to document the new optional `<we>` argument.

## 2026-06-03

### docs-wide BSP naming normalization

- Normalized the live docs entry points so they now describe one consistent BSP
  naming contract:
  - directory structure remains `bsp/board + bsp/ports`
  - module naming remains `bsp_xxx`
  - retired `*_port` module naming is no longer used as live guidance
- Rewrote the two docs index owners to the current live read path:
  - `docs/README.md`
  - `docs/architecture/README.md`
- Aligned the long-lived architecture wording with the same truth:
  - `control-plane port owner` -> `control-plane module owner`
  - `flash port owner` -> `flash BSP module owner`
  - `board / port owner` -> `board / BSP module owner`
- Kept the real shell startup split unchanged:
  - `shell_task` remains transport owner
  - `app_shell_task` remains session owner
  - only the stale BSP naming language was removed
  - no code-path semantics were changed in this docs round

### shell output contract second closeout

- Unified the live FalconFoc shell vocabulary around one outward-facing
  contract:
  - summary lines keep `cmd=<subcommand> result=<ok|error> ...`
  - guidance is limited to `hint` / `note` rows
- Normalized the active shell command families to the same dialect:
  - `state show` now keeps `cmd=show` across success and failure paths
  - `obs` scalar queries now use `cmd=` + `value=`
  - runtime guards now report `reason=idle_only`
  - mode redirects now report `reason=redirect target=cfg`
  - `cal enc` start now reports `cmd=enc action=start`
- Synchronized the surrounding live surfaces to the same truth:
  - banner examples now use `build_date=` / `build_time=`
  - console truth now uses `uart=USART1 baud=1000000 transport=dma`
  - help rows now use `group=` + `usage='...'`
  - `sys status` now documents `shell=on`, `queue_used`, `queue_depth`,
    `submit_ok_count`, and `submit_fail_count`

### `store save` shell size-evidence closeout

- Extended the FalconFoc `store save <slot>` shell output so it no longer stops
  at `result=ok` / `status=pending` only.
- The save path now reports both:
  - `data=<N>B`
  - `flash=<N>B`
- The save summary line now also reports the aggregated deferred-save totals:
  - `total_data=<N>B`
  - `total_flash=<N>B`
- Kept the existing runtime semantics unchanged:
  - `store save ...` still queues a deferred request only
  - actual Flash mutation still stays in the safe task path
- Kept ownership aligned with the live FalconFoc layering:
  - `motor_store.*` owns per-slot save-size truth
  - `motor_runtime.*` re-exports that read-only fact to shell consumers
  - `app_shell_cmd_store.c` only formats and emits the evidence

### shell final conservative closeout

- Closed the last live shell-output drift without expanding the command surface:
  - `diag pid` now keeps one stable action dialect:
    - `action=show`
    - `action=reset`
    - `action=set`
  - `diag param` now keeps one stable action dialect:
    - `action=show`
    - `action=set`
- Kept the helper layer intentionally thin:
  - `applications/shell/app_shell_output_helpers.*` now owns shared scalar
    summary and common `snapshot_unavailable` formatting
  - command owners still keep runtime semantics, guards, and reason selection
- Froze the final live board-witness path to one stable shell baseline:
  - `help`
  - `sys status`
  - `state show`
  - `obs all`
  - `diag pid all show`
  - `diag param show`
  - `store save sys`
  - `store status`

## 2026-06-02

### `bsp_console` transport-module contract closeout

- Reworked `bsp/ports/bsp_console.h` into a formal thin transport-module
  contract while keeping the current six-function northbound shape:
  - RX bring-up and TX action helpers now return `0 / -errno`
  - query helpers keep their primitive `uint16_t / bool / void` contract
  - ownership, lifecycle, and context notes now explain the split between
    thread-mode transport actions and IRQ-safe query helpers
- Reworked `bsp/ports/bsp_console.c` so the shared USART1 transport path now
  matches the current FalconFoc BSP rules:
  - vendor/CMSIS and DMA truth stay southbound in `.c`
  - action helpers validate arguments and reject ISR callers with `-EPERM`
  - `tm_runtime.h` now owns the cross-context check instead of open-coded
    assumptions in callers
- Added host regression coverage so `bsp_console` now acts as the formal
  transport-style BSP sample beside `bsp_flash` and `bsp_system`:
  - `tests/host/bsp/test_bsp_console_contract.c`
  - `tests/CMakeLists.txt`

### `bsp_system` second BSP sample contract closeout

- Reworked `bsp/ports/bsp_system.h` into a formal thin-module northbound
  contract:
  - public API is now `int bsp_system_reset(void)`
  - success/failure is unified to `0 / -errno`
  - ownership, lifecycle, context, and recovery notes now match the current
    FalconFoc BSP writing style
- Reworked `bsp/ports/bsp_system.c` so the reset path now follows the same
  light-BSP rules already frozen by `bsp_flash`:
  - vendor/CMSIS reset truth stays southbound in `.c`
  - the port consumes `tm_runtime.h` instead of open-coding raw context policy
  - ISR callers are rejected with `-EPERM`, while thread-mode callers remain
    allowed before or after scheduler start
- Added host and contract coverage so `bsp_system` now acts as the second
  copyable BSP sample for future `bsp/ports/bsp_xxx.*` work:
  - `tests/host/bsp/test_bsp_system_contract.c`
  - `tests/script/test_falconfoc_themis_lite_contract.ps1`

### `bsp_flash` board-backend contract closeout

- Rewrote `bsp/board/bsp_flash.h` as a minimal northbound contract:
  - kept only Flash geometry constants plus `read/write/erase`
  - removed BSP-private public error codes
  - removed public `get_sector` / `is_empty` utility exposure
- Reworked `bsp/board/bsp_flash.c` so Flash mutation now follows the formal
  FalconFoc BSP policy:
  - vendor Flash driver include stays southbound in `.c`
  - write/erase are task-context only and return negative `-errno`
  - critical sections now use `tm_irq_critical_enter()` /
    `tm_irq_critical_exit()` instead of raw IRQ intrinsics
- Updated host-side contract coverage so the fake Flash backend now matches the
  same `0 / -errno` result convention:
  - `tests/host/support/bsp_flash.h`
  - `tests/host/applications/test_foc_param_restore_contract.c`
- Tightened the Themis-lite contract gate so future regressions are rejected if
  `bsp_flash`:
  - leaks `THEMIS_PLATFORM_FLASH_DRIVER_HEADER` back into the public header
  - revives private `BSP_FLASH_ERR_*` public codes
  - revives public `bsp_flash_get_sector()` / `bsp_flash_is_empty()`
  - falls back to raw `__disable_irq()` / `__enable_irq()`

### FalconFoc CMake first-wave owner / entry / gate refactor

- Added the shared platform/CPU-class metadata helper:
  - `cmake/ThemisPlatformMetadata.cmake`
- Root `CMakeLists.txt` and the ARM GCC toolchain entry now share one metadata
  loader instead of maintaining duplicated path/default-resolution helpers.
- Normalized prepared inactive platform manifest style:
  - `platform/at/at32f423/CMakeLists.txt`
  - removed broad `GLOBS`
  - switched to explicit `SOURCES`
- Added the preset-first CMake smoke gate:
  - `scripts/gates/check_cmake_smoke.ps1`
  - lane coverage:
    - `host-debug`
    - live `fw-full-diag-carrier-debug`
    - prepared inactive `at32f415`
    - prepared inactive `at32f423`
- Tightened build-contract gates and tests so they now require:
  - shared metadata helper ownership instead of duplicated root/toolchain helpers
  - explicit prepared-platform manifest style
  - `fw-base` / `host-base` preset ownership to stay stable
  - firmware CLI defaults to stay aligned with the live baseline:
    - `THEMIS_PLATFORM_VENDOR=at`
    - `THEMIS_PLATFORM_SOC=at32f403a`
    - `THEMIS_FIRMWARE_TARGET_NAME=falcon-foc-at32`
- Updated live docs to freeze the new formal stance:
  - manual / CI configure verification is preset-first
  - `run.sh` remains a parameterized firmware CLI wrapper, not an architecture owner
  - raw `cmake -S/-B -DCMAKE_TOOLCHAIN_FILE=...` is expert/debug-only

### FalconFoc CMake policy and new-product landing guide formalization

- Added the formal FalconFoc CMake/build strategy and new-product landing guide under:
  - `docs/architecture/Themis_FalconFoc_CMake构建策略与新产品落地规范.md`
- Froze the current repository stance as:
  - new real products should fork into new repositories by default
  - same-product chip migration and prepared inactive SoC work may stay in the current repository
  - `work/` remains the graphical-project parent source
  - `platform/<vendor>/<soc>/generated` remains the live repository build input
  - CPU-class metadata remains the formal owner of:
    - `mcpu`
    - `mfpu`
    - `mfloat-abi`
    - FreeRTOS `portable/GCC/*`
    - `ARM_MATH_*`
- Updated the live architecture surface so the new document becomes part of the current read path:
  - `docs/architecture/README.md`
  - `docs/README.md`
- Updated the existing CMake architecture note so it now explicitly points to the new long-lived workflow guide:
  - `docs/architecture/Themis_CMake构建架构说明.md`

### FalconFoc small-MCU BSP policy and writing guide formalization

- Added the formal FalconFoc BSP lightweight strategy and writing guide under:
  - `docs/architecture/Themis_FalconFoc_BSP轻量化策略与编写规范.md`
- Froze the current small-MCU BSP stance as:
  - FalconFoc does not revive full `device/` / `drivers/` for control-plane peripherals
  - FOC hot paths remain direct-to-vendor / direct-to-register where latency matters
  - soft-task peripherals are collected behind thin `bsp/board + bsp/ports` contracts
  - BSP public APIs default to direct function surfaces rather than public function-pointer tables
  - `platform` stays vendor / SoC oriented, while `bsp` stays board / function oriented
- Locked the current migration guidance for future chip swaps to:
  - keep `bsp_xxx.h` northbound contracts as stable as practical
  - rewrite affected `bsp_xxx.c`, `bsp/board`, and `platform/<vendor>/<soc>` owners as needed
  - avoid mirroring `at/stm/gd/apm` taxonomy under `bsp/` by default
- Updated the live docs index so the new BSP guide is part of the current architecture surface:
  - `docs/architecture/README.md`
  - `docs/README.md`

### FalconFoc cross CPU-class / platform migration closure

- Added the generic CPU-class metadata owner under:
  - `platform/cpu/cortex-m4f-hard/ThemisCpuClassDefaults.cmake`
  - `platform/cpu/cortex-m3-soft/ThemisCpuClassDefaults.cmake`
- Root CMake, the ARM GCC toolchain entry, and `kernel/freertos` now consume
  CPU-class metadata instead of writing live CPU/FPU/FreeRTOS truths inline.
- The active live platform metadata now binds:
  - `platform/at/at32f403a/ThemisPlatformDefaults.cmake`
  - `THEMIS_PLATFORM_CPU_CLASS_DEFAULT=cortex-m4f-hard`
- `ARM_MATH_CM4` moved out of the active platform compile-definition defaults
  and into the CPU-class contract.

### Prepared inactive platform intake

- Imported prepared inactive real platform trees:
  - `platform/at/at32f415`
  - `platform/at/at32f423`
- Added FalconFoc-owned platform metadata for both:
  - `platform/at/at32f415/ThemisPlatformDefaults.cmake`
  - `platform/at/at32f423/ThemisPlatformDefaults.cmake`
- Added the platform timing seam required by current FalconFoc app-layer
  contracts:
  - `platform/at/at32f415/generated/inc/tm_platform_foc_timing.h`
  - `platform/at/at32f423/generated/inc/tm_platform_foc_timing.h`
- Current repository truth is now frozen as:
  - live baseline = `AT32F403A + cortex-m4f-hard`
  - prepared inactive = `AT32F415 + cortex-m3-soft`
  - prepared inactive = `AT32F423 + cortex-m4f-hard`

### Gate and doc alignment

- Updated the platform backend gate and Themis-lite contract gate so they now
  require:
  - `THEMIS_PLATFORM_CPU_CLASS_DEFAULT`
  - valid CPU-class defaults
  - no more hardcoded `ARM_CM4F` in `kernel/freertos`
  - no more live CPU-class literals in the generic ARM GCC toolchain entry
- Updated long-lived architecture docs so future chip migration is described as
  four independent owner decisions:
  - CPU class
  - platform / SoC
  - board
  - motor / product

## 2026-06-01

### FalconFoc platform / SoC migration guide formalization

- Added the formal long-lived platform and SoC migration guide under:
  - `docs/architecture/Themis_FalconFoc平台换芯与SoC迁移指南.md`
- Froze the current migration stance as:
  - `AT32F403A` remains the live baseline
  - `AT32F415` and `AT32F423` are handled as platform/SoC migrations first,
    not application rewrites
  - chip replacement should stay concentrated in:
    - `platform/at/<soc>`
    - `bsp/board`
    - `bsp/ports`
    - necessary product timing/config facts
  - shared layers must not absorb FalconFoc-only chip policy
- Recorded the current real coupling points that must be considered during
  future chip swaps:
  - root `CMakeLists.txt` default SoC/startup/linker/part-macro bindings
  - direct `AT32F403A` headers in current BSP thin layers
  - `applications/algo/foc_config.h` still carrying `240 MHz` SoC timing facts
  - gate scripts that still intentionally assume `at32f403a` as the sole live
    platform
- Added the future optimization direction for smoother cross-chip reuse:
  - move SoC timing facts out of app product config where possible
  - let each platform own its startup/linker/compile-definition metadata more
    completely
  - evolve gates from "single known SoC only" toward "one live SoC plus
    prepared inactive ports" when that becomes a real repository goal

### FalconFoc platform timing seam closure

- Added the live platform timing header:
  - `platform/at/at32f403a/generated/inc/tm_platform_foc_timing.h`
- `applications/algo/foc_config.h` no longer owns the live `AT32F403A`
  timer/PWM/loop cadence truth directly.
- The current owner split is now:
  - product defaults and formulas stay in `applications/algo/foc_config.h`
  - SoC timing facts stay in the active platform directory
- Host-side contract tests now require:
  - the active platform timing header to exist
  - `foc_config.h` to consume the platform timing seam
  - app-layer config to stay free of direct `240 MHz` timing literals

### FalconFoc platform metadata owner closure

- Added the active platform metadata file:
  - `platform/at/at32f403a/ThemisPlatformDefaults.cmake`
- Root `CMakeLists.txt` no longer owns:
  - `startup_at32f403a_407.s`
  - `AT32F403AxG_FLASH.ld`
  - `AT32F403ACGT7`
- These defaults are now formally owned by the active platform directory and
  only assembled by root CMake.
- The platform backend contract now allows prepared real platform directories to
  exist, as long as they:
  - do not become the live baseline implicitly
  - do not push chip policy back into shared layers
  - carry their own `ThemisPlatformDefaults.cmake`
- Updated the CMake architecture doc and the platform migration guide to the new
  owner split:
  - root chooses `vendor/soc`
  - platform defaults own startup/linker/part metadata
  - explicit overrides remain for exceptional or temporary bring-up paths

## 2026-05-14

### FalconFoc AT32F403A Themis-lite platform/build alignment

- This heading is kept as the current live maintenance-log anchor for contract
  tests and tooling.
- Current witness tokens:
  - debugger=`J-Link`
  - serial=`COM54`
  - flash=`./run.sh flash`
  - persistence=`store save sys`

### FalconFoc-only repository purification

- Removed retired and non-FalconFoc live documents from `docs/architecture/`,
  `docs/governance/`, and `docs/superpowers/`.
- Retained only the FalconFoc live document surface:
  - architecture decision and BSP boundary
  - startup chain and boot checklist
  - shell transport policy and shell output policy
  - FalconFoc shell usage manual
  - current overview and repo entry guides
- Removed retired governance prompts and stale cross-repo references.

### FalconFoc live boundary freeze

- Confirmed the active platform owner remains `platform/at/at32f403a`.
- Confirmed the active board owner remains `bsp/board`.
- Confirmed the active thin control-plane module owner remains `bsp/ports`.
- Confirmed FalconFoc does not assemble Themis `device/` or `drivers/`
  runtime layers.
- Confirmed the FOC hot path keeps direct hardware/library access where
  latency matters.

### Startup and parameter contract alignment

- Re-validated the live staged boot chain:
  - `platform/at/at32f403a/generated/src/main.c`
  - `applications/app_main.c`
  - `bsp/board/bsp_init.c`
  - export bring-up
  - `motor_task_init()`
  - `shell_task_init()`
  - `app_shell_task`
  - `vofa_task_init()`
- Re-validated FalconFoc parameter persistence as the live app-owned path:
  - owner: `applications/algo/foc_param.*`
  - backend: `tiny_param + bsp_flash`
  - save trigger: task context only
  - semantics: blank-is-normal, version-recoverable, per-group CRC isolation,
    no flash writes in ISR

### Serial and board-debug witness

- Current live serial witness:
  - debugger: `J-Link`
  - console witness port: `COM54`
  - shell probe baud: `1000000`
  - policy: `USART1 console-first`
- Current live flash/debug entry:
  - `./run.sh flash`
- Current documented persistence witness:
  - `store save sys`
  - reboot
  - `store status`

### Tooling cleanup

- Updated live smoke lane defaults to the dedicated FalconFoc OpenOCD config:
  - `tools/openocd/openocd_jlink.cfg`
- Removed dead host tests and stale script/doc references that belonged to
  retired surfaces.

### Default shell profile alignment

- Changed the default firmware profile owner from `standard` to `full-diag`.
- Current no-argument build/flash lane now defaults to the `diag` shell image:
  - `CMakeLists.txt`
  - `run.sh` / `scripts/lib/themis_firmware_cli.sh`
  - `include/config/tm_core_config.h`
- Added firmware CLI contract coverage so the default `./run.sh all` configure
  lane must emit `-DTHEMIS_DEVDRV_PROFILE=full-diag`.

### Next-phase roadmap freeze

- Added the formal FalconFoc next-phase execution roadmap under:
  - `docs/planning/FalconFoc_下一阶段正式执行路线图.md`
- Froze the recommended phase order as:
  - `P0` parameter persistence and recovery contract
  - `P1` application-layer ownership and product loop closure
  - `P2` thin BSP soft-peripheral expansion
  - `P3` algorithm-downstream review with minimal componentization

### P0 store-save contract closure

- Closed the first P0 application-layer drift in `applications/shell/app_shell_cmd_store.c`.
- `store save <motor|enc|sys|all>` now latches `param_save_req` only and no
  longer writes Flash directly from the shell command path.
- Real Flash save remains owned by `applications/tasks/motor_task.c`
  `_motor_handle_deferred_save()` in safe RTOS task context.
- Tightened runtime guard:
  - `store save ...` requires `axis=idle`
  - `store erase` requires `axis=idle`
  - `store load motor/sys` remains `axis=idle`
- Added host regression coverage:
  - `tests/host/applications/test_app_shell_store_contract.c`

### P0 restore-finalization contract closure

- Added the shared restore-finalization helper:
  - `applications/algo/foc_restore.*`
- Added the app-owned restore-after-load hook:
  - `motor_restore_after_param_load()`
- Startup restore and shell `store load motor/sys` now converge on the same
  application-layer post-restore path.
- That path refreshes derived runtime state and clears transient setpoints
  without overwriting persisted PID tuning.
- `foc_param_load_sys()` now validates persisted control/input enums and falls
  back to:
  - `CTRL_MODE_VOLT`
  - `INPUT_MODE_INACTIVE`
- Added host regression coverage:
  - `tests/host/applications/test_foc_param_restore_contract.c`

### P0 reboot-restore witness closure

- Real board witness completed on:
  - debugger=`J-Link`
  - serial=`COM54`
  - baud=`1000000`
- Fixed the last P0 restore drift:
  - shell/system restore path no longer clobbers persisted `input_mode`
  - initial FSM `IDLE` bootstrap now preserves restored `ctrl/input` policy
- Added regression coverage for:
  - restore-time transient setpoint clearing without mode clobber
  - initial `IDLE` enter preserving restored shell-visible mode policy
- Verified real-board restart path:
  - `cfg ctrl vel`
  - `cfg input traj`
  - `store save sys`
  - `sys reboot`
  - `cfg show`
  - `store status`
- Final board witness result:
  - reboot after `store save sys` restores `ctrl=vel`
  - reboot after `store save sys` restores `input=traj`
  - shell remains available with `ready=1`

## 2026-05-15

### P1 shell and FSM ownership closure

- Closed the duplicated control-mode write path in:
  - `applications/shell/app_shell_cmd_state.c`
- `state` is now event-only:
  - `start`
  - `stop`
  - `disable`
- `state mode ...` no longer mutates runtime control mode and now returns an
  explicit owner redirect:
  - `owner=cfg`
- Formal configuration ownership remains with:
  - `cfg ctrl ...`
  - `cfg input ...`

### P1 fault-clear contract closure

- Closed the FAULT exit drift in:
  - `applications/tasks/motor_fsm.c`
- `EV_CMD_CLR_FAULT` now clears the fault latch before returning the axis to
  `IDLE`.
- This aligns the live product behavior for:
  - `fault clear`
  - shell-visible `mask=0x00000000`
  - FSM-visible `FAULT -> IDLE`

### P1 host regression coverage

- Added host contract coverage for the P1 command/FSM boundary:
  - `tests/host/applications/test_app_shell_state_contract.c`
  - `tests/host/applications/test_motor_fsm_fault_clear_contract.c`
- Updated the host manifest:
  - `tests/CMakeLists.txt`
- Current witness:
  - `run_host_tests.ps1` passed with `16/16` tests green

### P1 diag / cfg / cal ownership closure

- Completed the second P1 owner audit for:
  - `applications/shell/app_shell_cmd_diag.c`
  - `applications/shell/app_shell_cmd_cfg.c`
  - `applications/shell/app_shell_cmd_cal.c`
  - `applications/shell/app_shell_cmd_fault.c`
- Frozen the live shell ownership split as:
  - `fault`
    - formal recovery surface through FSM only
  - `cfg`
    - formal config surface
    - remains `idle-only`
  - `cal`
    - calibration event surface
    - remains `idle-only`
    - command now reports `ok` only after the axis actually enters `CALIB`
  - `diag`
    - experimental surface
    - `set/ramp` remain runtime target-injection paths
    - `pid/param` write actions are now `idle-only`
    - `pid show` / `param show` remain readable in any axis state

### P1 new host regression coverage

- Added shell-owner regression tests:
  - `tests/host/applications/test_app_shell_diag_contract.c`
  - `tests/host/applications/test_app_shell_cfg_cal_contract.c`
- Added the minimal host shims required by the new shell coverage:
  - `tests/host/support/foc_ctrl.h`
  - `tests/host/support/foc_calib.h`
  - `tests/host/support/foc_common.h`

### P1 COM54 diag/cal board witness closure

- Re-validated the real-board interactive shell on:
  - debugger=`J-Link`
  - serial=`COM54`
  - baud=`1000000`
  - profile=`diag`
  - flavor=`carrier-demo`
- Captured the new board witness at:
  - `build/fw-full-diag-carrier-debug/output/board_witness/p1_diag_cal_boundary_witness_20260515_014913.log`
- Confirmed the live runtime boundary on hardware:
  - `state start` enters `axis=running`
  - `diag pid vel kp 0.1000` returns `policy=idle-only` while running
  - `diag param rs 0.2300` returns `policy=idle-only` while running
  - `state stop` returns the axis to `idle`
  - `cal enc` reports `result=ok` only when the axis actually enters `axis=calib`
  - `cal abort` restores the last committed encoder payload and returns to `IDLE`
- Closed the stale serial-probe drift in:
  - `scripts/find_themis_shell_port.ps1`
- The shell-port probe now matches the current live shell contract by checking:
  - boot banner when available
  - `help`
  - `sys status`
- The retired `ver`-signature assumption is no longer used for FalconFoc shell discovery.

### P1 diag/cal board flow formalization

- Added the formal FalconFoc board witness flow:
  - `scripts/flows/board/run_diag_carrier_witness.ps1`
  - wrapper: `scripts/run_diag_carrier_witness.ps1`
- This board flow is now the live owner for the COM54 `diag/cal` witness and no
  longer relies on ad-hoc terminal serial sessions.
- The new board flow explicitly captures and asserts:
  - `sys reboot`
  - post-reboot banner
  - `help`
  - `sys status`
  - `cfg ctrl vel`
  - `state start`
  - runtime rejection of `diag pid ...` writes
  - runtime rejection of `diag param ...` writes
  - `state stop`
  - `cal enc`
  - `cal status`
  - `cal abort`
- Updated live docs and script entry points:
  - `README.md`
  - `scripts/README.md`
- Locked the new script surface into the script contract:
  - `tests/script/test_firmware_cli_contract.ps1`
  - `scripts/gates/check_themis_quality_gate.ps1`
- Verified the new formal board flow on real hardware:
  - `scripts/flows/board/run_diag_carrier_witness.ps1 -PortName COM54 -SkipBuildAndFlash -SkipMemoryReport`
  - summary:
    - `build/fw-full-diag-carrier-debug/output/board_witness/diag_carrier_witness_20260515_021729.summary.md`
  - raw shell log:
    - `build/fw-full-diag-carrier-debug/output/board_witness/diag_carrier_witness_20260515_021729.log`
- Updated the host manifest:
  - `tests/CMakeLists.txt`
- Current witness:
  - `run_host_tests.ps1` passed with `18/18` tests green

## 2026-05-18

### FalconFoc 128K/32K shell two-state and memory slimming baseline

- Added the formal memory roadmap document:
  - `docs/architecture/Themis_FalconFoc_128K32K内存瘦身与Shell两态裁剪路线图.md`
- Froze the shell policy to compile-time two-state only:
  - `TM_SHELL_ENABLE=1`
  - `TM_SHELL_ENABLE=0`
- Removed the live `product / factory / diag` shell profile split from runtime policy.
- Shell-enabled images now expose one complete command surface; shell-disabled images compile shell owners out of the image.

### Shell transport/session memory tightening

- `applications/tasks/shell_task.c`
  - removed shell TX use of `mempool + spsc_queue`
  - switched to `1024 B ring_buffer` TX staging
  - overflow policy now drops new output and preserves transport continuity
- `applications/tasks/app_shell_task.c`
  - removed the `_tm_shell_task_backlog` shadow queue
  - kept one static message queue only
  - default queue depth is now `3`
- Updated host regression coverage and transport-shell gate to match the new single-queue contract.

### Feature-gate and linker-reserve convergence

- Added explicit feature gates in `tm_core_config.h` and CMake:
  - `TM_FEATURE_SHELL`
  - `TM_FEATURE_VOFA`
  - `TM_FEATURE_IPC`
  - `TM_FEATURE_TASK_BUDGET`
  - `TM_FEATURE_VERBOSE_LOG`
- Tightened linker reserve generation through the firmware CMake flow:
  - `shell off`: heap `0`, stack `0x400`
  - `shell on`: heap `0x200`, stack `0x400`
- Kept FreeRTOS heap unchanged at `10 * 1024 B` per current product constraint.

### Shell two-state live docs and witness contract closure

- Updated the live shell manual to the current two-state contract:
  - shell now documents only `TM_SHELL_ENABLE=1` and `TM_SHELL_ENABLE=0`
  - removed retired `product / factory / diag` runtime shell-profile wording
  - removed retired backlog wording from operator guidance
- Updated live transport and boot docs:
  - transport doc now records the single static queue contract
  - boot doc now records `line queue / command dispatch owner` instead of `queue / backlog`
  - architecture readme now links the formal 128K/32K memory roadmap
- Updated board witness and serial probe scripts to the current shell contract:
  - `scripts/find_themis_shell_port.ps1`
  - `scripts/flows/board/run_diag_carrier_witness.ps1`
  - `scripts/flows/board/run_tiny_carrier_regression.ps1`
- Serial probe and board witness no longer depend on retired `profile=diag` / `profile=tiny` shell signatures.
- The live shell witness contract now keys on:
  - `shell=on`
  - `flavor=carrier-demo`
  - `help`
  - `sys status`

## 2026-05-19

### FalconFoc shell / IPC build ownership return to Themis baseline

- Recovered FalconFoc build ownership back to the TEC-style baseline:
  - root `CMakeLists.txt` no longer owns FalconFoc-only `shell/ipc/vofa/task_budget/verbose_log` feature options
  - `subsys/CMakeLists.txt` again manifests:
    - `ipc/tm_ipc.c`
    - `runtime/tm_task_budget_probe.c`
    - `shell/tm_shell.c`
  - `applications/CMakeLists.txt` again manifests:
    - `shell/*.c`
    - `tasks/*.c`
- FalconFoc product feature defaults are now owned only by:
  - `include/config/tm_core_config.h`
  - current defaults:
    - `TM_FEATURE_IPC=0`
    - `TM_FEATURE_VOFA=0`
    - `TM_FEATURE_TASK_BUDGET=0`
    - `TM_FEATURE_VERBOSE_LOG=0`
    - `TM_FEATURE_SHELL=1` only for `full-diag`

### Shell / IPC slimming contract closure

- Kept the dual-owner shell contract unchanged:
  - `shell_task` = transport owner
  - `app_shell_task` = session owner
- `app_shell_task` stack budget is now owned by:
  - `TM_APP_TASK_STACK_SHELL_BYTES`
- IPC default image now relies on source-level self-pruning through:
  - `TM_IPC_ENABLE=0`
  - no default `tm_ipc_export_init`
  - no default slab / broker runtime
- Platform linker reserve is now fixed in the FalconFoc platform script:
  - `_Min_Heap_Size = 0`
  - `_Min_Stack_Size = 0x400`

### Decimal parser and image-contract verification closure

- Replaced the FalconFoc shell `strtof()` path with a narrowed decimal parser:
  - accepts signed decimal integers / decimals with optional surrounding spaces
  - rejects `1e3`, `nan`, `inf`, hex-float, and trailing garbage
- Added host regression coverage for the decimal parser contract:
  - `tests/host/applications/test_app_shell_common_contract.c`
- Tightened FalconFoc lite contract verification to check final firmware artifacts for:
  - shell-off ELF has no live `tm_shell` / `shell_task` / `app_shell_task` / `_shell_cmd_*`
  - default images have no IPC slab / broker / export runtime
  - shell-on image has no `_strtod_l` / `__gethex` / `_scanf_float`

## 2026-05-20

### FOC round helper closeout for calibration path

- Closed the encoder-calibration round drift at:
  - `applications/periph/foc_calib_enc.c`
- The pole-pair calculation now routes through:
  - `applications/algo/foc_math.h`
  - `foc_round_i32()`
- Product code comments now explicitly ban direct libc round-family calls on
  FalconFoc product paths.
- Added focused host helper coverage:
  - `tests/host/applications/test_foc_round_contract.c`
- This round keeps `kalman_filter` unchanged; any final-image presence is still
  recorded as the current `components/algorithm` default assembly result.

### FalconFoc shell RAM budget slimming at product owner layer

- Kept the live shell split unchanged:
  - `shell_task` = transport owner
  - `app_shell_task` = session owner
- Shrunk shell-on resident RAM through FalconFoc product-owned budgets only:
  - `TM_SHELL_TASK_STACK_BYTES = 192U * 4U`
  - `TM_APP_TASK_STACK_SHELL_BYTES = 384U * 4U`
  - `SHELL_TX_RING_SIZE = 768U`
  - `SHELL_RX_BUF_SIZE = 256U` unchanged
  - `TM_SHELL_TASK_QUEUE_DEPTH = 3U` unchanged
- `applications/tasks/shell_task.c` now takes the transport stack budget directly from `TM_SHELL_TASK_STACK_BYTES`.

### Product shell helper dedup without command-surface shrink

- Kept the full live command surface resident:
  - `sys`
  - `state`
  - `obs`
  - `fault`
  - `store`
  - `cfg`
  - `cal`
  - `diag`
- Added product-owned output helpers for repeated:
  - motion row
  - temp row
  - encoder row
- `state` and `obs` now reuse those helpers without changing command strings, help text, parameter syntax, or output field names.

### Verification snapshot

- Host tests:
  - `20/20` passed through `scripts/flows/host/run_host_tests.ps1`
- Firmware builds passed:
  - `fw-full-diag-carrier-debug`
  - `fw-standard-carrier-minsizerel`
  - `fw-tiny-carrier-minsizerel`
- Full-diag image checks:
  - `.shell_cmd = 0xc0` unchanged
  - `_shell_tx_ring_storage = 0x300` (`768 B`)
  - `_shell_rx_dma_buf = 0x100` (`256 B`)
  - `_tm_shell_task_queue_storage = 0x18c`
  - no `tm_ipc_export_init`
  - no `_strtod_l` / `__gethex` / `_scanf_float`
- Board witness on `2026-05-20`:
  - user requested `COM3`, but the machine's live shell probe resolved to `COM54`
  - formal witness passed on `COM54`
  - `help`, `sys status`, and diag/cal boundary checks stayed green
  - additional long-output checks for `help` / `obs all` / `diag pid all show` / `diag param show` / `store status` did not observe `rc=-7`

## 2026-05-21

### FalconFoc app runtime thin-layer owner closure

- Added the FalconFoc product runtime thin layer under:
  - `applications/runtime/`
- Live owner split is now:
  - `applications/algo`
    - pure math / observers / planning kernels
  - `applications/periph`
    - hardware-coupled FOC engines
  - `applications/runtime`
    - product meaning / restore / save / fault policy / motion command / calibration glue
  - `applications/tasks`
    - cadence owner only
  - `applications/shell`
    - shell vocabulary and diagnostics only

### Store / restore owner migration completion

- Removed live `.c` owners from:
  - `applications/algo/foc_param.c`
  - `applications/algo/foc_restore.c`
- Kept header-level compatibility wrappers only:
  - `applications/algo/foc_param.h`
  - `applications/algo/foc_restore.h`
- Formal persistence / restore owners are now:
  - `applications/runtime/motor_store.*`
  - `applications/runtime/foc_restore.*`

### Runtime seam closure for motion / calibration / fault

- Moved product motion-command owner to:
  - `applications/runtime/foc_motion_runtime.*`
- Moved calibration-derived runtime refresh and save/abort glue to:
  - `applications/runtime/motor_calibration_runtime.*`
- Moved software fault policy to:
  - `applications/runtime/motor_fault_runtime.*`
- `applications/periph/foc_ctrl.c` no longer remains the live owner of:
  - `foc_process_input_task`
  - `foc_cmd_*`
  - `foc_fault_check`

### Verification snapshot

- Host tests passed:
  - `scripts/flows/host/run_host_tests.ps1`
- Firmware build passed:
  - `./run.sh build`
- FalconFoc-lite contract gate passed after runtime-owner alignment:
  - `tests/script/test_falconfoc_themis_lite_contract.ps1`
- Full quality gate still reports one out-of-scope shared-layer issue:
  - `kernel/freertos/CMakeLists.txt`
  - `cmake-format --check` failure
  - no FalconFoc-only shared-layer edit was added in this round

## 2026-05-22

### Periph owner cleanup after runtime closure

- Removed the transitional `#if 0` migration leftovers from:
  - `applications/periph/foc_ctrl.c`
  - `applications/periph/foc_calib.c`
- Kept the live owner map unchanged:
  - runtime-owned motion / fault / calibration-glue logic stays in `applications/runtime/*`
  - `applications/periph/*` now reads as hardware-coupled live code only
- Tightened the FalconFoc-lite contract gate so these files may not regress back
  to `#if 0` migration leftovers.

### Applications writing-spec uplift and shell seam follow-up

- Normalized application-owned headers and implementation file heads across:
  - `applications/algo`
  - `applications/config`
  - `applications/periph`
  - `applications/runtime`
  - `applications/shell`
  - `applications/tasks`
- Added an application-owned motor task budget config header:
  - `applications/config/motor_task_config.h`
- `motor_task.c` now consumes explicit product-side task budget macros instead
  of file-local stack-size literals.
- Shell command owners no longer depend on exported `_motor_ins` directly for
  product semantics:
  - shell reaches the live motor through `app_shell_current_motor()`
  - the runtime owner exports `motor_runtime_current()` /
    `motor_runtime_snapshot()`
- Updated shell-related host tests so the runtime seam is live in host lanes
  rather than only in firmware integration.
- Verification status for this round is recorded with the host/build checks run
  in the same turn.

## 2026-05-24

### FOC closeout enhancement: verification, shell write seam, and fault owner

- Restored the host verification base so shell/runtime contract targets build
  and run again after the runtime seam expansion.
- Unified host parameter-store shim types with the live runtime store contract
  so shell/store/runtime tests no longer depend on a parallel fake status type.
- `applications/shell/*` command owners now keep the formal boundary:
  - read through runtime snapshot/read helpers
  - write through runtime request helpers
- `applications/shell/app_shell_cmd_diag.c` no longer reads or writes product
  truth through a direct live motor pointer:
  - `param show` now uses runtime-owned read views
  - `pid show` now uses runtime-owned read views
  - `set/ramp/pid/param` writes now flow through runtime-owned request seams
- `applications/runtime/motor_fault_runtime.c` no longer writes `axis_state`.
  Fault policy now latches `fault_code` and disables PWM only.
- `applications/tasks/motor_task.c` remains the fault-to-FSM cadence owner and
  now avoids redundant `EV_FAULT_TRIP` dispatch once the axis already entered
  `FAULT`.
- Added host regression coverage for the new fault owner rule:
  - `tests/host/applications/test_motor_fault_runtime_contract.c`
- Verification evidence for this round is now closed with fresh runtime and
  board witnesses:
  - `scripts/flows/host/run_host_tests.ps1` passed
  - firmware builds passed for `fw-full-diag-carrier-debug`,
    `fw-standard-carrier-minsizerel`, and `fw-tiny-carrier-minsizerel`
  - `COM54` diag/cal witness passed via
    `run_diag_carrier_witness.ps1 -SkipBuildAndFlash -SkipMemoryReport`
  - witness summary captured at
    `build/fw-full-diag-carrier-debug/output/board_witness/diag_carrier_witness_20260524_232931.summary.md`
- Remaining residue after code closeout is environment-only:
  - automated `run.sh all` witness still depends on restoring live
    `J-Link` / OpenOCD detection
  - no additional FalconFoc-only shared-layer logic was introduced to work
    around that tooling issue

## 2026-06-02

### Cross CPU-class / platform migration closeout follow-up

- Closed the next real migration blocker after the CPU-class metadata landing:
  - `bsp/board` and `bsp/ports` no longer hardcode the live
    `AT32F403A` device/header names
- Board-visible MCU and on-chip Flash facts are now consumed from platform
  compile definitions:
  - `CMSIS_device_header`
  - `THEMIS_PLATFORM_FLASH_DRIVER_HEADER`
  - `THEMIS_PLATFORM_FLASH_BASE_ADDR`
  - `THEMIS_PLATFORM_FLASH_SIZE_BYTES`
  - `THEMIS_PLATFORM_FLASH_SECTOR_SIZE_BYTES`
  - `THEMIS_PLATFORM_FLASH_SECTOR_COUNT`
- The motor outer-loop soft-IRQ selection is now also platform-owned:
  - `THEMIS_PLATFORM_MOTOR_SOFTIRQ_HANDLER`
  - `THEMIS_PLATFORM_MOTOR_SOFTIRQ_INSTANCE`
- Updated the live and prepared platform metadata owners:
  - `platform/at/at32f403a/ThemisPlatformDefaults.cmake`
  - `platform/at/at32f415/ThemisPlatformDefaults.cmake`
  - `platform/at/at32f423/ThemisPlatformDefaults.cmake`
- Tightened migration gates so future prepared platforms cannot regress by:
  - reintroducing direct `at32f403a_407.h` / `at32f403a_407_flash.h`
    includes in BSP
  - omitting platform-owned Flash geometry facts

### CMake phase-2 root slimming closeout

- Continued the first-stage CMake closure with a second-stage `root+helper`
  split:
  - root `CMakeLists.txt` now keeps only public cache entry declaration,
    helper call order, layer assembly, and final firmware/host target assembly
- Added four single-responsibility internal helpers under `cmake/`:
  - `ThemisBuildAxes.cmake`
  - `ThemisFirmwareAssembly.cmake`
  - `ThemisContracts.cmake`
  - `ThemisStatusReport.cmake`
- Moved the following implementation details out of root:
  - build-axis validation
  - profile/flavor compile-definition derivation
  - firmware entry/startup/linker/platform final assembly
  - contract target creation
  - configure status output
- Tightened CMake quality enforcement so root/helper boundaries are checked by
  gate rather than documentation only.
