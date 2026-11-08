# FalconFOC v2 平台化重构设计

> 状态：Proposal，尚未替换当前 live FalconFoc AT32 固件。
>
> 日期：2026-11-07

## 1. 目标

将当前面向 AT32F415 单板的 FalconFoc 固件重构为一个可验证、可移植、可扩展的开源电机控制平台。

目标产品由以下部分组成：

- 独立的 FOC 控制核心库
- 面向 MCU 和功率级的硬件端口层
- 无 FreeRTOS 依赖的确定性运行时
- 第一版自有 CAN-FD 控制协议
- 主机端 Python/C++/Rust 工具
- 可追溯的校准、参数和实机证据体系
- AT32F415 作为第一个 reference device

设计的首要顺序是物理正确性、实时确定性、安全边界、可测试性和可移植性，然后才是算法数量和高级功能数量。

## 2. 非目标

本阶段不做以下事情：

- 不把当前 `applications/` 目录直接改名后继续堆功能
- 不把 Shell 作为产品控制协议
- 不在控制核心中保留 FreeRTOS、CMSIS-RTOS2、动态内存或 MCU 头文件
- 不在第一版实现完整 CANopen 或 EtherCAT 协议栈
- 不承诺一次性覆盖所有电机、所有采样拓扑和所有无感算法
- 不将 AT32F415 的 ADC、PWM、相序、采样窗口和时钟常量写入通用核心
- 不把历史实机结论直接升级为通用控制常量
- 不在物理证据缺失时宣称产品级连续运行能力

## 3. 总体架构

```text
falconfoc-core
    ├── math
    ├── modulation
    ├── current_control
    ├── motion_control
    ├── observers
    ├── sensors
    ├── startup
    ├── calibration
    ├── safety
    └── state_machine

falconfoc-port
    ├── pwm
    ├── adc
    ├── timer
    ├── encoder
    ├── hall
    ├── fault_input
    ├── nonvolatile_storage
    └── clock

falconfoc-runtime
    ├── fast_tick_dispatch
    ├── slow_tick_scheduler
    ├── command_mailbox
    ├── telemetry_snapshot
    ├── watchdog
    └── trace_buffer

falconfoc-protocol
    ├── frame_codec
    ├── command_model
    ├── register_model
    ├── capability_model
    ├── fault_model
    └── versioning

falconfoc-transport
    ├── canfd
    ├── uart_debug
    ├── usb_cdc
    └── future_adapters

firmware
    └── at32f415_reference

tools
    ├── falconfoc-cli
    ├── canfd-monitor
    ├── log-decoder
    ├── parameter-editor
    └── evidence-packager
```

当前仓库保留为迁移宿主。新的目录和 API 必须先建立独立边界，再逐步迁移现有实现。迁移完成前，旧实现仅作为 reference firmware 和 compatibility source。

## 4. 控制核心契约

控制核心只处理标准化输入、控制状态和标准化输出。

### 4.1 核心上下文

核心上下文至少包含以下对象：

- `foc_config_t`
  - 电机电气参数
  - 机械参数
  - 保护参数
  - 控制器参数
  - 传感器和观测器能力
  - 功率级和采样能力
- `foc_measurements_t`
  - 母线电压
  - 相电流或重构电流
  - 温度
  - 传感器角度
  - 传感器速度
  - 采样质量和有效标记
- `foc_command_t`
  - 控制模式
  - 位置、速度、电流或转矩目标
  - 限速、限流和加速度限制
  - 命令序列号和有效期
- `foc_outputs_t`
  - 三相占空比
  - 使能请求
  - 保护动作
  - 当前状态
  - 诊断摘要
- `foc_timing_t`
  - 实际 `dt_s`
  - 采样序号
  - 触发时间戳
  - 计算耗时
  - overrun 标记

### 4.2 核心 API

第一版 API 只允许以下几类入口：

```c
void foc_init(struct foc_context *ctx,
              const struct foc_config *config);

void foc_reset(struct foc_context *ctx,
               enum foc_reset_reason reason);

void foc_set_command(struct foc_context *ctx,
                     const struct foc_command *command);

void foc_fast_tick(struct foc_context *ctx,
                   const struct foc_measurements *measurements,
                   const struct foc_timing *timing,
                   struct foc_outputs *outputs);

void foc_slow_tick(struct foc_context *ctx,
                   const struct foc_slow_inputs *inputs,
                   const struct foc_timing *timing);

void foc_get_snapshot(const struct foc_context *ctx,
                      struct foc_snapshot *snapshot);
```

