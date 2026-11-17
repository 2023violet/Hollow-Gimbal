# FalconFOC V2｜NEXT-MOTION-CONTRACT-01

日期：2026-11-17。软件验收裁决：**PASS**。用户已在验收后明确批准本次普通 commit/push；最终提交 SHA 与远端读回核对结果单独记录在交付回复及外部 publication-verification.json，避免提交内容的自引用。

## 1. 基线、范围与保护

- 仓库：`2023violet/Hollow-Gimbal`；分支：`codex/falconfoc-v2-vm01a`。
- 源码基线：`6b151a1ca745a37b0cd044528058f34b09834919`。任务开始时本地 HEAD、远端分支一致，工作树无未提交文件；交付前再次读取远端仍是同一 SHA。
- 实际工作树：`D:/study/Software/Hollow-Gimbal-vm01a`。主工作树 `D:/study/Software/Hollow-Gimbal` 未修改。
- 已阅读 AGENTS、Core README、Motion 源码/头文件/正式测试、CR03 与最新 QUALITY-GATE-01 报告。后者已关闭的 PowerShell 参数转发 P2-B 保持关闭，本轮未修改 Gate。
- 所有构建和变异使用新的外部目录 `D:/study/Software/FalconFOC-motion-contract-01-20261117`。使用不可变起始快照、私有 Git clone 和单独变异副本，未覆盖原工作树的构建目录。
- 2533 个原文件只允许本任务的五个旧文件发生变化；原 2427 个 ignored 文件（322054220 bytes）逐文件 SHA-256/大小保持不变。原快照保持不变。详见 [change-verification.json](2026-11-17-next-motion-contract-01/change-verification.json)。

差异清单：

| 文件 | 本轮改动 |
|---|---|
| `core/tests/test_foc_motion.c` | 计算溢出状态原子性、清输出、恢复；预测错误清输出断言 |
| `core/tests/test_foc_motion_reference.c` | 独立 Double 预测 Oracle、名义矩阵/固定种子与域外观测 |
| `core/include/foc_motion.h` | 仅直接相关契约注释；声明/结构/数值类型未变 |
| `core/README.md` | 三层预测契约及现有 CTest 覆盖说明 |
| `docs/governance/Themis_工程维护日志.md` | 仅追加本阶段记录 |
| 本报告与同名证据目录 | 新增 results、保护核对与原始日志归档 |

`core/src/foc_motion.c` 原文件 SHA-256：`2c91ab2fcf2b202d3f94e199c1b31642f81cea2e51dbdb320194639680249174`，与任务开始逐字节相同。其余生产 C 源、默认参数、Runtime、Board1、Safety、PWM、UART V1、相位来源、Golden、正式 CMake 和脚本均未改动。公共头文件去注释后的内容一致。

## 2. 具体测试缺口

旧 `test-foc-motion` 验证正常 PI、限幅、disabled、非法输入，但没有在算术溢出返回错误时，比较非零积分及整个逻辑状态。因此错误分支把 trial 提前写入 context，仍能通过旧正式 Core 测试。

旧预测测试在低速样本上用 Float32 时间转换、相位累加和 fmodf 构造 truth，并用 `2e-5` 近似比较；原独立 `test-foc-motion-reference` 的 5000 组覆盖 Rotor/Velocity/Position/Trajectory，未对预测器建立独立精度 Oracle。它们不能验证本任务的名义域 `2e-6 rad` 预算。

这些是测试覆盖和数值合同缺口，不是本轮发现生产 PI/预测算法错误。本轮没有替换算法，也没有改变任何原测试阈值或错误码。

## 3. Task A：计算错误原子性

新增 `test_velocity_error_atomicity()`，复用原 `test-foc-motion` CTest 入口。两组配置分别配合 `ref=+2/-2`，`measured=0`、`dt=1`、`limit=2`、enabled=true。初始化成功后将积分设为 `0.25`：

