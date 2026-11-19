# FalconFOC 参数与诊断快速开始

SOFT-01 提供可选的 C11 `falconfoc-control-plane` 目标。应用仍由 Runtime 执行控制、输出准入和生命周期。参数层通过已有 Runtime 读取和发布，全部实例调用沿用单核 `H0 > H1 > Foreground` 与串行、不可重入 Foreground 契约。Core-only 工程继续只链接 `falconfoc-core`。

## 构建与运行

准备 CMake 3.25+、Ninja 和 C11 编译器。从开发源码根执行。

```powershell
cmake -S examples/parameters -B build/parameter-example -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build/parameter-example --target falconfoc-parameter-demo
ctest --test-dir build/parameter-example -R '^test-falconfoc-parameters-example$' --output-on-failure
& ./build/parameter-example/falconfoc-parameter-demo.exe
```

外部集成可复制 `examples/parameters`，在独立目录构建。 `$falconSource` 指含本轮 `control_plane/` 和现有 `simulation/` 的源码目录。用不同编译器时使用新的构建目录。Windows Clang、MinGW GCC 和 Clang ASan/UBSan 已实跑。

```powershell
cmake -S "$exampleSource" -B "$exampleBuild" -G Ninja -DCMAKE_C_COMPILER=clang "-DFALCONFOC_SOURCE_DIR=$falconSource"
cmake --build "$exampleBuild" --target falconfoc-parameter-demo
& "$exampleBuild/falconfoc-parameter-demo.exe"
```

自己的工程可用 `add_subdirectory()` 引入 `control_plane` 并链接 `falconfoc-control-plane`。Host 示例另链接 `falconfoc-virtual-motor`。

## 查参数并解释读数

- 调用 `foc_parameter_count()`、`foc_parameter_at()` 或 `foc_parameter_find()` 获取描述符。ID 用于 C 接口，类型与枚举域可从头文件和描述读取。
- `foc_parameter_get_range()` 解析静态范围或从已初始化实例取得实例范围。`foc_parameter_validate()` 校验现有类型和数值范围。 `foc_parameter_write_access()` 查询当前实例和 Session 是否允许相应路径。
- `foc_parameter_read()` 从初始化后不可变配置或一份一致的 Runtime 快照读取。要让多个诊断字段来自同一发布代次，单次调用 `foc_runtime_read_snapshot()` 后读取该结构。机械位置、速度和 Sensor 时间需要有效的 H0 消费观测。使用时同时核查源时间的新鲜度。
- `command.observed_sequence` 和源时间反映 H0 的命令观察。可用 `foc_parameter_command_status()` 进一步验证匹配的已确认 UPDATE。运行中 K/D、参考和前馈字段随整条命令发布，读取有效性通过调用方持有的命令和收据完成。

### ID、类型、单位和范围

`F` 为 Float32，`U` 为 uint32，`I` 为 int32，`E` 为公开枚举（用 u32 携带），`M` 为 `FLT_MAX`。除特别标注外，区间端点包含在内。下表记录 SOFT-01 的接口。后续版本为可选 [Config Store](config-store.md) 标记了 14 项可持久化静态配置。

| ID | 名称 | 类型 / 单位 | 范围 / 访问 |
| --- | --- | --- | --- |
| 0x1001 | motor.pole_pairs | U / 1 | [1, UINT32_MAX]，只读 |
| 0x1002 | motor.electrical_offset | F / rad | [−M, M]，只读，调用方初始校准 |
| 0x1003 | motor.kt | F / N·m/A | (0, M]，只读，已限定的初始 Kt |
| 0x1004 | servo.mechanical_frame | U / 1 | [1, UINT32_MAX]，只读 |
| 0x1005 | motion.iq_limit | F / A | (0, Iq上限]，初始配置或独立候选 |
| 0x1006 | servo.torque_limit | F / N·m | (0, M]，初始配置或独立候选 |
| 0x1007 | servo.max_stiffness | F / N·m/rad | [0, M]，初始配置或独立候选 |
| 0x1008 | servo.max_damping | F / N·m·s/rad | [0, M]，初始配置或独立候选 |
| 0x1009 | servo.max_feedforward | F / N·m | [0, M]，初始配置或独立候选 |
| 0x100A | position.speed_limit | F / rad/s | (0, M]，Position 控制器初始上限，只读 |
| 0x2001 | servo.target_position | F / rad | [−1024, 1024]，整条 IMPEDANCE 命令 |
| 0x2002 | servo.target_velocity | F / rad/s | [−M, M]，整条 IMPEDANCE 命令 |
| 0x2003 | servo.stiffness | F / N·m/rad | [0, max_stiffness]，整条命令 |
| 0x2004 | servo.damping | F / N·m·s/rad | [0, max_damping]，整条命令 |
| 0x2005 | servo.feedforward | F / N·m | [−max_feedforward, max_feedforward]，整条命令 |
| 0x3001 | runtime.mode | E / 1 | `enum foc_runtime_mode`，只读 |
| 0x3002 | safety.state | E / 1 | `enum foc_safe_state`，只读 |
| 0x3003 | safety.faults | U / bits | Runtime fault 位字，只读 |
| 0x3004 | runtime.session | U / 1 | [0, UINT32_MAX]，只读 |
| 0x3005 | command.observed_sequence | U / 1 | [0, UINT32_MAX]，只读，命令已被观察时提供 |
| 0x3006 | command.observed_source_time | U / µs | [0, UINT32_MAX]，只读，命令已被观察时提供 |
| 0x3007 | sensor.source_time | U / µs | [0, UINT32_MAX]，只读，观测有效时提供 |
| 0x3008 | output.action | E / 1 | `enum foc_device_output_action`，只读 |
| 0x3009 | output.confirmed | U / 1 | [0, 1]，只读，适配器声明范围内的确认 |
| 0x300A | output.updates | U / 1 | [0, UINT32_MAX]，只读，已接受的软件 UPDATE |
| 0x300B | output.inhibit_pending | U / 1 | [0, 1]，只读 |
| 0x300C | sensor.valid | U / 1 | [0, 1]，只读 |
| 0x300D | sensor.position | F / rad | [−M, M]，只读，观测有效时提供 |
| 0x300E | sensor.velocity | F / rad/s | [−M, M]，只读，观测有效时提供 |
| 0x300F | runtime.pipeline_status | I / 1 | 原始状态码，int32 载体，只读 |

