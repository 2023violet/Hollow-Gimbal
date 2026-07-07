# FalconFoc App-Layer Seam Closure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Seal `applications/` so runtime is the only product-meaning write boundary while shell, tasks, and FSM keep the current external contract unchanged.

**Architecture:** Phase 1 tightens seams: shell and FSM read from runtime snapshots and send runtime requests instead of mutating product state directly, and `motor_task.h` becomes a pure task header again. Phase 2 splits `foc_common.h` so algorithm-only types stay in `applications/algo/` while live motor state moves to runtime-owned headers.

**Tech Stack:** C, CMSIS-RTOS2, existing host tests, existing FalconFoc gate, COM54 board witness.

---

### Task 1: Tighten the runtime seam API

**Files:**
- Modify: `applications/runtime/motor_runtime.h`
- Modify: `applications/runtime/motor_runtime.c`
- Modify: `tests/host/support/runtime_host_stubs.c`
- Modify: `tests/host/applications/test_motor_runtime_contract.c`

- [ ] **Step 1: Write the failing test**

```c
struct motor_runtime_snapshot snap;
struct motor_dev motor = {0};

motor_runtime_attach(&motor);
TEST_ASSERT_TRUE(motor_runtime_snapshot(&snap));
TEST_ASSERT_EQUAL_UINT8(AXIS_STATE_IDLE, snap.axis_state);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --output-on-failure -R test_motor_runtime_contract`
Expected: FAIL because the new seam contract is not yet fully wired.

- [ ] **Step 3: Write minimal implementation**

```c
bool motor_runtime_snapshot(struct motor_runtime_snapshot *snapshot)
{
    struct motor_dev *motor = _motor_runtime_current;
    if ((snapshot == NULL) || (motor == NULL)) {
        return false;
    }
    *snapshot = (struct motor_runtime_snapshot){0};
    snapshot->axis_state = (uint8_t)motor->axis_state;
    return true;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --output-on-failure -R test_motor_runtime_contract`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add applications/runtime/motor_runtime.h applications/runtime/motor_runtime.c tests/host/support/runtime_host_stubs.c tests/host/applications/test_motor_runtime_contract.c
