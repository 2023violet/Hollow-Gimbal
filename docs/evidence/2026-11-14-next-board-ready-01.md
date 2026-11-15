# FalconFOC V2｜NEXT-BOARD-READY-01 AT32F415 实板验证准备与硬件安全准入

## 结论

`NEXT-BOARD-READY-01 PREPARATION PASS`

本轮已经形成一套基于当前源码、当前 ARM 产物和仓库硬件资料的可执行实板验证方案。准备通过只表示：运行架构、资源口径、测量点、门禁顺序、缺失条件和停止边界已经明确；不表示已经刷机、上电、测得 WCET、验证功率级或运行电机。

以下状态继续保持：

- `G3-SAFE HARDWARE NOT VERIFIED`
- `G3-PWR LOCKED`
- `AT32F415 WCET NOT VERIFIED`
- `PWM OUTPUT LOCKED`
- 未连接真实调试器、未刷机、未上电、未启用 PWM、未运行电机。

证据范围：当前工作树 `codex/falconfoc-v2-rc0`、HEAD `46bc963f5e5849e4440d1714a845fe485d338837`。工作树已有的未提交修改和未跟踪文件均保留。本轮只读源码、构建产物和硬件资料，未修改生产代码或硬件安全路径。

## 1. 证据等级

- `VERIFIED`：当前源码、构建产物或仓库文件可以直接证明。
- `REQUIRES MEASUREMENT`：必须接触实际板卡、仪器或功率级才能证明。
- `NOT IMPLEMENTED`：当前软件链没有实现，不能设计成“已存在”的测量入口。
- `UNKNOWN`：仓库资料不足，必须由板卡或操作者补充。

## 2. 真实运行架构

### 2.1 Legacy Firmware

状态：`VERIFIED`，但其实时证据仍需实板测量。

实际启动链为：

```text
AT32F415 generated main
  -> wk_* peripheral bring-up
  -> app_main_bootstrap
  -> bsp_init / export stages
  -> osKernelInitialize / osKernelStart
  -> motor_task_init / shell_task_init / motor tasks
```

关键执行路径：

| 路径 | 当前源码事实 | 解释 |
| --- | --- | --- |
| TMR1 CH4 | `ARR=3749`，150 MHz timer，双向计数，约 20 kHz carrier/ADC event | `TMR1_CH_IRQHandler` 只软件触发 ADC injected group |
| ADC1 completion | `ADC1_IRQHandler` 读取 VBUS、IC、IB、IA，执行快速故障检查 | 产品镜像 `TM_MOTOR_FOC_ADC_DIVIDER=2`，FOC step 名义 10 kHz；bench 镜像为 5 kHz |
| TMR5 overflow | `ARR=1999`、divider 14，约 5 kHz | `THEMIS_PLATFORM_MOTOR_SOFTIRQ_HANDLER` 读取编码器并运行外环 |
| Position loop | `POS_LOOP_DIVIDER=5` | 由 5 kHz 外环分频到名义 1 kHz |
| Trajectory/input task | `motor_task` 中 `osDelay(1)` | 名义 1 kHz，但实际受 FreeRTOS 调度影响，不能当作硬实时定时器 |
| Velocity loop | `foc_outer_loop_step()` 每次 TMR5 soft IRQ 执行 | 名义 5 kHz |
| RTOS | FreeRTOS ARM_CM3 port，soft-float ABI，1 kHz tick | 任务执行与 ISR 共享同一 MCU，但不与 V2 Reference 的 H0/H1 API 复用 |

Legacy 的 `cortex-m3-soft` 是当前 FreeRTOS/工具链 CPU class 选择，不能把它写成 AT32F415 硅片型号。V2 独立 CMake 目标明确要求 `MCU_CPU=cortex-m4`、`MCU_FPU=none`、`MCU_FLOAT=soft`；两条固件都使用 soft-float，但 CPU 指令集/FreeRTOS port 选择不同。这是当前真实构建边界，不能用“Cortex-M4”或“ARM_CM3”其中一个标签覆盖两条链。

Legacy 的 `applications/algo/foc_config.h` 仍拥有 `150 MHz`、`10 kHz current`、`5 kHz velocity`、`1 kHz position` 和 `1 kHz trajectory` 的产品时序常量。TMR1 的真实 carrier 约 20 kHz，不能把旧宏名 `PWM_FREQ_HZ` 直接解释成硬件 carrier。

