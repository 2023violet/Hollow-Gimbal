# Themis FalconFoc 新版 Shell 使用手册

## 1. 文档目标

这份手册只回答一个问题：

**当前 FalconFoc 固件里的 shell，到底该怎么连、怎么看、怎么用。**

本文只描述当前 live 实现，覆盖：

- `applications/tasks/shell_task.*`
- `applications/tasks/app_shell_task.*`
- `applications/shell/*`

不再讨论旧 `motor_shell_cmds.c`、旧 profile 档位，或已经退役的 backlog 模型。

## 2. 先记住四句话

1. 当前 shell 走共享 `USART1`，Windows 实测串口口径是 `COM54`。
2. 终端参数是 `1000000 / 8N1 / 无流控`。
3. Shell 只有两种状态：`TM_SHELL_ENABLE=1` 编译进镜像，或 `TM_SHELL_ENABLE=0` 完全裁掉。
4. 只要 shell 编译开启，就暴露同一套完整命令面：`help / sys / state / obs / fault / store / cfg / cal / diag`。

## 3. Shell 两态规则

### 3.1 Shell On

当 `TM_SHELL_ENABLE=1` 时：

- `shell_task`
  - 负责 `USART1` DMA transport
- `app_shell_task`
  - 负责 shell session 与同步命令执行
- `applications/shell/*`
  - 负责命令词表、banner、help 文案和输出契约

当前 live 口径里，`tiny / standard / full-diag` 仍然是构建配置概念，但**不再对应三套 Shell 命令档位**。

### 3.2 Shell Off

当 `TM_SHELL_ENABLE=0` 时：

- 不存在 shell banner
- 不存在 `help`
- 不存在 `shell_task / app_shell_task / tm_shell`
- 不存在 shell RAM / FLASH 税

这是正式产品镜像的编译期裁剪路径，不是运行态开关。

## 4. 连接方式

### 4.1 串口参数

当前工程的正式串口口径如下：

| 项目 | 当前值 |
| --- | --- |
| UART | `USART1` |
| Windows 端口 | `COM54` |
| 波特率 | `1000000` |
| 数据位 | `8` |
| 校验位 | `None` |
| 停止位 | `1` |
| 硬件流控 | `None` |

补充说明：

- 当前 Windows 机器实测端口是 `COM54`。
- 如果 Windows 重新枚举，端口号可能变化，这时以设备管理器里的实际串口号为准。
- shell 和调试输出共用同一物理串口，使用前先关闭其他串口占用程序。

### 4.2 终端设置

推荐终端工具：

- `MobaXterm`
- `Tera Term`
- `PuTTY`
- `串口调试助手`

推荐输入设置：

- 发送行尾用 `CRLF` 或 `LF`
- 不要使用 `CR only`

原因很简单：

- shell 会忽略 `\r`
- shell 以 `\n` 作为真正的“执行这一行”

如果终端只发 `CR`，你会看到已经输入了字符，但命令不会执行。

## 5. 上电后会看到什么

当前 shell 启动后会主动打印一段窄行 banner，然后进入提示符：

```text
[shell] name=FalconFoc version=v1.0.0
  image  : build=May 18 2026 12:34:56 shell=on flavor=carrier-demo
  console: transport=uart1-dma
  hint   : help=type 'help'

>
```

你需要重点看两项：

- `shell=on`
  - 说明当前镜像确实编译进了 shell
- `flavor=`
  - 当前产品 flavor

如果刚打开串口没有看到提示符，先按一次回车。

## 6. Shell 输出怎么看

新版 shell 只保留三种正式输出形态。

### 6.1 Summary Line

第一行总是结论行：

```text
[topic] key=value key=value ...
```

例如：

```text
[state] cmd=status result=ok axis=idle ctrl=vel input=traj fault=0x00000000
```

这行最重要，脚本也优先抓这一行。

### 6.2 Detail Row

细节行统一是：

```text
  label  : key=value key=value ...
```

例如：

```text
  runtime: tick=1234 threads=4 heap_free=8192 heap_min=7424 heap_total=10240
```

### 6.3 Repeated Row

同类对象会重复输出同一个 `label`：

```text
  slot   : name=motor status=OK ver=1 size=88B addr=0x080fe800
  slot   : name=enc status=EMPTY ver=- size=- addr=0x080fd800
  slot   : name=sys status=OK ver=1 size=8B addr=0x080ff800
```

这就是新版 shell 的正式阅读方式，不再使用大表格。

## 7. 当前命令面

只要 shell 编译开启，就统一提供下面这组命令：

- `help`
- `sys`
- `state`
- `obs`
- `fault`
- `store`
- `cfg`
- `cal`
- `diag`

`help` 的输出来自静态导出的命令表，按 priority 与 category 排序，因此是当前镜像真实命令面的第一手证据。

