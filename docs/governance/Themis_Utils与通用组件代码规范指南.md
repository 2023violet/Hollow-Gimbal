# Themis Utils 与通用组件代码规范指南

## 1. 文档定位

本文不是 `applications/bsp/device/drivers/subsys` 那套分层规则的简化版。

它专门约束下面这类代码：

- `components/utils/`
- 未来新增的纯协议库
- 纯 parser / codec / container / algorithm glue
- 类似 `canopen` 这类希望独立移植、独立测试、独立复用的通用库

一句话结论：

**这类代码首先是 portable library，其次才是 Themis 仓库中的一个目录。**

## 2. 最高原则

### 2.1 portability-first

默认目标不是“在 Themis 里能跑”，而是：

- 离开 Themis 也能用
- 离开 FreeRTOS 也能用
- 离开 AT32 也能用

### 2.2 组件不依赖环境，环境适配组件

禁止在库内部直接绑死：

- `cmsis_os2.h`
- `tm_device_*`
- `bsp_*`
- 厂商寄存器头
- 板级引脚号

需要外部能力时，优先通过：

- `ops`
- callback
- `ctx`
- 用户传入的 buffer / allocator / time provider

### 2.3 零惊喜

调用者必须能一眼看出：

- 这个库会不会阻塞
- 会不会分配内存
- 会不会持有外部指针
- 会不会在回调里反调用户代码

## 3. 与 Themis 分层代码的边界

### 3.1 不要机械套用 `drv/dev/bsp/app`

下面这些命名和思维方式，不应直接套到 utils/generic library：

- `drv_xxx`
- `dev_xxx`
- `bsp_xxx`
- `tm_device_open/close/read/write/control`

因为它们对应的是 Themis 的设备模型，不是通用库模型。

### 3.2 允许的 Themis 依赖

只有下面这些依赖默认允许：

- C 标准库头
- `tmdef.h` 这类纯宏/编译期工具头
- 完全不带 RTOS/板级/硬件语义的公共小头

### 3.3 不允许的 Themis 依赖

- `tm_log`
- `tm_assert` 以外的 subsystem 机制
- `tm_runtime`
- `tm_device`
- `bsp_*`
- `drv_*`

如果真的需要其中某项能力，应先重新判断它还算不算 `components/utils/`。

## 4. API 设计规则

### 4.1 先函数语义，后仓库语义

函数名优先描述算法/容器/协议行为，例如：

- `ring_buf_put`
- `mempool_alloc`
- `canopen_pdo_encode`

不要为了看起来“像 Themis”硬加不必要前缀层次。

### 4.2 上下文必须显式

每个对外 API 至少要能回答：

- 是否可重入
- 是否线程安全
- 是否 ISR-safe
- 是否需要外部锁

如果默认不是线程安全，必须明确写出来。

### 4.3 返回值统一

推荐继续使用：

- `int` 返回 `0 / -errno`
- `ssize_t` 返回长度或 `-errno`
- `bool` 返回纯判断

不要混入：

- `1 = success`
- 私有错误码体系
- 既返回长度又顺手改全局错误状态但不说明

### 4.4 输入/输出所有权清楚

指针参数必须写清：

- 调用期间借用
- 调用后是否继续持有
- 是否允许 `NULL`
- buffer 长度单位是 byte、item 还是 sample

## 5. 依赖与实现规则

### 5.1 默认不使用动态内存

优先级如下：

1. 用户静态提供对象和 buffer
2. 用户传入 allocator 接口
3. 最后才考虑库内直接分配

如果库内真的要分配，必须提供：

- 生命周期说明
- 失败路径说明
- 释放对称接口

### 5.2 默认不使用全局可变状态

优先使用：

- 实例结构体
- 调用者持有的 context

允许的全局状态只应是：

- 只读查表
- 编译期常量
- 完全可证明安全的本地静态缓存

### 5.3 编译期开关优先

可裁剪功能优先使用：

- `#if`
- `enum`
- `static inline`

不要把未启用功能留成：

- 运行时永远不进的死分支
- 只靠注释说“未来再实现”

## 6. 实时性与上下文规则

### 6.1 默认不阻塞

如果库不是明确的 blocking abstraction，就不要：

- `osDelay`
- semaphore wait
- busy-wait 长循环

