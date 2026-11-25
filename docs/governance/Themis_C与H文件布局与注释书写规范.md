# Themis C 与 H 文件布局与注释书写规范

> **V2 适用范围（2026-11-24）**：Legacy Themis 固件（`applications/`、`bsp/`、`components/`、`subsys/`、FreeRTOS、Shell）已删除，只保留在 Git tag `legacy-final`。本文中这些目录与分层示例属于历史背景；V2 目录与职责以 [AGENTS.md](../../AGENTS.md) 第 3 节为准。命名、注释、文件布局、错误处理与验证纪律等通用规则继续适用。

## 1. 文档定位

本文只约束 Themis 自有 `.c/.h` 文件的内部组织方式、视觉节奏与注释书写方式。

它重点回答五件事：

- `.c` 文件从上到下应该怎么摆
- `.h` 文件从上到下应该怎么摆
- include 应该如何排序、什么时候允许空行
- public API 应该如何分组和注释，才能让调用方不看 `.c` 也能接入
- static state / struct / enum / field 在名字很长时，如何避免排版变成“一大坨”

本文不是下列文档的替代品：

- 分层、owner、返回值、RTOS、blocking、lifecycle 规则：
  - [Themis_代码规范指南.md](./Themis_代码规范指南.md)
- portability-first / utils / generic library 特殊规则：
  - [Themis_Utils与通用组件代码规范指南.md](./Themis_Utils与通用组件代码规范指南.md)
- 存量注释整改顺序与阶段性结论：
  - [Themis_注释规范化补强基线.md](./Themis_注释规范化补强基线.md)

一句话裁决：

**Themis 的架构边界向 Zephyr 对齐，Themis 的文件可读性与注释节奏向 STM32 HAL 学习，但绝不机械复刻 STM32 HAL。**

## 2. 设计来源与不复制清单

### 2.1 我们要吸收 STM32 HAL 的是什么

- 文件打开后能立刻看懂“这份文件是干什么的”
- 顶部说明像一份可读的工程手册，而不是只有 `@brief`
- 节顺序稳定，读者每次都能在固定位置找到固定信息
- 公开头文件不仅回答“是什么”，还回答“怎么接”
- API 不是一条一条裸排，而是按使用阶段和功能簇组织

### 2.2 我们明确不复制 STM32 HAL 的是什么

- 旧式 `#ifndef / #define / #endif` include guard
- 厂商版权、作者、法律声明大横幅
- 大量 Doxygen group 宏和 `@addtogroup`
- 80 列时代的旧式横线审美
- 机械式的大面积纵向对齐墙

### 2.3 Themis 自己的硬约束

- 行宽基线是 `100`，不是 `80`
- 头文件一律只使用 `#pragma once`
- `.c/.h` 结尾都必须保留：

```c
/* ---------------------------------------- end of file ----------------------------------------- */
```

- 分段注释统一使用单行 banner，不再新增三行横线块

## 3. 总体硬规则

### 3.1 适用范围

本文适用于 Themis 自有受管代码中的 `.c/.h`，包括但不限于：

- `applications/`
- `bsp/`
- `components/`
- `device/`
- `drivers/`
- `include/`
- `subsys/`
- `tests/`

默认不要求改写厂商目录、生成代码或外部子树的写法，但这些边界一旦被读取或包装，
仍应以本文的头文件入口质量为目标。

### 3.2 规则优先级

冲突时按下列顺序裁决：

1. `AGENTS.md`
2. 正式架构 / 治理文档中的 owner 与边界结论
3. 本文
4. `.clang-format`

说明：

- 能由 formatter 自动完成的，交给 formatter。
- formatter 还表达不了的“文件节奏、注释分工、可读性布局”，由本文裁决。
- 如果某条 `.clang-format` 现行行为与本文目标还没完全一致，当前先通过**语义分组、空行切段、减少横向拖拽**来收口；后续 formatter 重建时再向本文对齐。

### 3.3 三条总原则

- 先让文件像“正式入口”，再让它像“能编译的文本”
- 先让头文件回答“怎么用”，再让源文件展开“为什么这样实现”
- 先用语义分组改善观感，不靠手写大面积空格做列对齐表演

## 4. `.c` 文件布局标准

### 4.1 顶层顺序

正式推荐顺序如下：

