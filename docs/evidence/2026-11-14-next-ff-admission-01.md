# FalconFOC V2｜NEXT-FF-ADMISSION-01 Feedforward 软件准入契约

日期：2026-11-14  
分支：`codex/falconfoc-v2-rc0`  
HEAD：`46bc963f5e5849e4440d1714a845fe485d338837`

## 最终判定

`NEXT-FF-ADMISSION-01 PASS（Host 软件契约范围）`

本轮完成了现有 CAL-01、CONFIG-01、Sensors、Runtime 和 Feedforward 能力审计，并新增 Host-only admission contract 测试。测试明确区分：参数数值合法、参数来源/电机身份可信、电气速度质量可信，以及是否允许申请 Feedforward。

本轮没有修改生产控制方程、Runtime 状态事务、Safety FSM、Board 默认配置或硬件输出路径。`Board1 Feedforward` 仍未启用，真实电机参数仍未验证。

## Existing Capability / Missing Contract / Required Evidence

| Existing capability | 当前状态 | 缺失契约/证据 |
| --- | --- | --- |
| CAL-01 ADC 零点 | 已有离线候选、Welford 质量和失败状态 | 真实零电流前提、真实 ADC 测量 |
| CAL-01 转子方向/极对数/offset | 已有带参考电角度的离线候选和 residual | 实际电机辨识、安装方向和电角度对齐 |
| CONFIG-01 snapshot | 已有质量、范围、identity、坐标一致性和原子装配 | Rs/Ld/Lq/ψf 的来源、身份和版本元数据 |
| `foc_current_feedforward_init` | 检查 Ld/Lq/ψf 数值有限性和正值约束 | 不知道来自哪台电机、哪个校准版本、何时测得 |
| Runtime Feedforward | 配置启用时检查 H1 电气速度 valid、source time、age | 没有参数 provenance 字段；旧参数身份不能由当前 API 表达 |
| H1→H0 速度快照 | 有 seq、source time、session、年龄、future/old 拒绝 | 没有把 calibration version / motor identity 与速度快照绑定 |
| Feedforward 限幅 | 仍经过 Current PI、SVPWM、voltage limit、anti-windup、Safety | 无新的绕过路径；Host 已验证这一点 |
| Board1 默认 | 前馈和轨迹默认关闭，输出锁保持 | 真实电机参数、真实速度质量和硬件安全证据均缺失 |

现有代码已经能防止很多“数值错误”和“旧速度快照”问题，但不能把数值合法性升级为真实参数可信度。本轮通过 Host-only 契约把这项差距显式固定下来，没有假装已完成生产接入。

## 参数单位和数学契约

| 字段 | 单位/约定 | 当前来源状态 |
| --- | --- | --- |
| Rs | Ω，`>0` | 当前 Feedforward API 不使用；Host provenance contract 只验证数值 |
| Ld/Lq | H，均 `>0` | Host 模型/测试值；真实辨识未完成 |
| ψf | Wb，按现有公式要求 `>=0` | Host 模型/测试值；真实磁链未辨识 |
| pole pairs | 正整数 | CAL-01/CONFIG-01 候选可携带；实际电机未确认 |
| electrical speed | rad/s，正负方向遵循 Rotor/Park 约定 | H1 电气快照，具有 source time/seq/session |
| electrical angle | rad | H1 Rotor 映射和 H0 对齐 |
| current | A | Device decode 后进入 Current Core |
| voltage | V | Current PI、Feedforward、SVPWM 共用 |

当前生产前馈公式为：

\[
v_{d,ff}=-\omega_e L_q i_q
\]

\[
v_{q,ff}=\omega_e(L_d i_d+\psi_f)
\]

Feedforward 结果仍进入原有 Current PI 请求、向量限幅、SVPWM、实际电压重建和 anti-windup 提交路径。

## 四种准入状态

### A. 数值合法

Host admission contract 拒绝 NaN/Inf、非正 Rs/Ld/Lq、非法极对数和不能表示的参数组。这只代表数学输入合法，不能代表参数匹配实际电机。

### B. 参数来源可信

Host contract 要求 motor identity、calibration version 和 source kind 匹配。生产策略只接受 `IDENTIFIED`；`MODEL_ASSUMED` 只能由明确的 Host 研究策略允许。不允许两个电机的参数字段混合使用。

当前生产结构没有 provenance 字段，因此这一检查目前是 Host-only 契约，不会自动改变 Runtime。

### C. 电气速度可信

Host contract 和已有 Runtime 测试共同覆盖首次速度未建立、NaN/invalid、过期、future timestamp、旧 seq、旧 motion session、旧 calibration version、正负速度和 uint32 timestamp 回绕。