### 6.2 回调前先解耦状态

如果库要执行用户回调，优先遵守：

- 先更新内部状态
- 再离开临界区
- 最后调用回调

不要在持锁状态下执行外部回调。

### 6.3 ISR 兼容不是默认义务

纯通用库可以不支持 ISR，但必须写明。

如果支持 ISR：

- 不能偷偷调用不可重入 API
- 不能偷偷走 heap / mutex / printf

## 7. include、注释与排版

### 7.1 include 顺序

portable library / utils 代码在文件布局和注释节奏上，也遵守：

- `docs/governance/Themis_C与H文件布局与注释书写规范.md`

但在 include 内容上继续保留 portability-first 的收敛口径。

`.c` 文件保持：

1. 自己的头
2. 最直接相关的同域头 / 协议头
3. 纯外部依赖头
4. 极少量必要的 Themis 纯工具头
5. C 标准库头

并且默认使用**一个紧凑 include 块**，不再人为插入分类空行。

### 7.2 头文件最小依赖

头文件只 include 对声明真正需要的内容。

不要依赖“别的头已经顺手 include 了”。

### 7.3 注释解释边界

优先写这些：

- 为什么不用 RTOS / BSP
- 为什么这里必须由用户提供 hook
- 为什么这个结构体这样分组
- 为什么这个函数不是线程安全

另外，portable library / utils 代码还必须满足：

- `.h` 必须是完整入口文档。
  只要 API 涉及 allocator、buffer、ctx、callback、线程安全、ISR 兼容、阻塞、所有权、借用期，就必须在头文件中写清楚，不能要求调用者去猜实现。
- `.c` 必须在关键算法点解释 invariant 与 boundary。
  例如：
  - 为什么先更新内部状态再调回调
  - 为什么这里不用动态内存 / 不加锁 / 不支持 ISR
  - 为什么这个结构体布局或位打包方式不能随便改
- touched-file 立即生效。
  任何本轮修改过的 utils / generic library 文件，都必须把改动周围的注释提升到当前标准，而不是继续往“只有函数头、没有契约说明”的代码上加逻辑。
- touched-header uplift 是硬要求。
  只要本轮改动了公共头文件，就必须把该头文件暴露给调用方的 public symbol
  注释补齐到当前标准；不能继续维持“文件头有理念、API 本身没注释”的状态。
- 对 utils / generic library 来说，`.h` 就是调用方的主入口。
  如果调用者不看 `.c` 便无法知道 buffer 生命周期、callback 上下文、线程安全边界、
  返回值语义或最小接法，就说明这个入口文档仍然不合格。

并且 `.h` 的写法要进一步收敛到下面这个强标准：

- 文件头先交代设计哲学，再给最小 usage example。
  新人第一次打开头文件时，应该先知道：
  - 这个库为什么这样设计
  - 为什么不用 heap / RTOS / BSP
  - 最小正确调用长什么样
- 每个 public symbol 单独注释。
  结构体、回调、宏、函数必须是一项一注释，而不是几项合并成一段“总说明”。
- 入口注释要能直接回答“怎么接”。
  例如：
  - buffer 谁分配、活多久
  - callback 在谁的上下文里触发
  - iterator / visitor 是否允许修改容器
  - inline helper 是 strict API 还是 convenience wrapper
- `components/utils/` 也不是“还没想清楚放哪就先丢进来”的缓冲区。
  只要某个类型名、枚举值、count、默认实例或错误口径明显绑定当前板子、
  当前 RTOS、当前产品语义，它就不再是 portable library，应回到真实 owner。

另外，对 portable library / utils 来说还要额外记住：

- `.h` 文件也必须按“分组目录 + 单条符号注释”的方式组织 API，不能把几十条函数裸平铺到底
- `.c` 文件也应通过语义分组拆开 static state / struct / enum，避免一堵长对齐墙

### 7.3.1 group 注释与单条注释必须同时成立

对 `components/utils/` 和 future generic library 来说，Themis 现在正式不接受下面两种极端：

- 只有文件头很完整，后面 public API 裸排到底。
- 只有一条条 `@brief`，但完全看不出 init / bind / runtime / snapshot / helper 的阶段关系。

正确口径是：