```text
文件头注释
Includes
Private Macros
Private Types
Private State
Private Helpers / Private Prototypes
Public Functions
Private Functions（如有需要）
end of file 行
```

说明：

- `.c` 的职责不是重复头文件用户手册，而是解释**实现 owner、内部状态、lock / rollback / recovery / runtime glue**。
- 如果某个模块为了可读性需要先放 public function，再放 private function body，可以接受；
  但顶层章节顺序不能漂移成“每个文件都一套新发明”。

### 4.2 `.c` 文件头注释标准

`.c` 文件头必须保留：

- `@file`
- `@brief`
- `@version`
- `@date`

并且建议额外写出两段到三段实现级说明，而不是停在最小四行。

推荐结构：

```c
/**
 * @file    tm_input.c
 * @brief   Shared physical input gesture subsystem
 * @version v3.0.0
 * @date    2026.04.24
 *
 * Implementation role
 * -------------------
 * - pairs with `tm_input.h`
 * - owns internal state, runtime binding, and lock / rollback details
 * - consumer-facing call sequence stays documented in the header
 *
 * Key collaborators
 * -----------------
 * - `bsp_input`: raw board input sample provider
 * - `button`: gesture interpretation engine
 */
```

`.c` 文件头不需要把 `.h` 里的完整 `Usage example` 整段再抄一遍；
但如果实现里有**必须只在 `.c` 说清楚**的约束，例如锁顺序、恢复路径、调试降级路径，
就应该在 `.c` 头部或实现附近写出来。

### 4.3 `.c` 文件节奏要求

- `Includes` 后直接进入紧凑 include 块，不在 include 区域内人为插入多段空白分组
- `Private State` 应按语义成组，不要把句柄、配置、状态、统计、上次快照全糊成一堵墙
- `Public Functions` 是外部入口，优先让读者先看到它们
- `Private Functions` 负责把复杂度压回实现内部，不要让 public function 再嵌套巨大的局部状态机

## 5. `.h` 文件布局标准

### 5.1 顶层顺序

正式推荐顺序如下：

```text
文件头注释
#pragma once
Includes
extern "C"（如需要）
Public Macros / Constants
Public Types
Public Functions（按使用阶段分组）
end of file 行
```

### 5.2 `.h` 文件头注释标准

`.h` 是调用方主入口，默认假定使用者**不会顺手打开 `.c`**。

因此 `.h` 文件头不是“短介绍”，而是模块级使用说明。正式推荐结构如下：

```c
/**
 * @file    tm_input.h
 * @brief   Shared physical input gesture subsystem
 * @version v3.0.0
 * @date    2026.04.24
 *
 * Design overview
 * ---------------
 * ...
 *
 * Usage example
 * -------------
 * ...
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * ...
 *
 * Blocking
 * --------
 * ...
 *
 * Failure / recovery
 * ------------------
 * ...
 */
```

裁决：

- `.h` 头注释必须先交代“怎么理解这个模块”
- `.h` 头注释必须给出最小调用顺序或最小 usage example
- `.h` 头注释必须把 ownership / lifecycle / context / blocking / failure 中和模块相关的项写出来

### 5.3 `.h` 不是函数声明清单

非平凡公共头文件必须进一步满足：

- API 按**使用阶段**分组，而不是一长串裸声明平铺到底
- 每个 public symbol 仍然保留单独注释
- 分组注释负责告诉调用方“这一组什么时候用”
- 单条符号注释负责告诉调用方“这一条怎么用、有什么限制”

推荐的分组方式包括但不限于：

- `Lifecycle`
- `Configuration / Binding`
- `Runtime Step`
- `Snapshot / Diag`
- `Name Helpers`

也就是说，Themis 的标准不是“只有文件头很好”，而是：

**文件头像手册，API 分组像目录，单条符号注释像正式契约。**

### 5.4 `.h` 文件完整节顺序与可选项

不是每个头文件都必须包含所有内容，但一旦同时出现多个 public section，
默认顺序必须按下面执行：

```text
文件头注释
#pragma once
Includes
extern "C"（如需要）
Public Version / Feature Macros
Public Compiler / Portability Helper Macros
Public Forward Declarations
Public Typedef / Callback Types
Public Enums
Public Structs / Unions
Public Extern Declarations（默认不鼓励）
Public Inline Helpers
Public Functions（按使用阶段分组）
end of file 行
```

