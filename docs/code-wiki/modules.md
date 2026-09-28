# 模块、关键类型与 API

## 1. 代码树地图

```text
platform/
  at/at32f415/              AT32F415 生成式初始化、vendor 驱动、CMSIS/DSP
  cpu/cortex-m3-soft/       Cortex-M3 soft-float 与 FreeRTOS port 元数据
include/                    公共类型、errno、编译配置和 feature 开关
bsp/
  board/                    bsp_init、板级 Flash 后端
  ports/                    console、system reset 薄端口
applications/
  algo/                     FOC 数据模型、数学、SVPWM、PLL、轨迹
  periph/                   电流/PWM、编码器、校准执行、NTC
  runtime/                  产品运行时、运动输入、恢复、校准策略、故障入口、存储
  shell/                    Shell 命令和格式化输出
  tasks/                    motor/safe/shell/app-shell/VOFA 任务和 FSM
components/
  algorithm/                PID、Kalman 等可复用算法
  protocol/                 VOFA 协议
  utils/                    CRC、ring buffer、FSM、容器、tiny_param 等
subsys/                     export、runtime、IPC、日志、时间、Shell 基础
kernel/freertos/            FreeRTOS kernel + CMSIS-RTOS2
tests/host/                 主机端契约测试与 RTOS/硬件替身
scripts/                    构建、主机测试、质量门禁、板级脚本
```

## 2. Target 与模块责任

| CMake target | 主要源目录 | 对外职责 | 直接依赖/被谁使用 |
| --- | --- | --- | --- |
| `at32f415-hal` | `platform/at/at32f415` | F415 generated、vendor peripheral、CMSIS system、选定 DSP | 固件、BSP、applications |
| `freertos` | `kernel/freertos` | FreeRTOS kernel、CMSIS-RTOS2、ARM_CM3 port、heap | `subsys-core`、applications、固件 |
| `subsys-core` | `subsys/*` | export、assert、daemon、format、IPC、log、runtime、shell、time、task budget | applications、tests |
| `utils` | `components/utils/*` | 通用无产品语义工具库 | `subsys-core`、algorithm、applications |
| `algorithm` | `components/algorithm/*` | PID、filter 等可复用控制算法 | applications |
| `protocol` | `components/protocol/vofa` | VOFA JustFloat 帧格式与回调接口 | applications/tasks/vofa_task |
| `bsp-ports` | `bsp/ports` | console 和 reset 薄接口 | `bsp`、applications |
| `bsp` | `bsp/board` | board init 和 Flash backend | applications、固件 |
| `applications` | `applications/*` | 全部 FalconFoc 产品行为 | 固件主目标 |
| `falcon-foc-at32` | generated main + 上述静态库 | 最终 ELF/bin/hex 等固件产物 | 运行/烧录脚本 |

`components/utils/CMakeLists.txt` 当前对 `*/*.c` 做全量 glob，因此新工具文件会自动进入 `utils`；这提高了扩展便利性，也要求新增工具避免隐式依赖平台或 RTOS。

## 3. `applications/algo`：数据模型与基础算法

### 3.1 关键文件

| 文件 | 责任 | 调用边界 |
| --- | --- | --- |
| `applications/algo/foc_common.h` | `motor_dev`、配置、运行态、测量、校准、故障和模式类型 | 被 periph/runtime/tasks/shell 共享；不直接实现硬件访问 |
| `applications/algo/foc_config.h` | ADC/电机/保护/环路/校准默认值和派生时基 | `motor_runtime` 初始化；控制算法读取；参数运行时可覆盖部分值 |
| `applications/algo/foc_core.c/.h` | Clarke/Park、逆变换、SVPWM、限幅 | `foc_ctrl.c` 调用；只接收工程量和角度，不拥有 ADC/PWM |
| `applications/algo/foc_math.h` | wrap、LPF、ramp、sine/cos 和简单增益辅助 | 被控制、PLL、轨迹模块内联使用 |
| `applications/algo/pll_enc.c/.h` | 编码器位置误差、PLL 速度、累计圈数、LUT 修正 | `encoder.c` 更新；外环读取输出 |
| `applications/algo/traj_trap.c/.h` | 梯形轨迹 init/config/plan/eval/step | `foc_motion_runtime.c` 每 1 ms 驱动 |
| `applications/algo/traj_scurve.c/.h` | 独立七段 jerk-limited S 曲线 | 当前运行时未接入 `foc_process_input_task()` |