动作各有固定发现 ID，通过 `foc_parameter_request_action()` 转交现有接口。

| ID | 动作 | 执行与结果 |
| --- | --- | --- |
| 0x4001 | action.stop | Foreground 调用原 `foc_runtime_revoke_command()`，从快照看 INHIBIT 和新 Session |
| 0x4002 | action.clear_command_timeout | 只使用原隔离命令超时恢复，读快照 `clear_result` |
| 0x4003 | action.set_mode | 传完整 Lifecycle Request，模式取 Runtime 枚举 |
| 0x4004 | action.arm | 发布已有 Lifecycle Request |
| 0x4005 | action.start | 发布已有 Lifecycle Request |

用 `foc_runtime_read_lifecycle_result()` 匹配动作、请求序号和 H0 结果。独立事件或 ISR 仍直接调用 `foc_runtime_request_inhibit()`。参数动作入口遵守 Foreground 约束。

## 静态配置与命令的生效边界

- **初始配置。** 在调用正常初始化前，用 `foc_parameter_prepare_initial()` 批量校验配置族并生成独立候选。它校验外环限额与调用方 Pipeline 电流上限的组合。由调用方把候选交给同一 Pipeline 配置的 `foc_runtime_init()`，使用新 Runtime 实例。初始化成功后读取新实例验证有效值。运动限额与 Kt 的乘积也必须有限。
- **已初始化实例。** `foc_parameter_stage()` 从真实 Runtime 配置准备独立候选，要求确认 INHIBIT 的 DISABLED、当前 Session、无 Fault 和无待处理 STOP。对运动电流只可保持或降低当前有效值。输出仍是 `VALIDATED`，原实例数值不变。若要采用，调用方先完成旧实例的退出、适配器与生产者重绑定，再以新实例走现有初始化。丢弃旧实例的收据，重新建立 Sensor、序号和会话。
- **运行控制。** 用 `foc_parameter_publish_impedance()` 提交完整 `foc_runtime_command`。保留生产时刻、Sequence、机械坐标和 Motion Session。 `ACCEPTED` 表示 Runtime 邮箱接纳。查询只有在同实例、同 Session、同源命令配对，且当前 H0 快照确认成功 UPDATE 时才返回 `APPLIED`。轮询可能错过已被后续周期覆盖的 UPDATE，收据用于当前观测。
- **拒绝。** 错误区分未知 ID、类型、越界、只读、Session、重复/老序号、状态、Fault、Sensor 或待确认 INHIBIT。失败时保留独立候选或收据的原值。Sensor 无效、Adapter 拒绝及 STOP 均应核对 Runtime 的快照、返回码和 UPDATE 计数。
- **输出口径。** `output.confirmed` 是适配器声明范围内的确认。示例是 Virtual Motor，始终 `physical_applied=false`。Board1 的 V1 `OUTPUT_LOCKED` 保留原协议解释。

J/B 属于辨识候选或应用模型，Encoder CPR/方向需与校准和机械坐标绑定。当前注册表尚未接入这些拥有者。实测 q 电流、限幅和真实轴端转矩需要可信的生产发布源。K/D 初始默认由应用命令生产者持有。参数层的首批公开能力以可校验的已用配置、快照和事务为界。

robot_mc 的固定版资料描述了 Endpoint、可发现 JSON 与持久化。本轮实现提供本机 C11 发现与准入。统一远端发现、跨版本持久化和标定编排属于后续接口范围。 [robot_mc 固定版说明](https://raw.githubusercontent.com/cyberbeast0829/robot_mc/2f1fb61ec081eb7a42ab8436ba8f1504fc529da7/README.md)