`foc_fast_tick()` 不能调用协议、日志、Flash、阻塞 IO、动态内存或不确定时延的服务。

### 4.3 时间契约

控制核心不再使用“名义 PWM 频率”作为隐含时间来源。

每次 fast tick 都必须提供实际或经过硬件保证的时间信息。所有以下量必须从同一个 timing contract 推导：

- 电角度积分
- PI 积分
- 速度估计
- 滤波器系数
- 校准计时
- 看门狗和运行预算

如果实际节拍不稳定，系统必须显式报告 timing fault 或 timing degraded 状态。

## 5. 硬件端口层

硬件端口层只提供功能型接口，不能把平台细节泄漏到核心。

### 5.1 PWM port

负责：

- 三相高低桥占空比写入
- PWM 使能和安全关闭
- 更新同步
- 硬件 deadtime 和 break 状态读取

不负责：

- SVPWM 计算
- 电流环
- 相序策略
- 电机模式

### 5.2 ADC port

负责：

- 母线和相电流采样
- ADC rank 到逻辑通道的映射
- 原始值到物理值的换算
- 采样质量和窗口状态
- 采样序号和实际触发时间

每个板级 profile 必须明确声明：

- 采样拓扑
- rank 顺序
- 触发源
- 采样时间
- 允许的最小采样窗口
- 电流极性
- 放大器增益
- 零点策略
- 是否可以重构第三相

### 5.3 Sensor port

统一支持：

- 磁编码器
- 增量编码器
- Hall
- 无感观测器输入

所有传感器都需要返回：

- 角度
- 速度
- 有效标记
- 质量等级
- 错误状态
- 时间戳或采样序号

### 5.4 Storage port

核心只依赖逻辑存储接口。A/B、CRC、Flash sector、磨损均衡和升级策略属于 device/runtime 层。

## 6. 无 FreeRTOS 运行时

新架构不保留 FreeRTOS。

### 6.1 Fast path

由 PWM/ADC 硬件事件触发：

```text
hardware trigger
    -> sample capture
    -> measurement normalization
    -> sensor update
    -> foc_fast_tick()
    -> protection evaluation
    -> PWM commit
```

Fast path 禁止：

- malloc/free
- Flash 操作
- 协议编码
- 文本格式化
- 阻塞等待
- 不可控循环
- 依赖主循环状态的隐式握手

### 6.2 Slow path

使用静态调度器和主循环：

- 命令 mailbox 消费
- 速度环和位置环
- 校准状态机
- 参数保存任务
- CAN-FD 收发
- 诊断快照
- trace buffer 管理
- 看门狗和能力状态

调度器只需要：

- 静态任务表
- 周期和 deadline
- 事件位
- 静态 ring buffer
- 执行预算统计
- 过期任务跳过或降级策略

它不提供线程、阻塞队列、优先级反转、堆分配或隐式并发语义。

## 7. CAN-FD 自有协议 v1

第一版协议采用固定 64 字节 CAN-FD 数据帧和扩展 CAN ID。协议与控制核心解耦，CAN-FD 是第一条 transport。

### 7.1 CAN ID 建议

```text
28             24 23        16 15       8 7        0
+----------------+------------+----------+----------+
| priority       | message    | source   | target   |
| 4 bits         | class 8 bit| 8 bits   | 8 bits   |
+----------------+------------+----------+----------+
```

第一版必须固定：

- 小端编码
- 最大 64 字节 payload
- 消息版本字段
- 节点 ID
- 序列号
- flags
- opcode
- payload length
- ACK/NACK 规则
- command timeout
- fault event 规则

### 7.2 v1 消息类别

第一版只实现必要集合：

- `HELLO`
- `CAPABILITY_READ`
- `PARAM_READ`
- `PARAM_WRITE`
- `PARAM_SAVE`
- `COMMAND_SETPOINT`
- `COMMAND_STOP`
- `STATUS_READ`
- `STATUS_STREAM`
- `FAULT_READ`
- `FAULT_CLEAR`
- `CALIBRATION_START`
- `CALIBRATION_STATUS`
- `PING`
- `ACK`
- `NACK`

### 7.3 实时控制命令

`COMMAND_SETPOINT` 必须包含：

- command sequence
- command timestamp 或 freshness window
- mode
- position target
- velocity target
- current/torque target
- velocity limit
- current limit
- acceleration limit
- feedforward term
- stop behavior

