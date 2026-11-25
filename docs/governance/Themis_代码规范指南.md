# Themis 代码规范指南

> **V2 适用范围（2026-11-24）**：Legacy Themis 固件（`applications/`、`bsp/`、`components/`、`subsys/`、FreeRTOS、Shell）已删除，只保留在 Git tag `legacy-final`。本文中这些目录与分层示例属于历史背景；V2 目录与职责以 [AGENTS.md](../../AGENTS.md) 第 3 节为准。命名、注释、文件布局、错误处理与验证纪律等通用规则继续适用。

## 第一章：代码规范说明

### 1.1 文档定位

本文是 Themis 当前仓库的正式代码规范。

它不是泛泛而谈的 C 语言建议，而是面向当前工程真实问题的落地规则，重点约束：

- 代码应该放在哪一层
- 接口应该怎么命名和分工
- 标准设备生命周期应该怎么走
- RTOS / ISR / 阻塞 / 回滚应该怎么写
- generated code、镜像配置、正式文档如何保持一致

### 1.2 适用范围

本文适用于 Themis 仓库内所有正式维护的分层框架代码，尤其包括：

- `applications/`
- `bsp/`
- `components/`
- `device/`
- `drivers/`
- `subsys/`
- `include/`
- `docs/`

默认不要求改写厂商或工具生成目录的风格，但这些目录的边界必须被尊重：

- `project/`
- `libraries/`

下面这些 portability-first / generic library 代码，不应机械套用本文的分层前缀、
`tm_device` 生命周期或 RTOS 约束，而应优先遵守独立规范：

- `components/utils/`
- 未来新增的可移植协议库，例如 `canopen`、纯 parser、纯 codec、纯 container

V2 中对应的可移植目录是 `core/`、`runtime/`、`calibration/`、`sensors/`、`g3/`、`protocol/`，其依赖边界见 [AGENTS.md](../../AGENTS.md) 第 8 节。

### 1.3 规则优先级

冲突时按下面顺序执行：

1. 系统级与仓库级强制规则
2. `AGENTS.md`
3. `docs/architecture/FalconFOC_Project_Charter_and_Engineering_Memory_v1.0.md`
4. 已成立的架构文档、分层文档、ADR
5. 仓库根目录 `.clang-format`
6. 本文

### 1.4 术语约定

- `必须`：硬约束，违反后必须整改
- `建议`：推荐约束，允许极少数例外，但要能讲清原因
- `标准设备`：进入 `tm_device_*` 统一生命周期模型的设备类
- `board fact`：板级事实，例如引脚、默认总线、默认地址、默认实例 owner
- `generated truth`：图形化工具生成代码里已经成立的底层真值

### 1.5 Themis 的基本立场

- 边界优先于方便
- 语义优先于习惯
- 显式契约优先于隐式假设
- 长期维护优先于短期“先跑起来”
- 先把 owner、生命周期、上下文说明白，再补业务逻辑

### 1.6 两份底层规范的分工

- `Themis_代码规范指南.md`
  - 裁决分层、owner、命名、返回值、ISR、blocking、验证与文档同步。
- `Themis_C与H文件布局与注释书写规范.md`
  - 裁决 `.c/.h` 的布局、头文件入口手册化写法、API 分组、注释颗粒度与视觉组织。

一旦出现冲突，语义、owner、生命周期、上下文、验证问题以本文为准；文件布局、头文件注释、
public symbol 注释、API 分组、字段注释样式以 `Themis_C与H文件布局与注释书写规范.md` 为准。

## 第二章：代码框架

### 2.1 顶层目录职责

| 目录 | 正式职责 |
| --- | --- |
| `applications/` | 产品业务编排、产品服务、应用任务、应用启动入口 |
| `bsp/` | 板级事实、板级默认实例、板级 transport glue、早期 console glue |
| `components/` | 可复用领域组件，不承载板级事实 |
| `components/utils/` | portability-first 工具组件 |
| `device/` | 统一设备模型、typed device class、northbound 设备语义 |
| `drivers/` | southbound 芯片驱动、IRQ/DMA/register/runtime bridge |
| `subsys/` | framework 机制层，例如 export、log、ipc、time、daemon |
| `include/` | 全局公共头、聚合配置头、兼容头 |
| `docs/` | 正式架构、治理、设计、审查、维护文档 |

### 2.2 正式分层链路

Themis 当前推荐的典型调用链如下：

```text
applications/tasks
    -> applications/services
        -> bsp
            -> components
                -> device
                    -> drivers
                        -> vendor / generated code
```

例如当前最小蜂鸣器和显示链路：

```text
display_task -> display_service -> bsp_display -> ssd1316 -> dev_i2c -> drv_i2c
buzzer_task  -> buzzer_service  -> bsp_buzzer  -> buzzer  -> dev_pwm -> drv_pwm
uart_task    -> adc_service     -> dev_adc     -> drv_adc
```

### 2.3 各层正式职责

