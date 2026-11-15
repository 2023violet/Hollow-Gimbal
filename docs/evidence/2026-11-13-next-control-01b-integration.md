# FalconFOC V2｜NEXT-CONTROL-01B 可信电角速度接入与前馈软件集成

## 判定

`NEXT-CONTROL-01B SOFTWARE INTEGRATION PASS`

本轮已在 Host 软件链路中贯通：Board/Sensor 反馈模型 → H1 双槽 → H0
一致快照 → ADC 时间对齐 → Pipeline → 可选 Float32 前馈 Core → Device
映射 → Safety Gate → Host Mock。前馈默认仍关闭，Board1 没有写入启用配置。

本结论不代表真实编码器延迟、AT32F415 ISR WCET、功率级响应或真实电机参数已经
验证。没有刷机、上电、启用 PWM、写入 Flash、提交或推送。

## 实际数据链路

```text
Board1 SPI encoder
    -> raw angle + captured_us + transport result
    -> sensors/foc_encoder
    -> foc_runtime_sensor {mechanical_rad, mechanical_rad_s, captured_us, seq, valid}
    -> H1 foc_runtime_sensor_slot {
           sample,
           electrical {angle_rad, velocity_rad_s},
           derived target,
           session
       }
    -> H0 acquire-loads one complete slot
    -> foc_pipeline_input {
           predicted theta_elec_rad,
           adc.sample_tick,
           electrical_feedback.state.velocity_rad_s,
           electrical_feedback.source_us/source_seq/valid
       }
    -> Pipeline source-age and finite-value gate
    -> foc_current_step_with_feedforward()
    -> SVPWM -> Device mapper -> Safety gate -> Host Mock
```

H1 的 `foc_electrical_state.velocity_rad_s` 是：

```text
mechanical_rad_s * rotor.pole_pairs
```

传感器方向已经在 `sensors/foc_encoder` 的机械速度中处理，Rotor 映射只乘一次极对数。
电角度 offset 只影响角度，不改变速度。H0 不重新读取编码器、不重新运行滤波器、
不重新乘极对数，也不使用全局速度变量。

## 时序契约

Pipeline 新增可选配置：

```text
feedforward.enabled
feedforward.model
feedforward.max_age_us
```

H0 每个周期提供一份 `foc_pipeline_electrical_feedback`：

- `state.angle_rad` 和 `state.velocity_rad_s` 来自同一 H1 发布槽；
- `source_us` 是编码器反馈源时间，不是 H0 消费时间；
- `source_seq` 是同一 H1 发布序列；
- `valid` 只有在 H1 完整样本通过质量与 Rotor 转换后才为真。

对于 ADC 样本时间 `adc.sample_tick`，Pipeline 使用已有 uint32 模数时间关系检查：

```text
0 <= adc.sample_tick - source_us <= max_age_us
```

这同时拒绝未来源时间，并支持合法的 uint32 回绕。前馈速度是从 H1 源时间到 ADC
时间的零阶保持估计。它没有把编码器滤波群延迟、速度噪声、估计偏差或加速度假设
包装成完整物理误差上界；这些仍属于后续真实传感器/电机测量范围。

角度仍沿用现有 `foc_rotor_predict()` 路径。只有角度预测通过、H1 样本有效且速度
快照也通过独立年龄检查，才允许前馈 Core 调用。

## 实际代码修改

- [runtime/include/foc_pipeline.h](../../runtime/include/foc_pipeline.h)：增加可选前馈策略、
  H1 电气反馈快照和 `FOC_PIPELINE_ERR_FEEDBACK`。
- [runtime/src/foc_pipeline.c](../../runtime/src/foc_pipeline.c)：初始化并保存验证后的
  前馈上下文；在 Core 前检查有限值、有效性、未来时间和源到 ADC 年龄；失效时走原有
  `INHIBIT`，不提交试算 PI 状态；有效时调用 `foc_current_step_with_feedforward()`。
- [runtime/src/foc_runtime.c](../../runtime/src/foc_runtime.c)：H0 从同一已发布 sensor
  slot 复制角度预测所用状态和电角速度，不新增共享缓存。
- [runtime/tests/test_foc_control01b.c](../../runtime/tests/test_foc_control01b.c)：新增
  H1→H0→Pipeline→Core 端到端 Host 测试。
- [runtime/CMakeLists.txt](../../runtime/CMakeLists.txt)：将集成测试纳入 Runtime CTest。
- [runtime/README.md](../../runtime/README.md)：记录速度来源、年龄边界、失效语义和
  前馈默认关闭策略。

Core 的前馈公式、PI 抗饱和和失败事务没有重写；本轮只接入已完成的入口。

## Host 集成测试

新增 `test-foc-control01b` 覆盖以下场景：

