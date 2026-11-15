# FalconFOC V2｜NEXT-SYSTEM-01 完整控制链稳定性、故障注入与资源收敛

## 判定

`NEXT-SYSTEM-01 SOFTWARE PASS`

本轮没有修改生产控制算法、Runtime 调度模型、Safety FSM、Board1 输出路径或校准准入逻辑。新增一条独立的 `test-foc-system` Host 系统测试，把已存在的轨迹、速度 PI、前馈、电流 PI、SVPWM、Device、Safety 和 Host Mock 组合起来，执行确定性多速率事件流、重复回放和故障注入。

该结论覆盖软件 Host 集成、状态事务、异常传播、Sanitizer 和静态 ARM 构建。它不代表真实 AT32F415 WCET、栈高水位、SPI/ADC 时序、物理功率级或真实电机行为已经验证。

## A｜实际控制链与所有权

```text
Board1 nominal / CONFIG-01 explicit Host configuration
        ↓
ADC + Sensor snapshots
        ↓
H1 coherent electrical state
        ↓
Trajectory trial → Velocity PI trial → derived Iq target
        ↓
H0 angle prediction / source-age check
        ↓
Current PI + optional PMSM feedforward
        ↓
SVPWM / duty and voltage limiting
        ↓
Device mapping
        ↓
Safety gate
        ↓
Host Mock submission
        ↓
accepted UPDATE → staged state commit
```

`test-foc-system` 使用显式 Host 配置启用 feedforward 和 trajectory；Board1 的默认 `foc_board_init()` 仍然保持校准关闭、前馈关闭、轨迹关闭和功率输出锁定。CONFIG-01 的完整准入和 Board Profile A/B 数据链由既有 `test-foc-calibration` 与 `test-foc-board-config` 覆盖，本轮没有把 Host 合成参数写入默认固件。

## B｜多速率时序与快照一致性

系统测试采用 H0 `200 µs`、H1 `500 µs`、Foreground supervision `200 µs`，并在 H1 与 H0 同时到期时先发布 H1，再执行 H0。覆盖内容包括：

- H1 更新早于 H0 消费；
- H0 在 H1 之间重复消费同一完整槽；
- H1 角度、速度、源时间和有效性来自同一发布快照；
- H0 只消费已发布快照，不读取半写状态；
- 目标命令在模式切换后重新发布，旧会话不会进入新控制会话；
- 电角度预测和前馈源年龄检查继续由现有 Runtime/Pipeline 执行；
- uint32 回绕、未来时间、过期反馈和 H1/H0 交错由既有 `test-foc-control01b`、Runtime 和 Sensor 测试覆盖。

`test-foc-system` 的重复回放结果：

```text
system replay hash = 3f01f6e3
H0 due             = 103
H1 due             = 41
accepted outer seq = 40
motion session     = 3
UPDATE before fault = 96
H0/H1 missed       = 0 / 0
H0/H1 late         = 0 / 0
H0/H1 budget       = 0 / 0
```

两次完全相同的事件序列得到相同结构体结果和相同指纹。

## C｜控制状态事务与故障注入

系统测试在 UPDATE 被接受前分别保持轨迹、外环 PI 和电流 PI 状态为试算状态。测试流程为：

1. 启用轨迹、速度 PI、前馈和 Host Mock；
2. 执行正向速度目标；
3. 刷新命令后切换到反向目标；
4. 让 Runtime 连续提交有效 UPDATE；
5. 设置 Mock `reject_update`；
6. 验证本周期返回 `FOC_RUNTIME_ERR_CONTROL` / `FOC_PIPELINE_ERR_UPDATE`；
7. 验证电流 PI d/q 积分、速度 PI 积分和 trajectory context 与拒绝前完全一致；
8. 发送独立 WDT inhibit 请求；
9. 下一次 H0 确认 `INHIBIT`，Safety 进入 `FAULT_LATCHED`，故障位为 `FOC_SAFE_FAULT_WDT`。

诊断、故障和时间状态仍按 Runtime/Safety 契约更新；测试没有错误地要求所有诊断字段在 UPDATE 拒绝时冻结。

现有 `test-foc-motion-runtime` 和 `test-foc-control01b` 继续覆盖：

- 无效 Sensor；
- 电角度预测失败；
- 前馈反馈失效、未来和过期源时间；
- Device/Safety/Mock 拒绝；
- 模式切换和 `motion_session` 隔离；
- UPDATE 拒绝后的外环和内环状态保持；
- 故障清除不能复用旧会话状态。

## D｜长时间稳定性

已有 `test-foc-long` 继续执行 `100,000` 次确定性控制事件，并重复回放：

```text
long replay hash = 49dea950
updates          = 99,998
H0 due           = 100,001
missed           = 0
late             = 0
budget           = 0
```

新增系统链测试执行约 `19,000 µs` 的多速率轨迹/前馈/电流闭环事件流。两条测试共同覆盖了长事件流稳定性和启用完整外环后的事务行为。没有发现数值错误、非有限 UPDATE、未预期故障或状态指纹漂移。

## E｜PMSM 动态模型与数值结果

复用既有 PMSM Host 模型，不修改模型参数或旧阈值。当前回归输出包括：

- 正向与反向速度运行保持电流和电压有界；
- 速度运行时尾部误差约 `1e-4` 到 `5e-4 rad/s` 量级；
- 位置目标 `+7 rad` 和 `-3 rad` 的尾部位置误差分别约 `8.36e-4 rad` 和 `2.24e-3 rad`；
- 前馈正反速度、限幅和参数失配路径继续通过；
- 轨迹速度/位置规划、正反转、减速和 UPDATE 拒绝继续通过；
- 模型输出仍是合成 Host 结果，不是实际电机性能承诺。