### 2.2 V2 Reference Firmware

状态：`VERIFIED`，V2 的 H0/H1 已经绑定真实 AT32F415 中断；输出和功率级仍锁定。

V2 是独立 CMake 目标，使用自己的 `firmware/at32f415/src/main.c`，不链接 Legacy FreeRTOS。它与 Legacy 共用 AT32F415 vendor/startup/linker 资源，但不共用 Legacy 的 `app_main`、FreeRTOS 任务入口或生成式 Legacy 外设配置。

实际路径为：

```text
V2 main
  -> foc_at32_lock_output
  -> 150 MHz clock / ADC / SPI1 / TMR2 / TMR1 / TMR5 init
  -> NVIC enable
  -> foreground polling loop

TMR1 CH4 (+40 us in a 200 us period)
  -> ADC software trigger
  -> ADC1 completion IRQ
  -> foc_board_adc_complete
  -> foc_runtime_h0_step
  -> Core / Device / Safety software path
  -> foc_board_output_submit (UPDATE remains NOT_APPLIED)

TMR5 overflow (500 us)
  -> SPI1 encoder transaction start
  -> SPI1 IRQ receives word
  -> TMR5 CH2 closes bounded window
  -> foc_board_encoder_complete
  -> foc_runtime_h1_step

Foreground
  -> foc_runtime_foreground_poll
  -> foc_serial_poll
```

| V2 域 | 当前事实 | 状态 |
| --- | --- | --- |
| H0 nominal period | TMR1 period `200 us`，ADC trigger at `+40 us` | `VERIFIED` source mapping |
| H1 nominal period | TMR5 period `500 us`，SPI send `+20 us`，window close `+70 us` | `VERIFIED` source mapping |
| Common time base | TMR2 free-running 32-bit counter，1 µs/count | `VERIFIED` source configuration；实际时钟精度待测 |
| Foreground | `for (;;)` polling，不是独立硬件定时器或 RTOS task | `VERIFIED` |
| PWM | TMR1 output mux/channel/MOE 未启用；bridge pins 被置为 analog | `VERIFIED` locked reference behavior |
| H0/H1 hardware scheduling | 已实现于 Reference Firmware | `HARDWARE SCHEDULING IMPLEMENTED` |
| Physical output update | `foc_board_output_submit()` 对 UPDATE 返回 `NOT_APPLIED` | `NOT IMPLEMENTED` physical apply path |

因此，Host 中的 H0/H1 重放周期不能直接当成 V2 的实板周期；本轮可以证明 V2 参考固件存在相应中断入口和时序配置，但不能证明中断延迟、执行预算、ADC 建立时间或实际 PWM 响应。

## 3. ARM 资源统计统一口径

本轮重新执行了：

```text
cmake --build build/rc0/baseline-arm --parallel 4
cmake --build --preset build-fw-tiny-carrier-minsizerel --parallel
cmake --build --preset build-fw-standard-carrier-minsizerel --parallel
cmake --build --preset build-fw-full-diag-carrier-debug --parallel
```

四个构建均返回 `0`，没有修改源码或重新生成生产配置。

### 3.1 V2 ELF section

工具：Arm GNU Toolchain `14.3.Rel1 / 14.3.1`、`arm-none-eabi-size -A`、`readelf -S/-l`、`objdump`、MAP。

| Section | Size | 地址/含义 |
| --- | ---: | --- |
| `.isr_vector` | 368 B | Flash `0x08000000` |
| `.text` | 34,916 B | Flash code |
| `.rodata` | 1,352 B | Flash constants |
| `.ARM` | 8 B | exception index |
| `.init_array` + `.fini_array` | 8 B | startup arrays |
| `.data` | 84 B | RAM VMA `0x20000000`，Flash LMA `0x08008f2c` |
| `.bss` | 2,520 B | RAM `0x20000058..0x20000a30` |
| `._user_heap_stack` | 1,024 B | linker-reserved stack, heap reserve `0` |
| `.noinit` | absent | no separate noinit section |

### 3.2 计算口径

