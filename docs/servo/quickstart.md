# FalconFOC Servo 快速开始

这个 Host 示例在 Virtual Motor 上运行生产 Runtime IMPEDANCE，演示位置保持、连续反向、负载扰动、显式运动前馈和静态可达性。示例输出始终为 `physical_applied=false`，接受软件 UPDATE 只表示模拟输出适配器接纳。源代码在 [examples/servo](../../examples/servo)。

## 从已取得的源码运行

使用包含本示例的 FalconFOC 源码目录。已授权的工作树、源码归档或远端具有本示例的修订均可作为输入。本轮代码留在本地工作树，当前旧远端分支还不含此示例。准备 CMake 3.25 或更新版、Ninja、一个 C11 编译器。从源码根目录执行三个步骤。

```powershell
cmake -S examples/servo -B build/servo-demo -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug
cmake --build build/servo-demo --target falconfoc-servo-demo
& ./build/servo-demo/falconfoc-servo-demo.exe
```

Windows 用 GCC 时，在新的输出目录配置并将 `-DCMAKE_C_COMPILER=clang` 改为 `-DCMAKE_C_COMPILER=gcc`，或传编译器的本机路径。故障演示和示例测试入口如下。

```powershell
& ./build/servo-demo/falconfoc-servo-demo.exe --faults
ctest --test-dir build/servo-demo -R '^test-falconfoc-servo-' --output-on-failure
```

`--faults` 对预期的拒绝或锁存进行断言。正常和故障演示均用退出码 0 表示所有本场景软件检查通过。保留上述 CTest 的 `-R` 过滤。单独构建示例时，CMake 会登记依赖库的测试，其测试程序需要通过相应子项目的构建入口编译。

### 读终端输出

- `SET_MODE`、`ARM`、`START` 应分别在 **200、600、800 µs** 得到 H0 的 `APPLIED`，第一次 UPDATE 在 **1,000 µs**。
- `ZERO_FF` 和 `MODEL_FF` 各运行 2.4 秒虚拟时间，包含零位保持、正向运动、反向运动、K/D 变化、加卸载。反向段两组位置 RMS 分别约为 **0.019964 rad** 与 **0.000832 rad**。这些数值取自本示例的受控 Host 模型与参考。
- `Iq_request_audit_A` 量化由公开 Core 函数按 H0 已接受的命令与 Sensor 重新计算的限幅请求。 `Iq_plant_A` 取同一采样时刻的虚拟电机状态。 `limited_updates` 汇总 Core 限幅的样本数。
- `FG_us`、`sensor_us` 与 `ADC_us` 分别记录命令、Encoder 和 ADC 的源时间。例如 `t_us=1100200` 行显示 Sensor 来自 `1100000 µs`，年龄为 `200 µs`。 `confirmed=1` 表示 Host 适配器确认。
- 每组末尾应看到 `STOP action=INHIBIT`、会话 `1->2`、UPDATE `11996->11996`。 `--faults` 分别打印即时拒绝和 Fault 锁存原因。

## 从仓库外构建

将 `examples/servo` 目录复制到自己的工作区，保留 `CMakeLists.txt`、`main.c`、`README.md`。向 CMake 明确提供 FalconFOC 源码目录。

```powershell
cmake -S "$demoSource" -B "$demoBuild" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug "-DFALCONFOC_SOURCE_DIR=$falconSource"
cmake --build "$demoBuild" --target falconfoc-servo-demo
& "$demoBuild/falconfoc-servo-demo.exe"
& "$demoBuild/falconfoc-servo-demo.exe" --faults
```

`$demoSource` 指复制后的示例目录，`$demoBuild` 是新建的构建输出目录，`$falconSource` 指包含 `core/` 与 `simulation/` 的 FalconFOC 源码。路径可以有空格。也可以从自己的 CMake 工程直接使用两个公开目标。

```cmake
add_subdirectory("${FALCONFOC_SOURCE_DIR}/core" falconfoc-core EXCLUDE_FROM_ALL)
add_subdirectory("${FALCONFOC_SOURCE_DIR}/simulation" falconfoc-simulation EXCLUDE_FROM_ALL)
target_link_libraries(my_servo PRIVATE falconfoc-virtual-motor falconfoc-core)
```

`falconfoc-virtual-motor` 传递 `falconfoc-pipeline`、`falconfoc-encoder` 与公开头文件目录。示例的控制循环从标准 C 库和这些静态目标构建。Windows Host 编译与运行验证包含 Clang 和 MinGW GCC。

## 参数来源与修改 K/D

**HOST EXAMPLE PARAMETERS — NOT BOARD1 DEFAULTS**。Kt 取 **0.15 N·m/A**，来自 Host PMSM 的 **4 极对 × 0.025 Wb × 1.5**，基于 Id≈0。其它参数使用程序启动时打印的模型、编码器方向、CPR、电流限制和调度周期。

建议先按示例使用调用方显式已知的 J/B。后续也可从 Calibration 取得 `usable` 的 J/B 候选，核对其模型身份、机械坐标、装配与测量时序后再作为输入。示例调用 `foc_impedance_tuning_design()` 生成可检查的 K/D 候选；调用方负责把选定的刚度和阻尼显式填进 IMPEDANCE 命令。要试改 K/D，请在 [main.c](../../examples/servo/main.c) 的命令构造处修改 `stiffness_nm_per_rad`、`damping_nm_s_per_rad` 的取值并重编，两组前馈对照采用相同设置。保留初始化策略提供的参数上界。

显式前馈把同一轨迹时刻的速度和加速度与 J/B 一起传给 `foc_impedance_feedforward_evaluate()`；有效候选填到 `torque_feedforward_nm`。参考位置、速度、加速度一起求值，外载仍由测试工况施加给 Plant。无前馈和启用前馈的两组共享参考、K/D、初始状态、负载和电流限制。控制品质及电流变化属于受控 Host 工况。

静态诊断通过 `foc_impedance_feasibility_evaluate()` 显示有余量、转矩不足和来源未知。0.25 A × 0.15 N·m/A 只有 0.0375 N·m，抵抗 0.039 N·m 的判定为 `TORQUE_INSUFFICIENT`。该检查描述理想静止平衡。示例在运行前打印诊断，运行期电流上限仍取调用方的配置。

## 一帧如何到达 Plant

| 责任 | 公开入口 |
| --- | --- |
| 应用配置和参考 | 调用方持有轴模型、参数上限、轨迹和序号；用 Core 的 Tuning、可达性诊断与前馈 API 得出候选 |
| 命令与生命周期 | Foreground 调用 `foc_runtime_publish_lifecycle_request()` 与 `foc_runtime_publish_impedance_command()`，从公开快照取真实 Session |
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

示例不自动恢复故障会话。对于实机，移植者负责 Sensor/ADC 源时钟、校准、板级输出权限以及关断和恢复证据。Host 的接受和拒绝只验证软件契约，持续保留 G3 安全和目标 MCU WCET 的验证边界。
