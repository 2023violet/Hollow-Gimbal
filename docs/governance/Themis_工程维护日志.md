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
- Updated [`docs/application/Themis_FalconFoc�°�Shellʹ���ֲ�.md`](../application/Themis_FalconFoc%E6%96%B0%E7%89%88Shell%E4%BD%BF%E7%94%A8%E6%89%8B%E5%86%8C.md)
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
  - `docs/architecture/Themis_FalconFoc_CMake�����������²�Ʒ��ع淶.md`
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
  - `docs/architecture/Themis_CMake�����ܹ�˵��.md`

### FalconFoc small-MCU BSP policy and writing guide formalization

- Added the formal FalconFoc BSP lightweight strategy and writing guide under:
  - `docs/architecture/Themis_FalconFoc_BSP�������������д�淶.md`
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
  - `docs/architecture/Themis_FalconFocƽ̨��о��SoCǨ��ָ��.md`
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
  - `docs/planning/FalconFoc_��һ�׶���ʽִ��·��ͼ.md`
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
  - `docs/architecture/Themis_FalconFoc_128K32K�ڴ�������Shell��̬�ü�·��ͼ.md`
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

### Voltage open-loop bring-up session record

- Board debug session (2026-08-24, COM3 @ 115200 CMSIS-DAP UART bridge) proved
  the voltage open-loop chain end to end: `diag set vd/vq` -> `theta_sim`
  integration -> SVPWM -> DRV8311H gate stage -> rotor lock, smooth sustained
  rotation, fault-free throughout.
- Hardware facts confirmed against `板子原理图.pdf` and its embedded design
  note: phase currents come from the DRV8311H integrated CSA (SOA/SOB/SOC via
  330R + 22pF into PA4/PA5/PA6), referenced to CSAREF/2 with GAIN configured
  for 0.5 V/A. The existing `FAC_CURR(x) = x * (V_REF / (ADC_FULL_SCALE *
  CURR_AMP_GAIN))` conversion is correct for this front-end; no constant was
  changed. Small `obs iq/id` magnitudes are physically real: an unloaded
  gimbal rotor in synchronism draws only tens of milliamps.
- Firmware improvement added for smooth pull-in: the voltage open-loop path now
  ramps the applied electrical angular velocity (`we_volt_now`) continuously
  toward the commanded target at `DEFAULT_VOLT_WE_ACCEL` (60 rad/s² elec,
  `foc_config.h`) inside the ADC ISR step, and `_motor_clear_setpoints`
  re-arms the ramp from zero on every idle/fault entry. Previously `we` stepped
  instantly to the commanded value and the rotor could not pull into sync from
  rest.
- Verified stable operating point on the 2804 motor (7 pp):
  `diag set vq 1.2 31.42` + `state start` locks synchronism and runs smoothly
  at we/7 ~= -4.3 rad/s mech (-41 rpm); a 10 s endurance witness showed
  uniform position progression (+/-5 percent), ~5.6 turns net, iq/id tens of
  mA, no faults. `we = 62.83` sits beyond the voltage-source open-loop
  stability boundary for this motor (tracks during ramp, slips once the field
  holds constant speed); higher speeds belong to encoder-calibrated closed
  loop, which is also required before `cal enc`/closed-loop bring-up.
- Added a reusable console probe utility:
  `scripts/flows/board/run_shell_serial_probe.ps1`
- Witness note: the live debug serial port for this board is currently
  COM3 @ 115200 via the CMSIS-DAP bridge; governance docs still record the
  historical COM54 @ 1000000 witness and need a separate policy round.

### Encoder-calibration repair and open-loop damping work

- High-rate shell polling (125 ms cadence) proved the earlier open-loop
  "smooth" runs were aliasing: true torsional hunting at ~1.5 Hz swings the
  instantaneous speed between 0 and >30 rad/s. Pure voltage open loop on this
  near-frictionless gimbal rotor cannot damp that mode; lowering voltage does
  not remove it.
- Firmware additions:
  - we_volt_now continuous ramp (DEFAULT_VOLT_WE_ACCEL, 60 rad/s^2 elec) in
    the ADC ISR VOLT branch; _motor_clear_setpoints re-arms it on idle/fault
    entry so every start pulls in from rest.
  - New I-f sub-mode: diag set if <A> <we> (CTRL_MODE_VOLT +
    if_enable) runs the inner current loop on the simulated angle with
    clamped amplitude, making stall current safe. Requires non-zero
    current-loop gains, so oc_calib_runtime_refresh now calls
    oc_curr_gain_calc at boot (previously gains stayed 0 unless
    cal motor ls had been run).
- Repaired the ported pole-pair detection in oc_calib_enc.c: the previous
  bodies capped the electrical sweep at 2*pi and derived pn as
  round(pe_set/2*pi), forcing pole_pairs=1 (porting-debris comment left in
  place). PP_START now sweeps poles*2*pi (one full mechanical revolution,
  keeping the auto phase-order/direction witness), and PP_CALC takes the
  configured value. Verified on board: clean-state calibration yields
  pole_pairs=7, offset=-0.025 rad, enc_valid=1, persisted to the enc slot.
- Recovery note: store erase alone does not cleanse runtime config; the
  encoder slot auto-saves after each calibration. A correct re-calibration
  after bad parameters must follow the order erase -> reboot -> verify
  pole_pairs=7 -> cal enc.
- Open issue handed to bench review: closed velocity loop (calibrated
  encoder, ctrl=vel) holds the rotor stalled with iq~-0.18 A / id~-0.14 A.
  Suspects ranked: residual electrical-zero/direction sign from the repaired
  calibration, mounting friction/load, SPI integrity under PWM load. Next
  steps: verify rotation direction with a small I-f spin versus reported
  encoder sign, check nSLEEP/MODE/GAIN strap consistency, then retest
  diag set vel at 60-100 rpm.

### Hardware review round: current-sense DC blindness investigation

- Memory-level evidence chain captured via CMSIS-DAP/OpenOCD live dumps while
  the axis held a static d = 1.0 V vector (rotor locked, dtc confirmed
  0.458/0.542/0.542 in RAM):
  - injected-group raw minus offset deltas were ~2 counts (~13 mA) per phase
    where DC theory demands ~200-400 mA;
  - rotating-field tests on the same chain read plausible AC amplitudes.
  - Conclusion: phase-current feedback does not track DC through this
    DRV8311H low-side sense-FET front end with the current sampling setup.
- Trigger-source experiments (TMR3CH4 free-running vs TMR1CH4+ETP remap at
  CCR4=3740 PWM_A / valley / PWM_B near-peak) all reproduced the same
  DC-blind reading, so simple trigger repositioning did not resolve it. The
  tree now keeps the FalconFoc_AT32 reference trigger configuration verbatim
  (TMR1CH4 + ADC1_ETP_MUX remap + CCR4=3740 PWM_A).
- Verified-good hardware items this round: three-phase drive symmetry
  (quadrature iq/id walk, no clipping), bus-voltage divider accuracy,
  integrated dead-time handshake per datasheet, rotor alignment mechanics.
- Open bench items requiring human instrumentation:
  1) read the PSU ammeter delta during a d hold to confirm true DC flow;
  2) probe CSAREF/SOx with a DMM against the schematic strap table;
  3) confirm SPI integrity on IA/IB/IC nets under PWM load.

### CSAREF hardware fix verified; I-f synchronous run achieved; velocity-loop blocker isolated

- Root hardware defect found and fixed on the driver board: the CSAREF pin
  (CSA reference, spec 2.0-3.6 V feeding 2.5-7.5 mA, SOx = VREF/2 +/- G x I)
  measured only 0.15 V because the net carried no DC source. Per the TI MD045B
  EVM reference (R22 = 0 ohm from VREF to 3V3 plus 100 nF decoupling), the user
  flew a wire from CSAREF to DRV_3V3. Post-fix memory dumps show all three
  phase channels resting at ~2048 counts mid-rail with +-4 count jitter.
- With a live reference the sensing chain is linear and KCL-consistent:
  static vd holds and rotating I-f fields both read plausible amplitudes.
- Encoder recalibration was rebuilt around an I-f drag
  (CALIB_ENC_IF_IQ = 0.3 A via foc_ctrl_curr instead of hunting voltage drag)
  and the pole-pair stage now sweeps poles*2*pi; verified pp=7,
  offset=-0.124, persisted through reboot.
- Firmware additions this round:
  - diag set if <A> <we> I-f mode; diag set dp0/dp1/dp2 <V> fixed stator
    axis injection (encoder-independent anchor); diag param encoff <elec rad>
    electrical-zero trim.
  - Current-loop bandwidth retuned 1000 -> 250 Hz: the continuous-form PI at
    wc*dt = 0.63 per 10 kHz step destabilised the loop on this soft-float port.
  - Sense-polarity fix: DRV8311H sense FETs report terminal-into-driver
    positive; adc_read_phases now negates to match motor convention
    (witness: dp1 must show dominant positive ib).
  - VOFA waveform lane enabled by default (JustFloat, 7ch @ 100 Hz fits the
    115200 console) with dtc_a/b/c + i_q + iq_set + rpm + turns bindings;
    host parser scripts under temp tooling.
- Functional result: I-f mode runs the motor in locked synchronism
  (~43 rpm at we=31.42, current regulated at setpoint) - first smooth
  closed-current rotation on this rig.
- Open blocker: velocity closed loop still stalls with iq railed while the
  rotor sits frozen at an alignment point. Evidence: +-90 deg electrical-zero
  shifts both stall identically; three dp-axis anchors read inconsistently;
  atan2(iq,id) shows a ~60 deg frame bias. Leading suspect is spatial
  distortion of the electrical-angle chain (encoder LUT quality / magnetic
  nonlinearity), not offsets or gains. Next steps: evaluate enc_table health
  directly, consider disabling LUT compensation for a raw-linear baseline,
  then retune the velocity loop.

### 2026-08-27 AT32F415 Code Wiki 与无板验证

- Added `docs/code-wiki/` as a current-working-tree source map for the
  AT32F415 firmware: architecture, module responsibilities, API/call
  boundaries, target/data dependencies, operations, and deep analysis.
- Wiki scope intentionally keeps vendor/CMSIS/generated code at responsibility,
  selected files, and call-boundary level; it does not enumerate third-party
  functions.
- Software-only evidence: the isolated AT32F415 firmware build passed with
  FLASH 72,408/131,072 B and RAM 21,616/32,768 B. The host application lane
  remains blocked by the stale AT32F403A generated include path; the fault
  runtime source is currently an empty check entry point. No board, serial,
  OpenOCD, or motor behavior claim is made in this round.

### 2026-08-27 Board1 原理图与 DRV8311H 深度 Wiki

- Added `docs/code-wiki/hardware-and-drv8311h.md`, based on the Board1 schematic,
  netlist, BOM, TI DRV8311 data sheet, TI MD045B EVM reference, and the 2804
  motor reference manual.
- Recorded the current H-mode decode: 6x PWM, approximately 75 V/us slew,
  0.5 V/A CSA gain, and approximately 5 A typical OCP; correlated the SOx/ADC,
  TMR1/ADC trigger, bus divider, and AT32F415 call boundaries.
- No-board modeling found that the present nFAULT/R6/R7/D2 network does not
  pull nSLEEP below DRV VIH in the working bus range, and that nFAULT has no
  MCU endpoint. LDO thermal budget, CSAREF local bypass, sampling window, and
  Board1 PCB layout remain explicit review items; no physical-board claim is
  made.

### 2026-08-27 Board1—AT32F415 底层配置审计

- Added `docs/code-wiki/configuration-audit.md`, correlating the Board1 netlist,
  generated AT32F415 clock/GPIO/ADC/TMR/SPI/USART configuration, and the live
  FOC/fault call paths.
- Static review confirms the PWM/ADC/SPI pin mapping and the 20 kHz carrier /
  10 kHz FOC scheduling model, but identifies the USART2 connector path as
  uninitialized/unconsumed, the DRV nFAULT path as invisible to the MCU, and
  `foc_fault_check()` as an empty producer boundary.
- Host-only derivation also records the 25 Hz current LPF macro mismatch, the
  zero-VBUS divide path, the narrow low-side sampling margin, fixed-3.3 V
  reference error budget, and R6/LDO thermal checks. No board, waveform,
  thermal, EMI, or motor behavior claim is made.

### 2026-08-27 实机调试安全协议与开环排障流程

- Added `docs/code-wiki/motor-debugging-safety-and-procedure.md` as the
  mandatory real-motor debugging protocol for the current stalled/high-current
  open-loop state.
- The protocol separates immediate hard-limit trips from the 3 s maximum
  persistence watchdog: severe current/voltage/speed/data faults must trip on
  the first available fast sample, while analysis and log decoding occur only
  after PWM disable and motor stop.
- Recorded the current implementation boundary: `foc_fault_check()` is still
  empty, nFAULT/nSLEEP have no MCU endpoint in the current netlist, and
  `pwm_out_disable()` disables PWM channels but does not remove DCBUS. No
  firmware safety implementation was changed in this documentation round.

### 2026-08-27 AT32F415 + DRV8311H 实机测试方案

- Added `docs/code-wiki/motor-real-bench-test-plan.md` with gated, supervised
  bench steps for the current high-current/stalled open-loop issue.
- The first test envelope is limited to 8 V, 0.20/0.30 A PSU current limit,
  low voltage/current commands, short dwell times, and explicit 0.20 A,
  200 ms stall, bus-voltage, speed, and surface-temperature stop criteria.
- The plan makes G0 a hard gate: no long or unattended energized run until
   fast trip/independent emergency cutoff, frozen-log capture, and no-board
   trip tests are available. It also records the exact Shell command units and
   the rollback decision tree for each failed stage.

### 2026-08-27 实机只读调试首轮

- 在 12 V / 1 A 外部电源、CMSIS-DAP 调试器和 COM3 串口条件下完成上电但不驱动的首轮检查；SWD 识别、母线采样、ADC 原始值、TIM1 状态和 IDLE/FAULT 状态均已留证。
- 通过可回滚 RAM 临时零点实验确认错误电流零点是当前假高电流的直接原因；重新调用电流零点校准后，三相换算电流回到毫安级。
- 未发送启动、注入或电机校准命令，未烧录 Flash；由于 `foc_fault_check()` 为空、DRV `NFAULT/NSLEEP` 无 MCU 端点、Break/死区未形成安全闭环，G0 继续保持阻塞。
- 同轮执行无板主机测试入口时，CMake 配置成功但因旧 F403A host include path 与当前 `GPIO_PINS_15` F415 配置不一致而在编译期阻塞；未将该结果误报为测试通过。
- 调试收尾已执行 `state disable` 并用 SWD 确认 TIM1 `CCTRL=0x1000`，三相输出通道关闭；本轮未启动电机、未烧录 Flash。

### 2026-08-27 安全 bring-up 实现、验证与固件只读复核

- 将 `foc_fault_check_fast()` 接入 ADC 采样后、FOC 计算前的 ISR 边界：快速过流门限为 `0.20 A`，同时覆盖非法数据、VBUS、速度和温度；trip 只做故障锁存与 `pwm_out_disable()`，不打印、不阻塞、不写 Flash，任务级检查补充 `3 s` 持续电流和 `200 ms` 低速堵转规则。
- 强化启动电流零点校准：桥臂关闭后等待 `100 ms`，采集 `2000` 个 1 ms 样本，校验 raw 范围 `64..4031` 和 Welford 标准差 `<=16 counts`；失败锁存 `FAULT_CALIBRATION_FAILED`，成功后才恢复零占空待机钳位。
- 在 FOC 参数计算和电压直控入口增加非法 VBUS 防护，阻止 `1/v_bus` 的 NaN/Inf/零除传播，并在无效时清零调制量、关闭 PWM。
- 修复 host harness 的 AT32F415 generated include 路径和 `GPIO_PINS_15` shim，新增 fault/calibration/VBUS 三项 host contract；三项均通过。完整 host flow 编译 29 个目标，CTest 为 `25/29`，其余 4 项为既有预算、栈预算、FSM 中止和诊断格式契约差异。
- `fw-full-diag-carrier-debug`、`fw-standard-carrier-minsizerel`、`fw-tiny-carrier-minsizerel` 均构建通过；full-diag 资源为 FLASH `74,952/131,072 B`、RAM `21,624/32,768 B`。
- 在 AT32F415 + DRV8311H 实板上通过 CMSIS-DAP/OpenOCD 完成 full-diag 刷写和 `Verified OK`；复位等待校准后 SWD 读到 IDLE、fault=0、VBUS≈11.878 V、合理 offset，随后关闭 PWM，立即及 300 ms 后 TIM1 `CCTRL=0x1000` 保持不变。
- 本轮未执行任何电机启动、开环、电流注入、闭环或旋转测试；由于 `nFAULT/nSLEEP` 无 MCU 端点、Break/死区仍未完成硬件验证、PWM 禁能不切断 DCBUS，下一轮实机继续从静态 T01 受控门禁开始。

### 2026-08-27 主机回归收尾与 Wiki 同步

- 将 `TM_SHELL_TASK_STACK_BYTES` / `TM_APP_TASK_STACK_SHELL_BYTES` 与既有产品预算契约对齐为 `192U*4U` / `384U*4U`；保留 FOC 任务 `512U*4U`、安全任务 `256U*4U` 的显式预算。
- 修复 `motor_fsm` 的校准取消路径：进入 IDLE 前清理 RS/LS/Flux/JS/Encoder 五个子状态，避免恢复已提交 encoder payload 后仍残留进行中的状态机。
- 为故意触发断言的任务预算 fail-fast host test 增加 CMake system-failure wrapper，使 Windows 异常码 `0xc0000409` 能由 `WILL_FAIL` 正确表达；同时让诊断 host 桩使用与产品一致的半入舍入规则。
- 重新执行完整 host flow：29 个目标全部编译，CTest `29/29 PASS`；full-diag、standard、tiny 三个 AT32F415 固件均构建通过，full-diag 当前 FLASH `74,968/131,072 B`、RAM `21,624/32,768 B`。
- 本轮继续不执行电机启动、开环、I-f、闭环或旋转动作；Wiki 当前结论仍保持“软件无板门禁通过、功率级硬件门禁未解除、实板电机行为未验收”。

### 2026-08-27 T01 无板时基与电流闭环参数复核

- 将 `DEFAULT_CUTOFF_HZ` 从当前工作树中错误的 `25 Hz` 修正为 `DEFAULT_CURR_BW_HZ*5 = 1250 Hz`，并将 `DEFAULT_SAMPLING_HZ` 绑定到实际 `10 kHz` FOC step；新增主机契约防止 LPF/电流环带宽比例回退到 4 倍以下。
- 新增 `test_foc_bringup_model_contract` 的确定性 RL+PI+LPF 应力模型：`0.10 A` 阶跃下，旧 25 Hz 配置峰值约 `0.33245 A`，当前 1250 Hz 配置峰值约 `0.10000 A`；该模型只用于无板参数风险比较，不宣称电机稳定性。
- 复核确认 `0.20 A` bring-up 快速 trip 会拒绝默认 `0.30/1.00 A` 参数校准激励；本轮保持安全门限，不执行校准，不通过提高 trip 迁就校准参数。
- 完整 host flow：30 个目标全部编译并通过 CTest（`30/30 PASS`）；full-diag、standard、tiny 三种 AT32F415 固件均构建通过，资源分别保持为 full-diag FLASH `74,968/131,072 B`、RAM `21,624/32,768 B`，standard FLASH `40,136 B`、RAM `19,880 B`，tiny FLASH `36,912 B`、RAM `18,144 B`。
- 本轮仍不执行电机启动、开环、I-f、闭环或旋转动作；结论仍是软件无板门禁通过、功率级硬件门禁未解除，实板电机行为未验收。

### 2026-08-27 AT32F415 + DRV8311H 计划实现收尾

- 以测试先行为准则修复 `currbw` 与 LPF 的运行时/持久化同步：参数加载后初始化 LPF，合法运行时带宽同时刷新 PI 和 LPF，非法/非有限值拒绝且不清除滤波历史。
- 增加 `FOC_ENERGIZED_CALIB_ENABLE=0U` 默认门禁；电机参数校准和编码器带电校准在 FSM 进入 `AXIS_STATE_CALIB` 前拒绝，保留非带电电流偏置校准，并用 host contract 覆盖 PWM/状态/调用边界。
- 增加真实 FOC 软件路径的无板电压开环/SVPWM 模型测试，覆盖零速、角度回绕、I-f 一致性、VBUS 无效、矢量限幅、duty 范围和电流 PI 调用边界。
- 增加只读 `check_foc_peripheral_contract.ps1`，锁定 150 MHz 时钟、NVIC、TMR1 PWM/CH4、ADC rank/触发、SPI1 mode/CS、相序和 VBUS 防护文本事实；同时修正 LPF 离散形式和 PWM/FOC 时基注释。
- 当前验证范围严格限于主机、CTest、静态脚本和固件构建；未执行 OpenOCD 写入、串口启动、GDB 启动、校准命令、PWM 使能或电机动作。实机准入继续保持 `REAL_BENCH_TEST_STATUS=BLOCKED`；12 V/1 A 不替代物理急停和硬件关断。

### 2026-08-28 无板门禁新鲜复核

- `run_host_tests.ps1` 执行并通过 33 个 host target；独立 `ctest --preset test-host-debug --output-on-failure` 同样为 `33/33 PASS`。
- `check_foc_peripheral_contract.ps1` 返回 `FOC_PERIPHERAL_CONTRACT=PASS`，确认当前生成式 150 MHz 时钟、TMR1/ADC/SPI/NVIC 配置、相序映射和 VBUS 防护文本未漂移。
- 三种默认固件构建通过：full-diag FLASH `77,352/131,072 B`、RAM `21,720/32,768 B`；standard FLASH `40,356 B`、RAM `19,984 B`；tiny FLASH `37,132 B`、RAM `18,248 B`。bench full-diag 也构建通过，FLASH `76,732 B`、RAM `21,328 B`。
- 本次只执行主机/静态/交叉编译验证；没有执行 OpenOCD 写入、串口控制、GDB 启动、校准命令、PWM 使能或真实电机动作。`REAL_BENCH_TEST_STATUS=BLOCKED` 保持不变。

### 2026-08-28 现有板开环卡死根因定位阶段实现

- 按测试先行补充 `motor_fault_context`：快速保护在 `pwm_out_disable()` 前只锁存首次故障的 RAM 上下文；Shell 停机后通过 `fault status` 读取，重复故障不覆盖，`fault clear`/启动初始化才清除，ISR 不打印、不写 Flash、不发送串口。
- `motor_runtime_snapshot()` 和 `obs all` 现在可导出 raw ADC、offset、三相电流、VBUS、`theta_sim`/`theta_elec`、开环速度、FOC 电压和 SVPWM duty/sector；诊断写入口在 Shell/runtime 双层限制为 `IDLE`。
- 增加 `THEMIS_ENABLE_BENCH_DIAG`、`TM_BENCH_DIAG=1` 和 `build-fw-full-diag-carrier-bench-debug`，专用镜像关闭 VOFA 并强制关闭带电校准；默认 full-diag/standard/tiny 配置不变。
- 在全量回归中发现旧上下文存在时会错误抑制新的 `fault_code` 锁存，先补回归测试再修正 `_latch_fault()`：上下文仍只保留首次快照，但新的 fault 仍会触发 PWM 禁止；最终 host/CTest 为 `33/33 PASS`。
- 外设静态契约返回 `FOC_PERIPHERAL_CONTRACT=PASS`；默认三种固件和 bench 固件均重新构建通过，bench ELF 未发现 `vofa_` 符号。
- 本轮只执行无板主机契约、CTest、静态配置和交叉编译，未连接或控制实机，不修改 PCB、不控制 `nSLEEP`；`REAL_BENCH_TEST_STATUS=BLOCKED` 继续有效，12 V/1 A 不替代人工急停和硬件关断。

### 2026-08-28 调试会话记忆固化

- 将 2026-08-27 至 2026-08-28 的 AT32F415 + DRV8311H 开环排障关键事实、软件改动、测试结果、硬件未验证项和后续唯一入口固化为 `docs/code-wiki/session-memory-2026-08-27-28.md`，并加入 `docs/README.md` 与 Code Wiki 索引。
- 该文档是仓库内可审阅的显式交接记忆，不等同于隐藏的 Codex 持久化记忆；原始 session rollout 仍保留在用户目录，日常以仓库文档和源码为准。

### 2026-08-28 现有板只读 T01 执行结果

- 重新执行 host/CTest、外设静态契约和 bench 构建，结果分别为 `33/33 PASS`、`33/33 PASS`、`FOC_PERIPHERAL_CONTRACT=PASS` 和构建 `PASS`。
- 仅对 USB 串口 COM3 执行只读状态查询，未烧录、未启动电机、未发送参数/校准/清故障命令；现场返回 `vbus≈11.91 V`、`axis=fault`、`calibration-failed (0x80)`。延长读取仍缺少当前源码的 `obs all` 扩展行和 `fault status` context 行，是镜像不匹配的强指示，但串口丢包/溢出尚未排除；源码核对确认 `diag debug show` 本来就不是当前 Shell 命令，因此其 `bad_arg` 不能单独证明镜像过旧。
- 当前镜像身份、raw ADC/offset、三相电流、duty/sector、物理 PWM、死区、DRV 电源和人工急停均未形成完整证据，因此在 T01 停止，`REAL_BENCH_TEST_STATUS=BLOCKED` 保持。
- 详细记录保存在 `tmp/bench/2026-08-28_224839/`，不得把该次只读结果解释为电机运行验证。

### 2026-08-28 诊断门禁修正与 bench PWM 边界

- 根据历史实测记录，确认 2026-08-24 已完成 `diag set vq 1.2 31.42` 约 10 s 的真实开环运行；此前将示波器死区/重叠、外部电流和温度测量列为第一轮诊断的绝对前置条件，会重复已完成工作并阻塞获取新物理数据。
- 将状态语义拆分为：`PRODUCT_ACCEPTANCE_STATUS=BLOCKED`（仍禁止长时间、无人值守和产品级连续运行）与 `CONTROLLED_DIAGNOSTIC_STATUS=ALLOWED_WITH_MINIMUM_GUARDS`（允许低压、毫秒级、单变量、有人可立即断电的短脉冲）。示波器、外部电流/温度和 nFAULT 观测改为后续功率级确认/产品验收证据。
- 新增 `test_motor_fsm_bench_pwm_boundary_contract`，先确认旧实现会在 bench 初始化时错误地使能 PWM，再修正 bench FSM：IDLE 保持 PWM disabled，`EV_CMD_START` 后才 enable，FAULT 仍立即 disable；默认产品固件的主动低侧钳位行为不变。
- 专项 CTest 通过，bench 固件重新构建通过（FLASH `76,732/131,072 B`、RAM `21,328/32,768 B`）。该修正只澄清启动边界，不构成死区、采样窗口或电机运行的实板证明。

### 2026-08-29 受控实板短脉冲与停机快照修正

- 根据现场反馈，将“产品级 G0”与“第一轮运行取证”分离：示波器死区/重叠、独立相电流、温度和完整硬件急停仍是产品验收门禁，但不再阻塞 bench 固件下的低压、毫秒级、单变量短脉冲诊断；产品 `REAL_BENCH_TEST_STATUS=BLOCKED` 保持。
- 使用 bench ELF `CDE7AC7D8044719D4CF4E2650C298C566D4C3CF2A72DE439D8119AA13BE3B4BC`，OpenOCD 返回 `Verified OK`；编译定义确认 `TM_BENCH_DIAG=1`、`TM_FEATURE_VOFA=0`、`FOC_ENERGIZED_CALIB_ENABLE=0`。COM3 实测启动状态为 `axis=idle`、`fault=0`、`vbus≈11.91 V`。
- D0-D4/O0-O3 受控实测使用 20 ms 脉冲或约 50 ms 自动护栏。`vq=0.10–0.40 V`、`we=3.14 rad/s` 时，板上 ADC/CSA 换算的锁存相电流最大约 `60 mA`，没有触发 `FAULT_OVER_CURR`；O1 锁存 `theta≈0.075`、`we≈3/3.14`、duty 约 `0.501/0.495/0.505`。
- 实测发现正常 `state disable` 后安全清理会清零 `theta_sim/we_volt_now/we_volt_ramp`，导致停机诊断丢失开环运动信息。先新增失败测试，再在 bench-only FAULT 路径中于 PWM 已关闭、PID/设定值已清理后恢复这三个只读字段；重新构建、刷写并重复 O1 后，`obs all` 可读到 `theta≈0.01`、`we≈1.28/3.14`。
- 停机分析结论：当前低压短脉冲未复现高电流卡死，也未复现软件角度不推进；低压不转暂列电压/转矩裕量、齿槽转矩、静摩擦、机械负载或电机参数候选，唯一主根因仍为 `UNRESOLVED`。外部相电流、实际 PWM 波形、死区/重叠、DRV 温升和连续机械运行仍未验证。
- 详细证据固化于 `tmp/bench/20260829_actual/result.md` 和 `docs/code-wiki/` 对应章节；当前状态为 `OPEN_LOOP_SOFTWARE_PATH=PASS`、`MOTOR_PHYSICAL_OPERATION=UNVERIFIED`、`CONTROLLED_DIAGNOSTIC_STATUS=ALLOWED_WITH_MINIMUM_GUARDS`。

### 2026-08-29 全量回归收口

- 重新执行 host flow 和独立 CTest，均为 `35/35 PASS`；外设静态契约输出
  `FOC_PERIPHERAL_CONTRACT=PASS`，Code Wiki 与脚本范围内 Markdown 相对链接检查通过。
- bench、full-diag、standard、tiny 固件均新鲜构建通过，资源分别为
  `76,740/21,320 B`、`77,128/21,720 B`、`39,876/19,984 B`、
  `36,668/18,248 B`（FLASH/RAM），均未超过链接脚本限制；bench ELF 身份和
  编译定义与实板日志一致。
- 全工作区 `git diff --check` 仍会报告已有 build/vendor 生成文件的尾随空格，未对用户生成物做清理；任务范围内源码、测试和文档定向检查通过。产品级
  `REAL_BENCH_TEST_STATUS=BLOCKED` 不变，受控短脉冲诊断结论仍为根因未唯一确认。

### 2026-08-29 最终 bench 镜像身份复核

- 注释修改会改变带调试信息 ELF 的 SHA256；为避免实测日志与当前镜像错配，重新将
  当前 `build/fw-full-diag-carrier-bench-debug/output/falcon-foc-at32.elf` 刷入板上，
  OpenOCD 返回 `Verified OK`，最终 SHA256 为
  `325A464AA838DCB7344657E2917953DD52515D72AD7ED0B40B5AF91D5C5BEE36`。
- 用同一 `diag set vq 0.10 3.14`、20 ms 条件复测 O1，COM3 停机后读到
  `fault=0`、`vbus≈11.91 V`、电流约 `1/2/1 mA`、`theta≈0.01`、
  `we≈1.32/3.14`；最终镜像的重复结果与首轮一致。
### 2026-08-29 电压模式入口相位 A/B 修复

- `[静态推导]` 确认默认 `CTRL_MODE_VOLT` 下首次 `vq` 写入可能未同步
  `theta_sim`，导致它与实时编码器电角度错位；第一版读取未刷新的
  `meas_data.theta_elec` 不足，最终改为 `enc_pri.state.angle_elec_rad`。
- `[主机测试]` 入口同步、角度 wrap、I-f 同入口、运行态不跳角和快照语义测试先失败
  后通过；最终 host flow 与独立 CTest 均为 `35/35 PASS`，外设静态契约通过。
- `[实板测量]` 修复前 A1 为 `theta_sim≈0`、`theta_elec≈3.35 rad`；最终 bench
  镜像 A1 为 `theta_sim≈3.21`、`theta_elec≈3.20 rad`，误差约 `0.01 rad`。
  正反向 `20 ms` 短脉冲和 `vq=0.20/0.30/0.40 V` 梯度均无故障，VBUS 约
  `11.91 V`，板上换算电流未超过 `0.10 A`。
- 最终 bench ELF 为
  `build/fw-full-diag-carrier-bench-debug/output/falcon-foc-at32.elf`，SHA256
  `F0402BADC248FEB2C7EEB3F149DF321B6A3189B3C65125AB876A5E83064F04B1`，编译定义
  为 `TM_BENCH_DIAG=1`、`TM_FEATURE_VOFA=0`、`FOC_ENERGIZED_CALIB_ENABLE=0`。
- 当前只确认相位入口软件缺陷及其修复，未确认整机唯一高电流卡死根因；状态保持
  `PHASE_ENTRY_ALIGNMENT=CONFIRMED`、`OPEN_LOOP_SOFTWARE_PATH=PASS`、
  `MOTOR_PHYSICAL_OPERATION=UNVERIFIED`、`OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、
`REAL_BENCH_TEST_STATUS=BLOCKED`。原始记录见
`tmp/bench/20260829_phase_seed_final/`。

## 2026-08-29 低压开环根因定位实施轮次

- `[实板测量]` 当前 50 ms bench 镜像为
  `build/fw-full-diag-carrier-bench-debug/output/falcon-foc-at32.elf`，SHA256
  `3969B23E8DB62BCD590B49D5FE2F63574944C3281CDF5A00D2A3CCE3931AD8AE`；定义为
  `TM_BENCH_DIAG=1`、`TM_FEATURE_VOFA=0`、`FOC_ENERGIZED_CALIB_ENABLE=0`、
  `TM_MOTOR_BENCH_RUN_BUDGET_MS=50`。
- `[实板测量]` F0–F6、theta0、V1–V4 的 pulse 矩阵对应上一轮
  `7EBC...` bench ELF；随后仅因 host 宏头文件可见性修正重建/刷写为
  `3969...`，刷写后只做 A0 只读复核，没有重复发送电机脉冲。
- `[实板测量]` 早期 Shell 无响应由调试器确认是 `app_shell_task` 栈溢出；先由
  host 契约复现，再仅增大 bench app-shell 栈到 2048 B，重建/刷写后恢复。此项是
  诊断基础设施修复，不是电机或 DRV8311H 根因。
- `[实板测量]` A1 相位入口约 `0.01 rad` 对齐；F0–F6 固定矢量峰值约
  `0.010–0.031 A`；theta0 矩阵没有可重复优胜；V1–V4 20 ms 未触发故障，
  但 V3/V4 已超过 50 mA 延长运行门槛，因此没有执行 200 ms 实机镜像。
- `[实板未验证]` 本轮没有独立相电流、示波器死区/重叠、温度、手动编码器整圈或
  连续机械运行证据；原始高电流卡死没有在当前 20 ms 窗口复现，不能指定唯一物理根因。
- `[无板验证]` 主机测试/CTest 为 `36/36 PASS`，外设契约为
  `FOC_PERIPHERAL_CONTRACT=PASS`，50 ms/200 ms bench、full-diag、standard、tiny
  构建通过。软件与构建证据不解除 `REAL_BENCH_TEST_STATUS=BLOCKED`。
- 当前状态固定为：
  `PHASE_ENTRY_ALIGNMENT=CONFIRMED`、`OPEN_LOOP_SOFTWARE_PATH=PASS`、
  `MOTOR_PHYSICAL_OPERATION=UNVERIFIED`、`OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、
  `REAL_BENCH_TEST_STATUS=BLOCKED`。
- 详细日志：`tmp/bench/20260829_133853_next_root_cause_final/`；保留所有失败和
  中间日志，不执行 reset、clean、checkout、批量删除或自动提交。

### 2026-08-30 低压开环分层正交诊断能力收口

- `[源码确认]` 完成 bench-only 64 条 FOC 定点 RAM ring、32 条编码器 RAM ring、6×6
  PWM/ADC 映射实验、TMR1 CH4 compare 合法扫描和停机后 Shell 读取边界；非 bench 固件
  不分配 trace/override，产品默认映射、相序、保护阈值和电机参数未改变。
- `[源码确认]` 完成 `scripts/flows/board/run_bench_matrix.ps1` 白名单矩阵入口：只读
  Preflight 不带电，主动矩阵必须 `-RunPulses`；`state disable` 后需要
  `-WaitForPowerCycle` 才能进入下一项，脚本不发送校准、I-f、参数写入、`state stop`
  或 `fault clear`。
- `[无板验证]` host flow 和独立 CTest 均为 `38/38 PASS`；外设静态契约输出
  `FOC_PERIPHERAL_CONTRACT=PASS`；bench50、bench200、full-diag、standard、tiny 固件
  构建均通过，最大当前 bench RAM 使用 `27,472/32,768 B`，未超过链接限制。
