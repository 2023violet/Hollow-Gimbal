# Themis FalconFoc 新版 Shell 使用手册

## 1. 文档目标

这份手册只回答一个问题：

**当前 FalconFoc 固件里的新版 shell，到底该怎么连、怎么看、怎么用。**

本文以当前 live 实现为准，覆盖：

- `applications/tasks/shell_task.*`
- `applications/tasks/app_shell_task.*`
- `applications/shell/*`

不讨论旧 `motor_shell_cmds.c` 的历史命令面，也不讨论已经退役的宽表格输出习惯。

## 2. 先记住四句话

1. 当前 shell 走共享 `USART1`，Windows 实测串口口径是 `COM54`。
2. 终端参数是 `1000000 / 8N1 / 无流控`。
3. 所有命令都是小写、空格分词、按行执行。
4. 当前镜像能用哪些命令，先看 banner 里的 `profile=`，再敲 `help`。

## 3. 连接方式

### 3.1 串口参数

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

### 3.2 终端设置

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

## 4. 上电后会看到什么

当前 shell 启动后会主动打印一段窄行 banner，然后进入提示符：

```text
[shell] name=FalconFoc version=v1.0.0
  image  : build=May 13 2026 12:34:56 profile=factory flavor=carrier-demo
  console: transport=uart1-dma
  hint   : help=type 'help'

> 
```

你需要重点看两项：

- `profile=`
  - 当前命令档位
- `flavor=`
  - 当前产品 flavor

提示符固定是：

```text
> 
```

如果刚打开串口没有看到提示符，先按一次回车。

## 5. Shell 输出怎么看

新版 shell 不再假设你有 `128` 列终端。

正式输出只有三种形态。

### 5.1 Summary Line

第一行总是结论行：

```text
[topic] key=value key=value ...
```

例如：

```text
[state] cmd=status result=ok axis=idle ctrl=vel input=traj fault=0x00000000
```

这行最重要，脚本也优先抓这一行。

### 5.2 Detail Row

细节行统一是：

```text
  label  : key=value key=value ...
```

例如：

```text
  motor  : sensor=magnetic enc_valid=1
  meas   : vbus=24.10 iq=0.002 id=-0.001
```

### 5.3 Repeated Row

同类对象会重复输出同一个 `label`：

```text
  slot   : name=motor status=OK ver=1 size=88B addr=0x080fe800
  slot   : name=enc status=EMPTY ver=- size=- addr=0x080fd800
  slot   : name=sys status=OK ver=1 size=8B addr=0x080ff800
```

这就是新版 shell 的正式阅读方式，不再使用大表格。

## 6. 统一词表

### 6.1 成功与失败

动作类命令统一看：

- `result=ok`
- `result=error`

### 6.2 布尔值

当前输出里常见两类布尔词表：

- 运行态：`0/1`
- 策略位：`on/off`

### 6.3 参数槽状态

`store status` 里最常见的状态如下：

| 字段 | 含义 |
| --- | --- |
| `OK` | 该参数组已保存且校验通过 |
| `EMPTY` | 该槽位为空，首次上电时这是正常现象 |
| `ERR_VERSION` | 版本不匹配，需要按当前固件口径重新保存 |
| `ERR_CRC` | 数据损坏 |
| `ERR_FLASH` | Flash 读写失败 |
| `ERR_MAGIC` | 槽位内容不是该参数组的合法负载 |

`EMPTY` 不是 bug，很多新板第一次上电都会看到它。

## 7. 当前三档命令面

当前 FalconFoc shell 不是“所有镜像都一样”。

它按 `profile` 分三档：

| profile | 构建口径 | 主要命令面 |
| --- | --- | --- |
| `product` | `tiny` | `sys state obs fault store cfg` 的最小维护子集 |
| `factory` | `standard` | `product + cal`，同时开放 `store/cfg` 的受控写路径 |
| `diag` | `full-diag` | `factory + diag` |

### 7.1 `product`

`product` 面向量产和最小维护闭环。

特点：

- `store` 只保留 `status`
- `cfg` 只保留 `show`
- `cal` 不存在
- `diag` 不存在

### 7.2 `factory`

`factory` 面向受控维护。

相比 `product`，新增：

- `cal`
- `store save/load/erase`
- `cfg ctrl/input`

### 7.3 `diag`

`diag` 面向开发与深诊断。

相比 `factory`，新增：

- `diag set`
- `diag ramp`
- `diag pid`
- `diag param`

## 8. 常用命令速查

### 8.1 所有镜像都自带的内建命令

```text
help
```

作用：

- 列出当前镜像真正可用的命令
- 自动按类别排序

