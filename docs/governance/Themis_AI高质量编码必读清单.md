# Themis AI 高质量编码必读清单

> **V2 适用范围（2026-11-24）**：Legacy Themis 固件（`applications/`、`bsp/`、`components/`、`subsys/`、FreeRTOS、Shell）已删除，只保留在 Git tag `legacy-final`。本文中这些目录与分层示例属于历史背景；V2 目录与职责以 [AGENTS.md](../../AGENTS.md) 第 3 节为准。命名、注释、文件布局、错误处理与验证纪律等通用规则继续适用。

## 1. 文档定位

这份文档是 Themis 面向新 AI 和新同学的高质量编码必读清单。

它不替代 [AGENTS.md](../../AGENTS.md) 与[项目章程](../architecture/FalconFOC_Project_Charter_and_Engineering_Memory_v1.0.md)，而是把“怎样让 AI 写出更好的代码”收口成一套可执行的工作流、检查单和回头看制度。

如果只允许新增一份必须遵守的协作文档，那么这份文档就应该成为那份文档。

## 2. 外部经验收口成 Themis 规则

Themis 不照搬任何一个大项目，但吸收这些成熟来源里最稳定、最有工程价值的共识：

- OpenAI 的经验强调：任务描述必须具体，目标、约束、上下文、期望输出要提前说清楚，复杂任务应拆步并给出验收标准。
- GitHub Copilot/Coding Agent 的经验强调：问题范围要收紧，相关文件和预期行为要一起给，AI 需要明确知道“改哪里、不要改哪里、改完怎么算完成”。
- Google Engineering Practices 强调：代码评审首先看正确性、可读性、长期维护成本，而不是先看作者花了多少时间。
- Zephyr 的经验强调：代码风格、API 文档、配置边界和注释契约必须前置，不允许“实现先跑起来，文档以后补”。
- RT-Thread 的经验强调：设备模型、组件边界、接口风格要长期一致，不能一个模块一个写法。

Themis 的正式结论只有一句：

**AI 写得更好，不靠一句神奇提示词，而靠“上下文足够准确 + 边界足够清楚 + 验证足够扎实 + 回头看足够严格”。**

## 3. 让 AI 写得更好的 13 条硬规则

### 3.1 先给任务定义包，再让 AI 动手

任何一轮任务，至少要把下面五件事说清楚：

- 目标：这轮到底要解决什么问题。
- 范围：允许动哪些文件，不允许碰哪些层。
- 契约：成功语义、错误语义、生命周期语义是否会变化。
- 验收：至少要通过什么编译、测试、实板或资源检查。
- 非目标：这轮明确不做什么，防止 AI 越写越偏。

没有任务定义包，AI 很容易从“优化一个问题”滑向“顺手重写半套架构”。

### 3.2 先给边界包，再给实现自由

在 Themis，边界比技巧更重要。开始写代码前，必须先明确：

- 目标文件属于 `applications / bsp / components / device / drivers / subsys / docs` 哪一层。
- owner 是谁：board truth、generated truth、runtime state 各自归谁。
- 调用上下文是什么：task、ISR、init-only、panic path，还是 mixed context。
- 热路径在哪里：是否在 ISR、DMA completion、轮询等待、日志链、协议链上。
- 资源约束是什么：这轮是否影响常驻 RAM、热路径栈、ROM、锁等待。

如果边界没有先说清楚，AI 往往会写出“局部对、全局错”的代码。

### 3.3 一次只解一个主问题

高质量 AI 施工不是“一次改越多越厉害”，而是：

- 每一轮只解决一个主问题。
- 顺手修改只能处理低风险、同方向、能明确证明收益的内容。
- 如果新发现的问题会改变原任务方向，先记录评审结论，再决定是否并入。

这条规则直接决定代码会不会失控。

### 3.4 先理解现状，再提出新结构

AI 在 Themis 里不允许先发明新层、新 owner、新抽象，再倒推现有代码去配合。

正确顺序必须是：

1. 读现有文档。
2. 读现有契约。
3. 读现有代码与已有 diff。
4. 判断现在的问题到底是实现问题、契约问题，还是文档问题。
5. 只有在现有结构无法容纳目标时，才允许改结构。

### 3.5 强制进入“评审模式”

AI 不仅要会写代码，还要会像 reviewer 一样审自己。

每轮都必须显式做一次“评审模式切换”，优先检查：