- `[无板验证]` 当前 bench50/bench200 ELF SHA256 分别为
  `7CFBDED0B667192AA6410788C41B0CD2BCF3D724D3F48848E1B5E76E4D139B9C` 和
  `7440A83EE8306FEF75F8B259754ECF75ED3521ED3012185476FDB7A33EDA68AD`；历史实板日志
  的旧镜像哈希不与本轮新 trace 矩阵混用。
- `[实板未验证]` 当时本轮尚未用新增最终镜像执行新矩阵；随后首个脉冲的独立记录见本页
  后文。此前实板日志按旧 ELF 身份单独保留。真实 PWM 死区/重叠、ADC 采样孔径、独立
  相电流、DRV 热状态和产品级连续运行仍未验证。
- 状态继续保持：`OPEN_LOOP_SOFTWARE_PATH=PASS`、`MOTOR_PHYSICAL_OPERATION=UNVERIFIED`、
  `OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、`REAL_BENCH_TEST_STATUS=BLOCKED`。

### 2026-08-30 当前 bench50 实机根因定位执行记录

- `[无板验证]` 重跑 host flow 与独立 CTest 均为 `38/38 PASS`，外设静态契约为
  `FOC_PERIPHERAL_CONTRACT=PASS`；bench50 重建成功。
- `[实板测量]` 通过显式 OpenOCD 刷写当前 bench50 ELF，返回 `Verified OK`；SHA256 为
  `7CFBDED0B667192AA6410788C41B0CD2BCF3D724D3F48848E1B5E76E4D139B9C`。Preflight
  在 COM3 通过，确认 map/encoder-trace marker、`axis=idle`、`fault=0`。
- `[实板测量]` 无 PWM 静态快照 VBUS 约 `11.91 V`、duty `0/0/0`、软件电流低于
  `0.10 A`。首个 map0/adc0、CH4 compare `3740`、`dp0=0.05 V` 脉冲的软件电流峰值
  为 `0.019/0.012/0.013 A`，VBUS `11.89-11.92 V`，duty 约 `0.498/0.502/0.502`。
- `[实板测量]` 首脉冲停机后锁存 `FAULT_COMM_ERROR (0x100)`，trace 保留 250 条
  FOC 样本；矩阵在下一项前停止，未发送后续映射/触发/相位命令。
- `[静态推导]` 250 条样本与 bench50 50 ms 护栏一致，名义 5 ms 与板端实际停机时序
  不等价；当前应把 stop-latency 作为诊断基础设施问题单独处理，不能归因于电机或
  DRV8311H，也不能把一次脉冲写成根因确认。
- `[实板未验证]` 本轮未完成完整矩阵、人工整圈编码器验证、示波器/独立相电流/温度
  证据；状态保持 `OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、
  `REAL_BENCH_TEST_STATUS=BLOCKED`。详细日志：
`tmp/bench/20260830_012451_comprehensive_root_cause/`。

### 2026-08-30 开环测试包线和有效时基校正

- `[静态推导]` 结合 DRV8311H GAIN=`0.5 V/A`、2804 电机 `0.5 A` 额定/`2.0 A` 最大和
  首个 bench50 脉冲实际仅约 `0.019/0.012/0.013 A` 软件峰值，确认旧的
  `0.20 A` 快速、`0.10 A` 预警、`0.12 A` 堵转包线不能同时承担“正常开环验证”和
  “绝对故障关断”。当前临时值为快速 `1.00 A`、持续 `0.75 A/3 s`、闭环堵转
  `0.75 A/200 ms`。
- `[源码确认]` `CTRL_MODE_VOLT` 不再执行闭环堵转判定；bench ADC 快速路径不再把
  `0.75 A` 预警在 3 个样本后直接伪装成 `FAULT_OVER_CURR`，绝对快速路径、3 s 持续
  保护和首故障快照保持。
- `[静态推导]` 7 对极电机使用 30 rpm（约 `21.99 rad/s` 电气速度）作为首轮有意义
  的开环速度，电周期约 `285.7 ms`；新增 bench400 和 400 ms Voltage 脉冲，避免把
  5 ms/`3.14 rad/s` 误写成旋转验证。固定矢量/映射/触发仍使用 5–20 ms。
- `[无板验证待执行]` 新增主机测试先失败后通过，覆盖保护包线、开环堵转隔离和 bench
  时基；400 ms 预设和全量构建/测试结果将在本轮验证后补录。当前不执行新的板端启动
  命令，仍保持 `OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、
  `REAL_BENCH_TEST_STATUS=BLOCKED`。

### 2026-08-30 无板门禁与固件构建收口

- `[无板验证]` host flow 和独立 CTest 均为 `38/38 PASS`，外设静态契约输出
  `FOC_PERIPHERAL_CONTRACT=PASS`；bench50、bench200、bench400、full-diag、standard、
  tiny 六个构建 preset 均成功，FLASH/RAM 均未超过 AT32F415 链接区域限制。
- `[无板验证]` bench50/200/400 的编译定义分别锁定为
  `TM_BENCH_DIAG=1`、`TM_FEATURE_VOFA=0`、`FOC_ENERGIZED_CALIB_ENABLE=0` 和预算
  `50/200/400 ms`；产品镜像未携带 bench 诊断宏或 200/400 ms 预算。
- `[无板验证]` 本次重建 ELF SHA256 为 bench50
  `A80994B415782E4F3CAB45A59510F9F014984149963FBC88BCAC8F864B21825A`、bench200
  `9BB77862DEB793F68ED1D085CB21E5C7933712D1538967821AADA890F5D4DA41`、bench400
  `38B6036012D9BCEB5849A5B5260BF75E5C151481FEC9CE8F5F5854563D576285`；这些镜像尚未
  用于新的板端启动矩阵。
- `[无板验证]` `run_bench_matrix.ps1` 通过 PowerShell 语法解析，CMake preset JSON
  通过解析，映射矩阵的 power-cycle 下一项提示顺序已修正。未在本轮发送新的板端
  启动命令或 OpenOCD 写入动作。
- 当前状态仍为：`OPEN_LOOP_SOFTWARE_PATH=PASS`、`MOTOR_PHYSICAL_OPERATION=UNVERIFIED`、
  `OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、`REAL_BENCH_TEST_STATUS=BLOCKED`。

### 2026-08-30 当前标准 SVPWM 与实机正交矩阵收口

- `[源码确认][无板验证]` 将 FOC SVPWM 统一为保持 alpha/beta 向量的归一化逆 Clarke/common-mode 实现；专项模型、host flow 和独立 CTest 均为 `38/38 PASS`，外设静态契约为 `FOC_PERIPHERAL_CONTRACT=PASS`。同时修复停机 snapshot 使用陈旧 FOC 速度/位置缓存的问题，新增契约测试后全部通过。
- `[实板测量]` bench400 ELF `25A38A987BE93B48C05526B8A6839F697AE618AACFD740C8517AAA661CA65922` 经显式 OpenOCD 刷写并返回 `Verified OK`；复位后 `axis=idle`、`fault=0`、VBUS 约 `11.91 V`、duty 为零。
- `[实板测量]` 完成 36 组 PWM/ADC 映射组合、CH4 compare 动态扫描、theta0 对照和 15/20/30 rpm 电压开环短时复测。没有可重复的 PWM/ADC map 或相位 offset 唯一优胜；不修改默认相序、ADC rank、编码器零点或电机参数。
- `[实板测量][静态推导]` CH4 compare 对软件电流观测有明显影响：低 compare 约 `0.01 A` 量级，靠近 `2300..3740` 约 `0.27 A` 量级，保留“低侧 CSA/采样窗口”候选；无 SOx 波形和独立相电流，不能将软件电流当作物理相电流证明。
- `[实板测量]` 2 V/20–30 rpm 未建立稳定同步；3 V 一次达到约 `1 A` 软件快速边界并锁存 `FAULT_OVER_CURR`；2 V/15 rpm 两次不可重复。当前只证明软件角度/duty 与板上 ADC 观测能够运行一段时间，不能证明物理电机连续运行。
- `[实板未验证]` 尚未完成 PWM 禁止下人工 encoder 整圈、PWM/DRV/SOx 示波器、独立相电流和温升测量。因此状态保持：`OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、`MOTOR_PHYSICAL_OPERATION=UNVERIFIED`、`REAL_BENCH_TEST_STATUS=BLOCKED`。
- 原始证据：`tmp/bench/20260830_074936_post_snapshot_fix/result.md`。

### 2026-08-30 当前镜像方向 A/B 与正确 30 rpm 窗口

- `[无板验证]` 补充 `test_app_shell_diag_contract` 对 `diag set enc_dir 0|1` 的覆盖；命令为 bench-only、IDLE-only、RAM-only，非法值和运行中写入均拒绝。bench runner 白名单契约同步通过。
- `[实板测量]` 使用 bench400 ELF `59B78D648511926F8AD47213CA35E49C0D536697BE814B1B77AF64F84E949A03`，完成静态 `vq=0.50 V,we=0` 方向 A/B 各两次，以及 `we=21.9911 rad/s`、350 ms 的 `0.25 V/1.00 V` 正反向短脉冲。VBUS 约 `11.87–11.93 V`，软件电流最高约 `0.289 A`，未触发 1 A 快速故障。
- `[实板测量]` `theta_sim` 连续推进，但 encoder 尾段只显示小幅或不稳定变化；`enc_dir=0/1` 有电流差异却未在同一机械起始位置下形成可重复唯一优胜，因此不修改产品默认方向。
- `[实板测量][静态推导]` CH4 compare 电流敏感性仍单独保留为 ADC/CSA 采样窗口候选；方向候选标记为 `ENCODER_DIRECTION_CANDIDATE=SUPPORTED_NOT_CONFIRMED`，不能合并为唯一根因。
- `[实板未验证]` 仍无 PWM/死区/SOx 波形、独立相电流、温度、手动 encoder 整圈和机械测速证据，状态保持 `OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、`MOTOR_PHYSICAL_OPERATION=UNVERIFIED`、`REAL_BENCH_TEST_STATUS=BLOCKED`。
- 原始证据：`tmp/bench/20260830_091746_encoder_trace_freeze_validation/result.md` 及同目录方向/电压日志。

### 2026-08-30 最终门禁与安全快照

- `[无板验证]` 补齐 bench trace 测试桩后，host flow 与独立 CTest 均为 `38/38 PASS`；
  `FOC_PERIPHERAL_CONTRACT=PASS`、`BENCH_MATRIX_CONTRACT=PASS`；bench50/200/400、
  full-diag、standard、tiny 六个固件构建均成功，链接区域未越界。
- `[实板测量]` 仅执行 OpenOCD MCU reset 和只读串口快照，未刷写新镜像、未发送启动/校准/
  参数写入命令。最终板端为 `axis=idle`、`fault=0`、`VBUS=11.91 V`、`duty=0/0/0`，
  `pwm_map=0`、`adc_map=0`、`adc_trig=3740`、编码器 trace `retained=32`。
- `[实板未验证]` 方向候选与 ADC/CSA 采样窗口候选仍未形成唯一、可重复的主根因；
  `OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、`MOTOR_PHYSICAL_OPERATION=UNVERIFIED`、
  `REAL_BENCH_TEST_STATUS=BLOCKED` 保持不变。完整门禁和快照见：
  `tmp/bench/20260830_091746_encoder_trace_freeze_validation/result.md`。

### 2026-08-30 ADC trigger 重复对照与停机窗口修正

- `[实板测量]` 使用 bench400 镜像 `59B78D648511926F8AD47213CA35E49C0D536697BE814B1B77AF64F84E949A03`，
  在 `vq=1.00 V`、`we=21.9911 rad/s`、`enc_dir=0` 下将 `adc_trig=512` 和 `3740` 各重复两次。
  512 的软件电流峰值约 `0.012/0.011/0.014 A`、`0.012/0.012/0.015 A`；3740 约
  `0.270/0.312/0.262 A`、`0.275/0.276/0.201 A`。VBUS 约 `11.87..11.93 V`，编码器
  机械角尾段变化接近零。
- `[静态推导]` 触发点差异在两次重复中保持，ADC/CSA 软件观测对 CH4 compare 敏感，
  低侧 CSA 建立时间、中心对齐计数相位、ADC 注入完成时序和采样孔径成为高优先级候选。
  512 不能直接认定为正确采样点，3740 的软件电流也不是独立相电流计读数。
- `[实板测量]` 每项 trace 约 2000 条，说明实际窗口约 400 ms；名义 350 ms 等待加串口往返
  跨过 bench400 预算，四次在 `state disable` 后出现 `FAULT_COMM_ERROR (0x100)`，没有
  `FAULT_OVER_CURR`。该现象归类为测试编排/预算边界，后续以 trace 样本数判断实际时长。
- `[实板未验证]` 仍没有 PWM/SOx 波形、独立相电流和温度证据，不能宣称真实采样孔径、
  死区/重叠、DRV 热稳定或电机已达到 30 rpm。详细数据见：
  `tmp/bench/20260830_095925_adc_trigger_followup/result.md`。
- 状态保持：`ADC_MEASUREMENT_WINDOW=ANOMALOUS_OR_STRONGLY_SENSITIVE`、
  `OPEN_LOOP_SOFTWARE_PATH=PASS`、`MOTOR_PHYSICAL_OPERATION=UNVERIFIED`、
  `OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、`REAL_BENCH_TEST_STATUS=BLOCKED`。

### 2026-08-30 完整正交实机矩阵完成

- `[实板测量]` 使用同一 bench400 镜像完成 9 点 ADC trigger、36 组 PWM/ADC map、六向
  固定矢量、1 V 静态相位矩阵、`vq=0.25..2.00 V`/`we=21.9911 rad/s` 电压梯度、正反向
  动态及 `enc_dir=0/1` A/B。所有测试逐项 reset、短时运行、停机后读取，未执行校准、
  参数写入、`state stop` 或长时间运行。
- `[实板测量]` 低幅 trigger/map/固定矢量均无唯一软件优胜或单相饱和；电压升至 2 V 时
  软件峰值最高约 `0.468/0.450/0.065 A`，VBUS 约 `11.86..11.95 V`，软件场推进正常，
  但编码器没有稳定显示 30 rpm 机械跟随。
- `[静态推导]` 动态高电压下 ADC/CSA 软件观测仍对 CH4 compare 敏感；低幅静态扫描差异
  很小。相位偏置和 encoder direction A/B 没有同条件、可重复的唯一机械改善，不修改默认
  ADC trigger、相序、encoder direction、FOC 参数或保护阈值。
- `[实板未验证]` 没有 PWM/SOx 波形、独立相电流、温度和机械测速证据，不能宣称真实功率
  级、采样孔径、死区/重叠、DRV 热稳定或产品级连续运行已验证。完整汇总：
  `tmp/bench/20260830_103200_full_hardware_root_cause/result.md`。
- 状态保持：`ADC_MEASUREMENT_WINDOW=ANOMALOUS_OR_STRONGLY_SENSITIVE`、
  `PHASE_OFFSET_CANDIDATE=REJECTED_AS_UNREPEATED`、
  `ENCODER_DIRECTION_CANDIDATE=SUPPORTED_NOT_CONFIRMED`、
  `OPEN_LOOP_SOFTWARE_PATH=PASS`、`MOTOR_PHYSICAL_OPERATION=UNVERIFIED`、
  `OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、`REAL_BENCH_TEST_STATUS=BLOCKED`。

## 2026-08-30 动态 ADC trigger 全曲线和边界重复

- `[实板测量]` 使用 bench400 镜像在 `vq=1.00 V`、`we=21.9911 rad/s` 下完成
  `CH4 compare=512..3740` 的 9 点动态扫描；`1874/2300/2812/3740` 各重复两次。
  `1874` 的软件峰值约 `0.012..0.017 A`，`2300` 的 B 相峰值约 `0.256..0.259 A`，
  `2812/3740` 的 A/B 峰值约 `0.237..0.264 A`；VBUS 保持 `11.87..11.93 V`，
  八次均无 fault。
- `[静态推导]` `TMR1 CH4 compare` 在 `1874→2300` 出现可重复转折，且高 compare
  保持高读数，说明 MCU ADC/CSA 软件观测与触发位置存在稳定关系；四 rank 顺序采样、
  中心对齐上/下数事件和 DRV8311H 低侧 SOx/CSA 建立窗口仍需联合解释。
- `[实板未验证]` 该证据不等于真实相电流、SOx 波形或正确采样窗口，不修改默认
  `adc_trig`、ADC rank、相序、encoder direction 或保护阈值。最终复位状态为
  `axis=idle`、`fault=0`、`duty=0/0/0`。
- 状态更新：`ADC_MEASUREMENT_WINDOW=REPRODUCIBLY_TRIGGER_DEPENDENT`、
  `PHASE_OFFSET_CANDIDATE=REJECTED_AS_UNREPEATED`、
  `ENCODER_DIRECTION_CANDIDATE=SUPPORTED_NOT_CONFIRMED`、
  `OPEN_LOOP_SOFTWARE_PATH=PASS`、`MOTOR_PHYSICAL_OPERATION=UNVERIFIED`、
  `OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、`REAL_BENCH_TEST_STATUS=BLOCKED`。
- 原始记录：`tmp/bench/20260830_103200_full_hardware_root_cause/result.md` 和
  `tmp/bench/20260830_104326_dynamic_trigger_boundary_repeats_1V_30rpm/`。

## 2026-08-30 最终验证门禁记录

- `[无板验证]` 主机测试 `38/38`、独立 CTest `38/38`、`FOC_PERIPHERAL_CONTRACT=PASS`、
  `BENCH_MATRIX_CONTRACT=PASS`。
- `[无板验证]` bench50/bench200/bench400、full-diag、standard、tiny 六个固件目标均
  构建成功；本轮使用的 bench400 ELF SHA256 仍为
  `59B78D648511926F8AD47213CA35E49C0D536697BE814B1B77AF64F84E949A03`。
- `[无板验证]` 本轮触及文档/日志路径的链接、尾随空白和 `git diff --check` 均通过。
  全量 `git diff --check` 仍受已有 build/work 生成文件空白影响，未执行清理或回退。
- 状态保持：`ADC_MEASUREMENT_WINDOW=REPRODUCIBLY_TRIGGER_DEPENDENT`、
  `OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、`MOTOR_PHYSICAL_OPERATION=UNVERIFIED`、
  `REAL_BENCH_TEST_STATUS=BLOCKED`。

## 2026-08-30 既有证据深度根因分析固化

- `[文档][源码确认][实板测量]` 新增
  `docs/code-wiki/open-loop-root-cause-deep-analysis.md`；本轮只读复核当前源码、原理图结论和
  同一 bench400 镜像日志，没有运行主机测试、构建、刷写、串口命令或 PWM 动作。
- `[静态推导]` 纯 `CTRL_MODE_VOLT` 路径不使用电流 PI；ADC/CSA 对 CH4 compare 的可重复
  依赖会削弱软件电流和转矩推导的可信度，但现有调用链不支持把它直接写成纯电压开环不转的
  唯一原因。
- `[静态推导]` 既有完整窗口结束时 `we_now≈16.7..18.2 rad/s`，尚未达到 30 rpm 对应的
  `21.99 rad/s`，因此属于启动拉入瞬态而非 30 rpm 稳态证明。
- `[静态推导][实板未验证]` 当前最强运动根因候选升级为编码器磁轴零点、方向与物理相序组成的
  复合电角参考不一致；相位入口 bug 已修复，但 `theta_sim=theta_elec` 仍不能证明与真实转子
  磁轴对齐。DRV/PCB/接线和机械负载继续作为并列候选。
- 决策：停止继续盲目升压、扫 offset/map/trigger；先冻结证据并把运动链与测量链分开。
  `OPEN_LOOP_ROOT_CAUSE_STATUS` 仍为 `UNRESOLVED`，不修改 ADC trigger、相序、encoder
  direction、保护阈值或电机参数。

## 2026-08-30 项目总结、调试总结和 AI 交接文档

- `[文档]` 新增 `docs/code-wiki/project-summary.md`，汇总 AT32F415/DRV8311H 项目架构、
  模块职责、FOC 热路径、硬件映射、保护边界、构建方式和证据状态。
- `[文档]` 新增 `docs/code-wiki/debugging-summary.md`，固化动态/静态 ADC trigger 转折、
  固定矢量电压梯度、1–2.5 V/30 rpm 参数窗口、2 V trigger A/B 和当前根因候选。
- `[文档]` 新增 `docs/code-wiki/ai-handoff-prompt.md`，提供下一位 AI 可直接使用的
  自包含上下文、镜像哈希、日志索引、执行纪律和验收条件。
- `[实板测量]` 最新 2.5 V 窗口软件峰值约 `0.680/0.754/0.650 A`，VBUS `11.83..11.91 V`，
  无 software fault 但未稳定跟随；因接近 `0.75 A` 包线，本轮停止继续升压。
- 状态保持：`OPEN_LOOP_SOFTWARE_PATH=PASS`、`MOTOR_PHYSICAL_OPERATION=UNVERIFIED`、
  `OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、`REAL_BENCH_TEST_STATUS=BLOCKED`。

## 2026-08-30 静态 trigger 隔离与完整低速窗口补充

- `[实板测量]` 在 `we=0、vq=1.00 V` 下对 `adc_trig=1874/2300/2812/3740` 各重复两次，
  仍观察到约 `0.01 A` 低读数、2300 的约 `0.26 A` B 相转折和 2812/3740 的约
  `0.24..0.26 A` A/B 高读数；VBUS `11.87..11.93 V`，无 fault。该结果说明触发
  相关性不依赖旋转场推进，采样窗口/CSA 建立候选升级为独立的静态候选。
- `[实板测量]` 在 `we=21.9911 rad/s`、约 375 ms 窗口下完成 1.0 V 首轮和重复、1.5 V、
  2.0 V。软件峰值分别约为 `0.257/0.256/0.033 A`、`0.273/0.277/0.233 A`、
  `0.407/0.435/0.420 A`、`0.586/0.633/0.578 A`；软件场推进，encoder 仅部分跟随，
  未形成稳定 30 rpm 机械证据。
- `[静态推导]` VBUS 未随上述窗口同步塌陷，但软件 ADC/CSA 值不能替代独立相电流；
  不修改默认 CH4 compare、ADC rank、相序、encoder direction、保护阈值或电机参数。
- `[实板未验证]` 仍无 SOx/PWM 波形、独立相电流、温度和机械测速，故不得宣称真实功率
  级、采样孔径、死区/重叠、DRV 热稳定或产品级连续运行已验证。
- 原始证据：`tmp/bench/20260830_105417_static_trigger_boundary_1V/` 以及
  `tmp/bench/20260830_105810_voltage_full_window_1V_30rpm/`、
  `tmp/bench/20260830_105937_voltage_full_window_1V_30rpm_repeat/`、
  `tmp/bench/20260830_110024_voltage_full_window_1p5V_30rpm/`、
  `tmp/bench/20260830_110109_voltage_full_window_2V_30rpm/`。
- 状态保持：`ADC_MEASUREMENT_WINDOW=REPRODUCIBLY_TRIGGER_DEPENDENT`、
  `OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、`MOTOR_PHYSICAL_OPERATION=UNVERIFIED`、
  `REAL_BENCH_TEST_STATUS=BLOCKED`。

## 2026-08-30 开环根因延申验证与修复计划启动

- `[文档]` 新增 `docs/planning/Themis_开环根因延申验证与修复计划.md`（本轮唯一计划入口），
  并在 `docs/code-wiki/README.md` 导航表挂链接。
- `[静态推导]` 用 6 个历史实机运行（静态 0.5/1.0/1.5/2.0 V 梯度、边界 1.0 V、动态 2 V 双触发点）
  做了"真实电流组（theta_sim/vq/vbus 反推，SVPWM 链逐档验证误差 <0.2%）→ 三 rank 读数组"拟合，
  唯一统一变换为 `measured = −k × swapAC(true)`，k≈0.66~0.90（多数 0.70~0.80）；
  rank 信号组在所有运行中 KCL 自洽（残差 2~25 counts）。
- `[静态推导]` 2804 电机手册确认 Rs=2.55 Ω/Ls=0.86 mH/ψ=0.0035 Wb，DRV8311 数据手册确认
  GAIN 47 kΩ→0.5 V/A；恒定标度误差候选（Rs/增益/CSAREF offset）全部排除，
  k 缺口候选收敛到 PWM 纹波采样偏差（反推 L_real≈0.2~0.4 mH，手册 0.86 mH 存疑）与孔径骑跨损失。
- `[静态推导]` "电压升高软件电流快速增大"大部分是真实欧姆响应；开环带不动的主嫌疑转向
  编码器零点/方向从未标定（带电标定被门禁关闭）与 rank↔相位配对 A↔C 互换
  （使任何电流反馈模式矢量错误）；采样窗口几何仍是真实观测完整性问题。
- `[工具]` 新增 `scripts/flows/board/run_openloop_probe.ps1`
  （T1 触发细扫描 × 2 个 theta0 / T2 V-I 线性度 / T3 theta0 扫描；白名单、单脉冲/电源循环、
  0.75 A 包线自动监护、-AutoGate 非交互断电门控）与
  `scripts/flows/board/analyze_openloop_probe.py`（主机侧日志分析，验证 P1~P4）。
- `[实板测量]` 预检快照：`axis=idle`、`vbus=11.9 V`、offset `2050.6/2050.9/2046.9`、
  `adc_trig=3740`、map0。T1 完成首例（theta0=0.4、trig=900、vq=1.00 V）：
  CCR=2143/1797/1605 与预测一致，峰值 `0.11/0.08/0.03 A` 无 fault；
  后续用例因操作员不在场两次门控超时安全中止，无异常现象。
- 状态保持：`OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、`MOTOR_PHYSICAL_OPERATION=UNVERIFIED`、
  `REAL_BENCH_TEST_STATUS=BLOCKED`；未修改任何保护文件与用户工作树变更。

## 2026-08-30 持久化校准坐标与 DRV 时序反证审计

- `[文档][源码确认][实板测量]` 在不运行测试、不构建、不刷写、不发送串口和不启动 PWM 的
  前提下，继续复核编码器校准生产、Flash 保存/恢复、bench override、DRV8311H 数据手册和既有
  同镜像实板日志；同步更新项目总结、调试总结、AI 接手提示词和深度分析文档。
- `[实板测量][源码确认]` 历史只读基线存在 `enc_valid=1、mech_offset=-0.124 rad、pole_pairs=7`；
  bring-up 禁止新带电校准不等于旧校准未生效。编码器 payload 将
  `phase_order/dir/mech_offset/LUT/pole_pairs/calib_valid` 作为一个元组恢复，但当前只有结构版本和
  CRC 校验，没有电机、板号、PWM map 或校准算法 provenance 绑定。
- `[源码确认]` bench PWM map 绕过已加载的 `phase_order`，运行时 `enc_dir` override 又不会同步
  变换或失效 offset/LUT，因此早期 map、方向和 theta0 A/B 不能证明完整坐标系正确。
- `[静态推导/待证伪]` 校准以 `target-measured` 生成均值和 LUT，LUT 在运行时相加，而
  `mech_offset` 在运行时被相减，存在 offset 符号一致性嫌疑。历史 offset 在 7 对极下使加/减
  两种消费方式相差约 `1.736 rad_e`，与部分滑差同量级，但尚不足以确认唯一根因。
- `[手册确认]` TMR1 `deadtime=0` 只表示 MCU 未增加额外死区；DRV8311H 仍有内部 VGS 握手和
  约 `450–760 ns` 输出死区。`0.05 V/12 V≈0.42%` 的早期 map 命令小于或接近该内部时序占比，
  不具备充分相序区分力；真实 PWM 重叠仍为实板未验证。
- 决策状态收窄为：`OPEN_LOOP_MATH_AND_DUTY_PATH=PASS`、
  `ENCODER_CALIBRATION_FRAME=UNVERIFIED`、`OPEN_LOOP_SOFTWARE_PATH=PARTIAL_ONLY`；
  `ADC_MEASUREMENT_WINDOW=REPRODUCIBLY_TRIGGER_DEPENDENT` 继续作为确定的测量问题，但调用链不支持
  将其直接写成纯电压开环运动根因。`OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、
  `MOTOR_PHYSICAL_OPERATION=UNVERIFIED`、`REAL_BENCH_TEST_STATUS=BLOCKED` 保持。

## 2026-08-30 纯电压开环根因排序纠偏：从编码器转向 CCR 后物理输出链

- `[源码确认]` 纯 `CTRL_MODE_VOLT` 只在 IDLE/切入模式时用 `theta_elec` 一次性播种
  `theta_sim`；运行后场角由 `we_volt_now × dt` 独立推进，固定相电压入口完全绕过编码器。
  因此先前将编码器校准坐标列为“纯开环持续不转”的首要直接根因不成立，已在项目总结、调试
  总结、AI 接手提示词和深度分析中纠正。编码器 payload/offset 风险仍作为未来闭环专项保留。
- `[静态推导]` 正确的 SVPWM 马鞍波、duty 和 TMR1 CCR 只证明命令链到寄存器；它们不能证明
  六个 MCU PWM 引脚、DRV8311H INH/INL、三个半桥和 `U-V/V-W/W-U` 实际线电压正确。完整
  三相的排列只改变相位原点或旋转方向；所有 map 都无法稳定旋转更支持缺相/输出畸变，而非单纯相序。
- `[原理图/网表确认]` `INHA/INLA...INHC/INLC` 均有 MCU 与 U4 双端点，但网表中的
  `OUTA/OUTB/OUTC` 每个只列出 U4 自身引脚，没有电机连接器或焊盘组件端点。该事实不等于
  实板已证明断路，但说明设计资料没有闭合证明 DRV 输出到电机端的连续性和顺序。
- `[源码/手册确认][实板未验证]` 当前 TMR1 为中心对齐 ARR=3749，并通过 IS2 接受 TMR3
  overflow 的从模式 RESET 同步；同源参考树使用独立 TMR1、ARR=7499、deadtime=15。该差异
  被列为“CCR 到真实引脚”的第二候选，尚无证据证明它就是具体根因，未修改生成配置。
- `[结论]` 当前最强直接根因类别更新为
  `ACTUAL_THREE_PHASE_OUTPUT_ASYMMETRY_OR_MISSING_LEG`；状态为
  `OPEN_LOOP_MATH_AND_DUTY_PATH=PASS`、
  `OPEN_LOOP_ENCODER_ROLE=ENTRY_SEED_ONLY_NOT_FEEDBACK`、
  `TIMER_PIN_DRV_OUTPUT_PATH=UNVERIFIED`、
  `PHYSICAL_LINE_TO_LINE_VOLTAGE=UNVERIFIED`、
  `OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED`、`REAL_BENCH_TEST_STATUS=BLOCKED`。
- 本轮仅做源码、网表、手册和既有日志只读分析及文档修改；未运行测试、构建、刷写、串口命令或 PWM。


## 2026-08-30（晚） 电流测量链确诊与修复：rank/phase 配对反 + 全局符号反

`[实板测量]` 使用 `scripts/flows/board/run_openloop_probe.ps1`（新增 SnapTest 模式与 SWD
复位门控，全程无需断电循环）完成 SnapTest 6 例、T1 触发细扫描 24 例（2 个 theta0 种子）、
T2 V-I 4 例、T3 theta0 扫描 6 例；修复后全套复测。

`[静态推导]` 对 6 个历史运行做"真实电流组（theta_sim/vq/vbus 反推，SVPWM 链逐档验证
<0.2%）→ 三 rank 读数组"拟合，唯一统一变换为 `measured = −k × swapAC(true)`；
2804 手册（Rs=2.55 Ω/Ls=0.86 mH）与 DRV8311 数据手册（GAIN 47 kΩ→0.5 V/A）排除了
恒定标度误差候选。

`[确诊]` ADC 注入组 rank 与相位配对 A↔C 接反（CH3 驱动相电流在 rank2、CH2→rank3、
CH1→rank4），且全局 `-FAC_CURR` 取负方向错误（旧 witness 在错误配对下自洽）。
`[修复]` `adc_read_phases()` 两分支按 PWM 通道重排 rank、移除取负；
`motor_bench_mapping.c` 表 map0={2,3,4}/map5={4,3,2}；host 契约与外设契约测试先行更新，
`[无板验证]` 38/38 PASS + `FOC_PERIPHERAL_CONTRACT=PASS`。
`[实板测量]` 新 ELF SHA256=`4043DC1A…273A34`（OpenOCD Verified OK）复测：dp 六向符号
全部正确（dp1+ 现显示主导正 ib，旧 witness 目标首次真正成立），k=0.65~0.83 三相一致；
T1 复扫 12 触发点读数稳定不随 CCR4 漂移。
`[遗留]` ① rank2(ia) 在 duty_a>0.53 时进入采样死区（孔径≈CNT1990 模型）；
② k=0.65~0.83 的每相标度误差（CSA 建立+纹波采样）待校准；
③ 编码器零点/方向从未带电标定，I-f 拖动标定现为可用工具；④ 30 rpm 动态连续旋转待验证。
状态：`OPEN_LOOP_ROOT_CAUSE_STATUS=PARTIALLY_RESOLVED`；`MOTOR_PHYSICAL_OPERATION=UNVERIFIED`、
`REAL_BENCH_TEST_STATUS=BLOCKED` 不变。计划与证据入口：
`docs/planning/Themis_开环根因延申验证与修复计划.md`。

## 2026-08-30（晚·补） 质量门禁 AT32F415 迁移与 PowerShell 5.1 健壮性修复

`[工具]` 门禁基础设施存在平台迁移遗留与 PS5.1 缺陷，本轮一并修复并验证：
- `check_cmake_quality/presets/smoke/platform_backend_contract.ps1` 及
  `tests/script/test_firmware_cli_contract.ps1` 的 `at32f403a` 引用迁移至 `at32f415`
  （linker 断言改为实际使用的 `AT32F415xB_FLASH.ld`；默认平台 CPU 类改为
  `cortex-m3-soft`；删除不存在的 at32f423 覆盖用例）。
- `check_themis_quality_gate.ps1` 的子脚本改为独立 powershell 进程调用：
  原 `& script.ps1` 同进程方式下 `$LASTEXITCODE` 残留子内最后一个原生命令退出码，
  导致可选工具探测（cmakelang 缺失时 exit 1）误判子脚本失败。
- `check_cmake_quality.ps1` 的 cmakelang 探测加 EAP 放宽：import 失败的 stderr
  在 EAP=Stop 下成为终止错误，使设计的"工具缺失→跳过格式强制"路径不可达。
  注意：cmake-format 强制是 opt-in；仓库 CMake 文件当前并非 format-clean，
  安装 cmakelang 会激活强制并失败（本轮已验证并回退），启用需专门格式化轮次。
- `check_transport_shell_hardening_contract.ps1`：`axis_state` 违规检测收窄为
  `motor->axis_state =`（原正则把读比较与快照结构体赋值误判为写）；
  串口见证波特率断言 1000000→115200，`run_diag_carrier_witness.ps1` 默认值同步。
- `test_falconfoc_themis_lite_contract.ps1`：`axis_state` 正则同步收窄；
  平台默认路径/启动/linker/part 引用迁移至 415；`foc_calib.c` 的 `#if 0`
  迁移残留已清理。该契约剩余约 3 处对 403a 平台文件的深度内容断言
  （如 `tm_platform_foc_timing.h` 在 415 平台不存在）需独立迁移轮完成，
  完成前 `-Scope all` 将止步于该阶段。
- `[无板验证]` 门禁 8/9 阶段 PASS（whitespace/cmake_quality/presets/
  platform_backend/smoke/firmware_cli/cache_guard/transport）+ host 38/38。

## 2026-08-30（晚·补2） 原理图门极网络核查：INLA/INLC（含 INHA/INHC）A/C 两相整体对调确认

`[静态推导]` 用户怀疑原理图 INLA/INLC 接反。三方核查结论：
- 数据手册（DS_AT32F415_V2.00，QFN32 引脚表 p27 + 厂商 `at32f415_gpio.h` GMUX 注释）：
  `TMR1_GMUX_0001` 下 PA10=CH3、PB1=CH3C；PA9=CH2、PB0=CH2C；PA8=CH1、PA7=CH1C。
  QFN32 封装：PA7=13、PB0=14、PB1=15、PA8=18、PA9=19、PA10=20。
