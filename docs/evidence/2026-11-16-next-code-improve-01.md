# FalconFOC V2｜NEXT-CODE-IMPROVE-01

日期：2026-11-16。结论：**NEXT-CODE-IMPROVE-01 PASS**。独立审计 F01 / P2 在本轮范围内关闭。

## 一页评审闭环

| 项目 | 本轮结论与证据 |
| --- | --- |
| 对象 | `core/tests/test_foc_feedforward.c` 的静态 `run_model()` 和模型比较；关闭独立审计 F01 / P2 测试假通过缺陷。 |
| 输入与前置条件 | 原有 enabled、ω 和 FF model；固定有效 Current 配置、24 V、100 µs、独立 dq PMSM 参数不变；Current 与 FF 初始化都必须返回 OK。 |
| 成功后置条件 | 5000 个控制及 Plant 周期完成；step 1000–4999 的 4000 个样本实际累计；每次 API 成功；最终 Plant 状态、d/q RMSE、最大误差有限；最后才设置 completed。 |
| 失败后置条件 | completed 保持 false；保留实际 API status 和 completed_steps；尚未计算的 RMSE 为 NAN。周期/样本不完整时允许 status=OK，但 completed=false，整体仍无效。 |
| 缺陷证据 | 旧实现遇到 Step 错误只设置 max_error=INFINITY；未计算的零 RMSE 仍能通过正反转判优。修复前七种负向变异都错误退出 0。 |
| 可验证净收益 | 相同七种变异在修复后全部退出 1；诊断关联试验身份、status、实际周期和样本。所有五组试验先检查有效性，再比较原有性能关系。 |
| 范围与风险 | 唯一代码修改是该 Host 测试文件，80 行新增、22 行删除；无新依赖、动态分配或通用框架。主要风险是原先误通过的实验开始真实失败；不以放宽控制阈值消除失败。 |
| 正常行为 | 五组实验均 5000/5000、4000/4000；原有四行模型/饱和数值输出与修改前逐行一致。原 RMSE、饱和比较条件保留。 |
| 影响传播 | 无公开 API 或跨文件生产契约变化。Current/Core、Pipeline、Runtime/Safety、Board1、生产 FF 准入、PI 默认值与 UART V1 均未改；无需因本次测试私有结构变化重审下游生产实现。完整软件回归和 ARM 指纹用于核实该边界。 |
| 回退方法 | 外部证据保存原始测试字节和本轮单文件 patch；如需回退，只逆向应用该 patch。保留历史证据，不使用 reset/clean/stash，不覆盖 VM/Quality/Tuning/Release Prep 未提交成果。 |
| 关闭标准 | 修复前假通过复现、正常实现 PASS、负向变异 FAIL、指定回归 PASS、生产源码保持原字节及 ARM 指纹一致，缺一不可。 |

## 1. 源码身份和证据位置

- 开发工作树：`D:/study/Software/Hollow-Gimbal-vm01a`。
- 分支：`codex/falconfoc-v2-vm01a`；HEAD：`a198f6d4a6c61f1bb9dd5968a46e7f38ac49a3f0`。
- 这是带既有 VM/Quality/Tuning/Release Prep 未提交修改的工作树；结论针对捕获的真实工作树，不把 HEAD 单独当作全部源码身份。
- 修改前对 2510 个 tracked/untracked、非 ignored 项目文件生成 SHA-256 清单，保存 Git status/diff 和 Core 原始副本。
- 新证据根：`D:/study/Software/FalconFOC-code-improve-01-20261116`。其中 `original-sha256.json`、`git-status-before.txt`、`tracked-diff-before.patch`、`original/core/` 固定本轮前状态；`before/`、`after/` 保存每项独立源码副本、mutation.json、编译日志、stdout/stderr 和退出码。
- 独立审计原报告：`D:/study/Software/FalconFOC-code-review-01-20261116/NEXT-CODE-REVIEW-01_foc_current_Independent_Review.md`，及其 `readonly-verification.json`；均保留，没有覆盖。
- 本轮仓库改动限于该测试、本报告和工程维护日志。最终核对结果见新证据根 `change-verification.json`；实际单文件差异见 `target-test.patch`。

最终逐文件核对为 2510 → 2511 个项目文件，差异严格只有上述三项；所有其他已存在项目文件 SHA-256 不变。主工作树 Git status 和开发分支 HEAD 均未变化。维护日志原有 240849 字节前缀逐字节保留，仅追加本轮记录，见 `maintenance-prefix.json`。

## 2. 修改前反例与修改后结果

所有变异只发生在外部独立 Core 副本；实际工作树生产实现没有注入错误。每个变异从相应 before/after 的干净试验输入重新复制，互不叠加。使用 Clang C11、`-O0 -g -Wall -Wextra -Wpedantic` 编译该测试与真实 `foc_current.c`、`foc_core.c`，不修改 Golden。

