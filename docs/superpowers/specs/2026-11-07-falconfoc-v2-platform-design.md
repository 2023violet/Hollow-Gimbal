# FalconFOC v2 平台化重构设计

> 状态：Proposal，尚未替换当前 live FalconFoc AT32 固件。
>
> 日期：2026-11-07

## 1. 目标

将当前面向 AT32F415 单板的 FalconFoc 固件重构为一个可验证、可移植、可扩展的开源电机控制平台。

目标产品由以下部分组成：

- 独立的 FOC 控制核心库
- 面向 MCU 和功率级的硬件端口层
- Core 无 FreeRTOS 依赖，V2 reference device 使用确定性的裸机混合运行时
- 第一版自有 UART 二进制控制协议
- 后续可复用同一命令语义适配 CAN 2.0、CAN-FD 或其他传输
- 主机端 Python/C++/Rust 工具
- 可追溯的校准、参数和实机证据体系
- AT32F415 作为第一个 reference device

设计的首要顺序是物理正确性、实时确定性、安全边界、可测试性和可移植性，然后才是算法数量和高级功能数量。

## 2. 非目标

本阶段不做以下事情：

- 不把当前 `applications/` 目录直接改名后继续堆功能
- 不把 Shell 作为产品控制协议
- 不在控制核心中保留 FreeRTOS、CMSIS-RTOS2、动态内存或 MCU 头文件
- 不在第一阶段删除或替换已经验证可用的 Legacy FreeRTOS runtime
- 不把 CANopen、EtherCAT 或 CAN-FD 作为 V2 第一版门槛
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
    ├── uart
    ├── future_can2
    ├── future_canfd
    ├── future_usb
    └── future_ethernet

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

当前 Board1 的外部通信事实是 USART。AT32F415 芯片具备经典 CAN 2.0B Active 控制器，但当前 PCB 没有可用的 CAN 外部链路，芯片也不具备原生 CAN-FD 控制器。因此 V2 的第一条真实传输只能是 UART。

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
- `validated_motion_command_t`
  - 控制模式
  - 位置、速度、电流或转矩目标
  - 限速、限流和加速度限制
  - Device 层已经完成的有效性和超时判断
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

`foc_fast_tick()` 不能调用协议、日志、Flash、阻塞 IO、动态内存或不确定时延的服务。UART 帧序列号、CRC、节点地址和传输错误不进入 Core。数据流必须经过 `protocol_message -> device_request -> validated_motion_command -> foc_core` 的边界。

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

## 6. V2 reference runtime

当前 Legacy 固件继续保留 FreeRTOS。V2 Core 不依赖 FreeRTOS，V2 reference device 默认采用硬实时中断、前台事件驱动超级循环、静态协作调度和显式状态机的混合模型。未来可以增加 FreeRTOS runtime adapter，但不能改变 Core、Device service 和 Protocol 的语义。

具体执行域、资源所有权、调度器、邮箱、时间源、故障动作和实施禁令见 [Runtime ADR R1](2026-11-08-falconfoc-v2-runtime-adr-r1.md)。

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
- UART 收发和后续 transport adapter mailbox
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

## 7. Falcon Protocol v1 over UART

第一版协议使用当前 Board1 已有的 UART/USART 链路。协议定义消息语义和版本，UART 只负责字节流传输。未来的 CAN 2.0、CAN-FD、USB 或 Ethernet adapter 复用命令语义，但不要求所有 transport 拥有相同的时序和可靠性特征。

### 7.1 UART transport contract

- USART1，当前默认 115200 8N1
- DMA RX 环形缓冲
- DMA TX 有界发送队列
- USART IDLE、DMA half-transfer、DMA transfer-complete 或有界轮询共同推进接收消费
- 接收消费必须支持连续数据流，不能只依赖 IDLE 间隙
- RX 溢出必须可检测并产生 transport fault
- 控制命令、故障事件、状态遥测和调试日志使用不同队列策略

### 7.2 UART frame format

V1 使用有界长度二进制消息。候选帧格式为 COBS 加 `0x00` 分隔符，并在逻辑 payload 内包含 CRC16。

```text
version
message_type
flags
sequence
payload_length
payload
crc16
```

协议必须定义：

- 小端编码
- 字段类型和单位
- 消息版本
- 序列号
- payload 长度
- CRC16
- ACK/NACK
- 重复请求处理
- 请求超时
- 串口失联处理
- 重同步策略
- 最大输入帧长度

当前 115200 8N1 的理论字节预算约为 11520 B/s。持续遥测必须有带宽预算，遥测过载时允许丢弃旧样本，但不能丢弃 stop、disable、fault 和失联语义。

### 7.3 v1 message classes

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

### 7.4 实时控制命令

`COMMAND_SETPOINT` 必须包含：

- command sequence
- freshness window
- mode
- position target
- velocity target
- current/torque target
- velocity limit
- current limit
- acceleration limit
- feedforward term
- stop behavior

Device 在 command timeout 后必须进入预定义的安全状态。安全状态不能依赖主机继续发送文本命令。

### 7.5 协议设计约束

- 控制命令和诊断数据分开。
- 控制命令使用固定字段，避免动态字符串解析。
- 状态数据允许按 capability 选择字段。
- 参数写入和参数保存分成两个操作。
- 校准过程是状态机，不是一个阻塞命令。
- 所有错误都返回稳定的错误码和 fault detail。
- 协议 v1 的 schema 必须有版本号和生成工具。

第一版不实现 CANopen、EtherCAT 或 CAN-FD。后续可以提供 CAN 2.0、CAN-FD、CANopen、EtherCAT 或 VESC 风格的 adapter，但不允许这些协议反向污染核心对象模型。

