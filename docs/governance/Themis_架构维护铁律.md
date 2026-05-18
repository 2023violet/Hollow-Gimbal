# Themis 架构维护铁律

## 架构状态终局判定

当前 Themis 已完成从“能用”到“防呆、诚实、带刺”的跃迁，在单产品语境下已具备代码架构阶段性冻结条件。

本文件是后续 AI 与开发者进入 Themis 仓库前必须先遵守的仓库级治理约束。除非出现新的真实产品故障证据，否则不再以“继续优化”为名对稳定的底层代码发起结构性手术。

## 战果确认

- Host 测试当前为 `79/79 PASS`。
- 统一质量门禁当前为 `all PASS`。
- 已落地并被 Host 见证钉死的 5 个关键 Fail-Fast 机制节点如下：
- `IPC disabled`：disabled 分支不再伪装成初始化成功。
- `Daemon disabled`：disabled 分支不再伪装成 online/healthy。
- `Deferred Save`：非法 owner 或非法 runtime 不再静默吞错。
- `Budget Probe`：注册溢出不再静默忽略。
- `Runtime weak write`：没有 backend 时不再伪造“已写入长度”。
- `COM3` 实板 smoke 已复核 `standard + carrier-demo` 主链路：`ver`、`ui`、`stack`、`param status`、runtime 场景与 UI 手势见证均拿到真实回包。

## 《Themis 架构维护铁律》

1. 禁止在 subsys 中写任何产品策略。
2. 禁止在 disabled 分支返回 OK、healthy 或任何伪成功状态。
3. 禁止绕过 public API 直接访问、拼接或旁路内部 owner 状态。
4. 禁止为了“未来复用”重造抽象层、上移单产品 owner，或把 applications 问题伪装成 framework 问题。
5. 禁止用 Host 绿灯、fake/stub 测试或口头说明替代实板证据与真实链路见证。
6. 禁止假设某个 profile 可以随意裁掉 subsys 核心机制（如 daemon），裁剪必须基于真实的 owner 依赖图，而非主观想象。

## 未来唯一允许的优化方向

- `applications/` 层的产品演进：页面增减、产品流程调整、单产品行为扩写。
- `bsp/` 层的板级适配：引脚、外设实例、板级连线事实的真实变化。
- 证据面补充：在不改动稳定 contract 的前提下，继续补 Host witness、board witness、维护日志与 SSOT 文档。

## 补充复核

- `device/`、`drivers/`、`bsp/` 的内部实现当前处于健康冻结态，不建议继续深挖内部结构。
- `subsys/` 核心机制代码当前处于健康冻结态，不建议再以“更通用”为名重做 public contract。
- `applications/` 现有 task 拓扑当前处于健康冻结态，不建议再为了审美而改 owner 链路。