### 3.2 关键类型

| 类型 | 所在文件 | 关注点 |
| --- | --- | --- |
| `struct motor_hw_config` | `foc_common.h` | pole pairs、Rs/Ld/Lq/flux、gear、encoder 参数等持久化硬件参数 |
| `struct motor_prot_config` | `foc_common.h` | 电压、电流、速度、温度和扭矩阈值 |
| `struct motor_ctrl_config` | `foc_common.h` | id/iq/vel/pos PID、带宽、ramp、trajectory |
| `struct motor_calc_params` | `foc_common.h` | kt、ke、倒数、动态上下限等由配置推导的值 |
| `struct motor_ctrl_state` | `foc_common.h` | target/setpoint、feedforward、动态限幅、`theta_sim`、I-f 状态 |
| `struct motor_meas_data` | `foc_common.h` | 三相/总线电流电压、角度、速度、位置、温度 |
| `struct motor_dev` | `foc_common.h` | 单轴系统聚合对象，应用层唯一主要运行时实例 |
| `struct encoder` / encoder state | `encoder.h`、`foc_common.h` | SPI 读数、LUT、PLL、机械/电角、累计计数 |

## 4. `applications/periph`：实时外设适配与校准执行

| 文件 | 主要职责 | 谁调用 | 不能越过的边界 |
| --- | --- | --- | --- |
| `foc_ctrl.c/.h` | `foc_para_calc`、电流环、外环以及直控辅助函数 | `motor_task` 的 ADC/TMR5 handler，校准 | 不负责任务创建、Shell、Flash |
| `motor_drv.c/.h` | PWM enable/disable/duty、ADC injected phase read、PID/LPF 初始化与系数刷新 | `foc_ctrl`、FSM、encoder/calibration、runtime | 不拥有产品模式和状态机；`foc_lpf_refresh()` 不清除滤波历史 |
| `encoder.c/.h` | SPI1 blocking encoder read、方向/offset/LUT、PLL 更新 | TMR5 soft IRQ | 当前 DMA 关闭，不能假设零等待 |
| `foc_calib.c/.h` | current offset、Rs、Ls、flux、Js 校准状态机 | ADC handler 的 `foc_calib_loop` | ISR 内只计算/推进状态，不直接写 Flash |
| `foc_calib_enc.c/.h` | I-f encoder pole-pair/direction/phase/LUT/offset | `foc_calib_loop` | 最后只置 deferred save bit |
| `ntc_sensor.c/.h` | NTC/温度转换辅助 | 未来/安全路径按需要使用 | 当前主 ADC 普通 DMA 并非 live 路径 |

### 4.1 FOC 关键函数

| 函数 | 作用 | 调用时机 |
| --- | --- | --- |
| `foc_para_calc(motor)` | 将 ADC/encoder measurements 复制到 FOC 计算域，算电角速度、输出轴速度/位置和动态电压/电流限幅 | current loop 前 |
| `foc_current_loop_step(motor)` | 完整 `Clarke -> Park -> LPF -> id/iq PID -> 解耦/BEMF -> inverse Park -> SVPWM` | ADC1 handler 每第二 tick；电压/I-f 模式有专用分支 |
| `foc_outer_loop_step(motor)` | 位置级联到速度、速度到 `iq_set`，并把 `id_set` 置零 | TMR5 soft IRQ |
| `foc_ctrl_volt/current/vel/pos(...)` | 直接电压、电流、速度、位置控制辅助入口 | 校准或运行时直控 |
| `foc_lpf_init(motor)` | 按当前有效 `curr_bandwidth` 初始化 Id/Iq LPF 的输出与系数 | `motor_runtime_init()`，且发生在参数加载之后 | 初始化路径可清零输出 |
| `foc_lpf_refresh(motor)` | 按当前有效 `curr_bandwidth` 只刷新 Id/Iq LPF 系数 | runtime 参数修改 | 不清零 `lpf.output`，不改变 FOC 调度 |
| `encoder_init()` | 设置 SPI/片选、方向、LUT/PLL 参数 | `motor_runtime_init` |
| `encoder_update()` | 读取原始角度、方向/LUT、PLL、机械/电角/累计位置 | TMR5 soft IRQ；当前为 blocking SPI |

