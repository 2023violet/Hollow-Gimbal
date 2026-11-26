# FalconFOC V2｜NEXT-RELEASE-PREP-01 发布前工程收口证据

## 结论

- 工程准备：`NEXT-RELEASE-PREP-01 PREPARATION COMPLETE`
- 实际发布准入：`RELEASE_BLOCKED`
- 本轮没有 commit、push、merge、rebase、tag、Release、正式安装包或正式 `LICENSE`。
- Git Bash 已在本机发现并通过临时 PATH 使用，`Scope all` 的原始 Firmware CLI contract 已实际通过。
- 发布阻断集中在根项目许可证选择/权利人确认、第三方来源与 NOTICE 完整性、以及正式发布审批；不是由本轮软件回归失败造成。

当前安全边界继续保持：

```text
G3-SAFE HARDWARE NOT VERIFIED
G3-PWR LOCKED
AT32F415 WCET NOT VERIFIED
MOTOR PHYSICAL OPERATION UNVERIFIED
```

## 1. 身份与工作树保护

| 项目 | 实际值 |
|---|---|
| 工作树 | `D:\study\Software\Hollow-Gimbal-vm01a` |
| 分支 | `codex/falconfoc-v2-vm01a` |
| HEAD | `a198f6d4a6c61f1bb9dd5968a46e7f38ac49a3f0` |
| 原始开发基线 | `a198f6d4a6c61f1bb9dd5968a46e7f38ac49a3f0` |
| 主工作树 | `D:\study\Software\Hollow-Gimbal`，clean |
| 外部消费者 | `D:\study\Software\FalconFOC-core-consumer-release-prep-01` |
| Source Inventory | `docs/evidence/2026-11-16-next-release-prep-01-source-inventory.tsv` |

当前独立工作树仍包含 VM-01a～01d、Control Quality、Control Tuning 以及本轮文档的未提交差异。没有覆盖、删除、reset、clean、stash 或移动这些成果。`git ls-files --others --exclude-standard` 显示 26 个未跟踪项目文件；完整分类见 Source Inventory，而不是只依赖 `git diff`。

## 2. 产品定位收口

FalconFOC V2 当前对外应被描述为：

> 一个纯 C11、无强制 RTOS 依赖、面向有感 FOC 的可移植控制与 Runtime 库，提供 Host Virtual Motor 和 AT32F415 参考固件。

边界分为：

- **Core**：Clarke/Park、SVPWM、Current PI、Velocity PI、Position P、Trajectory。
- **Runtime**：H0/H1/Foreground、生命周期、命令/传感器发布、Safety、输出提交契约。
- **Reference Targets**：Host Virtual Motor、锁定的 AT32F415 Board1。
- **Tools**：Python CLI、V1 协议工具、虚拟设备、控制品质测量和 Host-only 整定研究。

AT32F415 是第一个参考目标，不是所有 MCU 的永久实现边界。当前没有宣称任意 MCU 即插即用、所有电机/采样拓扑通用、自动电机识别全流程、真实电机验收、物理功率级关断验收或 AT32F415 WCET 已证明。

根 README、docs 索引和 Host Quickstart 已同步到这个定位：

- [根 README](../../README.md)
- [Core README](../../core/README.md)
- [Runtime README](../../runtime/README.md)
- [Simulation README](../../simulation/README.md)
- [Protocol README](../../protocol/README.md)
- [Host Virtual Quickstart](../host-virtual-quickstart.md)

## 3. 外部 Core 消费者验证

外部验证目录没有复制 FalconFOC 的内部测试、Runtime、Device、Safety、Board1、FreeRTOS、Python 或 MCU 头文件，只包含：

```text
D:\study\Software\FalconFOC-core-consumer-release-prep-01\
├── CMakeLists.txt
├── main.c
└── README.md
```

消费者只包含公开头文件 `foc_current.h`，通过 CMake `add_subdirectory(<checkout>/core EXCLUDE_FROM_ALL)` 链接 `falconfoc-core`，完成：

1. `foc_current_init()` 初始化 Current PI；
2. 提供有效 phase current、electrical angle、VBUS、target 和 `dt_s`；
3. 执行真实 `foc_current_step()`；
4. 检查 `FOC_STATUS_OK`、`output_enable`、SVPWM sector 和有限 duty。

实际结果：

