# Themis 注释规范化补强基线

## 1. 文档定位

这份文档不是代码规范的替代品，而是当前仓库一次 **存量代码注释整改** 的执行基线。

它回答三件事：

- 这次注释整改为什么要做
- 哪些层已经接近规范，哪些层明显还没达标
- 后续按什么顺序从 `applications -> bsp -> components -> device -> drivers -> subsys` 推进

正式规范仍以：

- [Themis_代码规范指南.md](./Themis_代码规范指南.md)

为准。

## 2. 当前基线结论

根据 2026-03-28 这一轮对当前代码的抽查，Themis 的注释质量已经出现明显分层差异：

- `device/`、`drivers/`、`subsys/` 的较新头文件里，已经有一部分进入“契约化注释”阶段
- `applications/`、`bsp/`、`components/` 里仍有不少文件停留在“文件头 + 声明列表”阶段
- 即使在同一层内，质量也不均匀，说明这不是“某一层全坏”，而是“新旧写法混用”

这意味着后续整改不能靠零散补一句注释，而必须按层收口。

## 3. 本次整改的正式验收标准

后续每个被整改的文件，至少要满足下面几条：

1. 文件头不仅有 `@file/@brief/@version/@date`，还要写出模块怎么理解。
2. `.h` 必须明确 ownership、lifecycle、context、blocking、buffer/callback 有效期中的相关项。
3. 非平凡公共符号按“一项一注释”写，不能只留裸声明。
4. `.c` 在非显然逻辑处补 why 注释，尤其是 owner 检查、回滚、restore、兼容语义折中。
5. 注释语言和表达风格在同一文件内保持一致，不写“翻译代码”的废话注释。

## 4. 抽查结论

### 4.1 `applications/`

当前明显需要补强的典型文件：

- `applications/services/adc_service.h`
- `applications/services/display_service.h`
- `applications/tasks/uart_task.h`

当前问题集中在：

- 头文件只有最小文件头，没有 design overview / usage / context 摘要
- 公共结构体字段和函数声明缺少逐项契约注释
- service/task 的调用顺序没有在头文件里写死

### 4.2 `bsp/`

当前质量不均匀：

- `bsp_display.h` 已经开始写 layering / ownership contract，可作为 BSP 头文件参考样式
- `bsp_console.h` 仍然偏薄，缺少 early raw path / runtime path / panic path 的完整契约说明

结论：

- BSP 层不是“全都重写”，而是优先把薄头文件拉齐到 `bsp_display.h` 的水平

### 4.3 `components/`

当前需要重点补强的典型文件：

- `components/audio/buzzer/buzzer.h`
- `components/display/ssd1316/ssd1316.h`

当前问题集中在：

- 公开结构体和回调表缺少 ownership / callback context / buffer lifetime 注释
- 组件头文件大多只写“是什么”，没有写“怎么用”
- 公共字段没有逐项说明，新同事和新 AI 难以直接拿头文件理解调用关系

### 4.4 `device/`

`device/` 目前是相对较好的层。

可直接作为参考样式的典型文件：

- `device/serial/dev_uart.h`
- `device/adc/dev_adc.h`

但这不代表可以跳过该层整改，正式要求仍然是：

- 对所有仍偏薄的 typed header 做统一复核
- 保持 checked wrapper、legacy helper、ISR bridge 这些关系在头文件里始终可见

### 4.5 `drivers/`

`drivers/` 也已经出现较好的契约化写法，例如：

- `drivers/at/drv_uart.h`

但仍有需要补强的代表：

- `drivers/at/drv_adc.h`

当前问题集中在：

- 有模块级 contract 注释，但公开结构体字段和 helper 的逐项注释还不均匀
- 某些 inline helper 只有 warning，没有说明 why / 使用边界

### 4.6 `subsys/`

`subsys/` 当前整体质量较高，但也不完全一致。

可作为参考样式的文件：

- `subsys/log/tm_log.h`
- `subsys/shell/tm_shell.h`

这层后续更适合作为最后统一校准层，而不是第一批“救火对象”。

## 5. 正式执行顺序

后续批量整改按下面顺序推进：

1. `applications/`
   - 因为这层当前最薄，而且用户直接感知最强
2. `bsp/`
   - 先把板级 owner 和 transport 契约说清楚
3. `components/`
   - 把 bus ops / backend ops / ctx / buffer 关系补硬
4. `device/`
   - 统一 typed API 头文件口径
5. `drivers/`
   - 补 southbound runtime bridge 的 why 注释
6. `subsys/`
   - 最后统一做风格校准和残缺收口

## 6. 每轮整改的最小工作法

后续实际做代码注释整改时，建议坚持下面规则：

1. 一轮只吃一个层级，或者一个层级里的一个子切片。
2. 只补注释，不夹带行为变更；如果必须改行为，另开独立变更。
3. 每改完一个切片，至少做一轮编译检查。
4. 每轮结束都同步更新维护日志。
5. 如果某个文件已经明显不适合“局部补注释”，就直接整份头文件重写到规范水平。

