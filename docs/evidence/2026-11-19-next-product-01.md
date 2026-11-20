# NEXT-PRODUCT-01 · FalconFOC 实机 FOC 与 Servo 软件准备

2026-11-19

**PRODUCT-01 SOFTWARE READINESS PASS**  
**PRODUCT-01 HARDWARE GATES BLOCKED**

本轮完成 Host 三相提交模型、STOP/FAULT 交错测试、锁定态 GDB/DWT/SRAM 观测与校验程序。保持 H0/H1、软件 ADC trigger、SPI IRQ、现有采集结构和 `sched = KEEP_UNUSED`。P0 物理关断仍阻断，且现有通用 Pipeline 的同步 UPDATE 确认合同不能直接接延迟装载。没有部署或执行任何实板动作。 [锁定测量流程](../../firmware/at32f415/LOCKED_TIMING.md) · [测试与审核原始记录](2026-11-19-next-product-01/raw-results.zip) · [锁定 ARM 镜像](2026-11-19-next-product-01/locked-images.zip)

## 1. Git 与证据基线

工作树 `D:/study/Software/Hollow-Gimbal-vm01a`，分支 `codex/falconfoc-v2-vm01a`，HEAD `d824933946a365b8081089c2e396d77b3f661bc8`，本地跟踪引用 `34417392cd94932058e659442881a4bcc73eb695`，ahead 2。使用含 ARCH-00、RT-01～03、INTEG-01/02 的工作树。保留起始暂存区为空的状态、已有本地改动、历史证据、SDK、许可与发布状态。未提交或推送。 [项目纲领](../architecture/FalconFOC_Project_Charter_and_Engineering_Memory_v1.0.md) · [执行 ADR](../architecture/FalconFOC_BareMetal_Execution_ADR.md) · [INTEG-02](2026-11-19-next-integ-02.md)

## 2. 板端资源和真实环路缺口

现行 V2 启动先锁定，TMR1 是上数的 200 µs 逻辑槽。CH4 在 +40 µs 产生比较 IRQ，经 RT-01 Slot/Token 检查后写 `PCSWTRG`。ADC1 PCCE 后取四 Rank、调用 H0，再经 Core/Current PI/SVPWM、Runtime 最终 Guard、Board 锁定适配器。H1 每 500 µs 通过 TMR5/SPI1 采集和发布编码器。产品路径仍需真实采样窗、最终提交通路、合格装载生效和独立保护。 [实际入口](../../firmware/at32f415/src/main.c) · [Board Adapter](../../firmware/at32f415/src/foc_board.c) · [RT-01](2026-11-19-next-rt-01.md)

| 项目 | 源码 / 图纸裁决 | 物理裁决 |
| --- | --- | --- |
| 三相 PWM 基础 | `VERIFIED` 图纸与芯片复用。PA8/PA7 是 CH1/CH1C → INHA/INLA，PA9/PB0 是 CH2/CH2C → INHB/INLB，PA10/PB1 是 CH3/CH3C → INHC/INLC。Legacy 有中心对齐、预装载和互补配置参照。V2 锁定镜像把这些脚保持在模拟模式。 | `NOT VERIFIED` 实际板版次、六路电平、相线与极性。 |
| TMR1/ADC | `IMPLEMENTED` CH4 软件触发，Token 归属及 PCCE 排拒。TMR1 支持双向计数；未来需验证比较方向、峰谷/重复计数器、OVF/更新与实际装载的对应。 | `NOT VERIFIED` 居中 PWM 的唯一有效触发、采样孔径、装载时刻和干扰。 |
| Compare / preload / active | `IMPLEMENTED` Host 三相事务和装载模型。`MISSING` V2 真实 `C1DT/C2DT/C3DT` 写入、生产 load witness、延迟确认。 | `BLOCKED` 输出相关寄存器配置和生效。 |
| 驱动、死区与互补 | 图纸 U4 标为 DRV8311HRRWR，MODE/SLEW/GAIN 有硬件电阻设置。Legacy 的延时与电平选择只是历史参照，V2 没有导入。 | `NOT VERIFIED` 贴装档位、死区、互补电平、最小脉宽、CSA 建立时间和实际电流极性。 |
| Gate Disable、nFAULT、Break | `MISSING` 图纸中可独立操作的硬关断。nSLEEP 由 DCBUS 经电阻/钳位支路偏置，并经 R7 与 nFAULT 有被动连接；图中未见独立 MCU nSLEEP 控制，也未见 nFAULT 接到 MCU/TMR1 Break。默认 BRK 复用 PA6 已接 IA。DRV_3V3 还给 MCU 供电，改变 driver 睡眠路径会牵涉供电树。 | **P0 `BLOCKED`**。需实物核线并见证独立去能。 |
| ADC/编码器 | `IMPLEMENTED` 四 Rank、SPI 帧原始采集、编码器模式和估算。VBUS=PA1，IC=PA4，IB=PA5，IA=PA6；逻辑 A/B/C=IC/IB/IA。现有 Device 映射 `C1=C、C2=B、C3=A` 与图纸一致。 | `NOT VERIFIED` 真实增益、符号、电流零偏和机械/电角参考。 |
| 复位、驱动失效、WDT | `IMPLEMENTED` 锁定镜像在时钟/外设初始化前复位 TMR1 禁止输出，桥脚不进入 PWM 复用。`MISSING` 将 WDT、异常、CPU 停止及初始化故障绑定到独立实际关断。 | `NOT VERIFIED` 上电、棕断、调试停止、硬故障和 ADC IRQ 停止的关断及恢复。 |

