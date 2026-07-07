# Themis 产品 Shell 命令层重构方案

## 1. 文档目标

这份文档是 Themis 当前产品层对 **shell 命令层** 的正式重构方案。

它不讨论 `subsys/shell/` 机制层是否成立，而是专门回答下面四件事：

1. 为什么 Themis 需要把产品 shell 命令层和 shell 机制层拆开治理。
2. 面向量产产品时，shell 命令层的正式目标、边界、分档和资源纪律应该是什么。
3. `product / factory / diag` 三档 shell 命令层分别应暴露什么能力，不应暴露什么能力。
4. `FalconFoc_AT32` 作为第一份落地样板，应如何从当前命令层迁移到 Themis 正式口径。

这份文档的正式立场只有一句：

**Themis 保留轻量、可审计、可裁剪的 shell 机制层，但产品命令层必须按产品预算、产品责任和产品词表重构，不再允许把开发实验台 shell 直接长期常驻到量产镜像。**

## 2. 适用范围

适用于：

- `applications/shell/`
- `applications/tasks/` 中仍暂存的产品 shell 命令面
- APP shell banner
- APP shell help 文案
- 产品层状态查询、维护命令、参数修改命令
- 与 shell 命令层直接耦合的应用层资源预算、输出 contract 与 profile 开关

不直接裁决：

- `subsys/shell/tm_shell.*` 的机制正确性
- panic/raw console
- `assert / log` 的独立输出路径
- `uart_task` 的周期遥测输出

但如果这些路径与 APP shell 共用同一物理 UART、共用同一输出风格或共用同一资源预算，
则它们后续必须与本方案保持一致的资源纪律与词表纪律。

## 3. 当前问题定义

### 3.1 当前 shell 机制层与命令层已经发生目标错位

当前 Themis/FalconFoc 的 `tm_shell` 机制层方向是成立的：

- 静态注册
- heap-free command table
- 同步命令执行
- borrowed transport callback

这些机制都符合 Themis 对小 MCU 的正式工程立场。

问题不在机制层，而在当前产品命令层已经默认按“开发实验台 shell”设计：

- 命令面过宽
- RAM 常驻过大
- ROM 格式化税过高
- 输出过宽
- 词表不稳定
- 把低频的人机命令路径做成了高抽象、高格式化、高常驻成本的系统

正式裁决：

**当前 shell 命令层的问题，不是“写得差”，而是“目标错了”。**

### 3.2 `FalconFoc_AT32` 暴露出的真实问题

以 `FalconFoc_AT32` 当前实现为例，问题已经不是审美问题，而是预算问题。

当前镜像基线已明确显示：

- shell 命令层并不是只占一个 `.shell_cmd` 描述区
- 真正昂贵的是：
  - 命令节点表
  - hash map 桶与注册表
  - 大块帮助文案与格式化字符串
  - 宽输出导致的 TX pool / queue 预算
  - `snprintf()` / `strtof()` 级联带入的 libc 解析与格式化链路

如果继续把这类命令层按 full-open 方式留在 `128K ROM / 32K RAM` 级产品里，
那么产品逻辑、保护逻辑、通信逻辑、后续状态机扩展都会被 shell 长期侵占预算。

正式裁决：

**产品 shell 命令层必须从“默认全功能常驻”改成“按档位裁剪、按责任暴露、按资源计费”。**

## 4. 正式设计原则

### 4.1 机制层与命令层分治

Themis 将 shell 拆成两层：

- `subsys/shell/`
  - 负责共享机制
  - 负责静态命令注册、命令解析、输出 hook、session owner 配合点
- `applications/shell/`
  - 负责产品词表
  - 负责产品帮助文本
  - 负责产品级命令分档
  - 负责产品级输出 contract

正式规则：

- 机制层不拥有产品词表
- 产品层不重新发明第二套 shell 机制
- 产品层只拥有 vocabulary、policy、budget 和 producer-side bridge

### 4.2 Shell 是产品维护面，不是第二套业务系统

Themis 正式不接受下面这种设计倾向：

- 为了 shell 好看，再做一层复杂命令框架
- 为了 shell 通用，再引入一层长期不用的抽象
- 为了 help 漂亮，再把输出做成宽表格
- 为了开发方便，把所有内部能力都开放成长期常驻命令

正式定位应当是：

**shell 是产品维护面，不是第二套产品逻辑系统。**

它只能承担：

- 观测
- 有边界的维护
- 有边界的受控动作
- 有边界的参数编辑

### 4.3 低频路径不为炫技交 RAM 税