### 8.2 `sys`

```text
sys
sys status
sys tasks
sys reboot
```

说明：

- `sys` 与 `sys status` 等价
- `sys tasks` 只显示线程数量，不显示线程列表
- `sys reboot` 会先打印确认行，再延迟约 `20 ms` 复位

最常用的是：

```text
sys status
```

它会同时带出：

- 固件版本
- 当前 `profile` / `flavor`
- RTOS tick、线程数、堆使用
- shell queue / backlog / fail 计数

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
- `state start` / `state stop` / `state disable` 走正式状态机事件
- `state` 不再承担控制模式写入口
- 控制模式切换请使用 `cfg ctrl ...`

建议先看：

```text
state show
```

再决定是否启动或切模式。

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
- `fault clear` 会走正式状态机清故障事件
- 成功路径同时清除 fault latch，并退出 `FAULT`

如果当前状态不允许清故障，命令会返回：

```text
[fault] cmd=clear result=error axis=<...> rc=<...>
```

### 8.6 `store`

所有镜像都支持：

```text
store
store status
```

`factory/diag` 额外支持：

```text
store save <motor|enc|sys|all>
store load <motor|enc|sys|all>
store erase
```

Current contract:

- `store save ...`
  - latches a deferred save request only
  - real flash write is executed later by `safe_task` in RTOS task context
- `store erase`
  - erases persisted groups immediately
  - requires `axis=idle`
- `store load motor/sys`
  - may change the active runtime configuration
  - requires `axis=idle`
  - after a successful load, the firmware runs the shared application restore
    completion path and clears transient setpoints
- `store load enc`
  - currently remains independently allowed
  - encoder payload restore does not run the motor/sys setpoint-clear path

建议理解为三组独立参数：

- `motor`
  - 电机参数、保护参数、PID 配置等
- `enc`
  - 编码器标定数据
- `sys`
  - 默认控制模式、默认输入模式

### 8.7 `cal`

只在 `factory/diag` 存在：

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

使用建议：

- 启动前先确认 `state show` 里 `axis=idle`
- 标定过程中用 `cal status` 看阶段推进
- 想中止时用 `cal abort`

补充说明：

- `cal motor all` 是当前 live 命令面的一部分
- 具体阶段推进结果以 `cal status` 的 `stage=` 和 `step:` 输出为准

### 8.8 `cfg`

所有镜像都支持：

```text
cfg
cfg show
```

`factory/diag` 额外支持：

```text
cfg ctrl <volt|curr|vel|pos>
cfg input <inactive|pass|vel-ramp|torque-ramp|pos-filter|vel-filter|traj|tuning>
```

建议把它理解为“正式配置口”。

如果你希望配置在重启后仍然生效，改完后要执行：

```text
store save sys
```

### 8.9 `diag`

只在 `diag` 存在：

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

这是开发实验面，不建议拿它当量产维护口。

Current live boundary:

- `diag set ...`
  - runtime target injection
  - may be used while `axis=running`
- `diag ramp ...`
  - runtime trajectory / ramp injection
  - may be used while `axis=running`
- `diag pid ...`
  - `show` remains available in any axis state
  - write actions such as `kp/ki/kd/lim/reset` are `idle-only`
- `diag param ...`
  - `show` remains available in any axis state
  - write actions are `idle-only`

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

- 当前镜像是 `product/factory/diag` 哪一档
- 线程和 shell 是否正常
- 状态机是否在 `idle`
- 是否有活动故障
- 参数槽是 `OK` 还是 `EMPTY`

### 9.2 正常启动与停止

推荐顺序：

```text
state show
state start
state show
state stop
state show
```

如果 `state start` 被拒绝，不要先猜，直接回看返回的：

- `axis=`
- `fault=`
- `rc=`

### 9.3 故障处理

推荐顺序：

```text
fault status
state show
fault clear
fault status
```

如果还清不掉，通常说明：

- 当前不在允许清故障的状态
- 或根故障条件还存在

### 9.4 修改正式配置并持久化

仅适用于 `factory/diag`：

```text
cfg show
cfg ctrl vel
cfg input traj
store save sys
sys reboot
cfg show
store status
```

Current note:

- `store save sys` returns after the request is accepted.
- The actual flash write is drained later by the background safe task.

这套流程用于验证：

- 配置已改
- 参数已保存
- 重启后已恢复

### 9.5 编码器或电机标定

仅适用于 `factory/diag`：

```text
state show
cal status
cal enc
cal status
store save enc
```

或：

```text
cal motor all
cal status
```

如果要中途退出：

```text
cal abort
cal status
```

补充说明：

