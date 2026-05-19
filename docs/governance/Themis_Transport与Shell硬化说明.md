# Themis Transport 与 Shell 硬化说明

Contract tag: `FalconFoc-lite transport-shell-hardening`

## 1. 文档目标

这份文档只收口 FalconFoc-lite 当前仓库里 shared UART shell 路径的正式事实。

它回答四件事：

1. 当前 interactive shell 的 transport owner 和 session owner 分别是谁。
2. 共享 `USART1` 上为什么必须执行 `console-first` 策略。
3. 当前代码、测试、gate 分别用什么方式把这条路径锁住。
4. 后续如果再改这条路径，哪些边界和约束不能回退。

## 2. 当前正式拓扑

FalconFoc-lite 当前 live 拓扑如下：

- `applications/tasks/shell_task.*`
  - `USART1` transport owner
  - 负责 DMA RX、行缓冲、异步 TX staging
  - 在完整命令行就绪后调用 `tm_shell_task_submit_line()`
- `applications/tasks/app_shell_task.*`
  - shell session owner
  - 负责单静态 queue、transport-ready gate、同步命令执行
  - 通过 `tm_shell` 驱动命令分发
- `applications/shell/*`
  - 产品命令 vocabulary、banner、help 文案与输出契约
- `subsys/shell/tm_shell.c`
  - 通用命令解析引擎

当前正式链路如下：

```text
USART1 IRQ/DMA
  -> shell_task
  -> tm_shell_task_submit_line()
  -> app_shell_task
  -> tm_shell::shell_process_line()
  -> shell print backend
  -> USART1 TX
```

当前仓库的正式 truth 不是旧串口 owner 路径里的 `uart_task -> tm_shell_task` 方案。
凡是仍把 `uart_task`、`subsys/shell/tm_shell_task.*` 或“`applications/tasks/shell_task.*` 已退役”写成现行事实的描述，都是历史残留，不是当前契约。

## 3. 当前串口策略

当前正式串口策略如下：

- debugger=`DAP-Link`
- port=`COM54`
- uart=`USART1`
- baud=`1000000`
- policy=`console-first`

补充约束如下：

- `BOARD_CONSOLE_UART_NAME` 与 `BOARD_TELEMETRY_UART_NAME` 指向同一物理资源。
- 资源同一不代表默认要同时开两套主动输出。
- shell 控制台是默认基线。
- VOFA 或其他主动 telemetry 只能作为 `debug-only` 能力显式启用。
- 共享 `USART1` 上默认不允许长期常开主动 telemetry。

因此本仓库当前只接受下面这条口径：

**共享 `USART1` 上，shell 控制台优先；任何 VOFA 或主动 telemetry 都只能作为 debug-only 诊断模式存在，而不是默认常开能力。**

## 4. 当前硬化点

### 4.1 transport owner 与 session owner 分离

当前正式边界如下：

- `shell_task`
  - 只拥有 transport 事实
  - 不再承载产品命令词表
  - 不再承载产品 banner 文案
- `app_shell_task`
  - 拥有 shell session 生命周期
  - 统一处理单静态 queue 和 transport-ready 状态
- `applications/shell`
  - 拥有产品命令和输出 vocabulary

### 4.2 单队列包线

当前 shell line handoff 只使用一层静态 queue：

- `TM_SHELL_TASK_QUEUE_DEPTH`

正式语义如下：

- queue 有空间时允许提交
- queue 满时直接返回失败
- 不再保留 shadow backlog
- 当前诊断字段至少包括：
  - `queue_used`
  - `queue_high_water`
  - `submit_ok_count`
  - `submit_fail_count`

### 4.3 transport-ready 与 banner hook

当前 `shell_task` 在 bring-up 时必须显式完成下面动作：

- 调用 `tm_shell_task_init()`
- 调用 `tm_shell_task_set_banner_hook(app_shell_banner_print);`
- transport ready 后调用 `tm_shell_task_notify_transport_ready();`

这保证了：

- banner 仍然由产品层 owner 决定
- session owner 不反向 include 产品命令实现
- 启动顺序可以被 host test 和 gate 观测

### 4.4 shared USART1 上的 telemetry 收口

当前配置要求：

- `TM_APP_FEATURE_UART_TELEMETRY` 默认必须为 `0U`
- `TM_APP_FEATURE_UART_PASSIVE_TELEMETRY` 默认必须为 `0U`
- `vofa_task.c` 只能在显式 telemetry enable 时编译进入路径

这条规则的目标不是否定 VOFA，而是避免 shared console 被默认主动输出打穿。

## 5. 当前验证入口

### 5.1 host test

正式 host test：

- `tests/host/subsys/test_tm_shell_task.c`

当前至少锁住：

- 单 queue FIFO
- queue 满时返回失败
- 诊断字段与实际行为一致

### 5.2 FalconFoc-lite contract

正式 contract：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/script/test_falconfoc_themis_lite_contract.ps1 -RepoRoot <repo-root>
```

它至少检查：

- telemetry 默认关闭
- `bsp/bsp_config.h` 保持 shared `USART1` 资源事实
- transport 与 shell 治理文档包含当前串口策略
- 启动链文档仍描述 `app_main -> motor_task_init -> shell_task_init`

### 5.3 transport/shell gate

正式 gate：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_transport_shell_hardening_contract.ps1 -RepoRoot <repo-root>
```

它至少检查：

- `shell_task.*` 与 `app_shell_task.*` 文件存在
- `shell_task.c` 仍显式调用 `tm_shell_task_*` 相关接线
- host test、manifest 和 quality gate 已接入
- 治理文档仍记录 `console-first` / `debug-only` / `COM54`

## 6. 后续修改要求

后续如果再修改这条路径，必须同时满足：

1. 先说明谁是 transport owner，谁是 session owner。
2. 不允许重新把产品命令词表倒灌回 transport owner。
3. 不允许在 shared `USART1` 上恢复默认常开主动 telemetry。
4. 同步更新 host test、gate、治理文档和维护日志。
5. 质量门必须继续能在 FalconFoc-lite 模式下独立通过。

## 7. 一句话结论

FalconFoc-lite 当前 shell 路径已经正式收口为：

- `shell_task` = `USART1` transport owner
- `app_shell_task` = shell session owner
- `USART1` = `console-first`
- VOFA/telemetry = `debug-only`