shell 是低频、人工触发、非热路径。

因此正式裁决如下：

- shell 命令分发不追求为了 O(1) 而长期交高额 RAM 税
- shell 不得为了宽表格输出而推高 TX pool 常驻成本
- shell 不得为了“命令很全”而长期保留大量极少使用的 setter / 调试命令

如果某项设计收益主要体现在：

- 更酷
- 更像 PC CLI
- 更像大项目
- 更像实验台

而不是体现在：

- 量产可维护性
- 产线效率
- 现场维护效率
- 真实资源节省

则 Themis 统一裁决为：

**不值得。**

### 4.4 输出 contract 先服从资源纪律，再谈美观

Themis 正式 shell 输出规则已经在：

- `docs/governance/Themis_Shell与诊断输出规范.md`

中冻结。

本方案不再重新定义新的格式体系，而是要求产品命令层严格回归现有规范：

- `summary line`
- `detail row`
- `repeated row`

正式禁止：

- ASCII 大表格
- 宽 banner
- 装饰性 prose
- 一次性大段帮助文本
- 为了“整齐”而拉长字段宽度

一句话裁决：

**好看的 shell，不是宽，而是稳、短、清楚、词表一致。**

## 5. 正式分档模型

### 5.1 三档总览

Themis 产品 shell 命令层正式冻结为三档：

- `product`
- `factory`
- `diag`

这三档不是“谁高级谁低级”，而是三种不同责任域：

| 档位 | 面向对象 | 正式目标 | 资源态度 |
| --- | --- | --- | --- |
| `product` | 量产产品 / 常规现场维护 | 只保留最小必要观察与维护面 | 最严格 |
| `factory` | 产线 / 返修 / 受控维护 | 在 `product` 基础上增加受控校准与保存恢复 | 严格 |
| `diag` | 开发 / 深诊断 / 实验台 | 允许完整调试、扩展观测与实验性命令 | 最宽松 |

### 5.2 `product` 档

`product` 是量产正式档。

它的目标不是“什么都能改”，而是：

- 设备可确认
- 状态可观测
- 故障可判断
- 最小维护动作可执行

`product` 档允许：

- 版本与镜像信息
- 运行状态观测
- 故障状态观测
- 关键保存状态观测
- 极少量安全动作

`product` 档禁止：

- 大范围参数编辑
- 实验性模式切换
- 宽表格批量观测
- 连续调参型命令
- 面向算法开发的调试命令

正式目标是：

**量产机上的 shell，应该像一把可靠的小扳手，而不是一整个实验室工具箱。**

### 5.3 `factory` 档

`factory` 是产线 / 返修 / 受控维护档。

它的责任是：

- 在 `product` 基础上补齐受控维护能力
- 允许必要的校准
- 允许必要的保存恢复
- 允许少量经过约束的参数写入

`factory` 档允许：

- 标定入口
- 标定状态
- 保存 / 加载 / 擦除
- 少量关键参数写入
- 受控模式切换

`factory` 档仍然禁止：

- 全开放参数面
- 实验性轨迹或控制注入
- 高成本宽输出
- 与开发便利性强绑定的命令

正式定位：

**factory 是受控维护面，不是折中的 diag。**

### 5.4 `diag` 档

`diag` 是开发 / 深诊断档。

它允许：

- 更宽的观测面
- 更完整的 setter 面
- 更深的调试与实验命令
- 更高的输出频度和更高的帮助完整度

但即便是 `diag`，也不代表可以放弃 Themis 规则。

`diag` 仍必须遵守：

- 分层边界
- 输出 contract
- owner 约束
- 资源证据链

正式裁决：

**diag 可以更重，但不能更乱。**

## 6. 正式命令分层

### 6.1 命令分类原则

Themis 不再按“作者想到什么命令就挂进去”的方式扩展命令层。

正式命令分类必须先回答：

1. 这条命令属于观测、维护、校准、配置，还是开发实验。
2. 这条命令应出现在哪个档位。
3. 这条命令执行后，谁承担结果 owner。
4. 这条命令是否值得为量产常驻交 ROM/RAM 税。

### 6.2 推荐一级命令集合

Themis 产品 shell 命令层正式推荐使用短一级命令集合：

- `sys`
- `state`
- `obs`
- `fault`
- `store`
- `cal`
- `cfg`
- `diag`

说明：

- `sys`
  - 版本、镜像、重启、运行时间、基础系统摘要
- `state`
  - 产品主状态、模式、关键运行态