设备在 command timeout 后必须进入预定义的安全状态。安全状态不能依赖主机继续发送文本命令。

### 7.4 协议设计约束

- 控制命令和诊断数据分开。
- 控制命令使用固定字段，避免动态字符串解析。
- 状态数据允许按 capability 选择字段。
- 参数写入和参数保存分成两个操作。
- 校准过程是状态机，不是一个阻塞命令。
- 所有错误都返回稳定的错误码和 fault detail。
- 协议 v1 的 schema 必须有版本号和生成工具。

第一版不追求兼容 CANopen。后续可以提供 CANopen、EtherCAT 或 VESC 风格的 adapter，但不允许这些协议反向污染核心对象模型。

## 8. 控制能力分期

### V2.0 核心能力

- 三相 PMSM/BLDC
- 电压开环
- I-f 开环
- 有感电流环
- 有感速度环
- 有感位置环
- 编码器校准
- Hall 输入
- 过流、过压、欠压、超速、过温和传感器故障
- 基础参数保存
- CAN-FD 控制协议

### V2.1 能力

- MTPA
- 弱磁
- 死区补偿
- 采样点补偿
- 前馈解耦
- 轨迹规划
- 多电机实例
- 第二种 MCU

### V2.2 能力

- HFI
- SMO
- 磁链观测器
- 低速到高速的观测器切换
- 参数辨识
- 更完整的无感启动策略

每个能力必须有能力声明、参数依赖、测试、故障边界和硬件验证记录。代码存在不等于能力完成。

## 9. 参数和校准模型

参数拆成四组：

- `motor_electrical`
- `motor_mechanical`
- `sensor`
- `power_stage`

校准输出必须记录：

- 固件版本
- 协议版本
- 校准算法版本
- 电机 ID
- 板卡 ID
- 传感器 ID
- 参数 CRC
- 生成时间
- 校准质量和失败原因

任何参数改变控制坐标系、采样比例、极对数、相序或传感器解释方式时，都必须使旧校准失效或明确进入迁移流程。

## 10. 测试体系

### 10.1 Core host tests

- 坐标变换
- SVPWM
- PID
- 角度环绕
- 控制模式切换
- 限幅和抗饱和
- 故障状态机
- 时间步长变化
- NaN、Inf 和异常输入

### 10.2 Protocol tests

- 编解码 golden vectors
- 版本兼容
- 长度错误
- 序列号错误
- 超时
- ACK/NACK
- 重复帧
- 节点地址错误

### 10.3 Port tests

- ADC rank
- PWM channel
- timer frequency
- trigger source
- encoder SPI
- fault input
- Flash geometry
- safe output

### 10.4 Simulation

至少提供：

- 理想 PMSM 模型
- Rs/Ld/Lq/flux 变化
- 负载转矩
- 摩擦和齿槽
- 电流采样偏置
- 采样延迟
- PWM 饱和
- 传感器噪声和丢帧
- 控制周期抖动

### 10.5 Hardware evidence

每次实机结果必须绑定：

- 固件 SHA
- 板卡和电机
- 参数快照
- 供电条件
- 运行模式
- 仪器
- 原始日志
- 结论等级
- 未验证内容

## 11. 迁移阶段

### Phase 0 资产冻结

交付：

- 当前仓库基线 tag
- 当前 bench 和 product 构建身份
- 当前 45/45 host 测试结果
- 当前硬件证据索引
- 当前已知缺陷和过时文档列表

验收：

- 可以从基线重建当前固件。
- 所有证据都有来源和固件身份。
- 不允许对 legacy baseline 做破坏性清理。

### Phase 1 新目录和构建骨架

交付：

- `core`
- `port`
- `runtime`
- `protocol`
- `transport`
- `firmware`
- `tools`
- `tests`

验收：

- core 可以在主机编译。
- core 不包含 MCU、RTOS、Shell 和 Flash 依赖。
- firmware 可以单独链接 reference device。

### Phase 2 时间和运行时重建

交付：

- fast tick API
- slow tick scheduler
- mailbox
- snapshot
- trace buffer
- timing overrun monitor

验收：

- 控制核心使用显式 dt。
- 实际 tick 序号和周期可观测。
- 不存在通过 ISR 服务延迟隐式改变控制频率的握手。

### Phase 3 迁移基础 FOC

交付：

- math
- Clarke/Park
- SVPWM
- current loop
- voltage open loop
- fault boundaries

