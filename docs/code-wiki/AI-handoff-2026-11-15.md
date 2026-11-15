# FalconFoc V2｜2026-11-15 当前交接入口

## 先读什么

这份文件是当前软件候选基线的交接入口。它覆盖当前已推送代码、软件验证结果、真实边界和下一步入口；早期 `AI-handoff-*`、调试总结和板级记录保留为历史证据，不再作为当前状态的唯一来源。

软件基线提交：`278c6911e65d7782bb58d7b0b023fdcb9f414c08`

当前分支：`codex/falconfoc-v2-rc0`
远端：`origin/codex/falconfoc-v2-rc0`

## 项目现在是什么

FalconFoc 是面向 AT32F415 的单轴 FOC 固件仓库，当前软件候选已经包含：

- Float32 Clarke/Park/逆 Park/SVPWM 与电流控制核心；
- 电流 PI、速度/位置/轨迹控制、磁编码器反馈与传感器质量处理；
- 校准参数、安全准入、配置快照和运行时状态事务；
- Runtime/Pipeline、G3-NP、G3-SAFE Host 契约；
- UART/Shell 当前控制面、Legacy FreeRTOS 固件和独立 V2 Board 目标；
- AT32F415 平台时序事实：150 MHz timer input、Legacy TMR1 20 kHz event、TMR5 5 kHz soft IRQ、V2 1 MHz timebase。

当前 CPU/ABI 事实：AT32F415 使用 `cortex-m3-soft`、无 FPU、soft-float、FreeRTOS `ARM_CM3` port。该配置已经验证可构建，不应被当作缺陷自行改动。

## 已验证到哪里

`NEXT-RC0-BASELINE-01 PASS` 已证明当前工作树可以从独立源码快照重新构建：

- 源码清单 1661/1661，缺失 0，意外 0，SHA-256 不一致 0；
- Host Debug 45/45；
- Clang ASan/UBSan 35/35；
- Legacy tiny、standard、full-diag ARM 构建通过；
- V2 AT32F415 ARM 构建通过；
- Source contract 68 C/H、8 CMake、8 README；
- CMake formatter 29/29；
- `Scope falconfoc` 和 `Scope all` 通过；
- SYSTEM replay `3f01f6e3`，Long replay `49dea950`；
- V2 Flash `36,736 B`，RAM `3,632 B`，V2 ELF/BIN/HEX 与独立基线一致。

完整证据见 [NEXT-RC0-BASELINE-01](../evidence/2026-11-15-next-rc0-baseline-01.md) 和 [Themis 工程维护日志](../governance/Themis_工程维护日志.md)。

## 当前架构边界

```text
platform/at/at32f415
  -> generated startup / vendor / linker / peripheral facts
bsp/board + bsp/ports
  -> board facts and thin control-plane ports
applications
  -> product startup bridge, motor orchestration, shell, parameters
components / subsys
  -> reusable code and framework mechanisms
core / sensors / calibration / runtime / g3
  -> FalconFoc V2 reusable control and validation surfaces
```

FOC 热路径可以直接使用 vendor headers、生成代码和寄存器；软任务和控制面硬件通过薄 BSP port 访问。不要重新引入完整 `device/`、`drivers/` runtime，也不要为了“可移植”给热路径套通用 HAL。

当前实际启动链仍以生成式 AT32F415 入口为准：

`platform generated main -> applications/app_main -> bsp_init -> export bring-up -> motor_task_init -> shell_task_init / motor_task / motor_safe_task / vofa_task`

## 不能宣称的事情

- `G3-SAFE HARDWARE NOT VERIFIED`；
- `G3-PWR LOCKED`；
- `AT32F415 WCET NOT VERIFIED`；
- `MOTOR PHYSICAL OPERATION UNVERIFIED`。

Host 测试、静态资源、回放指纹和 ARM 构建不等于实板安全、真实 PWM 响应、栈高水位、连续旋转、温升或驱动器故障响应。任何刷机、上电、启用 PWM 和电机运行都需要单独的硬件准入。

## 新人第一天的命令

```powershell
git switch codex/falconfoc-v2-rc0
git log -1 --oneline
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/host/run_host_tests.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_themis_quality_gate.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal -Scope all
cmake --preset fw-full-diag-carrier-debug
cmake --build --preset build-fw-full-diag-carrier-debug --parallel 4
```

Host 运行优先使用项目允许的 MinGW GCC；当前 Windows Clang 22 会被 `include/tmdef.h` 的编译器契约拒绝。正式门禁中的 Git Bash、Mock OpenOCD 和 sanitizer 入口要保持隔离，不能直接对真实设备执行 CLI smoke。

## 下一步建议

软件 RC0 已形成可提交候选。下一步应从新的任务或提交开始，优先做独立 AT32F415 时序/栈测量准备和硬件安全准入设计；不要继续扩张 Host 验证框架，也不要把历史板级报告当作当前实机通过证据。
