# 空心杯云台 FOC 项目 — AI 交接文档（2026-08-31 深夜版）

> **[2026-09-26] 本文已转为历史文档。现役交接入口是 `AI-handoff-2026-09-26.md`**
> **（根因已确认：AVDD 供电压垮，本文的"±90°/磁偏心"等剩余问题假设已被取代）。**
>
> 本文是给下一位 AI 的完整交接：项目、软件、硬件、调试历程、方法论、剩余问题。
> 取代 `ai-handoff-prompt.md`（2026-08-30 版）作为唯一交接入口；旧文保留作历史。
> 证据文件：`tmp/bench/*`（各轮实测日志）、`docs/planning/Themis_开环根因延申验证与修复计划.md`、
> `docs/governance/Themis_工程维护日志.md`（全程编年记录）。

---

## 1. 项目与任务概述

- **对象**：单轴云台 FOC 固件（FalconFoc 架构衍生），MCU AT32F415KBU7（QFN32），
  驱动 DRV8311HRRWR（硬件 strap 版），编码器 KTH7823（16-bit 磁绝对，SPI1），
  电机 2804 云台（12N14P，7 对极，手册 0.5A 连续/2A 峰值）。
- **历史问题**：低压电压开环卡死/失步，软件观测电流异常，无法定位根因。
- **本轮成果**（详见第 5 节编年史）：确诊并修复电流采样链 A↔C 配对反+全局符号反；
  确认原理图门极网络 A/C 整体对调；实现 I-f→闭环在线切换；双向连续旋转 9+ 圈；
  编码器零偏 dp 对齐标定法建立；定位"位置依赖零力矩"为最后剩余问题。
- **工作纪律**（必须继承）：脏工作树不可清理；保护文件（motor_drv.c / foc_ctrl.c /
  foc_config.h / 保护阈值 / 电机参数）修改须先有诊断数据 + host 测试先行；
  每轮变更记入维护日志；实机测试遵守安全规则（第 7 节）。

---

## 2. 硬件理解（全部经实测/手册/网表三方验证）

### 2.1 芯片与引脚（数据手册 p27 QFN32 引脚表 + 厂商 gpio.h 注释）

| TMR1 通道 | QFN32 引脚 | 原理图网络名（**注意：与通道对错位**） |
|---|---|---|
| CH3 | PA10 = pin 20 | 网络 INHC |
| CH3C | PB1 = pin 15 | 网络 INLC |
| CH2 | PA9 = pin 19 | 网络 INHB |
| CH2C | PB0 = pin 14 | 网络 INLB |
| CH1 | PA8 = pin 18 | 网络 INHA |
| CH1C | PA7 = pin 13 | 网络 INLA |

- **关键事实（原理图 A/C 整体对调）**：固件按通道对命名（INHA/INLA = CH3/CH3C 对），
  但原理图把 CH1 对（PA8/PA7）接到了 DRV 的 INHA/INLA、CH3 对（PA10/PB1）接到
  INHC/INLC。即**固件 A 相门极对实际驱动 DRV 物理相 C**（B 相正确）。因高低侧
  一致对调，每半桥仍收到完整互补对——无直通风险，等效为固件 A/C 标签互换。
- **决策**：不改硬件、不做驱动侧再对调（会破坏已验证的自洽性）；以编码器标定吸收。
  `at32f415_wk_config.h` 门极定义处有警示注释；未来改版须修正网络名并把
  ADC map 切回 {4,3,2}（`diag map adc 5`）。
- ADC 采样：注入组 4 rank（rank1=VBUS/PA1，rank2=IC/PA4，rank3=IB/PA5，rank4=IA/PA6），
  采样 7.5 周期 @18.75MHz（每通道 ~1.07µs = 160 TMR 计数）。DRV8311H 低侧
  sense-FET CSA：**仅在低侧导通时读数有效**；SOA→rank4、SOB→rank3、SOC→rank2。
- **采样孔径之谜（未解，重要）**：实测有效采样孔径位于固定计数器位置
  （rank2≈CNT1990、rank3/4 更靠后），**与 CCR4 触发点基本解耦**（900..3100 扫描
  读数几乎不变）。rank2 存在死区：**duty_a > 0.53 时 ia 采样失效（读≈0）**。
  修复此几何 = 丝滑旋转的关键工程项之一。

### 2.2 DRV8311H 与功率级

- 硬件 strap：6x PWM、GAIN=47kΩ→**0.5V/A**（数据手册确认）、内部 OCP ~5A、
  内部死区插入 ~450ns（TMR1 deadtime=0，靠 DRV 握手）。nFAULT/nSLEEP 无 MCU 端点。