验收：

- 新核心 host tests 通过。
- 新核心与现有数学模型输出在允许误差内一致。
- 不依赖 `motor_dev`、Shell 或 FreeRTOS。

### Phase 4 AT32F415 port

交付：

- PWM port
- ADC port
- timer port
- encoder port
- board profile
- fault port

验收：

- 采样 rank、相序、PWM 通道和时钟全部由 profile 声明。
- 静态 port contract 通过。
- 当前板的安全关闭行为通过。

### Phase 5 CAN-FD protocol v1

交付：

- frame schema
- encoder/decoder
- node addressing
- command timeout
- status and fault frames
- parameter operations
- Python CLI

验收：

- 不使用固件文本 Shell 完成启停、速度、电流和故障操作。
- 主机可以发现节点、读取能力、发送 setpoint、读取状态和清除故障。
- 丢帧、重复帧和超时都有确定行为。

### Phase 6 参数和校准迁移

交付：

- versioned schema
- motor/sensor/power-stage groups
- calibration jobs
- persistent storage adapter
- invalidation rules

验收：

- 更换电机或板卡后旧校准不会静默生效。
- 保存、加载、升级和损坏恢复有 host tests 和 port tests。

### Phase 7 当前板实机复验

顺序：

1. 无 PWM 只读启动
2. ADC 零点和母线
3. 固定矢量
4. 低能量电压开环
5. I-f
6. 有感电流环
7. 速度环
8. 位置环
9. 连续运行和温升

验收：

- 真实控制频率与核心 timing 状态一致。
- 独立相电流、PWM 波形和故障边界有证据。
- 旧板经验只用于验证，不进入核心硬编码。

### Phase 8 第二个 reference device

第二个设备应故意选择与 AT32F415 不同的条件，例如：

- 不同 Cortex-M 主频
- 有 FPU 或不同数学路径
- 不同 ADC 触发机制
- 不同电流采样拓扑

验收：

- 核心 API 不因第二个平台增加条件编译污染。
- 两个平台都能运行相同的 core host model 和 protocol vectors。

### Phase 9 开源发布

交付：

- README
- architecture guide
- protocol specification
- porting guide
- motor onboarding guide
- calibration guide
- safety guide
- reproducible build
- release artifacts
- license and attribution files

验收：

- 外部开发者可以只阅读文档完成一次新板移植。
- 示例、工具、协议和固件版本相互匹配。
- 所有公开性能声明都有测试或实机证据。

## 12. 旧项目资产处置规则

### 保留并迁移

- FOC 数学和控制行为
- 保护边界
- 参数和校准语义
- 实际硬件故障机制
- 已验证的 host 行为
- 实机实验方法和证据等级

### 保留但降级为 board profile

- AT32F415 timer 和 ADC 数值
- DRV8311H 采样窗口
- A/C 相位交叉
- 具体 encoder SPI 配置
- 当前电源和电流比例
- COM3、DAP-Link 和 OpenOCD 配置

### 迁移后淘汰

- 产品 Shell
- Shell 字符串作为控制 API
- FreeRTOS task ownership
- 旧 `motor_dev` 巨型状态对象
- 与旧路径绑定的静态文档 contract
- 仅验证文件形状的测试

### 只归档

- 已被新证据推翻的根因假设
- 旧 MCU 路径
- 旧串口见证信息
- 旧镜像和旧参数日志
- 不再对应当前硬件的实验脚本

## 13. 回退和停止条件

每个迁移阶段都必须能够回到 legacy reference firmware。

如果出现以下情况，暂停迁移并回到上一阶段：

- 新核心和旧核心输出差异无法解释。
- 实际控制周期无法测量。
- 协议命令无法触发安全停机。
- 物理电流和软件电流无法建立一致性边界。
- 新平台需要在 core 中加入 MCU 条件编译。
- 校准数据在电机或板卡变化后仍然静默复用。
- 测试只能证明代码编译，不能证明运行行为。

## 14. 当前执行顺序

第一轮只做 Phase 0、Phase 1 和 Phase 2。完成时间基线、目录边界和无 RTOS runtime 后，再迁移 FOC 算法。CAN-FD 协议在 runtime mailbox 和状态快照稳定后进入，不提前把协议字段写进控制核心。

当前设计不会改变 live FalconFoc 启动链，也不会删除现有代码。旧实现只有在新核心、AT32F415 port、CAN-FD v1 和实机复验全部通过后，才进入删除或归档评审。