`applications/` 负责：

- 产品语义
- 页面切换、提示音策略、状态机、业务协调
- RTOS 任务 owner

`bsp/` 负责：

- 当前板子到底接了什么
- 默认实例如何组装
- 上层如何通过稳定入口访问当前板子的默认资源

`components/` 负责：

- 可复用控制器逻辑或领域逻辑
- 通过 backend ops / bus ops / 回调表解耦底层

`device/` 负责：

- 统一 northbound 语义
- `tm_device_ops` 适配
- typed API 的契约和错误码口径

`drivers/` 负责：

- 真正 southbound 硬件控制
- 寄存器、DMA、IRQ、时钟、状态机运行期桥接

### 2.4 禁止的落位方式

下面这些做法在 Themis 中默认判为错误：

- 把产品业务逻辑写进 `drivers/`
- 把板级事实写进 `components/`
- 把产品服务塞进 `subsys/`
- 在 `applications/` 直接 include `drv_xxx.h`
- 在 `components/utils/` include `cmsis_os2.h`
- 用 `common`、`helper`、`misc`、`module` 这种模糊目录作为长期归宿

### 2.5 推荐的文件骨架

头文件建议保持下面的组织顺序：

```text
文件头注释
#pragma once
Includes
extern "C"（如需要）
Public Macros / Constants
Public Types
Public Functions（按使用阶段分组）
```

源文件建议保持下面的组织顺序：

```text
文件头注释
Includes
Private Macros
Private Types
Private State
Private Helpers
Public Functions
```

关于 `.c/.h` 的正式文件布局、头注释深度、include 紧凑规则、API 分组规则、
static state / struct 的视觉组织规则，统一以：

- `docs/governance/Themis_C与H文件布局与注释书写规范.md`

为准。

### 2.6 应用层推荐框架

当前正式应用层目录形态为：

```text
applications/
├─ README.md
├─ app_main.c
├─ config/
├─ resources/
├─ services/
└─ tasks/
```

约束如下：

- `README.md` 负责说明应用层目录约定、模块命名和新增文件放置原则
- `app_main.c` 负责启动编排
- `config/` 负责应用层共享配置头，不承接运行态业务逻辑
- `resources/` 负责字体、图片和其他应用级静态资源
- `services/` 负责产品侧语义和策略
- `tasks/` 负责 RTOS 线程 owner、循环、阻塞等待、事件分发
- 不要把 service 做成单纯的 1:1 转发壳
- 不要把 task 写成业务细节垃圾桶

## 第三章：排版格式和注释

### 3.1 `.clang-format` 是硬约束

所有正式维护的 `.c/.h` 文件必须服从仓库根目录 `.clang-format`。

补充裁决：

- 这里说的“正式维护”指 Themis 自有目录，例如 `applications/`、`bsp/`、`components/`、
  `device/`、`drivers/`、`include/`、`subsys/`、`tests/`。
- `project/`、`libraries/`、vendor/no-touch 边界以及生成资源，不应被误算进
  `.clang-format` 真实性评估。
- 日常本地格式化统一使用 `scripts/format_themis.ps1`；默认只处理 changed/untracked 文件，
  需要整包整理时再显式传 `-AllTracked`。

当前关键格式约束包括：

- 4 空格缩进
- 不使用 Tab
- `ColumnLimit: 100`
- `PointerAlignment: Right`
- `SortIncludes: Never`
- `BreakBeforeBraces: Custom`

### 3.2 include 顺序

`.c` 文件必须先 include 自己的头文件，然后再 include 其他头。

Themis 当前正式口径不再鼓励“按 C 标准库 / RTOS / vendor / Themis 分四段、段间都空一行”的旧写法。

现行裁决如下：

1. 自己的头文件
2. 与本模块最直接相关的契约头 / 同域头
3. RTOS / vendor / 第三方环境头
4. C 标准库头

`.h` 文件只 include 对外声明真的需要的头文件。

禁止依赖“别人顺手 include 过所以我也能编译”的隐式 include。

include 的紧凑布局、空行例外条件与示例，以：

- `docs/governance/Themis_C与H文件布局与注释书写规范.md`

为准。

### 3.3 空行和分段

- 同一逻辑块之间用 1 个空行分隔
- 不连续堆 2 行以上空行
- 私有宏、私有状态、私有 helper、公共函数之间应保留节分隔
- 长函数内部可以通过小段空行帮助阅读，但不要靠空行代替分层设计
- 所有正式维护的 `.c/.h` 结尾都必须保留仓库统一的 `end of file` 闭合行

正式节分隔注释统一使用单行样式，例如：

```c
/* Includes --------------------------------------------------------------------- */
/* Private Helpers -------------------------------------------------------------- */
/* Public Functions ------------------------------------------------------------- */
```

禁止继续新增下面这种三行 banner：

```c
/* Includes
 * -------------------------------------------------------------------------------------
 */
```

原因不是“好不好看”，而是：