## 5. `applications/runtime`：产品运行时策略

### 5.1 运行时总入口

| 文件 | 责任 |
| --- | --- |
| `motor_runtime.c/.h` | 默认值、对象初始化/attach、模式/运动/校准/存储请求、snapshot、延迟保存消费 |
| `foc_motion_runtime.c/.h` | 1 ms 输入模式处理、ramp/traj/setpoint、开环电压/I-f/相电压命令 |
| `foc_restore.c/.h` | 参数加载后的派生量、轨迹配置、瞬态 setpoint 清理 |
| `motor_calibration_runtime.c/.h` | 校准结束后的常量/PI/PID 增益、状态收尾、编码器保存请求 |
| `motor_fault_runtime.c/.h` | `foc_fault_check_fast`/`foc_fault_check` 故障判定、首次故障 RAM 上下文、停机后读取/清除 |
| `motor_store.c/.h` | 三组参数 A/B slot、header/CRC/sequence、保存/加载/擦除 |

### 5.2 对外 API 与边界

| API | 责任 | 允许调用者 | 边界/前置条件 |
| --- | --- | --- | --- |
| `motor_runtime_init()` | 建立默认配置、初始化 encoder/PID/LPF/trajectory、尝试加载持久化参数 | `motor_task_init` | 任务创建前调用；当前加载状态被初始化流程忽略 |
| `motor_runtime_attach()` | 绑定全局运行时实例供 Shell/回调访问 | bring-up | 单实例语义 |
| `motor_runtime_snapshot()` | 在临界区复制可读状态，包括 raw ADC/offset、相电流、开环角度/速度、FOC/SVPWM 值 | Shell/诊断/测试 | 只读快照，避免 Shell 直接碰实时对象 |
| `motor_fault_context_read()` / `clear()` | 读取/清除首次故障 RAM 上下文 | 停机后的 fault Shell、测试 | 不写 Flash；不能在 ISR 中导出或清除 |
| `motor_runtime_request_axis_action()` | 将 start/stop/disable/fault-clear 交给 FSM | Shell/产品调用者 | 状态机拥有状态转换 |
| `motor_runtime_request_ctrl_mode()` / `...input_mode()` | 修改控制/输入模式 | Shell/产品调用者 | 参数合法性由上层解析和运行时限制共同完成 |
| `motor_runtime_request_store_save()` | 只置 `PARAM_SAVE_*` bit，不立即写 Flash | Shell/校准完成路径 | 由 safe task 消费 |
| `motor_runtime_request_store_load()` | 加载指定 group，返回各组状态与 partial | Shell/启动恢复 | motor/sys 与 Shell 规则要求 IDLE；当前不是全事务回滚 |
| `motor_runtime_request_store_erase()` | 请求全量擦除 | Shell | 只在 IDLE；具体 Flash 操作仍由运行上下文约束 |
| `motor_runtime_request_*motion()` | 传递 passthrough/ramp/trajectory/voltage/I-f/phase 命令 | Shell/应用 | 只设置产品状态，实时执行在对应 loop/task |
| `motor_runtime_diag_set_*()` | 修改 PID/硬件/保护/带宽/电压诊断参数 | `diag` Shell 或受控应用调用者 | 仅 `IDLE`；运行中返回 false，合法 `currbw` 同时重算 PI 和 LPF，非有限/非正值拒绝 |

### 5.3 运动输入模式的实现状态

`foc_process_input_task()` 当前实际处理：

- `INACTIVE`：速度/电流归零，位置保持测量值；
- `PASSTHROUGH`：直接使用 target 与动态限幅；
- `VEL_RAMP`：线性速度 ramp；
- `TORQUE_RAMP`：由 torque/kt/gear 换算后做电流 ramp；
- `TRAP_TRAJ`：调用 `traj_trap_step_update()`；
- `POS_FILTER`、`VEL_FILTER`、`TUNING`：当前 switch 分支为空操作；
- 未知值：清零 setpoint。

因此枚举存在不等于模式已经提供产品行为。`modules` 文档将它们列出，是为了让调用者知道可见接口和实际实现边界。

## 6. `applications/tasks`：任务、状态机与传输