裁决：

- 上面是**存在时的顺序**，不是要求每份头文件机械造出空 section。
- `#pragma once` 是唯一正式头保护方案，不再引入新的 include guard 变体。
- 版本号、功能开关、编译器 helper 宏只有在它们确实属于 public contract 时才放进 public header。
- `extern` 变量声明默认视为弱设计信号；若必须暴露，必须解释 ownership、并发语义和生命周期。
- `static inline` 只有在它属于正式 API 或真正的 convenience wrapper 时，才放进 public header。

### 5.5 public symbol 注释覆盖范围与默认样式

Themis 现在明确裁决：公共头文件里需要被文档化的对象，不只是函数。

只要一个符号对外可见，并且调用者需要靠它完成接入、配置、调用、判断或扩展，
它就应被视为 public contract 的组成部分。默认覆盖范围包括：

- public macro / constant
- public forward declaration（若其不透明句柄语义需要说明）
- public typedef / callback type
- public enum 类型与 non-obvious enum item
- public struct / union 类型
- public struct / union 中由调用者直接读写的 contract-bearing field
- public inline helper
- public function
- public extern declaration

默认注释样式如下：

- group 注释负责解释阶段和用途。
- 单条 public symbol 注释使用放在声明正上方的 `/** ... */`。
- public field 注释默认也放在字段正上方，而不是写成一整面 trailing comment 墙。

默认不再把下面这些写法作为 public contract 主样式：

- 行尾 `/* ... */`
- 行尾 `/**< ... */`
- 多个 symbol 共用一段模糊总注释

只有在下面这类极少数高密度场景中，才允许审慎使用 trailing 风格：

- 外部协议位定义 / wire-format item 必须和同一行常量强绑定
- dense table / bitfield 用正上方注释反而更难建立对应关系

即便如此，也应保持“少量例外”，而不是把整份 public header 写成对齐型注释墙。

### 5.6 public vs private 边界在头文件中的裁决

头文件不是“先放出来，之后再说”的缓冲区。

正式规则如下：

- 如果某个 struct / enum / helper / 状态码只服务实现内部，就留在 `.c` 或 private internal header。
- 一旦某个类型、成员顺序、枚举值、回调签名进入 public header，它就被视为正式 contract。
- 不要为了图省事，把 debug counter、私有状态、临时兼容桥状态直接公开到 `.h`。
- 如果某个 public struct 只是为了让调用者拿到句柄，却不希望其内部布局成为长期契约，应优先改为不透明句柄而不是暴露完整定义。

## 6. Include 布局与排序规则

### 6.1 总原则

Themis 受管 `.c/.h` 的 include 区域默认采用：

**一个紧凑区块 + 稳定语义顺序 + 默认不插空行**

禁止继续沿用“按 C 标准库 / RTOS / vendor / Themis 每一类都空一行”的旧写法。

### 6.2 `.c` 文件 include 顺序

`.c` 文件的 include 顺序固定为：

1. 自己的配对头文件
2. 与本模块最直接相关的契约头 / 同域头
3. RTOS / vendor / 第三方环境头
4. C 标准库头

并且默认保持**一个紧凑块**，不人为插入分类空行。

正确示例：

```c
/* Includes --------------------------------------------------------------------- */
#include "tm_input.h"
#include "button.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include <errno.h>
#include <stdint.h>
```

错误示例：

```c
/* Includes --------------------------------------------------------------------- */
#include "tm_input.h"

#include <errno.h>
#include <stdint.h>

#include "FreeRTOS.h"

#include "button.h"
#include "cmsis_os2.h"
#include "task.h"
```

为什么这样定：

- 打开文件后，读者应该先看到“这个模块直接依赖谁”，而不是先看到一堆分类学空行
- 空行过多会把一个本来只有 6 到 10 行的 include 块拆成视觉碎片
- 标准库头通常只提供通用类型或 errno，不应把更关键的业务 / runtime 依赖冲散到更远的位置

### 6.3 `.h` 文件 include 顺序

`.h` 文件只 include **对 public declaration 真正必需**的头文件。

默认顺序：

1. public contract 直接依赖的项目头
2. 外部环境或标准库头

同样默认不在 include 块中插入多余空行。

例如：

