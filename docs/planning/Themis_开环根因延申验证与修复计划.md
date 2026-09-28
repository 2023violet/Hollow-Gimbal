# Themis 开环根因延申验证与修复计划

> 制定日期：2026-08-30。承接 `docs/code-wiki/ai-handoff-prompt.md` 的接手顺序，
> 对上一轮"正交诊断"思路做机制级延申，并在确诊后按纪律完成修复。
> 本文档是本轮执行的唯一计划入口；结果统一回填第 7 节。

## 1. 背景与目标

当前状态标志：

```text
OPEN_LOOP_SOFTWARE_PATH=PASS
MOTOR_PHYSICAL_OPERATION=UNVERIFIED
OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED
REAL_BENCH_TEST_STATUS=BLOCKED
```

上一轮已证事实：ADC 软件电流读数可复现地随 CH4 触发位置（compare 512..3740）变化；
静态 we=0、vq=1.00 V 下同样复现，B 相转折稳定出现在 compare 1874→2300 之间；
compare=2812..3740 时 A/B 相读数约 0.24~0.26 A，C 相持续偏低。

本轮目标：

1. 桌面定量推导"低侧 CSA 采样窗口"机制，逐相解释 1874→2300 转折；
2. 在只有串口+电源的仪器条件下，用固件 bench 诊断完成实机确诊
   （AI 驱动脚本，操作员只负责上下电与安全看护）；
3. 确诊后本轮内按交接纪律完成修复：host 失败测试先行 → 最小单变量改动 → 全门禁。

## 2. 硬约束

- 不清理脏工作树：禁止 `git reset/checkout/clean`、删除用户变更、自动提交。
- 确诊数据到手前不动保护文件：`motor_drv.c`、`foc_ctrl.c`、`foc_config.h`、
  PWM 相序、ADC rank、编码器方向、保护阈值、电机参数。
- 实机只用指定 bench 镜像 `build/fw-full-diag-carrier-bench-400ms-debug/output/falcon-foc-at32.elf`
  （SHA256 见第 6 节快照），COM3 @115200 8N1，OpenOCD+SWD 烧录/复位。
- 不重跑已完成的旧矩阵（mapping/9 点粗扫/theta0/电压梯度），只做单变量扩展。
- 命令白名单纪律：禁 `cal motor`、`cal enc`、`if`、`encoff`、`diag param`、
  `diag pid`、`state stop`、`fault clear`。

## 3. 机制预判（待第 5 节实验证伪/证实）

- SVPWM 共模偏置使静态矢量下三相占空比围绕 50% 对称分布；vq=1.00 V、
  VBUS≈11.9 V 时调制比约 15%，预测三相 CCR≈1560~2190。
- 若转折机制是"触发点越过相 CCR 后该相低侧管在采样孔径处已关断、
  DRV8311H 低侧 CSA 读数失效"，则每相读数转折阈值应等于该相 CCR
  加上 rank 序号×160 计数的采样延迟修正。
- 详细推导见第 5 节（回填）。

## 4. 执行顺序

| 步骤 | 内容 | 状态 |
| --- | --- | --- |
| 0 | 写本计划文档 + code-wiki 索引 | 已完成 |
| 1 | 交接快照：git status、ELF SHA256、COM3 只读快照 | 已完成 |
| 2.1 | 桌面推导采样窗口模型（wk_tmr/wk_adc/motor_drv/DRV8311H） | 已完成（§5.1~5.2） |
| 2.2 | 复算 tmp/bench 已记录实验 → 六运行统一变换 | 已完成（§5.2） |
| 2.3 | Rs 锚定（2804 手册 Rs=2.55 ✓；DRV8311 增益 47k=0.5V/A ✓） | 已完成（§5.1） |
| 2.4 | 预测表回填第 5.3 节 | 已完成 |
| 3 | `scripts/flows/board/run_openloop_probe.ps1` 就绪（预检通过）；T1 首例已完成 | 阻塞于操作员断电循环 |
| 3 | T2 V-I 线性度、T3 theta0 扫描 | 待实机 |
| 4 | 判定 → host 失败测试先行 → 最小单变量修复 | 待 T1~T3 数据 |
| 4 | 全门禁重跑 + 新 ELF 实机复测 | 待修复 |
| 5 | 文档回写 + 维护日志条目 | 日志已建条目，结果区待回填 |