git commit -m "feat: add runtime snapshot seam"
```

### Task 2: Move shell reads to runtime snapshots

**Files:**
- Modify: `applications/shell/app_shell_common.h`
- Modify: `applications/shell/app_shell_cmd_state.c`
- Modify: `applications/shell/app_shell_cmd_obs.c`
- Modify: `applications/shell/app_shell_cmd_fault.c`

- [ ] **Step 1: Write the failing test**

```c
struct motor_runtime_snapshot snap;
TEST_ASSERT_TRUE(motor_runtime_snapshot(&snap));
TEST_ASSERT_EQUAL_UINT8(AXIS_STATE_RUNNING, snap.axis_state);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --output-on-failure -R test_app_shell_state_contract`
Expected: FAIL until shell consumes the snapshot seam.

- [ ] **Step 3: Write minimal implementation**

```c
struct motor_runtime_snapshot snap;
if (!motor_runtime_snapshot(&snap)) {
    return SHELL_ERR_FAIL;
}
TM_SHELL_SUMMARY("state", "axis=%s ctrl=%s input=%s fault=0x%08x", ...);
```

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --output-on-failure -R test_app_shell_state_contract`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add applications/shell/app_shell_common.h applications/shell/app_shell_cmd_state.c applications/shell/app_shell_cmd_obs.c applications/shell/app_shell_cmd_fault.c
git commit -m "feat: route shell read paths through runtime snapshot"
```

### Task 3: Move cfg/store/cal writes behind runtime requests

**Files:**
- Modify: `applications/runtime/motor_runtime.h`
- Modify: `applications/runtime/motor_runtime.c`
- Modify: `applications/runtime/motor_calibration_runtime.h`
- Modify: `applications/runtime/motor_calibration_runtime.c`
- Modify: `applications/shell/app_shell_cmd_cfg.c`
- Modify: `applications/shell/app_shell_cmd_store.c`
- Modify: `applications/shell/app_shell_cmd_cal.c`
- Modify: `tests/host/applications/test_app_shell_cfg_cal_contract.c`
- Modify: `tests/host/applications/test_app_shell_store_contract.c`

- [ ] **Step 1: Write the failing test**

```c
TEST_ASSERT_EQUAL_UINT8(PARAM_SAVE_SYS, _motor_ins.calib_state.param_save_req);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --output-on-failure -R test_app_shell_store_contract`
Expected: FAIL until save/load/abort flows go through runtime helpers.

- [ ] **Step 3: Write minimal implementation**

```c
void motor_runtime_request_default_ctrl_mode(struct motor_dev *motor, enum foc_ctrl_mode mode)
{
    if ((motor != NULL) && (motor->axis_state == AXIS_STATE_IDLE)) {
        motor->ctrl_mode = mode;
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --output-on-failure -R test_app_shell_cfg_cal_contract`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add applications/runtime/motor_runtime.h applications/runtime/motor_runtime.c applications/runtime/motor_calibration_runtime.h applications/runtime/motor_calibration_runtime.c applications/shell/app_shell_cmd_cfg.c applications/shell/app_shell_cmd_store.c applications/shell/app_shell_cmd_cal.c tests/host/applications/test_app_shell_cfg_cal_contract.c tests/host/applications/test_app_shell_store_contract.c
git commit -m "feat: move product writes behind runtime requests"
```

### Task 4: Remove runtime hooks from the task header and fix FSM include paths

**Files:**
- Modify: `applications/tasks/motor_task.h`
- Modify: `applications/tasks/motor_fsm.c`
- Modify: `tests/host/support/motor_task.h`
- Modify: `tests/host/applications/test_motor_fsm_abort_restore.c`
- Modify: `tests/host/applications/test_motor_fsm_fault_clear_contract.c`

- [ ] **Step 1: Write the failing test**

```c
void motor_calib_prepare_entry(struct motor_dev *motor);
void motor_restore_after_param_load(struct motor_dev *motor);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --output-on-failure -R test_motor_fsm_abort_restore`
Expected: FAIL until FSM includes the runtime seam explicitly.

- [ ] **Step 3: Write minimal implementation**

```c
#include "../runtime/motor_runtime.h"
#include "../runtime/motor_calibration_runtime.h"
```

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --output-on-failure -R test_motor_fsm_abort_restore`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add applications/tasks/motor_task.h applications/tasks/motor_fsm.c tests/host/support/motor_task.h tests/host/applications/test_motor_fsm_abort_restore.c tests/host/applications/test_motor_fsm_fault_clear_contract.c
git commit -m "feat: make motor task header pure and rebind fsm to runtime"
```

### Task 5: Split `foc_common.h` live state in phase 2 only

**Files:**
- Create: `applications/runtime/motor_state.h`
- Modify: `applications/algo/foc_common.h`
- Modify: `applications/runtime/motor_runtime.h`
- Modify: `applications/shell/app_shell_common.h`
- Modify: `tests/host/applications/test_motor_runtime_contract.c`

- [ ] **Step 1: Write the failing test**

```c
TEST_ASSERT_TRUE(sizeof(struct motor_runtime_snapshot) > 0);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --output-on-failure -R test_motor_runtime_contract`
Expected: FAIL until live state is moved out of the algorithm header.

- [ ] **Step 3: Write minimal implementation**

```c
struct motor_runtime_snapshot {
    uint8_t axis_state;
    uint8_t ctrl_mode;
    uint8_t input_mode;
};
```

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --output-on-failure -R test_motor_runtime_contract`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add applications/runtime/motor_state.h applications/algo/foc_common.h applications/runtime/motor_runtime.h applications/shell/app_shell_common.h tests/host/applications/test_motor_runtime_contract.c
git commit -m "feat: split algorithm types from runtime state"
```

### Task 6: Tighten gates and docs

**Files:**
- Modify: `tests/script/test_falconfoc_themis_lite_contract.ps1`
- Modify: `docs/application/README.md`
- Modify: `docs/architecture/Themis_FalconFoc无devdrv模式与FOC直控分层方案.md`
- Modify: `docs/governance/Themis_工程维护日志.md`
- Modify: `applications/runtime/README.md`

- [ ] **Step 1: Write the failing test**

```powershell
rg -n "motor_task\.h.*motor_calib_prepare_entry|_motor_ins" applications/shell applications/tasks
```

- [ ] **Step 2: Run test to verify it fails**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File tests/script/test_falconfoc_themis_lite_contract.ps1 -RepoRoot D:/study/Software/FalconFoc_AT32`
Expected: FAIL until the new seam boundary is enforced.

- [ ] **Step 3: Write minimal implementation**

```powershell
if (Select-String -Path $files -Pattern 'motor_task\.h.*motor_calib_prepare_entry|motor_restore_after_param_load') { throw 'runtime hooks must not leak from motor_task.h' }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File tests/script/test_falconfoc_themis_lite_contract.ps1 -RepoRoot D:/study/Software/FalconFoc_AT32`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tests/script/test_falconfoc_themis_lite_contract.ps1 docs/application/README.md docs/architecture/Themis_FalconFoc无devdrv模式与FOC直控分层方案.md docs/governance/Themis_工程维护日志.md applications/runtime/README.md
git commit -m "docs: record runtime seam closure"
```