- Flash 实际镜像使用量不是 ELF 文件大小，也不是调试 section 总和。
- `falconfoc-v2-at32f415.bin` 长度为 `36,736 B`，等于 Flash 中可加载的向量、代码、只读数据、ARM/init sections、对齐和 `.data` 初始值的连续范围。
- `readelf -l` 的 Flash LOAD 包含 `.isr_vector/.text/.rodata/.ARM/.init_array/.fini_array`；`.data` 的初始值在 Flash LMA 中另有一段。两者合并后得到 BIN 长度 `36,736 B`。
- RAM 静态对象为 `.data + .bss = 84 + 2,520 = 2,604 B`。
- Linker 还保留 `1,024 B` 的 `._user_heap_stack`；`_Min_Heap_Size=0`，所以它不是可用 heap。
- 加上对齐后，linker memory report 为 `RAM 3,632 B / 32 KB`。这个值包含静态对象、保留栈和必要对齐，不能再把 `.bss` 或 `.data` 重复加一次。
- ELF 文件大小 `65,824 B` 包含 debug sections，不能作为 Flash 烧录占用。
- Linker `ASSERT(_estack - _ebss >= 4096)` 只证明静态数据末端到栈顶至少保留 4 KiB headroom，不证明实际运行栈高水位。

### 3.3 当前产物

| 产物 | SHA-256 |
| --- | --- |
| `falconfoc-v2-at32f415.bin` | `9806ADBFA3282450527F40BB97CB1DA75FAFE41FB405F1126114187C6393B029` |
| `falconfoc-v2-at32f415.elf` | `51D7428FF2414FA4623DCE5086A6CD8D4F69058DFC6405B317DC28875E281551` |
| `falconfoc-v2-at32f415.hex` | `B80C263E6B07CE976E2BF1AEDED2737EED536C7224369091DC0919CAA48C51AE` |
| `falconfoc-v2-at32f415.map` | `BDC648824C427C873259638A30EB2FAE0C59E1598EBEA3C2C43E0C29CF6516B3` |

## 4. 静态调用和栈边界

V2 ELF 中存在以下真实入口和中断符号：`Reset_Handler`、`main`、`TMR1_CH_IRQHandler`、`ADC1_IRQHandler`、`TMR5_GLOBAL_IRQHandler`、`SPI1_IRQHandler`、`USART1_IRQHandler`。`SysTick_Handler`、`PendSV_Handler`、`SVC_Handler` 仍为 weak 默认符号，V2 目标没有启用 FreeRTOS。

当前 `.su` 静态栈帧：

| 函数 | 静态栈帧 |
| --- | ---: |
| `foc_current_step_internal` | 184 B |
| `foc_pipeline_step_guarded` | 312 B |
| `foc_runtime_control` | 224 B |
| `foc_runtime_h1_step` | 288 B |
| `foc_runtime_h0_step` | 96 B |
| `foc_board_output_submit` | 96 B |
| `foc_encoder_update` | 72 B |
| `foc_trajectory_velocity_step` | 当前 V2 target 未单独进入独立函数调用链，不能从 Host 结构体大小推导 |
| `foc_trajectory_position_step` | 当前 V2 target 未单独进入独立函数调用链，不能从 Host 结构体大小推导 |

不能将这些数值相加作为最大栈消耗。实际最大值还取决于调用链、编译器寄存器保存、库函数、ISR 嵌套和异常入口压栈。

Legacy 另外使用：

- MSP：异常和 ISR 栈；
- PSP：FreeRTOS 任务栈；
- `motor_task`：`2048 B`；
- `motor_safe`：`1024 B`；
- shell/VOFA/app shell：各自配置的 FreeRTOS 栈；
- FreeRTOS heap：`10 KiB` 配置上限；
- `configCHECK_FOR_STACK_OVERFLOW=2` 和 `uxTaskGetStackHighWaterMark` 已启用。

当前没有任务栈高水位、MSP 峰值或 ISR 嵌套实测，所以 `AT32F415 WCET NOT VERIFIED` 必须保持。

## 5. NVIC / FreeRTOS 抢占矩阵

### 5.1 Legacy

源码配置为 NVIC priority group 4，4 个 preemption bits；FreeRTOS `configPRIO_BITS=4`、`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY=5`、`SysTick/PendSV=15`。