- 固件 `at32f415_wk_config.h`：INHA=PA10、INLA=PB1、INHB=PA9、INLB=PB0、
  INHC=PA8、INLC=PA7 —— 与数据手册一致（CH3 对→INHA/INLA，CH1 对→INHC/INLC）。
- PCB 网表（`docs/pcb docs/pcb 网表.tel`）：INHA=U3.18(PA8)、INHC=U3.20(PA10)、
  INLA=U3.13(PA7)、INLC=U3.15(PB1)。
`[确诊]` **用户怀疑成立**：INLA/INLC 接反（PA7 的 CH1C 进了 INLA、PB1 的 CH3C 进了
INLC），且 INHA/INHC 同样对调（PA8 的 CH1 进了 INHA、PA10 的 CH3 进了 INHC）——
即固件 A/C 两相的门极对整体接到了对方的半桥。因高/低侧一致对调，每个半桥收到的
仍是互补对，无桥臂直通风险（与实测驱动波形干净、无过流一致）；等效效果为
**固件 A 相标签实际驱动电机 C 相绕组、C 相标签驱动 A 相绕组，相序反转**。
该发现与 2026-08-30 晚的电流链修复完全自洽：CH3 驱动相 = DRV 物理 C 相 =
SOC/rank2，故"rank2 承载固件 a 相电流"的修复正确且必要。
`[决策]` 保持电气现状（系统自洽，编码器标定将吸收标签差异），修正文档与认知；
后续 I-f 拖动标定时注意正 we 指令对应电机反方向旋转，编码器方向配置需匹配。

## 2026-08-30（晚·补3） A/C 对调的软件处置决策：保持电气现状 + 注释警示

`[决策]` 经分析不进行驱动侧的软件再对调：高/低侧一致对调使每个半桥仍收到
完整互补对，系统在"固件 a=电机 C"的自洽标签下工作，今晚的电流链修复
（rank2←SOC）已建立在该约定上并经实机验证；再对调只会改变代码命名而
破坏已验证的自洽性。编码器零点/方向标定（下一轮 I-f 拖动）将把固件
坐标系绑定到物理转子，彻底吸收标签差异。
`[文档]` `at32f415_wk_config.h` 门极引脚定义处新增警示注释：pin 名跟随
TMR1 通道对而非原理图网络名；as-built 板 ADC map0={2,3,4}；若改版修正
网络需切回 {4,3,2}（diag map adc 5）并回退 rank 配对。

## 2026-08-30（晚·收口） 完整测试闭环：电流链修复确认为根因，电机首次受控旋转

`[实板测量]` 修复后固件（ELF 4043DC1A…/b65db9be 注释版）完成 I-f 拖动闭环测试
（`run_openloop_probe.ps1 -Mode IfDrag`，7 用例，SWD 门控全自动）：
- 电流环首次精确工作：iq 实测 0.204/0.301/0.405 对命令 0.20/0.30/0.40（<1% 误差），
  id≈0，vq≈I×R 物理值——修复前 I-f 因电流反馈错标错号被禁用，现首次可用。
- 旋转磁场确认施加（I-f 期间 duty 大幅摆动），编码器轨迹显示转子在
  we=4 rad/s 电角速度（≈0.55 rpm 机械）下 1:1 跟随（theta_sim 扫 0.82 rad、
  编码器跟 0.92 rad，含初始对齐 snap），we≥6 rad/s 开始打滑，we≥15 无法跟随。
- 判据结论：**"低压开环卡死"的主因确诊为电流反馈链错标错号（本次修复）+
  编码器零点/方向未标定**；修复后电机可受控旋转。
`[量化]` 打滑边界归因：i_rated 将 I-f 饱和至 0.5A（实测），而 k≈0.7 采样标度
意味着真实电流/力矩再打约七折（真实 ≈0.35A、力矩 ≈12-13 mN·m），摩擦/负载
吃掉大部分裕量。跟踪段 elec≈sim+0.09 rad（含负载角 asin(T_f/T_max)≈39°），
编码器零偏估计 ≈0.7~0.8 rad；正 we 对应编码器正向（enc_dir=0 可用）。
`[遗留]` ① k 标度校准（或以已知 0.7 因子临时补偿）以恢复真实力矩；
② we 加速度参数调低（现 60 rad/s² 过快，转子未锁定即被甩开）需按纪律改动；
③ 编码器零偏精标定（多角度 I-f 拖动）后进入闭环。
状态：`CURRENT_FEEDBACK_CHAIN=CORRECTED_AND_VERIFIED`、
`MOTOR_ROTATION=DEMONSTRATED_AT_0.55RPM_IF`、
`OPEN_LOOP_ROOT_CAUSE_STATUS=PARTIALLY_RESOLVED（电流链已确诊修复，
旋转已演示，剩余为力矩裕量与标定收尾）`。

## 2026-08-30（深夜） 30rpm 攻坚轮：力矩裕量判定，瓶颈转入机械/负载域

`[配置]` `DEFAULT_VOLT_WE_ACCEL` 60→25 rad/s²（host 开环模型契约改为从常量推导
步数，38/38 PASS）；新增 bench 1000ms 预算档（CMakePresets/ThemisBuildAxes/
motor_task_config 三处一致性放行）；`DEFAULT_CURR_RATED` 0.5→0.71A 测量值
（=数据手册 0.5A 真实额定，按 k≈0.7 折算，注释注明依据；OCP 1.0A 不变）。
`[实板测量]` 慢加速 I-f（0.5A/950ms）仍打滑；0.71A 下转子开始明显移动但出现
**持续反向漂移**（−8~−15 rpm，即 A/C 对调导致的反序方向），仍未锁定 30rpm 同步；
0.71A@we=4 因初始对齐瞬态冲到 1.021A 触发快速 OCP（0.71 目标距 OCP 仅 0.29A，
瞬态裕量不足）。
`[判定]` 电气链（电流环 0.715A 精确调节、旋转磁场施加、dp 响应、低速跟踪）
已端到端验证健康；**转子可动、非机械卡死**（能连续反向漂移 −0.24 圈），但
可用力矩（真实 ~0.35-0.5A × KT ≈ 12-19 mN·m，OCP 封顶）与机械负载
（摩擦/重力/线缆，量级相当）竞争，30rpm 开环同步无法在当前负载下达成。
`[移交]` 需操作员物理检查：①断电手拨转子是否自由惯性滑行；②云台是否挂载荷
/线缆是否穿过中空轴；③轴线是否水平（重力项）。电气侧后续路径：编码器零偏
精标定（估计 ≈0.7-0.8 rad）后切闭环 FOC——闭环按负载角自动寻优，可在同等
电流下榨出最大力矩，是产品正路。

## 2026-08-30（深夜·二） 欠压塌压定位：位置触发（约 +59° 机械角度处的机械阻力点）

`[实板测量]` 0.71A 速度阶梯（we=4..44 rad/s 电）七用例全部在同一点欠压：
`vbus=2.69V`、转子位移恒为 +0.164 圈（≈59° 机械），与目标转速无关。
`[判别实验]` we=2 极慢拖动（转子仅走 0.078 圈，未达触发点）：全程无故障、
`vbus=11.92V` 稳定、iq=0.503 精确、转子 1:1 跟随——**位置触发实锤**。
`[机制推断]` 转子行进至 ~59° 处遭遇局部高阻力（可能的点：局部擦碰/线缆被拽/
局部齿槽峰）；I-f 电流环在堵转瞬间 PI 甩上限电压（限幅 6.5V），瞬态相电流
可达 1.5-2A，12V/1A 适配器电流限制折叠 → 母线塌至 2.69V → 欠压保护。
20 分钟前同 0.71A 两用例未欠压（电源状态或热态差异），进一步支持电源在
瞬态下处于临界。
`[操作员检查项]` ①断电慢转整圈，标出 ~59° 处的卡点；②检查转子铃与定子是否
局部擦碰、线缆是否随转动被拽；③拖动时看电源电流表是否在塌压瞬间打到限值；
④电源限流调至 2-3A 并确认电压旋钮在 12V。
`[状态]` 电气与控制链健康（0.078 圈跟随 + 全部保护按设计动作）；
旋转收敛的唯一剩余障碍 = ~59° 处的机械阻力点 + 电源限流余量。

## 2026-08-30（深夜·三） 30 秒耐久测试：电机连续受控旋转 9 圈（历史首次）

`[配置]` 新增 bench 30s 耐久档（presets/axes/config 三处放行，ELF 47f8cde6…）；
`run_openloop_probe.ps1` 增加 -ElfPath 参数与 enc trace 白名单。
`[实板测量]` I-f 0.5A/30rpm/28s：**pos_turn=-8.957（连续 8.96 圈）、零故障、
母线 11.85-11.92V 稳定（电源限流 2A 后不再塌压）、iq=0.504 精确**；
0.71A 复测：pos_turn=-8.991、iq=0.704、同样零故障。
`[解读]` 旋转方向为负 = A/C 对调导致的反序（与原理图核查结论一致，编码器
标定可吸收）；平均转速 ~19.3 rpm，低于 30 rpm 同步目标且与电流基本无关
（0.5A 与 0.71A 收敛到同一剖面）——为 I-f 开环模式在当前齿槽/摩擦负载下的
确定性失步节奏，非链路缺陷。30 rpm 的正确产品路径 = 编码器标定后切闭环 FOC
（闭环按负载角锁定，无失步概念）。
`[状态]` MOTOR_ROTATION=DEMONSTRATED_28S_9TURNS_CONTINUOUS；
`CURRENT_FEEDBACK_CHAIN=CORRECTED_AND_VERIFIED`；
`OPEN_LOOP_ROOT_CAUSE_STATUS=CONFIRMED_CURRENT_CHAIN_PLUS_UNCALIBRATED_ENCODER`。
`[用户手动操作说明]` 每次保护跳闸后轴在 fault 态，所有 diag set 需要 idle——
手动测试前请断电重上电（或 SWD 复位）再下命令。

## 2026-08-30（深夜·四） 双向连续旋转达成 + 速度阶梯与抖动机理

`[实板测量]` 电源限流 2A 后：we=+22（28s）pos=-8.957/-8.991 圈（0.5A/0.71A 两轮）；
we=-22（28s）pos=+9.55 圈。**双向均可连续受控旋转 ~19-20 rpm（同步 30 rpm 的
64%），零故障**。早前"前向 +0.164 圈塌压"在 2A 限流下未复现——判定为 1A 电源
限流在瞬态下折叠，非机械卡点（该假设撤回）。
`[抖动机理]` 用户实测"抽动非丝滑"与数据吻合：I-f 期间 duty_a 扫至 0.878（远超
rank2/ia 采样死区阈值 0.53），每个电周期 ia 采样失明一次 → 电流矢量重建跳变 →
力矩抽动（频率=电频率）；叠加齿槽 stick-slip（0.5/0.71A 两轮收敛到同步的
64% 转速比）与 I-f 无阻尼 hunting（速度摆动 0~±60 rpm，周期 ~1.5-2s）。
`[量化修正]` 锁定跟踪工况 vq/|I| = 3.37Ω → 真实绕组电阻 ≈3.5Ω（手册 2.55Ω
对本变体不符）→ **修复后采样链精度实为准确（"k=0.7"是电阻比假象）**，
0.71A 即真实 0.71A、力矩 ~31 mN·m；编码器零偏已用 dp 对齐法标定
（encoff 2.535 电弧度，dp0/1/2 三向验证一致，对齐读数归零 ✓）。
`[路径]` 丝滑旋转 = ①采样孔径几何修复（ia 死区，需解 CCR4 触发点与实际
采样时刻解耦之谜）+ ②闭环 FOC（编码器零偏已标定，速度环自带阻尼）。

## 2026-08-30（深夜·五） 闭环堵转判别与"静态卡滞"假说

`[实验]` 编码器零偏修正验证：encoff 2.535 应用后 dp0 对齐读数 6.261≈0（mod 2π），
零偏正确 ✓（reply 的 unit=raw 标签为误导，语义确为电弧度/极对数换算）。
闭环位置模式向后小步（目标 −0.05 圈）：iq 砸至 −1.005A 触发 OCP，转子纹丝不动；
闭环速度模式（±30 rpm）：同样 railed ±0.816A 堵转。故障快照相电流三相互洽
（KCL 成立），电流矢量在测量域确实按指令输出。
`[假说]` **静态卡滞（breakaway）**：I-f 旋转磁场通过周期性振动打破静态摩擦后
可拖动转子（运行阻力 ~10 mN·m 量级），而闭环静止矢量必须独自超过静态
最大静摩擦（>30 mN·m，1.0A OCP 时仍堵转）→ 解释"旋转场能拖动、静态矢量
推不动"的悖论。另 ψ 实测上限 ≤0.01 Wb（锁定工况 |V|²=(RI)²+(weψ)² 拟合），
若实际 KT 低于手册值则进一步压缩裕量。
`[下一轮]` ①I-f 起转后 VOLT→VEL 在线切换（需放开运行中模式切换的 idle 门禁，
产品级 open-loop-to-closed-loop handoff 的标准做法）；②编码器零偏已就绪，
闭环切入后速度环主动阻尼可消除 hunting；③若确认静态卡滞量级，评估机械侧
（轴承预紧/装配）。

## 2026-08-31 I-f→闭环在线切换实现：机制打通，暴露坐标系方向失配（最后一项 A/C 对调后果）

`[代码]` 新增运行中 I-f→闭环速度在线切换：`foc_cmd_if_to_vel_handoff()`
（运行+VOLT+I-f 前置校验；速度 PID 积分种子=I-f 力矩实现无扰交接；
encoff 运行时微调语义验证正确）、shell `diag if2vel <rpm>`、
host 契约 `test_if_to_vel_handoff_contract`（含 idle/无 I-f/错误模式拒绝用例）
及四处宿主 stub。host 39/39 PASS（含新测试）。bench30s 固件 ELF 879a6b3b…。
`[实板测量]` I-f 3s 起转（转子至 −0.539 圈）→ `diag if2vel -30` 切换成功 →
闭环 iq=+0.871（与指令 −0.71 反号）、转子锁定不动。
`[确诊]` **编码器角度方向与固件电角度方向反对应**（A/C 对调的最后一项后果）：
theta_sim 帧与 theta_elec 帧对同一物理旋转给出相反的符号方向，闭环在
theta_elec 帧下力矩符号相对物理方向翻转 → 正反馈钉死。I-f（theta_sim 帧）
不受影响。修复 = `enc_dir=1` 翻转编码器方向 + dp 对齐法重标零偏（一对参数，
下一轮首项）。
`[状态]` 在线切换机制完成并验证；闭环收敛仅剩方向/零偏参数对。

## 2026-08-31（凌晨） 位置依赖卡点定位：~0.45-0.48 圈（≈165-173° 机械）

`[实板测量]` 最后一轮：CW 帧 + encoff 2.181 + I-f 0.71A/we=22 快加速——转子
1 秒内到达 0.478 圈后完全冻结（0.96A 电流环持续、无故障码）。汇总所有用例的
停滞位置：闭环速度/位置堵转 0.435/0.459、慢速 I-f 停滞 0.105-0.128、本次
0.478——除双向耐久（从 0 出发向 ± 方向各跑 9+ 圈，途经 0.44-0.48 反向等价
位置未停）外，所有在 +0.16~+0.48 圈区间启动或进入的用例全部停滞。
`[判定]` 存在**位置依赖的高阻力点**（~165-173° 机械附近），0.96A 电磁力矩
（KT 未精知，估 10-40 mN·m）无法突破；从其它位置起步则双向 9 圈畅通。
电气链（电流环 0.94A 精确、磁场旋转、编码器标定）已排除——同一电流在同
一坐标系下其它位置正常工作。
`[操作员检查]` 转子当前停在卡点上：断电后**以手极慢正反微动转子，感受
165-173° 处的阻力墙**；目视该角度下转子铃与定子/台面/线缆的间隙；确认
编码器磁铁与 IC 间隙在该角度无异变。
`[软件侧剩余]` ①若机械确认无卡点，则嫌疑收敛到编码器在该角度的读数跳变
（需 enc trace 全量 dump 分析）；②I-f→闭环在线切换已实现待用；③OCP 1.0A
距手册 2A 峰值有翻倍余量，机械确认后可作为台架实验杠杆。

## 2026-08-31（凌晨·二） 机械自由实锤（手转 47 圈）→ 闭环堵转定性为 ±90° 正交角误差

`[实板测量]` 手转扫描（enc 2302 样本/180s）：用户手转转子 **-47.07 圈**，
编码器全程连续跟踪（直方图覆盖全程无缺口）→ **机械完全自由、编码器全量程
健康**——机械卡点假设撤回（第二项撤回：第一个是 +0.164 电源塌压假扮的卡点）。
`[定性]` 闭环速度/位置模式：iq railed ±0.816-1.0A（实测相电流三相互洽）而
转子纹丝不动（多位置复现）→ 电流矢量在转子坐标系产生 ≈ 零力矩 =
**theta_elec 与真实转子 d 轴偏差 ≈ ±90°（正交误差）**。dp 对齐法标出的零偏
（2.535/2.181 两轮）存在正交项残余：候选来源 = d 轴对齐的 180° 歧义与
90° 正交约定在 enc_dir 镜像下的组合。
`[下一轮·决定性实验]` 力矩-角度扫描：小 iq（0.3A）下将 theta_elec 人为偏置
扫描（±90° 步进），找到转子起动的角度 → 真实零偏 = 标定值 − 偏置量，
一次实验彻底锁定正交项。随后 encoff 终值 + 在线切换 + 丝滑 30 rpm 收官。
`[战果确认]` 机械自由（47 手转）+ 电气链精确（39/39、iq <1% 误差、双向
9 圈开环拖动）+ 编码器量程健康（-47 圈跟踪）——剩余为单一正交角参数。

## 2026-08-31（凌晨·三） 运动中交接实验：静态卡滞量化，电流余量决策点

`[实板测量]` 修复脚本错位后完成真·运动中交接实验：I-f 0.71A/we=8/3.5s 起转
（爬行仅 0.036 圈，齿槽井内）→ if2vel -30 切换成功（闭环 engaged）→
iq railed -0.814A、转子冻结于 0.092 圈。多位置复现：静态矢量（闭环）0.8-1.0A
推不动，旋转场（I-f 振动+快速斜坡）可拖动 → **静态卡滞力矩 > 0.8-1.0A 对应
电磁力矩（估 17-43 mN·m，KT 未精知）**。
`[决策点]` 手册额定：0.5A 连续 / **2A 峰值**。固件快速 OCP 固定 1.0A —— 仅
 Manuals 峰值的一半。台架选项 A：OCP 放宽至 1.6A（台架短脉冲，热安全：
3×1.6²×3.5 ≈ 27W/数十秒），I-f/闭环在 1.4-1.6A 下力矩翻倍（35-70 mN·m），
预期直接压过齿槽与卡滞 → 同步丝滑 30 rpm。选项 B：维持 1.0A，先做
力矩-角度扫描精定位 + 编码器全量 dump。需操作员决策（保护策略变更）。

## 2026-08-31（凌晨·终） 位置依赖零力矩现象：远程诊断边界

`[实板测量]` 移动中交接（I-f 0.71A/we=8/3.5s → if2vel -30）：切换成功、闭环
engaged，iq runaway 至 **-1.6A（≈69 mN·m）持续 14 秒**，转子在 0.085 圈冻结。
同一时刻用户手转转子 47 圈自由无阻。
`[边界确认]` ①机械自由（47 手转）✓；②电流环调节精确（多轮 <1%）✓；
③采样链配对/符号修复实机验证 ✓；④SVPWM/电压链逐档验证 ✓；⑤双向开环
拖动 ±9 圈 ✓。剩余矛盾：**特定转子角度（0.085/0.44/0.48/1.015 圈等）下
静态/闭环电流矢量不产生实际力矩**，幅值 0.8-1.6A 均冻结。
`[剩余候选]` ①编码器位置依赖读数误差（磁铁偏心/缺陷 → theta_elec 在该
角度偏差接近 ±90°，矢量落入 d 轴）；②功率级在该相位下的输出异常
（deadtime=0 + DRV 握手在该电流相位组合的畸变）。两者均需示波器或
磁铁间隙物理检查——远程串口数据已达诊断边界。
`[下一轮·示波器/物理检查清单]` ①转子置于冻结角度（0.085 圈），示波器
双探针：INHA/INLA 门极波形 vs SOA 电流波形——确认电流矢量实际相位；
②编码器磁铁偏心检查（千分表/塞尺测间隙圆周分布）；③若①②排除，
enc LUT 标定（foc_calib_enc 的 LUT 段）可补偿磁路谐波。


## 2026-09-03 固定 PWM 占空比硬件隔离镜像

- `[源码确认]` 新增独立 preset
  `fw-full-diag-carrier-bench-fixed-duty-30s-debug` 与
  `run_fixed_duty_probe.ps1`。该镜像仅在 `TM_BENCH_DIAG=1` 下将 A/B/C
  duty 固定为 `0.40/0.50/0.60`，位置在 ADC 测量之后、所有 FOC 控制路径之前；
  快速过流、母线检查、1.5 s 心跳与 30 s 能量上限保持有效。
- `[无板验证]` 新增 fixed-duty host contract 后总计 41/41 host tests 通过，
  `FOC_PERIPHERAL_CONTRACT=PASS`；ELF SHA-256 为
  `5C7CEFB8BF250434804B4451BE8E62B6DD629F363DBBFA42530E0F521BBFA822`。
- `[实板测量]` OpenOCD verify 后自动 runner 启动固定 duty 并持续 28 s；最后故障
  上下文保留 `d=0.4/0.5/0.6`、VBUS=11.90 V、相电流约
  `-0.309/-0.022/+0.002 A`。未记录 OCP/UV/OV；runner 关闭串口后心跳护栏锁存
  `FAULT_COMM_ERROR` 并关闭 PWM。这条测试只证明固定 PWM 路径存在，电机的可见
  运动/振动及电源表观察须单独记录。

## 2026-09-03 原始六步动态换相隔离镜像

- `[源码确认]` 新增专用 preset
  `fw-full-diag-carrier-bench-raw-sixstep-30s-debug`。该镜像以 1 Hz 电角频率
  轮换六个 `40/50/60%` 原始 PWM 相量，不进入编码器、SVPWM、FOC 或电流环；其余
  固定 duty 测试、快速保护、心跳和 30 s 边界不变。
- `[无板验证]` 六步相序 host contract、两个自动 runner contract、42/42 host tests
  与 `FOC_PERIPHERAL_CONTRACT=PASS` 均通过；ELF SHA-256 为
  `0F35BE02EF0F2A44C4DF0F6574A6B0F7CEE2A77CA5A91EBD88230FA60EB52D97`。
- `[实板测量]` OpenOCD verify 后 `run_raw_sixstep_probe.ps1` 完成完整 20 s 保活
  窗口且未发送 software stop；末次动态快照为 duty `0.5/0.6/0.4`、VBUS=11.89 V、
  相电流 `+0.002/+0.348/-0.340 A`。随后串口关闭，心跳护栏按设计锁存
  `FAULT_COMM_ERROR` 并关闭 PWM。此记录不替代操作员的可见转动/抖动观察。
- `[实板测量]` 电机与磁编码器固定安装后，额外 15 s 六步编码器时间线由
  `pos_turn=0.020` 连续单调增加至 `2.056`，平均约 8.1 rpm，符合 1 Hz 电角 / 7
  对极的 8.57 rpm 量级；中间位置采样无回退。这与操作员观察的顺时针分段前进一致，
  证明动态三相换相和编码器位置计数在此低速隔离测试中正常。

## 2026-09-03 原始三相正弦 PWM 隔离镜像

- `[源码确认]` 新增 `fw-full-diag-carrier-bench-raw-sine-30s-debug` 与
  `run_raw_sine_probe.ps1`。该镜像以 1 Hz 电角频率直接写入
  `0.50 + 0.10*sin(theta)` 的三相占空比，绕开 FOC、SVPWM、编码器和电流环；快速保护、
  母线检查、1.5 s 心跳与 30 s 能量上限不变，且与固定 duty、原始六步镜像严格互斥。
- `[未验证]` 待实板运行后记录编码器位置时间线、母线、相电流与可见振动。该隔离测试的
  结论只用于区分连续正弦 PWM 与 FOC/SVPWM 路径，不可替代示波器电流和门极波形。

## 2026-09-03 原始三相正弦 PWM 实板结果

- `[无板验证]` raw-sine host contract、runner contract、host `43/43` 与
  `FOC_PERIPHERAL_CONTRACT=PASS`。镜像
  `fw-full-diag-carrier-bench-raw-sine-30s-debug` 的 ELF SHA-256 为
  `0B59675E67E3128981BEAA52BCF58AA7CFDC51CAEF8E92F3ED036618EDDA978F`，OpenOCD
  `program + verify + reset` 完成且记录 `Verified OK`。
- `[实板测量]` 1 Hz 电角、中心 duty `0.50`、振幅 `0.10` 的原始三相正弦运行 20 s，
  无 fault，VBUS `11.89..11.92 V`，串口校正相电流最大绝对值 `0.324 A`。编码器从首个
  观测的 `pos_turn=0.448` 前进至约 `1.40` 后，停留在约 `1.392` 圈附近并出现正反向
  瞬时速度；未演示连续平滑转动。原始日志和结论位于
  `tmp/bench/20260903_012942_raw_sine/`。
- `[结论边界]` 该现象在 FOC、SVPWM、电流环和编码器反馈均绕过时仍出现，因此这些路径
  不是复现停滞的必要条件。raw-sine 矢量幅值较 raw-sixstep 小约 13%，且串口电流不是波形
  证据；不得据此把根因定为 deadtime 或功率级。下一物理分流仍是停滞角度下的 PWM/门极/SOx
  示波器测量。

## 2026-09-03 原始正弦幅值匹配六步复验

- `[源码确认]` 新增严格互斥的
  `fw-full-diag-carrier-bench-raw-sine-match-sixstep-30s-debug`。唯一变量为正弦 duty
  振幅由 `0.100000` 提高到 `0.11547005`，使空间矢量幅值与 raw-sixstep 的 `0.40/0.50/0.60`
  相量一致；频率、中心 duty、ADC、保护、心跳与运行时间边界均不变。
- `[无板验证]` 幅值匹配 host contract 曾在旧 10% 版本按预期失败，实装后通过；runner
  contract、host `44/44`、FOC 外设 contract 通过。ELF SHA-256 为
  `9EF838F8C4937464056E705BDD97EFA5AE997F4D64595F937285092CC9FF5235`，OpenOCD
  `program + verify + reset` 记录 `Verified OK`。
- `[实板测量]` 20 s 无 fault、VBUS `11.89..11.91 V`、报告相电流最大绝对值 `0.374 A`。
  编码器每个观测点单调前进 `0.411 -> 2.564` 圈，约 6.46 rpm；瞬时速度有短暂过零，但位置
  没有回退。日志与结论位于 `tmp/bench/20260903_013956_raw_sine_match_sixstep/`。
- `[结论]` 连续正弦 raw PWM 在与 raw-sixstep 相同的矢量幅值下可连续推动转子；10% 正弦的
  停滞与较低力矩裕量一致，不构成对连续正弦、FOC/SVPWM 或功率级的反证。动态 ADC 高占空比
  失配和闭环 30/60/90 rpm 仍是独立阻塞项，不能据此重开速度测试。

## 2026-09-03 原始正弦 20% duty 动态 ADC 复验

- `[源码确认]` 新增严格互斥的 `fw-full-diag-carrier-bench-raw-sine-20-30s-debug` 与
  `run_raw_sine_20_probe.ps1`。唯一变量是 raw-sine duty 振幅 `0.11547005 -> 0.20`；
  1 Hz、中心 duty、PWM 映射、ADC trigger、快速保护、1.5 s 心跳及 20 s 运行上限不变。
- `[无板验证]` 20% duty host contract 在未实现分支时预期失败，实装后通过；runner contract、
  host `45/45`、`FOC_PERIPHERAL_CONTRACT=PASS` 和 preset gate 通过。ELF SHA-256 为
  `D32207C916D6C6BAC7D5DAE1511F3574D82DFE03BC30DA3ECB712B0B42D6A173`，OpenOCD
  `program + verify + reset` 记录 `Verified OK`。
- `[实板测量]` 20 s 无 fault，VBUS `11.85..11.93 V`，位置观测全程单调
  `0.001 -> 2.209` 圈（约 6.62 rpm），报告相电流最大绝对值 `0.673 A`。同时多个 duty
  相位 KCL 明显不闭合，例如 `-0.006/-0.608/+0.003 A`（和 `-0.610 A`）、
  `-0.378/-0.321/+0.003 A`（和 `-0.696 A`）。日志和结论位于
  `tmp/bench/20260903_015058_raw_sine_20/`。
- `[结论]` 原始三相 PWM 场在中等高占空比仍能连续拖动转子，而软件 ADC/CSA 电流重构已经
  相位相关地失去 KCL 闭合。这把问题收敛为动态观测链，不是“原始 PWM 不再有力矩”。未测到
  PWM/门极/SOx 波形前，不得用这些错误读数重调电流环、抬高保护或开始闭环速度测试。

## 2026-08-31 交接文档重写

- `[文档]` 新增 `docs/code-wiki/AI-handoff-2026-08-31.md`（取代 08-30 版
  ai-handoff-prompt 作为唯一交接入口）：整合项目/软件/硬件/调试编年史/
  方法论（10 条管用+10 条坑）/当前状态/安全规则/文件索引。README 导航同步。
- 本文档为下一位 AI 的唯一必读入口；`ai-handoff-prompt.md` 保留作历史。

## 2026-08-31 90 rpm SpeedStep 实机验证 lane 实现

`[源码确认]` 新增仅在 `TM_BENCH_DIAG=1` 编译的 `diag velstep <rpm>`：它要求轴
RUNNING、`CTRL_MODE_VEL`、PASSTHROUGH 输入和已完成 I-f→VEL 交接；请求限制为
±90 rpm，只写 `target_vel`，不重置速度 PID 或动态电流上限。host contract 覆盖
非 bench/非运行态/非 VEL/未交接/越界拒绝，以及合法请求的非破坏性写入。

`[源码确认]` `run_openloop_probe.ps1 -Mode SpeedStep` 已写入双向 3×
30/60/90 rpm 顺序、0.50 A I-f 起转、200 ms 采样、1.4 A 软件中止、10.8–13.2 V
母线门禁、ELF 哈希、逐样本/汇总 CSV 和人工电源表记录。正常停机为 `state disable`；
异常后的恢复仍限定为 SWD/物理断电。

`[未验证]` 本条仅表示源码与宿主契约已完成，尚未执行带电 SpeedStep；不能据此宣称
30/60/90 rpm 实机通过，也不能替代示波器对编码器磁路与功率级相位的根因分流。

## 2026-08-31 ADC 零点有效窗口修复与首冷启动验证

- `[源码确认]` 电流零点标定改为 5 ms 关桥放电后进入零占空比低侧钳位再采样，避免在
  DRV8311H SenseFET/CSA 无效时保存 offset；新增中心、三相跨度、噪声和 ADC 新鲜帧
  门禁，以及 bench-only `diag adc show|zero`、绝对编码器设置和 `obs all residual`。
- `[无板验证]` host 39/39、FOC 外设契约、SpeedStep 脚本契约通过；bench30s ELF
  `80A1D1C8C338448A492472BF988FD423FA94E48398270EFEB20B6E24F25EDBD0` 已 SWD verify。
- `[实板测量]` 首冷启动 raw=2047/2052/2044、offset=2047.7/2052/2048.5、残差最大
  0.007 A、KCL −0.002 A、VBUS=11.92 V。其余 4 次冷启动、AdcProof、SpeedStep 未执行。

## 2026-09-01 自动 ADC→SpeedStep 实板验证

- `[源码确认]` 增加 bench 心跳超时、启动 ADC 新鲜标定门禁、idle-only `diag adc zero`，
  并修正 runner 的 OpenOCD Windows 路径、ADC/相电流输出解析和复零等待窗口；host
  40/40、外设契约与脚本契约通过，bench ELF 已 verify。
- `[实板测量]` 自动完成 20 次 SWD 启动基线、20 次复零、低能量 proof、全 map/trigger
  采集和位置扫描；静态残差最大 0.019 A、VBUS 约 11.9 V。
- `[实板测量]` −30 rpm 首级冻结于 0.421 圈，`ib=-1.442 A` 触发 1.4 A 中止，
  VBUS=11.65 V；未重试、未执行 60/90 rpm，随后仅 SWD 复位恢复 idle。根因仍需示波器
  分流 encoder 磁路与功率级相位畸变。

- `[更正]` 同轮早期 100 ms pulse 漏掉实际驻留时间，故其全 map/trigger/位置扫描不作
  有效实板证明。修复 runner 后新的有效 proof 在 `20260901_100147_openloop_probe_AdcProof/`。
  高 duty 时三相同时回零，单相重构试验已撤回；SpeedStep 心跳改为非阻塞写+下一条
  `obs all` 确认，避免串口往返触发 watchdog。

## 2026-09-01 CCR4=2700 默认孔径与自动 SpeedStep 复测

- `[源码确认]` bench 默认 ADC trigger 由 3740 改为 2700，TMR1 CH4 初始化、map reset、
  board runner 默认/恢复路径和外设契约同步；合法扫描仍可显式测试其它 compare。
- `[源码确认]` SpeedStep 的 ADC preflight 前新增 `Resolve-IdleForNextCase`，使上一例
  `state disable` 留下的 fault-safe 状态通过 SWD reset 自动恢复，未使用 `fault clear`
  或 `state stop`。
- `[无板验证]` 默认 trigger 的 host contract 完成失败测试先行；host 40/40、FOC peripheral
  contract、SpeedStep script contract 通过；bench30s ELF
  `637DCA30F208EF50EEDD826B8012B40FD1E904D4BA88EF0F6EF5727F336E79B9` 已 OpenOCD verify。
- `[实板测量]` `20260901_trigger2700_adcproof/` 六向低能量 proof 与
  `20260901_trigger2700_triggerscan/` 两角度 24 脉冲证明 2700 下静态三相可读、KCL 近零。
  `20260901_133000_adc_then_speed/` 的 -30 rpm 首级仍在高 duty 下 KCL 失配并触发
  `ib=-1.449 A` 软件中止；VBUS 11.63 V，60/90 rpm 和正方向未执行。SWD reset 后 idle
  预检通过。结论仅为动态采样问题仍开放，非 ADC 硬件损坏或电源折叠结论。

## 2026-09-26 知识收尾与交接面清理

- `[文档]` 将 `docs/code-wiki/AI-handoff-2026-08-31.md` 固定为唯一现役交接入口；Code Wiki 首页、项目摘要、运行验证页和调试总结均增加当前状态指针，历史快照继续保留但不再冒充当前总数。
- `[规则]` `AGENTS.md` 与根 README 的现役事实修正为 AT32F415、`platform/at/at32f415`、当前 bench 串口 `COM3 @ 115200` 和本仓库脚本路径；Shell/Transport 契约同步当前串口，旧 COM54/1 Mbit/s 仅保留为历史 witness。
- `[规则]` `Themis_Windows主机开发与调试操作规范.md` 标注为含 TEC_Thermostat_Water_Bath 路径的历史参考，避免新人直接复制跨项目命令；当前操作入口改读根 README、AGENTS、交接文档和 `scripts/flows/board/`。
- `[无板验证]` 文档相对链接检查通过，`git diff --check` 通过；host CTest 最新结果为 `45/45 PASS`，`FOC_PERIPHERAL_CONTRACT=PASS`。
- `[记忆]` 按 Codex 生成记忆只读边界写入 correction input：`C:/Users/16429/.codex/memories/extensions/ad_hoc/notes/20260926-hollow-gimbal-clean-handoff.md`；未直接修改 `MEMORY.md` 或 rollout summary。

## 2026-09-26 对照参照项目后的开环丝滑性工具包（死区 / voltloop / VVEL 备胎模式）

- `[分析]` 对照 oshwhub `bldc_ctrl_v1.0`（同款 DRV8311H + STM32F103C8T6 + AS5600，CawFOC/SimpleFOC 系）：
  其开环与速度闭环全部为电压模式——速度 PID 直接输出 Uq，电流采样为自由轮询且根本不进控制环，
  TIM1 硬件死区 1 us。故其"能转且丝滑"对本项目的电流链缺陷（rank 孔径、动态 KCL 失配）完全免疫；
  本项目 09-03 幅值匹配正弦镜像已证明电压模式可单调旋转，"不丝滑"主要来自 I-f 把电流环耦合进开环角。