| 配置 Kp / Ki / max_dt | 正输入计算路径 | 负输入计算路径 |
|---|---|---|
| `0 / FLT_MAX / 1` | trial 为 +Inf | trial 为 -Inf |
| `FLT_MAX / 1 / 1` | trial=2.25 有限，P/requested 溢出 | trial=-1.75 有限，P/requested 溢出 |

每个反例均要求 NONFINITE，积分恰为 0.25，三个配置字段和 initialized 全部不变。比较逻辑字段，不依赖 struct padding。输出预填 `{0.75, true, true}`，要求 iq_ref、saturated、enabled 三项全部清零。

随后在同一实例依次检查 valid disabled、NaN reference、零 dt，以及恢复 enabled/零误差的正常计算；不 reset、不重新初始化。错误和 disabled 继续保持状态、清输出，恢复输出为 enabled、未饱和、iq=0.25。原有非零正常积分、饱和与限流下降测试保留。

预测现有 model-error、超 horizon、非有限速度三条错误测试也预填非零输出，正式断言失败后 angle=0。

## 4. Task B：独立预测 Oracle 与工作域

Oracle 从实际存储的 Float32 theta/omega 转为 Double，独立计算：

```text
dt_ref = (double)age_us / 1000000.0
phase_ref = wrap_double((double)theta + (double)omega * dt_ref, mathematical 2*pi)
error = abs(remainder((double)actual - phase_ref, mathematical 2*pi))
```

没有调用生产 Float32 时间转换、相位累加或归约来生成参考答案。周期最短距离避免 0/2π 边界的伪失败；同时独立检查生产输出有限且落在 `[0, Float32 2*pi)`。失败诊断输出完整 theta、omega、age、actual、reference、circular_error、budget、model contribution 与 status，可直接复现。

工作域：输入相位 `[0, Float32 2*pi)`，`|omega_e| <= 7000 rad/s`，整数 age `0..650 us`，Board 默认预测策略 `{650 us, 2000 rad/s^2, 0.00065 rad}`。预算固定为 `2e-6 rad`，未放宽。

| 矩阵 | 数量 | 覆盖 |
|---|---:|---|
| 6 相位 × 13 速度 × 651 age | 50778 | 零/正/负速度、速度边界、接近零/π/2π相位、全部整数 age |
| ±7000 × 651 age × 3 个相邻 Float32 相位 | 3906 | 每个 age 的预测周期边界及相邻相位 |
| 固定种子 `0x0c012026` | 20000 | 域内额外 theta/omega/age 组合 |
| 合计 | **74684** | 原 5000 组独立运动参考另行保留 |

本轮 Clang Debug、GCC Debug、Clang ASan/UBSan 均得到：

- 最大样本计算误差：**1.0674847210623284e-6 rad**，小于 2e-6。
- 对应存储输入：theta=`6.2831850051879883`，omega=`7000`，age=`623 us`。
- 这些是新正式测试的实测结果，不引用历史随机样本作为本轮执行证据，也不宣称是整个连续/Float32 工作域的严格上界。
- 使用 binary32 float、至少 binary64 double，正常舍入、无 fast-math 的项目编译条件。未在目标 MCU 上运行此数值矩阵。

### 三层合同

1. **准入。** 既有 finite、非负模型限制、half-range horizon、elapsed horizon、模型贡献检查及归一化错误处理保持。可写输出在错误时清零。OK 仅表示通过这些检查，不附加任意合法公共 API 输入的精度保证。
2. **计算误差。** 上述 74684 样本约束 Double 对照下的计算误差；起点是已经存储的 Float32 输入，输入储存前的真实量化误差不在 Oracle 内。该测试包络不是新的 API 输入拒绝范围。
3. **系统总误差。** 加速度模型误差、编码器量化/速度估计误差、传感器采集和时间戳误差，均须独立预算。`max_error_rad` 只比较 `0.5 * max_accel * dt^2` 的假设贡献；默认 650 us 下该贡献为 0.0004225 rad。它既不是总误差保证，也没有证明真实加速度满足该假设。