| 来源 | 静态优先级 | 角色 | 是否允许使用 RTOS API |
| --- | ---: | --- | --- |
| `TMR1_CH_IRQn` | 4 | 采样触发 | 不应调用 RTOS API |
| `ADC1_IRQn` | 5 | ADC 完成、FOC current path | 处于 syscall 边界；当前路径不应依赖普通 task API |
| `TMR5_GLOBAL_IRQn` | 6 | 编码器/外环 | 低于实时控制优先级；当前路径不应直接调用普通 task API |
| `USART1_IRQn` | 7 | Shell/DMA transport | 通过 CMSIS/ISR-safe 机制通知 shell，需实板确认最终 port 行为 |
| `USART2_IRQn` | 8 | 备用串口 | 当前产品路径低于控制 ISR |
| `SysTick_IRQn` | 15 | FreeRTOS tick | 最低系统优先级 |
| `PendSV_IRQn` | 15 | FreeRTOS context switch | 最低系统优先级 |

源码优先级是静态配置事实；FreeRTOS port 初始化后最终寄存器值仍应在实板 H0 资料门禁中用调试器或寄存器 dump 复核。

### 5.2 V2 Reference

V2 没有 FreeRTOS，`foc_at32_irq_enable()` 设置 priority group 0，四个 preemption bits：

| 来源 | 静态优先级 | 抢占关系 |
| --- | ---: | --- |
| TMR1 CH4 / ADC1 | 0 | 最高；两者同优先级，硬件按挂起顺序串行 |
| TMR5 GLOBAL / SPI1 | 1 | 可被 H0 抢占 |
| USART1 | 2 | 可被 H0/H1 抢占 |
| SysTick/PendSV/SVC | weak/default，未启用 RTOS | 不属于 V2 运行调度 |

### 5.3 H0/H1 抢占矩阵

| 正在执行 | 可能抢占它的域 | 当前证据 |
| --- | --- | --- |
| USART1 foreground/IRQ | TMR1/ADC、TMR5/SPI | source priority |
| H1 TMR5/SPI | TMR1/ADC | source priority |
| H0 TMR1/ADC | 无更高 V2 peripheral IRQ | source priority；异常/调试事件仍需硬件确认 |
| V2 foreground loop | 所有已启用 IRQ | source priority |

## 6. ADC / SPI / PWM 时间关系

### 6.1 V2 ADC/H0

```text
TMR1 period = 200 us
TMR1 CH4 compare = +40 us
  -> TMR1_CH_IRQHandler
  -> ADC1 software injected trigger
  -> ADC injected ranks: VBUS, IC, IB, IA
  -> ADC1_IRQHandler
  -> timestamp at ADC IRQ
  -> foc_board_adc_complete
  -> foc_runtime_h0_step
```

当前 ADC 为 software trigger，rank 顺序为：

1. PA1 / ADC1_IN1 = VBUS
2. PA4 / ADC1_IN4 = driver IC → logical A
3. PA5 / ADC1_IN5 = driver IB → logical B
4. PA6 / ADC1_IN6 = driver IA → logical C

ADC clock divider、sample time 和四 rank 转换时间来自 vendor 配置；实际 ADC end-of-conversion 时刻、采样孔径和 CSA 建立裕量仍需示波器/逻辑分析仪或 DWT + GPIO 证据。当前软件没有把 ADC complete timestamp 当成模拟采样时刻，这是正确的边界。

### 6.2 V2 Encoder/H1

```text
TMR5 overflow every 500 us
  -> assert PA15 CS / begin encoder transaction
TMR5 CH1 +20 us
  -> transmit one zero word on SPI1
SPI1 IRQ
  -> capture 16-bit word and source receive time
TMR5 CH2 +70 us
  -> close bounded window
  -> foc_board_encoder_complete
  -> foc_runtime_h1_step
```

SPI1 为 mode 3、16-bit、MSB-first、software CS，PB3/PB4/PB5 分别为 SCK/MISO/MOSI。当前编码器无 wire CRC；传输错误、超时、连续性和时间有效性由软件质量状态处理。

### 6.3 PWM / Device