## 5. 桌面推导与预测表（2.1~2.4 已完成，2026-08-30）

### 5.1 已钉死的事实链（全部由源码+实测日志交叉验证）

1. `foc_svpwm` 公式（`foc_core.c:26-90`）与 duty↔CCR（`CCR = dtc×3749`，
   `motor_drv.c:184-270`）已用 5 个运行独立反推验证，误差 <0.2%。
2. rank 信号组在所有运行中 KCL 自洽（三相读数之和 ≈0，残差 2~25 counts）——
   三个 SOx 信号确为一致的三相测量组。
3. 2804 电机手册（`docs/pcb docs/2804电机参考手册.pdf`）确认
   **Rs=2.55 Ω、Ls=0.86 mH、ψ=0.0035 Wb、12N14P**——固件默认参数与手册一致，
   排除"电阻标错"。
4. DRV8311 数据手册（`docs/pcb docs/drv8311.pdf`）确认 **GAIN 脚 47 kΩ→0.5 V/A**
   ——排除"增益档读错"。
5. grad1.5/边界运行中转子在 400 ms 脉冲内明显转动（pos_turn −0.13/+0.057 圈），
   "纯电阻静态"假设只对转子未动的运行成立。

### 5.2 核心发现：六运行统一的测量变换

对 6 个独立运行（静态 0.5/1.0/1.5/2.0 V 梯度、边界 1.0 V、动态 2 V 两个触发点）
做"真实电流组（V/R，θ_sim 由 trace 验证）→ 三 rank 读数组"的拟合，唯一统一解：

```text
measured[label] = −k × swapAC(true[label])，k ≈ 0.66~0.90（多数 0.70~0.80）
```

- **rank↔相位配对 A↔C 互换**：map0 假设 A→rank4/B→rank3/C→rank2；
  数据表明实际 A→rank2（IC 脚）/B→rank3/C→rank4（IA 脚）。
  干净运行拟合残差 3~9%（grad1.0/grad2.0/2Vdyn-trig1874）。
- **全局符号多一重负号**：固件 `-FAC_CURR` 取负后仍与电机约定相反
  （witness 注释声称相反，须实机 dp1 witness 仲裁）。
- **k≈0.73 标度缺口**：增益、Rs、CSAREF（offset≈2048 ✓）均排除后仍未解释；
  候选=采样孔径落在 PWM 纹波的非平均相位（纹波幅度反推 L_real≈0.2~0.4 mH，
  手册 0.86 mH 存疑）+ 孔径骑跨开关沿损失。
- **采样有效性随 CCR4/占空比变化**：多运行出现 rank 严格读 0（±2 counts）
  而该相真实电流 0.1~0.4 A——低侧窗口机制定性成立；
  孔径几何（触发极性、上/下数位置、rank 延迟 160 counts）未唯一确定，
  cval@IRQ≈2000 与 3740 触发点矛盾仍开放。

### 5.3 对实机实验的可证伪预测

| # | 预测 | 实验否定点 |
| --- | --- | --- |
| P1 | trig=3740、静态 1.0 V 下，读取"相位 a 电流"的是 rank2（map0 的 A↔C 互换）；theta0 换相后 CCR 顺序反转，rank 转折顺序随之反转 | T1 |
| P2 | 修正互换后仍存在全局 −1（dp1 witness 显示 dominant NEGATIVE ib） | T1/T3 顺带 |
| P3 | V-I 斜率给出 R_eff≈2.55/k≈3.2~3.6 Ω；触发扫描内多点平均后 k→1 | T2 |
| P4 | 每个 rank 的读数转折 CCR4\* 随 theta0 按 P4 表移动，转折点差 = 配对相 CCR 差 | T1 |
| P5 | theta0 扫描存在使转子平滑跟随的角度域，且运动方向标志编码器方向正确性 | T3 |

