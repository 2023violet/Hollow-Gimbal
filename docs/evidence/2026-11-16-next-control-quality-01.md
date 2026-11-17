# FalconFOC V2｜NEXT-CONTROL-QUALITY-01 控制品质整改证据

## 结论

- 阶段：NEXT-CONTROL-QUALITY-01
- 结果：NEXT-CONTROL-QUALITY-01 PASS
- 控制品质：CONTROL_QUALITY_FAIL / CONTROL_QUALITY_PASS / EXPECTED_DEGRADED 按场景分别保留
- 本轮没有修改生产 Current/Velocity/Position、Runtime Safety、UART V1、Board1 参数或真实输出权限
- 真实硬件、MCU WCET、物理关断和产品级 PI 整定仍未验证

## 1. 身份与修改范围

| 项目 | 实际值 |
|---|---|
| 工作树 | D:\study\Software\Hollow-Gimbal-vm01a |
| 分支 | codex/falconfoc-v2-vm01a |
| HEAD | a198f6d4a6c61f1bb9dd5968a46e7f38ac49a3f0 |
| 主工作树 | D:\study\Software\Hollow-Gimbal，clean |
| ARM BIN | 37,864 B |
| ARM SHA-256 | BDEC4D2706049080FF758EE308ADDC1304294DFD5805F05A0CAE7D56D4191806 |

本轮修改集中在 simulation、tools/falconfoc-cli、Host tests、README、Quickstart、维护日志和本报告。旧 VM-01a～01d CSV/JSON/Golden/evidence 均保留。

## 2. CQ-01 状态窗口

JSON 增加 metrics_schema_version=2、measurement_window、full_window_rmse、running_window_rmse、running_tail_rmse、transient_peak_error、steady_state_error 和 settling_time_us。

保留历史 rmse、tail_rmse、peak_error 和 settling_us。

窗口规则：

- full：全部 virtual 记录，历史指标保留。
- running：state=RUNNING 且 output_action=UPDATE。
- running_tail：RUNNING 有效样本的最后 25%。

Current 实际结果：

| 窗口 | RMSE | Tail RMSE |
|---|---:|---:|
| 完整窗口 | 0.152791 A | 0.081809 A |
| RUNNING | 0.089169 A | 0.005697 A |

Current 仍保留 CONTROL_QUALITY_FAIL；没有因为 RUNNING 尾段较好而改写整体结论。

短/长时域独立观察：

| 场景 | 时域 | Quality | 结果 |
|---|---:|---|---|
| Velocity | 10 ms | EXPECTED_DEGRADED | final speed 0.35933 rad/s |
| Velocity | 100 ms | CONTROL_QUALITY_PASS（研究阈值） | final speed 2.61577 rad/s，Settling 95500 us |
| Position | 1.2 s | EXPECTED_DEGRADED | final error 0.10131 rad |
| Position | 2 s | CONTROL_QUALITY_PASS（研究阈值） | final error 0.00493 rad，Settling 1383000 us |

长时域通过独立 horizon=long 运行，没有替换历史短时证据。

## 3. CQ-02 Load 反事实

保留原场景：0.03 N*m，5000–15000 us，duration 30000 us，advance 500 us。

新增同初始状态、同命令、同传感器、同虚拟时间的 0 N*m 对照：

| 结果 | +0.03 N*m | 0 N*m |
|---|---:|---:|
| Final velocity | 0.961930233 | 1.064114650 |
| Final position | 0.013199709 rad | 0.016118727 rad |
| RMSE | 2.593820142 | 2.510737477 |
| Replay | 3758926069 | 3438665344 |

最大差异：velocity 0.141662264 rad/s、position 0.0030701113 rad、iq 0.027024388 A。

Oracle 结论：LOAD_EFFECT_CONFIRMED。

历史 speed_drop=0 保留，仅作为旧指标。Python Host test 运行同时间线 Load A/B；将 foc_virtual_motor_set_load 强制写零后，新的 Load Oracle 失败。

## 4. CQ-03 Fault 事件级证据

Host context 新增事件 journal 和 CSV/JSON 字段，记录：

- fault injection
- sensor failure observed
- H1 invalid observation
- H0 failure observation
- inhibit requested/accepted
- fault latched
- last accepted UPDATE
- 各事件对应 H0 UPDATE 计数

