# FalconFOC Servo 快速开始

`falconfoc-servo-demo` 是 FalconFOC 的 Host Servo 使用入口。它通过 C11 Control Plane 初始化配置并让生产 Runtime 执行 IMPEDANCE。默认运行包括位置保持、正反向、K/D 调整、外载及 STOP，输出始终带有 Host 身份和 `physical_applied=false`。 [示例源码](../../examples/servo)

## 从已取得的源码运行

准备含 INTEG-01 的 FalconFOC 源码、CMake 3.25 或更新版、Ninja 和 C11 编译器。以下命令从源码根目录运行。Linux/macOS 用同名程序，无需 `.exe`。

```powershell
cmake -S examples/servo -B build/servo-demo -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug
cmake --build build/servo-demo --target falconfoc-servo-demo
& ./build/servo-demo/falconfoc-servo-demo.exe --help
& ./build/servo-demo/falconfoc-servo-demo.exe --inspect --iq-limit 1.8
& ./build/servo-demo/falconfoc-servo-demo.exe
```

Windows 用 GCC 时，在新的输出目录配置并将 `-DCMAKE_C_COMPILER=clang` 改为 `-DCMAKE_C_COMPILER=gcc`，或传编译器的本机路径。故障演示和示例测试入口如下。

```powershell
& ./build/servo-demo/falconfoc-servo-demo.exe --faults
& ./build/servo-demo/falconfoc-servo-demo.exe --recovery
ctest --test-dir build/servo-demo -R '^test-falconfoc-servo-' --output-on-failure
```

`--inspect` 显示参数 ID、类型、单位、范围、重初始化要求和实际配置值，流程停在 DISABLED。默认 `2.2 A` 可用运行参数改变，例如 `falconfoc-servo-demo --iq-limit 1.8`。初始候选上限 `2.5 A` 来自模型的 Current 配置，已初始化实例的动态范围以当前有效值为上限。合法的较低电流也可能无法支撑默认扰动，正常工作流会在 ARM 前报告静态可达性判断。退出码 `0` 表示所选流程完成，`1` 表示配置拒绝或工作流失败，`2` 表示命令用法错误。`--faults` 中符合预期的故障与拒绝以 `0` 结束。使用 CTest 的 `-R` 过滤可只运行已构建的 Servo 程序测试。

### 读终端输出

- `config_candidate=VALIDATED` 表示候选离线校验通过，`effective` 来自实际 Runtime 读回。初始配置和运行中的完整命令分属两个事务。
- `request=ACCEPTED` 表示请求已进入邮箱。匹配请求序号的 `SET_MODE`、`ARM`、`START` 应分别在 **200、600、800 µs** 得到 H0 的 `APPLIED`。
- 第一次 UPDATE 在 **1,000 µs**。当快照和收据匹配当前 Session、命令序号、源时间并且适配器确认 UPDATE 时，显示 `command_receipt=APPLIED`。收据查询反映当前观测，历史状态可能已被后续周期覆盖。
- `ZERO_FF` 和 `MODEL_FF` 各运行 2.4 秒虚拟时间，包含零位保持、正向运动、反向运动、K/D 变化、加卸载。反向段两组位置 RMS 分别约为 **0.019964 rad** 与 **0.000832 rad**。这些数值取自本示例的受控 Host 模型与参考。
- `Iq_request_audit_A` 量化由公开 Core 函数按 H0 已接受的命令与 Sensor 重新计算的限幅请求。 `Iq_plant_A` 取同一采样时刻的虚拟电机状态。 `limited_updates` 汇总 Core 限幅的样本数。
- `sensor_pos_rad`、`sensor_vel_rad_s` 来自实际 Runtime 观察；`pos_rad`、`vel_rad_s`、`Iq_plant_A` 来自 Plant。 `FG_us`、`sensor_us` 和 `ADC_us` 保留不同来源时刻。失效目标和无法归属的采样年龄用 `invalid` 表示。
- 每组末尾先打印 STOP 被接纳，再打印 H0 确认 INHIBIT。旧 Session 拒绝。默认 UPDATE 保持 `11996->11996`，两组各有 23,992 个已接受 duty 驱动的虚拟子步。
- `--recovery` 在同一 Runtime 上让真实命令超时锁存，核对 INHIBIT，等待 H0 确认 CLEAR，提交新命令与新传感器样本，然后重新 ARM、START，确认新 UPDATE 后再次 STOP。

