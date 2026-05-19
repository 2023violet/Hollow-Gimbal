# FalconFoc Maintenance Log

## 2026-05-14

### FalconFoc AT32F403A Themis-lite platform/build alignment

- This heading is kept as the current live maintenance-log anchor for contract
  tests and tooling.
- Current witness tokens:
  - debugger=`DAP-Link`
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
- Confirmed the active thin control-plane port owner remains `bsp/ports`.
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
  - debugger: `DAP-Link`
  - console port: `COM54`
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
  - debugger=`DAP-Link`
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
  - debugger=`DAP-Link`
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
