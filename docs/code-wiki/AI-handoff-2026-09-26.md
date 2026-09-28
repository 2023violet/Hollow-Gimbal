# AI Handoff — 2026-09-26（现役交接入口）

本文档取代 `AI-handoff-2026-08-31.md` 成为唯一现役交接入口（08-31 文档保留为攻坚史）。
面向第一次接手的人：先读 TL;DR，再按需展开。维护日志全量在
`docs/governance/Themis_工程维护日志.md`（2026-09-26 当天有 7 轮条目）。

## 0. TL;DR

- **根因已确认**：整场"电机堵转/停滞/不丝滑"攻坚的根因是 **AVDD 3.3V 供电压垮**——
  DRV8311H 的 AVDD LDO 输入接 12V 母线（原理图注释本意 ~5V），输出同时带 MCU、编码器、
  CSAREF 和 DRV 自身内部负载，热/保护裕量为零。电机驱动一启动，3.3V 轨崩到 0.25V
  （用户万用表实测），AVDD UVLO 关断栅驱，电流消失，自恢复后再崩。
- **固件侧修复已全部就位**（本工作树，未提交——见下文警告）：波峰采样（CCR4=3740）、
  软件触发注入组、SVPWM 占空比钳位 [0.28, 0.72]、deadtime=0、编码器 LUT 标定跑通、
  闭环首次激活。
- **唯一待办（硬件）**：AVDD 供电路径修复（见 §3）。修好后跑 §5 验证阶梯，然后闭环整定。
- **红线**：修复落地前，调试器断开 = 3.3V 网络失电，禁止任何电机测试。

## 1. 根因判决与证据链

| 证据 | 内容 |
|---|---|
| PSU 实测 | 驻留矢量（dp0 3.0V）下母线电流 ~0.03A ≈ 无驱动（固件指令 vd=3V） |
| 万用表实测 | 3.3V 轨在运行期间 **3-4 次跌至 0.25V**（AVDD UVLO 2.0/1.8V 反复触发） |
| 网表事实 | `VIN_AVDD`(U4 pin7) 接 DCBUS(12V)；AVDD 输出同时供 U3(MCU)、U5(编码器)、CSAREF、上拉；AVDD 异常时 DRV"关闭相关功能" |
| 历史对账 | 08-27 "CSAREF=0.15V 无直流源"= AVDD 崩溃首次现场记录 |

**统一解释**：此前所有扑朔迷离现象都是这条电源链的崩溃时刻差异——
月度堵转/停滞、I-f hunting（1.5-2s 周期）、闭环 0.5-0.7Hz 极限环、
gain(θ) 增益地图（实为崩溃时刻图）、"相位特异死亡"（负载轨迹随机外观）、
历史拟合 k=0.66-0.90 各不相同（不同崩溃状态下的电流读数）。

## 2. 固件现状（全部在本工作树，未提交）

| 修复 | 位置 | 状态 |
|---|---|---|
| CCR4 2700→3740（计数器波峰采样） | `wk_tmr.c` + bench 默认 + 2 个 host 契约 | ✅ 验证：旋转 KCL −0.6~−1.5A → −0.01A |
| 软件触发注入组（CC4 中断 + armed 门控，弃用 TMR1CH4 硬件触发路径） | `motor_task.c` `TMR1_CH_IRQHandler` + `wk_adc.c` | ✅ 时序完全确定 |
| SVPWM duty 钳位 [0.28, 0.72] | `foc_config.h` + `foc_core.c` | ✅ 规避 duty>~0.79 时的驱动消失；I-f 不受影响 |
| TMR1 deadtime 1µs→0 | `wk_tmr.c` | ✅ 1µs 在低 duty 段吃 ~20% 开通窗（A/B 实测回退） |
| `FOC_SVPWM_DUTY` 相关宏、`CTRL_MODE_VVEL` 电压模式速度闭环、`diag voltloop` | `foc_common/foc_config/foc_ctrl/foc_motion_runtime/motor_runtime/shell` | ✅ host 45/45 |
| `cal enc` 解锁 opt-in | `ThemisContracts.cmake` `THEMIS_BENCH_FORCE_ENERGIZED_CALIB`（默认 OFF） | ✅ LUT 标定跑通，offset=−2.534，enc_valid=1 |
| runner 心跳修复 | `run_openloop_probe.ps1` ProbePulse 补 `diag bench keepalive` | ✅ 28s 脉冲全程存活 |

**警告：全部工作（含此前两个月的修复）都只在未提交的工作树里。接手后第一件事：
审查后按逻辑分组提交，在此之前严禁 `git reset/checkout` 丢弃工作树。**

## 3. 待办硬件修复（唯一阻塞项）：AVDD 供电路径

全部断电（12V 和 USB 都拔）后：

1. **焊桥恢复 AVDD 输出割口**（用户此前为实验割断的 LDO 输出→网络走线）
2. **割开 `VIN_AVDD`（U4 pin7）与 DCBUS 的连接**
3. **DAP-Link 的 5V（USB 5V）飞线到 VIN_AVDD**——LDO 压差降为 1.7V，热问题根除，
   网络由 LDO 原生供电（恢复板载独立运行能力）
4. **拆除调试器 3.3V 对该网络的飞线**（避免双源冲突）
5. 上电（12V + USB）

原理依据：原理图注释本意 VIN_AVDD≈5V；当前网表接 12V 导致 LDO 压差 8.7V 带全板负载。
微创替代（应急）：AVDD 网络并 220-470µF 电解 + 保留调试器馈电（治标）。