- CSA 线性范围 ±2.8A（0.5V/A、CSAREF 3.3V）。
- **真实绕组电阻 ≈3.5Ω**（锁定跟踪工况 vq/|I| 实测 3.37Ω + dp 静态拟合 3.4-3.9Ω）——
  **手册 2.55Ω 对本电机变体不符**。由此修复后采样链读数 = 真实电流
  （早前"k=0.7 偏小"是电阻比假象）。

### 2.3 电机与编码器

- 2804 云台：12N14P（**pp=7 已实测确认**：dp 120° 电角步进 = 编码器 120° 电角响应），
  手册 0.5A 连续/2A 峰值/300g·cm。**齿槽+摩擦阻力矩显著**（实测 stick-slip：
  0.5-1.2A 拖动均为同步转速的 64-76%，位置依赖停滞多点复现）。
- **用户手转 47 圈全程顺畅**（编码器 2302 样本全程跟踪）——机械自由，无卡点。
- 编码器 KTH7823：SPI1（PB3/PB4/PB5，CS=PA15），16-bit 绝对式，DIR_CCW 为
  出厂默认（**计数镜像态**）！`enc_dir 0`=CW=原始计数。方向翻转同时翻转
  速度反馈符号与电角度方向（闭环植物符号随之修正）——见 5.6。
- 电源：12V 适配器。**1A 限流会在拖动瞬态折叠（母线塌至 2.69V → 欠压故障），
  外观酷似机械卡点——必须设 2A 以上**。这是本次调试最大的"假象"之一。

### 2.4 电流保护族（2026-08-31 调整，序次必须保持）

```text
命令上限 DEFAULT_CURR_RATED = 1.2A（测量值 = 真实值）
任务级持续 FOC_WARN_CURRENT_A = 1.5A / 3s
闭环堵转 FOC_STALL_CURRENT_A = 1.5A（非电压模式 + <5rpm + 200ms）
快速 OCP FOC_FAST_CURRENT_TRIP_A = 1.6A（立即）
手册：0.5A 连续 / 2A 峰值 —— 1.2-1.6A 为短脉冲合法区间
```
序次约束：命令 < 持续 < 快速，否则保护规则互相击穿（曾出现 1.8>1.6 死规则）。

---

## 3. 软件理解

### 3.1 架构与控制链

```text
TMR1 中心对齐 20kHz（ARR=3749）→ CH4 触发 ADC 注入组
→ ADC1_IRQHandler: adc_read_phases() → foc_fault_check_fast()（O(1)）
→ 分频（产品 2 / bench 4）→ foc_current_loop_step() @10k(产品)/5k(bench)
   · CTRL_MODE_VOLT：theta_sim 积分 → foc_ctrl_volt（纯电压，旁路电流环）
     · I-f 子模式（if_enable=1）：电流环按 theta_sim 调节 iq=if_iq_target
   · 其它模式：Clarke/Park（theta_elec）→ id/iq PID → 解耦 → SVPWM
→ TMR5 软中断 5kHz：encoder_update（阻塞 SPI）→ foc_outer_loop_step（速度/位置 PID）
```

- **关键修正后的电流读取**（`motor_drv.c adc_read_phases`）：
  ia←rank2(SOC)、ib←rank3、ic←rank4(SOA)，**无全局取负**
  （旧代码 ia←rank4/ic←rank2 + `-FAC_CURR` 双重错误，本轮修复）。
- **I-f→闭环在线切换**（本轮新增，`foc_cmd_if_to_vel_handoff`）：
  前置校验（RUNNING+VOLT+I-f）；无扰交接 = 速度 PID 积分种子 = I-f 力矩；
  **必须写 target_vel**（passthrough 输入任务每拍把 target_vel 拷入 vel_set，
  只写 vel_set 会被陈旧 0 目标钉死——实测教训）；dyn_iq_limit = iq_max。
  shell 命令 `diag if2vel <rpm>`。
- **保护族**见 2.4；**速度环植物符号**：iq>0 → 机械负方向 → 编码器速度负
  （CCW 帧）—— enc_dir 切换会同时翻转该符号（见 5.6）。

### 3.2 Shell 命令清单（bench 固件）