## 7. 这轮的正式结果

2026-03-28 这一轮按用户要求不改代码，只做两件事：

- 把当前代码事实同步回 `docs/`
- 把注释规范化整改基线正式写成文档

因此，后续任何 AI 或人工进入这项工作时，都应以本文件和 `Themis_代码规范指南.md` 作为共同起点。

## 8. 2026-03-28 `bsp/` 阶段收口

本轮已经按既定顺序完成 `bsp/` 层的注释规范化补强，重点包括：

- `bsp_console.*`
  - 补齐 early raw path / runtime path / panic path 的职责边界、生命周期和上下文约束。
- `bsp_devices.*`
  - 明确 pre-kernel glue、SUBSYS 导出重绑定、DEVICE 阶段默认实例组装之间的顺序关系。
- `bsp_display.*` / `bsp_buzzer.*`
  - 统一写清默认实例所有权、late init 入口、任务上下文限制和共享实例借用语义。
- `bsp_config.h` / `bsp_pins.h`
  - 把板级事实宏从“罗列常量”提升为“带语义的 board facts 目录”。

这一轮仍然遵守基线里的工作法：

- 只做注释契约和 why-comment 补强，不夹带行为修改。
- 完成后立即做一次固件构建验证。
- 同步维护日志，确保后续进入 `components/` 时有明确交接点。

因此，下一阶段的正式目标更新为：

1. `components/`
   - 优先收口 `backend ops / bus ops / ctx / buffer lifetime` 这类最容易失去契约的信息。
2. `device/`
   - 统一 typed API 头文件的 open/close/control 与 checked wrapper 语义说明。
3. `drivers/`
   - 补 southbound runtime bridge、rollback、IRQ/运行时边界的 why-comment。
4. `subsys/`
   - 最后做风格校准与残缺收口。

## 9. 2026-03-28 `components/` 阶段收口

本轮已经按既定顺序完成 `components/` 层的主干注释规范化补强，重点包括：

- `components/audio/buzzer/*`
  - 把 backend hooks、sequence table 借用期、阻塞 beep 与非阻塞 playback 的边界写清楚。
- `components/display/mono_canvas/*`
  - 把 framebuffer alias、bitmap/font 资源借用期、page packing 约束和 UTF-8 / fallback 绘制语义写清楚。
- `components/display/ssd1316/*`
  - 把 bus ops、framebuffer 生命周期、page flush 约束和 controller init/cleanup 语义写清楚。
- `components/input/button/*`
  - 把 debounce tick、trigger mode、combo ownership 与事件生成时机写清楚。
- `components/algorithm/pid/*`
  - 把 full path / fast path 的 `dt` 语义、improve flags 和 PID 状态对象写清楚。
- `components/algorithm/filter/kalman_filter.*`
  - 先把公共头文件拉回正式契约形态，并补最关键的空指针/零维度保护。
- `components/protocol/vofa/*`
  - 清掉旧编码污染，统一 FireWater / JustFloat 的协议语义与绑定寿命说明。

这一轮除了注释契约补强，还顺手修了几类明确风险：

- `pid.c`
  - 显式补上 `<string.h>`，不再依赖隐式头文件透传。
- `kalman_filter.c`
  - 为 `Kalman_Filter_Init()`、`Kalman_Filter_Measure()`、`Kalman_Filter_Update()` 与 `H_K_R_Adjustment()` 补了最基础的空指针保护。
  - `uSize == 0` 时避免对可能为空的控制向量存储做 `memcpy()`。
- `vofa.c`
  - 删除旧的注释化死代码分支，避免后续误把废弃实现当成正式路径维护。

因此，下一阶段的正式目标更新为：

1. `device/`
   - 统一 typed API 头文件的 open/close/control、checked wrapper 与借用语义说明。
2. `drivers/`
   - 收口 southbound runtime bridge、rollback、IRQ/任务上下文边界的 why-comment。
3. `subsys/`
   - 最后做风格校准、薄头文件补强和残缺收口。

### 9.1 `kalman_filter.c` 最终收口补充

- `components/algorithm/filter/kalman_filter.c`
  - 源文件级说明、私有 helper 分段与 why-comment 已补齐到当前 `components/` 基线风格。
  - 临时矩阵/向量工作区改为按 `max(xhatSize, zSize)` 分配，避免动态量测路径在 `zSize > xhatSize` 时越界。
  - 自动量测重建阶段新增 `MeasurementMap` 范围校验，防止 0 值或越界映射破坏 `H` 矩阵。

至此，`components/` 可以视为正式收口完成，主任务继续按顺序进入 `device/`。

## 10. 2026-03-28 `device/` 阶段收口

本轮按既定顺序进入 `device/`，但执行策略不是机械全改，而是先逐文件复核，再只改真正掉队的基座文件与薄模块。

重点收口如下：
- `device.h` / `object.h`
  - 作为整层 typed API 与对象容器的公共基座，已统一补成正式契约头文件，后续 `drivers/` 与 `subsys/` 可以直接沿用这一套口径。