现有 motion model replay 指纹保持为 `884b005e`，故障结果为 `00000040`（WDT）。

## F｜配置、校准和安全边界

- CONFIG-01 的 `foc_calibration_admit()`、方向乘积映射和 Board1 默认配置未修改；
- 默认 Board1 仍使用名义 `2048/2048/2048`、7 极对、0 rad、前馈关闭、轨迹关闭；
- Host 显式配置才启用 trajectory/feedforward；
- Host 合成校准值未被当作真实 Board1 标定结果；
- Board1 Host UPDATE 仍由 `foc_board_output_submit()` 拒绝；
- `INHIBIT`、`ZERO_DUTY`、`ACTIVE_BRAKE` 仍保持不同的软件请求语义；
- 没有隐藏 PWM 使能、Flash 写入、在线配置或自动校准路径。

## G｜ARM 资源和执行路径

AT32F415 重新构建仍使用 Arm GNU Toolchain `14.3.1`、`cortex-m4` 指令目标、`cortex-m3-soft` CPU class、soft-float ABI 和 `FPU=none`：

```text
.text   = 34,916 B
.rodata = 1,352 B
.data   = 84 B
.bss    = 2,520 B
Flash   = 36,736 B
RAM     = 3,632 B
```

产物 SHA-256 与 CONFIG-01 保持一致：

```text
ELF  51D7428FF2414FA4623DCE5086A6CD8D4F69058DFC6405B317DC28875E281551
BIN  9806ADBFA3282450527F40BB97CB1DA75FAFE41FB405F1126114187C6393B029
HEX  B80C263E6B07CE976E2BF1AEDED2737EED536C7224369091DC0919CAA48C51AE
MAP  BDC648824C427C873259638A30EB2FAE0C59E1598EBEA3C2C43E0C29CF6516B3
```

关键静态栈帧：

```text
foc_runtime_control             224 B
foc_runtime_h0_step              96 B
foc_runtime_h1_step             288 B
foc_runtime_foreground_poll     240 B
foc_runtime_init                600 B
foc_current_step_internal       184 B
foc_trajectory_velocity_step     32 B
foc_trajectory_position_step     40 B
foc_encoder_update                72 B
foc_device_decode_sample          48 B
foc_device_map_output             56 B
```

这些是单函数 `.su` 帧大小，不是最大运行栈高水位。最终 H0/H1/Foreground 调用链还受到软浮点库、调用路径和中断嵌套影响，真实 WCET、栈高水位和 200/500 µs 实机预算仍未验证。

## H｜工程回归

| 检查 | 结果 |
| --- | ---: |
| Core | `10/10 PASS` |
| Sensors | `1/1 PASS` |
| Calibration | `1/1 PASS` |
| Runtime/Pipeline | `8/8 PASS`，新增 `test-foc-system` |
| G3-NP | `2/2 PASS` |
| G3-SAFE | `1/1 PASS` |
| Board/UART/Python | `4/4 PASS` |
| Protocol | `1/1 PASS` |
| Legacy Host | `45/45 PASS` |
| Legacy Bench | 构建通过 |
| Clang ASan/UBSan | Core `10/10`、Sensors `1/1`、Calibration `1/1`、NP `2/2`、Safe `1/1`、Runtime `8/8`、Protocol `1/1`、Board `4/4`，合计 `28/28 PASS` |
| Source contract | `61 C/H、8 CMake、8 README PASS` |
| C/H formatter | PASS |
| 受影响 CMake formatter | PASS |
| `Scope falconfoc` | PASS |
| `Scope all` | BLOCKED，既有 `g3/np/CMakeLists.txt` 格式债务 |

## I｜本轮源码修改

本轮没有生产代码或控制算法缺陷需要修复。唯一新增源码为：

- `runtime/tests/test_foc_system.c`：完整系统级 Host 事件流、重放指纹和 UPDATE/WDT 故障注入；
- `runtime/CMakeLists.txt`：将 `test-foc-system` 纳入 Runtime CTest；
- 本证据文档和工程维护日志。

没有修改 Core、Runtime 生产实现、Safety FSM、Board1 输出路径、校准准入、通信协议或默认参数。

## J｜未关闭事项

- 真实 AT32F415 H0/H1 WCET、软浮点执行时间和栈高水位仍未测量；
- 真实 SPI/ADC 采样窗口、编码器延迟、功率级响应和 WDT 硬件行为仍未验证；
- `Scope all` 继续受既有 `g3/np/CMakeLists.txt` cmake-format debt 阻断；
- `RC0 SOFTWARE BLOCKED`、`G3-SAFE HARDWARE NOT VERIFIED`、`G3-PWR LOCKED` 保持不变；
- 本轮没有刷机、上电、启动电机、启用 PWM、写 Flash、commit 或 push。

## 可复核命令

```powershell
cmake -S runtime -B build/system-runtime -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/system-runtime --parallel 4
ctest --test-dir build/system-runtime --output-on-failure
powershell -NoProfile -ExecutionPolicy Bypass -File build/rc0/run-sanitizer.ps1
cmake --build build/config-arm --parallel 4
cmake --build build/sw04-legacy-bench --parallel 4
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_falconfoc_source_contract.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format_themis.ps1 -Scope c -Mode check
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format_themis.ps1 -Scope cmake -Mode check
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_themis_quality_gate.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal -Scope falconfoc
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_themis_quality_gate.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal -Scope all
```

/* ---------------------------------------- end of file ----------------------------------------- */