- diff 噪音大
- formatter 不会帮你修正语义错误
- 全仓搜索时节标题不稳定
- 同一文件内极易出现长度飘移和收尾破损

批量整理受管代码时，统一通过 `scripts/format_themis.ps1` 的 banner 归一化逻辑收口，
不要手写各自不同的横线长度。

关于 banner 样式、空行节奏与 `end of file` 闭合规则，统一以：

- `docs/governance/Themis_C与H文件布局与注释书写规范.md`

为准。

### 3.4 文件头注释

所有正式维护的 `.c/.h` 文件必须保留统一头注释，至少包含：

- `@file`
- `@brief`
- `@version`
- `@date`

如果模块存在关键边界，还应在文件头写出：

- Layering Rule
- Ownership Rule
- Lifecycle Rule
- Context Rule

`.h` 与 `.c` 的头注释不是同一深度：

- `.h` 负责 consumer contract，默认要能让调用者不看 `.c` 也完成首次接入
- `.c` 负责 implementation role、内部协作者与 why-comment 的导览

正式头注释结构，以：

- `docs/governance/Themis_C与H文件布局与注释书写规范.md`

为准。

### 3.5 注释写什么，不写什么

注释优先解释这些问题：

- 为什么这样设计
- 这个边界为什么重要
- 这里防的是什么坑
- 为什么不能更“省事”地写
- 这里属于哪个 owner

不要写下面这种“翻译代码”的废话注释：

- `/* assign x to y */`
- `/* increment i */`
- `/* return result */`

对正式维护的分层框架代码，还必须满足下面三条：

- 头文件入口契约必须完整。
  任何对外可见的宏、结构体、枚举、句柄、回调和函数，只要存在下面任一语义，就必须在 `.h` 中明确写出：
  - ownership / single-owner 约束
  - 生命周期顺序
  - task-context / ISR-safe / blocking 约束
  - 指针、buffer、callback 的有效期与借用关系
  - 兼容 wrapper 是否会丢失 errno 语义
- 实现文件必须在非显然逻辑处补 why 注释。
  尤其是下面这些位置，不能只留空代码：
  - 锁顺序与 owner 检查
  - rollback / cleanup / restore 路径
  - 兼容接口为何保留、为何做语义折中
  - 寄存器镜像恢复、状态机跳转、跨层边界保护
- touched-file 立即生效。
  本轮被修改的文件，必须把改动附近的注释提升到当前标准；不允许继续在“入口无契约、实现无 why 注释”的状态上叠加新逻辑。

并且对 `.h` 还要额外满足下面三条硬要求：

- 文件头必须先给“怎么理解这个模块”。
  至少写出：
  - Design overview / boundary
  - ownership / lifecycle / context 的摘要
  - 一个最小 usage example 或伪代码
- 公开符号按“一项一注释”写。
  不能把多个函数、多个结构体字段、多个宏塞进一个大段总注释里交代；新同事、新 AI、两个月后的自己必须能直接靠单条注释找到单条入口的用法和限制。
- 非平凡公共头不能只写“是什么”，还要写“怎么用”。
  例如：
  - service/task/app 层要写最小调用顺序
  - dev/drv 层要写 open/close/control 或 checked/legacy wrapper 关系
  - callback / buffer / ctx / handle 要写借用期和 ownership
- touched-header uplift 是强制要求。
  只要本轮动了一个公共头文件，就必须顺手把该头文件暴露出来的 public symbol
  注释补到当前仓库标准，不能继续叠加“文件头很好、API 裸奔”的历史债务。
- 评审公共头文件时，默认 consumer 不会顺手打开 `.c`。
  如果头文件本身不能回答最小调用顺序、借用期、阻塞性、线程上下文和失败语义，
  就视为接口契约未完成，而不是“实现里其实写得很清楚”。
- API 组织必须同时满足“按阶段分组”和“逐项注释”。
  不接受把几十条 public function 仅仅按声明顺序平铺到底的写法。

#### 3.5.1 组注释与单条注释是互补关系，不是二选一

Themis 现在正式裁决如下：

- 文件头注释负责讲模块级理解框架。
- API 分组注释负责讲“这一组什么时候用、和前后阶段是什么关系”。
- 单条 public symbol 注释负责讲“这一项怎么用、有什么限制、调用者需要承担什么契约”。

因此不接受下面两种极端：

- 只有文件头很长，后面 public API 裸排到底。
- 只有一条条 `@brief`，但完全看不出生命周期、调用顺序和分组阶段。

公共头文件里需要按当前标准被文档化的对象，不只包含函数，还包括：

- public macro / constant
- public typedef / callback type
- public enum / non-obvious enum item
- public struct / union 及其 contract-bearing field
- public inline helper
- public extern declaration（默认不鼓励，但一旦暴露就必须说明）

对 public header 来说，默认文档风格是**组注释 + 单条符号注释 + 必要时字段注释**，
而不是任由不同 AI、不同人各写一套样式。