原理图第 1、2、3 页分别核对 MCU、驱动和编码器。芯片资料核对 TMR1 章节 14.3、ADC 章节 18、QFN32 引脚。AT32F415 勘误 Rev 2.0.18 §1.8.2 说明 `TMREN=0` 时 Break 可能不能清除 `OEN`，没有列出 workaround。独立禁止须在计数器停止或 CPU 不响应时仍有效；外部驱动由自身供电和真实引脚条件决定。 [Board1 原理图](../pcb%20docs/pcb原理图.pdf) · [AT32F415 引脚数据手册](https://www.arterychip.com/download/DS/DS_AT32F415_V2.02_EN.pdf) · [AT32F415 参考手册](https://www.arterychip.com/file/download/2002) · [厂商勘误入口](https://arterychip.com/en/product/AT32F415.jsp) · [DRV8311 数据手册](https://www.ti.com/lit/ds/symlink/drv8311.pdf)

## 3. PWM Output Port 合同与实现

[Host 模型](../../firmware/at32f415/src/foc_board_pwm_model.c) 由 Board Host 的测试目标编译，ARM CMake 分支不链接。它复用 `foc_device_map_output` 的完整浮点与三相映射校验，以及 RT-02 的 `foc_board_shadow_check_timing`。本轮只从 RT-02 既有 stage 操作提取纯检查，原 stage/load 语义、生产输入和外设路径继续沿用。

| 事务阶段 | 本轮实现与真实绑定 |
| --- | --- |
| 默认禁止 | 模型零初始化后拒绝接受请求。恢复要求新的合法 Session 和独立合格信号，旧会话的写/装载请求失效。 |
| `UPDATE_ACCEPTED` | H0 交付已通过最终 Guard 的完整请求，保留 Session、Submission Sequence、Command Sequence、Sample Slot 与 Generation。每组重新映射并核对三相 compare，结合采样窗、时间顺序与 deadline。 |
| `SHADOW_WRITTEN` | 明确观测依次完整写入 C1/C2/C3，任一错误、部分写入、失序和写入越过 setup fence 均撤销。Host 内部 Shadow 与 Active 采用不同数组。真实板必须证明所有写入在共同的装载期限之前完成，或先隔离装载及输出并使用有证据的硬件合同；不能凭一组连续 MMIO 写认定原子。 |
| `PWM_ACTIVE_APPLIED` | 模型仅在看到指定 Load、完整写集和最新 Guard 仍通过时更新 Host Active，状态叫 `MODEL_ACTIVE`。实际端需要与同一身份绑定的有效装载见证和结果确认，同时捕获意外峰/谷装载。单读预装载值无法确认 Active。 |
| `PHYSICAL_TORQUE_PRODUCED` | 仅通过授权下独立电流、转矩或运动证据裁定。 |

**P1 量产接口缺口。** `foc_pipeline_output_submit_t` 的同步布尔 ACK 只在 UPDATE 已应用时有效，并决定 Current PI 的试算状态提交。真实 Shadow 路径需保留 H0 唯一逻辑所有者，在提交与 load 回执之间持有至多一个带身份的待确认事务。H0 消费匹配回执后提交 PI，STOP、FAULT、错过装载、错误顺序和下一输出不合法时丢弃试算，撤销旧会话，并阻断新功率动作。不要直接把 Shadow 中间态接入现有布尔回调。批准生产端适配后首先验证“迟到/丢失回执不会提交积分”的回归，再校准寄存器与硬件见证。 [同步回调源码合同](../../runtime/include/foc_pipeline.h) · [RT-02 装载合同](2026-11-19-next-rt-02.md)

## 4. P0 故障、关断与恢复合同

| 触发 | 当前软件/Host 行为 | 需要在设备独立验证的动作及恢复 |
| --- | --- | --- |
| 上电/复位/调试/异常 | 锁定启动、时钟失败、ADC 失败时无 PWM 使能；默认异常入口不能承担物理关断。 | 六路输入、驱动状态、供电与输出初始安全；先禁止，后判身份；重新审批后才恢复。 |
| OCP/可疑电流、OV/UV、Driver Fault | VBUS 及输入有效性可引起软件 INHIBIT。当前 V2 无已证实的独立驱动故障输入/硬关断。 | 外部供电与驱动保护独立动作，确认故障输出、能量路径、门极/输出效果和故障持续；清除根因、锁存及显式重臂。 |
| Encoder/ADC 无效、H0 deadline/ADC IRQ 消失 | 既有 Safety、快速/监督心跳、Token 拒绝、STOP 请求；Host 注入验证卡在任一 Shadow 状态可撤销。 | 计数器停止、CPU ISR 不执行、采样失效时仍须在已量测时间内独立禁止。确认恢复时旧 Shadow 不会生效。 |
| WDT、HardFault、棕断 | Host 可请求故障；锁定 ARM 没有把这些状态绑定到已验证的实际 Gate Disable。 | 测实际复位到禁止和电源跌落/恢复，复位后回到默认禁止；保留故障记录，再显式重臂。 |
| STOP / Fault Latch / Explicit Rearm | 现有 Runtime 撤销，模型清空 Shadow/Active，拒绝旧 Session；任何迟到/重放 Load 都无法恢复模型输出。 | 独立去能先于软件状态确认，证实停止后的端口和桥输出，再审核是否允许新的 Session。 |

复位、Break、Gate 和实际输出状态均 `HARDWARE NOT VERIFIED`。零 duty、计数器关闭或仅向软件提交 INHIBIT 无法证明桥端物理去能。

## 5. 参数与频率晋级

- 参数准入为 **`CALIBRATION_REQUIRED` / `REFERENCE_REQUIRED` / `PARAMETER_NOT_VERIFIED`**。随仓库的 2804 规格标称 7 极对、0.5 A 额定、2 A 最大，型号/装机一致性待核。锁定固件当前 2.5 A 软件参数甚至高于该文档的标称最大值，禁止直接继承到实机配置。DRV 模式、实际 VM 范围、相电流映射/极性和 ADC 比例零偏须复核。
- KTH7823 的 65536 CPR、方向 -1、7 极对和零偏置是当前参考设置；真实 Motor/Encoder/Device/Frame 身份、机械零位和电角偏置均待见证。实际 Kt 需有能追溯的模型或测量，规格表 Ke 和通用 Host 参数不可充当合格的替代。PI 按实测 R/L/延迟和限流制定，外环限位、速度、加速度与电流/力矩边界均须按实际机构审查。
- 已有 `foc_board_init_with_calibration()` 负责显式应用已认可快照，生产锁定 `main` 使用默认初始化。复用 SOFT-05/06 的离线验证、候选审核与禁能初始化；尚缺板件、标定快照与产品配置/Runtime Session 联动的实际接入。当前 ADC_ZERO Host 阶段只证明诊断链。 [2804 规格](../pcb%20docs/2804电机参考手册.pdf) · [显式标定初始化](../../firmware/at32f415/README.md#explicit-calibrated-initialization) · [Measurement Trace](../../calibration/MEASUREMENT_TRACE.md)

| PWM / Current | 所需设计证据 |
| --- | --- |
| 20/20 kHz | 首选。50 µs 载波要量得最坏有效窗、真实采样和完整提交余量。假设 SOC +40 µs、转换 6.4 µs、setup 2 µs，下谷预算仅 1.6 µs。提前至 +10 µs 的算术余量为 31.6 µs，仍需证明三相低侧和 CSA 建立窗。 |
| 10/10 kHz | 备选。相同假设及 +10 µs 时，下谷算术余量 81.6 µs；重新验证采样、控制质量和保护。 |
| 20/10 kHz | 分频对照。若同样在当前采样槽的下谷生效，单拍写入期限与 20/20 相同；第二圈保持 Active 需明确计划和失效保护。 |

四 Rank 以名义 12.5 MHz ADC 时钟形成 6.4 µs 转换组，每 Rank 1.6 µs，孔径 0.6 µs。三相各自审核死区、开关瞬态、CSA/RC 建立、占空比和误差。采样与装载测量、总线/软浮点成本与干扰通过之后才讨论参数切换。本轮保持 5 kHz 逻辑 H0 及当前软件触发。 [RT-02](2026-11-19-next-rt-02.md) · [RT-03](2026-11-19-next-rt-03.md)

## 6. 目标板测量准备与 ARM 资源

新增 [GDB 导出命令](../../tools/falconfoc-calibration-replay/locked_timing_export.gdb) 和 [离线校验](../../tools/falconfoc-calibration-replay/locked_timing.py)。核对正向与中断/无 DWT/倒退/伪造 INHIBIT 日志，报告 CH4、PCCE、H0/H1、TMR5/SPI/USART、前台、Board Adapter 与 Runtime 期限计数。可读取冻结 Capture、DWT 窗口内观测最大值和独立栈染色数据。没有采样的点明确缺失；最新 span、窗口最大值及 WCET 分开。GDB 命令使用本机进程实跑，ARM DWARF 结构离线解析；Host 进程观察确认全段结束标记与只读解析，不涉及实板。现场流程保留最差路径见证和外部仪表任务。 [操作手册](../../firmware/at32f415/LOCKED_TIMING.md)

| Capture | DWT | Flash | SRAM | 静态栈估算 | 链接栈余量 |
| --- | --- | ---: | ---: | ---: | ---: |
| OFF | OFF | 43,836 B | 4,176 B | 3,456 B | 29,620 B |
| OFF | ON | 44,948 B | 4,592 B | 3,512 B | 29,204 B |
| ON | OFF | 45,048 B | 9,688 B | 3,488 B | 24,108 B |
| ON | ON | 46,224 B | 10,104 B | 3,544 B | 23,692 B |

ARM GNU 14.3.1、Thumb、Cortex-M4 soft-float、`-Os`，带 DWARF 的四组 BIN 与 INTEG-02 对应构建逐字节相同。模型符号完全留在 Host。Flash/SRAM 及 4 KiB 链接栈门槛通过；栈数字来自 `.su` 和已解析 ARM 调用链，物理高水位未测。既有 newlib stub、RWX 链接警告留存。第一次把 `-g` 写进 `CMAKE_C_FLAGS` 覆盖了工具链 CPU flags，构建触发 lock-free 断言；改用 `CMAKE_BUILD_TYPE=Debug` 保留 Cortex-M4 参数后四组通过。最初的 GDB 现场日志模板因宏参数拆词提前终止，本地 Host 复现后改为独立求值并复验。 [原始构建与观测日志](2026-11-19-next-product-01/raw-results.zip) · [锁定镜像](2026-11-19-next-product-01/locked-images.zip)

## 7. 回归与独立复核

| 通道 | 本轮结果 |
| --- | --- |
| Clang Board Host / MinGW GCC / Clang Board ASan+UBSan | 各 9/9 CTest。包含 PWM Host、离线时序 6 例、采集导入 30 例、UART V1 8 例。 |
| Host 新事务 | 完整三相写入、比较值错配、非法 NaN 和 action、错 Session/Token、回绕、乱序、错/晚 Load、部分写入、STOP/FAULT 在写中和 Load 前、老 Session 撤销与重臂通过。实际 Runtime `foc_runtime_request_inhibit` 的 STOP 与 fault 原因经 Host Guard 输入拒绝模型 Load。 |
| ARCH-00 / RT-01～03 / INTEG-01/02 | Board 时间模型和 IRQ 合同、Slot/Token、SPI 时序与抢占、Servo 生命周期、采集/Trace 回归通过。历史证据 SHA-256 和四组 ARM 基线对比通过。 |
| Core / Runtime / Sensor / Safety / Device NP / Protocol | 20/20 · 13/13 · 2/2 · 1/1 · 2/2 · 1/1。 |
| Virtual Motor / Servo / Control Plane | 35/35 · 7/7 · 5/5。 |
| Calibration / SOFT-05 / SOFT-06 | 8/8 · 1/1 · 1/1。 |
| Source Contract、严格 `Scope all`、Legacy Host | 全部通过，`RequireCMakeFormatter`、45/45。 |

复核分别核对原理图与引脚路由、Host 事务不变量、CMake ARM 隔离、原始记录和 GDB 读写范围。对三个隔离变异主动绕过 Guard、允许部分装载、放过读取比对，均被测试拒绝。复核资料来自本轮代码、独立工具和用例交叉检查；实际板件由现场见证。 [复核与变异输出](2026-11-19-next-product-01/raw-results.zip)

## 8. 分级验收与下一项工程操作

| 阶段 | 现状和进入条件 |
| --- | --- |
| 软件就绪 | **PASS**。Host 模型和数据检查程序可运行，锁定 ARM 和严格回归通过。 |
| BL1/BL2 锁定态 | **BLOCKED**。先核对实物 PCB、电源树和独立去能，授权后选用电机三相断开、功率独立隔离的台架。取回两个匹配镜像下的时间/栈数据与复位、Gate/Break、ADC/Encoder 的外部仪器见证。 |
| BL3/BL4 功率与 Current | **BLOCKED**。完成独立物理安全审查，再申请安全配置、带功率测试和电流闭环的逐项授权。需要真实 R/L/电流/角参考、物理截止、Id/Iq 跟踪、限流、过冲、稳态误差、PI 饱和恢复和保护时延证据。 |
| Velocity → Position → Impedance | **BLOCKED**。电流环和 Sensor 时间合同通过后，逐级核对负载扰动下的跟踪/稳态误差、峰值过冲、恢复时间、饱和占比、实际频率、Sensor 年龄、期限违例、故障恢复、机械限位和资源。用现有 Core、Runtime、Servo、Control Plane；有合格激励与仪器后可另做电流环频响和相位裕量。 |

分级方法参考 TI 的 incremental build 与 SFRA，具体放行级别来自本项目。当前保留 `G3-SAFE HARDWARE NOT VERIFIED`、`G3-PWR LOCKED`、`AT32F415 WCET NOT VERIFIED`、`MOTOR PHYSICAL OPERATION UNVERIFIED`、`REAL BOARD CAPTURE NOT VERIFIED`、`REAL DATA REPLAY NOT VERIFIED`。 [TI 分级与 SFRA](https://www.ti.com/tool/TIDM-02007)

**最有价值的下一步是完全断电核对实物 PCB 的关断线路。** 先确认 U4、nSLEEP、nFAULT、PA6/Break 和 MCU 供电的实际连通性、布板版次和独立去能条件，结论交硬件负责人。通过核线后，最小新增授权是对断开三相且功率独立隔离的锁定台架进行受限供电、批准的锁定镜像部署、只读 SWD、INTEG-02 采集邮箱与测量用 SRAM 操作。任何 Gate/Break 修改、运动、PWM 解锁和负载试验另行授权。本轮结束在该物理门禁。