- `obs`
  - 只读观测量
- `fault`
  - 故障状态、故障清除、故障摘要
- `store`
  - 保存恢复、槽位状态
- `cal`
  - 产线/维护级校准入口与状态
- `cfg`
  - 少量正式配置项
- `diag`
  - 只在 `diag` 档出现的开发扩展命令

### 6.3 正式命令与档位映射

推荐冻结口径如下：

| 一级命令 | `product` | `factory` | `diag` |
| --- | --- | --- | --- |
| `sys` | 是 | 是 | 是 |
| `state` | 是 | 是 | 是 |
| `obs` | 是 | 是 | 是 |
| `fault` | 是 | 是 | 是 |
| `store` | 只读 | 读写 | 读写 |
| `cal` | 否 | 是 | 是 |
| `cfg` | 极小子集 | 小子集 | 较完整 |
| `diag` | 否 | 否 | 是 |

### 6.4 FalconFoc_AT32 的当前命令重映射建议

以 `FalconFoc_AT32` 当前命令集为例：

- `motor`
- `mode`
- `set`
- `ramp`
- `get`
- `pid`
- `param`
- `calib`
- `store`

正式建议重映射为：

- `sys`
  - 吸收 `ver uptime reboot mem`
- `state`
  - 吸收 `motor status`、`mode`
- `obs`
  - 吸收 `get`
- `fault`
  - 吸收 `motor clr`、故障相关摘要
- `store`
  - 吸收 `store`
- `cal`
  - 吸收 `calib`
- `cfg`
  - 吸收 `param` 中少量正式可配置项
- `diag`
  - 吸收 `set`、`ramp`、`pid`、较宽 `param` 面

正式裁决：

**当前 FalconFoc 的 `set / ramp / pid / 大 param` 更像开发实验面，不应继续默认常驻在量产档。**

## 7. 正式输出模型

### 7.1 总原则

所有产品 shell 命令都必须满足：

- 第一行给结论
- 后续只给短行
- 同类对象重复行输出
- 单行宽度受控
- 帮助文案短而稳

### 7.2 Summary Line

每条命令正式输出必须以：

```text
[topic] cmd=<subcommand> result=<ok|error> key=value ...
```

开头。

推荐字段：

- `cmd=`
- `result=`
- `status=`
- `mode=`
- `count=`
- `rc=`

### 7.3 Detail Row

长信息拆成：

```text
  label  : key=value key=value ...
```

推荐 label：

- `state`
- `obs`
- `slot`
- `fault`
- `policy`
- `note`
- `hint`

### 7.4 Repeated Row

多个对象统一使用 repeated row：

```text
  slot   : name=motor status=valid version=1 data=64B
  slot   : name=enc status=empty version=- data=-
```

正式禁止：

- ASCII 表头
- 宽横线
- 对齐墙

### 7.5 Banner

APP shell banner 是唯一允许主动多行输出的内容。

正式推荐冻结为：

```text
[shell] name=<product> version=<ver>
  image  : build_date=<date> build_time=<time> shell=on flavor=<flavor>
  console: uart=<name> baud=<baud> transport=<mode>
  hint   : next='help'
```

正式禁止：

- ASCII art
- 装饰字符墙
- 冗长欢迎词

### 7.6 Help

help 文案正式要求：

- 动词清楚
- 对象清楚
- 不追求“像 man page”
- 按组输出
- 只给当前档位真正可见的命令

正式建议：

- `product` 只显示极小命令面
- `factory` 额外显示校准/保存恢复
- `diag` 才显示完整扩展命令

## 8. 正式资源纪律

### 8.1 命令层预算必须单独结算

从本方案起，shell 命令层不得继续混在“整体固件体重”里被模糊看待。

必须单独结算至少四类成本：

- 命令描述区
- 命令静态节点 / map / 表
- 输出缓冲与 TX handoff 常驻内存
- 格式化 / 解析带来的 libc 链路 ROM 税

### 8.2 命令分发优先用更便宜的结构

对于 `product / factory` 档，正式优先级是：

1. 显式小表 + 线性查找
2. 小规模静态 descriptor 表
3. 只有 `diag` 或确有必要时，才接受较重 map 结构

理由不是“线性查找更高级”，而是：

- 命令数量小
- 调用频度低
- RAM 更贵

### 8.3 输出宽度必须反向约束 transport 预算

当前很多 shell 预算膨胀，并不是 transport 本身有问题，而是命令层默认写出宽行和大段文本。

正式规则：