#### 3.5.2 书写规范收口任务必须先分类，再决定能否批处理

凡是任务目标包含“书写规范收口”“注释补齐”“头文件入口统一”“API 分组整理”等语义，
都不允许把 formatter、脚本替换、正则批处理当成主方法。

AI 或人工在开工前必须先把问题分成至少两类：

- 可以批量处理的低风险问题：
  - 三行 banner 归一为单行 banner
  - EOF 闭合行补齐
  - 无语义风险的 include 紧凑化
  - 显然重复的空行节奏问题
- 必须逐文件人工判断的问题：
  - public header 入口契约是否完整
  - API 分组是否符合真实调用阶段
  - public symbol / public field 注释是否说清契约
  - `.c` 文件的 why-comment 是否补在真正关键的位置
  - private/public 边界是否被错误暴露
  - static state / struct / enum 是否只是“格式对齐”，而没有语义分组

只要一个文件落入第二类，就必须先读文件、再判断、再修改，不能宣称“已经统一”
却从未逐文件审视其 public contract。

当前仓库内可直接参考的正式样式，不是凭空想象，而是：

- `components/utils/pubsub/pubsub.h`
- `components/utils/micro_json/micro_json.h`
- `components/utils/hash_map/hash_map.h`

也就是说，Themis 新写的 `.h` 不允许再停留在“只有文件头 + 函数声明列表”的水平。

文件布局与注释的具体写法，见 `docs/governance/Themis_C与H文件布局与注释书写规范.md`。

### 3.6 注释语言

当前建议如下：

- `docs/` 以中文为主
- 代码中的接口说明、实现注释、架构说明优先英文或中英混合的工程化表达
- 同一个文件内尽量保持注释语言一致

### 3.7 TODO / FIXME 使用规则

- 只有明确后续动作和 owner 时才允许写 `TODO`
- `FIXME` 只用于确认存在缺陷但本轮不能一起改的点
- 没有上下文说明的 `TODO/FIXME` 视为低质量注释

## 第四章：标识符命名

### 4.1 通用命名风格

- 文件名：`snake_case`
- 函数名：`snake_case`
- 结构体标签：`snake_case`
- 枚举标签：`snake_case`
- 宏和编译期常量：全大写加下划线

### 4.2 分层前缀

Themis 当前正式前缀如下：

- `tm_`：framework 级公共 API
- `drv_`：southbound driver 层
- `dev_`：device class 层
- `bsp_`：板级入口
- `BOARD_`：板级事实
- `TM_`：框架共享宏或协议常量

补充硬规则：

- `applications/` 里的产品 service 才允许稳定使用 `*_service` 语义命名。
- `subsys/` 里的 shared owner 必须用“真实共享机制名”命名，而不是沿用 APP
  service 语义偷懒迁移。
- `subsys/` 不是产品定制入口，也不是“先塞进去再说”的垃圾场。
  某个改动如果只对当前产品、当前页面、当前板级拓扑成立，应优先回到
  `applications/`、`bsp/` 或配置层，而不是改 shared owner 让未来消费者一起背锅。
- shared subsystem 若需要表达 board input / LED 的 id、count、name，必须直接借用
  `bsp_*` contract；禁止在 `subsys/` 再公开第二套 `*_ID_1/2/3`、`*_COUNT`、
  `key1/key2/key3` 之类的并行拓扑命名。
- 因此：
  - `display_service`、`buzzer_service` 这类产品 owner 继续留在 `applications/`
  - `tm_input`、`tm_led` 这类 shared subsystem 用 `tm_` 前缀描述共享 contract
- 不允许再把 APP 兼容壳、历史过渡名或“看起来像 service”的产品词直接搬进
  `subsys/`

### 4.3 私有符号命名

文件内私有符号建议以下划线开头：

- 私有状态：`_module_state`
- 私有 helper：`_module_helper()`
- 私有表项：`_module_table`

静态私有命名要能从名字直接看出 owner，例如：

- `_bsp_display_transport`
- `_bsp_buzzer_status`
- `_display_service_page_index`

### 4.4 布尔量命名

布尔量必须用“可读成判断句”的命名：

- `is_ready`
- `is_busy`
- `opened`
- `bus_opened`
- `playback_active`

不要把布尔量命名成模糊名词，例如：

- `flag1`
- `mode_ok`
- `temp`

### 4.5 状态变量命名

模块级错误状态建议统一使用：

- `_xxx_status`

模块级 ready 状态建议统一使用：

- `_xxx_ready`

模块级运行句柄建议统一使用：

- `_xxx_handle`
- `_xxx_default`

### 4.6 命名语义必须与行为一致

- `init`：完成初始化，不代表线程启动或业务开始
- `attach`：完成绑定，不应暗含完整 init
- `open/close`：进入或退出标准设备生命周期
- `start/stop`：运行态启停
- `process`：外部周期推进
- `get_xxx`：不应暗藏副作用

### 4.7 禁止命名

下面这些名字不能作为正式长期接口名：

