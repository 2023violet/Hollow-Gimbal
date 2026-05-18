# FalconFoc 下一阶段正式执行路线图

## 1. 文档目标

这份文档用于正式裁决 FalconFoc 当前从“已经可运行”进入“可持续演进”的下一阶段执行顺序。

本文不讨论历史方案，不讨论抽象上的“以后可能”，只回答四个问题：

1. 现在最优先要做什么。
2. 哪些事情现在不该做。
3. 每一阶段的目标、边界、输入输出是什么。
4. 如何判断这一阶段真正完成，而不是“看起来改了很多”。

---

## 2. 当前项目状态判断

基于当前仓库与实机现状，FalconFoc 已经具备以下事实：

- `platform/at/at32f403a` 已经是正式 SoC bring-up owner。
- `bsp/board + bsp/ports` 已经具备当前产品最小可用支撑。
- `applications/` 已经承接了启动、shell、状态机、参数、FOC 产品语义。
- 默认 shell 已切到 `diag`，并已验证可通过 shell 正常启机、切模式、下发速度目标、实际控制电机。
- 当前项目仍然只有一个真实产品：FalconFoc 本体。
- 当前产品仍然不组装 Themis `device/` / `drivers/` runtime。

这意味着：

- 当前阶段的主矛盾已经不是“能不能跑起来”。
- 当前阶段的主矛盾是“产品 ownership 和长期演进口径是否稳定”。

所以，下一阶段不应该优先继续扩 BSP，也不应该优先做“为了抽象而抽象”的 FOC 组件化。

---

## 3. 总体裁决

FalconFoc 下一阶段正式顺序裁决如下：

1. `P0` 先收口参数系统与恢复口径。
2. `P1` 再收口应用层 ownership 与启动/状态/命令闭环。
3. `P2` 再推进 BSP 软任务外设薄封装扩展。
4. `P3` 最后做算法下沉审查，而不是先做算法搬家。

一句话概括：

**先把“产品怎么活、怎么记、怎么恢复”定死，再去谈“结构怎么更漂亮”。**

---

## 4. 为什么不是先做 BSP

当前不优先继续做 BSP，原因有四个：

### 4.1 当前 BSP 已经满足“FOC 单产品跑通”

当前板级最关键的目标已经实现：

- 上电启动
- shell 可用
- 状态切换可用
- 电机可控制

在此基础上继续大量优化 BSP，并不能直接提升产品语义稳定性。

### 4.2 当前没有大量新增软任务外设需求

只有当 FalconFoc 下一步明确要接入：

- OLED
- LED
- 蜂鸣器
- 按键
- 电池电量检测
- 充电检测
- 关机倒计时

这类软任务外设时，`bsp/ports` 才会进入高优先级建设阶段。

### 4.3 FalconFoc 的核心风险不在 BSP，而在策略口径

当前真正容易失控的是：

- 参数保存策略
- 参数恢复策略
- 参数版本升级策略
- shell 写入口与状态机之间的关系
- 启动链上各阶段的职责边界

这些都属于应用层与参数层，而不是 BSP 层。

### 4.4 BSP 过早扩张会掩盖主问题

如果现在继续扩 BSP，很容易出现：

- 目录更多
- port 更多
- 接口更多
- 实际产品口径仍然不稳定

这会制造“工程看起来更大了”，但并没有真正降低产品风险。

---

## 5. 为什么不是先做 FOC 算法组件化

当前也不应优先把 FOC 算法整体搬入 `components/`。

### 5.1 当前只有一个产品 owner

FalconFoc 现在只有一个真实产品语义：

- 一个状态机
- 一套校准流程
- 一套保护策略
- 一套参数恢复口径
- 一套 shell/operator 入口

此时把 FOC 代码整体“通用化”，大概率是在提前抽象。

### 5.2 FOC 主体并不天然是“可复用组件”

FOC 相关代码里天然混着三类东西：

1. 纯数学或控制算法
2. 产品状态语义
3. 与硬件强耦合的控制路径

只有第一类天然适合进入 `components/algorithm`。

第二类和第三类如果强搬进去，只会让组件层掺入产品语义与硬件路径，破坏边界。

### 5.3 当前更需要的是“审查”，不是“搬家”

正确顺序不是：

- 先把东西搬去 `components/`

而是：

- 先审查哪些代码是真正纯算法
- 再逐项决定是否下沉

---

## 6. 正式阶段划分

## 6.1 `P0` 参数系统与恢复口径收口

### 目标

把 FalconFoc 的参数保存、参数恢复、参数版本升级、启动恢复、shell 触发保存这条链，彻底对齐到当前 Themis 的正式口径。

### 范围

核心 owner：

- `applications/algo/foc_param.c`
- `applications/algo/foc_param.h`

联动 owner：

- `applications/tasks/motor_task.*`
- `applications/shell/app_shell_cmd_store.c`
- `applications/shell/app_shell_cmd_cfg.c`
- `applications/shell/app_shell_cmd_cal.c`
- `docs/architecture/Themis_参数持久化存储设计说明.md`

### 必做事项

1. 明确三组参数的 owner 与 schema 边界：
   - `motor`
   - `enc`
   - `sys`
2. 审查当前 payload 是否包含不该持久化的运行态字段。
3. 固化“空槽正常、CRC 隔离、版本可恢复、写入避重复”的正式语义。
4. 明确版本升级策略：
   - 哪些字段变更只需默认补齐
   - 哪些字段变更必须迁移
   - 哪些字段变更必须判废并回默认
5. 明确 shell 写入口到 save/load/erase 的权限边界。
6. 明确启动恢复顺序与失败回退行为。

### 本阶段不做