| 编译器 | 结果 |
|---|---|
| Clang 22.1.8 | Configure/Build/Run PASS；`status=0 output_enable=1 sector=2` |
| MinGW GCC 16.2.0 | Configure/Build/Run PASS；`status=0 output_enable=1 sector=2` |

这个结果证明 Core 的公开 C11 接口可以在仓库外被独立调用。它没有证明 Runtime、Device、Safety 或真实硬件接口已经具有同等独立包管理能力。Runtime 当前仍组合 `foc_device_sim.h` 与 `foc_safe.h`，属于下一阶段可移植性任务，不在本轮做大规模重构。

## 4. Core → Runtime → Device 依赖接缝

```text
Core
  ├─ foc_core.h / foc_current.h / foc_motion.h / foc_trajectory.h
  │   └─ 纯 C11、数学与控制状态、无硬件/RTOS/动态分配
  ↓
Runtime / Safety
  ├─ foc_pipeline.h
  ├─ foc_runtime.h
  └─ foc_safe.h
      └─ H0/H1/Foreground、Safety、生命周期、提交事务
  ↓
Device Contract
  ├─ foc_device.h
  └─ foc_device_sim.h
      └─ 固定 ADC 四 rank、phase order、PWM compare 映射、Host closed gate
  ↓
Reference Targets
  ├─ Host Virtual Motor / virtual-device
  └─ AT32F415 Board1 / locked board adapter
```

依赖归因：

- **可移植算法契约**：Core 的 SI 单位、d/q/αβ/ABC 数学、PI、Trajectory、显式 `dt_s` 和状态拥有权。
- **参考设备事实**：`foc_device_adc_config` 的四 rank、VBUS/current conversion、ABC/ACB phase order、PWM period/channel mapping、Board1 vendor startup。
- **尚未完成的抽象边界**：Runtime Pipeline 公开头文件目前直接组合 Device Simulator 和 Safety；它适合当前 Host/Board1 参考验证，但不能宣称是完全设备无关的 Runtime package。后续应单独进入 `NEXT-PORTABILITY-01`。

本轮没有把 `foc_device.h`、ADC rank 格式或输出回调伪装成所有采样拓扑的通用标准，也没有顺手重构 Runtime。

## 5. Source Inventory 与交付分类

完整逐路径清单见 [Source Inventory TSV](2026-11-16-next-release-prep-01-source-inventory.tsv)，包含 2,482 个已跟踪路径和 28 个未跟踪项目文件，并额外标记构建树和外部消费者构建树。

交付分类如下：

| 分类 | INCLUDE / EXCLUDE / REVIEW | 内容 |
|---|---|---|
| SOURCE | INCLUDE | `core/`、`runtime/`、`g3/np/`、`g3/safe/`、`sensors/`、`calibration/`、`protocol/`、Host `simulation/`、必要工具源码 |
| TEST | INCLUDE | Core、Runtime、Sensors、Calibration、G3-NP、G3-SAFE、Protocol、Simulation、Board Host、Python/C CLI 测试 |
| DOCUMENTATION | INCLUDE | 根 README、各模块 README、Quickstart、架构/治理文档、当前证据报告 |
| RESEARCH EVIDENCE | INCLUDE | VM-01a～01d、Control Quality、Control Tuning 报告与 `build/control-tuning-01` 原始 JSON/CSV |
| BUILD ARTIFACT | EXCLUDE | `build/**` 下的 CMake cache、二进制、CSV/JSON/log；仅作为本地复现证据，不进入源代码发布清单 |
| TEMPORARY | EXCLUDE | 临时 scratchpad、外部消费者 `build/`、个人绝对路径、调试日志和系统缓存 |
| REQUIRES_REVIEW | REVIEW | AT32F415 generated/vendor/CMSIS、FreeRTOS、Artery 生成 BSP、Legacy applications、任何未完成权利人确认的继承代码 |
| OUT OF SCOPE | EXCLUDE | `work/` 图形工程源，不纳入 FalconFOC live library 发布边界 |

本轮没有把本机绝对路径或构建目录写入正式源代码入口；报告中的路径只用于证据定位。

## 6. 第三方来源与许可决策材料

本地检查得到的事实：