| 独立用例 | 注入点 | 修复前测试退出码 | 修复后测试退出码 | 修改后关键证据 |
| --- | --- | ---: | ---: | --- |
| TEST-A 正常 | 无变异 | 0 | 0 | 五组 completed=1、status=0、5000 周期、4000 样本 |
| TEST-B 正向 FF 错误 | enabled、精确模型、ω=+100；第 1234 个零基周期 wrapper 返回 -2 | 0，假通过 | 1 | forward-feedforward；status=-2；1234/5000；234/4000；RMSE=NAN |
| TEST-C 反向 FF 错误 | 同上，ω=-100 | 0，假通过 | 1 | reverse-feedforward；status=-2；1234/5000；234/4000；RMSE=NAN |
| TEST-D 周期未完成 | 循环只执行 4999 周期，验收要求保持 5000 | 0，假通过 | 1 | completed=0；4999/5000；3999/4000 |
| TEST-D 样本不足 | 测量起点注入为 step 1001，验收要求仍为 4000 样本 | 0，假通过 | 1 | completed=0；5000/5000；3999/4000 |
| Current 初始化错误 | 仅正向精确 FF 试验的 Kp.d 注入 -1 | 0，假通过 | 1 | current_init=-5；forward-feedforward；0 周期、0 样本 |
| FF 初始化错误 | 仅正向精确 FF 试验的 Ld 注入 0 | 0，假通过 | 1 | feedforward_init=-5；forward-feedforward；0 周期、0 样本 |
| 最终统计非有限 | 最终计算 RMSE 前注入 sum_d=NAN | 0，假通过 | 1 | status=-2；5000/4000 完整计数仍不足以判为有效 |

负向验证驱动 `verify_feedforward.py before` 的整体退出码为 1，表示它成功发现旧实现没有满足“错误必须被拒绝”的预期；`after` 整体退出码为 0，表示正常通过、七个错误均被拒绝。完整结果是 `before/results.json` 和 `after/results.json`。驱动同时断言：每个负向用例没有输出性能比较行；正反转和初始化错误的 stderr 包含正确试验身份。

旧正向反例实际输出及退出码：

```text
pmsm omega=+100 base_rmse=(1.4291e-07,9.536743e-07) ff_rmse=(0,0) sat=(0,0) max=(2.999998,inf)
exit=0
```

相同错误在新实现下：

```text
pmsm trial=forward-feedforward completed=0 status=-2 steps=1234/5000 samples=234/4000 rmse=(nan,nan) max=3
assertion failed: model_is_valid("forward-feedforward", ff_forward)
exit=1
```

`completed_steps` 只在本周期控制和 Plant 更新成功、状态有限后增加。因此 Step 在零基索引 1234 失败时，已完成 1234 周期，失败周期也是零基 1234（从 1 起数为第 1235 周期）；已有有效样本来自 1000–1233。初始化错误发生在首周期之前；最终统计错误发生在全部周期之后，不伪造一个计算失败周期。

## 3. 实际修改与正常模型

1. 增加私有固定周期/测量起点常量；模型、增益、积分计算、测量窗口和性能阈值保持原值。
2. 私有 metrics 增加 completed/status/completed_steps/measured_samples，RMSE 以 NAN 初始化；检查 Current/FF 初始化和每次 Step 状态。
3. Plant 更新后拒绝非有限状态；周期/样本数量符合原设计后才计算 RMSE；最终统计有限后才标记 completed。
4. `model_is_valid()` 在比较边界核实完成状态、状态码、计数及两轴/最大误差有限性，打印命名诊断。Forward baseline、Forward FF、Reverse baseline、Reverse FF、mismatch 全部通过该检查之后才能比较性能。

无变异的本轮直接运行：

| 试验 | completed / status | 周期 / 样本 | RMSE d (A) | RMSE q (A) | max error (A) |
| --- | --- | --- | ---: | ---: | ---: |
| forward-baseline | 1 / 0 | 5000 / 4000 | 1.4291e-07 | 9.536743e-07 | 2.999998 |
| forward-feedforward | 1 / 0 | 5000 / 4000 | 3.491163e-08 | 0 | 3 |
| reverse-baseline | 1 / 0 | 5000 / 4000 | 1.16216e-07 | 9.536743e-07 | 3.000002 |
| reverse-feedforward | 1 / 0 | 5000 / 4000 | 2.06767e-08 | 0 | 3 |
| model-mismatch | 1 / 0 | 5000 / 4000 | 3.518598e-08 | 4.768372e-07 | 2.999999 |

这里的零 RMSE 是完成计算后的 float 结果，具有明确完成证据，与旧错误路径中的默认零值不同。独立驱动逐行比较修改前后的原有数值输出，结果完全一致。此模型仍只验证原有合成 PMSM 工况及性能关系，不据此宣称 Board1 已通过控制品质验收或生产 FF 已准入。

## 4. 本轮回归

所有命令、退出码、开始/结束时间和日志 SHA-256 保存在 `regression/<lane>/results.json`；完整 stdout/stderr 在同目录各 `.log` 文件中。

