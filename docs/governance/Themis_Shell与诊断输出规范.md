# Themis Shell 与诊断输出规范

Contract tag: `FalconFoc-lite shell-output-policy`

## 1. 目标

这份规范约束 FalconFoc-lite 当前 `applications/shell/` 的正式 shell 输出。

目标只有三个：

1. 让人看得懂。
2. 让脚本抓得稳。
3. 让 128K ROM / 32K RAM 级别 MCU 也扛得住。

## 2. 当前 owner 口径

当前 live owner 必须按下面口径理解：

- `applications/tasks/shell_task.*`
  - `USART1` transport owner
- `applications/tasks/app_shell_task.*`
  - shell session owner
- `applications/shell/*`
  - 产品命令 vocabulary、banner、help 文案、输出策略

当前共享串口策略如下：

- uart=`USART1`
- port=`COM54`
- policy=`console-first`
- active telemetry=`debug-only`

也就是说，当前 FalconFoc-lite 的 shell 输出规则必须服务于 shared console，而不是服务于宽屏终端或实验台大表格。

## 3. 正式输出形态

### 3.1 Summary Line

每条命令的正式输出必须先给一条 summary line：

```text
[topic] key=value key=value ...
```

规则：

- 以 `\r\n[topic] ` 起行
- `topic` 必须稳定、短、可预测
- 只放这条命令最关键的结论
- 不写大段 prose

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

对当前 `COM54 @ 1000000` 共享控制台，必须额外遵守：

- 不允许把输出建立在 `128` 列宽终端假设上
- runtime-visible shell 文本保持 `ASCII` 安全
- 诊断明细优先 `Repeated Row`
- 不使用宽 ASCII 表格
- 不使用 ASCII art banner

这条约束的目标是：

- 串口抓取稳定
- shell 回放日志稳定
- 最小化 ROM/RAM 与一次性 burst 输出压力

## 5. 词表规范

### 5.1 布尔值

布尔词表分两类：

- 运行态状态位：`0/1`
- 策略位：`on/off`

禁止混用：

- `yes/no`
- `true/false`
- `enable/disable`

### 5.2 结果词表

动作类命令统一使用：

- `result=ok`
- `result=error`

必要时补：

- `rc=<int>`

### 5.3 字段命名

字段名统一使用小写 snake 风格，例如：

- `queue_used`
- `queue_high_water`
- `timeout_ms`
- `sample_seq`

同一概念禁止在不同命令中反复换名。

## 6. Banner 规范

APP shell banner 是唯一允许在会话启动时主动输出的多行信息。

但也必须保持窄行、稳定、ASCII-safe。

推荐形态：

```text
[shell] name=Themis image=falconfoc
  image  : shell=on build=debug
  console: uart=USART1 port=COM54 baud=1000000
  hint   : help=type 'help'
```

不允许：

- 宽 banner
- 大块装饰字符
- 依赖 128-column 终端的排版

## 7. Help 与诊断输出要求

`help`、`sys status`、`store status`、`diag ...` 这类命令都应遵守同一输出模型：

- 第一行 summary
- 后续 repeated row
- 不拼大表格
- 不把解释性 prose 混进核心输出

如果需要 hint，单独用：

```text
  hint   : next='store save sys'
```

## 8. 与资源目标的关系

FalconFoc-lite 面向 128K ROM / 32K RAM 级芯片时，shell 输出规范必须服务于资源预算：

- 优先稳定 vocabulary，而不是追求“全能终端体验”
- 优先窄行和重复 row，而不是炫技式表格
- 优先少量关键命令，而不是堆叠大而全的输出面

这也是 `console-first` 策略的一部分。

## 9. 一句话结论

FalconFoc-lite 当前 shell 输出只接受下面这条正式口径：

**shared `USART1` 上，`shell_task` 负责 transport，`app_shell_task` 负责 session；输出必须窄行、ASCII-safe、Repeated Row 优先，并服从 `console-first`。**