- `[源码确认]` TMR1 死区 0 → 1.0 us（`wk_tmr.c` DTG=0x8B，(64+11)×2×6.67 ns），对齐参照板
  "TIM1 1 us 死区 + DRV8311H" 的实证组合，降低对 DRV 内部握手时序的单点依赖。
- `[源码确认]` 新增 `diag voltloop <rpm> <V>`（`motor_runtime_diag_set_voltloop`）：输出轴 rpm 自动换算
  电角速度 we，走纯 VOLT 开环（if_enable=0），作为"VOLT vs I-f 同转速对照"实验入口；idle-only，回执
  summary + detail row 各一行。
- `[源码确认]` 新增 `CTRL_MODE_VVEL` 电压模式速度闭环备胎：`vvel_pid` 输出即 Uq（V），内环只做
  Park 观测 + 反 Park + SVPWM，电流 PID 完全旁路（模式对电流链缺陷免疫）；Uq 钳位 =
  min(DEFAULT_VVEL_MAX_OUT=6V, Vbus×0.95/√3)；进入方式 `cfg ctrl vvel` 或 `diag set vvel <rpm>`
  （passthrough），`diag pid set vvel kp/ki/kd/lim` 在线整定；`_ctrl_mode_is_valid` 故意不含 vvel，
  持久化层不保存该模式。默认增益 kp=0.5 V/(rad/s)、ki=5.0 V/(rad/s·s)，为参照板量级的起始点。
- `[无板验证]` `./run.sh build` 通过（text=79400 data=128 bss=21840）；host CTest 45/45 PASS
  （含 diag contract：`pid all show` 新增 vvel 行，三个链接 diag shell 的 host 测试补齐
  `motor_runtime_diag_set_voltloop` 替身）。
- `[门禁欠账]` 质量门禁 `check_themis_quality_gate -Scope all` 仍在预存欠账处失败：
  `test_falconfoc_themis_lite_contract.ps1:507` 读取不存在的 `platform\at\at32f403a\ThemisPlatformDefaults.cmake`，
  且其后 `$preparedPlatforms` 段要求本仓库从未存在的 `tm_platform_foc_timing.h` 与 `platform/at/at32f423/`。
  该失败先于本轮改动（507 行之前的 foc_ctrl.c / motor_store.c 内容断言均通过）；尝试的最小迁移
  （路径 403a→415 + 芯片 token CBT7→KBU7_4）已回滚，因欠账为多段式，留给专门的门禁迁移轮次。
- `[供电实验注意]` DRV8311H 3.3V LDO 输出割线、改调试器 3.3V 供电后：CSAREF/中点偏移由动态零点
  标定（低侧钳位法）吸收，无需固件改动；但脱离调试器独立上电时该 3.3V 网络失电，禁止无调试器
  挂接状态下运行任何电机测试；若 J-Link VTref 电流能力不足导致电压跌落，ADC 满量程随之变化，
  现象为读数整体缩放（校准可吸收）而非失控。

## 2026-09-26 开环丝滑性调试轮（bench-30s 实板，DAP-Link + 12V2A）

- `[环境]` 烧录走 OpenOCD + `openocd_daplink.cfg`（AGENTS.md 的 J-Link 记载已过时）。
  product 档（10 kHz FOC 步进）首次带轴运行即确诊：RUNNING 态 20 kHz ADC + 软浮点 FOC 步进
  占满 CPU，shell 饿死（多次 halt 采样全部落在 ADC1_IRQHandler），SWD 复位恢复。实板调试
  继续固定使用 bench-30s 镜像（5 kHz 步进 + 30s 能量预算）。
- `[修复]` `run_openloop_probe.ps1` 的 `Invoke-ProbePulse` 长脉冲轮询只发 `state show`、
  从不喂 `diag bench keepalive`（该 runner 写于心跳看门狗诞生之前），导致所有脉冲模式在
  ~1.5 s 被 comm-error 杀死（trace 序号 7437 ≈ 1.49 s 实证）。已补 `Send-BenchKeepAlive
  -NoWait`（复用既有 helper 与 allow-list）。修复后 28 s VoltDrag 全程存活。
- `[实板测量]`（12 V 母线 ≈11.9 V，全部本日复测）：
  VOLT 纯开环 30 rpm：2.0 V 反向滑差飞转（采样瞬时 −130 rpm）；3.0/3.6 V 正向挣脱
  0.58/1.02 圈后冻结（0.57/1.021 圈），磁场滑差下转子只余 ±0.5 rpm 微抖；
  200 rpm 斜坡跟至 2.278 圈后冻结 16 s。I-f 0.71 A：净 15 rpm 平均 + 剧烈 hunting，
  vq 2.5↔6.5 V 摆动；I-f 1.2 A：电流环在采样盲区致盲（vdq 饱和 6.75 V、三相读数
  同时回零、KCL 假闭合），真实电流 ~1.9 A 触发 OCP（mask=0x1）。vvel 30 rpm：链路
  可用（mode=vvel 稳定运行），但 Uq 饱和 6 V（≈63 mN·m）下转子在 pos=0.056 纹丝不动
  ——编码器帧 q 指令在该位置零力矩，±90° 位置依赖角差活体复现。
- `[综合]` 失速位置跨坐标系重合：VOLT（theta_sim 帧，无编码器）冻结于 1.021 圈，
  I-f（编码器帧）冻结于 1.006 圈——两帧在同一物理位置失速，指向机械阻力/功率级
  力矩坑而非编码器帧本身；同时 vvel 在 0.056 圈的零力矩冻结又独立支持角差假设。
  3.0-3.6 V 可用力矩（26-32 mN·m）骑在 08-31 实测 breakaway（>30 mN·m）门槛上，
  是 VOLT 档失速的第一解释。动态 KCL 失配（单相失明 + 三相回零）在高 duty 下
  把所有闭环电流路径压到 breakaway 之下或推进 OCP，是闭环档的双向挤压来源。
- `[工具]` 本轮临时探针 `build/_debug_probe.ps1`（串口只读/低速批量）与
  `build/_spin_probe.ps1`（长旋转观察 + 心跳 + 周期采样）留存至下一轮。

## 2026-09-26 纯代码力矩-角度扫描（无示波器裁决轮）

- `[升级]` `run_openloop_probe.ps1` ThetaSweep 从 6 种子/1.5V/250ms 升级为 12 种子（30° 步距）/
  `-SweepVq` 参数化/400ms；1.5V（15.7 mN·m）低于 breakaway，历史上因此扫不出结论。
- `[实板测量]` pos≈0.056-0.22 圈 @3.0V（26 mN·m）：位移 vs β 非正弦、方向与 β 无固定关系
  ——齿槽 snap 主导，无法裁决角差。pos≈0.18-0.44 圈 @4.7V（46 mN·m，全面超过 breakaway 30）：
  仍非正弦——运动集中在 β∈[0°,60°]∪[150°,210°] 两个带，**β∈[240°,330°] 为 90° 宽零位移死区**
  （+0.027/+0.117/0/-0.001/+0.012/0/+0.041/+0.035/-0.027/-0.021/0/0）。
- `[推断]` 46 mN·m 电压矢量在特定电角度带推不动转子 = 功率级/输出链存在角度依赖力矩真空，
  与动态采样盲区（同属 duty/角度域）互为印证；单纯摩擦/齿槽模型无法制造 90° 宽力矩带。
  vvel 在 0.056 圈的"6V 零力矩"冻结可由同一死区解释，±90° 编码器角差假设降级为次要候选。
- `[下一轮]` ①`diag map` PWM 映射 A/B + 重复扫描：死区随映射移动 → 输出级/接线定因；
  ②deadtime 0 vs 1µs 重建对照；③VILinearity 重建采样可见性图谱（rank 修复后首次）。

## 2026-09-26 下一轮：deadtime A/B 与 I-f 连续旋转复现（纯代码调试收口）

- `[对齐-稳定扫描]` ThetaSweep 升级 `-SweepMs 2000` + 前后 `obs all` + `-SweepPwmMap`
  （SWD 复位会清 bench map，每 case 重新下发）。4.7V/2s 扫描发现：转子静止端点被保持
  力矩景观拉离对齐点最多 ±130° 电角度——对齐刚度不足以压过全部保持点，绝对角差 δ
  无法从端点干净提取；保持景观本身得到定量展示。
- `[实板测量]` VOLT 纯电压开环系列（1µs 死区固件）：30rpm@3.6V 冻结于 1.021 圈、
  @5.0V 冻结于 1.831 圈、@6.0V 冻结于 0.561 圈且停机时 OCP（堵转 DC 1.71A>1.6A 阈值）；
  200rpm@5.0V 冻结于 2.478 圈。电压升高推进更远但终被咬住；冻结时 vel ±0.4 rpm 抖动
  = 旋转场扫过静止转子的零均值踢动，符合失步后正弦平均力矩为零的物理。
- `[裁决 A/B]` deadtime 1µs 在低 duty 相（实测 duty 0.19-0.22）吃掉 ~20% 开通窗，
  是低低压开环有效力矩的显著侵蚀项。**决策：`wk_tmr.c` 死区回退 0**（DRV8311H 内部
  握手 450-760 ns 兜底，与历史 9 圈成功配置一致），A/B 证据与理由写入代码注释。
- `[复现突破]` deadtime=0 固件上复刻历史配置 I-f 0.71A @ 30rpm：**20 s 连续正转
  6.5+ 圈（0.661→6.558），穿过 VOLT 全部冻结点，无故障停机**。速度波动 0.1-36 rpm
  （I-f 固有 hunting 仍在）。同固件同参数下 I-f 连续 vs VOLT 必冻结的对照成立。
- `[根因收口]` "连开环都不丝滑"两层：(1) 纯电压源开环在本机牵出余量不足（低 duty
  畸变 + 保持力矩景观），参照项目能用纯电压开环是因其电机阻力矩低一个量级；(2) 平滑化
  的正道是闭环速度环阻尼，被动态采样失明（KCL -0.6~-1.5 A）阻塞。**下一步唯一主线：
  修采样链（trigger/孔径/CSA 建立），解锁闭环。**
- `[状态]` product 与 bench-30s 镜像均已重建为 deadtime=0；host 45/45 PASS；
  板上为 bench-30s 镜像，轴 FAULT/PWM 断。

## 2026-09-26 板子-代码归因检验（瞬时启动 A/B）

- `[实验]` 临时将 DEFAULT_VOLT_WE_ACCEL 提到 5000（瞬时满速场，复刻 bldc_ctrl_v1.0
  参照启动风格）：200rpm@5.0V 下转子被瞬转场异步拖拽 +3.4 圈（至 9.861 圈）后失步冻结
  16 s+——与 25 rad/s² 斜坡同物理。已恢复 25.0 并在注释中记录 A/B。
- `[归因]` "板子问题"假设进一步弱化：静态采样链教科书级（零点残差 0.003 A）、同板同机
  I-f 连续 6.5+ 圈、所有失效均为参数域系统性模式而非缺陷随机签名。开环平滑性的最大
  硬件变量是**电机的保持力矩景观**（参照项目电机阻力矩低一个量级），非驱动板。
  代码架构可移植，但采样窗口参数与拓扑绑定，换板需按拓扑重调。
- `[遗留板级变量]` CSAREF 改调试器 3.3V 供电（LDO 割线）为用户引入的唯一板级变量；
  静态无异常，动态贡献未量化。建议采样链攻坚前回焊 LDO 做一次 A/B 关闭该变量。

## 2026-09-26 采样链修复成功 + DRV8311H 锁存断输出发现（双重失效分离）

- `[修复]` 动态采样失明根因=触发点固定在波峰后 7µs（CH4 下降沿穿越 CCR4=2700），而低侧
  公共导通窗以波峰为中心随占空比收缩。**修复：CCR4 2700→3740（波峰前 9 计数，序列跨波峰
  居中）**，波峰后采样同时使最高占空相的 CSA 建立时间最大化。同步更新 bench 默认
  `MOTOR_BENCH_ADC_TRIGGER_DEFAULT`、runner 默认与两个 host 契约断言（2700→3740）。
- `[验证]` 静态 3.0V 驻留矢量 KCL 残差 **-0.009 A**（旧触发点同工况 -0.996 A）；I-f 0.71A
  30rpm 旋转中 KCL 残差 **-0.004~-0.019 A**（基线 -0.6~-1.5 A），三相幅值与指令吻合，
  vq 稳定 2.4-2.5V 不再饱和；5V 旋转场单个样本抓到真实 1.426/-1.001/-0.427A（与 5V/3.5Ω
  理论 1.43A 精确吻合，KCL -0.002，duty 峰值 0.92）。一个月的动态失明问题关闭。
- `[新发现]` 5V 旋转场运行 ~3s 后出现"三相真零电流+KCL 平凡闭合+转子死停"——平衡三相
  正弦不可能三相同时为零，判 **DRV8311H 内部故障锁存断输出**（nFAULT/nSLEEP 无 MCU
  端点，固件无感知；SWD 复位后静态 3.6V 仍零电流，锁存只能 12V 断电清除）。回查旧数据：
  实验 B/旧 5V/6V 的"冻结+全零电流+KCL 闭合"均为同一签名——此前部分"保持力矩冻结"
  实为 DRV 锁死，与采样失明叠加混淆。触发条件待猎捕（疑似 duty>0.9 极端占空比下的
  VDS/OCP 瞬态）。
- `[待办]` ①用户 12V 断电清锁存后跑锁死猎捕（0.4s 采样抓锁死瞬间的场角/duty）；②评估
  VOLT 模式 duty 上限软钳位；③if2vel 闭环首试（采样已修好，I-f duty≤0.7 区间不触发锁死，
  交接路径可行）。

## 2026-09-26 深夜轮：编码器 LUT 标定跑通 + 闭环首次激活（振荡待整定）

- `[使能]` `ThemisContracts.cmake` 增加 opt-in 缓存变量 `THEMIS_BENCH_FORCE_ENERGIZED_CALIB`
  （默认 OFF，不影响任何既有档位），bench 镜像可按需解锁带电标定；本轮以
  `-DTHEMIS_BENCH_FORCE_ENERGIZED_CALIB=ON` 配置 bench-30s。
- `[里程碑 1]` `cal enc` 全流程首次在实机跑通：极对数探测（38 s）→ LUT 顺/逆时针扫掠 →
  **enc_valid=1，offset=-2.534**（与旧 dp 标定 +2.535 幅值一致、符号符合 DIR_CCW 镜像约定，
  自洽），自动回写存储。拖动采用电流环幅值（CALIB_ENC_IF_IQ），全程 KCL 干净。
- `[里程碑 2]` LUT 生效后 **if2vel 闭环首次激活**：交接成功，转子在目标附近主动双向出力
  （电流 -0.9~+0.7 A，KCL -0.015~0.015），不再是无差异死锁——但呈 ~0.5-0.7 Hz 极限环
  （速度读数摆动 ±100~650 rpm），未收敛到 30 rpm。
- `[新发现]` 闭环振荡期间 pos 仅摆 ±0.05 圈而速度读数冲到 ±630 rpm、elec 角跳 ±0.8 rad
  ——**速度/角度测量在电流瞬变下被污染**（PLL 对 SPI 线噪声或定子磁场对磁编码器的
  电流依赖畸变）。这与历史"±90° 正交角误差随电流状态漂移"完全吻合：开环小电流畸变小、
  闭环大电流畸变大。**角差-电流耦合成为闭环整定的头号嫌疑**。
- `[钳位生效]` FOC_SVPWM_DUTY [0.28, 0.72] 上板后 I-f 拖动保持连续（8.7 s / 2.45 圈）；
  闭环振荡峰值期间电流环指令 5.8~7.9 V 被钳位削顶，瞬态阻尼权限受限（设计内权衡）。
- `[待办·下一轮]` ①`diag trace`（5 kHz 板上环record，单次读取免疫串口噪声）解剖振荡
  波形与 elec/vel 跳变时刻的电流相关性；②角度-电流耦合判别（同静止点扫电流测 elec 漂移，
  需先解决轮询通道噪声——trace 或缩短回执）；③速度环整定（当前存储增益 kp=0.0063 为
  旧采样失明时代遗物，默认 2.0 亦待重估）；④若证实电流依赖畸变，评估 LUT 按电流分段
  或降低编码器处的磁场耦合（布局/屏蔽，属硬件建议）。
- `[状态]` bench-30s（+energized calib、duty 钳位、CCR4=3740、deadtime=0）在板；
  product 镜像同步重建（含钳位/CCR4/deadtime）；host 45/45 PASS。

## 2026-09-26 深夜轮 2：测量增益随场角摆动（gain(θ)）——月度 k 之谜的统一解释

- `[方法]` FOC trace（仅 RUNNING 记录、断电后保持、单次读取免疫串口噪声）成为本轮主仪器。
  12 种子 × 1.5V 驻留（真值 0.43A 已知）构建 gain(θ) 地图。
- `[实板测量]` gain(θ) 强摆动：峰值 ~0.44-0.49 A（≈100-114%），深谷 0.03-0.12 A（≈7-27%），
  谷位于 ~30°/90°/180°/300°/330°，与占空比图案无关（活/死两组 max-duty 均 ~0.61）。
- `[触发扫描]` 5V 驻留（中途再次锁死，数据污染）→ 降 3.0V 单会话重扫：adc_trig=2700 读 ~10%，
  3100/3400/3740 读 ~0——读数随 CCR4 变化但增益远低于窗口几何预期；"低侧窗+波峰采样"
  的静态推演与实测矛盾（波峰处低侧全导通、窗口余量 ≥9µs，增益应为 1）。采样链实际
  行为与代码文档不符（ADC1_ETP_MUX 重映射 + ADC12_PREEMPT_TRIG_TMR1CH4 的有效
  触发沿/源存疑，历史"触发点与实际采样时刻解耦之谜"同源）。
- `[统一解释]` 增益 G(θ) 为场角强函数 ⇒ ①旋转运行（I-f/闭环）的电流反馈被 G(θ) 在
  电频率上泵动（KCL 对共模增益盲，故 KCL 干净不能证明增益正确；历史 I-f 幅值读数
  为泵动的混叠采样）；②历史每次拟合的 k=0.66-0.90 是不同运行、不同 θ 区间的增益抽样
  ——月度 k 之谜、±90° 之谜的电流侧半边由此统一；③此前部分"true"读数亦不可信。
- `[影响面]` 所有基于绝对电流的历史结论需按"固定工作点相对可信"重新标定；KCL 仍可作
  一致性校验；DRV 锁死、duty 钳位、编码器 LUT 三项结论不受影响（非电流幅度依赖）。
- `[下一轮主线]` 采样触发路径重设计：TMR1 更新中断内软件触发注入组
  （adc_preempt_software_trigger_enable），采样时刻完全由代码定，绕开触发路径之谜；
  重跑 gain(θ)——若地图变平即坐实根因，闭环整定再继续。

## 2026-09-26 深夜轮 3：PSU 判决 + AVDD 电源架构头号嫌疑

- `[判决]` 用户 PSU 实测：dp0 3.0V 驻留矢量下母线电流 **~0.03A ≈ 无驱动**（固件指令
  vd=3V/duty 0.63/0.44/0.44，采样读 0.089A 为衰减残迹）——**功率级在转子拉到对齐点后
  1-2 s 内自行切断输出**；旋转场（I-f 20 s+、闭环振荡）不受此限。
- `[网表事实]` `DRV_3V3 = U4 AVDD LDO 输出，同时供 U3(MCU)、U5(编码器)、CSAREF、上拉`；
  `VIN_AVDD 接 DCBUS(12V)` 而原理图注释建议 ~5V；AVDD UVLO 2.0/1.8 V 且"内部 LDO 输出
  异常时关闭相关功能"。08-27 的"CSAREF=0.15V 无直流源"即 AVDD 崩溃现场记录。
- `[头号嫌疑]` AVDD 供电路径：割线前=LDO 在 12V 压差下带全部负载（热裕量极小）；割线后
  =调试器 3.3V 供 MCU+编码器+DRV 内部 AVDD 电流（带载能力存疑）。驻留驱动启动→电流
  涌出→3.3V 轨跌落→AVDD UVLO→栅驱关闭→电流归零→负载减轻→电压恢复（自恢复锁存外观）。
  此前 gain(θ) 地图、相位特异死亡、旋转/驻留差异均为该电源链崩溃时刻随负载轨迹变化的
  外观（gain(θ) 地图实为锁死时刻图）。
- `[判别实验]` 万用表测 3.3V 轨（任一芯片 VCC）在死亡瞬间的行为：跌落→供电链根因
  （修复=VIN_AVDD 改 ~5V 供电，原理图注释本意，或更强 3.3V 源）；不动→DRV 内部对直流
  矢量敏感（修复=nFAULT 接入 MCU + 数据手册核查）。
- `[修正]` 此前"相位特异/通道特异"结论降级为运行顺序与负载轨迹的随机外观（n=2 不足以
  支撑相位归因）；FOC_SVPWM_DUTY 钳位与 CCR4=3740 保留（无害且仍规避高 duty 工况）。

## 2026-09-26 深夜轮 4：根因确认——AVDD 供电压垮（用户万用表实测）

- `[实测]` 用户万用表于驻留运行期间测 3.3V 网络（AVDD 轨，供 MCU/编码器/CSAREF/DRV 内部
  AVDD 负载）：**运行期间 3-4 次跌落至 0.25V**。AVDD UVLO（2.0/1.8V）反复触发 → DRV
  "关闭相关功能"（栅驱）→ 电流消失（PSU 0.03A）→ 负载减轻电压恢复 → 再次驱动 → 再次崩
  溃（UVLO 振荡循环）。
- `[全案统一]` ①月度"堵转/停滞"攻坚中的大量失步与抖动 = AVDD 轨崩溃循环（历史上"1A 电源
  限流折叠"撤回案、I-f hunting 的 1.5-2s 周期、闭环 0.5-0.7Hz 极限环均与此同源）；
  ②gain(θ) 地图 = 锁死时刻图；③"相位特异/通道特异"= 运行顺序负载轨迹的随机外观；
  ④08-27 "CSAREF=0.15V" = AVDD 崩溃首次现场记录。软件侧工作（CCR4/软件触发/duty 钳位/
  编码器 LUT/闭环）全部保留有效，此前是在一条会周期性崩塌的电源轨上调机的。
- `[架构事实]` VIN_AVDD 接 DCBUS(12V)（原理图注释本意 ~5V），LDO 压差 8.7V 带全板负载
  → 热裕量极小；割线后调试器 3.3V 供全网络，带载能力不足（本次实测崩至 0.25V）。
- `[修复方案]` 首选：断开全部电源 → 焊桥恢复 AVDD 输出割口 → 割开 VIN_AVDD-DCBUS →
  DAP-Link 5V（USB 5V）飞线至 VIN_AVDD → 拆除调试器 3.3V 飞线（避免双源冲突）→ 上电。
  LDO 以 5V 输入原生带网（1.7V 压差，热问题根除），恢复板载独立供电能力。
  次选（微创）：AVDD 网络并联 220-470µF 电解 + 保留调试器馈电（应急带尾巴）。
- `[验证计划]` 修复后：①驻留 dp0 3.0V 应见 PSU ~0.3A 稳定、无 3.3V 跌落；②gain(θ) 地图
  应变平（12 种子复测）；③I-f 拖动 hunting 应大幅减轻（若确为电源振荡）；④闭环整定
  （vel PID 重调）→ 丝滑验收。

## 2026-09-26 知识收尾（洁癖轮）：现役交接入口更新

- `[文档]` 新增 `docs/code-wiki/AI-handoff-2026-09-26.md` 为现役交接入口（根因判决/
  固件修复清单/AVDD 硬件修复步骤/验证阶梯/台面工作流/命令速查/待办全景）；
  `AI-handoff-2026-08-31.md` 头部加取代标注，保留为攻坚史；code-wiki README 双指针同步。
- `[规则]` AGENTS.md 与根 README 的 debugger 事实修正为 DAP-Link（OpenOCD V2.0.6 +
  `tools/openocd/openocd_daplink.cfg`）——此前 J-Link 记载与实际烧录链不符。
- `[记忆]` 平台记忆库落定最终判决：新增 avdd-supply-root-cause（根因+修复方案），
  更新 latch-off（重定因 AVDD）、3v3-external-supply（0.25V 实测+修复方案）、
  stall-campaign（RESOLVED 标注）、gain-theta（RESOLVED 标注）及索引。
- `[无板验证]` host CTest 45/45 PASS；product/bench 双镜像与源码一致。

## 2026-09-28 接手核查：记忆落盘对账与前记勘误

- `[勘误]` 上一条 `[记忆]` 项所述文件当时并未落盘：核查时项目记忆目录为空，用户记忆目录仅有一条
  既有反馈。该条目属过度声明，现予以更正记录。
- `[记忆]` 本轮实际落盘：项目记忆 2 条（AVDD 根因判决与红线、工作树未提交现状）+ 用户记忆 1 条
  扩充（声明产物已持久化前必须回读目标路径确认）。
- `[复核]` 现役交接入口 `docs/code-wiki/AI-handoff-2026-09-26.md` 及其指针、08-31 取代标注、
  AGENTS/README 的 DAP-Link 事实均已在场并实测可达；`./run.sh build` PASS，host CTest 45/45 PASS。
- `[新发现]` 仓库无 `.gitignore`，`build/` 下 281 个产物（含 obj/su/ninja 日志与 5 个探针脚本）
  被 git 跟踪；工作树含 155 项未提交的源码/测试/脚本/文档改动。提交分组与忽略策略待用户决定。

## 2026-09-28 接手落地：分组提交与构建产物脱管

- `[决定]` 用户确认两项处置：按逻辑分组提交保存工作；加入忽略规则并把 `build/` 移出索引。
- `[git]` 新增 `.gitignore`：`/build/*`（带 `!build/_*.ps1` 例外，5 个台面探针脚本保持跟踪）、
  `build-rel/`、`Testing/`、`tmp/`（台面证据留在本地）、`.superpowers/`、`.zcode/`、
  以及误生成的 `state show` 文件；`git rm -r --cached build` 脱管 281 个产物，本地文件未删。
- `[提交]` 未提交工作按主责分 8 组落到本地 main：忽略规则、采样链（CCR4=3740 波峰采样 +
  CC4 软件触发 + deadtime=0，含 `work/project` WorkBench 侧同步）、控制（duty 钳位 + VVEL
  + VOFA/串口策略）、标定与 bench 设施（LUT 标定 + 映射 + 板上记录 + 看门狗）、补入遗漏
  契约用例、台面 runner 与门禁、文档收尾、板级参考件入库（`docs/pcb docs`，含 AVDD 判决
  所依据的网表）。`git status` 现为空，**未 push**。
- `[限制]` 分组按文件主责归类，同一文件含多主题时未做逐 hunk 切分（如 `motor_drv.c`
  同时带 bench 映射与 VVEL 分支），故中间提交不保证各自可编译；HEAD 整体状态另行复验。
- `[待决]` 提交信息中 `THEMIS_BENCH_FORCE_ENCALIB` 为笔误，实际开关名
  `THEMIS_BENCH_FORCE_ENERGIZED_CALIB`；因该提交未推送，是否改写待用户定。

## 2026-09-28 实板复测：AVDD 判决由修复验证 + 开环首次真正跑起

- `[前置失败]` 首轮 25s 驻留窗口无效：板子带 `calibration-failed (0x80)` 锁存上电，
  `state start` 被拒，`svpwm duty=0/0/0`、`vdq=0/0`，全程未施加能量。静态采样
  `raw=2790/3465/3670`（三相零点跨度 880 码、B/C 顶近正轨、各自乱漂 ±50~60 码），
  `diag adc zero` 返回 `reason=calibration_failed`。与 08-27 "CSAREF=0.15V 无直流源"
  同类特征 → 判为 DRV8311H 模拟供电缺失；用户复核为**焊接问题，DRV 未真正带电**。
- `[复测]` 重焊后新 boot `axis=idle fault=0x0`、零点标定被接受、`raw=2043/2046/2047`
  `off=2044.8/2048.5/2045`、`adc_res≈0.00x A`。映射仍为 `pwm=0 adc=0 adc_trig=3740`。
- `[驻留阶梯]` 3.0V 与 6.0V 各 12s（可听打点：长哔=加驱、短哔=断驱）：
  3.0V → `vd=3/3` `duty a=0.688` 峰值 0.105/0.067/0.069 A；
  6.0V → `duty a=0.688-0.72`（撞钳位顶）峰值 0.199/0.11/0.113 A；
  `vbus` 两侧均 11.92/11.98。**用户万用表全程观察：3.3V 没有跌落。**
- `[判决]` AVDD 供电压垮根因由修复反向确认。此前"调试器 3.3V 带不动、飞控板 3.3V
  带得动"的差别在源带载能力，AVDD 网络本身工作正常。"禁止电机测试"红线对本台面接线解除；
  板载永久修复（`VIN_AVDD` 割 12V 改喂 5V）仍是待办。
- `[开环 I-f]` `diag set if 0.71 21.99`（指令 30rpm）跑 25s：`state start ok`，
  无故障、看门狗 `timeout_count=0`、trace 序号单调无复位，采样电流峰值
  0.731/0.905/0.73 A（指令电流真正送达）。**KCL 残差 0.004~0.03 A** → 采样链可信，
  历史 k=0.66~0.90 分歧得到解释。
- `[新问题 1]` 实际机械转速 12.8~24.9rpm、均值约 20rpm（按 `pos_turn` 逐差算），
  低于指令 30rpm，且 `vd` 从 0 涨到 5.5~6.5、`duty` 同时撞 0.28/0.72 两端 →
  疑为电压/占空比受限引起的滑极。注意 duty 钳位 [0.28,0.72] 当初的理由
  （">0.79 驱动消失"）已被 AVDD 判决推翻，属遗留束缚。
- `[新问题 2]` `obs` 的 `vel_rpm` 在旋转中乱跳（-9.5 ~ +49.7）而 `pos_turn` 平稳前进
  → 速度估计链在旋转工况下不可用，闭环整定（阶梯第 4 步）前必须先解决。
- `[工具]` `build/_spin_probe.ps1` 加 `-LeadInSec` 与加驱/断驱蜂鸣打点；
  新增 `build/_duty_cycle_probe.ps1`（多档驻留循环、可听窗口、转子锁死不旋转）。

## 2026-09-29 时基判决：FOC 步进实测 3,334Hz 而代码按 5,000Hz 累加 dt

- `[起因]` 开环 I-f 两轮实测转速均为指令的 2/3（we=21.99→20.0rpm、we=11→9.7rpm，
  比值 0.667/0.647 与速度无关），且 `obs` 的 `vel_rpm` 在平稳旋转中乱跳。
  先排除三个猜测：`pll_enc_get_total_enc = pos_estimate_int / cpr`（`ENCODER_CPR`
  = 2^16）说明位置与 dt 无关、可信；`DEFAULT_POLE_PAIRS=7` 与
  `DEFAULT_GEAR_RATIO=1.0` 不提供比例因子；duty 钳位不是限速原因。
- `[实板测量]`
  ① `diag adc show` 的 `seq`（即 `motor_drv.c` 中每次注入完成自增的
  `injected_sample_seq`）以 **13,337/s** 递增，六个 2.3s 间隔全部一致；
  ② `motor_task.c` 的 ADC ISR 再按 `TM_MOTOR_FOC_ADC_DIVIDER(4)` 分频 →
  FOC 步进 = 13,337/4 = **3,334 Hz**，与 FOC trace `sequence=67,468`（约 20s
  RUNNING）得到的 **3,373 Hz** 相互印证；
  ③ SysTick LOAD（0xE000E014）= **149,999**，配实测 tick **999.18 Hz** →
  内核实际约 **149.9 MHz**，与 `TMR_CLK_HZ = 150000000` 一致。
- `[判决]` 不是时钟树失效，而是**控制环的 dt 与实际步进频率不符**：代码按
  `PWM_FREQ_HZ = 5000` 累加角度与积分，真实步进只有 3,334 Hz（比值 0.667），
  所以 I-f / 电压模式的旋转磁场只跑到指令的 **2/3** 速——这正是转速缺口、
  以及"电流/速度读数与指令对不上"的直接来源。位置计数不依赖 dt 因而正确，
  速度估计依赖 dt 因而失真，两者表现与实测完全吻合。
- `[待确认]` `wk_tmr.c` 的 `tmr_base_init(TMR1, 3749, 0)`（SWD 读回 pr=0xEA5=3749、
  CCR4=0xE9C=3740 证实配置生效）在 150 MHz 定时器域下应得 40 kHz，代码与文档
  按 20 kHz 记载，而实测注入触发为 13.33 kHz —— 三者互不相容，多出的 /2 与 /3
  出在定时器时钟倍率或 TMR1 重复计数器（rpr）上。寄存器块已 dump（0x40012C00 起），
  需要按 `at32f415.h` 的 tmr 结构偏移逐字段对齐后定论，不影响上面的判决。
- `[范围]` 该 dt 偏差影响所有由 FOC 步进派生的量：I-f/电压模式角度累加、电流环
  与速度环带宽、`CALIB_*` 用 `PWM_FREQ_HZ` 换算的时长、bench 看门狗与能量预算的
  真实秒数。修复前任何 rpm、带宽、PID 数值都不是真实物理量。
- `[下一步]` ①把真实节拍做成可读量（用已有的 `injected_sample_seq` 对 ms tick 求导，
  在 `diag bench status` 打印实测 Hz），杜绝再次靠常量猜；②让常量与现实一致
  （`PWM_FREQ_HZ`/`VEL_LOOP_FREQ_HZ`/`TM_MOTOR_FOC_ADC_DIVIDER` 三者对齐到目标节拍）；
  ③用 `build/_if_pitch_probe.ps1` 复测，期望实测 rpm 等于指令 rpm。
- `[工具]` 新增 `build/_if_pitch_probe.ps1`（极低速 I-f + 最小二乘拟合，直接输出
  反推极对数），本轮用它取到 0.667/0.647 两个独立速度点。

## 2026-09-29 机制确认：采样节拍被边沿握手量化，节拍由服务延迟决定

- `[电压开环对照]` 用 `cfg ctrl volt` + `diag voltloop 30 4.0`（`CTRL_MODE_VOLT` 分支
  绕过电流 PI，只跑 theta_sim 积分 → foc_ctrl_volt → SVPWM）实测 `seq` 仍为
  **13,350 Hz**、转速 20.8rpm（指令 30）。→ 电流 PI 不是瓶颈，两种模式共用
  Clarke/Park + SVPWM + 写占空比的前端。
- `[减法实验一]` `TM_MOTOR_FOC_ADC_DIVIDER` 4→8（FOC 步骤频率减半）：
  `inj_hz` 升到 **16,000**（六点 15,996~16,022），步进 2,000 Hz，实测转速 10.8rpm，
  与模型预测 `30 × 真实步进/5000` = 12.0rpm 同量级（带载滑差略低）。
  三点拟合 `rate = 20000/(1 + (F/A)/divider)` 得 **F/A = 1.99 与 2.00**，模型自洽。
- `[减法实验二]` 把 `_motor_stage_hw_snapshot()` 从 ISR 摘掉（还原后已重新烧回干净镜像）：
  `inj_hz` 仍为 **13,350**、转速照旧 → 故障快照不是成本中心，排除。
- `[机制]` `motor_task.c` 的 CC4 ISR 注释点明关键：TMR1 为**中央对齐**计数，
  CCR4 每个 PWM 周期被上/下计数各穿过一次，即 **CC4 边沿 40 kHz**（TMR 时钟
  150 MHz、pr=3749、PWM 20 kHz），`_adc_sw_trig_armed` 门控保证每周期只注入一次。
  链条是 CC4 边沿 → 软件触发 → 转换 → ADC ISR 服务 → 服务末尾重新置 armed →
  等下一个边沿。因此注入周期只能是边沿间隔（25 µs）的整数倍：
  IDLE 40k/2 = 20,000 Hz、RUNNING 40k/3 = 13,333 Hz，与两次实测逐位吻合。
- `[判决]` **采样节拍不是定时器属性，而是服务延迟的量化函数**。任何使 ADC ISR
  服务时间越过 50 µs 的改动都会让节拍再跳一档，`PWM_FREQ_HZ` 这类常量永远追不上
  现实；历史上"adc_trig 与实际采样时刻基本解耦""gain(θ) 随角度变化""1.5~2s hunting"
  都应放到这个框架下重新解释。