| 文件 | owner | 关键接口 |
| --- | --- | --- |
| `motor_task.c/.h` | 电机任务和 ISR owner | `motor_task_init`、`ADC1_IRQHandler`、平台 soft IRQ、motor/safe task entry |
| `motor_fsm.c/.h` | 轴状态和安全转换 | `motor_fsm_init`、`motor_fsm_dispatch`、`motor_fsm_dispatch_calib` |
| `shell_task.c/.h` | USART1 DMA RX/异步 TX 和行提交 | `shell_task_init`、USART1 handler、RX/TX queue |
| `app_shell_task.c/.h` | 静态有界 Shell 会话队列和 session task | `tm_shell_task_init`、`tm_shell_task_submit_line`、snapshot/host process |
| `vofa_task.c/.h` | 10 ms telemetry task | `vofa_task_init`/bind/process |

### 6.1 Shell 传输和会话的分离

```text
USART1 IDLE/DMA RX
  -> shell_task.c 读取 DMA 环形区
  -> tm_shell_task_submit_line()
  -> app_shell_task.c 静态队列（默认深度 3）
  -> session task
  -> applications/shell/app_shell_cmd_*.c
```

`shell_task.c` 不解析产品命令；`app_shell_task.c` 不直接操作 UART 寄存器。二者用 bounded line queue 连接，并拒绝 ISR 中提交、超长行、队列满等情况。

## 7. `applications/shell`：命令面

| 命令族 | 文件 | 主要子命令 | 业务入口 |
| --- | --- | --- | --- |
| `sys` | `app_shell_cmd_sys.c` | `status`、`tasks`、`reboot` | 系统/任务诊断和 reset port |
| `state` | `app_shell_cmd_state.c` | `show`、`start`、`stop`、`disable` | runtime request -> FSM |
| `obs` | `app_shell_cmd_obs.c` | `all`、`iq`、`id`、`vel`、`pos`、`vbus`、`temp`、`enc` | snapshot；`all` 导出 raw/offset/电流/角度/FOC/SVPWM |
| `fault` | `app_shell_cmd_fault.c` | `status`、`clear` | fault context/FSM clear；`status` 停机后导出首故障快照 |
| `store` | `app_shell_cmd_store.c` | `status`、`save`、`load`、`erase` | parameter runtime |
| `cfg` | `app_shell_cmd_cfg.c` | `show`、`ctrl`、`input` | mode setting |
| `cal` | `app_shell_cmd_cal.c` | `status`、`enc`、`motor`、`abort` | calibration request |
| `diag` | `app_shell_cmd_diag.c` | `set`、`ramp`、`pid`、`param` | idle-only diagnostic tuning |

输出统一使用 `TM_SHELL_SUMMARY`/`TM_SHELL_ROW`，偏向短 ASCII、安全宽度和可脚本解析的 `key=value`。Shell 命令不应直接调用 vendor peripheral API。

`cal enc` 与 `cal motor` 仍是可见的只读命令面，但在默认 `FOC_ENERGIZED_CALIB_ENABLE=0U` 下，runtime/FSM 会在进入 `AXIS_STATE_CALIB` 前拒绝它们；它们不能打开 PWM，也不能施加校准电压/电流。启动电流偏置校准不经过这两个带电入口。

`store` 的当前边界：

- `erase`：只允许 IDLE；目标固定为 all；
- `save`：只允许 IDLE，立即返回 `mode=deferred`，真正写入由 safe task 完成；
- `load motor/sys`：只允许 IDLE；`load enc` 可由更宽的运行时规则处理，但调用者仍应关注当前轴状态；
- 命令先输出摘要，再输出每个 group 的状态/大小行。

## 公共机制与辅助目录

### `subsys`

| API/模块 | 责任 | 调用边界 |
| --- | --- | --- |
| `INIT_EXPORT` / `tm_export_run_range` | 编译期收集并按阶段运行初始化项 | `app_main` 调度；模块只注册，不自行改变阶段顺序 |
| `tm_runtime` / `tm_assert` | 运行时错误、断言和 host/firmware 差异 | 被应用、BSP、测试使用；不承载电机策略 |
| `tm_shell` / shell registry | 通用 Shell 注册、解析和输出框架 | `applications/shell` 提供产品命令；transport 由 tasks/BSP 提供 |
| `tm_ipc` / `tm_task_budget_probe` | IPC、任务预算与诊断 | task/runtime 机制；测试可用 fail-fast probe |
| `core_time`、`tm_log`、`tm_format` | 时间、日志和安全格式化 | 调度/诊断基础设施 |

