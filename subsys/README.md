# Subsystem Layer

`subsys/` 是共享机制与 shared owner 层，不是产品 service 垃圾桶。

先看：

- `input/tm_input.h`
- `led/tm_led.h`
- `shell/tm_shell_task.h`
- `storage/tm_param_store.h`
- `ui/screen_manager.h`

这里负责：

- shared runtime / shell / storage / UI mechanism contract
- 可被多个 APP owner 消费的正式 shared owner
- 不承载 TEC-only 产品语义和页面业务策略

不要误去：

- `applications/services/` 看产品 service owner
- `applications/tasks/` 看 cadence / blocking / thread lifetime
- `bsp/` 看 board topology 和默认实例