| 命令 | 作用 | 备注 |
|---|---|---|
| `sys status/tasks/reboot` | 系统状态 | |
| `state show/start/disable` | 状态机 | **stop 会进 fault，正常停用 disable；fault 态恢复=断电或 SWD 复位** |
| `fault status/clear` | 故障 | clear 在纪律禁用清单 |
| `obs all/enc/...` | 观测（含 elec_rad/raw） | `obs enc` 供手转扫描 |
| `diag set vq <V> <we>` | 电压开环（we=电角速度 rad/s） | **纯电压，Mecco 式，不碰电流采样** |
| `diag voltloop <rpm> <V>` | **电压开环按输出轴 rpm 下令**（2026-09-26 新增） | 内部换算 we=电角速度；回执含 `we_elec`；idle-only |
| `diag set if <A> <we>` | I-f 开环（电流环调节） | 依赖电流链质量 |
| `diag set iq <A> <acc>` | 电流模式力矩（theta_elec 帧） | 力矩-角度扫描工具 |
| `diag if2vel <rpm>` | **I-f→闭环在线切换**（本轮新增） | 仅 RUNNING+VOLT+I-f |
| `diag set dp0/1/2 <V>` | 单相定向矢量 | **dp 对齐标定工具**（≥3.2V 破齿槽） |
| `diag param encoff <电弧度>` | 编码器零偏微调 | `unit=raw` 回执标签是误导，语义=电弧度/pp |
| `diag set enc_dir 0/1` | 编码器方向 | **0=CW=原始计数；默认 1=CCW=镜像** |
| `diag set adc_trig <n>` | CH4 触发点 | 实测与实际采样时刻基本解耦（谜） |
| `diag map show/reset/...` | PWM/ADC 映射 | bench 专用 |
| `diag trace / enc trace` | 64/32 条 RAM 环形轨迹 | **show 打印的是 min/max，非首末！** |
| `cfg ctrl <volt/curr/vel/pos/vvel>` | 控制模式 | idle-only；vvel=电压模式速度闭环（速度 PID 直接出 Uq，绕过电流环，备胎对照模式，2026-09-26 新增，`diag pid set vvel ...` 可调，持久化层故意不存 vvel） |
| `store status/save/load/erase` | 参数持久化 | save 延迟提交 |
| `cal status/...` | 校准 | 带电校准被门禁禁用 |
| `diag pid ... show` | PID 增益 | vel kp=2/ki=0.5；pos kp=50 |

### 3.3 构建 / 烧录 / 测试 / 脚本

- host 测试：`powershell -File scripts/flows/host/run_host_tests.ps1`（**39/39**，
  含本轮新增 `test_if_to_vel_handoff_contract`）；质量门禁 `scripts/gates/check_themis_quality_gate.ps1 -Scope all`
  （8/9 过，`test_falconfoc_themis_lite_contract` 待 403a→415 迁移，见维护日志）。
- bench 预算档：`fw-full-diag-carrier-bench-{50,200,400,1000,30s}-ms-debug`
  （30s 为本轮新增耐久档；build preset 同名）。
- **多用途实测脚本**：`scripts/flows/board/run_openloop_probe.ps1`
  模式：SnapTest（dp 六向）/ IfDrag（I-f 拖动）/ VoltDrag（纯电压）/ Align
  （dp 对齐标定，通电读数）/ Handoff（在线切换）/ PosClose / TorqueSweep
  （力矩-角度扫描）/ TriggerScan / VILinearity / ThetaSweep / **SpeedStep**（仅
  bench30s：I-f 起转后双向 3×30/60/90 rpm 闭环短时阶跃）。
  `-AutoGate` SWD 复位门控（无需断电循环）；`-MaxPeakAmps` 包线守卫；
  `-ElfPath` 指定镜像。分析器：`scripts/flows/board/analyze_openloop_probe.py`。
- 烧录：OpenOCD + CMSIS-DAP（`tools/openocd/openocd_daplink.cfg`），
  `program <elf> verify`；**适配器速度已在 cfg 内，勿传 50kHz 参数**。
- 串口：USART1 PB6/PB7，**115200 8N1**，COM3。**用户终端占用 COM3 会
  导致访问被拒——先关用户的串口程序**。
- 当前板上固件：bench-30s `879a6b3b…`系（含在线切换+1.2/1.5/1.6 保护族
  为 `c0f1eeac…` 后又含 encoff/enc_dir 试验变更，以 build 目录为准）。

---

## 4. 调试历程编年史（含数据与结论）

### 4.1 起点（前 AI 交接状态）
电压开环卡死；观测电流异常；ADC 读数随触发位置变化（1874→2300 转折）；
根因未定；39/38 测试。

### 4.2 电流采样链确诊与修复（本轮最大成果）
- **方法**：6 个历史运行做"真实电流组（theta_sim/vq/vbus 反推，SVPWM 链
  逐档验证 <0.2%）→ 三 rank 读数组"拟合，唯一统一变换
  `measured = −k × swapAC(true)`，k≈0.66-0.90。
- **确诊**：rank/phase 配对 A↔C 反 + 全局符号反。**修复**：ia←rank2、
  ib←rank3、ic←rank4、去负号。实机 dp 六向验证（dp1+ 显示正 ib，
  旧 witness 首次真正成立）。host 39/39。