- 命令层先收缩输出
- 再决定 `TX_BLOCK_SIZE`
- 再决定 `TX_QUEUE_CAP`

不得继续反过来：

- 先开大池
- 再容忍宽输出

### 8.4 `product` 档禁止重型格式化依赖

正式建议：

- `product` 档尽量避免命令层引入 `snprintf()` 级别格式化
- `product` 档尽量避免大范围浮点字符串解析
- `factory` 档谨慎开放
- `diag` 档可接受更完整格式化，但必须诚实记账

### 8.5 资源目标口径

对于 `128K ROM / 32K RAM` 级产品，正式资源态度冻结为：

- shell 命令层不是“剩多少用多少”
- shell 命令层是和产品业务抢预算的正式子系统
- 量产镜像必须优先给主业务、保护逻辑、通信逻辑、升级空间留预算

如果产品要求：

- 至少预留 `50K ROM` 给业务扩展
- 且最终镜像还必须再保留约 `20K` 安全余量

那么正式裁决就是：

**量产 shell 命令层只能走 `product` 或 `factory`，不能默认常驻 `diag`。**

## 9. 正式目录与 owner 方案

### 9.1 正式目录落位

Themis 产品 shell 命令层正式落位应为：

```text
applications/
└─ shell/
   ├─ app_shell_cmd_root.c
   ├─ app_shell_cmd_sys.c
   ├─ app_shell_cmd_state.c
   ├─ app_shell_cmd_obs.c
   ├─ app_shell_cmd_fault.c
   ├─ app_shell_cmd_store.c
   ├─ app_shell_cmd_cal.c
   ├─ app_shell_cmd_cfg.c
   ├─ app_shell_cmd_diag.c
   ├─ app_shell_profile.h
   └─ app_shell_output.h
```

说明：

- `applications/shell/` 正式拥有产品命令 vocabulary
- `applications/tasks/` 不再长期寄存产品命令面
- `tasks/` 只保留 session owner、transport glue、task cadence

### 9.2 owner 划分

正式 owner 应冻结为：

- `subsys/shell/`
  - shell engine
  - shared output helper
  - shared shell task/session owner
- `applications/shell/`
  - product vocabulary
  - product help
  - product profile gating
  - product output contract glue
- `applications/services/`
  - 被 shell 调用的正式语义 owner
- `applications/tasks/`
  - shell owner task、transport handoff、调度与阻塞等待

### 9.3 禁止事项

正式禁止：

- 在 `applications/tasks/` 长期堆积产品命令实现
- 在 `applications/shell/` 内部绕过 service 直接写板级资源
- 为 shell 单独再发明第二套 command framework
- 在命令文件里私自复制业务状态名词表

### 9.4 Profile 开关与构建契约

这份方案不仅要求命令“逻辑上分档”，还要求它们在构建期可被明确裁剪。

正式建议：

- `product`
  - 只编译最小命令面
- `factory`
  - 在 `product` 基础上增量开启校准与保存恢复
- `diag`
  - 才编译完整调试命令面

推荐冻结为单一 profile 选择，而不是多个布尔开关自由拼装。

例如：

```text
TM_SHELL_PROFILE_PRODUCT
TM_SHELL_PROFILE_FACTORY
TM_SHELL_PROFILE_DIAG
```

正式理由：

- 单一 profile 更容易记账
- help 面天然与命令面一致
- 不会出现“命令裁掉了，但帮助还在”或“帮助没显示，但命令还能调”的漂移
- 后续做 tri-profile ROM/RAM 对账时口径更稳定

正式禁止：

- 让量产镜像默认同时打开 `factory + diag`
- 用零散 `#ifdef` 在各文件里临时散落 profile 判断
- 构建 profile、help profile、命令注册 profile 三套口径各写各的

## 10. FalconFoc_AT32 作为首个样板的落地要求

### 10.1 当前样板定位

`FalconFoc_AT32` 不是这份方案的唯一对象，但它是第一份落地样板。

这意味着它需要同时承担两件事：

1. 自己先从旧命令层迁移出来
2. 为后续 Themis 产品给出第一份可信样板

### 10.2 当前命令面的正式裁决

当前 `FalconFoc_AT32` 命令面中：

- `motor status`
- `get`
- `store status`

属于产品化后仍值得保留的方向。

而下面这些能力，默认应降到 `diag` 或部分降到 `factory`：

- `ramp`
- `pid`
- 大部分 `param`
- 宽输出 `show`
- 面向实验台的轨迹 / 目标注入

### 10.3 推荐迁移顺序

推荐按下面顺序迁移：