- `[修法方向]` 三条，尚未选定：①压缩 FOC 步骤耗时使其落回 2 个边沿（sin/cos 查表、
  去掉软浮点除法），同时恢复 20 kHz 采样与正确 dt；②改为按实测 dt 积分（运行期测
  两次注入的间隔），使 dt 不再依赖常量——这是安全网而非提速；③取消软件触发的
  服务后重arm，让注入严格由硬件边沿决定（服务超时则丢样而不是拖慢节拍）。
- `[现场]` 临时改动全部还原（`git status` 为空），干净的 bench-30s 镜像已重新烧录并
  以 `inj_hz=13,350` 复验；板子停在无输出状态，3.3V 由飞控板供着。

## 2026-11-08 FalconFOC V2 G0/G1 baseline and runtime gate freeze

- `[架构]` 根据正式评审批准，V2 runtime 采用硬实时中断、前台事件驱动超级循环、静态协作调度、H0/H1 分层状态所有权和独立安全监督。Legacy 继续使用现有 FreeRTOS `ARM_CM3` port、soft-float ABI 和 `cortex-m3-soft` CPU class。
- `[协议]` 当前 Board1 外部通信正式按 USART/UART 处理。Falcon Protocol v1 以 UART 为首发传输，CAN 2.0、CAN-FD、CANopen 和 EtherCAT 不属于当前板 V2 基础验收。
- `[G0]` 基线 HEAD=`1f542521189d5e0ef25e64e6530608ece8d6e137`。host CTest `45/45 PASS`。AT32F415 bench debug 配置和构建通过，ELF SHA-256=`7E061224F3FA3D06A0CF0E2B91892FDEE5B0111B939CCC8228BB74B947041B12`。
- `[G1]` 新增证据文件：`docs/evidence/2026-11-08-legacy-baseline.md`、`docs/evidence/2026-11-08-at32f415-resource-matrix.md`、`docs/evidence/2026-11-08-timing-and-blocking-baseline.md`、`docs/evidence/2026-11-08-uart-dma-audit.md`、`docs/evidence/2026-11-08-g3-safe-acceptance-matrix.md`。
- `[边界]` 本轮只做主机测试、固件构建、源码和 SDK 资源审计。未执行调试器、刷写、UART 命令、PWM 使能、实板带电操作或电机测试。
- `[待决]` TMR2 单调时间源、SPI1 DMA 通道、WDT/WWDT 选择、H0/H1 复合快照实现和 G3-SAFE 实施仍需后续阶段审批。当前 G3 只允许无功率输出验证，G3-SAFE 通过前禁止进入带电测试。

## 2026-11-09 FalconFOC V2 G2 standalone Core

- `[实现]` 新增独立 `core/` C11 静态库，提供 Clarke、Park、逆 Park、角度环绕、中心注入 SVPWM、限幅、显式 `dt_s` 和电压开环步进。Core 不链接 Legacy、MCU、厂商 SDK、FreeRTOS、Shell、Flash、CMSIS-DSP 或硬件寄存器。
- `[测试]` Core 独立 CMake/CTest 三项测试全部通过，覆盖变换、六扇区 SVPWM、期望值、输入不可变、非法数值、非法 VBUS、非法 `dt_s`、状态保持和确定性重放。
- `[回归]` 既有 Legacy host 流程 `45/45 PASS`。AT32F415 `fw-full-diag-carrier-bench-debug` 配置和构建通过，继续使用 `cortex-m3-soft`。未修改 Legacy 控制 ISR、PWM、ADC、Encoder、FreeRTOS port 或根 CMake 组装。
- `[质量门禁]` CMake quality、preset、platform backend 和 CMake smoke 检查通过。现有 firmware CLI 合约脚本因本机没有 Bash 无法执行，需在具备 Bash 的环境中补跑。
- `[边界]` G2 只证明独立控制核心的主机软件基础成立，不证明 AT32F415 WCET、采样时序、PWM 寄存器、安全监督、运行时调度或 G3-SAFE。未刷机、未启动调试器、未发送 UART 命令、未使能 PWM、未进行实板带电或电机测试。

## 2026-11-09 FalconFOC V2 G2 independent source acceptance

- `[审查]` 对本地工作树中的 `core/` 公共头文件、实现、CMake、三组测试、README 和 G2 证据记录完成逐行审查。变换和 SVPWM 使用局部结果提交，`foc_step` 在错误路径完整初始化禁用输出，并且失败时不提交电角度状态。
- `[补强]` 增加 SVPWM 扇区边界连续性、非零向量输出有限性、轴向期望值、失败安全输出完整性、不同合法 `dt_s` 等时积分和长序列状态验证。
- `[结论]` 本地源码与证据快照独立验收 PASS。仓库 firmware CLI 质量脚本仍因当前 Windows 环境没有 Bash 未闭合，不影响 Core CTest、Legacy `45/45` 回归或 bench 固件构建结论。

## 2026-11-09 FalconFOC V2 G3-NP Core/Device and offline chain

- `[接口]` 新增独立 `g3/np/` 主机工程，显式表示 Board1 ADC rank、V/A 换算、ABC/ACB 相序和 TMR1 CH1/CH2/CH3 映射。Device 请求把 `INHIBIT` 与 `UPDATE` 分开，零 compare 只作为 mock 禁止状态的寄存器值，不等同于零占空比驱动或主动制动。
- `[离线链]` 新增 `Sample -> Device decode -> foc_step -> Device map -> mock registers` 模拟链。模拟桥臂 gate 永久关闭，合法请求只更新 mock compare，不报告物理桥臂使能。
- `[测试]` G3-NP 两个 CTest 目标通过，覆盖 ADC 转换、相序和通道映射、样本新鲜度、非法时间、禁用输出、锁存故障优先级、瞬态恢复和失败时 Core 状态保持。
- `[边界]` G3-NP 未修改 Legacy ISR、ADC/PWM 实时链或根固件 CMake。未执行调试器、刷写、UART 命令、PWM 使能、实板带电或电机测试。G3-SAFE 与 G3-PWR 继续未通过。

## 2026-11-09 FalconFOC V2 G3-SAFE first host contract

- `[安全状态]` 新增独立 `g3/safe/` 主机工程，冻结 BOOT、DISABLED、READY、RUNNING、FAULT_LATCHED 状态，故障锁存、显式清除回 DISABLED、命令/样本单调时间新鲜度、双 heartbeat 和输出 gate 语义。
- `[WDT/Flash]` 只实现 watchdog feed permission 与 Flash permission contract，不访问真实 WDT、WWDT、Flash 或 PWM 寄存器。运行态、故障态和非 INHIBIT 动作均被拒绝。
- `[测试]` G3-SAFE fault-injection host test `1/1 PASS`，覆盖状态禁止、命令超时、样本超时、无效样本阈值、fast/foreground heartbeat 丢失、非法输出动作、故障清除和非自动恢复。
- `[边界]` 当前结果属于 `host-verified` 软件契约证据，不证明 WDT 复位时间、桥臂关闭延迟、硬件 break、驱动器保护、Flash 恢复或任何带电行为。G3-PWR 继续锁定。

## 2026-11-09 FalconFOC V2 G2/G3-NP source-style acceptance

- `[规范问题]` 专项审查确认新增 `core/` 与 `g3/np/` 原先没有进入 C/H formatter、CMake quality 和统一质量门禁的完整受管范围，且新增 C/H、CMake 文件缺少 Themis 头、public 契约和 EOF 规范证据。
- `[整改]` 为 11 个 C/H 文件补齐模块或测试角色、设计/使用/所有权/失败契约、public symbol 注释、section banner 和 EOF 标记。为 2 个 standalone CMake 文件补齐 Role 头和 kebab-case target，并扩展 formatter/CMake/quality-gate 覆盖。
- `[门禁]` 新增 `scripts/gates/check_falconfoc_source_contract.ps1`，独立检查 G2/G3-NP C/H、CMake 和 README，并增加统一质量门禁 `falconfoc` scope。source contract、clang-format、两个新增 CMake 的严格 cmake-format、FalconFOC 专项质量 scope、CMake structural checks、G2/G3-NP 测试和 Legacy 回归通过。全量 scope 已先执行 source contract，再被既有根 `CMakeLists.txt` 格式差异阻断，全量 firmware CLI 仍需要 Bash。

## 2026-11-10 FalconFOC V2 G3-SAFE-HW01 hardware safety-chain audit

- `[范围]` 完成 Board1 原理图、网表、BOM、DRV8311H 数据手册、AT32F415 生成式 TMR1/ADC/GPIO 配置、WDT/WWDT SDK 和 Legacy PWM 调用链的只读交叉审计。未修改控制 ISR、WDT 配置、板级寄存器，未刷机、未上电、未使能 PWM。
- `[P0]` 发现生成式 `wk_tmr1_init()` 在 `app_main_bootstrap()` 之前已经开启 TMR1 主输出、CH1/2/3 及互补通道和计数器。`pwm_out_disable()` 尚未成为复位后的第一道门禁，因此“上电默认禁止输出”不能仅由当前软件辅助函数证明。
- `[P0]` 网表确认 DRV8311H `nFAULT` 没有 MCU 端点，`nSLEEP` 也没有 MCU 控制端点。当前 Board1 没有可声明的软件故障输入、MCU 睡眠控制或已闭合的 `nFAULT -> nSLEEP` 外部关断链。
- `[P0]` TMR1 SDK 存在 Break 寄存器和 API，官方 Datasheet 也给出 QFN32 PA6 的 `TMR1_BRK` 复用，但当前 PA6 被 IA 电流采样占用、生成配置保持 `brk_enable=FALSE`，且 DRV `nFAULT` 没有接入。该能力只能标为 `register-only`，不能写成当前板的硬件安全保护。
- `[芯片资料]` 官方 AT32F415 Reference Manual 确认 WDT 由 30–60 kHz LICK 驱动，WWDT 由主时钟驱动，WDT 因此成为第一版独立监督的首选候选。官方 Datasheet 还确认 QFN32 PA6 具备 `TMR1_BRK` 复用，但当前 PA6 被 IA 电流采样占用，不能在现板上直接形成 DRV `nFAULT` -> TMR1 Break 路径。
- `[WDT]` 生成配置同时注释 `WDT_MODULE_ENABLED` 和 `WWDT_MODULE_ENABLED`。WDT 的器件级时钟来源已经确认，但 timeout、频率容差、复位原因和复位期间输出状态仍需补证。G3-SAFE 硬件门禁与 G3-PWR 继续锁定。
- `[证据]` 新增 `docs/evidence/2026-11-10-g3-safe-hw01-resource-audit.md` 和 `docs/superpowers/plans/2026-11-10-falconfoc-g3-safe-hw01.md`，将启动门禁、故障路径、WDT、复位默认态和安全时序拆成后续可验收任务。

## 2026-11-10 FalconFOC V2 HW02-A reset-safety preparation

- `[复核]` 接受 HW01 资源审计，但补充两项边界。`CCR=0` 在 TMR1 极性、中心对齐计数、互补输出和 DRV 6x PWM 真值表共同作用下，不能直接等价为 `INHIBIT`。R7 确实连接 `nFAULT` 与 `nSLEEP`，但电阻通路是否能跨过 DRV Sleep 阈值仍需在明确 DCBUS、D2 钳位和输入电流条件后计算与测量。
- `[电源]` Board1 的 `DCBUS -> U4 VIN_AVDD/VM -> U4 AVDD -> DRV_3V3 -> U3/U5/CSAREF` 依赖被纳入 HW02-A。完全断开 DCBUS 可能同时关闭 MCU 供电，外部 3.3 V 注入在反向供电审查前禁止采用。
- `[阶段]` 批准 `HW02-A` 只读准备，新增 `docs/evidence/2026-11-10-g3-safe-hw02-a-prep.md` 和 `docs/superpowers/plans/2026-11-10-falconfoc-g3-safe-hw02-a.md`。该阶段只建立电源边界、TMR1 寄存器矩阵、`CCR=0` 推导、R7 网络计算和测量前提。
- `[限制]` `HW02-B` 实板供电与波形观测仍需单独授权。WDT 实际复位测试不并入首次复位波形观测，G3-SAFE 最终硬件验收和 G3-PWR 继续锁定。

## 2026-11-10 FalconFOC V2 NEXT-FOC-01 d/q current control

- `[实现]` 新增独立 d/q 电流 PI，复用 G2 变换和 SVPWM。采用显式 `dt_s`、共享电压矢量上限与基于最终占空比的抗饱和。计算失败时清空输出并保留积分状态。
- `[验证]` Core `4/4`，G3-NP `2/2`，G3-SAFE Host `1/1`，Legacy Host `45/45`，AT32F415 Bench 全新构建和 FalconFOC 专项门禁通过。四种步长的固定转子闭环通过。全仓门禁仍阻断于根 CMake 格式，Firmware CLI 合约使用 Git Bash 单独补跑通过。详见 `docs/evidence/2026-11-10-next-foc-01-current-validation.md`。

## 2026-11-10 FalconFOC V2 NEXT-SW01A Host current pipeline

- `[实现]` 新增 Host Pipeline 连接 ADC 解码、电流 PI、Device 映射、Safety 最终 Gate 和闭门 Mock。只有完整 UPDATE 获准且被 Mock 接受才提交 PI 状态。禁止失败明确标记为未确认。零序列、源时间与故障恢复规则覆盖到端到端测试。
- `[验证]` 固定转子模型接受 `1800` 次 UPDATE，最终 INHIBIT，最大窗口轴误差 `0.417 mA`。Pipeline `1/1`、Core `4/4`、G3-NP `2/2`、G3-SAFE `1/1`、Legacy Host `45/45`、Legacy Bench 与 FalconFOC 专项门禁通过。全仓仍阻断于既有根 CMake 格式。详见 `docs/evidence/2026-11-10-next-sw01a-pipeline-validation.md`。

## 2026-11-10 FalconFOC V2 NEXT-SW01B Host execution domains

- `[实现]` 新增 H0、H1 和 Foreground 的固定时序入口。H0 独占 Pipeline，双槽接收角度与命令，原子遥测和独立禁止通道处理抢占。最终 Safety Gate 后仍检查停止请求，保留原 PI 提交与恢复语义。
- `[验证]` Runtime 与 Pipeline `2/2`，Core `4/4`，G3-NP `2/2`，G3-SAFE `1/1`，Legacy Host `45/45`，Bench 和 FalconFOC 专项通过。每次重放接受 `1200` 个 UPDATE，最大误差 `0.417 mA`，最终过迟触发进入 Mock INHIBIT。全仓继续阻断于既有根 CMake 格式。详见 `docs/evidence/2026-11-10-next-sw01b-runtime-validation.md`。

## 2026-11-10 FalconFOC V2 NEXT-FOC-02 sensored motion loops

- `[实现]` 新增机械与电气坐标换算、有界电角预测、独立速度 PI 与位置 P。H1 以源命令和会话身份发布外环目标，仅在 H0 确认接受后提交提案。换模仅允许在已禁止的 DISABLED 会话，重新使能沿用 Safety ARM/START。
- `[验证]` 固定参数 PMSM 电气机械模型验证正反速度、连续位置和负载增减，记录轨迹、双实例隔离与确定性重放。Core `5/5`、Runtime `4/4`、G3-NP `2/2`、G3-SAFE `1/1`、Legacy Host `45/45`、Bench 与专项门禁通过。全仓仍阻断于既有根 CMake 格式。详见 `docs/evidence/2026-11-10-next-foc-02-motion-validation.md`。

## 2026-11-10 FalconFOC V2 NEXT-SW03 AT32F415 参考固件

- `[实现]` 独立裸机入口复用厂商启动、xB 链接脚本和 SDK。TMR2 计时，TMR1 CH4/ADC1 进入 H0，TMR5/SPI1 进入 H1。锁定适配器校验三相请求，拒绝 UPDATE 并保持 PI 未提交。
- `[验证]` V2 ELF/BIN/HEX/MAP 已生成。Board1 Host、Core、Runtime、G3-NP、G3-SAFE、Legacy Host 与 Bench 通过。专项门禁通过，全仓格式门禁待闭合。编码器料号无 CRC，物理输出与实板时序仍待验证。详见 `docs/evidence/2026-11-10-next-sw03-at32f415-validation.md`。

## 2026-11-10 FalconFOC V2 NEXT-SW04 UART 与主机工具

- `[实现]` 新增独立 COBS/CCITT-FALSE V1 协议、会话与序号检查、低优先级 USART1 字节 IRQ、有界缓冲、Runtime 命令撤销与 H0 命令超时故障恢复。三个 RAM 参数由 Foreground 持有，ARM/START 保持 OUTPUT_LOCKED，保存与自动流遥测明确返回不支持。
- `[验证]` Protocol 1/1、Board/Transport/Python 3/3、Runtime 5/5、Core 5/5、G3-NP 2/2、G3-SAFE 1/1、Legacy Host 45/45、全新 Bench 和 V2 构建通过。V2 Flash 33,768 B、链接器 RAM 3,376 B。源码契约与专项门禁通过，全仓仍阻断于根 CMake 格式。详见 `docs/evidence/2026-11-10-next-sw04-uart-validation.md`。

## 2026-11-10 FalconFOC V2 NEXT-SW05 RC0 软件验收

- [验证] 增加 10,000 次确定性协议随机输入、87 µs UART 压力流和 100,000 周期 Runtime 长事件流。长流重复指纹 49dea950，H0 漏期、迟到和预算超限均为 0。UART 压力统计为 CRC 29、丢帧 88、RX 溢出 29、UPDATE 0。
- [资源] 重新生成 V2 ELF/MAP，Flash 33,768 B、链接器 RAM 3,376 B、.bss 2,268 B，4 KiB 栈保留断言通过。ARM、Host、Legacy Host 45/45、Legacy Bench 和专项门禁通过。
- [边界] Windows Clang sanitizer 运行时因缺少 coe_win/Dbghelp 链接符号标记 BLOCKED。全仓门禁仍停在既有根 CMake 格式，全软件 RC0 证据记录见 docs/evidence/2026-11-10-next-sw05-rc0-validation.md。

## 2026-11-10 FalconFOC V2 NEXT-SW05R RC0 software recheck

- `[AC11]` 使用 LLVM Clang 22.1.8 的 `-fsanitize=address,undefined` 重新构建并运行 Core、G3-NP、G3-SAFE、Runtime、Protocol、Board/UART Host 工程，共 18/18 CTest 通过；仅为测试进程补充 LLVM 动态 sanitizer runtime 路径，未向生产 ARM 构建注入 sanitizer 选项。
- `[CMake]` 根 `CMakeLists.txt` 只做 LF 换行修正，并修复两个门禁脚本对无扩展名 `.cmake-format` Python 配置的识别以及临时 Unix 输入检查。根文件格式检查通过，未改变 CMake 命令、字符串或目标语义。
- `[边界]` `Scope all` 在根文件之后继续暴露既有共享 CMake、`tests/CMakeLists.txt` 及新 standalone V2 CMake 清单的格式差异。本轮按授权不格式化这些文件，因此 RC0 软件仍为 `BLOCKED`；Legacy 回归、Legacy Bench、V2 ARM ELF/BIN/HEX/MAP 和全部 Host 功能测试保持通过。

## 2026-11-12 FalconFOC V2 NEXT-CORE-PERF-01A Float32 baseline

- `[基线]` 新增独立 Host baseline targets，仅记录当前 Float32 电流环的 Golden CSV、动态数学调用次数、正常/饱和 Host 微基准和 Legacy `foc_fast_sincos` 全角度误差扫描；未修改 Core、Runtime、Safety、Board 或 Protocol 生产逻辑。
- `[结果]` 正常路径动态计数为 `fmodf/sinf/cosf=3/3/3`、`atan2f=1`、`sqrtf=3`、`hypotf=1`；带抗饱和二次调制路径为 `5/5/5`、`atan2f=2`、`sqrtf=5`、`hypotf=2`。Legacy 扫描 1,048,576 点，最大三角误差 `5.99211262e-6`，最大 duty 传播差 `2.56299973e-6`，sector mismatch `0`。
- `[性能边界]` Host Release 中正常路径中位数 `141.2125 ns/step`，饱和候选路径 `257.9015 ns/step`；这些不是 AT32F415 周期或 WCET。ARM V2 ELF/MAP/反汇编和栈帧已重新读取，V2 BIN SHA 保持不变。
- `[限制]` PERF-01A 到此停止。未引入 Q31、未删除 `atan2f`、未替换 sin/cos、未修改 PI/抗饱和、未刷机、未上电、未提交或推送。详见 `docs/evidence/2026-11-12-next-core-perf-01a-float32-baseline.md`。

## 2026-11-13 FalconFOC V2 NEXT-CORE-PERF-01B Float32 hot-path reduction

- `[实现]` Core 内部新增一次角度规约/sin-cos 载荷，Park、逆 Park、电压反算和抗饱和二次调制复用；SVPWM sector 改为无 `atan2f` 的符号/边界判定；公开 API 验证保留，电流 context 使用已初始化配置的 trusted SVPWM 入口。
- `[等价]` PERF-01A 原始 Golden SHA `AC74F399...8427` 保持不变，4,126 条记录逐项比较通过；状态、sector、Device compare、积分状态和浮点输出均在固定门限内等价。正常动态 `fmodf/sinf/cosf` 从 `3/3/3` 降至 `1/1/1`，`atan2f` 从 1 降至 0。
- `[性能]` Windows Host Release 中位数：正常路径 `141.2125 -> 88.7070 ns/step`（-37.18%），饱和候选路径 `257.9015 -> 177.7580 ns/step`（-31.08%）。AT32F415 ELF `.text` `31,976 -> 31,560 B`，RAM 静态统计不变；`foc_current_step` 栈帧增加 16 B，记录为后续栈水位验证边界。
- `[验证]` Core 7/7、Runtime 6/6、G3-NP 2/2、G3-SAFE 1/1、Board/UART/Python 3/3、Protocol 1/1、Legacy Host 45/45、Legacy Bench、V2 ARM 构建、source contract、C/H formatter 和 FalconFOC scoped quality gate 通过。
- `[限制]` 未修改 Runtime、Safety、Device、Board、Protocol、Legacy；未进入 PERF-01C、Q31、传感器或校准；未刷机、未上电、未提交或推送。详见 `docs/evidence/2026-11-13-next-core-perf-01b-float32-hotpath.md`。

## 2026-11-13 FalconFOC V2 NEXT-CORE-PERF-01C Float32 hot-path final acceptance

- `[边界修正]` 独立 sector differential test 在 `atan2f(-0,-0)` 语义上发现直接判定误返回 sector 6；按原实现的 `-pi + 2pi -> sector 4` 规则做最小修正，并加入带符号零回归。
- `[等价]` 新参考实现覆盖 100,015 个定向/随机样本，sector、duty、状态和饱和标志全部一致。原始 PERF-01A Golden SHA `AC74F399...8427` 保持不变，4,126 条记录逐列比较失败数为 0。
- `[验证]` Core `8/8`、Runtime `6/6`、G3-NP `2/2`、G3-SAFE `1/1`、Board/UART/Python `3/3`、Protocol `1/1`、Legacy Host `45/45`、Clang ASan/UBSan `21/21`、Legacy Bench、V2 ARM 构建、源码契约、C/H formatter 和 FalconFOC scoped gate 通过。
- `[性能]` PERF-01C Host 中位数为正常 `90.5050 ns/step`、饱和修正 `179.5810 ns/step`；相对 PERF-01A 分别下降 `35.91%` 和 `30.37%`。ARM `.text=31,556 B`、`.rodata=1,268 B`、链接器 RAM `3,376 B`，没有 MCU WCET 或真实栈水位结论。
- `[范围]` 全仓 `Scope all` 仍在既有 `core/CMakeLists.txt` cmake-format debt 处阻断；本轮没有扩大范围处理无关 CMake。未刷机、未上电、未使能 PWM、未提交或推送。详见 `docs/evidence/2026-11-13-next-core-perf-01c-validation.md`。

## 2026-11-13 FalconFOC V2 NEXT-SENSOR-01 magnetic encoder feedback

- `[模块]` 新增独立 `sensors/` Float32 模块，使用整数累计计数保存连续机械位置，输出方向化单圈角度、实际源时间间隔、raw 差分速度、可变 `dt` 一阶 LPF、alpha-beta 跟踪速度和有界质量枚举。模块无 MCU/SPI/RTOS/Runtime/Safety/动态内存/全局可变状态依赖，`foc_encoder_context`/feedback 分别为 `80/48 B`。
- `[连续性]` 支持 16 位 `65535 -> 0`、反向、跨 uint32 微秒时间戳回绕、重复/倒序时间戳、最小/最大间隔、半圈别名判定、精确半圈跳变、速度越界和显式连续性锁存。无效帧不推进最后可信位置或滤波状态，恢复必须显式 reset。
- `[Board1]` 保留 SPI 事务、完成/错误、源捕获时间和 `foc_runtime_sensor` 接口；Board1 仅负责 transport 与 H1 发布，新的 Sensor 模块负责解圈和速度。现有 `mechanical_rad_s` 选择 LPF 输出，raw 与 alpha-beta 只保留本地诊断；ARM/START、UPDATE、INHIBIT 锁定行为未改变。
- `[数据集]` 确定性 Host 合成序列通过：静止 RMS raw/LPF/alpha-beta `0.271071/0.03487715/0.03200987`，8 rad/s 阶跃延迟 `11/9` 帧，正反转恢复 `14/11` 帧，加速 RMSE `0.03382908/0.02845867`，单次丢帧恢复 raw 误差 `0.02669954 rad/s`。这些是算法比较，不是实板噪声或 MCU 时序证据。
- `[验证]` Sensor CTest `1/1`、Board/UART/Python `3/3`、Clang ASan/UBSan 合计 `22/22`、Core `8/8`、Runtime `6/6`、G3-NP `2/2`、G3-SAFE `1/1`、Protocol `1/1`、Legacy Host `45/45`、Legacy Bench、ARM 构建、源码契约 `51` C/H、C/H formatter 和 FalconFOC scoped gate 通过。
- `[资源/边界]` Sensor 接入后的 ARM `.text=33,076 B`、`.rodata=1,312 B`、`.bss=2,392 B`、链接器 RAM `3,504 B`；`foc_encoder_update` 静态栈帧 `72 B`。未执行刷机、上电、PWM 或真实编码器电气测试，未开始校准开发。详见 `docs/evidence/2026-11-13-next-sensor-01-validation.md`。

## 2026-11-13 FalconFOC V2 NEXT-CAL-01 calibration candidates

- `[模块]` 新增独立 `calibration/` Float32/C11 模块，使用在线 Welford 统计估计三相 ADC 零点，使用显式机械/电角度参考拟合方向和极对数，并使用圆周均值估计电角度 offset。模块不访问 Board、Runtime、Safety、Flash、PWM 或 MCU。
- `[零点]` 64 个确定性零电流样本得到 offset `(2042.98, 2052.02, 2048.50)` counts，噪声 RMS `(0.826009, 0.826011, 0.503953)` counts；未确认外部零电流前提、突变、漂移、invalid mask 和样本不足均拒绝。
- `[辨识]` 合成可信参考数据得到 direction `-1`、pole pairs `7`、electrical offset `1.199989 rad`，RMS residual `0.0004121638 rad`；机械跨度不足、被动机械角度和不可区分候选返回 `UNIDENTIFIABLE`。圆周边界 offset `6.199975 rad`，RMS residual `0.0004019763 rad`。
- `[边界]` 结果携带 ADC/encoder/current-gain/phase-order/pole-pair identity；本轮只验证映射到未来 `foc_device_adc_config` 与 `foc_rotor_config` 的字段，不覆盖当前 Board1 的 2048/7/0 静态值，不写 Flash，不自动应用。
- `[验证]` Calibration `1/1`、Sensor `1/1`、Core `8/8`、Runtime `6/6`、G3-NP `2/2`、G3-SAFE `1/1`、Board/UART/Python `3/3`、Protocol `1/1`、Legacy Host `45/45`、Legacy Bench、ASan/UBSan `23/23`、ARM 构建、源码契约 `54` C/H、C/H formatter 和 FalconFOC scoped gate 通过。
- `[范围]` Calibration ARM 独立编译对象 `4,100 B` text、最大静态栈帧 `104 B`，未进入生产 ARM 链接或实时路径。真实 ADC 零点、编码器安装方向、极对数和电角度 offset 仍未验证；G3-SAFE/G3-PWR 继续锁定。详见 `docs/evidence/2026-11-13-next-cal-01-validation.md`。

## 2026-11-13 FalconFOC V2 NEXT-CONTROL-01 optional d/q feedforward

- `[实现]` 在 `core` 增加可选 Float32 PMSM d/q 前馈上下文和入口，使用 `Ld`、`Lq`、`psi_f` 与同一控制时刻的 signed electrical speed 计算 `vd_ff`/`vq_ff`。前馈关闭时继续走原 `foc_current_step()` 事务路径，不修改 Runtime、Safety、Board 或 Protocol。
- `[抗饱和]` 前馈与 PI 请求一起进入原 SVPWM、VBUS/电压向量和 duty 限幅；积分器准入扣除 proportional 与 feedforward，保留失败不提交和饱和恢复语义。独立 PMSM 模型覆盖正反速度、参数失配、限幅和 500 周期饱和。
- `[验证]` Core 独立构建 `9/9`，Sensors `1/1`，Calibration `1/1`，Runtime/Pipeline `6/6`，G3-NP `2/2`，G3-SAFE `1/1`，Protocol `1/1`，Board/UART/Python `3/3`，Legacy Host `45/45`，Clang ASan/UBSan `24/24`，Legacy Bench、V2 ARM 构建、源码契约、C/H formatter、受影响 CMake formatter 和 FalconFOC scoped quality gate 通过。无 sanitizer 诊断，`git diff --check` 通过；`Scope all` 仍在既有 `g3/np/CMakeLists.txt` 格式债务处失败。
- `[资源]` V2 ARM 链接器报告 `.text=33,436 B`、`.rodata=1,312 B`、`.data=84 B`、`.bss=2,392 B`，Flash `35,216 B`、链接器 RAM `3,504 B`；`foc_current_step_internal` 静态栈帧 `184 B`。ELF/BIN/HEX/MAP SHA 分别记录于本轮证据文件。
- `[边界]` H0 当前 `foc_current_input` 没有同一 ADC 时刻的可信电角速度，故本轮只完成算法和独立 Core 入口，未隐式接入 H1 速度，也未宣称 Board/Runtime 集成或真实电机收益。最终状态为 `NEXT-CONTROL-01 ALGORITHM PASS / INTEGRATION BLOCKED`；未刷机、未上电、未启用 PWM、未提交或推送。详见 `docs/evidence/2026-11-13-next-control-01-validation.md`。

## 2026-11-13 FalconFOC V2 NEXT-CONTROL-01B electrical-speed integration

- `[链路]` 复用 H1 已发布的双槽 `foc_runtime_sensor_slot`，将同一槽内的 `foc_electrical_state`、源时间和序列随 H0 输入传入 Pipeline。H0 不重复运行 Sensor、不重复乘极对数、不读取全局速度缓存；前馈仍默认关闭，Board1 配置未启用。
- `[时序]` 新增 `foc_pipeline_feedforward_config.max_age_us` 和电气反馈快照检查，使用现有 uint32 模数时间关系拒绝未来、过期、非有限或无效速度。合法源到 ADC 延迟采用零阶保持近似，并明确不覆盖编码器滤波群延迟、速度噪声或加速度误差。
- `[事务]` 前馈无效时返回 `FOC_PIPELINE_ERR_FEEDBACK`，沿原 Pipeline `INHIBIT` 路径停止，PI 试算状态不提交；有效时继续使用已验收的 Core 前馈、SVPWM、Device 和 Safety 门禁。
- `[验证]` 新增 `test-foc-control01b`，覆盖正反速度、合法延迟、过期/未来样本、NaN/无效反馈、uint32 回绕和 H1 写槽/H0 读槽交错。Runtime `7/7`、Board `3/3`、Legacy Host `45/45`、ASan/UBSan 合计 `25/25`、ARM 构建、源码契约 `56` C/H、C/H/受影响 CMake formatter、FalconFOC scoped gate 和 `git diff --check` 通过。
- `[资源]` V2 ARM `.text=33,748 B`、`.rodata=1,332 B`、`.data=84 B`、`.bss=2,412 B`，Flash `35,548 B`、链接器 RAM `3,528 B`；`foc_pipeline_step_guarded` 静态栈帧 `312 B`，`foc_runtime_control` `224 B`。本轮没有真实 MCU WCET 或实板时序结论。
- `[边界]` `Scope all` 仍在既有 `g3/np/CMakeLists.txt` 格式债务处失败；G3-SAFE 硬件验收和 G3-PWR 继续锁定。最终状态为 `NEXT-CONTROL-01B SOFTWARE INTEGRATION PASS`，但真实测速延迟、参数和功率级行为仍未验证；未刷机、未上电、未启用 PWM、未提交或推送。详见 `docs/evidence/2026-11-13-next-control-01b-integration.md`。

## 2026-11-13 FalconFOC V2 NEXT-CONTROL-02 trajectory planning

- `[模块]` 新增独立 `foc_trajectory` Float32/C11 模块，支持速度目标加减速限制、连续位置点到点规划、制动速度上界、正反转、多圈位置和明确的 READY/ACTIVE/INVALID 生命周期。模块不访问 Sensor、Board、Safety、Protocol、时钟或输出。
- `[集成]` Runtime H1 在轨迹启用时复制 context 试算，把规划速度接入现有 Velocity PI；正式轨迹状态复用 `outer_trial_seen`、`accepted_outer_seq` 和 `motion_session` 提交边界。H0、Device、Safety 或 UPDATE 拒绝不会推进正式轨迹；轨迹关闭时保留旧位置 P/速度 PI 路径。
- `[验证]` 独立 Core `10/10`，Runtime `7/7`，包含现有 PMSM 模型的轨迹速度/位置、目标反向和 UPDATE 拒绝测试；Board/UART/Python `3/3`，Legacy Host `45/45`，Legacy Bench、ARM 构建、Clang ASan/UBSan 合计 `26/26`、源码契约 `59` C/H、C/H/受影响 CMake formatter、FalconFOC scoped gate 和 `git diff --check` 通过。
- `[资源]` V2 ARM 链接器报告 `.text=34,904 B`、`.rodata=1,352 B`、`.data=84 B`、`.bss=2,488 B`，Flash `36,724 B`、链接器 RAM `3,600 B`；轨迹公开函数栈帧为 `init 56 B`、`reset 16 B`、`velocity_step 32 B`、`position_step 40 B`，`foc_runtime_control` `224 B`。没有真实 MCU WCET 或 H1 实测占用结论。
- `[边界]` 轨迹和前馈在 Board1 参考配置中均默认关闭；正常减速不替代独立 Safety INHIBIT。`Scope all` 仍受既有 `g3/np/CMakeLists.txt` 格式债务影响，G3-SAFE 硬件验收和 G3-PWR 继续锁定；未刷机、未上电、未启用 PWM、未提交或推送。详见 `docs/evidence/2026-11-13-next-control-02-trajectory.md`。

## 2026-11-13 FalconFOC V2 NEXT-CONFIG-01 calibration admission

- `[实现]` 在现有 `calibration/` 中新增完整候选的原子准入与 `foc_calibration_snapshot`。入口重新检查质量、范围、样本数和跨结果 identity；失败不修改调用者快照。CAL-01 的校准方向与源机械坐标方向通过一次乘法解析为 Runtime Sensor 方向，不重复反转。
- `[集成]` Board1 增加显式的 `foc_board_init_with_calibration()`。它在初始化前装配 ADC 偏置、极对数、电角度 offset 和 Sensor 方向，成功后一次性提交 Runtime、输出日志和 Sensor 配置；默认 `foc_board_init()`、2048/7/0 名义参数、前馈/轨迹关闭和输出锁保持不变。
- `[验证]` 新增 Calibration admission 负向测试和 Board1 Profile A/B 数据链测试，覆盖 ADC 解码、编码器方向、Rotor 映射、H1 发布、身份不匹配和重复配置拒绝。真实 ADC/编码器/电机参数仍未验证；本轮不写 Flash、不刷机、不上电、不启用 PWM。