### 7.6 安全语义

协议命令必须区分：

- `STOP`，停止运动目标并按配置处理输出保持
- `DISABLE`，强制关闭功率输出
- `BRAKE`，进入明确声明的制动策略
- `FAULT_CLEAR`，只清除允许清除的故障，不自动重新使能功率级

串口失联、RX 溢出、帧损坏、序列号异常和命令过期都必须进入明确的安全状态。协议入口的检查不能替代最终执行点的安全检查。

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
- UART 二进制控制协议

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

Flash 保存必须在最终写入前再次确认轴状态、功率输出状态和调度上下文。参数入口已经通过安全检查，不能作为最终写入点的唯一依据。擦除、写入和恢复失败都必须可观测并进入安全错误路径。

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
- 连续 UART 流重同步
- COBS 解码错误
- CRC 错误
- RX 溢出
- 遥测过载
- 节点地址错误或 transport 不适用

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

### G0 资产冻结

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

### G1 硬件和时间事实纠正

交付：

- UART 外部链路事实
- AT32F415 CAN 2.0B 与 CAN-FD 能力表
- Cortex-M4、DSP、无 FPU 事实确认
- 当前 CPU class 和 FreeRTOS port 的事实记录
- ADC/PWM/encoder/fault 能力清单
- 当前采样频率、控制频率和执行时间测量计划

验收：

- 不修改 legacy 默认 CPU 配置，不把 CPU class 或 FreeRTOS port 迁移作为 V2 门禁。
- 可选建立 Cortex-M4 DSP 编译优化实验，只有在 WCET、代码尺寸和行为回归均有数据后才形成独立决策。
- 记录 CMSIS-DSP 定义、GCC `-mcpu`、软浮点 ABI 和 FreeRTOS 旧 port 的现状。
- 形成当前板支持和不支持的能力表。

### G2 最小 Core 和构建骨架

交付：

- `core`
- Core 数据结构和显式 timing/sample/output contract
- Clarke/Park
- SVPWM
- 电压开环
- 电压和占空比限幅
- Core host tests
- 最小 reference device 构建目标

验收：

- core 可以在主机编译。
- core 不包含 MCU、RTOS、Shell 和 Flash 依赖。
- 电压开环模型、SVPWM 边界和限幅测试通过。
- Core API 已足够支撑 G3 的 ADC 到 PWM 纵向链。

### G3 AT32F415 第一条板级纵向链

交付：

- 新 Core
- AT32F415 port
- 采样输入
- 控制计算
- PWM 输出
- timing snapshot

验收：

- ADC 到 Core 到 PWM 的最小链路在当前板上运行。
- 实际采样时间、控制时间、输出更新时间和漏样可以读取。
- 不依赖 UART 命令完成 fast path。
- 先通过 G3-SAFE 门禁，再允许任何真实 PWM 输出或带电机测试。

### G4 裸机运行时和安全

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
- H0 电流控制域与 H1 传感器/运动外环域各自独占内部状态。
- 前台调度器具备单轮预算、job 最大迟到、饥饿计数和调度超限事件。
- 命令、传感器和遥测分别使用规定的一致性发布规则。

### G5 基础有感 FOC

交付：

- current loop
- encoder sample publication
- sensor quality
- velocity loop
- position loop
- calibration state
- fault boundaries

验收：

- 有感电流、速度和位置控制在指定板卡和电机上完成有界验证。
- 不依赖 `motor_dev`、Shell 或 FreeRTOS。Legacy Device 可以继续通过 FreeRTOS 运行，但 Core API 不得暴露 RTOS 类型。

### G6 UART Protocol v1、参数和校准

交付：

- UART frame schema
- encoder/decoder
- host CLI
- command timeout
- status and fault frames
- parameter operations
- calibration jobs
- versioned persistence

验收：

- 不使用固件文本 Shell 完成启停、速度、电流和故障操作。
- 主机可以通过 UART 发现设备、读取能力、发送 setpoint、读取状态和清除故障。
- 连续数据流、损坏帧、重同步、RX 溢出、重复帧和超时都有确定行为。
- 更换电机或板卡后旧校准不会静默生效。

### G7 当前板实机复验

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

### G8 第二参考平台和后续 transport

第二个设备应故意选择与 AT32F415 不同的条件，例如：

- 不同 Cortex-M 主频
- 有 FPU 或不同数学路径
- 不同 ADC 触发机制
- 不同电流采样拓扑

验收：

- 核心 API 不因第二个平台增加条件编译污染。
- 两个平台都能运行相同的 core host model 和 protocol vectors。

### G9 开源 Beta

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
- Legacy FreeRTOS task ownership
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

第一轮只做 G0、G1 和 G2。完成 legacy 基线、硬件资源与时间事实、运行时 ADR 和最小 Core 后，再做 G3 当前板纵向链路。UART 协议在 Device mailbox 和状态快照稳定后进入，不提前把协议字段写进控制核心。

当前设计不会改变 live FalconFoc 启动链，也不会删除现有代码。旧实现只有在新核心、AT32F415 port、UART Protocol v1 和实机复验全部通过后，才进入删除或归档评审。CAN 2.0、CAN-FD、CANopen 和 EtherCAT 适配不属于当前板的 V2 基础验收。

2026-11-08 G0/G1 只读审计结果见 `docs/evidence/2026-11-08-*.md`。当前 Legacy 基线、host `45/45`、AT32F415 bench build、TMR/DMA/UART/IRQ 资源、H0/H1 所有权风险、UART 连续流风险和 G3-SAFE 前置条件均已记录。TMR2、SPI1 DMA 和 WDT/WWDT 仍是待决资源，不得在实施阶段自行占用。