本轮覆盖默认 Board 预测设置与包含 Host Virtual 的数值包络；原 Runtime/Simulation 集成回归也重新执行。但这不等于完整默认电机控制品质认证、真实电机参数标定或系统传感器精度认证。

### 保留的域外高速样本

theta=`0.35820722579956055f`，omega=`65535000 rad/s`，age=`400 us`，策略 `{400 us, 2000 rad/s^2, 0.0003 rad}`：status=OK，actual=`0.90759086608886719`，Double reference=`0.90910567256580066`，圆周误差 **0.0015148064769334724 rad**。模型贡献仅为 0.00016 rad。

该样本说明 OK 与模型贡献限制不能替代计算精度认证。正式测试仅要求保留原准入行为、有限有界输出并打印误差；不套用名义阈值，也不反向要求误差必须维持很大。更大速度、无界相位/offset、更长预测 horizon、不同舍入或 fast-math、所有未采样 Float32 组合均未获得此项精度保证。

## 5. G1 / G2 / G3：GREEN 与有效 RED

- G1：不可变起始快照，新 Core 构建，Clang 22.1.8 Debug **15/15**。
- G2：新增测试及契约叠加到私有 clone，新构建 **15/15**；CTest `-N` 确认原 `test-foc-motion` 与 `test-foc-motion-reference` 仍注册，计数未增加。直接执行也记录四个 PI 反例和 74684 数值样本。
- G3：每个变异各有独立 source、旧测试构建、新测试构建；没有修改原工作树生产源。下列各行均是本轮实际执行：

| 隔离变异 | 旧 Core 测试 | 新正式测试 | 实际失败证据 |
|---|---|---|---|
| 算术错误分支写入 trial | 15/15 PASS | `test-foc-motion` FAIL | 积分 +Inf，预期0.25 |
| 仅 trial<0 时提前写入 | 15/15 PASS | 同上 FAIL | 正例通过后，积分 -Inf |
| 仅有限 trial 时提前写入 | 15/15 PASS | 同上 FAIL | Ki 溢出例通过后，P 溢出时积分2.25 |
| 时间换算 `1e-6f → 1.000001e-6f` | 15/15 PASS | `test-foc-motion-reference` FAIL | theta=0，omega=-7000，age283，误差2.1559344691723936e-6 |

每个旧/新构建均编译成功。新 CTest exit=8、直接可执行程序 exit=1，失败来自状态/数值行为断言；编译失败不计为检出。没有改宽断言或删旧检查。

首个运行器尝试保留了复制文件的旧 mtime，复用构建器返回 `no work to do`，导致未执行新测试。该尝试已明确排除为无效 RED，保留在 `mutations/`；有效证据来自重新创建的 `mutations-fresh/`，每一步都使用独立的新构建目录。此为隔离取证流程问题，非产品缺陷。正式 G4 在所有有效变异完成后，按 Core→Runtime→Simulation→Gate 的顺序重新执行。

## 6. G4：完整回归

正式 G4 在私有 clone 中执行用户指定的三条 Core 命令，child `CC` 指向 Clang，其余参数未改，新的 `build/motion-contract-01` 从零配置：

```text
cmake -S core -B build/motion-contract-01 -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/motion-contract-01 --parallel 4
ctest --test-dir build/motion-contract-01 --output-on-failure
```

| 本轮新执行项目 | 结果 |
|---|---|
| Core / Clang 22.1.8 Debug，正式 G4 | 15/15 |
| Core / GCC 16.2.0 Debug，额外交叉编译器检查 | 15/15 |
| Runtime / Clang Debug，正式 G4 | 11/11 |
| Simulation / Clang Debug，正式 G4 | 15/15 |
| Core / Clang ASan+UBSan | 15/15 |
| Simulation / Clang ASan+UBSan（含11个Runtime条目） | 15/15 |
| Source contract | 81 C/H、9 CMake、9 README，exit0 |
| 修改的三个 C/H 格式检查 | exit0 |
| 严格 `Scope all -RequireCMakeFormatter -Summary`，正式 G4 | exit0，`[Total] PASS`，30 个 CMake 格式检查、Legacy Host45/45 |