### `components`

| 模块 | 关键 API/文件 | 当前使用边界 |
| --- | --- | --- |
| `components/algorithm/pid` | `pid_init`、`pid_reset`、`pid_calc`、position/fast variants | FOC current/outer loop 和校准增益；不访问硬件 |
| `components/algorithm/filter` | `kalman_filter`、LPF 辅助 | 通用算法；Kalman 当前不在 FOC hot path |
| `components/protocol/vofa` | `vofa_bind`、`vofa_process`、JustFloat framing | `vofa_task` 同步发送；singleton、无分配、非线程安全 |
| `components/utils` | CRC、ring buffer、FSM、mempool、tiny_param、Welford 等 | 跨层基础工具；不能反向依赖 applications/BSP |

### `include`、`scripts`、`tests`

- `include/` 是公共头文件根，集中 `tmdef`、`tm_errno`、`tmconfig` 和 `include/config`；不承载 vendor `device/drivers` 运行时。
- `scripts/` 提供 `run.sh` 使用的 CLI、host test、质量门禁和独立 board lane；本 Wiki 只执行 host/firmware lane。
- `tests/host/` 通过 RTOS/BSP/runtime 替身做契约测试；测试替身必须显式说明它没有证明真实寄存器、时序和物理行为。

本计划新增的 host 目标还直接链接 `applications/periph/foc_ctrl.c`、`applications/algo/foc_core.c` 和 PID 实现，验证电压开环/I-f 的真实软件调用链；这仍是主机数学验证，不是 PWM 或电机验证。

## 8. 第三方与生成式代码范围

本节遵守“只说明职责、选用文件和调用边界”的范围，不对 vendor/CMSIS 的每个函数做 Wiki 化展开。

### 8.1 AT32F415 vendor 驱动

| 选用范围 | 责任 | 上层边界 |
| --- | --- | --- |
| `platform/at/at32f415/vendor/drivers/inc` | F415 外设寄存器/类型/驱动声明 | generated `wk_*.c` 和 `motor_drv/encoder/bsp` 通过芯片头/外设句柄使用；应用产品策略不能下沉 |
| `platform/at/at32f415/vendor/drivers/src/at32f415_adc.c` | ADC 外设实现 | `wk_adc.c` 完成配置，`motor_drv.c` 读取结果 |
| `.../at32f415_tmr.c` | TMR/PWM/中断相关实现 | `wk_tmr.c` 配置，`motor_drv.c` 设置 duty，`motor_task.c` 处理应用 ISR |
| `.../at32f415_spi.c` | SPI 外设实现 | `wk_spi.c` 初始化，`encoder.c` 读编码器 |
| `.../at32f415_usart.c`、`.../at32f415_dma.c` | USART/DMA 实现 | `wk_usart/wk_dma.c` 配置，BSP console 和 shell transport 使用 |
| `.../at32f415_flash.c` | Flash 控制器原语 | 只由 `bsp_flash.c` 间接使用，不从 ISR/命令处理器直接调用 |
| 其余 `crm/gpio/debug/exint/misc/crc` | 时钟、GPIO、调试、中断、杂项、CRC | generated init/BSP 边界内使用 |

功率级实机准入不属于模块 API 的隐含副作用。软件模块全部通过无板门禁也不能越过 `nFAULT/nSLEEP`、Break/deadtime、物理断电和测量准备条件；当前状态为 `REAL_BENCH_TEST_STATUS=BLOCKED`。

### 8.2 CMSIS / DSP