现有 `test_foc_control01b` 已覆盖 H1→H0 的合法延迟、过期/未来样本、回绕和 slot 交错；本轮 admission harness 额外验证了“速度数值合法但属于旧身份/旧会话时仍然拒绝”。

### D. Feedforward 使用准入

只有 Feedforward 显式 enabled、参数数值合法、参数来源和 motor identity 可信、calibration version 匹配、电气速度 valid/fresh/seq 新/session 新且 source timestamp 在窗口内时，Host contract 才返回 OK。

这个 Host OK 不等于功率级允许输出，也不等于真实电机已校准。

## Host admission matrix

新增 [test_foc_ff_admission.c](D:/study/Software/Hollow-Gimbal/runtime/tests/test_foc_ff_admission.c)，注册为 `test-foc-ff-admission`。覆盖：identified 参数 + fresh speed、model assumed 策略分离、零电感、非有限磁链、motor identity mismatch、calibration version mismatch、首次/过期/旧 session/旧序列、timestamp wrap、正负速度、生产 Feedforward 方程和显式 enable 边界。

测试输出：

```text
test-foc-ff-admission passed matrix=7 numeric=1 provenance=1 speed=1 wrap=1 feedforward_equation=1 production_wiring=host_only
```

## 失效与状态连续性

已有 Runtime 处理：Feedforward 配置非法时 Pipeline 初始化失败；H1 电气速度过期/无效时 H0 返回 feedback error 并进入原有 INHIBIT 路径；future/old source 不被消费；UPDATE 被拒绝时 Pipeline 保持试算事务语义；Feedforward OFF 使用原有 PI-only 路径。

本轮没有新增前馈斜坡、模式状态机或自动重启逻辑。Feedforward ON/OFF 仍经过原有限幅和 anti-windup，不存在绕过 SVPWM/Safety 的新路径。

## 当前能力是否足够

1. CAL-01 当前覆盖 ADC 零点、转子方向、极对数和电角度 offset 候选，不包含真实 PMSM Rs/Ld/Lq/ψf 辨识。
2. CONFIG-01 对校准几何候选有 identity/quality；对 PMSM 参数 provenance 尚不足。
3. Runtime 已检查数值和速度新鲜度，但参数来源可信目前没有生产字段。
4. H1→H0 对 source time、seq、session、年龄和未来样本已有约束，仍缺 calibration version/motor identity 绑定。
5. 本轮不需要修改生产代码；现有 Host/Runtime 能力足以保持 Feedforward 默认关闭并拒绝明显无效输入。
6. 真实参数必须等待电机身份、Rs/Ld/Lq/ψf 测量来源、适用条件、极对数、编码器方向、电角度 offset 和真实 H1 电气速度质量。

下一阶段若要生产接入，最小方案是：在校准快照中携带同一电机 identity/version 的 PMSM provenance，再由 H1 electrical snapshot 携带匹配 calibration version，最后在现有 Feedforward gate 前执行一次性准入。这个方案需要单独设计和验收，本轮不实现。

## 回归

- Core：`14/14 PASS`。
- Sensors：`2/2 PASS`。
- Calibration：`1/1 PASS`。
- Runtime/Pipeline：`10/10 PASS`，包括 Robustness 和 FF admission。
- G3-NP：`2/2 PASS`。
- G3-SAFE：`1/1 PASS`。
- Protocol：`1/1 PASS`。
- Board/UART/Python：`4/4 PASS`。
- Clang ASan/UBSan：Core `14/14`、Sensors `2/2`、Calibration `1/1`、G3-NP `2/2`、G3-SAFE `1/1`、Runtime `10/10`、Protocol `1/1`、Board `4/4`，合计 `35/35 PASS`。
- Host Debug：`45/45 PASS`。
- SYSTEM replay：`3f01f6e3`。
- Long replay：`49dea950`。
- Source contract：`68 C/H、8 CMake、8 README PASS`。
- CMake formatter：`29/29 PASS`。
- `Scope falconfoc`、`Scope all`、`git diff --check`：PASS。
- AT32F415 ARM 构建：PASS；新增 Host admission 未链接进生产 ARM 固件，资源无解释变化。

以下仍不能声明：`PHYSICAL MOTOR PARAMETERS VERIFIED`、`BOARD1 FEEDFORWARD ENABLED`、真实 Rs/Ld/Lq/ψf 已辨识、真实编码器方向/极对数/electrical offset 已确认，或真实 H1/WCET 已验证。

继续保持：`G3-SAFE HARDWARE NOT VERIFIED`、`G3-PWR LOCKED`、`AT32F415 WCET NOT VERIFIED`、`MOTOR PHYSICAL OPERATION UNVERIFIED`。