- **教训**：旧 witness（"dp1 须正 ib"）在双重错误下自洽——**witness 必须
  在修复后重验**。

### 4.3 原理图核查（用户怀疑 → 三方验证确认）
数据手册 QFN32 表（PA7=13/PB0=14/PB1=15/PA8=18/PA9=19/PA10=20）+ 厂商
GMUX_0001 注释 + 网表：**INLA/INLC、INHA/INHC 整体对调**（B 相正确）。
高/低侧一致对调 → 无直通 → 等效标签互换。

### 4.4 旋转达成与电源教训
- I-f 0.5A@we=22：**−8.96 圈/28s 连续**（首次！）；0.71A：−8.99。
- **电源 1A 限流在拖动瞬态折叠（母线 2.69V → 欠压故障）**，外观与机械
  卡点难以区分（多个用例停在 +0.164 圈的"假卡点"）。**电源必须 ≥2A**。
- 该假象一度导致错误的"机械卡点"假设——**欠压塌压要第一时间查电源限流**。

### 4.5 编码器标定（dp 对齐法）与两个陷阱
- **方法**：dp 定向强矢量（**≥3.2V，2V 破不了齿槽**）通电 1.2s → 通电期间
  `obs enc` 读电角度（**断电后读数会回弹，不可用**）→ 三向读数与场角差
  的均值 = 零偏误差。三向一致性 = 标定质量判据。
- **陷阱 1（齿槽污染）**：2V 对齐时转子停在齿槽谷，三向读数散布 ±1.4 rad
  电角 → 得出错误零偏 2.535/2.181（真值≈0，见 4.6 力矩扫描反推）。
- **陷阱 2（enc_dir 默认）**：出厂默认 DIR_CCW（镜像）——`enc_dir 1`
  等于没改；`enc_dir 0` 才是真正翻转。镜像同时翻转闭环植物符号。
- 用户手转 47 圈 → 编码器全程健康、机械自由（机械卡点假设第二次撤回）。

### 4.6 力矩-角度扫描（决定性方法，反转所有结论）
CTRL_MODE_CURR + encoff 偏置扫描（0/±45/±90/135/180°，iq 0.45A/2.5s）：
- v=180°：**−0.994 圈连续旋转**（max 负力矩方向 = 自由方向）；
- v=±90°：零力矩（纯 d 轴）；v=0/45/135：齿槽级微动。
**结论**：真实零偏 ≈ **0（默认帧即正确！）**——dp 对齐标定被齿槽污染，
**所有 encoff 修正（2.535/2.181）都是错的，闭环堵转正是错误 encoff 把
力矩矢量转了 −125°~−145° 所致**。⟹ **闭环调试前必须删掉一切 encoff
修正（或设 0）**。

### 4.7 I-f→闭环在线切换（实现 + 两个实测教训）
- 实现：无扰（i_out 种子）+ target_vel 所有权 + 三重前置校验 + host 契约。
- **教训 1**：只写 vel_set 不写 target_vel → passthrough 任务用陈旧 0
  覆盖 → 0 rpm 保持假象。
- **教训 2**：脚本补丁用正则插入时 `[^}]*` 会被函数体内的 `{return;}`
  截断 → 把 stub 插进函数体中间（语法灾难）——**插入后必须整文件编译验证**。
- **教训 3**：命令顺序——`cfg ctrl vel`/`diag set vel` 必须在 `state start`
  之前（idle_only），放在 start 后会被拒（整轮测试作废，日志可见
  `result=error axis=running`）。

### 4.8 抖动（抽动）机理与"丝滑"差距
三层叠加：①ia 采样死区（duty_a>0.53 每电周期失明 → 3.5Hz 力矩抽动）；
②齿槽 stick-slip（0.5/0.71/1.2A 均收敛到同步的 64-76%）；③I-f 无阻尼
hunting（速度 0↔±60 rpm，周期 1.5-2s，轮询时间线实测）。**Mecco 参考
（同代码族）丝滑的原因：其电机为真空杯（无铁槽、齿槽≈0、惯量小）**——
同一代码在不同电机上平滑度不同是物理差异；本机要丝滑需闭环（速度环自带
阻尼）或采样修复后的大电流同步。

### 4.9 位置依赖零力矩（当前最后剩余问题）
- 现象：特定转子角度（0.085/0.44/0.48/1.015 圈等）下，闭环/静态电流矢量
  0.8-1.6A 推不动转子（iq runaway 至 −1.6A 持续 14s 转子冻结）；同电流
  在其它位置正常；手转 47 圈自由。
- 已排除：机械卡点（手转+47 圈）、电源（2A 后无塌压）、电流环（<1% 误差）、
  采样配对/符号（dp 验证）。