```c
/* Includes --------------------------------------------------------------------- */
#include "bsp_input.h"
#include <stdbool.h>
#include <stdint.h>
```

如果 public declaration 只需要一个不透明指针或前置声明，就不要额外 include 完整头。

### 6.4 只有两种情况允许在 include 区域中空一行

- 存在**必须保序**的特殊配置头，且需要用空行显式隔离
- 存在 no-touch / generated / compatibility 过渡头，且必须用空行强调它的边界

这两种情况都必须足够少见，并且最好在附近补一句 why-comment。

### 6.5 include 排序的额外裁决

- 不要为了“看起来整齐”而把 include 全部按字母排序，导致语义顺序消失
- 不要依赖隐式 include
- 不要让 `.c` 比 `.h` 更懂 public contract

## 7. 节分隔与空行规则

### 7.1 正式 banner 样式

正式节分隔统一使用单行 banner：

```c
/* Includes --------------------------------------------------------------------- */
/* Private Macros --------------------------------------------------------------- */
/* Private Types ---------------------------------------------------------------- */
/* Private State ---------------------------------------------------------------- */
/* Private Helpers -------------------------------------------------------------- */
/* Public Functions ------------------------------------------------------------- */
```

硬规则：

- 不再新增三行横线块
- 不手写多个近义标题混用同一含义
- 不为 2 到 3 行的小片段滥加 banner

### 7.2 空行节奏

- major section 与 major section 之间用 banner 分隔
- section 内部不同语义子组之间允许且建议保留 1 个空行
- 不连续堆 2 行以上空行
- banner 与它管理的第一行内容之间不要再额外空一行

### 7.3 结尾规则

所有正式维护的 `.c/.h` 结尾都必须保留：

```c
/* ---------------------------------------- end of file ----------------------------------------- */
```

这不是装饰，而是 Themis 的文件闭合标记。

## 8. 注释书写规则

### 8.1 注释写什么

优先写这些信息：

- 为什么这样组织，而不是为什么这行代码能运行
- 这个边界由谁负责
- 这个调用顺序为什么不能乱
- 这段实现防的是什么历史坑或运行期风险
- 这份公共头让调用方如何最小正确接入

### 8.2 注释不写什么

禁止“翻译代码”的废话注释，例如：

- `/* increment counter */`
- `/* return result */`
- `/* assign value */`

### 8.3 `.h` 与 `.c` 的注释分工

`.h` 负责：

- 回答“这是什么模块”
- 回答“怎么用”
- 回答“谁拥有它”
- 回答“会不会阻塞 / 能不能在 ISR 用 / 指针借用期是什么”

`.c` 负责：

- 回答“为什么这样实现”
- 回答“为什么这个锁顺序 / rollback / recovery / degrade path 不能乱”
- 回答“为什么这个 helper 放在这里、为什么这个状态要这样拆”

一句话记忆：

**头文件讲 contract，源文件讲 rationale。**

### 8.4 非显然实现必须补 why-comment

下面这些位置不能只留裸代码：

- lock 顺序与 lock 降级路径
- rollback / cleanup / restore
- 兼容 wrapper 为什么还保留
- 板级 / runtime / generated 边界为什么这样接
- 调试注入、fault recovery、watchdog 兜底等非常规路径

### 8.5 符号注释的颗粒度与长短裁决

Themis 不追求“每个 public symbol 都写一样长”，但必须追求“每个 public symbol 都足够清楚”。

正式裁决如下：

- 一行注释只适用于真正简单的 name helper / predicate / obvious constant wrapper。
- 只要符号带有参数、借用期、上下文、阻塞性、错误码、回调、副作用或前置条件，
  就必须使用完整的多行注释块。
- 对 public struct / union field 来说，只要字段名称本身无法完整表达语义、单位、范围、
  ownership、有效期或模式切换含义，就必须补字段注释。

因此不接受下面这些“看似省事，实际让接口继续模糊”的写法：

- `/** @return borrowed name */` 代替完整契约说明
- 整组函数只给一个总注释，不给单条入口注释
- public struct 暴露了几十个 field，却没有说明哪些是 caller-owned、哪些是 runtime-written
- 一份头文件里混用四种注释风格，让读者猜哪一种才是正式口径

## 9. Public API 分组与注释规则

