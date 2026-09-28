# Themis Shell 与诊断输出规范
Contract tag: `FalconFoc-lite shell-output-policy`

## 1. 目标

这份规范约束 FalconFoc-lite 当前 `applications/shell/` 的正式 shell 输出。
目标只有三个：

1. 让人看得懂
2. 让脚本抓得稳
3. 让 128K ROM / 32K RAM 级别 MCU 也扛得住

## 2. 当前 owner 口径

当前 live owner 必须按下面的口径理解：

- `applications/tasks/shell_task.*`
  - `USART1` transport owner
- `applications/tasks/app_shell_task.*`
  - shell session owner
- `applications/shell/*`
  - 产品命令 vocabulary、banner、help 文案、输出策略

当前 bench 共享串口策略如下（旧 COM54/1 Mbit/s 记录仅属历史 witness）：

- uart=`USART1`
- port=`COM3`
- baud=`115200`
- policy=`console-first`
- active telemetry=`debug-only`

也就是说，当前 FalconFoc-lite 的 shell 输出规则必须服务于 shared console，而不是服务于宽屏终端或实验台大表格。

## 3. 正式输出形态

### 3.1 Summary Line

每条命令的正式输出必须先给一条 summary line：

```text
[topic] cmd=<subcommand> result=<ok|error> key=value ...
```

规则：

- 以 `\r\n[topic] ` 起行
- `topic` 必须稳定、短、可预测
- `cmd` 必须代表用户实际输入的子命令，或者无参时的默认子命令
- 同一条命令的成功路径和失败路径，`cmd` 必须完全一致
- 多级命令额外使用 `action=`
- 只放这条命令最关键的结论，不写大段 prose

例子：

```text
[state] cmd=show result=ok axis=idle ctrl=vel input=traj fault=0x00000000
[obs] cmd=iq result=ok value=2.5 unit=A
[diag] cmd=pid action=show result=ok loop=all
[diag] cmd=pid action=set result=ok loop=vel field=kp value=0.1000 unit=raw
```

### 3.2 Detail Row

summary line 放不下时，后续只允许 detail row：

```text
  label  : key=value key=value ...
```

规则：

- 两个空格开头
- label 用短词
- 冒号后继续用 `key=value`
- 不堆长句

### 3.3 Repeated Row 优先

当一条命令要列多个对象时，优先 repeated row：

```text
  task   : name=shell queue_used=0
  task   : name=vofa enabled=0
```

不要优先使用：

- 宽 ASCII 表格
- 大量空格对齐
- 多层标题块

原因：

- 更省输出
- 更容易脚本解析
- 增减字段时不容易破版

## 4. 共享串口约束

对当前 `COM3 @ 115200` 共享控制台，必须额外遵守：

- 不允许把输出建立在 `128` 列宽终端假设上
- runtime-visible shell 文本保持 `ASCII` 安全
- 诊断明细优先 `Repeated Row`
- 不使用宽 ASCII 表格
- 不使用 ASCII art banner

这条约束的目标是：

- 串口抓取稳定
- shell 回放日志稳定
- 最小化 ROM/RAM 与一次性 burst 输出压力

## 5. 正式词表

### 5.1 布尔值

布尔词表分两类：

- 运行态状态位：`0/1`
- 策略位或功能开关：`on/off`

禁止混用：

- `yes/no`
- `true/false`
- `enable/disable`

### 5.2 结果词表

动作类命令统一使用：

- `result=ok`
- `result=error`

必要时补：

- `reason=<snake_case>`
- `rc=<int>`

### 5.3 字段命名

字段名统一使用小写 snake 风格，例如：

- `queue_used`
- `queue_depth`
- `submit_ok_count`
- `timeout_ms`
- `sample_seq`

同一概念禁止在不同命令中反复换名。

### 5.4 当前禁止项

当前正式禁止继续扩散这些旧字段：

- `ver=`
- `size=` 用于参数数据大小
- `payload=`
- `flash_total=` 作为单槽字段
- `val=`
- `queue=<used>/<depth>`
- `ok=<count>`
- `fail=<count>`
- `writer=`
- `snapshot=unavailable`
- `policy=idle-only`
- `owner=cfg`

对应正式替代口径：

- `version=`
- `data=`
- `flash=`
- `total_data=`
- `total_flash=`
- `value=`
- `queue_used=`
- `queue_depth=`
- `submit_ok_count=`
- `submit_fail_count=`
- `reason=snapshot_unavailable`
- `reason=idle_only`
- `reason=redirect target=cfg`

## 6. Hint / Note 规则

`hint` / `note` 允许比 summary 更像人话，但只允许放在辅助行：

```text
  hint   : next='store save sys'
  note   : write='queued; safe-task will commit flash later'
```

裁决：

- `summary` 不写人话
- `hint` / `note` 允许简短引号短句
- `hint` 只负责下一步引导
- `note` 只负责补充当前行为的误解点
- `hint` / `note` 不承载核心状态真相

`diag` 当前正式收口如下：
- `diag pid ... show` -> `cmd=pid action=show`
- `diag pid ... reset` -> `cmd=pid action=reset`
- `diag pid ... <field> <value>` -> `cmd=pid action=set`
- `diag param show` -> `cmd=param action=show`
- `diag param <name> <value>` -> `cmd=param action=set`

## 7. Banner 与 Help

APP shell banner 是唯一允许在会话启动时主动输出的多行信息，但也必须保持窄行、稳定、ASCII-safe。

推荐形态：

```text
[shell] name=FalconFoc version=v1.0.0
  image  : build_date=Jun 03 2026 build_time=21:00:00 shell=on flavor=carrier-demo
  console: uart=USART1 baud=1000000 transport=dma
  hint   : next='help'
```

补充裁决：

- 文档不能再要求 runtime banner 输出 `port=COM54`
- `COM54` 是 host witness，不是固件实时真相
- runtime banner 只输出板上已知事实

`help`、`sys status`、`store status`、`diag ...` 这类命令都应遵守同一输出模型：

- 第一行 summary
- 后续 repeated row
- 不拼大表格
- 不把解释性 prose 混进核心输出

`help` 当前正式建议：

```text
[help] count=9
  cmd    : name=sys group=System usage='[status|tasks|reboot]'
  cmd    : name=help group=Built-in usage='Show command list'
```

也就是：

- 不再输出裸分类头如 `  [System]`
- 分类进入 repeated row 的 `group=`
- 带空格的帮助文本必须进入引号字段，如 `usage='Show command list'`

## 8. 与资源目标的关系

FalconFoc-lite 面向 128K ROM / 32K RAM 级芯片时，shell 输出规范必须服务于资源预算：

- 优先稳定 vocabulary，而不是追求“全能终端体验”
- 优先窄行和 repeated row，而不是炫技式表格
- 优先少量关键命令，而不是堆叠大而全的输出面

这也是 `console-first` 策略的一部分。

## 9. 一句话结论

FalconFoc-lite 当前 shell 输出只接受下面这条正式口径：

**shared `USART1` 上，`shell_task` 负责 transport，`app_shell_task` 负责 session；summary 保持严格 `key=value`，`hint` / `note` 只做辅助引导，输出必须窄行、ASCII-safe、Repeated Row 优先，并服从 `console-first`。**