- 剩余候选：①编码器磁铁偏心/缺陷 → 该角度 theta_elec 失真 ±90°（矢量落
  d 轴）；②deadtime=0 + DRV 握手在该相位组合的输出异常。
- 鉴别手段：示波器（INHx vs SOx 相位）或磁铁间隙检查；软件侧可做
  力矩-角度精扫描（小电流 ±90° 细步）与 enc LUT 标定。

---

## 5. 方法论（什么管用、什么坑人）

**管用**：
1. 拟合反推（用已验证的电压链反推真实电流组，与测量组做变换拟合）——
   一次定位配对/符号；
2. 力矩-角度扫描（CTRL_MODE_CURR + encoff 偏置步进 + 位移读数）——
   一次反转所有标定结论；
3. dp 对齐通电读数（≥3.2V、通电期间读）；
4. 运行中轮询时间线（0.7s 间隔 state show）——抽动/失步指纹；
5. 手转扫描（`obs enc` 高频轮询 + 用户手转）——机械/编码器健康判据；
6. 单变量 + SWD 复位门控 + 每用例日志（`run_openloop_probe.ps1` 模式）。
7. 三方交叉核验（数据手册引脚表 + 厂商 GMUX 注释 + PCB 网表）——在不改硬件的
   前提下确认 A/C 门极标签整体对调，并排除单相低侧误接的直通风险；
8. 同一用例改变供电包线（12V/1A 与 12V/≥2A）并记录 VBUS——快速把电源限流折叠
   与真实机械/控制卡滞分开；
9. 使用明确的 bench 预算档、`-ElfPath` 和镜像哈希——保证一次串口/实板结论只归属
   于实际刷入的固件，避免把历史 trace 与新功能混用；
10. 交接前用“命令所有者 → 运行态写入点 → 下一任务拍”追踪控制量——例如 I-f→速度
    切换必须同时更新 `target_vel`，才能避免任务把只写入的 `vel_set` 覆盖回陈旧目标。

**坑人**：
1. 电源限流折叠伪装成机械卡点（先查电源电流表！）；
2. `diag trace show` 打印 min/max 而非首末——方向推断不可用；
3. trace raw 顺序 = (vbus, ic, ib, ia) 标签序而非 rank 序；
4. `encoff` 回执 `unit=raw` 是误导（实际电弧度语义）；
5. `enc_dir` 默认 CCW（1=没改，0=翻转）；
6. allowlist 单引号阻止 `$number` 插值；正则插入 stub 会被函数体 `{}` 截断；
7. `$number` 类匹配需双引号；`diag set` 全家 idle-only（状态机 fault 态
   一律拒绝——恢复=断电/SWD 复位）；
8. 断电后读编码器=回弹位置，标定必须通电读；
9. PowerShell 5.1：native stderr + EAP=Stop = 终止错误（OpenOCD 横幅、
   cmakelang 探测都踩过）；`& script.ps1` 同进程调用使 `$LASTEXITCODE`
   残留子内原生命令退出码（门禁已改独立进程调用）；
10. pdftotext 的表格行错位——引脚表必须看渲染图。

---

## 6. 当前状态与剩余问题（优先级序）

```text
CURRENT_FEEDBACK_CHAIN   = STATIC_LOW_DUTY_VERIFIED_DYNAMIC_HIGH_DUTY_INVALID
MOTOR_ROTATION           = DEMONSTRATED_9TURNS_28S_BIDIRECTIONAL
ENCODER_ZERO_CALIB       = METHOD_VERIFIED_VALUE_UNCERTAIN(齿槽污染)
IF2VEL_HANDOFF           = IMPLEMENTED_HOST39_39_BOARD_EXECUTED
JERK                     = TRIPLE_MECHANISM_IDENTIFIED
POSITION_ZERO_TORQUE     = OPEN(编码器磁路 or 功率级, 需示波器/物理检查)
PROTECTION_FAMILY        = 1.2(cmd) < 1.5(sustained/stall) < 1.6(fast)
TESTS                    = HOST 45/45 + 外设契约 PASS
REAL_BENCH_TEST_STATUS   = BLOCKED（结构限制不变：deadtime=0、nFAULT 无端点）
```

### 6.1 2026-09-03 原始 PWM 隔离增量

- `[实板测量]` 固定 duty `0.40/0.50/0.60` 的 A/B/C 相电压为 `7.18/6.09/5.15 V`，
  PSU `0.08 A`；随后 1 Hz 电角 raw-sixstep 连续 15 s，编码器从 `0.020` 单调到
  `2.056` 圈。这证明基础 PWM 映射、三相桥、相线和低速六步换相可工作。