- `common`
- `helper`
- `util1`
- `process_data2`
- `do_task`
- `test_new`

## 第五章：函数

### 5.1 单一职责

一个函数应该只解决一个层次的问题。

正确示例：

- `bsp_display_ssd1316_attach()` 负责板级显示 transport 绑定
- `ssd1316_init()` 负责控制器初始化
- `display_service_process()` 负责页面轮转策略与渲染调度

错误示例：

- 一个函数里同时做 board pin 绑定、业务策略、页面排版、I2C 细节和任务等待

### 5.2 参数检查

公共函数必须做参数检查，尤其是：

- 指针是否为 `NULL`
- 长度是否为 0
- 枚举或范围是否合法
- 输入是否满足当前模块契约

内部 helper 可以依靠 `tm_assert()`，但不能用断言替代公共参数检查。

### 5.3 返回值规则

正式规则如下：

- 状态型函数返回 `int`
- 长度、计数型函数返回 `ssize_t`
- 纯判断返回 `bool`
- 成功返回 `0`
- 失败返回负的 `errno`

禁止：

- 成功返回 `1`，失败返回 `0`
- 同类 API 同时存在两套错误码体系
- 中间层吞掉底层错误码后只返回“失败”

### 5.4 函数长度与拆分

出现下面任一情况时，应该拆 helper：

- 一个函数已经包含两个以上层次的职责
- rollback 路径太长
- 相同输入校验出现三次以上
- 同一段状态推进逻辑需要跨多个调用点复用

### 5.5 生命周期语义必须清楚

对标准设备，typed API 的语义必须和 `tm_device_*` 一致。

当前硬规则：

- `read/write/control` 进入统一设备模型时，默认要求先 `open`
- `find` 只代表“找到对象”，不代表“已经可用”
- 默认实例 owner 如果要长期持有某个标准设备，必须显式 `open`
- 如果 init 失败，必须回滚 `close`

本轮实际案例：

- `bsp_buzzer_init()` 在 `tm_pwm_device_find()` 之后显式 `tm_pwm_open()`
- `bsp_display_ssd1316_init()` 在 `tm_i2c_bus_device_find()` 之后显式 `tm_i2c_bus_open()`
- 初始化失败时分别回滚 `tm_pwm_close()` / `tm_i2c_bus_close()`

### 5.6 阻塞语义必须可见

函数如果会阻塞，头文件或文件头必须写清楚：

- 是否 busy wait
- 是否调用 `osDelay`
- 是否等待 semaphore / mutex
- 是否可能长时间占用总线

尤其不能在“名字像 getter”的函数里隐藏阻塞等待。

### 5.7 回滚路径必须对称

凡是有多步初始化的函数，都要按“谁申请、谁回滚”的顺序写 rollback：

1. 绑定资源
2. 打开设备
3. 拉高电源
4. 初始化控制器

一旦后续步骤失败，就逆序回滚。

### 5.8 ISR 相关函数必须显式标注

函数如果允许或要求在 ISR 上下文使用，必须在头文件或注释中写清楚。

默认规则：

- 没写 `ISR-safe` 的接口，按 task-context only 理解
- 会拿 mutex、queue、semaphore、device lock 的函数，一律不是 ISR-safe

## 第六章：变量

### 6.1 变量分组

结构体字段建议按下面顺序组织：

1. 配置字段
2. 资源句柄
3. 运行状态
4. 统计字段
5. 缓存或临时字段

这样更利于人工阅读、静态分析和生命周期审查。

### 6.2 变量初始化

- 局部变量定义后尽快初始化
- 结构体临时对象优先使用 `{0}`
- 模块级静态状态必须在声明处给出明确初值
- 不允许依赖“BSS 默认是 0”来隐藏状态语义

### 6.3 全局变量使用规则

全局或文件静态变量只允许承载真正共享的 owner 状态，例如：

- 默认实例句柄
- 模块状态码
- 共享 transport 描述符
- task handle

禁止通过全局变量绕开正式接口，例如：

- 应用层直接拿 `drv_xxx` 的内部对象
- 跨模块通过 `extern` 共享业务中间态

### 6.4 生命周期和所有权

每个指针都要能回答三个问题：

- 它归谁拥有
- 它什么时候有效
- 它什么时候释放

如果不能回答，就说明接口设计还不够清楚。

### 6.5 内存占用

下面这些情况下要优先考虑压缩或改布局：

- 大结构体被频繁按值传递
- 大数组静态常驻但只有极少数路径使用
- 计数器、状态位仍在用 `uint32_t` 存布尔语义
- 位域、枚举、掩码可以明显减少内存但没有使用

### 6.6 运行时缓存

共享缓存必须写清 owner 和并发约束。

例如当前 `bsp_display_get_canvas()` 返回的是共享 framebuffer 别名，因此必须明确：

- 单 owner
- task-context only
- 不能多份 canvas 并发改同一底层 framebuffer