实际事件：

| 事件 | 时间 |
|---|---:|
| fault injection | 5000 us |
| sensor failure observed | 5000 us |
| H1 invalid observation | 5000 us |
| H0 failure observation | 5000 us |
| software INHIBIT requested | 5000 us |
| software INHIBIT accepted | 5000 us |
| FAULT_LATCHED | 5400 us |
| last accepted UPDATE | 4800 us |

对应计数：

| 项目 | 数值 |
|---|---:|
| software_inhibit_latency_us | 0 |
| fault_latch_latency_us | 400 |
| updates_before_fault | 11 |
| updates_at_sensor_failure | 11 |
| updates_at_h0_failure | 11 |
| updates_at_inhibit | 11 |
| updates_at_fault_latched | 11 |
| updates_after_inhibit | 0 |
| updates_after_fault_latched | 0 |

未观察事件使用 -1，不再用 0 us 冒充事件发生。

## 5. CQ-04 独立 Oracle 和变异

Virtual Sensor 测试增加独立 d/q→alpha/beta→phase B/C 解析式参考，覆盖非零 d/q、电流正负符号和 ADC decode。预期值不调用被测转换函数。

Virtual Motor 测试增加同一电压、同一初始状态下 0 N*m 与 0.03 N*m 的速度/位置差异断言。

变异结果：

| 变异 | 结果 |
|---|---|
| ADC alpha feedback sign 反转 | 对应 Sensor Oracle 失败 |
| Load torque 强制为零 | Load trajectory Oracle 失败 |

这些变异在整改前可让 Simulation 14/14 PASS；现在由对应行为断言检出。

## 6. Host-only Target Policy

复核确认：

- Board1 使用 foc_serial_init()，默认 LOCKED。
- Host Virtual 使用显式 VIRTUAL_ONLY。
- UART/HELLO/Session/PARAM_WRITE 不能修改 policy。
- Board1 ARM/START 继续 OUTPUT_LOCKED。
- Board1 UPDATE 继续 NOT_APPLIED。
- physical_applied=false 语义保持。
- ARM BIN 与 VM-01c 一致。

没有发现真实 Board1 获得 VIRTUAL_ONLY 的路径。本轮没有引入新的策略框架。

## 7. 回归

| Lane | 结果 |
|---|---|
| Simulation | 14/14 PASS |
| Clang ASan/UBSan Simulation | 14/14 PASS |
| Runtime | 11/11 PASS |
| Board Host | 4/4 PASS |
| Python/C CLI | 8/8 PASS |
| Legacy Host | 45/45 PASS |
| Source contract | 79 C/H, 9 CMake, 9 README PASS |
| C/H + CMake formatter | PASS |
| CMake quality | PASS |
| Scope falconfoc | PASS |
| Scope all | SCOPE_ALL_ENVIRONMENT_BLOCKED，缺 Bash |
| ARM | 37,864 B / BDEC4D2706049080FF758EE308ADDC1304294DFD5805F05A0CAE7D56D4191806 |

Scope all 前置 Source/CMake/Preset/Platform/Smoke/Cache/Shell 阶段通过；Firmware CLI contract 仍因环境缺 Bash 阻断，没有修改门禁或伪造 PASS。

## 8. 尚未解决

- Current RUNNING RMSE 达到 0.089169 A，但正式产品阈值适用范围仍需定义。
- Velocity/Position 长时结果只证明研究 Profile 下的 Host 观察，不是 Board1 控制品质认证。
- Load A/B 证明 Plant 对负载响应，不证明真实功率级抗扰。
- Fault 软件抑制/锁存时间已可观察，真实桥臂关断仍未验证。
- MCU WCET、栈高水位、ISR 抢占和真实电机行为仍未验证。

## 9. 阶段结论

NEXT-CONTROL-QUALITY-01 PASS

本轮关闭了：

- RUNNING 与非 RUNNING 指标窗口混用；
- Load 只看 speed_drop 的指标缺陷；
- Fault inhibit/latch/UPDATE 事件混淆；
- alpha feedback sign 和 Load no-op 变异漏检。

本轮没有把研究阈值通过解释成产品性能通过，也没有进入 PI 整定或硬件阶段。

/* ---------------------------------------- end of file ----------------------------------------- */