- `[实板测量]` 10% raw-sine（绕过 FOC/SVPWM、电流环和编码器反馈）20 s 内由 `0.448`
  前进至约 `1.40` 圈后停在 `1.392` 附近。它的空间矢量较 raw-sixstep 小约 13%，因此不作
  功率级或控制路径结论；ELF/日志为
  `0B59675E67E3128981BEAA52BCF58AA7CFDC51CAEF8E92F3ED036618EDDA978F` /
  `tmp/bench/20260903_012942_raw_sine/`。
- `[实板测量]` 单变量提高 raw-sine 振幅至 `0.11547005`（与 raw-sixstep 空间矢量严格
  相等）后，20 s 无 fault、VBUS `11.89..11.91 V`、报告相电流峰值 `0.374 A`，位置每个
  样本单调 `0.411 -> 2.564` 圈（约 6.46 rpm）。这证明连续正弦 raw PWM 本身能连续推动
  转子，10% 试验停滞与力矩裕量不足一致；没有证据表明 `foc_svpwm()` 是根因。ELF SHA-256 为
  `9EF838F8C4937464056E705BDD97EFA5AE997F4D64595F937285092CC9FF5235`，日志在
  `tmp/bench/20260903_013956_raw_sine_match_sixstep/`。
- `[实板测量]` 再将 raw-sine 振幅提高至 `0.20` 后，20 s 无 fault、VBUS
  `11.85..11.93 V`，位置仍单调 `0.001 -> 2.209` 圈（约 6.62 rpm）。但多个动态
  duty 相位的串口相电流不满足 KCL，例如 `-0.006/-0.608/+0.003 A`（和 `-0.610 A`）与
  `-0.378/-0.321/+0.003 A`（和 `-0.696 A`）；最大报告绝对值为 `0.673 A`。ELF SHA-256
  `D32207C916D6C6BAC7D5DAE1511F3574D82DFE03BC30DA3ECB712B0B42D6A173`，完整记录在
  `tmp/bench/20260903_015058_raw_sine_20/`。
- `[结论边界]` 该结果不验证动态 ADC 高 duty 采样、闭环电流控制或 30/60/90 rpm；它不能
  用来改保护、相序或编码器 offset。20% 结果反而直接证明：物理旋转仍在持续时，动态
  ADC/CSA 重构可以相位相关地失效；速度门禁仍停在动态采样波形证据之前。

**剩余问题（按优先级）**：
1. 动态 ADC/CSA 采样孔径：高占空比时会失去 KCL 闭合，即使原始正弦仍在持续转动。
   必须用示波器在不闭合 duty 相位同时测 CH4、PWM/互补门极、SOA/SOB/SOC 与真实相电流；
2. 位置依赖零力矩（~0.085/0.44/0.48 圈等）——在第 1 项分流后才检查磁铁间隙；
   软件侧备选：enc LUT 标定（`foc_calib_enc` LUT 段）；
3. 正交角精标定（力矩-角度细扫描，小电流 ±90° 细步）；
4. 闭环速度环调试（前 3 项完成后）——目标丝滑 30 rpm；
5. `test_falconfoc_themis_lite_contract` 的 403a→415 迁移（门禁 8/9）。

## 7. 实机安全规则（继承并强化）

- 电源限流 **≥2A**（1A 会折叠伪装成卡点）；异常先断 12V 不等串口；
- 正常停机 `state disable`（进 fault 态是设计行为）；恢复=断电/SWD 复位；
- 硬停条件：任相 ≥ 快速 OCP、逼近 1.5A 无回落、VBUS 出 10.8-13.2V、
  卡死/尖叫/抖动异常、发热异味、串口失联；
- 禁止：`cal motor`、`cal enc`、`state stop`、`fault clear`（bench 纪律）、
  带电改参数（所有 diag set idle-only 是故意的）；
- 包线：命令电流 ≤1.2A（保护族 1.5/1.6 兜底）；时长 ≤30s（预算档自动停）。

## 8. 文件索引（本轮全部产出）

| 文件 | 内容 |
|---|---|
| `docs/planning/Themis_开环根因延申验证与修复计划.md` | 本轮计划+推导+预测表+结果回填 |
| `docs/code-wiki/AI-handoff-2026-08-31.md` | **本文** |
| `docs/code-wiki/debugging-summary.md` | 调试总结（含本轮全部追加条目） |
| `docs/governance/Themis_工程维护日志.md` | 编年日志（8 条本轮条目） |
| `scripts/flows/board/run_openloop_probe.ps1` | 多模式实测脚本（SWD 门控） |
| `scripts/flows/board/analyze_openloop_probe.py` | 日志分析器（标签语义） |
| `scripts/flows/board/run_bench_matrix.ps1` | 旧矩阵脚本（参考） |
| `tests/host/applications/test_if_to_vel_handoff_contract.c` | 交接契约测试 |
| `tests/script/test_speed_step_probe_contract.ps1` | SpeedStep runner 语法/安全字段契约 |
| `tmp/bench/*_openloop_probe_*` | 全部实测原始日志 |
| `tmp/handscan*.log` | 手转编码器扫描数据 |

