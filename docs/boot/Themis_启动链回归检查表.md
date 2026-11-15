# FalconFoc 启动链回归检查表

## 1. 目标

本文固定当前 FalconFoc Themis-lite 的真实启动链，用于防止 live
文档、脚本和代码回退到退役启动语义。

## 2. 当前真实启动链

```text
platform/at/at32f415/generated/src/main.c
    -> app_main_bootstrap()

applications/app_main.c
    -> bsp_init()
    -> export_init_execute_range(...)
    -> osKernelInitialize()
    -> export_init_execute_range(...)
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

## 3. 当前 owner 边界

- 平台入口：`platform/at/at32f415/generated/src/main.c`
- 启动桥：`applications/app_main.c`
- board pre-kernel glue：`bsp/board/bsp_init.c`
- APP bring-up owner：`motor_task_init`
- shell transport owner：`shell_task_init`
- shell session owner：`app_shell_task`
- secondary APP bring-up owner：`vofa_task_init`

## 4. 回归检查项

1. `platform/at/at32f415/generated/src/main.c` 是否仍然只承接芯片级 bring-up。
2. `applications/app_main.c` 是否仍然是 staged boot 的唯一桥接入口。
3. `bsp/board/bsp_init.c` 是否仍然只承接最小 pre-kernel glue。
4. `motor_task_init()` 是否仍然先调用 `shell_task_init()`，再调用 `_motor_init()`。
5. `shell_task_init()` 是否仍然只负责 transport bring-up，而不是 export owner。
6. `app_shell_task` 是否仍然保持 session owner 身份。
7. `vofa_task_init()` 是否仍然保持独立 APP export 身份。
8. `./run.sh build` 是否可用。
9. `./run.sh flash` 是否可用。
10. `COM54` 上的 shell 是否仍能正常进入并响应 `ver`。
11. `store save sys -> reboot -> store status` 是否仍保留参数恢复能力。

当前 live 补充规则：

- `tm_ipc_export_init()` 默认不在 FalconFoc 默认镜像里出现
- `vofa_task_init()` 只在 `TM_FEATURE_VOFA=1` 或 telemetry 路径显式开启时进入启动链
- `COM54` 上的 shell witness 以 `help` / `sys status` 为准，不再使用退役的 `ver` 探针

## 5. 文档回退判定

下面内容如果重新出现在 live 启动文档中，应视为回退：

- 把 board bring-up owner 写回旧 BSP SoC 目录
- 把 shell bring-up 写成独立 export owner
- 把退役任务链写回 live 启动主链
- 把平台路径写回旧 SoC 名称或旧平台目录
- 把旧 shell/task owner 写回当前口径
