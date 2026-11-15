# FalconFOC V2｜NEXT-CONTROL-02 Float32 速度与位置轨迹规划

## 判定

`NEXT-CONTROL-02 SOFTWARE PASS`

本轮完成了独立 Float32 轨迹数学、H1 Runtime 接入、PMSM Host 动态对照、
状态事务验证和软件回归。轨迹规划在 Board1 参考配置中保持关闭，不能据此
宣称真实 MCU 时序、真实负载下的停止距离或真实电机到位性能。

本轮没有刷机、上电、启用 PWM、写入 Flash、提交或推送。

## 现有外环链路

```text
Foreground motion command
    -> Runtime command slot
    -> H1 coherent Sensor snapshot
    -> optional foc_trajectory trial
    -> existing velocity PI trial
    -> derived Iq target
    -> H0 accepted_outer_seq / motion_session confirmation
    -> Current PI / optional feedforward
    -> SVPWM -> Device -> Safety -> output admission
```

现有速度 PI 仍然负责速度误差和积分器，位置 P 仍然是轨迹关闭时的旧路径。
轨迹模块只生成机械 `rad/s` 参考，不创建第二套 PI。

## 轨迹数学

### 速度模式

对用户机械速度目标 `omega_target`，先限制到 `±max_velocity_rad_s`，再按实际
`dt_s` 使用：

```text
step = max_accel_rad_s2 * dt_s
planned_next = planned_previous
              + clamp(target - planned_previous, -step, +step)
```

因此：

```text
abs(planned_next) <= max_velocity_rad_s
abs(planned_next - planned_previous) <= max_accel_rad_s2 * dt_s
```

正转到反转必须先经过有界减速，不能直接跳变。

### 位置模式

位置使用连续机械坐标，不做单圈规约：

```text
distance = target_position - measured_position
brake_speed = sqrt(2 * max_accel_rad_s2 * abs(distance))
target_velocity = sign(distance) * min(max_velocity_rad_s, brake_speed)
```

随后仍经过同一加速度限制器。目标容差内请求零速度。该模型是离散参考生成器，
不保证真实电机绝不超调；实际结果还取决于速度 PI、电流限制、电压饱和、惯量、
负载和采样延迟。

### 数值和生命周期

- 轨迹配置启用时要求最大速度、最大加速度、最大 `dt` 和位置容差有限且为合法值。
- `dt` 为零、负数、NaN、Inf 或超过配置上限时返回错误，输出清零，context 不推进。
- Context 区分 `UNINITIALIZED`、`READY`、`ACTIVE` 和 `INVALID`。
- 新会话通过 `reset` 建立零速度参考；失效 context 必须显式 reset。
- 两个实例拥有独立 planned velocity，不使用全局状态。

## Runtime 事务集成

修改位于：

- [foc_trajectory.h](../../core/include/foc_trajectory.h)
- [foc_trajectory.c](../../core/src/foc_trajectory.c)
- [foc_runtime.h](../../runtime/include/foc_runtime.h)
- [foc_runtime.c](../../runtime/src/foc_runtime.c)
- [test_foc_trajectory.c](../../core/tests/test_foc_trajectory.c)
- [test_foc_motion_model.c](../../runtime/tests/test_foc_motion_model.c)

`foc_runtime_motion_config` 增加可选 trajectory 配置，默认值保持 disabled。H1
在轨迹启用时复制 `runtime->trajectory` 为 trial，先生成规划速度，再调用现有
Velocity PI trial。

正式状态的提交条件与已有外环完全一致：

```text
previous H1 sample seq == accepted_outer_seq
same motion_session
H0 accepted corresponding UPDATE
```

如果 H0、Device、Safety 或最终 UPDATE 拒绝，H1 生成的 trajectory trial 不会
覆盖正式 `runtime->trajectory`。新会话、故障、停止或模式切换后，下一次 H1 会
重新建立轨迹状态。正常轨迹减速不会延迟独立 Safety INHIBIT。

## 独立算法验证

新增 `test-foc-trajectory`，覆盖：