## 从仓库外构建

将 `examples/servo` 目录复制到自己的工作区，保留 `CMakeLists.txt`、`main.c`、`README.md`。向 CMake 明确提供 FalconFOC 源码目录。

```powershell
cmake -S "$demoSource" -B "$demoBuild" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug "-DFALCONFOC_SOURCE_DIR=$falconSource"
cmake --build "$demoBuild" --target falconfoc-servo-demo
& "$demoBuild/falconfoc-servo-demo.exe"
& "$demoBuild/falconfoc-servo-demo.exe" --faults
& "$demoBuild/falconfoc-servo-demo.exe" --recovery
```

`$demoSource` 指复制后的示例目录，`$demoBuild` 是新建的构建输出目录，`$falconSource` 指包含 `core/`、`simulation/` 和 `control_plane/` 的 FalconFOC 源码根。路径可以有空格。外部应用可以链接这些公开目标。

```cmake
add_subdirectory("${FALCONFOC_SOURCE_DIR}/core" falconfoc-core EXCLUDE_FROM_ALL)
add_subdirectory("${FALCONFOC_SOURCE_DIR}/simulation" falconfoc-simulation EXCLUDE_FROM_ALL)
add_subdirectory("${FALCONFOC_SOURCE_DIR}/control_plane" falconfoc-control-plane EXCLUDE_FROM_ALL)
target_link_libraries(my_servo PRIVATE falconfoc-virtual-motor falconfoc-control-plane falconfoc-core)
```

`falconfoc-virtual-motor` 传递 `falconfoc-pipeline`、`falconfoc-encoder` 与公开头文件目录。示例的控制循环从标准 C 库和这些静态目标构建。Windows Host 编译与运行验证包含 Clang 和 MinGW GCC。

## 参数来源与修改 K/D

Kt 取 **0.15 N·m/A**，来自 Host PMSM 的 **4 极对 × 0.025 Wb × 1.5**，基于 Id≈0。Kt 和机械 Frame 是已选 Host 模型的只读输入。电流、转矩和 K/D 上限由 Control Plane 验证后用于初始化，再从 Runtime 读回。程序启动时也输出 J/B、编码器方向、CPR 和调度周期。标定来源状态为 `HOST_MODEL`。

示例用模型 J/B 调用 `foc_impedance_tuning_design()`，把选定的 K/D 显式放进完整 IMPEDANCE 命令，在 **1.45 s** 发布一组新 K/D。两组前馈对照保持相同的参考和 K/D 调整。应用接入标定候选时，须核对 J/B 的质量、模型身份、机械坐标和测量时序。已初始化实例的静态配置保持固定，候选只能用于下一次从合格配置初始化。

显式前馈把同一轨迹时刻的速度和加速度与 J/B 一起传给 `foc_impedance_feedforward_evaluate()`；有效候选填到 `torque_feedforward_nm`。参考位置、速度、加速度一起求值，外载仍由测试工况施加给 Plant。无前馈和启用前馈的两组共享参考、K/D、初始状态、负载和电流限制。控制品质及电流变化属于受控 Host 工况。

静态诊断通过 `foc_impedance_feasibility_evaluate()` 显示有余量、转矩不足和来源未知。0.25 A × 0.15 N·m/A 只有 0.0375 N·m，抵抗 0.039 N·m 的判定为 `TORQUE_INSUFFICIENT`。该检查描述理想静止平衡。示例在运行前打印诊断，运行期电流上限仍取调用方的配置。

## 一帧如何到达 Plant