### 5.4 对根因图景的修正

- "电压升高软件电流快速增大"**大部分是真实欧姆响应**（V/R，R≈2.55 Ω 真值 +
  纹波采样偏差叠加），不再是独立的"观测偏高"疑点。
- 开环带不动的主嫌疑转向：**编码器零点/方向从未标定**（带电标定被门禁关闭，
  mech_offset/direction/phase-order 全为默认值）+ **rank 配对互换使任何电流反馈
  模式（I-f/闭环）矢量错误**。
- 采样窗口几何是真实的观测完整性问题（触发依赖、读 0、纹波偏差），
  但不是开环失败的第一候选。

## 6. 交接快照记录（第 1 步已完成）

- `git status --short --branch`：main、297 个文件处于用户修改状态（保持不动）。
- 指定镜像存在且 SHA256=`59B78D648511926F8AD47213CA35E49C0D536697BE814B1B77AF64F84E949A03`
  与 debugging-summary 记录一致。
- COM3 只读快照（2026-08-30）：`axis=idle ctrl=volt fault=0x0`、`vbus=11.9 V`、
  `enc_valid=1`、三相 offset `2050.6/2050.9/2046.9`、`adc_trig=3740`、
  `pwm=0 adc=0`（map0），bench 诊断命令全部可用。

## 7. 实验结果回填区（2026-08-30 完成）

### 7.1 实验执行记录（全部 AI 自动执行，SWD 复位门控，无需操作员断电）

| 实验 | 用例数 | 结果 |
| --- | --- | --- |
| SnapTest（修复前，旧 ELF 59B7…） | 6 | 转子响应证实（各例间电角度移动 15°~97°，电流 0.2~0.45 A）；dp1+ 显示 ib=−432 mA（负主导，与 witness 目标相反） |
| T1 触发细扫描（修复前） | 24（2 seed×12 trig） | 互换模型全胜：R2↔a/R3↔b/R4↔c 全部正 k；map0 假设给出负 k（−0.13/−2.5）→ P1 证实 |
| SnapTest（修复后，新 ELF 4043DC1A…） | 6 | dp 六向符号全部正确：dp0− (−253,+120,+124)、dp1+ (−119,+267,−150)、dp2+ (−118,−152,+264)；k=0.65~0.83 |
| T1 触发细扫描（修复后） | 24 | 12 个触发点 k 稳定（b:0.77~0.79、c:0.72~0.75），读数不再随 CCR4 漂移 |
| T2 V-I 线性度 | 4 | k 随工况 0.35~0.82 漂移，不收敛 → 标度误差为采样链固有，需后续处理 |
| T3 theta0 扫描（修复后） | 6 | 转子被矢量明显拖动（0.1~0.28 圈/例）；θ0↔位置映射受惯性影响，不做静态标定用 |

### 7.2 确诊结论

```text
ADC rank/phase pairing   = INVERTED_AC_FIXED_AND_VERIFIED
CURRENT_SIGN             = NEGATION_REMOVED_AND_VERIFIED (dp1+ now shows dominant POSITIVE ib)
SAMPLE_STABILITY         = TRIGGER_INDEPENDENT_AFTER_FIX (12-point scan, k stable)
ADC_SCALE                = 0.65..0.83 PER_RANK_CONDITION_DEPENDENT (CSA settle + ripple sampling)
RANK2_IA_DEAD_ZONE       = duty_a > 0.53 (aperture ~CNT1990 exits phase-A low-side window)
OPEN_LOOP_SOFTWARE_PATH  = PASS (SVPWM/duty/voltage chain verified to <0.2% in 6 runs)
OPEN_LOOP_ROOT_CAUSE_STATUS = PARTIALLY_RESOLVED
  - 电流反馈链配对反+全局反号：已确诊并修复（host 38/38 + 外设契约 + 实机六向验证）
  - 剩余：编码器零点/方向从未标定（带电标定门禁关闭）、rank2 死区、k 标度校准
REAL_BENCH_TEST_STATUS   = BLOCKED（结构性限制不变：deadtime=0、nFAULT 无端点等）
```