- 有没有改坏成功/失败语义。
- 有没有跨层污染。
- 有没有引入新的 owner 冲突。
- 有没有把慢路径逻辑带进热路径。
- 有没有把一次性调试逻辑做成常驻成本。
- 有没有漏掉文档、测试、资源验证。

### 3.6 把“回头看”制度化

Themis 正式要求 AI 至少做三次回头看。

第一次回头看：开工前。

- 需求有没有理解偏。
- 文件落层有没有放错。
- owner 和调用上下文有没有看漏。
- 这轮到底是修实现、修契约，还是修文档。

第二次回头看：第一版改完后。

- 重新读自己刚改过的代码，不按作者视角，而按 reviewer 视角。
- 检查命名、注释、错误码、锁边界、ISR 安全、常驻 RAM、hot path。
- 搜索同类代码，确认没有写出风格孤岛。

第三次回头看：收尾前。

- `git diff` 是否只包含本轮目标内容。
- 测试、构建、资源数字是否真的复核过。
- 文档、维护日志、计划文档是否同步。
- 这轮是否已经达到可以 `commit/push` 的状态。

没有回头看，AI 最容易把“能跑”误判成“够好”。

### 3.7 所有改动都要绑定验收

Themis 不接受“感觉正确”。

每类改动都必须绑定最小验收：

- 改 `components`：至少要有 host test 或纯逻辑验证。
- 改 `subsys`：至少要有契约级或编译级验证。
- 改 `device`：至少要验证 northbound contract。
- 改 `drivers`：至少要做固件构建，必要时做联调或实板复核。
- 改资源相关内容：必须重新看 size/map/基线。

### 3.8 文档不是收尾装饰，而是契约的一部分

如果本轮改动会影响下面任一项，就必须同步文档：

- 边界
- owner
- 生命周期
- 配置语义
- 错误语义
- 测试口径
- 新 AI 或新同学最容易误解的点

在 Themis，文档缺失本身就是质量缺陷。

### 3.9 先保守，后极致

Themis 追求高性能，但不接受“先把系统写脆，再靠规范补救”。

默认优先级必须是：

1. 正确性
2. 契约稳定
3. 可恢复性
4. 可观测性
5. 性能
6. 进一步极限优化

真正优秀的优化，不是单点快，而是快、稳、可测、可解释。

### 3.10 资源优化必须算总账

任何 ROM/RAM 优化都必须回答四个问题：

- 节省了多少。
- 换来了什么复杂度。
- 有没有把成本从常驻 RAM 挪到了栈、热路径、锁等待或文档复杂度。
- tiny 档、standard 档、diag 档是否仍然可收口。

只看一个数字，不看总账，常常会把“看起来更省”做成“实际上更差”。

### 3.11 把 AI 规则做成可复用模板

比“临场发挥”更可靠的是模板化输入。

Themis 后续给 AI 任务时，推荐固定带上四个包：

- 任务包：目标、范围、非目标、验收。
- 边界包：分层、owner、上下文、热路径、资源。
- 证据包：现状文件、已有文档、已有错误信息、已有基线。
- 回头看包：要求 AI 在结束前做 reviewer 复核、验证复核、文档复核。

这比单纯说一句“认真点”有效得多。新 AI 接手时，先按 [AGENTS.md](../../AGENTS.md) 第 1 节的读序建立仓库心智图。

### 3.12 把规则写进 AI 真会读取的入口文件

只把规则放在文档里，还不够。

要想让后续 AI 真正写得更好，还必须把最核心的规则同步到仓库级入口文件，例如：

- `AGENTS.md`
- `.github/copilot-instructions.md`
- path-specific instructions

GitHub 的官方建议本身就强调：清晰的 repository custom instructions、构建测试命令、目录结构说明和 path-specific instructions，会显著提高代理式改码质量。

Themis 的正式做法是：

- 详细规则放 `docs/governance/`
- 高优先级执行骨架同步到 `AGENTS.md`
- 后续如果引入其他代理入口，再做同义同步，而不是各写各的

### 3.13 每轮必须形成可追溯闭环

只要本轮已经产生真实代码或正式文档改动，就必须做到：

- 变更可追溯：`git diff` 清楚。
- 验证可追溯：构建/测试/资源结果清楚。
- 原因可追溯：维护日志清楚。
- 结果可追溯：`git commit` 清楚。
- 条件允许时远端也同步：`git push` 清楚。