| 责任 | 公开入口 |
| --- | --- |
| 应用配置和参考 | `foc_parameter_prepare_initial()` 和 `foc_runtime_init()` 完成静态准入；Core 的 Tuning、可达性诊断及前馈 API 构造参考 |
| 命令与生命周期 | Foreground 通过 `foc_parameter_request_action()` 和 `foc_parameter_publish_impedance()` 发布，从 Runtime 快照、生命周期结果及 Control Plane 收据验证实际状态 |
| Encoder 与 H1 | 使用 `foc_encoder_update()` 和 `foc_rotor_phase_from_counts()`，在同一 `captured_us`/Sensor Sequence 上提交位置、速度和电角；H1 调用 `foc_runtime_h1_step()` |
| ADC 与 H0 | 从同一 Host 单调时钟取得 ADC 源时刻，H0 调用 `foc_runtime_h0_step()`；H0 负责模式切换、目标准入、Current/Pipeline/Safety 和最终输出 |
| 监督与观察 | 按调度执行 `foc_runtime_foreground_poll()`，从 `foc_runtime_read_snapshot()` 读取状态、源时刻、已确认的目标与 UPDATE |
| Host 端口 | 调用方的已确认输出适配器与 Virtual Motor；只有接受的软件 UPDATE 才作为下一时间段的理想平均电压输入 |

示例的时钟每步推进 100 µs。H0、H1、Foreground 监督各自在 200、500、200 µs 的周期服务。事件同刻时，先发布到期的命令，再执行 H1、监督和 H0。新命令带实际发布时刻，Encoder 源时间与 ADC 时刻分别记录。重用旧观测时保留它的原时间。发布 SET_MODE、ARM、START 后等待 H0 的 Applied 结果，核对返回的动作、状态、模式和 Session。模式切换完成后，先提交当前 Session 的合法命令并建立可用观测，再请求 ARM 和 START。

Plant 用上一时间段接受的 PWM 比较值换算理想平均电压。 `physical_applied=false` 表明示例的输出在 Host 执行，实机输出权限留给板级移植。

## 故障演示与排查

| 现象 | 核对项目 |
| --- | --- |
| Kt 未知或初始化拒绝 | 配置与真实轴一致、正的已校验 Kt、可追溯的 dq 折算 |
| K/D 或前馈越界 | 先核对 Runtime 初始化上界以及 Core 候选是否有效，再发布命令 |
| 模式选择或 START 拒绝 | 先检查 H0 生命周期结果和 Safety 状态，再检查当前命令、Sensor 有效性与实时门限 |
| 命令超时或序号重复 | 维持命令就绪和新颖度，避免把重发旧记录当前置新输入 |
| Sensor 无效或超龄 | 保留源时间戳，检查连续位置、有效速度和原始电角是否来自同一测量 |
| Session 过期 | 从公开快照读新 Session；STOP、Fault、受控清除或切模式可以撤销旧目标 |
| 执行器能力不足 | 比较 Kt×允许 Iq、独立力矩上限与外载，需要时更改目标或验证执行器配置 |
| STOP、Adapter 拒绝或故障后仍请求运行 | 核对 INHIBIT 和 UPDATE 不再推进；确认原因后按产品的清除契约，在新的合法 Session 重新发布参考并通过 ARM、START |

只有已确认 INHIBIT、故障为独立 `COMMAND_TIMEOUT` 且满足现有条件时，`--recovery` 才能运行。Encoder 故障和需要复位的故障显示 `BLOCKED`。工作流采用显式重新准入。

## Host 与 Board1

Host C11 Control Plane 提供 IMPEDANCE。UART V1 维持 CURRENT、VELOCITY 和 POSITION；远端 IMPEDANCE 和通用模式切换在 V1 中不可用。Board1 的 ARM/START 仍为 `OUTPUT_LOCKED`。现有 Python CLI 继续提供对应的 Host 虚拟设备和 CSV/JSON 输出。该 Servo 程序的终端字段可重放对照，两组前馈指标保持既有 Host 口径。 [UART V1 合同](../../protocol/README.md) · [INTEG-01 验收](../evidence/2026-11-19-next-integ-01.md)