## 第七章：宏与常量

### 7.1 宏的正式用途

宏只应用于下面这些场景：

- 编译期开关
- 寄存器位定义
- 常量表达式
- 条件编译兼容层
- 节分隔、属性修饰、导出注册这类预处理器能力

### 7.2 能不用宏就不用宏

下面这些场景优先考虑别的形式：

- 可类型检查的常量：优先 `enum` 或 `static const`
- 可替代函数式宏的逻辑：优先 `static inline`
- 运行时状态：绝不使用宏假装成状态机

### 7.3 宏命名

- 框架级共享宏：`TM_`
- 板级事实宏：`BOARD_`
- 文件私有宏：带模块前缀，例如 `BSP_DISPLAY_I2C_CHUNK`

禁止：

- `MAX1`
- `TEMP_MACRO`
- `DEBUG_A`

### 7.4 宏表达式安全

函数式宏必须满足：

- 参数和整体结果都带括号
- 不产生多次求值副作用
- 不依赖调用者记忆优先级

如果做不到，就改成 `static inline`。

### 7.5 magic number 管理

运行时代码中禁止散落没有语义的 magic number。

正确示例：

- `DISPLAY_TASK_PERIOD_MS`
- `BOARD_DISPLAY_I2C_ADDR`
- `BUZZER_SERVICE_INTERVAL_MS`

### 7.6 镜像配置规则

`drivers/config/*.h` 里的镜像配置必须和 generated 真值保持一致。

当前硬规则：

- 工具生成的底层真值以 `project/src/wk_*.c` 为准
- Themis 镜像配置只承接运行期需要的事实
- 一旦 generated 真值变了，镜像宏必须同步

本轮实际案例：

- `project/src/wk_tmr.c` 中 `TMR2` 的 period reload 为 `2180`
- `drivers/config/pwm_config.h` 中 `PWM2_DEFAULT_PERIOD_RELOAD` 必须同步为 `2180U`

## 第八章：分层依赖与跨层调用

### 8.1 允许的依赖方向

正式依赖方向如下：

- `applications -> bsp / components / device / subsys`
- `bsp -> components / device / drivers / subsys`
- `components -> 标准头 / 自身子模块 / 通过 ops 注入的抽象`
- `device -> subsys / CMSIS-RTOS2`
- `drivers -> device / subsys / vendor`

#### 8.1.1 BSP 对 `drivers` 的依赖是窄特权

`bsp -> drivers` 这条依赖方向成立，但它不是“BSP 可以随意越层”的通行证。

正式规则是：

- BSP 的 runtime path 优先走 `dev_*` typed API 或稳定的 shared/BSP contract
- BSP 只有在 `pre-kernel early path` 或 `compile-time board truth` 这两类场景下，才允许直接触碰 `drv_*`
- 典型允许场景包括：
  - `bsp_console` 的 early raw console 直接调用 `drv_uart_fast_putc()`
  - `bsp_pins.h` 直接借用 `drv_gpio.h` 的 `GET_PIN()` 宏表达板级 pin truth
- 一旦 runtime path 已经可以稳定走 `dev_*`，就不允许继续扩大直接 include `drv_xxx.h` 的范围

收口一句话：

> BSP 不是自由越层，而是在 early/raw/board-truth 场景下拥有被严格限制的窄特权。

### 8.2 明确禁止的反向依赖

- `drivers` 依赖 `applications`
- `subsys` 依赖产品 service
- `components` 依赖板级头或厂商寄存器头
- `applications` 直接 include `drv_xxx.h`

### 8.3 默认实例的层次边界

板级默认实例必须由 `bsp` 持有，不能散落到组件或应用层。

例如当前：

- `bsp_buzzer` 持有默认 `struct buzzer`
- `bsp_display` 持有默认 `struct ssd1316`
- `bsp_devices_late_init()` 集中承担默认实例晚绑定 owner

如果当前镜像需要裁剪某些板级默认实例，正式规则是：

- 裁剪语义仍属于 board-owned build contract，应收口到 `bsp_config.h` 或等价 BSP 配置入口
- `bsp/*.c` 不得直接消费 `TM_APP_FEATURE_*` 这类 APP/product feature 宏
- `bsp_devices_late_init()` 只读取已经中性化的 `BOARD_DEFAULT_*_ENABLED` 一类 BSP 合同标记

#### 8.3.1 `bsp_*_init()` 必须先声明自己属于哪一类

后续凡是新增或重写 `bsp_*_init()`，都不允许只写一个模糊的 “init”。必须先判定它属于下面四类中的哪一类，并在头文件/实现注释里写清楚：