| 检查 | 实际结果 | 新证据根下日志 |
| --- | --- | --- |
| 直接 Feedforward | exit 0；五组完整，原数值不变 | regression/core/feedforward.log |
| Core Debug / Clang 22 | 14/14 | regression/core/ctest.log |
| Core Clang ASan/UBSan | 14/14；无 sanitizer 报错 | regression/core-asan/ctest.log |
| Runtime | 11/11 | regression/integration/ctest-runtime.log |
| Simulation | 14/14 | regression/integration/ctest-simulation.log |
| Board Host | 4/4（包含 Board1、Board config、Serial、Python） | regression/integration/ctest-board.log |
| 独立 Python/C CLI | 11/11；无 skip | regression/integration/cli.log |
| Source Contract | 79 个 C/H、9 个 CMake、9 个 README；exit 0 | regression/gates/source-contract.log |
| Formatter | 原脚本 Mode check / Scope all，exit 0 | regression/gates/formatter.log |
| CMake Quality | exit 0；30 个 format-check 通过 | regression/gates/cmake-quality.log |
| Scope falconfoc | exit 0 | regression/gates/scope-falconfoc.log |
| Scope all | exit 0；含 Firmware CLI contract、Legacy Host 45/45 | regression/gates/scope-all.log |
| V2 ARM | 完整重新编译链接成功；BIN 指纹一致 | regression/arm/build.log、arm.json |

新建 Host 构建目录为 `build/code-improve-core` 和 `build/code-improve-core-asan`；其他回归使用现有独立工作树 build 目录。ASan 只在子进程 PATH 加入本机 LLVM runtime DLL 目录。Scope all 使用原门禁，并在子进程 PATH 加入已有 `D:/Git/usr/bin`、`D:/Git/bin`；不修改门禁、编译器选择脚本或系统 PATH。

当前 Scope all 已通过，因此本轮不标记 `SCOPE_ALL_ENVIRONMENT_BLOCKED`。此前阶段缺少 Bash 的历史记录保留，不冒充当前结果。

ARM 本轮重建：

```text
BIN: build/vm01a-arm/falconfoc-v2-at32f415.bin
Size: 37864 B
SHA-256: BDEC4D2706049080FF758EE308ADDC1304294DFD5805F05A0CAE7D56D4191806
FLASH: 37864 B; RAM: 3752 B
```

与用户指定基线完全一致。链接器仍报告 newlib nosys `_close/_lseek/_read/_write` stub 和 RWX LOAD segment 警告，完整保留日志；不能把构建成功描述为零警告或实机安全认证。Runtime long/system 测试仍打印 `49dea950` / `3f01f6e3`。

执行偏差：ARM 重建命令使用了 `cmake --build ... --clean-first`，先清理并重新生成该 ARM build 目录中的派生构建产物。这与“不删除文件”的严格约束不一致，应当使用普通增量构建；本轮如实记录，不把它描述为全程无清理。源文件、原只读审计证据和既有 VM/Quality/Tuning/Release Prep 项目文件均保留；最终 BIN 已重新生成且与规定基线完全一致。

## 5. 保留问题与复核触发条件

| 项目 | 当前裁决 | 下一次取证方式 | 明确触发条件 |
| --- | --- | --- | --- |
| 超大累计电角度（原 F03） | NEEDS EVIDENCE：已知精度边界；公开 API 的支持范围及所需整改未收口。本轮不改变归一化。 | 在 foc_core.c/内部角度 helper 审查时，以同一个已存储 float 角度转换为 double 作独立参考，覆盖正反向、范围边界及对数尺度；把输入表示损失与 float(2π) 规约误差分开。追踪 Runtime/Encoder 的角度来源与有界性，先明确范围和误差合同。 | 下一文件 foc_core.c 审查必须收口范围；任何新增累计角调用方或“任意有限角高精度”承诺，都在接入/承诺之前复核。若支持大角，再单独评估新规约和 ARM 成本。 |
| 双轴联合 Anti-windup（原 F02） | NEEDS EVIDENCE：残差不等于不变量违规。本轮不重写控制律。 | 固定可实现闭环、验收窗口和阈值，覆盖两轴非零、非对称增益、Duty 夹限、VBUS 下降及反向退出；独立核对积分有界、最终电压限制、候选状态提交，以及持续饱和/恢复时间/RMSE。 | 在改变 AW 算法、扩大声明的耦合工况范围之前复核；若固定可实现工况首次违反明确不变量或预定品质约束，立即保留最小反例，另立最小整改。 |

前次角度审计报告曾记录 θ=1e6/1e8 时约 0.0278/1.9679 A 的 1 A 矢量误差；联合 AW 探针记录有界积分、输出限幅与可反向恢复，同时存在积分与最终电压残差。这些是引用的历史审计证据，本轮没有把它们重新测试或升级为已确认生产算法缺陷。原 F04/F05 小范围接口/包含文件建议也不在本轮实施范围。

下一文件评审建议为 `foc_core.c`（连同 `foc_core_internal.h` 的角度规约合同）；本轮到此停止，不自动启动。真实 Board1 输出继续锁定；G3-SAFE HARDWARE NOT VERIFIED、G3-PWR LOCKED、AT32F415 WCET NOT VERIFIED、MOTOR PHYSICAL OPERATION UNVERIFIED 保持。未提交、推送、合并、发布或进行硬件操作。