完整 Gate 的 preset/backend/smoke/firmware CLI mock/cache guard/transport-shell/current artifact contract 等按现有流程通过。没有重新实现上一轮已关闭的参数转发问题。Host cache 在私有 clone 先用 documented runner 和显式 GCC 配置。

current artifact contract 所需的6个 Legacy ELF/MAP 是从受保护目录复制并核对 SHA 的**历史产物**；不是本轮新 ARM 重建。未运行刷机、功率输出或电机。PI、Runtime、Board1 软件行为保持的证据是生产源/参数逐文件不变和本轮回归通过；不将此扩大为硬件正确性或控制品质改善。

## 7. G5：结果审查与裁决

- 新检查确实由原正式 CTest 入口调用，原测试保留，总计仍15个条目。
- 独立 Oracle 采用 Double 时间、数学周期和短角距离；没有让生产 Float32 路径给自己算答案。
- 4个可编译的负向变异均被原来无法检出的新行为断言拒绝；未变异生产实现完整回归通过。
- 默认数值包络的样本结果通过预算，域外反例未被包装成精度达标；model-error 与系统误差明确分离。
- 五个旧文件差异和新增证据均在 allowlist，旧产物、旧 evidence、主工作树及源快照受保护；生产实现/公共声明未变化。

**PASS** 仅针对本任务的测试、数值合同和软件回归强制验收。两个指定的 P2 跟进项在此范围内关闭。本轮未确认新的 P0/P1/P2 生产缺陷。新增测试提升回归检出能力，不宣称控制品质得到提升。

## 8. 剩余边界与下一项建议

数值全域证明、真实传感器总误差、目标 MCU libm 行为与 WCET 仍未验证。未被 live Gate 调用的旧 AT32F403A 静态检查保持独立跟进，本轮未运行/清理它，亦未将其标为通过。

下一项建议：沿实际 Sensor→H1 发布→H0 预测→Current 软件链建立**系统角度误差预算与组合扰动验收**。已有鲁棒性研究包含量化、速度噪声和固定延迟，已有整定也包含 holdout，不应重做这些已完成的工作；进一步应把有界量化、速度估计偏差、采集延迟及时间戳抖动/回绕组合到实际软件链，使用独立参考检查预测相位及 d/q 影响，并区分超预算与数据失效的预期行为。Host 注入只验证所声明的假设，不替代真实传感器标定。保留现有控制品质指标与退化标签，本轮不自动启动此项，不改算法或物理输出权限。

```text
G3-SAFE HARDWARE NOT VERIFIED
G3-PWR LOCKED
AT32F415 WCET NOT VERIFIED
MOTOR PHYSICAL OPERATION UNVERIFIED
```

## 9. 可复核证据与 Git 停止点

仓库证据：[results.json](2026-11-17-next-motion-contract-01/results.json)、[change-verification.json](2026-11-17-next-motion-contract-01/change-verification.json)、[raw-logs.zip](2026-11-17-next-motion-contract-01/raw-logs.zip)。归档含 argv/cwd、UTC、exit、日志 SHA-256、CTest 输出、变异定义、运行器与三类数值结果；完整构建及所有隔离源仍保留在外部证据目录。zip 不是新的生产测试框架。

差异、未跟踪文件和保护核对完成后，按本轮第7节先停止并请求确认；用户随后明确回复“批准本次 commit / push”。确认前本地/远端均为基线SHA，未提前提交或推送；change-verification.json 保留该时点的保护收据。获批后重新检查分支/远端身份，仅显式暂存本任务9个文件，普通提交/推送并读回远端 HEAD；核对一致后才报告 Git 交付完成。不 force、不合并、不改写历史，不进入下一轮。