- `cal enc` / `cal motor ...` 只有在命令提交后轴真正进入 `calib`，shell 才返回 `result=ok`
- 如果事件未被状态机接收，命令层会直接返回 `result=error`

### 9.6 开发调试

仅适用于 `diag`：

```text
diag pid all show
diag param show
obs all
sys status
```

建议把 `sys status` 也一起看上，方便观察：

- `queue`
- `backlog`
- `ok`
- `hold`
- `fail`

## 10. 使用约束与注意事项

### 10.1 当前 shell 不是 PC 终端

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

这表示 transport 侧这一行没有被正常接收。

### 10.3 很多命令要求 `axis=idle`

下面这些命令最常见的前提都是 `idle`：

- `cfg ctrl ...`
- `cfg input ...`
- `store save ...`
- `store load ...`
- `store erase`
- `cal ...`
- `diag pid ...` 写路径
- `diag param ...` 写路径

如果当前处于 `running` 或 `calib`，命令被拒绝是正常保护行为。

补充说明：

- `diag set ...`
  - 不在这条 `idle-only` 保护里
- `diag ramp ...`
  - 不在这条 `idle-only` 保护里
- 这两类命令属于 `diag` 实验口里的运行时目标注入
- `diag pid/param` 的写操作属于结构性改写，当前 live 口径已收回 `idle-only`

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

如果当前只有调试器 `3.3V` 供电，没有外部功率电源，那么：

- shell 本身
- 状态查询
- 参数读写

这些路径仍然可以验证；

但涉及电机驱动、闭环、标定成功与否的结果，不能当作完整实机闭环结论。

## 11. 常见问题

### 11.1 打开串口后没有任何输出

先查这四项：

1. 端口是不是 `COM54`
2. 波特率是不是 `1000000`
3. 行尾是不是 `CRLF` 或 `LF`
4. 串口有没有被其他工具占用

再做两个动作：

- 按一次回车
- 复位板子

### 11.2 `help` 里没有某条命令

先看 banner 里的 `profile=`。

最常见情况：

- `profile=product`
  - 没有 `cal`
  - 没有 `diag`
- `profile=factory`
  - 没有 `diag`

这不是命令丢了，而是当前镜像档位本来就不开放。

### 11.3 返回 `rc=-2`

```text
[shell] cmd=<name> result=error rc=-2
```

含义：

- 命令名不存在
- 或该命令不在当前镜像里

优先检查：

- 拼写
- 当前 `profile`
- `help` 是否列出了这条命令

### 11.4 返回 `rc=-3` 或 `rc=-4`

含义：

- `rc=-3`
  - 参数个数不对
- `rc=-4`
  - 参数内容不合法

最直接的处理方法就是回到 `help`，按当前语法重输。

### 11.5 返回 `profile=product`

例如：

```text
[store] cmd=save result=error profile=product
```

或：

```text
[cfg] cmd=ctrl result=error profile=product
```

含义：

- 当前命令对象存在
- 但当前镜像档位不允许这个写操作

如果你需要这些能力，请切换到 `factory` 或 `diag` 镜像。

### 11.6 `store status` 看到 `EMPTY`

这是正常现象，常见于：

- 首次上电
- 还没做过 `store save ...`
- 做过 `store erase`

只有当你明确已经保存过，重启后仍然不是 `OK`，才需要继续排查。

### 11.7 `sys status` 里 `fail` 持续增长

说明 shell 输入有丢失。

优先排查：

- 是否在做高频命令注入
- 串口工具是否一次性灌入过长命令
- 当前输出是否太密集

建议同时看：

- `queue=`
- `backlog=`
- `fail=`

这三个字段一起判断最可靠。

## 12. 对应代码入口

如果后续你们既要用 shell，也要继续维护它，建议从这几个文件进入：

- `applications/tasks/shell_task.*`
  - `USART1` DMA transport owner
- `applications/tasks/app_shell_task.*`
  - shell session、queue、backlog owner
- `applications/shell/*`
  - 命令词表、banner、profile gating、输出行为
- `subsys/shell/tm_shell.*`
  - 通用命令引擎与 `help` / prompt / 错误码基础行为

对应治理文档：

- [Themis_Shell与诊断输出规范.md](../governance/Themis_Shell与诊断输出规范.md)
- [Themis_产品Shell命令层重构方案.md](./Themis_产品Shell命令层重构方案.md)

## 13. 一句话结论

新版 FalconFoc shell 的正确使用方法只有一句话：

**先看 `profile`，再敲 `help`；`state` 负责事件，`cfg` 负责正式配置，按窄行 summary/detail 输出读结果。**