- 零速启动和正向加速；
- 最大速度限制；
- 正向减速和正反转；
- 不规则步进与实际 `dt`；
- 短距离和长距离位置移动；
- 接近目标主动减速；
- 目标反向和多圈连续位置；
- NaN/Inf、零/负 dt 和非法配置；
- 两个独立实例；
- reset/invalidate 生命周期；
- 轨迹关闭时参考值兼容。

核心不变量、失败不提交和状态清零全部通过。

## PMSM Host 动态模型

复用现有 `test_foc_motion_model` 的电气和机械 PMSM 模型。新增轨迹启用运行：

- 速度目标 `+8 rad/s` 后切换到 `-8 rad/s`；
- 记录规划速度上限和 H1 规划加速度上限；
- 位置目标从 `+2 rad` 改为 `-1 rad`；
- 保持 Current PI、Velocity PI、电流限制、Device Compare 和 Safety Gate 不变；
- 轨迹关闭的既有模型结果继续保持原阈值。

模型还注入了一次 UPDATE 拒绝，确认 H0 拒绝后正式 trajectory state 不推进。
该模型验证的是 Host 软件闭环，不是实际电机的绝对停止距离保证。

## 回归结果

- Core CTest：`10/10 PASS`，新增 `test-foc-trajectory`。
- Runtime/Pipeline：`7/7 PASS`。
- Board/UART/Python：`3/3 PASS`。
- Legacy Host：`45/45 PASS`。
- Legacy Bench：构建通过。
- Clang ASan/UBSan：Core `10/10`、Sensors `1/1`、Calibration `1/1`、G3-NP `2/2`、
  G3-SAFE `1/1`、Runtime `7/7`、Protocol `1/1`、Board `3/3`，合计 `26/26 PASS`，
  无 sanitizer 诊断。
- Source contract：`59 C/H、8 CMake、8 README PASS`。
- C/H formatter、受影响 CMake formatter、`Scope falconfoc` 和 `git diff --check`：PASS。

## ARM 资源

AT32F415 V2 独立构建通过，Board1 trajectory 默认关闭：

```text
.text   = 34,904 B
.rodata = 1,352 B
.data   = 84 B
.bss    = 2,488 B
Flash   = 36,724 B
RAM     = 3,600 B
```

轨迹函数静态栈帧：

```text
foc_trajectory_init     = 56 B
foc_trajectory_reset    = 16 B
foc_trajectory_velocity_step = 32 B
foc_trajectory_position_step = 40 B
```

关键调用链栈帧：

```text
foc_runtime_control       = 224 B
foc_runtime_h0_step       = 96 B
foc_pipeline_step_guarded = 312 B
```

产物 SHA-256：

```text
ELF  2E053C12778E0D2B3E904E846944CE144167FF750DACD46FEE00B9FA236FB344
BIN  47E4F58C2348CACFCA14F13DAF6B7A2D00D5D097C97DC7A187C7AD933A64984D
HEX  94C0CD49EE4ABD7C7A5A5F894DEBA1A31714EFE6F7E15DA989395AA3C7C5D375
MAP  43035CE1949E60B9A5867A0071B20C7E110240648C8215A642123ED0DC52020A
```

Host 时间不能转换成 AT32F415 周期，ARM 静态栈帧不能替代真实 H1 ISR 高水位测量。

## 未验证边界

- Board1 trajectory 默认关闭，未启用真实运动规划；
- 真实机械惯量、负载扰动、测速噪声、传感器丢帧和物理制动能力未测量；
- 未验证真实 H1 执行时间、MCU WCET、浮点库成本和实际栈高水位；
- 轨迹公式不能被解释为真实电机零超调或绝对停止距离保证；
- `Scope all` 仍受既有 `g3/np/CMakeLists.txt` cmake-format 债务影响；
- G3-SAFE 硬件验收和 G3-PWR 继续锁定。

本轮完成软件算法与 Runtime 集成验收，但不授权任何真实功率输出。

/* ---------------------------------------- end of file ----------------------------------------- */