V2 当前 `foc_at32_lock_output()` 将 PA7/PA8/PA9/PA10 和 PB0/PB1 置为 analog，并复位 TMR1；`foc_at32_timers_init()` 只使用 TMR1 CH4 compare，不配置六路 PWM output/mux/MOE。`foc_board_output_submit()` 接受软件 INHIBIT，但 UPDATE 永远返回 `FOC_BOARD_UPDATE_NOT_APPLIED`。

因此：

- ADC → Core → Safety 软件链：`VERIFIED` source/Host boundary；
- PWM compare 写入真实 TMR1：`NOT IMPLEMENTED` in V2 reference;
- compare 锁存、DRV8311H 输入波形、死区、功率级响应：`REQUIRES MEASUREMENT`；
- 当前不存在“软件输出结果到真实桥臂响应”的可测量闭环。

## 7. H0/H1 实际 WCET 测量设计

### 7.1 测量工具优先级

1. **DWT cycle counter（首选候选）**：在 AT32F415 Cortex-M4 上验证 DWT/DEMCR/CYCCNT 可用性后，在临时测量分支对 ISR 入口/出口采样。当前没有把 DWT 可用性当成已证明事实。
2. **GPIO 边沿 + 逻辑分析仪（独立交叉验证）**：仅使用原理图确认的安全测试点或专用测试焊盘；不得复用六路 PWM、ADC 或编码器引脚。
3. **Timer capture**：需要确认可用 timer channel、引脚复用和不会改变控制链；当前没有直接可用的已授权 capture 入口。
4. **调试器 trace**：取决于实际调试器、芯片 trace 引脚和工具链；当前仓库只有 OpenOCD/DAP-Link 配置，不能假定 SWO/ETM 已接出。

### 7.2 H0 测量点

应分别记录以下时间戳，而不是只包住 `foc_runtime_h0_step()`：

| 点 | 含义 |
| --- | --- |
| `T0` | TMR1 CH4 compare 事件对应的 TMR2/DWT 时间 |
| `T1` | TMR1 ISR 实际入口 |
| `T2` | ADC software trigger 写入点 |
| `T3` | ADC IRQ 入口 |
| `T4` | ADC rank 读取完成 |
| `T5` | H0/Pipeline 入口 |
| `T6` | Safety gate 返回 |
| `T7` | H0 结束 |
| `T8` | 未来真实 PWM compare 提交点；当前为 `NOT IMPLEMENTED` |

可计算：ISR entry latency `T1-T0`、trigger-to-ADC IRQ `T3-T2`、sample-read overhead `T4-T3`、H0 execution `T7-T5`、sample-to-control completion `T7-T2`。当前不能计算真实 actuator response `T8-T7`，因为输出锁未解除且没有 PWM 提交点。

### 7.3 H1 测量点

记录：CS assert、SPI transmit、SPI receive IRQ、TMR5 close、encoder decode、H1 entry/exit、H1 publication sequence、H0 consume sequence。重点计算：SPI transaction duration、H1 execution、source-to-publication age、publication-to-H0-consume age。

### 7.4 统计和判定

每种工况至少保存：样本数、最小值、平均值、P95、P99、Observed Maximum、中断抢占次数、missed deadline、late deadline、invalid sample、fault count。

当前软件配置中的候选预算是：H0 budget `100 us` / max delay `150 us`，H1 budget `150 us` / max delay `150 us`，Foreground budget `150 us`。这些是现有 Runtime policy 的起始参数，不是硬件通过门槛。实测最大值只能写 `Observed Maximum`，不能写成数学意义的 `Proven WCET`。

工况至少包括：禁用输出空载、有效 ADC/编码器、命令刷新、饱和/限幅、传感器超时、串口连续输入、H0 与 H1 相互抢占、Foreground 高负载、故障锁存和恢复尝试。

## 8. Board1 输出安全边界

| 项目 | 当前判断 |
| --- | --- |
| V2 软件 UPDATE lock | `VERIFIED`，Board adapter 返回 `FOC_BOARD_UPDATE_NOT_APPLIED` |
| V2 软件 INHIBIT | `VERIFIED` Host/source；只表示软件动作被接受 |
| ZERO_DUTY / ACTIVE_BRAKE | `VERIFIED` locked result；不表示真实桥臂动作 |
| TMR1 MOE/PWM mux in V2 | `VERIFIED` 未启用 |
| DRV8311H nSLEEP | `UNKNOWN/REQUIRES MEASUREMENT`；硬件资料显示不是 MCU GPIO 控制 |
| DRV8311H nFAULT | `UNKNOWN/REQUIRES MEASUREMENT`；硬件资料显示没有 MCU 输入闭环 |
| Watchdog | V2 Reference 当前未启用；`NOT IMPLEMENTED` physical watchdog evidence |
| Fault latch | Host/Safety `VERIFIED`；功率级物理锁存 `REQUIRES MEASUREMENT` |
| ADC/Encoder/Bus fault | 软件质量路径 `VERIFIED`；真实传感器和功率级响应待测 |