## 9. 给下一位 AI 的三句话

1. **基础 PWM、raw-sixstep 与匹配幅值的连续 raw-sine 都能转动**——不要再把
   `foc_svpwm()` 作为唯一首要嫌疑；20% raw-sine 也持续转动。
2. **动态 ADC/CSA 是当前第一阻塞点**——20% raw-sine 在持续物理旋转时仍出现最高约
   `-0.699 A` 的 KCL 残差；在波形实测前，串口电流不能用于重调电流环或放宽保护。
3. **闭环 30 rpm 尚不具备再次尝试条件**——下一步是示波器同时测 CH4、PWM/门极、SOx、
   真实相电流、DRV_3V3、CSAREF、nSLEEP；之后才分流编码器磁路和功率级。

---

## 10. 90 rpm SpeedStep lane（源码完成，尚未带电执行）

`diag velstep <rpm>` 是 `TM_BENCH_DIAG=1` 专用命令。它只在 I-f→速度交接已经
完成后接受 RUNNING/VEL/PASSTHROUGH 状态的 ±90 rpm 请求，并且只更新
`target_vel`；速度 PID 积分、输出和 `dyn_iq_limit` 不会被重置。回执始终包含
`result`、`reason`、`vel_rpm` 与 `unit=rpm`。

执行入口：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/board/run_openloop_probe.ps1 `
  -Mode SpeedStep -RunPulses -RepoRoot D:/study/Software/Hollow-Gimbal -PortName COM3 -BaudRate 115200
```

它从 0.50 A、`we=±4 rad/s` I-f 起转 2.5 s，交接至对应 ±30 rpm，按 30/60/90 rpm
每档 5 s、200 ms 采样；负/正方向各 3 轮。产物在
`tmp/bench/<timestamp>_adc_then_speed/`，包括 ADC 冷启动基线、低能量相位证明、预检/ELF 哈希、逐样本 CSV、
分级汇总 CSV、人工电源表表格和 `result.md`。软件 ≥1.4 A 或 VBUS 越出
10.8–13.2 V 即安全失败；异常后禁用 `fault clear` / `state stop`，只能 SWD/物理断电。

`[未验证]` 截至本文更新，该 runner 尚未连接实板；不能把“机制已实现”写成
90 rpm 已通过，且没有示波器时失败仍不能归因到编码器磁路或功率级。

## 11. 2026-08-31 ADC 零点链修复与首个冷启动证据

`[源码确认]` 启动零点标定曾在 `pwm_out_disable()` 后采样。该状态使 DRV8311H
低侧 SenseFET/CSA 输出失去有效条件，而 TMR1-CH4 触发仍在运行，能够稳定地产生
错误但未到轨的 offset。现改为：全关 5 ms 放电 → 零占空比低侧钳位 → 100 ms
稳定 → 2000 个新鲜 injected ADC 样本。每路必须在 1900–2200 counts、标准差 ≤16、
三路跨度 ≤64；停帧直接标定失败。`diag adc show|zero`、`obs all residual` 和
`diag enc setabs <dir> <offset>` 均仅在 bench 诊断镜像中提供。

`[无板验证]` 39/39 host tests、FOC peripheral contract、SpeedStep script contract
均通过；30 s bench ELF SHA-256 为
`80A1D1C8C338448A492472BF988FD423FA94E48398270EFEB20B6E24F25EDBD0`。

`[实板测量]` 首个冷启动日志
`tmp/bench/20260831_232722_openloop_probe_AdcBaseline/preflight.log`：VBUS=11.92 V，
raw=2047/2052/2044，offset=2047.7/2052/2048.5，残差最大 0.007 A、三相和 −0.002 A；
它确认此前虚构的多安培软件电流来自无效 CSA 状态下的零点标定，而非本次 ADC
原始值直接显示硬件失效。

`[未验证]` 还缺 4 次实际 12 V 断电冷启动、六个 0.20 V `AdcProof` 脉冲和其后的
双向 SpeedStep；在这些门禁全部通过前，不得启动 30/60/90 rpm。

## 12. 2026-09-01 自动 ADC→SpeedStep 实板结果（当前入口）