| 来源 | 证据 | 当前处理 |
|---|---|---|
| FalconFOC 自研 Core/Runtime/Host 工具 | 当前源码和架构文档；没有根 `LICENSE` 或统一 SPDX | 权利人/项目所有者需确认版权归属和项目许可证 |
| FreeRTOS Kernel V10.4.3 LTS Patch 3 | `kernel/freertos/LICENSE` 与源文件 MIT 文本 | 保留原版权/许可文本；不把 FreeRTOS 内容并入 Core 许可证声明 |
| CMSIS/CMSIS-DSP | `platform/at/at32f415/vendor/cmsis/**`；`dsp/ComputeLibrary/README.md` 明确 Compute Library 部分 MIT、其余 CMSIS-DSP 部分 Apache-2.0 | 需要锁定上游版本、补齐对应 LICENSE/NOTICE 和修改说明；当前本地 vendor 树缺少统一发行清单 |
| Artery AT32 生成/BSP 代码 | generated 文件头含 Artery custom copyright/use/disclaimer；非通用 OSS 许可证 | 只能按 Artery 条款和 MCU 配套用途审查；需确认可公开再分发范围和保留 notices |
| CMake/Ninja/Clang/GCC/Python | 构建/验证环境工具，不随 FalconFOC 源代码发布 | 在 CI/开发者文档中列为工具前置条件，不复制其许可证到项目根 |

MIT 与 Apache-2.0 决策要点：

- **MIT**：保留版权和许可文本；允许使用、修改、再分发、销售；没有 Apache-2.0 那样的明确专利授权条款；下游通知负担最小。
- **Apache-2.0**：保留版权、许可和 NOTICE；需要标注修改；包含明确专利授权及终止条款；对涉及控制算法和实现专利风险的下游更清晰。
- FreeRTOS 的 MIT 条款与 Apache-2.0/MIT 项目代码通常可组合，但不能替代 CMSIS、Artery 或继承 Legacy 的来源确认。
- CMSIS-DSP 本地材料同时涉及 MIT 和 Apache-2.0，不能把整个 vendor 目录简单归为一种许可证。
- Artery 的 custom 条款不应被推断为 MIT、Apache-2.0 或 BSD。

推荐决策材料（未执行）：如果权利人确认 FalconFOC 自研代码和继承 Legacy 代码的版权链，建议优先评估 Apache-2.0 作为项目自研部分的项目级许可，以获得明确专利授权；若项目所有者明确偏好最小通知负担且不需要显式专利条款，再评估 MIT。无论选择哪一个，都必须把第三方 FreeRTOS、CMSIS-DSP、CMSIS、Artery 代码的原始 notices 分层保留。

当前发布阻断：没有权利人确认、没有选定项目许可证、没有统一第三方 `NOTICE`/来源清单。因此本轮不添加正式 `LICENSE`，也不宣布 `RELEASE_READY`。

## 7. VM / Control Quality / Control Tuning 归档状态

必须保留并已保留：

```text
RESEARCH COMPLETE
IMPROVEMENT CONFIRMED
HOLDOUT PASS
```

Host Research Profile 仍为：

```text
Current Kp = 1.26       Current Ki = 630.0
Velocity Kp = 0.225     Velocity Ki = 1.75
Position Kp = 5.0
```

这些参数只标记为 `HOST RESEARCH PROFILE ONLY`，没有写入真实 Board1 配置。短窗口反向 Current 的 `CONTROL_QUALITY_FAIL` 仍保留；`HOLDOUT PASS` 只表示本研究定义的硬约束/独立验证通过，不表示所有工况的产品质量达标。

原始数据位置：

- [Control Quality report](2026-11-16-next-control-quality-01.md)
- [Control Tuning report](2026-11-16-next-control-tuning-01.md)
- `build/control-tuning-01/study.json`
- `build/control-tuning-01/optimization/`
- `build/control-tuning-01/holdout/`
- `build/control-tuning-01/confirmation/`

没有删除历史 VM/Quality/Tuning 实验，也没有把临时调参日志放到根目录。

## 8. 完整软件门禁