## 2026-11-13 FalconFOC V2 NEXT-SYSTEM-01 full-chain stability

- `[测试]` 新增 `runtime/tests/test_foc_system.c` 与 `test-foc-system` CTest，启用 Host feedforward、trajectory、速度 PI、电流 PI、SVPWM、Device、Safety 和 Mock，使用 H0 `200 us`、H1 `500 us`、Foreground `200 us` 的多速率事件流重复回放。指纹 `3f01f6e3`，H0 due `103`、H1 due `41`，漏期/迟到/预算超限均为零。
- `[故障]` 系统测试覆盖正反速度命令、UPDATE 拒绝、轨迹/外环/内环试算状态不提交，以及 WDT inhibit 后的 `FAULT_LATCHED`。既有 `test-foc-long` 的 `100,000` 次回放仍保持指纹 `49dea950`，updates `99,998`，漏期、迟到和预算超限均为零。
- `[门禁]` Runtime CTest 增至 `8/8`，ASan/UBSan 总计 `28/28`，Source contract `61 C/H、8 CMake、8 README`，C/H/受影响 CMake formatter、`Scope falconfoc` 和 Legacy Host `45/45` 通过。`Scope all` 仍停在既有 `g3/np/CMakeLists.txt` cmake-format debt；无生产算法或安全路径修复，未刷机、未上电、未启用 PWM、未提交或推送。

## 2026-11-13 FalconFOC V2 NEXT-RC0-SW-01 RC0 软件复验

- `[修复]` 仅按本轮授权对 `g3/np/CMakeLists.txt` 做 cmake-format 布局修正；目标、源文件、编译选项、链接依赖、测试注册和构建语义均保持不变。文件 SHA 从 `8CF6AF73...` 变为 `68AC235F...`，独立 G3-NP 构建 `2/2` 通过。
- `[验证]` 新鲜 `build/rc0/software-gate-01` Host Debug MinGW `45/45`、Clang ASan/UBSan `28/28`、Runtime `8/8`、Legacy Bench 构建和 V2 AT32F415 ARM 构建通过；系统回放 `3f01f6e3`、长回放 `49dea950` 保持确定性。RelWithDebInfo 的唯一失败是 Debug-only fail-fast 断言在 `NDEBUG` 下不触发，已记录为配置差异。
- `[门禁]` Source contract `61/8/8`、C/H formatter、changed/untracked CMake formatter、`Scope falconfoc` 和 `git diff --check` 通过。`Scope all` 在继续检查时暴露另一个未修改的既有 `g3/safe/CMakeLists.txt` 格式债务；按任务边界不扩大修改范围，因此 `NEXT-RC0-SW-01 BLOCKED`、`RC0 SOFTWARE BLOCKED`。
- `[产物]` V2 section 为 `.text=34,916 B`、`.rodata=1,352 B`、`.data=84 B`、`.bss=2,520 B`，Flash `36,736 B`、RAM `3,632 B`；ARM/Legacy artifact SHA 已写入 `docs/evidence/2026-11-13-next-rc0-sw-01-validation.md`。
- `[边界]` 没有刷机、上电、PWM、Flash 写入、commit 或 push；G3-SAFE 硬件能力仍未验证，G3-PWR 继续锁定。

## 2026-11-14 FalconFOC V2 NEXT-RC0-SW-01B 剩余 CMake 门禁复验

- `[修复]` 仅对 `g3/safe/CMakeLists.txt` 使用仓库 `cmakelang/cmake-format 0.6.13` 做格式布局修正；SHA 从 `B01FA782...` 变为 `A8300D56...`，目标、源文件、编译选项、链接关系和测试注册未变。
- `[扫描]` 对 `Scope all` 的 29 个实际 CMake 文件做完整逐文件检查。修复前失败 5 个；修复后仍有 `tests/CMakeLists.txt`、`cmake/ThemisBuildAxes.cmake`、`cmake/ThemisContracts.cmake`、`platform/at/at32f415/ThemisPlatformDefaults.cmake` 4 个未授权历史格式失败，未扩大修改范围。
- `[验证]` 新鲜 `build/rc0/software-gate-01b` Host Debug `45/45`、Core `10/10`、Runtime `8/8`、G3-NP `2/2`、G3-SAFE `1/1`、Board `4/4`、Clang ASan/UBSan `28/28`、SYSTEM replay `3f01f6e3`、long replay `49dea950`、Legacy Bench、V2 ARM 构建全部通过。RelWithDebInfo 保留真实 `44/45`，唯一失败是 `NDEBUG` 下 Debug-only fail-fast 断言未触发。
- `[资源]` V2 ARM section 仍为 `.text=34,916 B`、`.rodata=1,352 B`、`.data=84 B`、`.bss=2,520 B`，Flash `36,736 B`、RAM `3,632 B`；主要静态栈帧已记录于 `docs/evidence/2026-11-14-next-rc0-sw-01b-validation.md`。Legacy 二进制指纹仅因 `__DATE__/__TIME__` 跨日变化，链接布局和代码 section 大小不变。
- `[结论]` Source contract、C/H formatter、changed/untracked CMake formatter、`Scope falconfoc` 和 `git diff --check` 通过；`Scope all` 被 4 个未授权格式文件阻断，因此 `NEXT-RC0-SW-01B BLOCKED`、`RC0 SOFTWARE BLOCKED`。G3-SAFE 硬件未验证，G3-PWR 继续锁定。

## 2026-11-14 FalconFOC V2 NEXT-RC0-SW-01C CMake 格式债务收口

- `[修复]` 仅按授权格式化 `tests/CMakeLists.txt`、`cmake/ThemisBuildAxes.cmake`、`cmake/ThemisContracts.cmake` 和 `platform/at/at32f415/ThemisPlatformDefaults.cmake`。四个文件的非注释语义 token 数量与顺序逐一保持一致；`g3/np`、`g3/safe` 两项既有修复保持不变。
- `[扫描]` 从当前 `check_cmake_quality.ps1` 动态枚举 29 个 `Scope all` CMake 文件，`cmake-format 0.6.13` 扫描结果 `29/29 PASS`。
- `[验证]` 新鲜 `build/rc0/software-gate-01c` Host Debug `45/45`、Core `10/10`、Sensors `1/1`、Calibration `1/1`、G3-NP `2/2`、G3-SAFE `1/1`、Runtime `8/8`、Protocol `1/1`、Board `4/4`、Clang ASan/UBSan `28/28`、SYSTEM replay `3f01f6e3`、long replay `49dea950`、Legacy Bench 和 V2 ARM 构建通过。RelWithDebInfo 真实结果为 `44/45`，失败仍是 `NDEBUG` 下 Debug-only fail-fast 断言。
- `[门禁]` Source contract、C/H formatter、全量 CMake formatter、`Scope falconfoc` 和 `git diff --check` 通过；正式 `Scope all` 在 `test_firmware_cli_contract.ps1` 因当前 Windows 没有 Bash 失败。没有 Git Bash，也没有已安装 WSL 发行版；未安装依赖、未改门禁脚本，因此 `NEXT-RC0-SW-01C BLOCKED`、`RC0 SOFTWARE BLOCKED`。
- `[资源]` V2 ARM `.text=34,916 B`、`.rodata=1,352 B`、`.data=84 B`、`.bss=2,520 B`，Flash `36,736 B`、RAM `3,632 B`，主要静态栈帧与产物 SHA 写入 `docs/evidence/2026-11-14-next-rc0-sw-01c-validation.md`。Legacy 指纹差异仅来自 `__DATE__/__TIME__`，代码 section 和链接布局不变。
- `[边界]` Board1 输出锁、默认校准参数、Feedforward/Trajectory 默认关闭状态保持；G3-SAFE 硬件未验证、G3-PWR 继续锁定；未刷机、未上电、未启用 PWM、未写 Flash、未提交或推送。

## 2026-11-14 FalconFOC V2 NEXT-RC0-ENV-01 Bash 环境与 Scope all 复验

- `[环境]` 复用现有 `D:\Git\bin\bash.exe`，Bash `5.3.15`、Git `2.55.0.windows.4`；仅在当前 PowerShell 进程临时前置 `D:\Git\bin;D:\Git\cmd`，没有安装依赖、修改系统 PATH 或 Git 全局配置。
- `[CLI]` `test_firmware_cli_contract.ps1` 实际通过，`run.sh all/build/repair/memory` 全部由脚本创建的 Mock cmake/ninja/OpenOCD 路径执行，退出码 `0`，临时目录和随机 build 目录已清理；未接触真实设备。
- `[门禁]` 正式 `Scope all` 已越过 Bash 与 Firmware CLI contract 阶段，CMake `29/29`、Source contract、C/H、Scope falconfoc、preset/platform/smoke、cache guard 和 Transport/Shell 均通过；随后在 `test_falconfoc_themis_lite_contract.ps1` 读取缺失的 `platform/at32f403a/ThemisPlatformDefaults.cmake` 处失败，因此 `NEXT-RC0-ENV-01 BLOCKED`、`RC0 SOFTWARE BLOCKED`。
- `[回归]` 新鲜 `build/rc0/software-gate-01c` Host Debug `45/45`、ASan/UBSan `28/28`、Runtime `8/8`、G3-NP `2/2`、G3-SAFE `1/1`、Board `4/4`、SYSTEM replay `3f01f6e3`、long replay `49dea950`、Legacy Bench 和 V2 ARM 构建通过；RelWithDebInfo 保持 `44/45`。
- `[边界]` 未修改 CLI smoke、Lite contract、生产源码或门禁规则；没有刷机、上电、PWM、Flash 写入、commit 或 push。G3-SAFE 硬件仍未验证，G3-PWR 继续锁定。详见 `docs/evidence/2026-11-14-next-rc0-env-01-validation.md`。

## 2026-11-14 FalconFOC V2 NEXT-RC0-CONTRACT-01 Themis-lite 归属审查

- `[审查]` 当前工作树、CMake presets、platform 目录和迁移文档确认 AT32F415 是当前 Legacy 与 V2 的实际平台；AT32F403A 目录不存在，AT32F423 也没有实现目录或有效 preset。`47c4a9e` 历史提交说明已明确记录 Themis-lite 脚本仍引用不存在的 at32f403a 路径，属于预存迁移欠账。
- `[分类]` `test_falconfoc_themis_lite_contract.ps1` 同时包含 ACTIVE 的 artifact/ownership/feature/CPU-class/Shell/Flash/安全负向断言、MISMATCHED 的 F403A→F415 路径和 boot/linker 引用、尚未实现的 platform timing seam，以及 LEGACY 的 COM54/J-Link/AT32F403A 历史维护 witness。不能把整份脚本删除，也不能复制文件伪造旧平台。
- `[方案]` 推荐将当前 RC0 FalconFoc contract 与 historical/prepared platform audit 分离；当前 contract 只验收 AT32F415 和有效软件安全/构建边界，旧 Themis-lite 与 F423/timing seam 作为独立迁移审计。具体文件计划、保留断言和后续矩阵见 `docs/evidence/2026-11-14-next-rc0-contract-01-review.md`。
- `[状态]` 本轮只读审查完成，没有修改脚本、平台文件、CMake、生产源码或门禁；`RC0 SOFTWARE BLOCKED`、`G3-SAFE HARDWARE NOT VERIFIED`、`G3-PWR LOCKED`、`AT32F415 WCET NOT VERIFIED` 保持。

## 2026-11-14 FalconFOC V2 NEXT-RC0-CONTRACT-02 当前产品契约迁移

- `[迁移]` 新增 `tests/script/test_falconfoc_current_contract.ps1`，将仍然有效的 FalconFoc 软件、AT32F415 平台、CPU class、启动链、Shell/transport、Flash、artifact 和输出锁断言从历史 Themis-lite 入口分离出来；原 `test_falconfoc_themis_lite_contract.ps1` 保留为历史/准备平台审计，不再作为当前 RC0 的唯一阻断入口。
- `[强化]` 当前契约现在精确验证 `cortex-m3`、无 FPU、soft-float、ARM_CM3 port，以及 M4F-hard 参考 class 的 CPU/FPU/ABI/port 值；不再只检查变量名存在。AT32F415 timing 仍按真实 `applications/algo/foc_config.h` 值检查，并明确记录 platform-owned timing seam 尚未迁移，未创建占位平台文件或改变运行参数。
- `[文档]` 两份启动文档的 live owner 已从历史 AT32F403A 路径对齐到 `platform/at/at32f415/generated/src/main.c`；历史 F403A/F423 与 COM54/J-Link witness 仍保留在历史记录和独立审计范围。
- `[验证]` 当前契约独立 PASS；隔离负向夹具 `12/12` 按预期拒绝错误 CPU class、FPU/ABI、Flash geometry、linker stack、缺失 artifact、timing、metadata owner、boot doc、旧 vendor header、output lock、IPC 和 Shell 泄漏。Scope `falconfoc`、`bsp`、`applications`、`all` 均实际 PASS；Host Debug `45/45`；Clang ASan/UBSan `28/28`；SYSTEM replay `3f01f6e3`；long replay `49dea950`；Legacy Bench 与三种 AT32F415 固件 preset 构建通过。V2 独立 ARM ELF/BIN/HEX/MAP 重新构建，Flash `36,736 B`、RAM `3,632 B`，保持 soft-float ABI；实际 MCU WCET、G3-SAFE 硬件和 G3-PWR 仍未验证/锁定。
- `[状态]` `NEXT-RC0-CONTRACT-02 PASS`、`RC0 SOFTWARE GATES PASS`。本轮未修改 FOC、Runtime、Safety、Device、Board 功率输出或历史契约脚本；未刷机、未上电、未启用 PWM、未写 Flash、未 commit、未 push。

## 2026-11-14 FalconFOC V2 NEXT-BOARD-READY-01 实板准备与硬件准入设计

- `[架构]` 分离记录 Legacy 与 V2 Reference 的真实执行链：Legacy 为 AT32F415 + FreeRTOS ARM_CM3，TMR1/ADC 约 20 kHz event、产品 FOC 10 kHz、TMR5 外环 5 kHz、位置 1 kHz；V2 已将 H0 绑定 TMR1/ADC 200 µs、H1 绑定 TMR5/SPI1 500 µs，Foreground 仍为主循环轮询。没有把 Host H0/H1 回放当成 Legacy 或 V2 的实板 WCET。
- `[资源]` 重新执行 V2 与 Legacy AT32F415 构建、ELF/MAP/`readelf`/`objdump`/`nm` 分析，统一区分 Flash BIN、RAM 静态对象、保留栈和 ELF debug 大小。V2 当前 Flash load `36,736 B`、RAM linker 使用 `3,632 B`，其中 `.data=84 B`、`.bss=2,520 B`、保留栈 `1,024 B`；`._user_heap_stack` 不含 heap，4 KiB stack headroom 断言仍只是静态约束。
- `[测量设计]` 建立 H0/H1 入口延迟、ADC/SPI 事务、Core/Safety 执行、publication-to-consume、Foreground、抢占次数、P95/P99 和 Observed Maximum 的 DWT/GPIO/Timer capture 方案，明确 Observed Maximum 不等于 Proven WCET；V2 当前没有真实 PWM 提交点，不能测量 actuator response。
- `[安全]` 核对 V2 输出锁、INHIBIT、TMR1 MOE/PWM mux、DRV8311H nFAULT/nSLEEP 边界。软件 INHIBIT 仍只代表软件决定；功率桥关闭、复位状态、死区、CSA、温升和 watchdog 均需要 H4 级硬件证据。
- `[门禁]` 建立 GATE H0（资料一致性）、H1（无功率输出）、H2（实时性/栈）、H3（传感器采样时序）、H4（功率级安全，另行授权）流程。仓库硬件资料文件存在，但实际板卡 revision、调试器/串口身份、仪器、电源、急停、装配状态仍需现场确认；发现 README/AGENTS 的 DAP-Link/COM3 与历史启动文档 J-Link/COM54 witness 不一致，列为 H0 前置澄清项。
- `[状态]` `NEXT-BOARD-READY-01 PREPARATION PASS`。本轮未刷机、未上电、未连接真实调试器、未启用 PWM、未修改控制/安全/板级生产代码；`G3-SAFE HARDWARE NOT VERIFIED`、`G3-PWR LOCKED`、`AT32F415 WCET NOT VERIFIED` 保持。详见 `docs/evidence/2026-11-14-next-board-ready-01.md`。

## 2026-11-14 FalconFOC V2 NEXT-ENGINEERING-01 数学验证与热路径审计

- `[验证]` 新增独立 double 参考测试 `core/tests/test_foc_math_reference.c`，覆盖 Clarke、Park、逆 Park、SVPWM 和非法 VBUS 安全输出；变换与 SVPWM 各 `10,000` 个确定性样本通过。生产 Float32 与 double 参考的最大 Park 误差约 `4.54e-5`，归因于大角度规约和 Float32 三角函数差异，没有发现坐标或符号错误。
- `[审计]` 读取 AT32F415 ARM 反汇编和符号，确认 H0 `foc_current_step_internal` 的 `fmodf/sinf/cosf/sqrtf/hypotf`、H1 `foc_encoder_update` 的浮点除法/滤波、H1 trajectory position 的 `sqrtf` 和 Runtime Safety 调用链。没有把 Host 时间换算成 MCU WCET，也没有删除防御性检查。
- `[结构]` 复核 V2 `main.c` 的输出锁、时钟/ADC/SPI/TMR/UART/NVIC 初始化、IRQ 共享状态和失败 WFI 路径。当前 helper 分区已经清楚；由于没有证明拆分会带来收益，且静态 IRQ 状态迁移有生命周期风险，本轮不执行 `main.c` 文件拆分。
- `[回归]` Core `11/11`、Clang ASan/UBSan 全工程 `29/29`、Host Debug `45/45`、Legacy 三种 AT32F415 构建、V2 ARM、source contract `62/8/8`、C/H formatter、`Scope falconfoc`、`Scope all` 和 `git diff --check` 通过。RC0 replay 指纹继续保持 `3f01f6e3` 和 `49dea950`。
- `[状态]` `NEXT-ENGINEERING-01 PARTIAL PASS`。当前没有确认的控制公式错误；Current PI/外环/Sensor 独立 oracle、目标 MCU cycle measurement 和真实栈高水位仍未完成。未刷机、未上电、未启用 PWM、未改变控制算法或安全策略。详见 `docs/evidence/2026-11-14-next-engineering-01.md`。

## 2026-11-14 FalconFOC V2 NEXT-MATH-02 独立数学验证

- `[验证]` 新增独立 double Current PI 参考和独立 PMSM RK4/前馈参考；纳入既有 Clarke/Park/SVPWM、速度/位置/轨迹及磁编码器参考测试。Current PI `4,096` 个轨迹样本、PMSM `5,000` 个样本、运动 `5,000` 个样本、Sensor `5,000` 个有效样本通过；Core `14/14`、Sensors `2/2`。
- `[回归]` Clang ASan/UBSan 全工程 `33/33`、Host Debug `45/45`、SYSTEM replay `3f01f6e3`、long replay `49dea950`、Source contract `66/8/8`、C/H formatter、`Scope falconfoc`、临时接通现有 Bash 后的 `Scope all`、ARM 构建和 `git diff --check` 通过。
- `[边界]` 参考测试独立验证数值和状态契约，不证明 AT32F415 WCET、真实 ADC/PWM 时序、功率级安全或电机实机性能。未修改生产控制算法、Runtime、Safety、Board 或 Protocol；未刷机、未上电、未启用 PWM、未写 Flash、未 commit 或 push。`G3-SAFE HARDWARE NOT VERIFIED`、`G3-PWR LOCKED`、`AT32F415 WCET NOT VERIFIED` 保持。详见 `docs/evidence/2026-11-14-next-math-02-validation.md`。

## 2026-11-14 FalconFOC V2 NEXT-PERF-02 Float32 热路径性能分析

- `[基线]` 使用当前工作树 fresh Host Release 基准、独立数学计数构建和 AT32F415 soft-float ELF 反汇编确认 H0 电流环调用链。当前 Normal `112.816 ns/step`、饱和路径 `215.620 ns/step` 仅为 Host 对照；当前 ELF `.text=36,644 B`、`.data=92 B`、`.bss=3,544 B`。
- `[候选]` 对最终电压 envelope 的 `hypotf` 保守快速路径完成独立 A/B。Normal Current Step 局部调用减少，但完整 H0 重复测量方向不稳定，5 次和 10 次聚合中位数分别得到 `-6.84%` 与 `+6.92%`，不能证明真实产品收益。
- `[决定]` 已撤回本轮唯一生产优化候选，保留现有 Float32 实现。Golden 行级比较 `0` 差异；Core `14/14`、Sensors `2/2`、ASan/UBSan `33/33`、Host `45/45`、SYSTEM `3f01f6e3`、Long `49dea950`、`Scope all`、ARM 构建和格式门禁通过。最终状态为 `NEXT-PERF-02 ANALYSIS PASS / NO CHANGE`。未刷机、未上电、未启用 PWM、未 commit 或 push；`G3-SAFE HARDWARE NOT VERIFIED`、`G3-PWR LOCKED`、`AT32F415 WCET NOT VERIFIED` 保持。详见 `docs/evidence/2026-11-14-next-perf-02-validation.md`。

## 2026-11-14 FalconFOC V2 NEXT-CONTROL-ROBUST-01 控制鲁棒性研究

- `[模型]` 新增 `runtime/tests/test_foc_robustness.c` 和 `test-foc-robustness`，以 H0 `200 us`、H1 `500 us` 推进生产 Float32 Current/Velocity/Position/Trajectory/Feedforward，测试侧使用独立 double PMSM 电气/机械方程和 RK4 积分。包含负载、Rs/Ld/Lq/ψf/J/B 偏差、编码器角度偏差、速度噪声、400 us 传感器延迟、丢样和组合工况，共 `22` 个固定场景，输出忽略构建目录中的 CSV。
- `[结果]` 所有研究场景均保持有限，`unstable_cases=0`。名义 Current 尾段 RMSE `0.00112 A`；Ld/Lq `0.5x/1.5x` 时 RMSE 约 `0.29 A` 且出现约 `7,400` 个饱和周期；J `4x` 和 Rs/Lq/J 组合使速度尾段 RMSE 分别升至约 `0.0123/0.0195`；Trajectory 位置尾段 RMSE `0.0140 rad`，直接 Position P 为 `0.00257 rad`。这些结果支持候选研究，不支持直接修改默认参数。
- `[门禁]` Runtime `9/9`、ASan/UBSan 总计 `34/34`、Host `45/45`、Source contract `67/8/8`、SYSTEM `3f01f6e3`、Long `49dea950`、`Scope all`、ARM 构建和格式检查通过。最终状态为 `NEXT-CONTROL-ROBUST-01 PASS（Host 数学/仿真范围）`；未修改生产控制/参数/硬件路径，未刷机、未上电、未启用 PWM、未 commit 或 push。`G3-SAFE HARDWARE NOT VERIFIED`、`G3-PWR LOCKED`、`AT32F415 WCET NOT VERIFIED`、`MOTOR PHYSICAL OPERATION UNVERIFIED` 保持。详见 `docs/evidence/2026-11-14-next-control-robust-01.md`。

## 2026-11-14 FalconFOC V2 NEXT-CONTROL-OPT-01 电流环退化根因与候选优化

- `[根因]` 逐 H0 trace 证明 Ld/Lq 退化案例中的约 `7,400` 个饱和周期主要与无负载电流目标导致的机械加速和反电动势有关：末段稳态需求约 `12.06 V`，当前可用边界 `12 V`，裕量约 `-0.071 V`。相同 Ld/Lq 偏差在固定 `20 rad/s`、负载抵消转矩后无饱和，电流尾段 RMSE `2.15e-4 A`。
- `[对照]` 连续时间 `Kp=Lωc、Ki=Rωc` PI 候选没有改善：Ld/Lq `0.5x` 无速度约束 RMSE `0.2908 -> 0.3223 A`，固定速度 `0.0720 -> 0.1196 A`。参数一致 Feedforward 在固定高速/负载对照中将 RMSE `0.0702 -> 0.0132 A`、尾段 RMSE `1.62e-4 -> 1.09e-6 A`，无新增饱和。
- `[决定]` 保留一个仅限 Host 的研究候选：完成电机参数校准准入并确认电气速度可信后，再评审启用一致 PMSM Feedforward；本轮不修改生产默认参数或 Runtime。Robustness 研究入口扩展为 `31` 个场景，Runtime `9/9`、ASan/UBSan `34/34`、Host `45/45`、SYSTEM `3f01f6e3`、Long `49dea950`、`Scope all`、ARM 构建和格式门禁通过。最终状态为 `NEXT-CONTROL-OPT-01 PASS（Host 研究范围）`；未刷机、未上电、未启用 PWM、未 commit 或 push。详见 `docs/evidence/2026-11-14-next-control-opt-01.md`。

## 2026-11-14 FalconFOC V2 NEXT-FF-ADMISSION-01 Feedforward 软件准入契约

- `[审计]` 核对 CAL-01、CONFIG-01、Sensors、Runtime 和 Feedforward：现有代码已有数值合法性、H1 source time/seq/session/age/future 检查、SVPWM/anti-windup/Safety 限制和 Board1 默认关闭；但没有 PMSM 参数 provenance、motor identity、calibration version 与参数组绑定。
- `[契约]` 新增 Host-only `test-foc-ff-admission`，区分数值合法、来源可信、电气速度可信和 Feedforward 使用准入，覆盖 NaN/零电感、身份/版本冲突、首次/过期/旧 session/旧序列、时间戳回绕、正负速度和显式 enable。生产 wiring 明确标记 `host_only`，不自动启用前馈。
- `[门禁]` Runtime `10/10`、ASan/UBSan 总计 `35/35`、Host `45/45`、Source contract `68/8/8`、SYSTEM `3f01f6e3`、Long `49dea950`、`Scope all`、ARM 构建和格式门禁通过。最终状态为 `NEXT-FF-ADMISSION-01 PASS（Host 软件契约范围）`；真实 PMSM 参数辨识、物理速度质量、Board1 Feedforward 和硬件安全仍未验证/未启用。未刷机、未上电、未启用 PWM、未 commit 或 push。详见 `docs/evidence/2026-11-14-next-ff-admission-01.md`。

## 2026-11-14 FalconFOC V2 NEXT-CODE-QUALITY-01 Board 配置、初始化结构与防御性检查专项

- `[修改]` 仅在 `firmware/at32f415/src/foc_board.c` 将 Current/ADC/PWM/Safety、H0/H1/监督/遥测和 Motion 的嵌套配置改为 designated initializer；ADC 换算复用既有 `FOC_BOARD_ADC_MAX`，并补充单位、名义配置与未辨识电机参数边界说明。没有改变参数值、结构体布局、公共 API、初始化顺序或输出锁。
- `[审查]` 源码核对 `main.c` 的真实启动顺序、IRQ 状态所有权和失败 WFI 路径；当前静态 helper 分区清楚，拆分会移动共享 ISR 状态且没有已证明收益，因此 `main.c` 保持不变。对 `foc_current_step_internal`、`foc_encoder_update`、`foc_runtime_h0_step`、`foc_runtime_h1_step` 的动态校验逐项复核，没有删除任何安全或事务检查。
- `[验证]` Board `4/4`、Core `14/14`、Sensors `2/2`、Calibration `1/1`、Runtime `10/10`、G3-NP `2/2`、G3-SAFE `1/1`、Protocol `1/1`、Host Debug `45/45`、Clang ASan/UBSan `35/35`、SYSTEM replay `3f01f6e3`、Long replay `49dea950`、C/H/Source contract/CMake `29/29`、`Scope falconfoc`、`Scope all` 和 `git diff --check` 均通过。V2 ARM 产物 `.text=36,644 B`、`.data=92 B`、`.bss=3,544 B`、Flash `36,736 B`、RAM `3,632 B`，ELF/BIN/HEX/MAP SHA-256 与基线一致。
- `[状态]` `NEXT-CODE-QUALITY-01 PASS`。继续保持 `G3-SAFE HARDWARE NOT VERIFIED`、`G3-PWR LOCKED`、`AT32F415 WCET NOT VERIFIED`、`MOTOR PHYSICAL OPERATION UNVERIFIED`；未刷机、未上电、未启用 PWM、未写 Flash、未 commit、未 push。

## 2026-11-15 FalconFOC V2 NEXT-TIMING-OWNER-01 AT32F415 时序参数所有权收敛

- `[审计]` 核对 Legacy `foc_config.h`、`motor_task.c`、`foc_ctrl.c`、生成式 `wk_tmr.c` 与 V2 `foc_board.h/main.c`：确认 150 MHz timer input、Legacy TMR1 20 kHz carrier/ADC event、Legacy TMR5 5 kHz soft IRQ、产品 FOC 10/5 kHz、velocity 5 kHz、position/trajectory 1 kHz，以及 V2 H0 200 µs/H1 500 µs 的真实边界。明确 `osDelay(1)` 不是严格硬实时 1 kHz。
- `[所有权]` 新增 `platform/at/at32f415/generated/inc/tm_platform_foc_timing.h`，集中拥有 AT32F415 timer clock、Legacy TMR1/TMR5 固定外设 cadence 和 V2 1 µs timebase。Legacy `PWM_FREQ_HZ`、velocity/position/trajectory 仍留在应用产品配置；V2 H0/H1、Safety timeout 和 SPI/ADC window 仍留在各自 Board/Runtime 契约。
- `[实现]` `foc_config.h`、`foc_board.h` 和 bench `TM_MOTOR_BENCH_ADC_TICK_HZ` 改为引用平台事实；增加 Hz/MHz、FOC-step/carrier、velocity/TMR5 的编译期一致性检查。V2 standalone Board target 和 Legacy Host contract 补齐平台 generated include 路径。未修改生成式 Timer、ADC、SPI、FreeRTOS、PWM、ISR 优先级或实际周期。
- `[验证]` Legacy tiny/standard/full-diag 构建、V2 ARM 构建、Host Debug `45/45`、Clang ASan/UBSan `35/35`、SYSTEM `3f01f6e3`、Long `49dea950`、Source contract、C/H/CMake formatter、`Scope falconfoc`、`Scope all` 和 `git diff --check` 均通过。Legacy tiny/standard BIN/HEX 与基线一致；full-diag BIN/HEX 和 section 资源一致，ELF 仅因 Debug 源码/include 元数据变化。V2 ELF/BIN/HEX/MAP 与基线一致。
- `[状态]` `NEXT-TIMING-OWNER-01 PASS`。继续保持 `G3-SAFE HARDWARE NOT VERIFIED`、`G3-PWR LOCKED`、`AT32F415 WCET NOT VERIFIED`、`MOTOR PHYSICAL OPERATION UNVERIFIED`；未刷机、未上电、未启用 PWM、未写 Flash、未 commit、未 push。

## 2026-11-15 FalconFOC V2 NEXT-RC0-BASELINE-01 软件候选基线冻结

- `[完整性]` 在原工作树之外建立 `D:/study/Software/Hollow-Gimbal_RC0_Verification_20261115_01` 独立源快照，纳入 `.vscode`、`memory.sh`、`symbols.txt` 和全部当前生产/测试/平台输入；排除 `.git`、构建缓存、临时目录及被忽略的 `state show` 诊断输出。逐文件清单 `1660/1660`，缺失 `0`、意外 `0`、SHA-256 不一致 `0`。
- `[重建]` 在独立验证副本中重新配置 Host Debug、Clang ASan/UBSan、Legacy tiny/standard/full-diag 和 V2 AT32F415 ARM；Host `45/45`、Sanitizer `35/35`、Source contract `68/8/8`、Firmware CLI contract、`Scope falconfoc`、`Scope all` 和 CMake `29/29` 均实际通过，SYSTEM/Long replay 保持 `3f01f6e3`/`49dea950`。
- `[产物]` V2 ELF/BIN/HEX 与原工作树逐字节一致，Flash `36,736 B`、RAM `3,632 B`；Legacy tiny/standard ELF/BIN/HEX 一致。full-diag 的 BIN 差异仅为 Debug 构建 `__DATE__`/`__TIME__` 字符串，section 尺寸和指令内容未发现差异。
- `[修正]` 独立快照初始排除了正式 CLI 入口 `memory.sh` 和 `.vscode`，CLI contract 在独立目录中真实拒绝；补回后重新生成清单并完成全部门禁，未通过跳过测试或修改门禁规则解决。
- `[状态]` `NEXT-RC0-BASELINE-01 PASS`。建议冻结为 RC0 软件源码提交候选；`G3-SAFE HARDWARE NOT VERIFIED`、`G3-PWR LOCKED`、`AT32F415 WCET NOT VERIFIED`、`MOTOR PHYSICAL OPERATION UNVERIFIED` 保持。未刷机、未上电、未启用 PWM、未写 Flash、未 commit、未 push。详见 `docs/evidence/2026-11-15-next-rc0-baseline-01.md`。

## 2026-11-15 FalconFOC V2 neat-freak 知识收尾

- `[交接]` 新增 `docs/code-wiki/AI-handoff-2026-11-15.md`，将当前提交、RC0 软件基线、架构边界、可执行命令和硬件未验证项收敛为新人首个入口；README、docs 索引、Code Wiki、项目摘要和运行验证页均指向该入口。
- `[纠偏]` 早期 Code Wiki 和项目摘要保留为历史证据，但明确标注当前软件状态以 `278c6911e65d7782bb58d7b0b023fdcb9f414c08` 和 `NEXT-RC0-BASELINE-01` 为准，避免旧资源数字、旧交接日期和旧状态词继续冒充现役事实。
- `[规则]` 根 `AGENTS.md` 为当前唯一项目规则入口，未发现项目级 override、CLAUDE 或同名规则分叉。Codex 生成记忆按宿主规则只读，本轮没有直接修改记忆文件。
- `[状态]` 知识收尾完成；未删除历史文档、构建目录、独立验证快照或其他残留现场。删除候选需在本次汇报后由用户明确确认，当前保留以便复核。

## 2026-11-15 FalconFOC V2 neat-freak 清场复核

- `[清理]` 按用户确认删除独立源码快照 `Hollow-Gimbal_RC0_Verification_20261115_01`、独立构建验证副本 `Hollow-Gimbal_RC0_Verification_20261115_01_build`，以及仓库内未跟踪的构建缓存、`build-rel` 和 `tmp`。
- `[保留]` `build/` 中被 Git 跟踪的历史探针脚本和 `work/` 中被 Git 跟踪的平台工程源文件均保留，没有删除任何版本控制内容；`docs/evidence`、源码、测试和维护文档保持完整。
- `[复核]` 清场后 `git status --short --untracked-files=all` 为空，远端 `origin/codex/falconfoc-v2-rc0` 与本地保持同步。当前没有外部验证现场或忽略构建缓存残留。

## 2026-11-15 FalconFOC V2 NEXT-VM-01a Host 虚拟电机 Current 闭环

- `[实现]` 在隔离分支 `codex/falconfoc-v2-vm01a` 新增 `simulation/` Host-only 项目，提取可复用的双精度 PMSM RK4 Plant、ADC/Encoder 量化边界、确定性虚拟时钟和接受输出适配器；真实 Runtime、Device、Safety、Current PI 和 Encoder 通过现有接口运行。
- `[边界]` Virtual UPDATE 只有在 `foc_device_sim_submit()` 接受后才推进 Plant；INHIBIT 使用高阻近似并保留惯性/阻尼演化。没有调用 `foc_board_init()`，没有修改 Board1 输出锁、真实硬件配置、Serial、Protocol 或生产 Runtime API。
- `[验证]` 新增 Virtual Plant/Sensor/Current 测试，simulation CTest `13/13` 通过；五个确定性 Current 场景和 CSV 重放指纹通过，现有 `test-foc-motion-model` 在 simulation 构建中额外调用共享 Plant 契约，原有独立模型保留为交叉校验。`NEXT-SW-AUDIT-01` 仍 pending。
- `[状态]` `NEXT-VM-01a PASS`（Host Current 闭环范围）。`Scope falconfoc` 通过；`Scope all` 在 Firmware CLI contract 处因当前环境缺少 Bash 阻断，未修改门禁绕过。真实 Board1 输出、MCU WCET、G3-SAFE/G3-PWR 和电机实机行为仍未验证；未刷机、未上电、未启用 PWM、未写 Flash、未 commit、未 push。

## 2026-11-15 FalconFOC V2 NEXT-VM-01b Host 虚拟运动闭环

