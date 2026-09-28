# Motor Safety Bring-up Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在不驱动电机、不依赖实板转动的前提下，建立可验证的快速保护闭环，修复当前主机验证阻塞，并把电流采样零点校准与 VBUS 异常路径收敛到可以安全进入下一轮实机测试的状态。

**Architecture:** ADC 中断只执行常数时间、无阻塞的硬保护检查；检测到硬故障时先锁存 `fault_code` 并立即关闭 PWM，状态机在任务上下文中接管状态迁移。任务级检查负责兜底和持续时间规则，不在保护路径中打印、加锁、写 Flash 或等待。启动校准保持在电机任务中，但增加模拟量稳定等待和原始值/离散度验证。FOC 参数计算对无效 VBUS 做防御性处理。

**Tech Stack:** AT32F415、C、CMake、PowerShell、host contract tests、arm-none-eabi firmware build、OpenOCD/CMSIS-DAP 只读寄存器验证。

**Spec:** `docs/code-wiki/motor-debugging-safety-and-procedure.md`、`docs/code-wiki/motor-real-bench-test-plan.md`。

## Global Constraints

- 不执行电机启动、开环运行、闭环运行或任何需要转动的实机动作。
- 在软件保护代码通过 host 测试和固件编译前，不刷写新的固件。
- 不使用 `git reset --hard`、`git checkout --`、递归删除或清理用户现有工作。
- 保持 `axis_state` 由 `motor_fsm` 所有；runtime 保护层只锁存故障并关闭 PWM。
- 快速保护路径不得打印、写 Flash、阻塞、申请堆内存或依赖任务调度。
- 每个结论区分当前新鲜命令证据、已有实板只读证据和尚未验证的推断。

---

## Task 1: Extend the host contract tests before implementation

**Files:** `tests/host/applications/test_motor_fault_runtime_contract.c`, `tests/CMakeLists.txt`。

- [x] 保留并运行现有的 `foc_fault_check` 过流 RED 测试，记录当前失败原因。
- [x] 增加 `foc_fault_check_fast` 的测试：相电流超过 bring-up 硬阈值时锁存 `FAULT_OVER_CURR`、只调用一次 `pwm_out_disable()`，且不改写 `axis_state`。
- [x] 增加 VBUS 无效、欠压、过压、非有限值和超速测试，并验证重复调用不会重复关闭 PWM。
- [x] 增加低于硬阈值但持续超过 3 s 的任务级告警测试，验证最终进入故障锁存；测试使用显式运行时复位，避免测试之间共享计数器。
- [x] 先只构建该测试目标，确认新增行为在实现前失败。

**Verification:** `cmake --build build/host-debug --target test_motor_fault_runtime_contract --parallel`；执行生成的测试程序并保留 RED 输出。

## Task 2: Align the host harness with the live AT32F415 target

**Files:** `tests/CMakeLists.txt`, `tests/host/support/encoder.h`。

- [x] 将 host target 的生成头文件路径从历史 `platform/at/at32f403a/generated/inc` 对齐到当前 `platform/at/at32f415/generated/inc`。
- [x] 为 host GPIO shim 补齐当前测试所需的 `GPIO_PINS_15` 契约，保持现有 shim 风格。
- [x] 重新配置并构建 host tests，确认不再因历史平台头文件或 GPIO 常量导致编译中断。

**Verification:** `scripts/flows/host/run_host_tests.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal` 至少完成 configure 和 compile 阶段；失败时保留首个真实错误。

## Task 3: Implement the fast fault path and task-level fallback

**Files:** `applications/runtime/motor_fault_runtime.h`, `applications/runtime/motor_fault_runtime.c`, `applications/tasks/motor_task.c`, `applications/algo/foc_config.h`。