| lane | 结果 |
|---|---|
| External Core consumer / Clang | Build + Run PASS |
| External Core consumer / MinGW GCC | Build + Run PASS |
| Core CTest | `14/14 PASS` |
| Runtime CTest | `11/11 PASS` |
| Simulation CTest | `14/14 PASS` |
| Simulation Clang ASan/UBSan | `14/14 PASS` |
| Sensors | `2/2 PASS` |
| Calibration | `1/1 PASS` |
| G3-NP | `2/2 PASS` |
| G3-SAFE | `1/1 PASS` |
| Board Host | `4/4 PASS` |
| Protocol | `1/1 PASS` |
| Python/C CLI + Control Tuning tests | `11/11 PASS` |
| Legacy Host GCC | `45/45 PASS` |
| Source Contract | `79 C/H、9 CMake、9 README PASS` |
| Formatter | PASS |
| CMake Quality | PASS |
| Scope falconfoc | PASS |
| Scope all | PASS；使用已有 `D:\Git\usr\bin\bash.exe` 加入临时 PATH |

`Scope all` 的 Bash 依赖经过实际检查：

- `D:\Git\usr\bin\bash.exe`：GNU Bash `5.3.15`；
- Firmware CLI contract 实际执行并报告 `all assertions passed`；
- 没有安装系统级依赖，也没有修改门禁或跳过项目；
- 默认 PowerShell PATH 仍不含 Bash，但受支持的本机 Git Bash 已可被显式加入执行环境；CI 应使用明确的 Git Bash/MSYS2/Linux runner，而不是依赖未声明的 PATH 偶然状态。

## 9. ARM 安全和产物一致性

重新构建 `build/vm01a-arm`，结果：

```text
BIN size: 37,864 B
SHA-256: BDEC4D2706049080FF758EE308ADDC1304294DFD5805F05A0CAE7D56D4191806
```

与本阶段前一致。Board Host/Serial contract 保持：

- Board1 默认 Target Policy 为 `LOCKED`；
- ARM/START 返回 `OUTPUT_LOCKED`；
- Board1 UPDATE 继续被拒绝；
- Host Virtual 的 `VIRTUAL_ONLY` 不传播到真实 Board1；
- Virtual telemetry 的 `physical_applied=false`；
- 未修改生产 PI、Runtime Safety、UART V1、Board1 Timer/ADC/SPI 或输出锁。

本阶段没有刷机、上电、发起真实 PWM/MOE、运行真实电机或测量 WCET/栈高水位。

## 10. 发布文件清单与当前阻断项

建议进入下一轮非公开候选审查的内容：

- `core/`、`runtime/`、`g3/np/`、`g3/safe/`、`sensors/`、`calibration/`、`protocol/`；
- `simulation/` 和 `tools/falconfoc-cli/`；
- 与上述目录匹配的 CMake/测试入口；
- 根 README、模块 README、Host Quickstart、当前架构文档；
- VM/Quality/Tuning 证据报告和机器结果索引；
- 在许可证确认后，分层的第三方许可证/NOTICE 材料。

当前明确 `RELEASE_BLOCKED` 的事项：

1. 项目根许可证尚未选择，权利人/版权链尚未确认；
2. CMSIS/CMSIS-DSP 上游版本与 MIT/Apache-2.0 notices 尚未形成完整可分发清单；
3. Artery generated/BSP custom 条款的公开再分发边界尚未由权利人确认；
4. Legacy 继承代码的版权和再分发范围仍需单独核对；
5. 当前工作树仍是未提交的研发工作树，没有发行提交、tag 或正式包；
6. 真实电机、功率级关断、MCU WCET 和产品级控制品质仍未验证，应在发布说明中明确为未验证限制。

这些阻断项的关闭条件是：完成权利人确认和分层 license/NOTICE、冻结可复现提交、通过正式发布审查，并在发布说明中保留当前硬件与控制品质边界。它们不要求本轮修改生产算法。

## 11. 后续最小任务

1. `NEXT-LEGAL-PROVENANCE-01`：锁定自研/Legacy/CMSIS/Artery/FreeRTOS 的来源、版本和再分发许可，形成 NOTICE 草案并由权利人选择项目许可证。
2. `NEXT-PORTABILITY-01`：在不改变控制器语义的前提下，评估 Runtime 与 Device/Safety 接缝是否需要独立可移植包边界。
3. `NEXT-CI-RELEASE-01`：在带明确 Bash 的 CI runner 上冻结 Core、Runtime、Simulation、Protocol、Python 和 ARM hash 门禁。
4. `NEXT-MCU-VALIDATION-01`：在硬件授权和测量方案具备后，验证 AT32F415 WCET、抖动、栈水位和故障到输出关闭延迟。

本轮完成工程准备后停止，不自动提交、合并、发布或进入硬件阶段。