| 场景 | 结果 |
| --- | --- |
| 前馈关闭，与原 Pipeline 行为兼容 | PASS |
| 有效零电角速度，前馈开启与关闭输出一致 | PASS |
| 有效正向电角速度 | PASS |
| 有效反向电角速度 | PASS |
| 零阶保持的合法源到 ADC 延迟 | PASS |
| H1 源与 ADC 同一微秒 | PASS |
| H1 样本过期 | PASS，`FOC_PIPELINE_ERR_FEEDBACK` + INHIBIT |
| H1 源时间位于 ADC 之后 | PASS，角度/速度链拒绝 |
| H1 速度 NaN/无效样本 | PASS，H0 不使用上一会话速度 |
| uint32 时间戳回绕 | PASS |
| H1 写槽与 H0 读取交错 | PASS，只消费完整旧槽 |
| 失效时 PI 状态保持 | PASS |
| 启用策略年龄边界和半范围配置拒绝 | PASS |
| Device/Safety 后续状态 | PASS，仍沿原 Pipeline 事务门禁 |

测试还确认：H1 在 H0 读取过程中发布新样本时，H0 使用的角度和速度仍来自同一个
旧槽；不会出现角度来自样本 N、速度来自样本 N+1 的混合。

## PMSM 与时序误差边界

CONTROL-01 已验证的合成 PMSM 电气模型继续作为算法对照：

```text
vd = Rs*id + Ld*did/dt - omega_e*Lq*iq
vq = Rs*iq + Lq*diq/dt + omega_e*(Ld*id + psi_f)
```

本轮没有重新改写该模型，而是增加端到端时间条件验证：精确源时间、20 µs 合法
延迟、80 µs 超龄、未来源时间和 uint32 回绕。合法延迟进入前馈，超龄/未来输入
被拒绝并禁止 UPDATE；不会静默用 `0 rad/s`，也不会继续使用旧速度。

CONTROL-01 的精确速度模型结果仍为：

```text
omega_e=+100: PI-only q RMSE=9.536743e-07, PI+FF q RMSE=0
omega_e=-100: PI-only q RMSE=9.536743e-07, PI+FF q RMSE=0
```

这些是合成参数和 Host 模型结果，不是 Board1 的实际电机收益。参数失配、LPF 群
延迟和真实加减速仍需要真实传感器与电机证据。

## 回归与资源

- Runtime/Pipeline CTest：`7/7 PASS`，包含新增 `test-foc-control01b`。
- Board/UART/Python Host：`3/3 PASS`。
- Legacy Host：`45/45 PASS`。
- Clang ASan/UBSan：Core `9/9`、Sensors `1/1`、Calibration `1/1`、G3-NP `2/2`、
  G3-SAFE `1/1`、Runtime `7/7`、Protocol `1/1`、Board `3/3`，合计 `25/25 PASS`，
  无 sanitizer 诊断。
- Legacy Bench：构建通过。
- Source contract：`56 C/H、8 CMake、8 README PASS`。
- C/H formatter、受影响 CMake formatter、`Scope falconfoc` 和 `git diff --check`：PASS。

AT32F415 V2 独立 ARM 构建通过，仍使用 `cortex-m4` 编译目标、`cortex-m3-soft`
CPU class、soft-float ABI 和 `FPU=none`：

```text
.text   = 33,748 B
.rodata = 1,332 B
.data   = 84 B
.bss    = 2,412 B
Flash   = 35,548 B
RAM     = 3,528 B
```

关键静态栈帧：

```text
foc_current_step_internal = 184 B
foc_pipeline_step_guarded = 312 B
foc_runtime_control       = 224 B
foc_runtime_h0_step       = 96 B
```

本轮静态产物 SHA-256：

```text
ELF  276EC8C528E50AD3DD905437AF6F8701B1190730DE39F3BD44FDD2DA9E2A9BC4
BIN  27FD4D52D06F52227661679C188EA06220A302BC099237CAF7D1F4CFCFB1CDDF
HEX  C8E43622FB8A221356BEC8CF05254222CCCDB86E996DC5C54614064D7BC66D58
MAP  ACC16AA624D06E4787541C5DC98392BEAE2B3235F2D6960836FB88A2A3CC4EB8
```

前馈默认关闭时没有新增 H0 三角函数、速度滤波器、动态内存或共享可变全局状态。
Host 计时和 ARM 静态栈帧不能替代真实 MCU WCET、ISR 占用或栈高水位测量。

## 未验证边界

- Board1 `pipeline_config.feedforward.enabled` 仍为默认 `false`；CAL-01 合成参数没有
  写入生产配置。
- 真实编码器 SPI 延迟、LPF 群延迟、速度噪声、加速度上界和电机 `Ld/Lq/psi_f` 尚未测量。
- 未验证真实 ADC 采样窗口与 H1 捕获之间的硬件时延。
- 未验证实际 PWM 更新截止时间、桥臂禁止时延、功率级故障行为或 WDT 复位行为。
- `Scope all` 仍受既有 `g3/np/CMakeLists.txt` cmake-format 债务影响；本轮没有扩大范围
  修改该历史文件。
- G3-SAFE 硬件验收与 G3-PWR 继续锁定。

本轮满足软件集成验收，但不授权任何真实功率输出。

/* ---------------------------------------- end of file ----------------------------------------- */