| 类型 | 典型入口 | 正式语义 |
| --- | --- | --- |
| `pre-kernel bootstrap` | `bsp_init()`、`bsp_devices_init()`、`bsp_console_prepare_early()`、`bsp_wdt_prepare_early()` | 在 scheduler 与 device model 完整就绪前建立最小板级桥 |
| `validation-only` | `bsp_adc_init()`、`bsp_uart_init()` | 只验证 board-default resource 存在并暴露稳定 accessor |
| `raw IO ready` | `bsp_input_init()`、`bsp_led_init()` | 准备当前镜像启用的 raw IO 表面 |
| `runtime-owning default instance` | `bsp_wdt_init()`、`bsp_buzzer_init()`、`bsp_display_init()`、`bsp_console_bind_runtime()` | 打开、绑定并维护板级默认运行时 owner |

正式要求：

- 不同类型的 `init` 不得混用一套论证口径
- `validation-only` 不应偷偷长成 runtime owner
- `runtime-owning default instance` 必须补齐 owner / lifecycle / failure 语义
- 评审 BSP 代码时，先判断 `init` 类型，再判断它是否应该裁剪、上移或补 gate

### 8.4 shared subsystem 不得重定义板级拓扑

如果某个 shared subsystem 的公共契约需要引用“当前板上有哪些输入/输出”，
正式规则是：

- 板级 id、数量、名字、默认映射由 `bsp/` 持有
- `subsys/` 可以借用 `bsp` 已成立的 id/struct/name helper
- `subsys/` 不得再公开第二套并行 public enum，例如：
  - 第二套 `KEY1/KEY2/KEY3`
  - 第二套 `LED1/LED2`
  - 第二套 `*_COUNT`

也就是说，`subsys` 可以拥有 shared timing / dispatch / debounce / routing 契约，
但不能重新声明“这块板到底有几个键、几个灯”这种 board fact。

## 第九章：接口设计、ops 与句柄

### 9.1 ops 结构只放必要能力

不要为了“显得完整”把没稳定的能力先塞进 ops。

`ops` 必须满足：

- 能支撑当前真实复用场景
- 必需项和可选项边界清楚
- 参数能支撑多实例复用

### 9.2 推荐参数形态

当前推荐优先使用：

- `void *ctx + 业务参数`

这样可以天然支持：

- 多实例
- 不同 backend
- BSP 绑定和组件逻辑解耦

### 9.3 注册必须完整

禁止下面两类半成品接口：

- 定义了 ops，却没有正式注册
- 注册了 ops，但缺少当前语义下的必需实现

### 9.4 typed API 不能绕开设备语义

如果 typed API 本质上还是进入 `tm_device_control()`，那它就必须遵守统一设备模型契约。

不能出现：

- `tm_device_read/write()` 要求先 open
- 但 typed control API 又偷偷允许未 open 直接跑

### 9.5 句柄和实例 owner

多实例模块必须通过句柄、上下文、user_data 区分实例。

禁止依赖：

- 全局单例偷偷区分实例
- 通过外部符号猜当前用的是哪一个硬件

## 第十章：并发、RTOS 与 ISR

### 10.1 默认按 task-context 理解

下面这些路径默认都按 task-context only 理解：

- `tm_object_*`
- `tm_device_*`
- 大多数 typed device API
- I2C / SPI / UART 阻塞传输
- `bsp_display_*`
- `bsp_buzzer_*`

### 10.2 ISR-safe 必须显式声明

ISR 中只允许调用明确声明为 `ISR-safe` 的接口。

没写清楚就按不可在 ISR 使用理解。

### 10.3 禁止的高风险组合

下面这些写法默认禁止：

- 持有 mutex 时长时间 busy wait
- 关中断期间执行长耗时硬件操作
- 在 ISR 里调用阻塞函数
- 在持锁期间调用外部回调
- 在公共遍历函数持锁期间执行用户 visitor

### 10.4 回调上下文必须写死

凡是暴露回调，必须明确：

- 谁调用它
- 运行在哪个上下文
- 是否允许阻塞

例如 GPIO IRQ 回调，如果运行在硬中断上下文，头文件必须硬声明，不能只在实现里“默认大家知道”。

### 10.5 polling 与事件驱动

当前 Themis 允许最小 polling 样板存在，但要满足：

- 有明确周期
- 有超时或退出路径
- 不在错误上下文里放大阻塞

如果某条链路已经明显影响实时性，应优先评估：

- 事件驱动
- 状态机推进
- 非阻塞 API + 外部 owner 循环

## 第十一章：错误码、日志、文档与验证

### 11.1 错误码

统一使用负的 `errno` 体系，例如：

- `-EINVAL`
- `-ENODEV`
- `-ENOSYS`
- `-EBADF`
- `-EBUSY`
- `-ERANGE`
- `-EIO`
- `-ETIMEDOUT`

### 11.2 日志与可观测性

- 不能吞掉底层错误码
- 初始化失败要保留 `_xxx_status`
- 启动失败要尽量留下最后失败 hook 或阶段信息
- 普通日志路径与 panic/raw 输出路径要分开

### 11.3 generated code 边界

`project/` 和 `libraries/` 默认视为只读边界。

正式规则：