- [x] 增加命名清晰的 bring-up 保护阈值：快速过流、持续告警电流、堵转电流/速度/持续时间和持续告警时间；阈值与现有产品额定参数分离。
- [x] 实现 `foc_fault_check_fast()`：只在 `RUNNING`/`CALIB` 检查驱动态；验证相电流、VBUS、速度的有限性和范围；硬故障锁存故障码并调用 `pwm_out_disable()`；保证幂等性。
- [x] 在 ADC 采样后、FOC 计算前调用快速检查；发现已锁存故障时立即跳过本次 FOC 计算，避免保护后重新写入 PWM 比较值。
- [x] 恢复任务级 `foc_fault_check()` 作为兜底：检查 dq 电流、母线电压、速度、温度/传感器有效性，并实现 3 s 持续告警与堵转规则。
- [x] 保持状态迁移在 `motor_fsm`，由任务上下文把锁存故障送入 FAULT；不在 runtime 直接写 `axis_state`。

**Verification:** 先运行 Task 1 的目标测试，再运行所有依赖 `motor_fault_runtime` 的 host contract tests；确认过流触发时间不依赖日志或任务等待。

## Task 4: Harden current-offset calibration for startup bring-up

**Files:** `applications/periph/foc_calib.c`, `applications/periph/foc_calib.h`, `applications/tasks/motor_task.c`, related host test files if needed。

- [x] 在采样窗口前加入明确的模拟量稳定等待，并确保校准阶段 PWM 输出处于已知的安全状态。
- [x] 对均值、标准差和原始 ADC 范围做有限性及合理性校验；失败时锁存 `FAULT_CALIBRATION_FAILED`、关闭 PWM，并阻止把无效 offset 继续送入 FOC。
- [x] 保持已有 Welford 统计逻辑，不改变三相 ADC 通道顺序和 offset 的物理含义。
- [x] 为成功和失败的校准判定补充可在无板环境运行的 contract test 或纯判定测试；不把实板采样结果伪装成 host 证据。

**Verification:** host calibration contract test（若该文件的硬件耦合无法独立链接，则以可测试的纯判定边界覆盖）；固件编译通过；静态检查确认失败路径不会继续调用后续校准。

## Task 5: Guard FOC parameter calculation against invalid VBUS

**Files:** `applications/periph/foc_ctrl.c` and its applicable host test/contract target。

- [x] 在 `1.0f / v_bus` 和电压限幅计算前验证 VBUS 为有限值且大于安全下限。
- [x] 无效 VBUS 时禁止产生新的有效调制输出，并让上层快速/任务保护路径负责故障锁存。
- [x] 增加 host regression coverage，避免零值或非有限 VBUS 产生 NaN/Inf 传播。

**Verification:** 相关 host target；固件 build；检查正常 VBUS 路径的现有计算和 PWM 量程没有回归。

## Task 6: Run the complete no-board validation and safe closeout

**Files:** docs indexes, `docs/code-wiki/motor-debugging-safety-and-procedure.md`, `docs/governance/Themis_工程维护日志.md`。

- [x] 运行 fault/calibration/FOC 相关 host tests 和完整 host flow，记录测试数量、失败项与首个错误。
- [x] 运行 `./run.sh build` 的等价 CMake 固件构建命令；在 host 安全目标和固件均通过后，按用户已授权的实机调试请求刷写并校验 full-diag ELF，但不执行电机启动。
- [x] 执行限定任务路径的 `git diff --check`、查看 diff 和工作树；限定路径通过。全仓检查仅命中既有 generated/build/map/backup 文件的尾随空白，未清理或覆盖用户已有修改。
- [x] 更新 Code Wiki 的职责、调用边界、保护时序、测试命令和当前证据状态；同步 README 索引与维护日志。
- [x] 在 host 安全目标和固件构建通过后，使用 CMSIS-DAP/OpenOCD 做只读/安全关闭复核：复位后确认故障/禁能状态和 PWM 关闭；不发 `state start`，不发送任何会产生驱动输出的命令。
- [x] 记录下一轮实机测试的单步上电、快速断电和停机后的日志分析入口；任何异常优先断电，分析延后到电机停止后。

**Verification:** host tests、firmware build、`git diff --check`、只读 SWD/串口状态检查均有可复现命令和输出摘要；未进行电机转动测试。