## 4. 修复后验证阶梯（按序，全部通过才算电源修复完成）

1. **驻留**：`diag set dp0 3.0` + `state start` → PSU 应稳定 ~0.3A、无 3.3V 跌落
   （万用表复测）、采样读数 ~0.86A（若仍 10% 则另开采样链调查）
2. **gain(θ) 复测**：12 种子 × 1.5V（θ0+vq 驻留 + trace show）→ 地图应变平
   （旧地图是崩溃时刻图）
3. **I-f 拖动**：`diag set if 0.71 21.99` → hunting 应大幅减轻（若仍 1.5-2s 周期
   摆动则电源之外还有第二因素，回流水冷思考）
4. **闭环整定**：I-f 启动 → `diag if2vel 30` → 速度环收敛到 30rpm。
   存储的 vel PID 增益 kp=0.0063 是采样失明时代遗物（默认 2.0/0.5 也未验证），
   `diag pid set vel kp/ki` 在 IDLE 时逐档试。

## 5. 台面工作流与工具

- **烧录**：`./run.sh flash`（OpenOCD V2.0.6 + DAP-Link，`tools/openocd/openocd_daplink.cfg`，
  50kHz SWD）。**下载器是 DAP-Link**（AGENTS.md 旧记载 J-Link 已修正）。
- **镜像**：实板调试固定用 **bench-30s**（`cmake --preset fw-full-diag-carrier-bench-30s-debug`
  + `cmake --build --preset build-...`）。product 镜像（10kHz FOC 步进）RUNNING 态会把
  软浮点 CPU 吃满、饿死 shell（SWD halt 采样证实）——勿用于带轴调试。
- **看门狗**：bench RUNNING 态心跳超时 1.5s，探针每 ≤0.5s 发 `diag bench keepalive`
  （只发不读）；30s 能量预算自动断。
- **恢复**：`state disable`/看门狗跳闸 → FAULT 态；SWD 复位恢复
  （`openocd -f tools/openocd/openocd_daplink.cfg -c "init; reset run; shutdown"`）。
  断 12V 不会重启 MCU（调试器 3.3V 维持）——AVDD 修复前这是常态，修复后恢复真实断电。
- **探针**（`build/` 下，本轮工具，留用）：
  - `_spin_probe.ps1` — 长旋转观察（心跳 + 周期采样 + 运行中命令 `-MidCmd`）
  - `_debug_probe.ps1` — 逗号分隔批量命令
  - `_wait_vbus.ps1` / `_wait_powercycle.ps1` / `_latch_hunt_orchestrator.ps1` — 电源实验
- **串口**：COM3 @ 115200，噪声大（PWM 耦合），长回执靠重试或板上 trace。

## 6. 命令速查（本轮新增/关键）

| 命令 | 作用 |
|---|---|
| `diag voltloop <rpm> <V>` | 纯电压开环按输出轴 rpm 下令（自动换算 we） |
| `diag set vvel <rpm>` / `cfg ctrl vvel` | 电压模式速度闭环备胎（速度 PID 直出 Uq，绕过电流环） |
| `diag if2vel <rpm>` | I-f→闭环在线交接（RUNNING+VOLT+I-f） |
| `diag set if <A> <we>` | I-f 开环（0.71A/21.99=30rpm 是验证过的配置） |
| `diag pid set <loop> kp/ki/kd/lim` | 在线整定（IDLE-only；vvel 可调） |
| `cal enc` / `cal status` | 编码器 LUT 标定（需 energized-calib opt-in 镜像） |
| `diag trace show` / `diag enc trace show` | 板上环形记录（FOC 步进/编码器），免疫串口噪声 |
| `diag map set pwm/adc <0-5>`、`diag set adc_trig <n>` | bench 映射/采样时刻（SWD 复位后需重设） |

## 7. 已知未解 / 待办

- **AVDD 硬件修复**（§3）→ 验证阶梯（§4）→ 闭环整定。此后若 hunting 仍有残余，
  再考虑角差-电流耦合（定子磁场对编码器的畸变）与 LUT 分段。
- **product 镜像 10kHz 饿死**：RUNNING 态 CPU 饱和，产品档带轴运行前需降步频或优化
  （独立工作项，勿在调试轮顺手做）。
- **质量门禁**：`check_themis_quality_gate` 在预存欠账处失败
  （`test_falconfoc_themis_lite_contract.ps1` 仍引用不存在的 at32f403a 路径、
  `tm_platform_foc_timing.h`、at32f423——多段式架构漂移，需专门一轮迁移）。
- **nFAULT/nSLEEP 无 MCU 端点**：DRV 故障/睡眠固件不可见。建议下次改版接入
  （本轮的驱动消失曾因此静默）。
- **文档漂移已修**：AGENTS/README 的 J-Link→DAP-Link；boot 文档的 COM54 历史记载
  保留为历史 witness。

## 8. 历史与全景

- 攻坚全史：`AI-handoff-2026-08-31.md`（电流链确诊、±90° 推理、隔离镜像——其中
  "±90°/磁偏心"假设已被 AVDD 根因+LUT 标定取代，保留为方法论记录）。
- 逐轮记录：`docs/governance/Themis_工程维护日志.md` 2026-09-26 的 7 轮条目。
- 参照项目对照：oshwhub `bldc_ctrl_v1.0`（同款 DRV8311H；其丝滑主因=电机阻力矩低
  + 电压模式架构不依赖电流链）。