没有闭环，下一轮 AI 的上下文质量会显著下降。

### 3.14 先写不变量、失败模式和反例

真正高质量的 AI 改码，不是“先改再看会不会炸”，而是先把下面三件事写清楚：

- 不变量：哪些语义绝对不能变，例如生命周期、owner、错误码、线程上下文、ISR 安全。
- 失败模式：这轮最可能引入什么坏结果，例如死锁、悬空指针、热路径变慢、常驻 RAM 增长、文档与实现失配。
- 反例样本：至少列 2 到 3 个“如果写坏了会怎样”的具体场景，用它们反向约束实现。

这一步的价值非常大。它会逼着 AI 在动手前先理解“什么不能被破坏”，比单纯强调“认真点”有效得多。

### 3.15 优化必须证据化，禁止“肉眼判断式优化”

只要任务目标里包含下面任一关键词：

- 变慢 / 变快
- 占用变高 / 变低
- 阻塞 / 抖动 / 卡顿
- 死机 / 超时 / 丢包 / 现场偶现

就必须先做证据化动作，再讨论优化方案：

- 先拿到当前基线：构建大小、关键计数器、任务周期、热路径耗时、错误计数。
- 先确认观测口径：数据来自 map、`arm-none-eabi-size`、调试器、OpenOCD、逻辑分析仪，还是日志。
- 先区分“真实变差”与“感知误差”。
- 没有证据，不允许把猜测写进正式结论。

在嵌入式系统里，很多“感觉慢了”的问题最后都不是主观感觉本身，而是某个等待路径、锁边界、DMA completion 或任务节拍真的发生了漂移。AI 必须先证据后结论。

### 3.16 调试埋点必须可开关、可回收、可预算

可观测性很重要，但 Themis 不接受“为了调试把长期成本写进系统”。

任何新增埋点、统计、profiling、trace，都必须同时回答：

- 默认是否关闭。
- 打开后增加多少 ROM / RAM / 热路径成本。
- 是否可以通过宏、profile 或配置开关彻底裁掉。
- 是否只保留最小字段，能否让调试器直接观察而不是强依赖日志打印。

正式规则：

- 一次性定位问题的逻辑，不得无条件常驻。
- 长期保留的观测点，必须是“值回票价”的正式合同。
- 调试辅助代码也要算资源账，不能逃离资源审查。

### 3.17 每轮都要显式声明资源预算和退出条件

AI 如果只知道“尽量优化”，就很容易把局部优化做成系统复杂度升级。

所以每轮开始前，建议显式声明：

- ROM 预算：允许增长多少，或者必须下降多少。
- RAM 预算：重点盯常驻 RAM、静态对象、栈、堆、队列、FIFO。
- 热路径预算：ISR、DMA completion、轮询循环、日志链是否允许增加额外判断。
- 结构预算：这轮是否允许新增抽象层、公共接口、文档债务。

同时必须写明退出条件：

- 到什么程度算完成。
- 超过什么阈值必须停下回看。
- 哪些收益不足以覆盖复杂度，就不应该继续做。

优秀的 AI 不是“拼命写”，而是知道什么时候该停。

### 3.18 把 reviewer 的问题模板化，而不是临场发挥

高质量代码的稳定来源，不是灵感，而是稳定的复查模板。

Themis 后续建议每轮至少固定检查下面九项：

- 契约是否变了。
- 生命周期是否闭环。
- owner 是否混了。
- 热路径是否变重了。
- ISR / task / init-only / panic path 边界是否写乱了。
- 常驻 RAM 和 ROM 是否有新增。
- 文档是否同步。
- 验证是否真的覆盖这轮风险点。
- diff 是否只包含本轮目标。

如果这套问题每轮都重复出现，就不要继续靠人工口头提醒，而要沉淀到：

- `docs/governance/`
- `AGENTS.md`
- 仓库脚本
- 自定义 agent / skill / hook

规则被机器读到，才会真正稳定执行。

### 3.19 把高频经验沉淀成仓库能力，而不是对话记忆

下面这类高频经验，一旦重复出现三次以上，就不应该只存在某一轮对话里：

- 固定构建命令
- 固定验证命令
- 固定资源统计口径
- 固定 reviewer 检查清单
- 固定 no-touch 边界，例如第三方库、厂商库、图形化生成代码
- 固定 profile / 开关 / 诊断模式