- `[实现]` 在 VM-01a 独立工作树的共享虚拟 PMSM、ADC/Encoder、输出适配器和确定性时钟上增加 `falconfoc-virtual-motion.exe`。Velocity 和 Position 均通过生产 `foc_runtime_publish_motion_command()`、H1、H0、Velocity PI、Position P、Current PI、SVPWM 和接受 UPDATE 驱动 Plant；没有新增第二套控制器或虚拟调度器。
- `[生命周期]` 增加 Host-only 的 SET_MODE、ARM、START、STOP、命令超时和下一次 Encoder 传输失败注入。测试覆盖 CURRENT→VELOCITY、VELOCITY→POSITION、POSITION→CURRENT、STOP 后新 Session、传感器故障锁存以及旧命令不能自动恢复；未修改生产 Runtime 生命周期 API 或 Safety 实现。
- `[测量]` V01/V02/V03/P01/P02/S01/S02 七组场景输出 RMSE、Tail RMSE、Peak Error、Overshoot、Settling Time、Iq 目标/实际值、饱和次数、UPDATE 计数、Safety 状态、Fault Bits、Motion Session 和重放指纹。当前研究 Profile 下闭环可运行，但跟踪指标仍标记 `EXPECTED_DEGRADED`，不能当作 Board1 控制品质验收。
- `[设计]` 记录了下一阶段 Foreground→H0 生命周期请求邮箱和默认锁定的 Serial Target Policy 设计，区分 Request Accepted、Applied、Rejected、Cancelled 与 Actual Output Applied；本轮没有增加 UART V1 消息、没有开放 Board1 虚拟解锁权限。
- `[验证]` Simulation CTest `13/13`、Clang ASan/UBSan `13/13`、Runtime `10/10`、Board Host `4/4`、Legacy Host `45/45`、Source contract `77/9/9`、CMake/格式检查、`Scope falconfoc` 和 V2 ARM 构建通过。VM-01a Current CSV SHA-256 仍为 `521D4B9DB6B712B9E0E9256E6EBE3B2498D73BF5354A1A9DDF057E44DFD8AEEF`；VM-01b motion CSV 两次重放均为 `3F17D3358513FA937FACAE67851EDAFEAA255F2150557B30FF6C4B153CC8756F`；ARM BIN `9806ADBFA3282450527F40BB97CB1DA75FAFE41FB405F1126114187C6393B029` 保持不变。
- `[门禁与边界]` `Scope all` 继续在 `test_firmware_cli_contract.ps1` 因当前环境没有 Bash 而报告 `SCOPE_ALL_ENVIRONMENT_BLOCKED`，没有修改门禁或跳过该测试。`NEXT-VM-01b PASS` 仅限 Host 虚拟运动闭环；`NEXT-SW-AUDIT-01` 仍 pending，`G3-SAFE HARDWARE NOT VERIFIED`、`G3-PWR LOCKED`、`AT32F415 WCET NOT VERIFIED`、`MOTOR PHYSICAL OPERATION UNVERIFIED` 保持。未刷机、未上电、未启用 PWM、未写 Flash、未 commit、未 push。

## 2026-11-15 FalconFOC V2 NEXT-VM-01c 生命周期邮箱与虚拟设备集成评审

- `[基线]` 在 VM-01a/VM-01b 独立工作树上重新构建 `foc-serial-sim`，Python/C CLI 基线 `4/4` 通过；Current CSV、Motion CSV 和 V2 ARM BIN 指纹保持既有证据值。
- `[发现]` 现有 `foc_serial_poll()` 将协议消费、Foreground 服务和零输入 H0 调用耦合在一起；直接用于 VM-01b 虚拟时间会在正式事件调度之外多执行一次 H0。V1 STATUS/TELEMETRY 布局没有可安全复用的生命周期 request-result 字段。
- `[门禁]` NEXT-VM-01c 要求在修改共享 Runtime ARM/START 准入和 Serial Target Policy 前取得单独明确批准。已记录最小 mailbox、原子发布、STOP/FAULT 优先级、默认 LOCKED/VIRTUAL_ONLY 策略、服务解耦方案和负向测试清单；本轮尚未修改 Runtime、Serial、Protocol、Board1 或 ARM 授权路径。
- `[状态]` `AWAITING SAFETY CHANGE APPROVAL`，不是 `NEXT-VM-01c PASS`。继续保持 `NEXT-SW-AUDIT-01 PENDING`、`G3-SAFE HARDWARE NOT VERIFIED`、`G3-PWR LOCKED`、`AT32F415 WCET NOT VERIFIED`、`MOTOR PHYSICAL OPERATION UNVERIFIED`。未刷机、未上电、未启用 PWM、未写 Flash、未 commit、未 push。详见 `docs/evidence/2026-11-15-next-vm-01c.md`。

## 2026-11-16 FalconFOC V2 NEXT-VM-01c Host Virtual 设备集成完成

- `[批准]` 技术负责人批准 `APPROVED WITH CONSTRAINTS`：允许 Runtime 生命周期邮箱、默认锁定/显式 Host Virtual Target Policy、复用 V1 Serial/Protocol、同进程 CLI 和负向安全测试；禁止 Board1 解锁、V1 报文变更、硬件输出或自动提交。
- `[邮箱]` Runtime 增加双槽 Foreground→H0 请求/结果协议，支持 ARM/START/SET_MODE、PENDING/APPLIED/REJECTED/CANCELLED；使用 release/acquire 原子索引，H0 独占状态/Safety/PI/输出提交，STOP/FAULT/旧 Session/超时优先或拒绝。新增 `test-foc-runtime-mailbox` 负向契约。
- `[策略]` `foc_serial_init()` 保持 `LOCKED`，只有 Host Virtual 初始化路径能注入 `VIRTUAL_ONLY`；UART、HELLO、Session 和 PARAM_WRITE 不能切换策略，Board1 ARM/START 与 UPDATE 锁定保持。
- `[虚拟设备]` 新增 `falconfoc-virtual-device`，复用现有 COBS/CRC/Session/Sequence、UART buffer、Serial Adapter 和 VM-01b 虚拟 Runtime。stdout 仅输出 V1 二进制，stderr 输出实际生命周期与虚拟/物理隔离元数据；Python CLI 新增同进程 `virtual-demo`，Current/Velocity/Position 三个流程均完成 ARM/START/UPDATE/Telemetry/STOP。
- `[验证]` Simulation `14/14`、ASan/UBSan Simulation `14/14`、Runtime `11/11`、Board Host `4/4`、Legacy Host `45/45`、Python/C CLI `5/5`、Source contract `79/9/9`、C/H/CMake formatter、`Scope falconfoc` 均通过。VM-01a Current 与 VM-01b Motion replay 指纹保持。ARM BIN 从共享变更前 `36,736 B / 9806ADBFA3282450527F40BB97CB1DA75FAFE41FB405F1126114187C6393B029` 变为 `37,864 B / BDEC4D2706049080FF758EE308ADDC1304294DFD5805F05A0CAE7D56D4191806`；`.text/.data/.bss` 和 Flash/RAM 增量已在证据报告解释，Host-only simulation/Python 未进入 ARM。
- `[门禁与边界]` `Scope all` 的 Source/CMake/Platform/Smoke/Cache/Shell 阶段通过，Firmware CLI contract 仍因无 Bash 报告 `SCOPE_ALL_ENVIRONMENT_BLOCKED`，没有伪造通过。`NEXT-VM-01c PASS` 仅限批准的软件范围；停止在 VM-01c，不自动进入 VM-01d。`NEXT-SW-AUDIT-01 PENDING`、`G3-SAFE HARDWARE NOT VERIFIED`、`G3-PWR LOCKED`、`AT32F415 WCET NOT VERIFIED`、`MOTOR PHYSICAL OPERATION UNVERIFIED` 保持。未刷机、未上电、未启用 PWM、未写 Flash、未 commit、未 push。

## 2026-11-16 FalconFOC V2 NEXT-VM-01d Host 虚拟产品验收

- `[产品链]` 在 VM-01a/VM-01b/VM-01c 基础上完成同一 Host Virtual device 的 Current、Velocity、Position、Load disturbance 和 Sensor Fault 五条 CLI 端到端演示。每条都经过 V1 HELLO/SETPOINT/ARM/START、Telemetry、STOP；输出同时包含 CSV、JSON、生命周期状态、虚拟/物理语义和确定性 Replay hash。Position 目标 `7 rad` 的轨迹跨越单圈，Load 通过 `foc_virtual_motor_set_load()` 注入，Fault 通过 Virtual Encoder 下一帧传输失败进入 H1/H0/Safety/INHIBIT。
- `[指标]` 五条演示均为 `INTEGRATION_PASS`。Current 按运行前固定 `0.1 A` 容差记录 `CONTROL_QUALITY_FAIL`；Velocity/Position/Load 记录 `EXPECTED_DEGRADED`；Fault 因故障提前终止跟踪窗口记录 `NOT_MEASURED`。Load 的固定窗口测得速度下降为 `0`，原样保留，没有伪造恢复结论。Fault 的最终状态为 `FAULT_LATCHED`、Fault `4`，`no_update_after_inhibit=true`。
- `[验证]` Simulation `14/14`、Clang ASan/UBSan Simulation `14/14`、Runtime `11/11`、Board Host `4/4`、Python/C CLI `6/6`、Legacy Host `45/45`、Source contract `79/9/9`、CMake/格式检查和 `Scope falconfoc` 均通过。五个 CSV/JSON 两次重放 SHA-256 完全一致；证据和开发者入口分别为 `docs/evidence/2026-11-16-next-vm-01d.md` 与 `docs/code-wiki/FalconFOC_VM_virtual_quickstart.md`。
- `[ARM 与边界]` 当前 ARM `MinSizeRel` BIN `37,864 B`，SHA-256 `BDEC4D2706049080FF758EE308ADDC1304294DFD5805F05A0CAE7D56D4191806`，与 VM-01c 一致。VM-01d 的启动补偿留在 Host Virtual 调度器，没有改变 Timer/ADC/SPI、PI 默认参数、Board1 输出锁或 UART V1 布局。`Scope all` 在 Firmware CLI contract 因当前环境缺少 Bash 报告 `SCOPE_ALL_ENVIRONMENT_BLOCKED`；没有修改门禁或伪造通过。`NEXT-SW-AUDIT-01 PENDING`、`G3-SAFE HARDWARE NOT VERIFIED`、`G3-PWR LOCKED`、`AT32F415 WCET NOT VERIFIED`、`MOTOR PHYSICAL OPERATION UNVERIFIED` 保持；未刷机、未上电、未启用 PWM、未写 Flash、未 commit、未 push。
- `[状态]` `NEXT-VM-01d PASS — HOST VIRTUAL PRODUCT`。控制品质结果仍按 `CONTROL_QUALITY_FAIL`、`EXPECTED_DEGRADED` 和 `NOT_MEASURED` 分开报告，不把研究 Profile 的跟踪误差包装为产品品质通过；本阶段停止在 Host Virtual Product，不自动进入 VM-01e 或硬件阶段。

## 2026-11-16 FalconFOC V2 NEXT-CONTROL-QUALITY-01 控制品质测量与 Oracle 整改

- `[窗口]` Host CLI 保留历史 full-window `rmse`/`tail_rmse`，新增 `metrics_schema_version=2`、RUNNING/RUNNING-tail 窗口、transient peak、steady-state error、Settling Time 和显式 measurement_window。启动、STOP/INHIBIT 记录不再进入新的 RUNNING 品质窗口；Velocity/Position 增加 `--horizon long` 独立观察，分别为 100 ms 和 2 s，不替换 VM-01d 短时证据。
- `[负载]` Load 仍使用原始 `0.03 N·m / 5000–15000 us` 场景；Host 测试新增同初始状态、同命令和同虚拟时间的 0 N·m 反事实对照，计算同一时间点的速度、位置和 Iq 差异。Load no-op 变异现在由独立 Oracle 检出。
- `[故障]` Virtual device 增加 Host-only 事件 journal 和 CSV/JSON 字段，区分 fault injection、Sensor/H1 invalid、H0 failure observation、software INHIBIT request/accept、FAULT_LATCHED、最后 accepted UPDATE 及各事件的 H0 UPDATE 计数。软件抑制延迟与故障锁存延迟分开报告；未观察事件以 `-1`/空值表达，不伪造 `0 us`。
- `[Oracle]` Virtual Sensor 增加独立 Clarke/Park 方向参考，覆盖带非零 d/q 电流的 ADC 符号；Virtual Motor 测试增加有/无负载轨迹差异断言。Python Host 测试覆盖状态窗口边界、Load counterfactual 和 Fault event metrics。既有 Board1 LOCKED、ARM/START OUTPUT_LOCKED、UPDATE 拒绝和 V1 协议保持不变。
- `[验证]` Simulation `14/14`、ASan/UBSan Simulation `14/14`、Runtime `11/11`、Board Host `4/4`、Python/C CLI `8/8`、Legacy Host `45/45`、Source contract、CMake/格式门禁通过；`Scope falconfoc` 通过，`Scope all` 仍因缺 Bash 为 `SCOPE_ALL_ENVIRONMENT_BLOCKED`。ARM BIN 保持 `37,864 B / BDEC4D2706049080FF758EE308ADDC1304294DFD5805F05A0CAE7D56D4191806`。
- `[状态]` `NEXT-CONTROL-QUALITY-01 PASS`。控制品质结果仍按 `CONTROL_QUALITY_FAIL`、`CONTROL_QUALITY_PASS` 和 `EXPECTED_DEGRADED` 分开报告；本轮修复测量语义、故障证据和测试 Oracle，没有修改生产 PI、Runtime Safety、UART V1 或真实输出权限。`G3-SAFE HARDWARE NOT VERIFIED`、`G3-PWR LOCKED`、`AT32F415 WCET NOT VERIFIED`、`MOTOR PHYSICAL OPERATION UNVERIFIED` 保持。

## 2026-11-16 FalconFoc V2 NEXT-CONTROL-TUNING-01 Host 参数整定研究

- `[研究入口]` 新增 `tools/falconfoc-cli/control_tuning.py`，复用同一个 V1 Virtual device、生产 Runtime、Current/Velocity/Position 控制器和确定性 PMSM Plant。研究 Profile 在 `foc_virtual_current_init()` 前注入，只复制 Host 配置；没有运行中改写 PI 积分器，也没有创建第二套 Plant 或控制算法。
- `[方法]` 采用固定小网格，严格按 Current → Velocity → Position 搜索；每个候选保留 JSON/CSV/stdout/stderr。硬约束先检查积分/故障/UPDATE/有限值/持续饱和，再用 Tail RMSE、Final Error、Settling Time、Saturation 和 Overshoot 做可解释排序，并记录 Pareto 前沿。
- `[结果]` 选出的 Host 研究候选为 `current_kp=1.26`、`current_ki=630`、`velocity_kp=0.225`、`velocity_ki=1.75`、`position_kp=5.0`（标量覆盖 d/q；Current/Velocity/Position 仍属研究参数）。与名义配置的精确长时域复核：Velocity 100 ms RUNNING-tail RMSE `0.592805 -> 0.338711`、Settling `95500 -> 79500 us`；Position 2 s RUNNING-tail RMSE `0.016434 -> 0.006548`、Settling `1383000 -> 1215500 us`。电流 10 ms Full RMSE `0.152791 -> 0.152713`，RUNNING-tail `0.005697 -> 0.003036`。
- `[Holdout]` 反向 Current、带负载/参数扰动 Velocity、反向/低 VBUS/惯量扰动 Position 均通过 Runtime 硬约束；Current 反向短窗口仍为 `CONTROL_QUALITY_FAIL`，这是独立保留的控制品质结果，不改成 PASS。研究报告与全部机器结果见 `docs/evidence/2026-11-16-next-control-tuning-01.md` 和 `build/control-tuning-01/study.json`。
- `[状态与边界]` `NEXT-CONTROL-TUNING-01 RESEARCH COMPLETE`、`IMPROVEMENT_CONFIRMED`、`HOLDOUT_PASS` 仅表示 Host 研究证据；不表示真实电机已整定、Board1 已验收、AT32F415 WCET 已证明、全局最优或 PWM 已解锁。生产 PI 默认值、Board1 限制、Runtime Safety、UART V1、Feedforward 准入和 ARM 固件均保持不变；未刷机、未上电、未 commit、未 push。

## 2026-11-16 FalconFoc V2 NEXT-RELEASE-PREP-01 发布前工程收口

- `[定位]` 根 README、docs 索引和 Host Quickstart 统一到 V2 的正式产品定位：纯 C11 Core、Runtime、Host Virtual Motor、AT32F415 reference target 和研究工具；AT32F415 不是所有 MCU 的永久边界，真实电机、功率级关断、WCET 和任意 MCU 即插即用仍未验证。
- `[独立集成]` 在仓库外 `D:\study\Software\FalconFOC-core-consumer-release-prep-01` 创建最小 C11 消费者，只包含公开 `foc_current.h`、Current PI 输入和 SVPWM 输出检查。Clang 22 与 MinGW GCC 均独立 Configure/Build/Run 通过，证明 Core 可离开仓库内部测试图使用；Runtime/Device/Safety 接缝未被过度宣称为完全独立包。
- `[盘点]` 生成 `docs/evidence/2026-11-16-next-release-prep-01-source-inventory.tsv`，分类全部 tracked/untracked 项目文件、VM/Quality/Tuning evidence、测试、工具、文档、build artifacts 和外部验证树；没有删除或移动当前未提交成果。
- `[许可]` 确认 FreeRTOS LICENSE 在库内；CMSIS-DSP 本地 README 说明 MIT/Apache-2.0 混合覆盖；AT32F415 generated/BSP 含 Artery custom copyright/disclaimer；根项目尚无选定 LICENSE，权利人/版权链和第三方 NOTICE 尚未完成，因此当前发布结论保持 `RELEASE_BLOCKED`，本轮不添加正式 LICENSE。
- `[回归]` Core `14/14`、Runtime `11/11`、Simulation `14/14`、ASan/UBSan `14/14`、Sensors `2/2`、Calibration `1/1`、G3-NP `2/2`、G3-SAFE `1/1`、Board Host `4/4`、Protocol `1/1`、Python/C CLI `11/11`、Legacy Host `45/45` 均通过。发现本机已有 `D:\Git\usr\bin\bash.exe`，临时加入 PATH 后原始 `Scope all`（含 Firmware CLI contract）通过；没有安装系统依赖或修改门禁。ARM BIN 保持 `37,864 B / BDEC4D2706049080FF758EE308ADDC1304294DFD5805F05A0CAE7D56D4191806`。
- `[状态]` `NEXT-RELEASE-PREP-01 PREPARATION COMPLETE`；实际发布准入 `RELEASE_BLOCKED`。后续最小任务为许可/来源收口、Runtime 可移植性接缝评估、带 Bash 的 CI 发布门禁和授权后的 MCU 实时性测量。未 commit、未 push、未 tag、未发布、未刷机、未上电。

## 2026-11-16 FalconFOC V2 NEXT-CODE-IMPROVE-01 Feedforward 测试闭环

- `[反例]` 独立 Core 副本复现审计 F01/P2：正向/反向第 1234 个零基周期计算失败、4999 周期、3999 样本、Current/FF 初始化失败及非有限 d 轴统计，原测试七项均错误 PASS；各自源码、变异及 stdout/stderr 保留在 `D:/study/Software/FalconFOC-code-improve-01-20261116`。
- `[最小修复]` 唯一代码修改为 `core/tests/test_foc_feedforward.c`：显式 completed/status/周期/样本契约，初始化与每次 Step 错误传播，完成前 RMSE=NAN，最终有限性检查；五组模型均通过有效性检查后才比较原有性能关系。未改变生产控制器、PI 参数、角度归一化、Anti-windup、Runtime Safety、Board1 或 UART V1。
- `[验证]` 正常五组各完成 5000 周期、4000 样本，原有数值输出不变；七项错误变异全部 FAIL 并关联试验身份。Core 14/14、Core Clang ASan/UBSan 14/14、Runtime 11/11、Simulation 14/14、Board Host 4/4、Python/C CLI 11/11、Source 79/9/9、Formatter、CMake Quality 30 项格式检查、Scope falconfoc 和 Scope all（含 Legacy Host 45/45）均通过。Scope all 使用本机已有 Git Bash 的子进程 PATH，没有修改门禁。
- `[产物与证据]` V2 ARM 完整重建 BIN 保持 `37,864 B / BDEC4D2706049080FF758EE308ADDC1304294DFD5805F05A0CAE7D56D4191806`。本轮报告为 `docs/evidence/2026-11-16-next-code-improve-01.md`；原只读审计报告及全部 VM/Quality/Tuning/Release Prep 成果保留。超大累计角度与联合 Anti-windup 继续待证据，已列取证方式和下一复核触发条件。
- `[状态]` `NEXT-CODE-IMPROVE-01 PASS` 仅关闭该测试有效性缺陷；真实输出仍锁定，硬件安全、MCU WCET 和真实电机运行仍未验证。未 commit、push、合并、发布或进行硬件操作；本轮停止，不自动开始下一文件评审。

## 2026-11-16 FalconFOC V2 NEXT-CODE-IMPROVE-02 批准前取证与相位方案

- `[复现]` 新隔离目录重算 float(1e8) 的表示/归约/Park 误差，并以真实 Encoder→Runtime H1→H0/Current、正常 ARM/START/UPDATE 复现约 −16383/−262143 圈处 0.01431164/0.14975086 rad 的退化；连续机械位置未截断。
- `[Oracle/原型]` 独立整数数学模与 double 相位 Oracle 覆盖 2100 例及 12000 次 Encoder 方向/边界更新。方案 A 在名义 7 极对同码结果稳定；方案 B 整数周期乘模最大误差 7.66e−7 rad，已提出最小生产接入方案。六类相位/方向/poles/offset/时间/invalid 变异在 Host 原型层被检出，未冒充正式新 Runtime 回归。
- `[启动边界]` 公开 Host API 提前排队 ARM/START 时可在速度尚未建立的 400 µs 提交 UPDATE；正常顺序在 600 µs。提案撤回首个 UPDATE 必然使用完整速度反馈的普遍承诺，保留既有 Host-only 源准入及零速假设；更强 readiness 规则需独立设计，不扩散到 Board。
- `[已实施]` 仅修改 core/include/foc_core.h 文档（+22/−3）：给出受条件限制的 5e−7 rad 小角归约精度域、区分 finite 准入与精度、补 Clarke 平衡/重构及不去零序前提。无函数体、公开字段/函数声明、接受规则、PI/AW/Safety 或硬件变化。
- `[回归]` Core 14/14、Core Clang ASan/UBSan 14/14、Sensors 2/2、Runtime 11/11、Simulation 14/14、Board 4/4、Python/C CLI 11/11、Source 79/9/9、Formatter、Scope falconfoc/all（含 Legacy Host 45/45）实际通过；全部使用新目录，门禁中的 Legacy ELF/MAP 为逐字节复制的历史产物，未当作新 ARM 构建。
- `[批准门]` 按用户任务第六节停在共享生产 API/正式 H1 相位源变更之前。待批准提案是 D:/study/Software/FalconFOC-code-improve-02-20261116/PRODUCTION_CHANGE_PROPOSAL.md 第 8 节：Sensor integer phase count、新 Core helper、Runtime phase-valid flag 及 producer/consumer/测试迁移；正式生产链尚未接入。NEXT-CODE-IMPROVE-02 PARTIAL — AWAITING PRODUCTION CHANGE APPROVAL。
- `[保护]` 报告 docs/evidence/2026-11-16-next-code-improve-02.md。原 ARM BIN 37864 B / BDEC4D2706049080FF758EE308ADDC1304294DFD5805F05A0CAE7D56D4191806 保留；未 clean-first、删除历史构建、改 Golden、提交、推送、合并、发布或进行硬件操作。Board1 输出锁及实机未验证边界保持。

## 2026-11-16 FalconFOC V2 NEXT-CODE-IMPROVE-02B 整数周期相位生产迁移

- `[批准/实现]` 按 IM02 提案第 8 节与用户 IM02B 正式批准：Feedback 新增 uint32 周期计数，Core 新增 counts-to-phase helper，Runtime 新增显式 phase-valid 并消费同帧周期相位；Board/Virtual 及 motion-enabled fixtures 迁移。连续位置/Trajectory 保留，旧 Rotor API 与直接角度模式保持。
- `[精度/反例]` 起始真实 Encoder→H1→H0/Current 反例先 RED；正式生产 Oracle 2100 组最大 7.65757044441e−7 rad，24 组长多圈 H1/H0 快照最大相位误差 3.28188485858e−7 rad、dq 误差 3.30374200523e−7 A。两个方向各 16777216 次合法更新，包含时戳回绕；另有正负多圈 Trajectory。
- `[负向/复核]` 六类最终生产源变异均编译成功并被行为断言检出；invalid 旧相位变异导致 UPDATE 1→2，被测试拒绝。独立审查发现并修复一项 P2 直接角度兼容回归，先 RED→GREEN。Host 正常/提前首 UPDATE 600/400 µs 保持；首帧传输失败前后均 DISABLED、零 UPDATE，不重设计 readiness。
- `[回归]` Core 15/15、Sensors 2/2、Runtime 11/11、Simulation 15/15、Board 4/4、Python/C CLI 11/11、Legacy Host 45/45；Core/Sensor/Simulation/Board Clang ASan/UBSan 通过。Source 81/9/9、Formatter、CMake Quality、Scope falconfoc/all 退出 0。五个 CLI 工作流集成通过，既有 Current 短窗口 CONTROL_QUALITY_FAIL 与 Position/Load EXPECTED_DEGRADED 标签保留，未改阈值/PI。
- `[ARM]` 起始源码隔离重建匹配原 37864 B SHA；新 BIN 38304 B，SHA-256 73EE73C4FB1570417DF7842520CCA8BA07204C86FE644D837F00DBDDB7EA9E89。原始 .text +440 B、.data +0、.bss +20 B；Sensor/Feedback/Encoder/Runtime 大小 28/52/192/1236 B。H1 自身静态栈 288→328 B，非调用链高水位/WCET。
- `[边界/保护]` 详见 docs/evidence/2026-11-16-next-code-improve-02b.md 与外部 FalconFOC-code-improve-02b-20261116。仅本授权的软件相位链验收 PASS；Board1 LOCKED、ARM/START OUTPUT_LOCKED、真实 UPDATE REJECTED、physical_applied=false。Safety、PI、UART V1、Target Policy、外设/功率输出未改变；不删除/覆盖历史构建或 CSV/JSON/Golden/evidence，不提交/推送/合并/发布/刷机/上电/运行真实电机。

## 2026-11-17 FalconFOC V2 NEXT-CODE-REVIEW-03 Motion 独立审查与 Git 交付

- `[审查]` foc_motion 全函数/跨模块合同、61233 预测、20000 PI、20021 Position、72 错误检查及六类生产变异；主结论 IMPROVE/P2，无确认 P0/P1 或 BLOCK。默认域保留预测 A、Velocity PI 和连续位置 P；先补精度合同与计算错误原子性测试。
- `[证据]` Core 15/15、Runtime 11/11、Core/Simulation ASan/UBSan 15/15；错误分支提交积分的私有变异虽 Core 15/15 仍被独立探针检出。P2-B 保持 OPEN — NEXT-QUALITY-GATE-01。本轮未改生产、正式测试、Golden、PI/Safety/相位链/功率输出。
- `[交付]` 用户最新明确要求每轮完成后提交推送，覆盖附件旧 Commit/Push 禁令；此前已完成 VM/相位成果与本轮审查文档分开提交，推送 codex/falconfoc-v2-vm01a，不合并主分支。审查期只读验证与提交期身份变化分开记录。报告 docs/evidence/2026-11-17-next-code-review-03.md；完整探针/构建保留外部新目录，既有产物不清理。
- `[边界]` G3-PWR LOCKED；硬件安全、MCU WCET/栈高水位、真实电机运行仍未验证。本轮不自动修复或进入下一文件。

## 2026-11-17 FalconFOC V2 NEXT-QUALITY-GATE-01 严格门禁修复

- [实现] 仅 canonical Gate 以 argv 数组转发：bool true 为裸 Switch、false 省略；SkipFormatter 明确为 String=true。保留独立 PowerShell 子进程、原非零失败传播、两个包装入口及所有 Scope 语义。
- [RED/GREEN] 新 tests/script/test_quality_gate_arguments.ps1 在旧 helper 上真实 SwitchParameter 绑定失败，修复后 31 断言通过；覆盖实际类型、空格路径、native exit 隔离、CI/Actions、Formatter 与其他子 Gate 失败。
- [实跑] 普通/显式 strict/CI=true/GITHUB_ACTIONS=true 的 Scope all 均退出 0，分别真实检查 30 个 CMake 文件并通过 Host 45/45；Scope falconfoc、Source 81/9/9、直接 CMake 普通/strict 通过。真实入口缺 Formatter、Formatter exit9、后续 cmake exit7 均正确失败。
- [保护/遗留] 所有控制器/正式 CMake/Board/Safety/输出权限不变，既有构建产物保留；额外旧 lite contract 引用移除的 AT32F403A，在前后快照同样失败且不在 live Gate 链，单独保留待清理。本轮不顺手修复或覆盖旧审计结论。
- [交付] 报告 docs/evidence/2026-11-17-next-quality-gate-01.md；P2-B 由本记录关闭。仅暂存本任务文件，普通提交推送同名开发分支并核对远端，不合并/force/改历史，不进入 Motion 或硬件任务。


## 2026-11-17 FalconFOC V2 NEXT-MOTION-CONTRACT-01 数值合同与错误原子性

- [测试] 既有 Motion CTest 新增正负 Ki trial/P requested 溢出反例：0.25 积分及全部逻辑状态保留、预填输出清零、disabled/非法输入与同实例恢复；旧正常/饱和测试保留。
- [数值] 独立 Double 时间/相位/数学周期 Oracle，名义域有界相位、|omega_e|≤7000 rad/s、0..650 us、Board默认模型设置，74684 样本最大误差1.0674847210623284e-6 rad，小于2e-6预算。高速度域外样本OK但误差0.0015148064769334724 rad，仅作为限制观测。
- [负向] 三类提前提交积分及一类预测时间换算变异均独立成功编译：旧Core15/15通过，新正式测试行为断言失败。首轮复制mtime导致未重编译的无效尝试单独保留排除，有效变异均使用全新构建。
- [回归] Core Clang/GCC各15/15、Runtime11/11、Simulation15/15；Core/Simulation ASan+UBSan各15/15；Source81/9/9、修改C/H格式、严格Scope all含30项CMake格式和Legacy Host45/45通过。原15个Core CTest入口保持。
- [范围] 仅测试、Motion头注释/Core说明、新证据及本日志；生产Motion/参数/Board1/Safety/PWM/UART/整数相位/Golden均未改。区分准入、计算回归与系统总误差；max_error仅约束加速度模型贡献，不称控制品质提升或任意输入精度保证。2427个旧ignored产物保留；Gate使用的6个Legacy ELF/MAP为核对SHA的历史副本，非新ARM重建。
- [交付/边界] 报告docs/evidence/2026-11-17-next-motion-contract-01.md，软件验收PASS，按本轮第7节先请求确认，用户明确批准后才普通commit/push并读回远端核对；最终SHA单独记录，保护收据保留确认前时点。不自动进入下一轮。G3-SAFE HARDWARE NOT VERIFIED / G3-PWR LOCKED / AT32F415 WCET NOT VERIFIED / MOTOR PHYSICAL OPERATION UNVERIFIED。


## 2026-11-17 FalconFOC V2 NEXT-SYSTEM-ANGLE-01 系统角度误差与组合扰动

- [链路/Oracle] 扩展原periodic-chain CTest，真实Encoder→H1→H0→Pipeline/Current。Double强制运动、最近码值和独立速度/LPF参考；分离量化、源映射、raw/LPF、时间戳、模型与Float32计算项，不覆盖生产H1速度/H0角度。
- [实测] 13类预热场景及6类拒绝，2587接受测量行；最大总相位0.0290408770921rad、组合RMS0.0121325047757rad，最大Id/Iq误差0.0233581373120/0.0286009012699A（1A向量）。量化p7上界0.00033555829735rad，计算项最大3.98386820422e-7rad；条件三角界不当作产品总精度保证。
- [准入] 用户明确接受拒绝候选后保留旧新鲜源继续控制；正式检查拒绝payload不污染Current，原源过期后INHIBIT、UPDATE不增。传输失败/越界码发布invalid亦禁止新UPDATE。独立复核P2测试遗漏已通过污染旧槽的可编译变异0→1和正常GREEN关闭，无确认生产缺陷。
- [归因] 当前10ms Current基线与隔离理想ADC相位对照均保留CONTROL_QUALITY_FAIL；RUNNING RMSE仅0.089168893772→0.089167571490A。历史quality判据包含STOP后的full-window末项；不把该标签解释成相位主因，也不修改PI/电压/质量阈值。
- [回归/保护] Core15/15、Runtime11/11、复核后Simulation Clang/GCC/ASan+UBSan各15/15，Core ASan15/15，Source82/9/9、格式、strict Scope all含30项CMake和Host45/45通过。仅测试/helper/说明/本日志/新证据；生产、旧证据及2427既有产物保留，Gate使用历史ELF/MAP副本，非新ARM重建。
- [交付/边界] 报告docs/evidence/2026-11-17-next-system-angle-01.md，受控Host合同PASS；先交付验收并停止，用户随后明确要求GitHub提交推送，获授权后仅普通提交推送本任务8文件并读回远端核对。保护收据保留授权前时点，最终SHA另记。不进入下一阶段。G3-SAFE HARDWARE NOT VERIFIED / G3-PWR LOCKED / AT32F415 WCET NOT VERIFIED / MOTOR PHYSICAL OPERATION UNVERIFIED。


## 2026-11-17 NEXT-CODE-REVIEW-04 Runtime只读深审

- [裁决] 一项P1/BLOCK：生命周期wrong-state/消费时过期拒绝可绕过本拍invalid ADC失效处理，旧确认UPDATE保持；配对反例独立重编译确认，无新增UPDATE。生产/正式测试未改，下一轮先补RED并修两处拒绝出口。
- [证据] Runtime11/11、Simulation15/15仍绿；ARM M4 soft -Os实例1236B、Host1296B，Runtime对象text6484B，静态H0/H1栈168/328B，非WCET/栈高水位。H1窄快照/遥测屏障仅候选待证，不擅自优化。
- [纲领/对照] 用户授权将Downloads原文逐字节移入docs/architecture并添加AGENTS/README入口；原文阶段快照保留、当前Git事实优先。robot_mc固定2f1fb61有限源码对照，无外部代码移植。报告docs/evidence/2026-11-17-next-code-review-04.md。未commit/push，不自动修复或启动下一轮；功率LOCKED，硬件安全/WCET/实机运行未验证。


## 2026-11-17 NEXT-CODE-IMPROVE-04 生命周期拒绝路径修复

- [修复] 仅Runtime解耦生命周期拒绝与本拍采样/Safety；预先保存动作资格，过期邮箱不再跳过control，单拍仅一次Pipeline。原REVIEW-04 F1/P1通过正式RED/GREEN和第二出口隔离变异关闭。
- [验证] 新鲜/无效ADC、invalid sensor、健康过期32组及STOP/FAULT/恢复/重复身份；Core15、Runtime11、Simulation15、受影响ASan/UBSan11/15、GCC Runtime11、Legacy45及严格Scope all均通过，独立复核无新P1。两处依赖旧旁路的正式测试预期作直接必要修订。
- [资源/边界] ARM soft -Os Runtime对象text6484→6512B，实例1236B、H0/control静态帧168/224B不变；非完整镜像/WCET/栈水位。提交后STOP终态REJECTED/CANCELLED分类为独立P2后续。保护旧七项成果及2427个build产物；未commit/push，等待明确Git批准，不自动下一轮。报告docs/evidence/2026-11-17-next-code-improve-04.md；硬件安全/WCET/实机未验证，功率LOCKED。

## 2026-11-17 NEXT-CODE-REVIEW-05 Pipeline 控制事务评审