`[源码确认]` bench 镜像新增运行中串口心跳：`diag bench keepalive|status`，1.5 s
未续期会关闭 PWM 并锁存安全故障；`state start` 要求 2000/2000 新鲜有效 ADC 校准，
`diag adc zero` 仅允许 idle、无 fault。自动 runner 仅用 SWD reset，禁止 `fault clear`、
`state stop` 与物理断电门控。

`[实板测量]` ELF `C4063EFD1652EC52353FAC55DB13BAD78E4451A3D56D2815F7B48915CBD36A20`
已烧录 verify。`tmp/bench/20260901_083911_adc_then_speed/` 完成 20 次 SWD 启动基线、
20 次空闲复零、六个 0.20 V proof、6 map × 8 trigger × 2 theta × 六向量及位置扫描；
已自动恢复 map0/CCR4=3740。静态 offset 约 2046.7–2052.0 counts，残差最大 0.019 A，
VBUS 约 11.9 V，证明静态 ADC/CSA 零点链健康。

`[实板测量]` SpeedStep 负向第 1 轮 −30 rpm **安全失败**：
`tmp/bench/20260901_095438_adc_then_speed/` 显示转子停在约 0.421 圈、实际速度
0–0.8 rpm，`ib=-1.442 A` 超过 1.4 A 门限，VBUS 仍为 11.65 V。30/60/90 rpm 两方向
均未通过，且未重试覆盖失败。末样本三相和 −1.790 A；下一步必须在冻结相位以示波器
检查 SOA/PWM/门极，并检查 encoder 磁铁圆周间隙。无示波器不得在磁路与功率级间选根因。

`[更正，2026-09-01]` 上述 `20260901_083911_adc_then_speed/` 中的六向 proof、全
map/trigger 与位置扫描，运行时 100 ms pulse 实际没有等待即发送 disable，只能保留为
脚本时序缺陷的原始记录，**不得作为电流映射或位置结论**。修复后有效 proof 位于
`tmp/bench/20260901_100147_openloop_probe_AdcProof/`：dp1/dp2 正负向有反向的毫安级
响应、KCL 近零，支持 map0 只在低 duty 条件下可读。

`[实板测量]` 高 duty 试验显示 `duty_c≈0.94` 时三相 ADC 都回到零附近，并非仅 C 相
缺失；单相 KCL 重构假设已撤回，未留在固件。SpeedStep 的同步串口往返也将采样拖至约
0.75 s 并可能触发心跳，runner 已改为非阻塞心跳、由下一条 `obs all` 回执确认。高 duty
采样孔径仍是开放问题。

## 13. 2026-09-01 CCR4=2700 复测（当前结论）

`[源码确认]` bench 默认 ADC trigger 已从 `3740` 改为 `2700`，覆盖
`motor_bench_mapping.h`、TMR1 CH4 初始化、`diag map reset`、实测 runner 的默认/恢复
路径和脚本契约。SpeedStep 入口也会先经现有 SWD gate 恢复 idle；不会以 `fault clear`
或 `state stop` 掩盖上一例状态。

`[无板验证]` 新增默认孔径 host contract 先在旧 `3740` 上失败，改动后 host 40/40、
FOC peripheral contract、SpeedStep script contract 均通过。30 s bench ELF SHA-256：
`637DCA30F208EF50EEDD826B8012B40FD1E904D4BA88EF0F6EF5727F336E79B9`，OpenOCD verify
通过。

`[实板测量]` `tmp/bench/20260901_trigger2700_adcproof/` 的六个 0.20 V/100 ms 正负
脉冲在 `CCR4=2700` 下完成，响应成对反向、峰值不超过 0.05 A、VBUS 约 11.9 V、无
激活 fault。`tmp/bench/20260901_trigger2700_triggerscan/` 的两个电角度各显示三相响应；
2700 点 KCL 残差约为 +5/-5 counts，峰值最高 0.27 A、VBUS 11.86--11.95 V。

`[实板测量]` `tmp/bench/20260901_133000_adc_then_speed/` 重新完成五次 SWD baseline
（最大静态残差 0.009 A）和 AdcProof 后，负向 -30 rpm 首级仍安全失败：转子约停在
`pos_turn=0.420`、速度约 -0.22..0.93 rpm、`ib=-1.449 A` 超过 1.4 A 中止门限，
VBUS 最低 11.63 V。末样本 duty 约 0.35/0.04/0.96，三相和 -1.786 A，说明静态有效
的 2700 孔径在动态高占空比下仍失去完整三相采样。60/90 rpm 与正方向均未执行。

`[结论边界]` ADC 原始转换与静态零点链没有显示硬件损坏证据；当前未解决的是高占空比
的动态采样策略。无示波器时不得在编码器磁路与功率级相位畸变之间选定根因，也不得
把 2700 写成完整的闭环修复。