应该优先把它们沉淀为：

- `AGENTS.md` 入口规则
- `docs/governance/` 正式文档
- `scripts/` 自动脚本
- 仓库级 custom instructions
- 项目级 specialized agent / skill

让 AI 稳定写好代码的关键，不是“记住更多”，而是“把正确做法搬进长期可复用入口”。

### 3.20 每轮都要留下接力包

真正长期稳定的 AI 协作，不是某一轮写得多漂亮，而是下一轮能否无痛接力。

所以每轮结束时，必须至少留下：

- 本轮改了什么。
- 本轮没改什么。
- 当前真实基线是什么。
- 还剩哪些已知风险。
- 下一轮应该从哪里接着做。
- 哪些构建、测试、上板、调试命令是这轮已经验证过的。

没有接力包，AI 交接几轮以后，质量几乎一定会下滑。

## 4. Themis 推荐的 AI 施工模板

下面这套模板，建议以后每轮任务都显式提供给 AI：

```text
目标：
- 本轮要解决的核心问题是：

范围：
- 允许修改：
- 不允许修改：

边界：
- 目标层级：
- owner 约束：
- 调用上下文：
- 热路径/ISR：
- 资源预算：

验收：
- 必做验证：
- 必看资源：
- 必同步文档：

回头看：
- 改完先按 reviewer 视角复查
- 收尾前复查 diff / test / docs / size / git status
```

如果任务不是轻量小修，而是路线图审查或架构整改，先按 ADR 的 Architecture Change Gate
（[FalconFOC_BareMetal_Execution_ADR.md](../architecture/FalconFOC_BareMetal_Execution_ADR.md)）
写清方案、验收与回滚，再动手。

## 5. Themis 的“高质量完成”定义

一轮任务只有同时满足下面几点，才算高质量完成：

- 代码正确。
- 契约没漂移。
- 边界没写乱。
- 文档跟上了。
- 验证做了。
- 资源影响说清楚了。
- diff 足够干净。
- 后续 AI 可以无痛接力。

## 6. 必须避免的 10 种低质量行为

- 没读文档就直接改架构。
- 没看已有 diff 就覆盖别人工作。
- 只看功能，不看 owner 和生命周期。
- 只看编译，不看验证。
- 只追求省几百字节，不看整体复杂度和稳定性。
- 把调试逻辑做成常驻成本。
- 把板级事实写进通用层。
- 把一次性 workaround 写成长期契约。
- 只改代码，不补文档和维护日志。
- 没有回头看就直接结束本轮。

## 7. 这份文档放在哪里最合适

这类内容最适合放在：

- `docs/governance/`

原因很简单：

- 它属于工程治理规则，不属于单一模块设计。
- 它同时约束 AI、开发者、reviewer 和接力同学。
- 它应该被 `README`、工程宪章和 `AGENTS.md` 一起引用。

正式结论：

**这份内容应长期放在 `docs/governance/Themis_AI高质量编码必读清单.md`。**

## 8. 外部参考

下面这些来源不直接替 Themis 下结论，但它们提供了强有力的工程共识参考：

- [OpenAI Prompt Engineering Best Practices](https://help.openai.com/en/articles/10032626-prompt-engineering-best-practices-for-chatgpt)
- [OpenAI GPT-5 Codex Prompting Guide](https://cookbook.openai.com/examples/gpt-5-codex_prompting_guide)
- [GitHub Copilot Coding Agent Best Practices](https://docs.github.com/en/enterprise-cloud@latest/copilot/tutorials/coding-agent/get-the-best-results)
- [GitHub Copilot Repository Custom Instructions](https://docs.github.com/en/copilot/how-tos/custom-instructions/add-repository-instructions)
- [Google Engineering Practices: Code Review](https://google.github.io/eng-practices/review/)
- [Google Engineering Practices: The Standard of Code Review](https://google.github.io/eng-practices/review/reviewer/standard.html)
- [Anthropic Claude Code Common Workflows](https://code.claude.com/docs/en/common-workflows)
- [Zephyr Project Coding Guidelines](https://docs.zephyrproject.org/latest/contribute/style/code.html)
- [RT-Thread Contribution / Community Engineering Practices](https://www.rt-thread.io/contribution.html)

引用这些来源的目的，不是照搬流程，而是把它们沉淀成适合 Themis 的长期规则。