| 选用文件/目录 | 责任 | 选用原因/边界 |
| --- | --- | --- |
| `vendor/cmsis/cm4/device_support/system_at32f415.c` | SystemCoreClock 与系统初始化支持 | platform target 内部；`bsp_init` 只读取系统时基事实 |
| `vendor/cmsis/cm4/device_support/startup/gcc/startup_at32f415.s` | 启动汇编、向量表、栈/复位入口 | linker/startup 平台边界；业务不修改启动 ownership |
| `vendor/cmsis/dsp/Source/FastMathFunctions/arm_sin_f32.c`、`arm_cos_f32.c` | FOC/PLL 使用的快速三角函数 | `foc_math`/控制算法调用；不做平台调度 |
| `vendor/cmsis/dsp/Source/MatrixFunctions/*` | 选定矩阵运算 | 供可复用算法/后续模块使用；当前 Kalman 不在 FOC 热路径 |
| `vendor/cmsis/dsp/Source/CommonTables/CommonTables.c` | DSP 公共表 | 由 `at32f415-hal` 链接提供 |

## 9. 测试模块映射

| 测试类别 | 代表 target | 覆盖边界 |
| --- | --- | --- |
| subsys | `test_tm_export`、`test_tm_shell`、`test_tm_shell_task`、`test_tm_runtime` | export/格式/assert/任务队列/日志/时间/console 合约 |
| BSP | `test_bsp_system_contract`、`test_bsp_console_contract` | 主机替身下的 reset/console 边界 |
| 应用 Shell | `test_app_shell_common_contract`、`...burst/state/store/cfg_cal/diag` | 解析、队列背压、状态、存储命令、诊断参数 |
| FSM/runtime | `test_motor_fsm_abort_restore`、`test_motor_fsm_fault_clear_contract`、`test_motor_runtime_contract` | 校准中止、恢复、故障清除、snapshot/请求 |
| 存储/恢复 | `test_foc_param_restore_contract` | payload、A/B、CRC/状态映射和恢复 |
| 运动/校准 | `test_foc_motion_runtime_contract`、`test_motor_calibration_runtime_contract` | ramp/traj、派生增益与校准 runtime |
| 故障/保护 | `test_motor_fault_runtime_contract`、`test_motor_fault_context_contract`、`test_foc_vbus_guard_contract` | 快速过流锁存、首故障上下文、任务级 3 s/200 ms 后备、非法 VBUS 和 PWM disable |
| 带宽/开环模型 | `test_motor_runtime_contract`、`test_foc_voltage_openloop_model_contract` | 持久化/运行时 LPF 同步、零速/角度回绕、I-f、SVPWM 和 VBUS 数学边界 |
| 校准门禁 | `test_motor_fsm_calibration_gate_contract`、`test_motor_fsm_abort_restore` | 默认拒绝带电校准，兼容显式开启宏的中止恢复路径 |

## 10. 2026-08-30 bench-only 诊断模块职责与调用边界

| 模块 | 职责 | 选用文件 | 调用边界 |
| --- | --- | --- | --- |
| FOC 采样 trace | 保存最近 64 次 FOC 完成后的定点观测 | `applications/runtime/motor_fault_runtime.h/.c`、`applications/tasks/motor_task.c` | 仅 bench FOC 热路径写 RAM；Shell 停机后读取；不打印、不写 Flash |
| PWM/ADC 映射实验 | 提供 6×6 纯 RAM 映射表，默认 map0 对齐当前硬件事实 | `applications/periph/motor_bench_mapping.h/.c`、`applications/periph/motor_drv.c/.h` | runtime 在 IDLE 设置；periph 在 PWM/ADC 访问处消费；产品编译路径不启用 override |
| ADC 触发实验 | 校验并写入 TMR1 CH4 compare | `applications/periph/motor_bench_mapping.*`、`applications/periph/motor_drv.c` | 仅 IDLE、仅允许 256..3740；trace 记录实际软件状态；不宣称真实采样窗口 |
| 编码器 trace | 保存 32 条手动旋转所需的 SPI/角度/速度记录 | `applications/periph/encoder_bench_trace.h/.c`、`applications/periph/encoder.c/.h` | 编码器更新点写 RAM；Shell 停机后读；不执行校准、不改变角度 |
| 矩阵脚本 | 运行只读预检和白名单单变量脉冲矩阵 | `scripts/flows/board/run_bench_matrix.ps1` | 需要 `-RunPulses` 才能带电；串口失联即停止；不操作 PSU、不发送校准/清故障/`state stop` |

这些模块都属于“诊断辅助面”，不拥有产品运行策略。产品运行仍由 FSM、fault runtime
和现有 FOC 控制链拥有；bench 开关不能成为产品默认行为。