软件 `INHIBIT` 不能写成“桥臂已经关闭”。必须将 software decision、MCU pin state、DRV8311H input state、OUTA/B/C waveform 和母线电流分别取证。

## 9. 分阶段硬件准入

### GATE H0｜硬件资料一致性

- 前置：确认原理图、PCB、BOM、MCU 料号、DRV8311H 型号、编码器型号和装配版本。
- 设备：资料包、版本记录；不需要上电。
- 检查：ADC rank/极性、PWM 六路映射、SPI1 引脚、nFAULT/nSLEEP、供电树、急停和功率地。
- 通过：资料版本一致，源码映射逐项闭合；文档中的 DAP-Link/J-Link、COM3/COM54 冲突先被解决。
- 失败停止：任何引脚、版本或保护链不一致，禁止 H1。
- 证据：版本清单、原理图页码、网表/BOM 交叉表、源码 commit/worktree fingerprint。

### GATE H1｜无功率输出基础验证

- 前置：H0 通过；V2 仍保持输出锁；电机和功率母线不接入或按批准的无功率方式隔离。
- 设备：调试器、示波器/逻辑分析仪、3.3 V/低压供电和复位控制。
- 检查：复位、时钟、UART、TMR2 计时、TMR1/ADC/SPI IRQ 进入、六路 PWM 引脚保持禁止状态。
- 通过：无 PWM toggling、无意外桥臂使能、IRQ 时间戳可记录、故障状态可读。
- 失败停止：任一 PWM/DRV 输入出现非预期活动、复位异常、供电异常或 UART 失控。
- 证据：电源/复位波形、引脚静态电平、IRQ trace、串口日志。

### GATE H2｜实时性验证

- 前置：H1 通过；测量 instrumentation 单独授权并可撤回。
- 设备：DWT 或逻辑分析仪、调试器、必要的 GPIO 测试点。
- 检查：H0/H1 entry latency、execution、preemption、P95/P99/Observed Maximum、MSP/PSP/任务栈高水位。
- 通过：实际观测值在已批准预算内，零未解释 deadline violation；结果标记为 observed，不冒充 WCET。
- 失败停止：出现 missed/late、栈越界、不可解释的抢占或 timer drift。
- 证据：原始 trace、统计脚本、工具版本、镜像 SHA、时钟条件。

### GATE H3｜传感器和采样时序

- 前置：H2 通过；先保持功率输出锁，编码器和 ADC 使用可控输入。
- 设备：逻辑分析仪/示波器、可控编码器或手动转动工装、低压供电；独立电流测量在需要时加入。
- 检查：SPI CS/SCK/MISO/MOSI、ADC trigger/complete、采样窗口、VBUS/CSA、时间戳、方向、极对数和 PWM 更新关系。
- 通过：rank 顺序、时间戳和编码器序列与资料一致；采样窗口和 CSA 建立时间有波形证据；不存在未解释的丢样/回绕/未来样本。
- 失败停止：任一 rank/极性/方向错误、ADC/SPI 超时、采样处于不可解释噪声窗口。
- 证据：原始波形、原始 SPI/ADC 记录、解析脚本、校准参数身份。

### GATE H4｜功率级安全准入

必须另行批准。需要确认 DRV8311H 的 nSLEEP/nFAULT 物理路径、PWM 输入、死区/传播延迟、相电流、母线限流、温度、急停和复位期间的桥臂状态。没有独立硬件证据，不得解除 `G3-PWR LOCKED`。

## 10. 硬件资料与设备缺口

仓库存在以下资料文件，状态为 `VERIFIED FILE PRESENT`，但文件存在不等于当前板卡装配和版本已经确认：