- `device.c` / `object.c`
  - 文件级说明与分段注释已拉回当前仓库风格，明确一个负责标准设备生命周期，一个负责对象容器与快照迭代语义。
- `device/wdt/*`
  - 旧风格薄头文件已升级为正式 typed contract，inline helper 与注册前置条件也同步补强。

本轮顺手收掉的明确风险：
- `device/gpio/dev_gpio.c`
  - `tm_pin_get()` 现在会先拒绝 `name == NULL`，避免空指针直接透传进 southbound `pin_get()`。
- `device/wdt/dev_wdt.h`
  - `tm_wdt_start()` / `tm_wdt_stop()` / `tm_wdt_feed()` / `tm_wdt_set_timeout()` 现在有显式空指针保护。
- `device/wdt/dev_wdt.c`
  - `tm_wdt_device_register()` 现在要求 `ops->control` 必填，避免注册成功后才在运行期暴露 southbound 契约缺失。

复核结论：
- `dev_adc.h` / `dev_i2c.h` / `dev_spi.h` / `dev_pwm.h` / `dev_uart.h` 等 typed header 已基本达到当前基线，因此本轮不做为“看起来统一”而进行机械改写。

因此，`device/` 可以视为正式收口完成，主任务继续按顺序进入 `drivers/`。

## 11. 2026-03-28 `drivers/` 阶段收口

本轮进入 `drivers/` 后，执行策略仍然是“先全读，再只改掉队文件”，不把已经基本达标的 southbound 头文件机械重写一遍。

重点收口如下：
- `drv_common.h`
  - 作为整层公共根头，已补成正式 southbound include contract，并补上 `#pragma once` 与分层边界说明。
- `drv_adc.h` / `drv_wdt.h` / `drv_flash.h`
  - 已统一补清 board-mirror、runtime-state、northbound 绑定和 generated bring-up 边界。
- `drv_adc.c` / `drv_wdt.c` / `drv_flash.c`
  - 文件级说明已同步到当前仓库风格，强调 southbound 只做 runtime bridge，不重新定义板级事实。

本轮顺手收掉的明确问题：
- `drv_common.h`
  - 补上 `#pragma once`，消除公共根头继续依赖重复包含偶然不出错的脆弱状态。
  - 修正注释中会触发编译器 warning 的 `drivers/at/*` 写法，避免新基线自身引入 comment warning。

复核结论：
- `drv_uart.h` / `drv_i2c.h` / `drv_spi.h` / `drv_pwm.h` / `drv_gpio.h` 已基本达到当前基线，因此本轮不做机械改写。

因此，`drivers/` 可以视为正式收口完成，主任务继续按顺序进入 `subsys/`。

## 12. 2026-03-28 `subsys/` 阶段收口

本轮按既定顺序进入最后一层 `subsys/`，执行策略仍然是“先全读，再只改真正薄弱的模块”，不为追求表面统一去重写已经基本达标的 framework 头文件。

重点收口如下：
- `subsys/runtime/*`
  - 把 runtime 运行时桥接从“薄声明头”补强为正式契约头文件，写清 ISR / task / pre-scheduler 三种上下文下的 tick、heap、critical-section 语义。
- `subsys/assert/*`
  - 把 assert 层从旧风格说明升级为正式 fault-report contract，补清配置拷贝、error-map 借用期、fatal handler 预期和 `TAG` 使用约束。
- `subsys/shell/*`
  - 把 shell 层从“功能说明”提升为正式 command-registry contract，补清静态导出、transport callback 借用期、单 owner 任务上下文和 bounded-buffer 取舍。
- `subsys/daemon/*`
  - 把 daemon 头源文件补齐 software-watchdog 设计口径，明确 feed 快路径、process 轮询路径、list owner 责任和 teardown 前提。

复核结论：
- `subsys/export/*`
  - 导出阶段模型、优先级规则、range executor 和 linker-order 语义已经达到当前基线，本轮保留。
- `subsys/ipc/*`
  - message lifecycle、ISR deferred publish、slab ownership 与 O(1) free 语义已经足够完整，本轮保留。
- `subsys/log/*`
  - drop-on-busy 并发模型、backend 约束和 runtime lock 语义已经基本达标，本轮保留。
- `subsys/time/*`
  - unified clocksource、RTOS-aware delay 和 probe 语义已经完整，本轮保留。

本轮没有发现需要另起行为补丁的明确运行时缺陷，因此仍遵守本轮主任务边界：
- 只做注释契约、文件级说明和 why-comment 收口，不夹带功能修改。
- 完成后立即做一次固件构建与 host tests 复核。
- 同步维护日志，把 `subsys/` 收口结果写回治理文档。

因此：
1. `applications -> bsp -> components -> device -> drivers -> subsys` 这一轮注释规范化补强已经按既定顺序全部完成。
2. 后续再进入相关代码时，不再按“整层补注释专项”推进，而改为按功能需求做增量维护，并继续以当前基线作为正式口径。