- 不把产品长期语义写进 generated 文件
- 运行期需要的事实通过 `drivers/config/*.h` 做镜像
- 如果镜像与 generated 真值不一致，必须同轮修正

### 11.4 文档同步规则

代码改了，正式文档也要同轮同步。

至少要同步以下其中之一：

- 设计说明
- 架构整改说明
- 启动链文档
- 治理文档
- 维护日志

### 11.5 维护日志规则

本仓库所有真实改动结束前都要更新：

- `docs/governance/Themis_工程维护日志.md`

记录内容至少包括：

- 范围
- 变更摘要
- 修改原因
- 验证情况
- 后续事项

### 11.6 提交前最小检查

正式建议的最小检查集合如下：

1. `git diff --check`
2. 关键路径全文检索，确认没有遗漏同类问题
3. 受影响文件至少做一轮语法级检查或工程编译
4. generated 真值和镜像配置对照检查
5. 文档与维护日志同步检查
6. 能上板的改动尽量补最小实板回归
7. 本轮只要 `git status` 已出现真实改动，就必须在结束前完成 `git add` / `git commit`，远端可用时继续 `git push`

### 11.7 书写规范收口类任务的标准提示词

当任务本质上是在做“目录级书写规范收口”“公共头文件注释补齐”“API 分组与入口手册化整改”
时，后续 AI 提示词不应只写“去检查格式和注释”，而应显式把执行方法约束出来。

一个合格的提示词，至少必须包含下面这些硬要求：

- 先读 `AGENTS.md`、这三份底层规范、以及目标目录自己的 README / 架构文档。
- 先抽样若干文件判断问题密度，再把文件按“可批量处理”和“必须人工逐文件处理”分组。
- public header、public type、callback、API 分组、字段注释、private/public 边界，一律按文件级人工审查处理。
- script / formatter 只能辅助处理低风险节奏问题，不能替代 public contract 判断。
- 每一批修改后都要做验证，不能等全部改完再一次性碰运气。
- 本轮必要的治理文档和维护日志必须同步。

下面这版模板可以直接替换旧的泛化提示词：

```text
**任务**：对 `<scope>` 目录进行一次完整的书写规范收口，并让结果同时符合
`AGENTS.md`、`Themis_代码规范指南.md` 和 `Themis_C与H文件布局与注释书写规范.md`。

**硬要求**：

1. 开工前必须先阅读：
   - `AGENTS.md`
   - 与目标范围相关的治理文档与 README
   - 上述三份底层规范

2. 先做“现状分析”，但不是泛泛扫一遍，而是抽样若干代表性文件并确认以下问题类型：
   - 头文件入口注释是否完整，是否能让调用者不看 `.c` 直接接入
   - API 是否按真实使用阶段分组，而不是裸声明平铺
   - public macro / type / enum / struct / callback / function / inline helper 注释是否完整
   - 源文件头注释与 why-comment 是否充分
   - include 是否仍按旧式碎裂分组
   - static state / struct / enum 是否存在格式坍塌但缺少语义分组
   - private/public 边界是否被错误暴露

3. 完成抽样后，必须先把文件分成两类再动手：
   - A 类：只涉及 banner、EOF、空行、低风险 include 紧凑化，可在确认模式一致后小批量处理
   - B 类：涉及 public header 契约、API 分组、字段注释、private/public 边界、复杂 static state / struct 组织，必须逐文件人工审查和修改

4. 分批修改时必须遵守：
   - 仅修改 `<scope>` 及本轮必要的治理文档与维护日志
   - 对 B 类文件必须一个文件一个文件进去看，不允许用脚本直接重写
   - 对 public header 的修改必须同步补齐该头文件暴露出来的 public symbol 注释
   - 对 `.c` 文件的修改必须同步整理实现级头注释、关键 why-comment 和明显坍塌的状态分组

5. 每一批修改后都要立刻验证：
   - `git diff --check`
   - 与 `<scope>` 对应的质量门禁 / host tests / build / 必要时实板验证
   - 再继续下一批

6. 收尾时必须完成：
   - 维护日志同步
   - `git add` / `git commit`
   - 远端可用时继续 `git push`

7. 严禁以下做法：
   - 只跑 formatter 或脚本就宣称“规范化完成”
   - 不区分 public / private，就批量改写整个目录
   - 只补文件头，不补 public API 契约
   - 只看单个问题，不回到三份底层规范检查是否一致
```

## 第十二章：快速自检清单

在提交任何一段正式代码前，至少自问下面这些问题：

- 这段代码放在这层真的对吗
- 这条调用链有没有跨层或逆向依赖
- 标准设备是不是先 `open` 再 `read/write/control`
- init 失败后是否对称回滚
- 阻塞语义、上下文语义、owner 语义是否写清楚
- 错误码是否被原样保留
- generated 真值和镜像宏是否一致
- 相关文档和维护日志是否已更新
- 本轮已落地的真实改动是否已经完成 `commit/push` 同步

如果这 8 个问题里有 2 个以上答不上来，就不应该直接合入。