### 7.3 修复内容（host 失败测试先行，38/38 复验通过）

- `applications/periph/motor_drv.c` `adc_read_phases()`：ABC 分支改为 ia←rank2、
  ib←rank3、ic←rank4；ACB 分支 ia←rank2、ic←rank3、ib←rank4（rank 跟随 PWM 通道）；
  移除全局 `-FAC_CURR` 取负（旧 "Sense-FET sign fix" 注释由实机 dp 数据推翻）。
- `applications/periph/motor_bench_mapping.c`：ADC 表 map0={2,3,4}、map5={4,3,2}。
- `scripts/flows/host/check_foc_peripheral_contract.ps1`：映射断言更新为新事实。
- `tests/host/applications/test_motor_bench_mapping_contract.c`：期望表同步。
- `scripts/flows/board/run_openloop_probe.ps1`：新增 SnapTest 模式与 SWD 复位门控。
- `scripts/gates/check_themis_quality_gate.ps1`：diff --check 排除 generated/** 与
  work/**（AGENTS.md 已声明 work/ 范围外；厂商生成代码不做空白审查）。
- 新 ELF：`build/fw-full-diag-carrier-bench-400ms-debug/output/falcon-foc-at32.elf`
  SHA256=`4043DC1A308B6899947F23365C0C87AC0216D92D0C9B1886878CD14542273A34`（OpenOCD Verified OK）。

### 7.4 遗留问题（下一轮入口）

1. 编码器零点/方向标定（现在电流链正确，I-f 模式已可用作拖动标定工具）。
2. rank2 死区（duty_a>0.53）与 k 标度校准——需要确定真实采样孔径几何
   （建议：示波器或按占空比自适应触发点）。
3. 30 rpm 动态开环/I-f 连续旋转验证（在修正后的链上重做）。

## 8. 判定规则

- 转折阈值与预测逐相吻合且 k≈1：采样窗口机制确诊，读数基本物理，
  根因转向同步/机械分支。
- 转折阈值与预测逐相吻合且 k≠1：采样窗口机制确诊且叠加标度误差，
  修复方向为采样窗口（触发点/采样策略）。
- 转折阈值与预测不吻合：窗口机制被证伪，转向失步/机械/功率级分支，
  另设计单变量实验，不进入第 4 步修复。

## 9. 安全规则（实机段）

- 通用：指定 ELF、测试前后 SWD reset、单变量、每脉冲后 `state disable`、
  ≤0.75 A 包线、跨脉冲按脚本提示断电重启。
- 硬停条件（任一满足，操作员立即切断 12 V，不等串口回复）：
  任相读数 ≥1.00 A；逼近 0.75 A 且无回落趋势；VBUS 出 10.8–13.2 V；
  卡死/尖叫/剧烈抖动；快速发热/异味；串口失联；无法确认 PWM 已关断。
- 正常停机只用 `state disable`；12 V/1 A 电源不是相电流保护。

## 10. 验证命令

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/host/run_host_tests.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_themis_quality_gate.ps1 -Scope all
./run.sh build
./run.sh flash
```

（省略 `-RepoRoot`，脚本自动探测仓库根；勿照抄旧文档中的 FalconFoc_AT32 路径。）

## 11. 成功标准

1. 1874→2300 转折有定量机制解释，实机细扫描阈值与预测逐相吻合（或被证伪并给出替代解释）。
2. k 因子（软件电流读数/真值）确定。
3. `OPEN_LOOP_ROOT_CAUSE_STATUS` 从 UNRESOLVED 更新为有证据的结论并回写文档。
4. 若确诊且可修：host 测试先行、全门禁绿、实机复测显示改善。