1. 先收输出格式
   - 把所有大表格改成 summary/row
2. 再收命令分档
   - 标注哪些命令属于 `product/factory/diag`
3. 再拆目录 owner
   - 从 `applications/tasks/` 迁到 `applications/shell/`
4. 最后再收资源
   - 去重命令
   - 缩 descriptor
   - 缩 TX 预算
   - 缩格式化依赖

正式原因：

- 先收格式，最容易见效且最不容易改错
- 先收分档，才能决定后续哪些代码还值得保留
- 先做 owner 迁移，后面资源治理才不会反复改落位

### 10.4 FalconFoc_AT32 三档样板命令面

为了让后续实现不再靠口头理解，`FalconFoc_AT32` 第一版推荐先冻结成下面这张样板表。

| 一级命令 | `product` | `factory` | `diag` | 说明 |
| --- | --- | --- | --- | --- |
| `sys` | `ver uptime reboot` | 同 `product` | 同 `factory` | 保留基础镜像与维护入口 |
| `state` | `motor status mode` | 同 `product` | 扩展调试状态 | 量产面只保留关键运行态 |
| `obs` | 关键只读观测 | 增量观测 | 完整观测 | 不再默认大批量宽输出 |
| `fault` | `fault status clear` | 同 `product` | 扩展 portrait | 现场先看故障，不先调参 |
| `store` | `store status` | `status save load erase` | 同 `factory` | 保存恢复属于受控维护面 |
| `cal` | 无 | `cal start/status/apply` | 扩展校准 | 标定不进入量产常驻面 |
| `cfg` | 极少正式配置项 | 小规模正式配置项 | 较完整参数面 | 大多数参数写入不再常驻量产档 |
| `diag` | 无 | 无 | `set ramp pid param dump` | 开发实验面整体后撤到 `diag` |

这张表不是示意图，而是后续代码迁移的第一份正式样板口径。

如果后续某条命令想进入 `product`，必须先回答：

1. 它是不是现场维护闭环真的必需。
2. 它是否会显著增加 ROM/RAM 常驻成本。
3. 它是否会把算法实验能力错误地下放到量产镜像。

## 11. 验收口径

### 11.1 架构验收

必须满足：

- 机制层与命令层 owner 分离
- `applications/shell/` 成为正式产品命令面 owner
- `product / factory / diag` 三档边界明确
- 帮助与输出不再漂移

### 11.2 输出验收

必须满足：

- 所有命令回归 `summary + row`
- 无 ASCII 大表格
- 无宽 banner
- help 文案短而稳定
- 当前档位只显示当前档位可见命令

### 11.3 资源验收

必须满足：

- 命令层 RAM/ROM 有单独账本
- `product` 档明显轻于当前 full-open 命令面
- `factory` 档明显轻于 `diag`
- 命令层不再无上限侵占量产预算

### 11.4 样板验收

`FalconFoc_AT32` 作为首个样板，至少应验证：

- `product` 档可以完成最小维护闭环
- `factory` 档可以完成校准与保存恢复闭环
- `diag` 档保留开发便利性
- 三档帮助、输出、资源口径一致且可解释

## 12. 最终裁决

1. Themis 保留现有 shell 机制层方向，不再错误地把资源问题归咎于 `tm_shell` 本身。
2. Themis 当前真正要重构的是产品命令层，而不是再发明第三套 shell 机制。
3. 产品 shell 命令层正式冻结为 `product / factory / diag` 三档。
4. 所有产品 shell 输出都必须回到现有 Themis 正式输出规范，不再允许 ASCII 大表格和宽输出成为默认形态。
5. `FalconFoc_AT32` 作为首个样板，必须先完成输出收口、命令分档和 owner 迁移，再进入字节级资源治理。

一句话结论：

**Themis 的 shell 必须从“开发实验台全能命令面”回到“产品可维护、预算可解释、输出可冻结”的正式产品命令层。**

## 13. 附：后续实施时必须同步冻结的三份事实

这份方案进入实现阶段后，至少还必须把下面三份事实同步冻结，否则命令层仍会继续漂移：

1. profile 到命令表的映射事实
   - 哪些命令编进 `product / factory / diag`
2. profile 到 help 输出的映射事实
   - 当前镜像只能显示当前档位真实存在的命令
3. profile 到资源账本的映射事实
   - 至少记录 ROM、RAM、栈、TX budget 四类成本

正式要求是：

**命令面、帮助面、资源面必须共用同一份 profile 真值，而不是三套口头理解。**