## 8. 常用命令速查

### 8.1 `help`

```text
help
```

作用：

- 列出当前镜像真正可用的命令
- 自动按类别排序
- 用于确认 `diag / cal / store / cfg` 是否已经编译进 shell-on 镜像

### 8.2 `sys`

```text
sys
sys status
sys tasks
sys reboot
```

说明：

- `sys` 与 `sys status` 等价
- `sys tasks` 只显示线程数量
- `sys reboot` 会先打印确认行，再延迟约 `20 ms` 复位

当前 `sys status` 的关键信息包括：

- summary:
  - `version=<fw-version>`
  - `shell=1`
  - `flavor=<product-flavor>`
- `runtime` 行：
  - `tick`
  - `threads`
  - `heap_free`
  - `heap_min`
  - `heap_total`
- `shell` 行：
  - `ready`
  - `queue=<used>/<depth>`
  - `ok`
  - `fail`

注意：当前 live 口径**已经没有 backlog 字段**。

### 8.3 `state`

```text
state
state show
state start
state stop
state disable
```

说明：

- `state` 与 `state show` 等价
- `state start / stop / disable` 走正式状态机事件
- `state mode ...` 已收回，命令会返回 `owner=cfg`
- 控制模式切换请使用 `cfg ctrl ...`

建议先看：

```text
state show
```

再决定是否启动或停机。

### 8.4 `obs`

```text
obs
obs all
obs iq
obs id
obs vel
obs pos
obs vbus
obs temp
obs enc
```

说明：

- `obs` 与 `obs all` 等价
- `temp` 会同时返回 `motor` 和 `driver`
- `enc` 会返回机械角、电角和角速度

### 8.5 `fault`

```text
fault
fault status
fault clear
```

说明：

- `fault` 与 `fault status` 等价
- `fault clear` 走正式状态机清故障事件
- 如果当前状态不允许清故障，会返回 `result=error axis=<...> rc=<...>`

### 8.6 `store`

```text
store
store status
store save <motor|enc|sys|all>
store load <motor|enc|sys|all>
store erase
```

当前语义：

- `store save ...`
  - 只是锁存 deferred save 请求
  - 真正的 flash 写入由 `safe_task` 在任务上下文完成
- `store erase`
  - 立即擦除持久化数据
  - 要求 `axis=idle`
- `store load motor/sys`
  - 可能改变当前运行配置
  - 要求 `axis=idle`
  - 成功后会走共享 restore-after-load 路径并清理瞬态设定值
- `store load enc`
  - 允许独立恢复
  - 不触发 motor/sys 的设定值清理路径

建议理解为三组独立参数：

- `motor`
- `enc`
- `sys`

### 8.7 `cfg`

```text
cfg
cfg show
cfg ctrl <volt|curr|vel|pos>
cfg input <inactive|pass|vel-ramp|torque-ramp|pos-filter|vel-filter|traj|tuning>
```

说明：

- `cfg show` 查看当前正式配置
- `cfg ctrl ...` 与 `cfg input ...` 都要求 `axis=idle`
- 改完如果希望重启后保留，需要执行：

```text
store save sys
```

### 8.8 `cal`

```text
cal
cal status
cal enc
cal motor all
cal motor rs
cal motor ls
cal motor flux
cal motor js
cal abort
```

说明：

- `cal status` 会给出：
  - `axis`
  - `stage`
  - `enc_valid`
  - `save_req`
- `cal motor all` 当前会从 `rs` 阶段入口启动
- `cal abort` 只有在 `axis=calib` 时才会成功

建议流程：

```text
state show
cal status
cal enc
cal status
cal abort
```

### 8.9 `diag`

```text
diag set <vel|pos|iq|vd|vq> ...
diag ramp <vel|iq|pos> ...
diag pid ...
diag param ...
```

常见示例：

```text
diag pid all show
diag pid vel kp 0.1000
diag param show
diag param rs 0.2300
diag set vel 500
diag ramp vel 1000 2000
```

当前 live 边界：

- `diag set ...`
  - 运行时目标注入
  - 可在 `axis=running` 时使用
- `diag ramp ...`
  - 运行时轨迹或斜坡注入
  - 可在 `axis=running` 时使用
- `diag pid ...`
  - `show` 任何状态可读
  - 写路径 `kp/ki/kd/lim/reset` 为 `idle-only`
- `diag param ...`
  - `show` 任何状态可读
  - 写路径为 `idle-only`

## 9. 推荐操作流程

### 9.1 第一次连机

按这个顺序最稳：

```text
help
sys status
state show
fault status
store status
```

这样你能立刻确认：

- shell 是否真的开启
- 线程和 heap 是否正常
- shell queue 是否堵塞
- 状态机是否在 `idle`
- 参数槽是 `OK` 还是 `EMPTY`