### 9.1 单条注释不是问题，裸平铺才是问题

Themis 仍然坚持：

- 每个 public symbol 单独注释

但不接受下面这种写法：

- 文件头很完整
- 后面几十个函数只是机械地“一条声明 + 一条短注释”一直排到底
- 调用方看不出先后关系和使用阶段

### 9.2 正确写法

先按使用阶段给 API 分组，再给单条符号注释。

示例：

```c
/* Lifecycle -------------------------------------------------------------------- */

/**
 * @brief Initialize the shared input gesture subsystem.
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int tm_input_init(void);

/* Runtime Step ----------------------------------------------------------------- */

/**
 * @brief Consume one board input sample snapshot.
 *
 * @param  sample  Caller-owned sample snapshot from `bsp_input_sample()`.
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int tm_input_process_sample(const struct bsp_input_sample *sample);

/* Snapshot / Diag -------------------------------------------------------------- */
```

### 9.3 组注释应该写什么

一个 API 分组的组注释只需要回答：

- 这一组函数在哪个阶段用
- 是否需要先 init / bind / open
- 是否是热路径 / 只读 snapshot / 名字转换 helper

不要把组注释写成第二份文件头，也不要把组注释省略到完全看不出阶段。

正式建议写法是：

```c
/* Lifecycle -------------------------------------------------------------------- */
/*
 * Call these during bring-up / teardown before any runtime-step API.
 */
```

或者在语义足够简单时，直接保留 banner + 单条 symbol 注释。

### 9.4 Themis 不使用 STM32 风格 `@addtogroup` 包裹

Themis 可以学习 STM32 HAL 的“分组像手册”思想，但不把下面这套包裹式写法作为正式标准：

- `@addtogroup`
- `@{`
- `@}`

原因如下：

- 它并不能替代真正的 public symbol 契约说明。
- 它会把 public header 的视觉重心从“调用方如何接入”拉回“文档工具如何收组”。
- Themis 当前已经通过 `banner + 组注释 + 单条注释` 达成同样目的，且更利于代码审查与 diff 稳定性。

因此，后续新写或重写的 Themis public header，默认不再引入新的 `@addtogroup` 包裹块。

### 9.5 Doxygen 标签 MUST / SHOULD / MAY 口径

Themis 当前正式标签口径如下：

- MUST：
  - `@file`
  - `@brief`
  - `@version`
  - `@date`
  - `@param[in]` / `@param[out]` / `@param[in,out]`
  - `@return` 或 `@retval`
- SHOULD：
  - `@note`
  - `@warning`
  - `@details`
- MAY：
  - `@see`
  - `@deprecated`
  - `@pre` / `@post`
  - `@code` / `@endcode`

补充裁决：

- 当函数只有一个总体返回语义时，优先用 `@return`。
- `bool` / pointer / borrowed-name / snapshot-copy 这类查询型 API 默认优先用 `@return`，
  直接用一句话讲清“成功时是什么，失败时是什么”。
- 当函数需要逐项列出多个离散错误码、命名结果值或多种返回口径时，优先用 `@retval`。
- `@retval` 不是 `@return` 的机械替代品；只有当“逐项枚举结果”真的能让调用契约更清楚时才使用它。
- `@details` 只在 `@brief` 无法独立讲清 contract 时使用，不要机械堆长篇背景。
- 不允许为了“更像 HAL”而堆砌标签本身；标签只服务契约清晰，不服务样式表演。

### 9.6 Header consumer 假设

评审公共头文件时，默认 consumer：

- 不会打开 `.c`
- 不知道当前产品细节
- 不知道历史演进

因此如果头文件本身不能回答：

- 最小调用顺序
- 借用期
- blocking
- context
- failure 语义

就视为这个 public header 仍然不合格。

## 10. static state / struct / enum 的视觉组织

### 10.1 不要做“一整墙对齐”

当一个很长的名字把整段声明都拉得很丑时，不要继续把无关变量硬塞在同一堵对齐墙里。

正式做法是：

- 按语义拆组
- 组间留 1 个空行
- 让 `.clang-format` 只在小组内部对齐

### 10.2 static state 推荐分组

优先按下面顺序分组：

1. 资源句柄 / 静态属性
2. 对外回调 / 绑定信息
3. 状态码 / 运行状态
4. 统计计数器
5. 最近一次事件 / 快照缓存