- `docs/pcb docs/pcb原理图.pdf`
- `docs/pcb docs/pcb 网表.tel`
- `docs/pcb docs/pcb bom.xlsx`
- `docs/pcb docs/drv8311.pdf`
- `docs/pcb docs/2804电机参考手册.pdf`
- `docs/pcb docs/TI DRV8311H评估板/...`

当前仍然 `UNKNOWN` 或 `REQUIRES CONFIRMATION`：

- 实际可用的 AT32F415 Board1 实物及其 PCB/装配 revision；
- 3.3 V/低压调试电源和受控母线电源；
- 实际调试器是 DAP-Link 还是 J-Link；
- 当前串口是 COM3 还是历史记录中的 COM54；
- 示波器/逻辑分析仪带宽、探头和安全接地方式；
- 独立相电流探头或电流计；
- 手动/受控编码器工装、真实 KTH7823 传感器连接状态；
- 快速断电/急停手段和操作者现场安全流程；
- 当前板卡是否已装配 DRV8311H、C23/CSAREF 去耦、R6/R7/R10/D2 等关键器件。

仓库有 `tools/openocd/openocd_daplink.cfg`，但这只证明工具配置存在，不能证明现场有匹配调试器或目标板连接。

## 11. Timing Owner 未完成项

当前存在三组时序事实：

1. `applications/algo/foc_config.h`：Legacy 150 MHz、FOC 10 kHz、velocity 5 kHz、position/trajectory 1 kHz；
2. `platform/at/at32f415/generated/src/wk_tmr.c`：Legacy TMR1 约 20 kHz carrier/ADC event、TMR5 约 5 kHz；
3. `firmware/at32f415/include/foc_board.h` 与 `foc_board.c`：V2 H0 `200 us`、H1 `500 us`、TMR2 1 µs epoch、H0/H1 budget。

这些值的物理意义不同，但当前没有统一的 platform-owned timing seam。风险包括：把 Legacy 的 20 kHz carrier 当成 10 kHz FOC step、把 V2 H0/H1 Host policy 当成 Legacy 实时事实、或把 ADC IRQ 时间误认为模拟采样时刻。

最小后续迁移范围（本轮不执行）：

- 将 SoC timer clock、APB timer multiplier、counter tick 和真实 peripheral period 归入 AT32F415 platform timing owner；
- 将产品 loop policy（Legacy 10/5/1 kHz 与 V2 200/500 µs）分别表达，避免共用一个含义不清的宏；
- 通过 compile-time assertions 和 `foc_config`/Board config cross-check 防止漂移；
- 在迁移后重新构建 Legacy/V2，并重新执行 H0/H1 测量设计；
- 不改变当前控制周期、PWM 频率或时钟树，除非另行授权并有实测依据。

在 timing owner 收口之前，H2/H3 的测量可以执行，但任何结果必须标记实际时基和配置身份，不能作为跨固件通用 WCET 结论。

## 12. 本轮离线复验

本轮实际执行并通过：

- V2 `cmake --build build/rc0/baseline-arm --parallel 4`；
- Legacy tiny、standard、full-diag 三个 AT32F415 preset build；
- `arm-none-eabi-size -A`、`readelf -S/-l`、`objdump`、`nm`、MAP 资源和符号分析；
- V2 startup/linker/IRQ/output-lock 源码核对；
- `git diff --check`（保留既有 CRLF warning，无 diff error）。

RC0 Host `45/45`、ASan/UBSan `28/28`、Scope all、SYSTEM replay 和 long replay 继续引用 `docs/evidence/2026-11-14-next-rc0-contract-02-validation.md`，本轮没有因为只读准备任务重复全部历史 Host 测试。

## 13. 最终判定

`NEXT-BOARD-READY-01 PREPARATION PASS`

但以下结论明确禁止提前写入：

- `G3-SAFE HARDWARE PASS`
- `G3-PWR UNLOCKED`
- `AT32F415 WCET PASS`
- `MOTOR PHYSICAL OPERATION PASS`
- `REAL FOC FUNCTION PASS`

下一步只能在硬件资料版本、调试器/串口身份和现场设备确认后，单独授权进入 GATE H0；即使 H0～H3 通过，H4 仍需要独立功率级安全授权。

/* ---------------------------------------- end of file ----------------------------------------- */