- 不新增复杂参数后台服务
- 不引入 TEC 的 `device/` / `drivers/` 参数运行时
- 不为了“像 TEC”而复制 TEC 的组织形式

### 验收标准

1. 文档层：
   - 参数设计文档可清楚回答“保存什么、不保存什么、为什么”。
2. 代码层：
   - 参数 schema、slot 语义、恢复口径与文档一致。
3. 行为层：
   - `store save/load/status/erase`
   - 重启恢复
   - 版本不匹配恢复
   - CRC 损坏隔离
   都能被验证。
4. 板级层：
   - 至少完成一轮真实上板保存-重启-恢复验证。

---

## 6.2 `P1` 应用层 ownership 与产品闭环收口

### 目标

把 FalconFoc 现有应用层真正变成“单产品 owner 清晰”的结构，而不是“先能跑起来，后面再慢慢看”。

### 范围

核心 owner：

- `applications/app_main.c`
- `applications/tasks/motor_task.*`
- `applications/tasks/motor_fsm.*`
- `applications/tasks/app_shell_task.*`
- `applications/shell/*`
- `applications/periph/*`

### 必做事项

1. 固化启动链 owner：
   - `app_main -> bsp_init -> export -> task bring-up`
2. 固化状态机与 shell 的关系：
   - 什么命令只是提交事件
   - 什么命令允许直接改配置
   - 什么命令必须在 `idle`
3. 收口校准流程 owner：
   - 编码器校准
   - 电机参数校准
   - 校准中断/退出/失败恢复
4. 收口故障与恢复路径：
   - `start / stop / disable / fault clear`
5. 收口任务职责：
   - `shell_task`
   - `app_shell_task`
   - `motor_task`
   - `motor_safe_task`
   - `vofa_task`

### 本阶段不做

- 不复制 TEC 的 UI/service/device 模式
- 不为了统一目录而拆分不存在的产品子域
- 不强行把只有一处使用的代码提炼成“通用服务”

### 验收标准

1. 每个应用层文件都能回答：
   - 它是谁的 owner
   - 它不负责什么
2. shell -> event -> task -> 状态变化链条清晰可追踪。
3. 启动、停止、故障清除、校准进入退出都能在板级验证。
4. 不再出现“命令能改，但系统语义说不清”的状态。

---

## 6.3 `P2` BSP 软任务外设薄封装扩展

### 目标

只在 FalconFoc 真实需要更多软任务外设时，按 “功能薄封装” 扩展 `bsp/ports`。

### 触发条件

只有明确要做产品化外围时，才进入本阶段，例如：

- 指示灯
- 蜂鸣器
- 按键
- OLED
- 电池检测
- 充电检测

### 范围

核心 owner：

- `bsp/ports/*`
- `bsp/board/*`
- 必要时联动 `applications/periph/*`

### 必做事项

1. 只按功能建 port，不按外设建 HAL。
2. 只保留最小函数面。
3. 共享总线只在确实需要时做最小通道文件。
4. 严禁引入：
   - vtable
   - device registry
   - pin database
   - 多层 generic hal

### 本阶段不做

- 不把 FOC 热路径塞进 `bsp/ports`
- 不包装 ADC/PWM/采样对齐/电流环 ISR
- 不恢复 Themis `dev/drv` runtime

### 验收标准

1. 新增软任务外设都能在 `bsp/ports` 找到清晰 owner。
2. `applications/` 不再散落硬件细节。
3. FOC 热路径依旧保持直控，不受影响。

---

## 6.4 `P3` 算法下沉审查与组件化最小化

### 目标

不是“把 FOC 搬到 components”，而是做一次正式算法审查，判定哪些代码值得下沉。

### 审查原则

只有同时满足以下条件的代码，才允许考虑进入 `components/algorithm`：

1. 不含产品状态语义
2. 不依赖 `_motor_ins`
3. 不依赖具体 shell、task、calib 流程
4. 不直接包含 vendor MCU 头
5. 可以独立做 host test
6. 后续确实存在复用价值

### 推荐候选

优先考虑：

- 纯 PID 算法
- 纯滤波算法
- 纯轨迹规划
- 纯数学变换/估算器

谨慎对待：

- 带 `motor_dev` 映射的控制逻辑
- 与校准流程耦合的算法
- 与保护策略耦合的逻辑

不建议下沉：

- FOC 主控制编排
- 热路径硬件耦合控制
- 产品状态驱动逻辑

### 验收标准

1. 形成一份“可下沉 / 不可下沉 / 暂缓”的清单。
2. 每个条目都有边界理由。
3. 不发生“搬了位置，但边界更糟”的伪重构。

---

## 7. 推荐正式执行顺序

正式推荐顺序如下：

1. `P0` 参数系统与恢复口径收口
2. `P1` 应用层 ownership 与产品闭环收口
3. `P2` BSP 软任务外设薄封装扩展
4. `P3` 算法下沉审查与组件化最小化

不推荐顺序如下：

1. 先大修 BSP
2. 先把 FOC 全搬去 `components`
3. 先把目录改得和 TEC 一样
4. 先做“更通用”的抽象层

---

## 8. 当前最推荐的立即起步任务

如果只选一个现在就应该开始的方向，正式推荐是：

### 起步任务 A

**对 `applications/algo/foc_param.*` 做一次完整口径收口。**

包括：

- payload 审查
- 版本升级策略审查
- save/load/erase shell 行为审查
- 启动恢复链审查
- 重启恢复板测闭环

### 起步任务 B

在 A 完成后，继续做：

**应用层 owner 与启动/状态/命令闭环梳理。**

---

## 9. 一句话裁决

**FalconFoc 下一阶段不应先追求“更像 TEC”，而应先把参数口径和应用层产品 ownership 定死；BSP 继续最小化前进，算法组件化只做审查后最小下沉。**