例如：

```c
static osMutexId_t         _tm_input_mutex = NULL;
static StaticSemaphore_t   _tm_input_mutex_storage;
static const osMutexAttr_t _tm_input_mutex_attr = {
    .name    = "tm_input",
    .cb_mem  = &_tm_input_mutex_storage,
    .cb_size = sizeof(_tm_input_mutex_storage),
};

static tm_input_event_handler_t _tm_input_handler         = NULL;
static void                    *_tm_input_handler_context = NULL;
static int                      _tm_input_status          = TM_INPUT_STATUS_INIT;

static uint32_t _tm_input_sample_count           = 0U;
static uint32_t _tm_input_dispatch_count         = 0U;
static uint32_t _tm_input_dispatch_error_count   = 0U;
static uint32_t _tm_input_dispatch_handled_count = 0U;

static uint16_t          _tm_input_last_dispatch_event_mask  = 0U;
static enum bsp_input_id _tm_input_last_dispatch_input_id    = BSP_INPUT_ID_INVALID;
static uint8_t           _tm_input_last_dispatch_click_count = 0U;
```

注意：

- 这里的关键不是“完全不对齐”，而是**不让一个超长名字拖垮整个模块状态区**
- 通过空行切组，可以利用当前 formatter 的 `AcrossEmptyLines: false` 把视觉拖拽限制在小范围内

### 10.3 struct / enum 推荐分组

当 struct 或 enum 很长时，按语义分段，而不是从第一行一路无脑排到底。

推荐原则：

- 把超长数组、callback、snapshot 成员单独放一组
- 把 config / handles / status / counters / last event 分开
- 只有明确属于同一语义带的字段，才应连续排在一起

例如：

```c
struct tm_input_diag_snapshot {
    struct tm_input_channel_diag channels[BSP_INPUT_ID_COUNT];

    uint32_t sample_count;
    uint32_t dispatch_count;
    uint32_t dispatch_error_count;
    uint32_t dispatch_handled_count;
    uint32_t dispatch_pop_count;
    uint32_t dispatch_ignored_count;

    int      status;
    int      last_dispatch_result;
    uint16_t last_dispatch_event_mask;
    uint8_t  last_dispatch_click_count;

    enum bsp_input_id last_dispatch_input_id;
    bool              initialized;
};
```

比起“所有字段强行一列对齐”，这种写法更像给人看的结构，而不是给空格表演看的结构。

### 10.4 不要靠手写空格造美观

禁止：

- 手工插入大量空格试图“抵消”长标识符
- 在同一文件混用多套纵向对齐策略
- 因为怕对齐难看，就把完全不同语义的变量塞进一个匿名数组或魔法结构里

正确手段始终是：

- 改分组
- 改节奏
- 改语义组织

不是改成“人工排版艺术”。

## 11. 触达文件后的最低整改标准

### 11.1 touched `.h`

只要本轮改了公共头文件，就必须顺手把下列内容提升到当前标准：

- 文件头结构
- include 紧凑布局
- API 分组
- public symbol 注释
- 文件结尾闭合行

### 11.2 touched `.c`

只要本轮改了 `.c` 文件，至少应同步收口：

- `.c` 头注释不再停留在最小四行
- include 块改为紧凑布局
- 三行 banner 改为单行 banner
- 明显成墙的 static state 至少按语义切组
- 新增或改动附近的非显然逻辑补 why-comment

### 11.3 当局部修已经不够时

如果一个文件已经明显处于下面状态：

- 顶层顺序混乱
- include 块碎裂
- 头文件像裸声明仓库
- struct / static state 一整墙长空格

那么不要继续“补一小处算一小处”，而应整份文件重写到本文标准。

## 12. 最终结论

Themis 未来的高质量 `.c/.h`，应同时满足三件事：

- 架构边界和 owner 判断像 Zephyr 一样真实
- 文件可读性和说明节奏像 STM32 HAL 一样稳定
- 视觉组织和现代约束又仍然是 Themis 自己的：
  `100` 列、`#pragma once`、单行 banner、紧凑 include、明确 EOF 闭合

如果一个文件只是“逻辑正确”，但头文件像裸声明清单、源文件像格式化后的临时草稿，
那它仍然不算达到 Themis 的正式水位。