- 文件头先讲设计哲学、依赖边界和最小 usage example。
- API 分组注释再讲“这一组是什么阶段、什么时候调用、是否需要前置条件”。
- 单条 public symbol 注释再讲“这一项的参数、返回值、借用期、上下文和限制”。

这条规则对下面这些 public symbol 一视同仁，而不是只约束函数：

- public macro / constant
- public typedef / callback type
- public enum / non-obvious enum item
- public struct / union 及其 contract-bearing field
- public inline helper
- public function

### 7.3.2 portable library 不使用 `@addtogroup` 包裹式写法

Themis 在 utils / generic library 中明确不引入 STM32 HAL 风格的：

- `@addtogroup`
- `@{`
- `@}`

原因不是 Doxygen 本身“有错”，而是：

- 这套包裹并不能替代真正的 per-symbol contract 注释。
- 它会把 portable library 的 public header 重新拖回“文档工具优先”的旧式写法。
- Themis 现行标准已经通过 `banner + 组注释 + 单条注释` 完成同一目的，且更利于人工审查和 diff 稳定性。

因此，utils / generic library 的正式 public header 组织方式是：

- 单行 banner 做阶段分组
- 简短组注释交代阶段
- `/** ... */` 单条注释交代每个 public symbol 的契约

### 7.3.3 public header 注释样式与 Doxygen 标签口径

对 portable library / utils 的 public header，正式默认样式是：

- 组注释使用普通 C 注释或简短段落，负责交代阶段和用途。
- 单条 public symbol 使用放在声明正上方的 `/** ... */` 注释块。
- 不把行尾 `/* ... */` 或 `/**< ... */` 作为默认 public contract 写法。

只有在下面这类少数高密度场景中，才允许审慎使用 trailing 风格：

- 外部协议位定义 / 数据布局说明必须紧贴同一行常量
- dense table / bitfield / wire-format item 用正上方注释反而更难对应

即便如此，也应优先保持“少量例外”，而不是把整份 public header 写成 trailing comment 墙。

标签口径如下：

- MUST：
  - `@brief`
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

补充裁决：

- 只要函数返回的是单一语义结果，就用 `@return`。
- 只要需要逐项列出多个离散错误码或多种结果口径，就用 `@retval`。
- 不允许为了“像 STM32”而机械堆叠标签；标签只服务于调用契约清晰，而不是服务于文档工具表演。

### 7.4 节分隔样式

utils / generic library 的节分隔同样使用单行 banner：

```c
/* Includes --------------------------------------------------------------------- */
/* Public Types ----------------------------------------------------------------- */
/* Public Functions ------------------------------------------------------------- */
```

不要继续写成三行横线块。原因和 framework 代码一致：

- 不利于批量格式化
- 不利于全文检索
- 不利于保持统一 diff 粒度

如果某个 utils 文件需要批量收口，统一交给 `scripts/format_themis.ps1`
处理，不要手写多种横线风格混在同一仓库里。

仓库里已经达到这个标准的参考对象包括：

- `components/utils/pubsub/pubsub.h`
- `components/utils/micro_json/micro_json.h`
- `components/utils/hash_map/hash_map.h`

后续所有 utils / generic library 新增或重写的 `.h`，至少要向这三份头文件对齐，而不是退回到“函数列表式注释”。

## 8. 测试与验证规则

### 8.1 宿主机测试优先

这类代码默认应优先支持：

- host unit test
- fuzz / parser test
- deterministic input-output test

### 8.2 不把板测当主验证

如果一个 `utils` 或 generic library 只能靠板子测，通常说明它已经掺进了过多环境耦合。

## 9. 对 future generic library 的正式要求

以后如果在 Themis 里增加：

- `canopen`
- `modbus codec`
- `binary protocol parser`
- `filesystem format parser`

默认先问三件事：

1. 它是不是应该先写成 portable library
2. 它是不是应该先有 host test
3. 它是不是应该先通过 `ops/ctx` 注入环境能力

如果答案是“应该”，那它就应优先遵守本文，而不是直接套 `drv/dev/bsp` 思维。

## 10. 最终结论

`components/utils/` 和 future generic library 的优秀标准，不是“看起来像 Themis 分层代码”，而是：

- 足够克制
- 足够可移植
- 足够清楚
- 足够容易宿主测试

真正的高水平，不是把 framework 规则到处复制，而是知道哪些代码根本不该背那套重量。