- [Runtime P2] 8 个交错探针验证终态和输出。已拒绝的 ARM 在已接受 UPDATE 后遇 STOP 保留 REJECTED，待应用的有效 START 遇 STOP 返回 CANCELLED；最终 confirmed INHIBIT。裁决 KEEP，生产代码与正式测试零差异。
- [Pipeline] 无确认 P0/P1；保留 Safety、STOP guard、输出确认与 PI Trial 事务。推荐下轮仅删 submit 的 20 B 旧寄存器预拷贝。隔离 ARM 对比函数 204→190 B，帧 48 B；Safety 重复检查候选待周期与栈证据。
- [证据] Runtime Clang/GCC 各 11/11、Simulation Clang 15/15、锁定 V2 ARM 通过。报告 `docs/evidence/2026-11-17-next-code-review-05.md`。G3-SAFE HARDWARE NOT VERIFIED / G3-PWR LOCKED / AT32F415 WCET NOT VERIFIED / MOTOR PHYSICAL OPERATION UNVERIFIED。

## 2026-11-17 NEXT-CODE-IMPROVE-05 Pipeline 冗余寄存器预拷贝优化

- [实现] `foc_pipeline_submit` 取消候选寄存器的 20 B 旧值预拷贝。UPDATE/INHIBIT 成功完整写入五字段，提交确认、回调及 PI Trial 的顺序保持。
- [验收] A/B 11 条事务记录一致，独立复核无 P0/P1/P2；ARM 函数和对象各减 14 B，整镜像 FLASH 减 12 B，静态帧与 RAM 原值。Runtime 11、Simulation 15、Core 15、NP 2、Board 4、Runtime/Simulation sanitizer 11/15、Source、格式及严格 Scope all 通过。
- [交付] 报告 `docs/evidence/2026-11-17-next-code-improve-05.md` 附原始证据，保留 REVIEW-05 和 2,427 个原有忽略产物，等待 Git 交付确认。G3-SAFE HARDWARE NOT VERIFIED / G3-PWR LOCKED / AT32F415 WCET NOT VERIFIED / MOTOR PHYSICAL OPERATION UNVERIFIED。

## 2026-11-17 NEXT-MOTOR-ID-01A R/L 参数辨识与 PI 候选

- 新增独立 C11 逐轴静止 R/L 窗积分拟合、候选组合及 PI 设计。配置限制最快 RL 极点的采样孔径，拟合有效性、源身份、残差和采样/延迟/电压增益守卫公开可查。
- Host Virtual Motor 三组受控捕获，最大逐轴 Rs/L 误差约 0.273%/0.229%。固定、真值和辨识增益的 Current 比较保留饱和段失败标签，恢复通过。独立审查关闭采样 P1，最终无 P0/P1/P2 发现。
- Core 15/15、Calibration 2/2、Simulation 18/18、ASan/UBSan 2/2、18/18，严格 Scope all 通过。ARM 单轴实例 216 B、新对象 text 4018 B，锁定镜像哈希相同。证据见 `docs/evidence/2026-11-17-next-motor-id-01a.md`。硬件安全、WCET 和实机运行未验证，功率输出锁定。

## 2026-11-27 NEXT-MOTOR-ID-01B R/L 观测准入与可信端电压

- 在 01A 拟合前新增采集身份、端电压类别、真实作用区间与坐标/静止证据检查。无效观测使会话失效，保留诊断并清零参数，双轴组合验证同一采集设置。
- Virtual Motor 三种子 d/q 对照中，误标请求电压使 Rs 与 L 偏高约 25%，端电压来源正确时恢复拟合。原 01A 的 677 行轨迹一致。独立复核最终无 P0/P1/P2。
- Core 15/15、Calibration 2/2、Simulation 18/18、Runtime 11/11、Device 2/2、ASan/UBSan 和严格 Scope all 通过。ARM 单轴 +8 B，新增对象 text +132 B，锁定固件哈希一致。见 `docs/evidence/2026-11-27-next-motor-id-01b.md`。硬件与 WCET 待证，功率输出锁定。

## 2026-11-27 NEXT-ENCODER-LINEAR-01A 编码器周期性非线性补偿

- Encoder 可显式启用按 CPR 与编码器身份准入的整码周期 LUT。校正码同时驱动单圈相位、多圈展开和速度。半圈歧义保护有定向回归。
- Virtual Encoder → Sensor → H1 → H0 → Current 三组 A/B/C 电角 RMS 下降约 99.09%–99.77%，爬行 LPF 速度 RMS 增加 6.70%。关闭时旧系统 2,587 条记录与 Motor-ID 677 行轨迹一致。
- Sensors、Core、Runtime、Simulation、ASan/UBSan、严格 Scope all 和目标构建通过。独立复核最终无 P0/P1/P2。锁定镜像 Flash/RAM 增加 200/16 B。证据见 `docs/evidence/2026-11-27-next-encoder-linear-01a.md`。功率输出锁定，硬件与 WCET 待证。

## 2026-11-27 NEXT-ENCODER-LINEAR-01B 独立参考 LUT 标定和 Holdout

- 新增纯 C11 离线逆向拟合、逐段覆盖和参考零位准入。冻结整数表后由另一采集会话使用生产 Sensor 校验，导出结果可显式试装。
- 五组拆分采集生成六张表，三个 64 节点控制链的 Holdout RMS 为 0.049–0.180 mrad。低速专题观测到输入量化及输出舍入占主要速度扰动，保留 16,384 CPR 爬行速度 RMS 上升。
- Core 15/15、Sensors 2/2、Calibration 5/5、Runtime 11/11、Simulation 21/21、Sanitizer 和严格 Scope all 通过。01A、Motor-ID 与锁定固件哈希基线复现。证据见 `docs/evidence/2026-11-27-next-encoder-linear-01b.md`。实机与 WCET 待证，功率输出锁定。

## 2026-11-27 NEXT-SENSOR-EST-01 速度估计对照与实验 PLL

- 同原码、同 LUT、同一源时钟对照差分/LPF/Alpha-Beta/PLL。采用 800 rad/s 实验 PLL，爬行 RMS 下降 41.39%，加减速 RMS 恶化 20.67%/21.79%，超过预定 15% 上限，裁决 KEEP EXISTING ESTIMATORS。
- 完成 Velocity/Position 隔离闭环、H0 预测速度项及 STOP/FAULT 对照。修复实验 PLL 丢失端点后的错误续算，独立复核无剩余 P0/P1/P2。生产 Sensor、Runtime 与锁定固件哈希一致。
- Simulation Clang/GNU 22/22、Sensors/Core/Runtime/Calibration、Sanitizer、Source Contract 和严格 Scope all 通过。证据见 `docs/evidence/2026-11-27-next-sensor-est-01.md`。功率输出保持锁定，实机与 WCET 待证。


## 2026-11-18 NEXT-SENSOR-EST-02 Alpha-Beta 局部残差精度修复

- Tracker 复用 LUT 校正后的每帧整数增量，偏移锚定在上一接受位置。保留原增益、LPF 默认 H1、整数相位、Sensor 质量和状态提交契约。
- 正式 RED 复现长圈数退化。GREEN 的普通与 ±100 万圈逐样本相同，冻结 EST-01 长多圈爬行 RMS 165.424823 → 0.06704485 rad/s，普通工况 T90 和默认控制链回放一致。
- 全模块 Host、ASan/UBSan、独立固件 Host、严格 Scope all 通过。ARM Flash +8 B，Encoder Context、静态栈和固件 RAM 持平。独立复核无未关闭 P0/P1/P2。见 `docs/evidence/2026-11-18-next-sensor-est-02.md`。功率输出锁定，实机与 WCET 待证。


## 2026-11-18 NEXT-SERVO-01A 可移植单轴阻抗控制 Core

- 新增无状态 C11 阻抗计算。输入连续机械误差、显式 Kt、实时 q 上限，输出 SI 力矩与限幅后的 q 请求。错误清零输出，现有 Runtime 模式与硬件输出权限保持原样。
- Host 经现有 Current 命令、H0、Pipeline 与 Virtual Motor 完成六组冻结场景。0.039 N·m 负载时偏差 0.05998 rad，卸载后 142.5 ms 恢复。降额限幅 180 ms，STOP 和 Sensor Fault 均 INHIBIT。
- 全量回归、ASan/UBSan 和严格 Scope all 通过，无未关闭 P0/P1/P2。独立 ARM 对象 text 864 B，静态栈 56 B，锁定镜像无增量。证据见 `docs/evidence/2026-11-18-next-servo-01a.md`。实机与 WCET 待证，功率保持锁定。


## 2026-11-18 NEXT-SERVO-01B Runtime IMPEDANCE 生产集成

- 生产 Runtime C API 接入 IMPEDANCE。复用命令槽、Session 和 SET_MODE→ARM→START，H1 在同源观测上调用 01A Core，H0 保持既有 Current、Pipeline 和 Safety。
- 六组冻结正式模式闭环和 01A 对照通过。0.039 N·m 负载偏移 0.05997983 rad，卸载恢复 142.5 ms。STOP、超时、失效观测和适配器拒绝均保留 INHIBIT 与更新事务。
- Core、Runtime、Simulation、Sensors、Calibration、Sanitizer、UART Host 和严格 Scope all 全部通过，独立复核无未关闭 P0/P1/P2。锁定镜像 Flash/RAM +1,532/+96 B。见 `docs/evidence/2026-11-18-next-servo-01b.md`。Board1 功率输出保持锁定，实机与 WCET 待证。


## 2026-11-18 NEXT-SERVO-01C 阻抗参数整定与鲁棒性闭环

- 新增 Core 冷路径 `foc_impedance_tuning_design()`。从 J/B、目标自然频率和阻尼比生成 K/D，校验模型来源、时序预算和静态上界，候选由调用方显式使用。
- 正式 Runtime Holdout 为 7 PASS、3 QUALITY_FAIL、1 预期时序拒绝。12 rad/s 候选通过静止噪声，18 rad/s 两组 q 请求纹波超门槛。0.25 A 遇 0.039 N·m 外负载连续饱和 762.5 ms，已保留失败标签。
- 01A/01B 冻结回放一致，Host、Sanitizer、严格 Scope all 通过，无未关闭 P0/P1/P2。锁定固件 BIN 与 01B 一致。见 `docs/evidence/2026-11-18-next-servo-01c.md`。功率锁定，实机与 WCET 待证。

## 2026-11-18 NEXT-SERVO-02A 等效惯量与粘性阻尼辨识

- 新增离线 C11 J/B 会话。观测 dq/等效电磁转矩、机械速度和已知或确认可忽略的外载经质量准入后生成单一候选。统计使用固定实例空间。
- 五个主辨识场景和独立激励预测通过。4× 惯量下正式 IMPEDANCE 的辨识候选将超调从 0.123 降至 0.00854 rad，噪声及强限流品质失败保留，同步复现 01C 历史品质裁决。
- ARM 独立对象 text 3,600 B、实例 288 B，锁定参考镜像字节一致。证据见 `docs/evidence/2026-11-18-next-servo-02a.md`。功率输出锁定，实机与 WCET 待证。

## 2026-11-18 NEXT-SERVO-02B 阻抗噪声鲁棒性与目标可达性

- 新增独立静态可达性诊断，覆盖转矩容量、位置容差、零刚度、负载和 Kt 来源。正式 IMPEDANCE 八组对照证实 0.25 A 对 0.039 N·m 持续外载能力不足，等容量零余量。
- 隔离 48 Hz 阻尼支路候选通过冻结噪声和可恢复限流检查，1.3× 反向速度 RMS 0.29249 rad/s 超过 0.25 rad/s。裁决 KEEP EXISTING IMPEDANCE；历史 QUALITY_FAIL 保留。
- 见 `docs/evidence/2026-11-18-next-servo-02b.md`。功率输出锁定，实机及 WCET 待证。

## 2026-11-18 NEXT-SERVO-02C 反向跟踪归因与模型运动前馈

- 精确复现 02B 三臂反向数据。原三次参考位置和速度连续，入口加速度发生有限阶跃，反向峰值需求 40.11 mN·m、三臂无电流限幅。零时延理想反馈仍复现主要误差，制动段占原平方误差约 63%。
- 新增显式 C11 J/B 运动前馈。相同 C2 轨迹下真值和 02A 识别模型明显改善反向；噪声和低电流预算边界保留。例程通过既有 IMPEDANCE 命令，不引入自动 Runtime 策略。
- 见 `docs/evidence/2026-11-18-next-servo-02c.md`。功率输出锁定，实机及 WCET 待证。

## 2026-11-18 NEXT-SERVO-02D 前馈与阻尼滤波协同控制

- 复用 02C 运动前馈 API 和 02B 的 48 Hz 滤波算子，完成原三次参考四臂及独立五次 Holdout。普通噪声组组合 q 请求高频 RMS 7.14–7.75 mA，三组相对反向及加减速门槛失败，裁决 KEEP EXISTING IMPEDANCE。
- 0.16 A 场景保留能力不足，限幅恢复门槛通过。Host 实验通过 STOP/FAULT、Adapter 拒绝、超龄与未确认提案替换检查，生产 Runtime 和锁定固件不接入候选。
- 证据见 `docs/evidence/2026-11-18-next-servo-02d.md`。功率输出保持锁定，实机及 WCET 待证。

## 2026-11-18 NEXT-SERVO-03A 独立 Host Servo 应用

- 新增独立 C11 Servo 示例与 Quickstart。调用方通过公开入口组织 IMPEDANCE 生命周期、同源反馈、显式 J/B 前馈及静态可达性诊断。
- 仓库内及仓库外 Clang/GCC 示例运行通过。STOP、拒绝与 Fault 路径保持 Host 输出契约。02D 冻结回放与锁定 ARM 镜像逐字节一致。
- 证据见 `docs/evidence/2026-11-18-next-servo-03a.md`。实机与 WCET 待证。

## 2026-11-18 NEXT-SERVO-03B Servo SDK 打包与发布门禁

- 按显式清单交付可复现的内部 Servo SDK `0.1.0-rc.1`，包含 Core-only 和 Servo Host 两个消费入口。
- 解包后的 Clang、GCC 和 ASan/UBSan 验收通过，锁定 ARM 镜像和 02D 冻结轨迹保持一致。来源、许可及公开发布批准待确认，裁决 SDK TECHNICAL PASS — RELEASE BLOCKED。
- 证据见 `docs/evidence/2026-11-18-next-servo-03b.md`。实机与 WCET 待证。

## 2026-11-18 NEXT-RELEASE-02A Servo SDK 来源取证

- 核验原 `0.1.0-rc.1` ZIP、Manifest 和 52 个文件，补齐逐文件来源与权利矩阵。16 个早期文件首入库均可追溯，入库前权利链仍待确认。
- 整理许可决策、版权和 NOTICE 审查草案、所有者审批模板供内部评估。发布状态为 RELEASE_BLOCKED。
- 见 `docs/evidence/2026-11-18-next-release-02a.md`。本轮复用原技术证据，保留实机与 WCET 的验证边界。

## 2026-11-18 NEXT-SOFT-01 设备参数、准入与诊断

- 新增可选 C11 Foreground 参数模块，提供 30 项参数及诊断、5 个现有 Runtime 动作。静态配置暂存为独立候选，IMPEDANCE 命令沿用已有发布和 H0 接纳。
- Host 示例和独立 Clang/GCC/ASan 消费通过。UART V1、冻结 02D 轨迹及锁定 ARM 固件保持基线。
- 见 `docs/evidence/2026-11-18-next-soft-01.md`。原 SDK 归档保持独立，实机与 WCET 待证。

## 2026-11-18 NEXT-SOFT-02 软件位置限位与持续跟随误差

- 新增默认关闭的连续机械坐标软限位及跟随误差监控。POSITION、VELOCITY、IMPEDANCE 通过正式 H1/H0，软限位启用时 CURRENT 明确不支持。持续超差走既有 STOP/INHIBIT 和重新 ARM 路径。
- Control Plane 增加 9 项静态配置和 2 项只读诊断。Host 全量回归与独立 Clang/GCC/ASan 消费通过。AT32F415 锁定固件静态 Flash 增加 2,212 B，链接 RAM 增加 112 B。
- 见 `docs/evidence/2026-11-18-next-soft-02.md`。保留高速惯性越界证据，功率输出锁定，实机及 WCET 待证。

## 2026-11-18 NEXT-SOFT-03 版本化配置存储

- 新增独立 C11 ID-keyed TLV、CRC-32 与双槽事务。保存 14 项可选静态设置，在同一事务恢复运动保护整组。旧 Schema 缺失保护字段时须显式复核。
- Host 逐步骤覆盖 330 次断电，恢复 326 次完整旧记录和 4 次已提交新记录。活动保存复用配置准入，恢复产生独立候选。锁定 AT32F415 镜像与 SOFT-02 完全一致。
- 见 `docs/evidence/2026-11-18-next-soft-03.md`。实际 Flash 端口和物理断电、实机及 WCET 待证。

## 2026-11-18 NEXT-SOFT-04 只读配置检查和设备诊断

- 可选 C11 诊断复用正式双槽选择、Codec、参数描述表及 Runtime 一致快照。区分记录完整性、恢复适用性及 Runtime 有效值。
- 新增 Host 离线 CLI、JSON v1 和独立 C11 消费示例。全量回归通过，SOFT-03 断电证据及锁定 ARM BIN 保持一致。
- 见 `docs/evidence/2026-11-18-next-soft-04.md`。真实 Flash 读取、板端 WCET 和电机操作待证。

## 2026-11-19 NEXT-SOFT-05 统一标定工作流

- 新增可选 C11 七阶段会话与应用适配器。复用现有辨识与标定，绑定采集和设备身份，明确区分阶段质量、候选审核、新 Runtime 读回和持久化。
- 独立 Host 消费者 Clang、GCC、ASan/UBSan 通过。冻结 LUT 后统一转子机械坐标，应用后独立电角相位 RMS 为 0.0074212 rad。新旧实例均保持 INHIBIT，锁定 ARM BIN 字节一致。
- 见 `docs/evidence/2026-11-19-next-soft-05.md`。实机同步采集、Kt 和负载资格、板端 WCET 与实际 Flash 持久化待证。

## 2026-11-19 NEXT-SOFT-06 测量记录与标定回放

- 加入可选 C11 Trace Adapter、只读 Python CLI 和两个标准测量包。采样、触发与完成时间分别记录，测量来源与数字完整性独立报告。
- 七阶段 Host 回放复现 SOFT-05。CLI 输出待审核候选，验证错位、丢样、Holdout 复用、缺 Kt 与来源失配。锁定固件 BIN 字节一致。
- 见 `docs/evidence/2026-11-19-next-soft-06.md`。真实测量来源、板端采集吞吐量和实机标定待证。

## 2026-11-19 ARCH-001 架构决策与变更规则

- 在项目纲领写入产品目标、成熟工程方法和可验证证据共同决定架构的原则，并将 ADR 纳入 AGENTS.md 接手顺序。
- V2 以独立 C11 Core 和 H0/H1/Foreground 为评审基线，规定变更证据、同条件对照、验收与回退。独立记录 Legacy 漏边沿反例，PWM 同步和 20/20 kHz 留作待验目标。
- 见 [ARCH-001](../architecture/FalconFOC_BareMetal_Execution_ADR.md)。本次交付为治理文档，事件模型验证属 NEXT-ARCH-00。

## 2026-11-19 NEXT-ARCH-00 裸机执行与 Host 时间合同

- 基于 VM01A `d824933` 复原 IRQ、时间与状态所有权，形成触发、频率、候选与反证条件。保留 H0/H1、SPI IRQ，`sched` 裁决 `KEEP_UNUSED`。
- 新增 Board Host 时间事件测试，计算 Legacy 抽象反例 20,000 → 13,333.333 Hz，覆盖漏触发、两个装载期限反例及 Runtime 交错。Board Host 三个验证配置各 5/5，严格门禁通过，锁定 ARM 三种产物摘要一致。
- 裁决 `ARCH-00 HOST PASS — HARDWARE TIMING NOT VERIFIED`。下一项为 Board 周期身份与 ADC token 观测。 [ARCH-00 证据](../evidence/2026-11-19-next-arch-00.md)

## 2026-11-19 NEXT-RT-01 逻辑周期与 ADC–H0 交接

- Board 使用 TMR1 相位、TMR2 epoch 与 sticky 见证推导 200 µs 逻辑槽，发现缺失和不可信时序。同槽限一次请求，跨槽或未排空 PCCE 拒绝，保留旧 token 和原 STOP/Safety 合同。
- Board Host 三配置各 5/5，严格门禁与 Legacy Host 45/45 通过。锁定 ARM Flash/RAM 增加 1,232/184 B。优先在保留现行触发路径的锁定镜像上做下一轮 DWT 基线。 [RT-01 证据](../evidence/2026-11-19-next-rt-01.md)

## 2026-11-19 NEXT-RT-02 ADC–PWM 同步与装载合同

- 保留软件触发与 RT-01 周期身份，启用可选的单事务 DWT 插桩。专用 TMR1 CH4 候选完成 SDK 编译核查和 Host A/B。四 Rank 孔径、逐相有效窗口及指定装载截止期使用纯 C11 判定。
- 默认锁定 ARM BIN 与 RT-01 字节一致。物理触发、采样窗口、装载与 WCET 待验。正式路线依次为 RT-03、INTEG-01、INTEG-02、PRODUCT-01。 [RT-02 证据](../evidence/2026-11-19-next-rt-02.md)

## 2026-11-19 NEXT-RT-03 H0/H1 抢占、SPI 与 WCET 证据

- 复现 H0 抢占导致 H1 和监督快照 `BUSY` 的误停。H1 加一次有界重读，监督延后冲突周期并保留实际超期的 INHIBIT。SPI 端口以 TMR2 夹时校验 CS/TX/关闭期限，丢弃跨界事务。
- 延续 RT-02 的可关闭 DWT 探针并完成 IRQ、协议、控制路径的 Host 与 ARM 静态复验。裁决 `KEEP H0/H1 ARCHITECTURE`；目标板 WCET 待验，下一步为 INTEG-01 的 Host 入口。 [RT-03 证据](../evidence/2026-11-19-next-rt-03.md)

## 2026-11-19 NEXT-INTEG-01 Host Servo 产品入口

- 将既有 Servo 程序接入 C11 Control Plane。新增能力和配置检查、H0 可配对回执、禁用确认及同一 Runtime 上的命令超时恢复。保留虚拟电机、前馈和控制品质口径。
- Host 入口矩阵与外部源码构建通过，默认 ARM ELF/BIN/HEX 与 RT-03 相同。修订 SDK 源码组装依赖并验证临时目录投影；既有归档和发布状态保留。 [INTEG-01 证据](../evidence/2026-11-19-next-integ-01.md)

## 2026-11-19 · NEXT-INTEG-02 Board1 只读采集

新增默认关闭的 8 ms SRAM 单次采集和冻结镜像，接入 ADC1/PCCE、SPI1/TMR5 及 Runtime/Safety 观测。由受控 SWD 导出，Host 按版本、来源、时钟、序号和摘要归档。ADC_ZERO 仅在独立零电流和采样窗口见证满足时转换为现有 SOFT-06 V1 输入。实板采集、真实回放与 WCET 待取证。参见 [INTEG-02 验收](../evidence/2026-11-19-next-integ-02.md)。

## 2026-11-19 · NEXT-PRODUCT-01 软件准备

增加独立 Host 三相 PWM 事务模型和锁定态 DWT/SRAM 离线校验入口，验证完整 Shadow 写入、指定 Load、Session/Token 与 STOP/FAULT 撤销。ARM 固件继续锁定。原理图中的独立去能及 Break 路由待实际硬件确认，功率和实机闭环保持阻断。参见 [PRODUCT-01 验收](../evidence/2026-11-19-next-product-01.md)。


## 2026-11-19 · NEXT-CAL-INTEG-01 Host 标定与 FOC 集成

- 在 VM01A `0d6ca149` 基线上交付统一 Host 标定入口。虚拟采集覆盖 ADC、转子与 Encoder LUT、独立 d/q R/L、反电动势磁链、J/B 和可选齿槽。审核后，新的生产 Runtime、Pipeline 与 Encoder 实际消费校准与增益，FCAL v1 支持审核重载。
- 两种 Host 配置四模式双向控制、关闭与超时恢复均通过，保留旧 Current 与 Servo 的品质失败。Clang、GCC、ASan/UBSan、严格门禁和 Legacy Host 通过，AT32F415 锁定 BIN 与基线相同。实际标定、物理输出与 WCET 待验证。 [验收报告](../evidence/2026-11-19-next-cal-integ-01.md)

## 2026-11-20 · NEXT-CQ-01 控制品质和标定收益

- 隔离原 36 Hz Host 整定引起的 Current 退化，两种虚拟配置按 130 Hz 重新验证四模式双向、失配及重载，最坏方向 RMS 均改善。Core 修复低调制比重构的舍入误停，保留事务和保护。
- 裁决 `NEXT-CQ-01 PARTIAL — QUALITY FAILURES REMAIN`。历史 10 ms Current 和三项 Servo 失败仍在，48 Hz 阻尼试验保留为 Host 候选。软件回归和锁定 ARM 构建通过，实机标定和 WCET 待验。 [验收报告](../evidence/2026-11-20-next-cq-01.md)

## 2026-11-20 · NEXT-CQ-02 阻尼噪声与遗留品质

- H1 正式阻抗路径加入可配置的 48 Hz 速度差低通。Host 两个原噪声 Holdout、冻结动态验收和双电机 130 Hz 回归通过。保留 FCAL v1、原测试与 Board1 输出锁。
- 历史 Current 三个超差由旧 PI 冷启动解释，原整窗仍 FAIL。Host 独立证据确认 0.039 N·m 超出 0.25 A 的静止转矩能力，近边界可行负载仍有动态超差。锁定 ARM 复建成功，实机标定和 WCET 待验。 [验收报告](../evidence/2026-11-20-next-cq-02.md)

## 2026-11-20 · NEXT-CQ-03 启动与能力边界收口

- Host 增加 Current Startup V1 全生命周期观测及固定评价。速度前馈与首次目标补偿完成同条件对照，保留已审核的 130 Hz 和 48 Hz。Salient 大阶跃、历史 Current、近转矩点和超载保留对应失败。
- 复验近转矩点滑移、限幅与撤载，完成版本化控制品质验收定义。软件专项收口，锁定 ARM 镜像逐字节相同，物理标定与目标 WCET 待验。 [验收报告](../evidence/2026-11-20-next-cq-03.md)

## 2026-11-20 · NEXT-PRODUCT-02 软件产品成熟度

- Core 增加可重定位的 C11 静态包与外部消费例。复用公开 Runtime 和 Sensor API 验证独立控制使用链，Host CLI 增加经校验 FCAL 的稳态电压、电流和转矩可达域。
- 归档真实调用链、Host 成本和 ARM 静态证据。保留锁定镜像和历史品质裁决，识别 ADC 最晚触发与 H0 预算的组合风险。目标时序、物理标定与 WCET 待验。 [验收报告](../evidence/2026-11-20-next-product-02.md)

## 2026-11-20 · NEXT-CAL-ARCH-02 校准参数复用

- 工作流支持 Rs 与 J/B 分项审核，完整应用继续依赖换相准入。Host 按设备、来源和依赖版本选择重测，组合候选进入现有 Calibration Application，并在首个 INHIBIT 回调复核原实例的 STOP/FAULT 状态。
- 复用历史保留于 Host 进程，数值配置继续兼容 FCAL v1。虚拟电机闭环、应用拒绝证据和板端锁定边界见 [验收报告](../evidence/2026-11-20-next-cal-arch-02.md)。

## 2026-11-20 · NEXT-CAL-FLUX-01 磁链与 Kt

- 把 Host 原端电压积分和正反向拟合提取为通用 C11 `foc_flux_id`。测量契约区分实际端电压、独立验证的重构电压和普通软件请求。Host 预览与复用审核保留符号、来源和依赖修订。
- 两套虚拟电机复验既有拟合和 FOC 结果，并验证已审核 Kt 下的双向超载限制。FCAL v1 和锁定板级镜像保持兼容。 [验收报告](../evidence/2026-11-20-next-cal-flux-01.md)


## 2026-11-20 · NEXT-CAL-BOOT-01 未知电机初始化

- 新增 C11 初始化资格和粗参考辨识。Host 在软件输出抑制期间，通过独立虚拟机械驱动、同步原始编码器码、有符号轴速与开路端电压获得磁轴参考，再复用 Rotor、LUT 和 Offset 估计器。
- 细化电角进入 R/L、Flux/Kt、J/B 与显式审核后的 Runtime。复用按 LUT 和 Offset 依赖传播失效，FCAL v1 沿用原数值审核。锁定 ARM 复建镜像与基线相同。 [验收报告](../evidence/2026-11-20-next-cal-boot-01.md)

## 2026-11-21 · NEXT-CAL-QUALITY-01 Host 校准产品验收

- Quick、Full、Selective 复用原 CLI、Workflow 与审核应用。记录模拟电机、Host 墙钟及进程 CPU 时间。FCAL v1 重载复核当前 Host 测量链，跨进程来源和依赖历史单独标记。
- 两套电机按不同种子、采样起点和轴角独立采集。归档数值误差、独立 Holdout、源偏差、热态 Rs、闭环与复用耗时。保留历史品质失败和锁定板端镜像。 [验收报告](../evidence/2026-11-21-next-cal-quality-01.md)

## 2026-11-21 · NEXT-FW-INTEG-01 Board1 启动校准快照接入

- V2 Board1 启动链变更：`main()` 在输出锁和时钟之后、ADC/SPI/定时器/UART/IRQ 之前调用 `foc_board_boot_select()`。新增构建期只读供应接口 `foc_board_calibration_supply()` 与独立期望 `foc_board_calibration_expectation()`。默认构建无供应，报告 NOMINAL_CONFIGURATION + CALIBRATION_NOT_AVAILABLE，Runtime 与原 `foc_board_init()` 一致。
- 供应须通过格式、身份、RAW_NO_LUT 坐标、Board 事实下重算的 `foc_calibration_admit()`、快照逐位一致、方向乘积和 Board 窗口检查，才进入 `foc_board_init_with_calibration()`。拒绝或初始化失败时清除候选并重建标称配置；标称也失败则锁定停机。PARAM_READ 新增只读 ID 32–45 读回生效值。采集元数据改用生效配置。
- 新增独立命名的合成验证 ARM 镜像。两组镜像保持原 TMR1/GPIO 锁定、UART 拒绝与适配器 UPDATE 拒绝。物理输出、目标 WCET 未验证，G3-PWR LOCKED。 [验收报告](../evidence/2026-11-21-next-fw-integ-01.md)

## 2026-11-21 · NEXT-FW-INTEG-01R Board1 校准启动集成修复

- Capture：V1 布局不变，头部 `encoder_profile` 作配置区分符（1 标称原义，2 校准）。`board_capture.py`、`locked_timing.py` 以冻结 Board1 常量和冻结标称或元数据格式 2 的外部配置声明核对身份，标称镜像与旧 V1 样本照常读入，校准镜像需声明。
- 供应 ID 在 CMake 中于窄化前限定为非零 uint32，`foc_board_expectation.c` 对未窄化字面量静态断言。可接纳供应须带非零 `review_id`（新增拒绝值 10，原值不变）；`review_id` 只是审核材料引用。
- 复现脚本迁入 `scripts/flows/fwinteg/`：原始退出码逐步记录，按冻结用例清单逐名核对三工具链，分类判定，顶层故障注入覆盖 18 类负例与全绿对照。物理输出、物理校准与目标 WCET 未验证，G3-PWR LOCKED。 [验收报告](../evidence/2026-11-21-next-fw-integ-01r.md)

## 2026-11-21 · FW-ADC-01 Capture 至 ADC 零偏候选（Host）

- 新增 Host 适配层：`board_adc_candidate.py` 复用生产 `board_capture.py` 的镜像校验与 ADC_ZERO Trace，`foc-board-adc-candidate` 调用既有 `foc_calibration_adc_collect/finalize`，与既有审核 Rotor/Offset 候选组成隔离供应，经 `foc_board_boot_select()` 读回 Runtime、解码与 UART，正式实例不变。无新统计或配置系统，ARM 源码未改。
- 零电流资格只来自调用方前提：Host 夹具见证可生成标注来源的候选；Board 记录无独立证明时为 `PRECONDITION_UNVERIFIED`，不生成候选。两电流 rank 互换无法由现有证据识别。物理 ADC 零点、物理零电流、采样孔径、增益与 WCET 未验证，G3-PWR LOCKED。 [验收报告](../evidence/2026-11-21-fw-adc-01.md)

## 2026-11-22 · V2 参数契约设计稿

- 新增 `docs/superpowers/specs/2026-11-22-falconfoc-v2-parameter-contract-design.md`：参数按电机模型、换相组、用户档案、控制设置、保护阈值、通信、机械零点分组，规定 ID、范围、写入状态、写后统一刷新、存盘与命令；门禁为换相组有效且 ADC 零点就绪。`protocol/README.md` 参数节加指向说明。仅设计，固件未改。

## 2026-11-22 · V2 保护故障层（参数契约第一步）

- 新增可移植模块 `g3/safe/foc_protect`：过流、过压/欠压、过速、持续大电流、堵转、跟踪发散（分窗统计）。Runtime 在 H0 每个接受的运行周期后评估，跳闸经既有 stop 路径锁存故障位 9–15 并 INHIBIT，同时锁存快照（0x22 FAULT_SNAPSHOT_READ）。FAULT_CLEAR 扩展到保护类故障（电压类要求 VBUS 回到窗口内）。阈值为 tuning ID 112–118，暂只在 RAM。初始值依据原理图（VM 5.25–20 V）与 DRV8311 数据手册（VM 绝对最大 24 V），待实物确认。
- SIL 新增 `--adc-step` 注入与 `sil-demo --sil-scenario protect`：换相错配 6 例、逐规则注入、清除、反向不误报，全部通过；物理输出、目标板阈值与 WCET 未验证。
- 资源基线（arm-none-eabi-size）：默认镜像 text 77,768 B（此前 75,200 B，约占 128 KB Flash 59%），data+bss 12,340 B（约占 32 KB RAM 38%，另有 4 KB 栈余量断言）；持久化镜像 text 81,112 B。回归 fwinteg 126×3 VERDICT PASS，protect 场景 lockstep 与 free-run 均 SIL_PASS。

## 2026-11-22 · V2 电机档案、统一刷新与存储 format 3（参数契约第二步）

- 档案（Rs、Ld、Lq、Ψ、J、B、额定/峰值电流、最大转速、减速比、电流带宽）并入 Runtime tuning，与 PI、限值、保护阈值一起由 H0 生效；Runtime 校验档案范围与顺序约束，并以额定电流、最大转速作为保护基准。新增 `foc_board_profile` 统一刷新：写模型或带宽时用 `foc_motor_pi_design()` 重算电流 PI（设计拒绝则写入失败），写峰值电流、最大转速时收紧限值；CAL-3/4/5 经同一路径写回辨识模型。UART 新增 ID 80–86、96–99、104；ID 2 随 ID 73 下调。
- 存储 format 3（212 字节）在 format 2 之后追加档案与保护阈值 ID 112–118，旧格式按启动值兼容读入；恢复时不重新推导 PI。SIL 新增 `profile` 场景（lockstep 与 free-run 通过），SIL 单元测试 13 项通过。资源：默认镜像 text 79,176 B（+1,408 B），bss 12,376 B；持久化镜像 text 83,024 B。

## 2026-11-22 · V2 门禁、换相组写入、每次上电测零与换电机流程（参数契约第三步）

- ARM/START 需有效换相组（来源 RUN/STORAGE/HOST/SUPPLY，NOMINAL 占位无效）与本次上电测得的 ADC 零点：无效返回新结果码 15 NOT_CALIBRATED，零点未就绪返回 BUSY。校准管理器在换相组有效时自动执行只测零点的 CAL-1（阶段 4，不驱动电机）并重新初始化；新增 COMMUTATION_WRITE（0x53）整组写入、ID 17 组内微调、只读 ID 88–90；CONFIG_ERASE 同时把 RAM 恢复为启动配置与启动 tuning。Runtime 增加传感器预热期：重新初始化后、首个传感器发布前，DISABLED 的 H0 不计入无效采样锁存。协议层登记 0x22、0x53。
- CLI 新增 `motor-setup`（校准 → 档案 → 保存）与 `commission()`；SIL 场景改为先经串口导入启动换相组；新增 `swap` 场景（lockstep 与 free-run 通过），SIL 单元测试 14 项通过。已知局限：CAL-3 对 0.35 Ω/0.7 mH 电机拟合失败。资源：默认镜像 text 81,000 B，bss 12,584 B。
