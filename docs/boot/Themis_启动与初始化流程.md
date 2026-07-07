# FalconFoc 启动与初始化流程

## 1. 目标

本文只记录当前 FalconFoc Themis-lite 仓库真实生效的启动链。

当前 live owner 只包括：

- `platform/at/at32f403a/generated/src/main.c`
- `applications/app_main.c`
- `bsp/board/bsp_init.c`
- `motor_task_init()`
- `shell_task_init()`
- `app_shell_task`
- `vofa_task_init()`

## 2. 当前正式启动链

```text
platform/at/at32f403a/generated/src/main.c
    -> wk_wait_for_power_stable()
    -> wk_system_clock_config()
    -> wk_periph_clock_config()
    -> wk_debug_config()
    -> wk_nvic_config()
    -> wk_timebase_init()
    -> wk_gpio_config()
    -> wk_adc1_init()
    -> wk_dma1_channel1_init()
    -> wk_dma1_channel2_init()
    -> wk_dma1_channel3_init()
    -> wk_dma1_channel4_init()
    -> wk_usart1_init()
    -> wk_spi1_init()
    -> wk_spi3_init()
    -> wk_tmr2_init()
    -> wk_tmr1_init()
    -> wk_tmr6_init()
    -> wk_can1_init()
    -> app_main_bootstrap()

applications/app_main.c
    -> bsp_init()
    -> export_init_execute_range(EXPORT_LEVEL_PRE_KERNEL_FIRST, EXPORT_LEVEL_PRE_KERNEL_LAST)
    -> osKernelInitialize()
    -> export_init_execute_range(EXPORT_LEVEL_POST_KERNEL_FIRST, EXPORT_LEVEL_APP)
        -> tm_ipc_export_init()
        -> motor_task_init()
            -> shell_task_init()
                -> tm_shell_task_init()
                -> osThreadNew(shell_task)
            -> _motor_init()
            -> osThreadNew(motor_task)
            -> osThreadNew(motor_safe_task)
        -> vofa_task_init()
            -> vofa_register_send_callback()
            -> osThreadNew(vofa_task)
    -> osKernelStart()
```

当前 live 补充规则：

- `tm_ipc_export_init()` 默认不在 FalconFoc 默认镜像里出现，只有 `TM_IPC_ENABLE=1` 时才进入启动链
- `vofa_task_init()` 是 feature-gated APP export，不属于默认产品镜像常驻路径

## 3. Owner 收口

### 3.1 `platform/at/at32f403a/generated/src/main.c`

职责：

- SoC 时钟、中断、timebase、vendor 外设初始化
- 完成 MCU 基线 bring-up 后跳转到 `app_main_bootstrap()`

不负责：

- RTOS task owner
- product shell policy
- parameter restore policy

### 3.2 `applications/app_main.c`

职责：

- staged boot bridge
- 串起 `bsp_init()`、export、`osKernelInitialize()`、`osKernelStart()`

### 3.3 `bsp/board/bsp_init.c`

职责：

- board-owned pre-kernel glue
- 当前 live 内容保持最小化

当前事实：

- `core_time_init(SystemCoreClock)`

### 3.4 `motor_task_init()`

职责：

- 当前 APP bring-up owner
- 必须显式先调用 `shell_task_init()`
- 然后进入 `_motor_init()`
- 最后创建 `motor_task` 和 `motor_safe_task`

### 3.5 `shell_task_init()` 与 `app_shell_task`

职责拆分：

- `shell_task_init()`
  - `USART1` DMA transport owner
- `app_shell_task`
  - shell session owner
  - line queue / command dispatch owner

### 3.6 `vofa_task_init()`

职责：

- VOFA callback 注册
- `vofa_task` 创建

## 4. 当前参数与串口口径

当前 live 策略：

- shell 串口为 `USART1`
- shell 采用 `console-first` 策略
- telemetry 默认关闭，只有显式诊断场景才开启
- 参数默认通过应用层参数模块处理版本、恢复、升级和保存策略
- flash 读写 backend 归 `bsp/board`

## 5. 当前实板见证

当前文档中记录的 live board-debug witness：

- debugger: `J-Link`
- serial: `COM54`
- build entry: `./run.sh build`
- flash entry: `./run.sh flash`

当前文档结论只覆盖：

- 启动链
- shell 可用性
- 参数恢复链条