### 9.2 正常启动与停止

```text
state show
state start
state show
state stop
state show
```

如果 `state start` 被拒绝，不要先猜，直接看返回里的：

- `axis=`
- `fault=`
- `rc=`

### 9.3 修改正式配置并持久化

```text
cfg show
cfg ctrl vel
cfg input traj
store save sys
sys reboot
cfg show
store status
```

这套流程用于验证：

- 配置已改
- 请求已进入 deferred save
- 重启后配置已恢复

### 9.4 调试运行态目标与参数边界

```text
state start
diag set vel 500
diag pid all show
diag pid vel kp 0.1000
diag param rs 0.2300
state stop
```

预期是：

- `diag set vel 500`
  - 可以工作
- `diag pid vel kp 0.1000`
  - 运行中返回 `policy=idle-only`
- `diag param rs 0.2300`
  - 运行中返回 `policy=idle-only`

## 10. 使用约束与注意事项

### 10.1 当前 shell 不是 PC shell

它有意保持轻量，所以不要按 PC shell 习惯来用。

当前限制包括：

- 没有命令历史
- 没有自动补全
- 没有复杂行编辑
- 不建议依赖退格修正整行

最稳的使用方式就是：

**整行输完，再回车执行。**

### 10.2 命令长度有上限

当前单行最大长度是 `128` 字节。

如果一行太长，或者串口注入太猛，可能会看到：

```text
[shell] cmd=rx result=error rc=-7
```

这表示 transport 侧这一行没有被正常接收，或者 TX / line path 已经触发边界保护。

### 10.3 很多配置写操作要求 `axis=idle`

下面这些命令最常见的前提都是 `idle`：

- `cfg ctrl ...`
- `cfg input ...`
- `store save ...`
- `store load motor ...`
- `store load sys ...`
- `store erase`
- `cal ...`
- `diag pid ...` 写路径
- `diag param ...` 写路径

被拒绝不是 bug，而是当前保护语义。

### 10.4 `cal` 和 `diag` 可能驱动功率级

下面这些命令不只是“读状态”，它们可能真的让电机动作：

- `state start`
- `cal ...`
- `diag set ...`
- `diag ramp ...`

实机操作前请确保：

- 供电条件正确
- 功率级允许动作
- 机械端安全

## 11. 常见问题

### 11.1 打开串口后没有任何输出

先查这四项：

1. 端口是不是 `COM54`
2. 波特率是不是 `1000000`
3. 行尾是不是 `CRLF` 或 `LF`
4. 串口有没有被其他工具占用

然后做两个动作：

- 按一次回车
- 复位板子

### 11.2 `help` 里没有某条命令

优先检查两件事：

- 当前是不是 shell-on 镜像
- `help` 里是否真的导出了该命令

当前 live 口径下，不再用 `product / factory / diag` 去解释命令缺失。

### 11.3 返回 `rc=-2`

```text
[shell] cmd=<name> result=error rc=-2
```

含义：

- 命令名不存在
- 或当前镜像根本没有编译进这条命令

### 11.4 返回 `rc=-3` 或 `rc=-4`

含义：

- `rc=-3`
  - 参数个数不对
- `rc=-4`
  - 参数内容不合法

最直接的处理方法就是回到 `help`，按当前语法重输。

### 11.5 `sys status` 里 `fail` 持续增长

说明 shell 行提交有丢失。

优先排查：

- 是否在做高频命令注入
- 串口工具是否一次性灌入过长命令
- 当前输出是否太密集

建议同时看：

- `ready=`
- `queue=`
- `ok=`
- `fail=`

当前 live 口径中，**没有 backlog 可再看第二层排队状态**。

## 12. 对应代码入口

如果后续你们既要用 shell，也要继续维护它，建议从这几个文件进入：

- `applications/tasks/shell_task.*`
  - `USART1` DMA transport owner
  - RX、TX ring_buffer、line submit
- `applications/tasks/app_shell_task.*`
  - shell session owner
  - 单静态 queue、ready gate、同步命令执行
- `applications/shell/*`
  - 命令词表、banner、输出行为
- `subsys/shell/tm_shell.*`
  - 通用命令引擎与 `help` / prompt / 错误码基础行为

对应治理文档：

- [Themis_Shell与诊断输出规范.md](../governance/Themis_Shell与诊断输出规范.md)
- [Themis_Transport与Shell硬化说明.md](../governance/Themis_Transport与Shell硬化说明.md)

## 13. 一句话结论

新版 FalconFoc shell 的正确使用方法只有一句话：

**先确认 `shell=on`，再敲 `help`；`state` 负责事件，`cfg` 负责正式配置，按窄行 summary/detail 读结果。**
