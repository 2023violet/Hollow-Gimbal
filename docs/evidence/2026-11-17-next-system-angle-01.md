# FalconFOC V2｜NEXT-SYSTEM-ANGLE-01

日期：2026-11-17。裁决：**PASS — 受控 Host 软件链、误差分账与拒绝行为验收**。没有给出产品总角度限值，故不宣称实机精度或完整控制品质达标。用户在验收后明确要求 GitHub 提交推送；最终 SHA 和远端核对结果单独记录于交付回复及外部 publication-verification.json。

## 1. 基线与调用链

实际工作树 `D:/study/Software/Hollow-Gimbal-vm01a`，分支 `codex/falconfoc-v2-vm01a`，起始本地/远端同为 `f00e9a414d4de2e4d7a82cab47ebbeef6d54179b`，工作树干净。附件中的“Motion 尚待 Git 授权”已过时：上一轮已获明确授权并完成推送，未将其成果混入本轮。QUALITY-GATE-01 的已关闭问题保持关闭。

本轮真实路径及源码位置：

- `simulation/src/foc_virtual_motor.c:291`：Double 运动位置经最近码值量化；反向 Encoder 同时反向编码与解码，未重复施加方向。
- `sensors/src/foc_encoder.c:218`：源时间戳与原始码进入 Encoder；int64 连续计数、单圈整数相位分别维护，速度差分后经 variable-dt 80 Hz LPF。
- `core/src/foc_motion.c:29`：整数周期计数先乘极对/取模再转 Float32；offset 仅施加一次，不从连续 Float32 位置恢复相位。
- `runtime/src/foc_runtime.c:1056`：H1 发布实际 filtered mechanical velocity；同帧电角速度为 `p * mechanical_rad_s`，不是 alpha-beta tracker 输出。
- `runtime/src/foc_runtime.c:315`：H0 从已接收源的 captured_us 到 ADC sample_tick 算模32位年龄，执行恒速预测；执行时刻 now_us 另做新鲜度检查。
- `runtime/src/foc_pipeline.c:406` / `core/src/foc_current.c:223`：预测相位进入真实 Current/Clarke/Park、PI、SVPWM 及 Host 输出提交。dq 与同帧生产预测值的独立 Double 旋转相符，验证该相位实际被 Current 消费。

复用原 `test-foc-periodic-chain` 与 Virtual context/输出适配器，只扩展测试并增加一个 test-only Double helper；未建立第二套控制器或 Runtime。G1 新 Core15/15、Runtime11/11、Simulation15/15。

## 2. Oracle、配置与有条件预算

Oracle 以受控恒加速度轨迹 `theta_m(t)=theta0+omega0*t+alpha*t²/2` 产生真实机械位置；独立最近码值、计数差分/Double LPF、数学周期归约计算参考。生产编码器生成器、Encoder 和整个 Runtime/Current 链照常执行。没有覆盖 H1 的速度或 H0 的角度。

CPR=65536，p=7（另有p=4），80 Hz LPF，offset 为存储的0.37f；假定极对/offset正确，无 DNL/额外传感器噪声。测试初始化采用 Board 的650us/2000rad/s²/0.00065rad预测策略，保持 H0/H1/Foreground=200/500/200us；不等同于整套 Board 标定配置。真实加速度最高1750 electrical rad/s²。系统矩阵实际电速度约在±770rad/s以内；原 Core 名义计算矩阵仍覆盖7000rad/s、全部0..650us。

每例40个捕获、20ms历史预热后，统计600..40000us的198个 RUNNING UPDATE。另有6类拒绝场景；共2587个接受测量行。没有覆盖任意冷启动到高速轴的估计器初态。固定种子 `0x51a72026`，时间戳扰动±20us、ADC抖动±5us、H1/H0执行抖动0..10us；三者独立开关。传输可用时间 `capture+5us` 是**测试假设**，不是模拟或测量的 SPI 完成事件；CSV明确标为 assumed，检查 H1 在其之后执行。

令真实捕获到ADC间隔为T，报告时间戳算出的间隔为Tr，`omega_hat` 为H1实际电速度：

```text
e_phase = wrap_shortest(theta_h0 - theta_true_at_adc)
        = e_quant + e_mapping + (p*omega_raw - omega_true_capture)*T
          + (omega_hat - p*omega_raw)*T
          + omega_hat*(Tr-T) - alpha_e*T²/2 + e_numeric     (mod 2*pi)

|e_phase| <= Q + 1e-5 + E_omega*T
             + (|omega_ref_lpf| + p*0.001)*|Tr-T|
             + |alpha_e|*T²/2 + 2e-6
Q = p*pi/CPR
E_omega = |omega_ref_lpf - omega_true_capture| + p*0.001
```

角度项单位rad，速度rad/s、时间s。Q成立于最近码值量化，p7时为 **0.00033555829735rad**，本轮实际最大量化项 **0.000335280825791rad**。`omega_ref_lpf`来自独立 Double 码值/时间参考；0.001 mechanical rad/s 是生产与参考间的**算术容差**，不是速度估计对真实运动的精度保证。1e-5来自现有单圈映射合同，2e-6沿用 Motion 名义计算合同，未放宽。本轮预测计算误差最大 **3.98386820422e-7rad**。

这个保守三角界以轨迹、码值、时间误差和参考速度偏差为条件；不是通用估计器或产品精度上界。每个样本另检查有符号分账闭合。raw/LPF/时间项相关，不能把不同样本的绝对最大值相加当实测误差，也不能据此给“贡献百分比”。`max_error_rad`仅约束假设加速度的模型贡献，不能保证系统总误差小于0.00065rad。

## 3. 单项与组合实测

每个正常场景198行；单位rad。完整逐行数据、参数和最坏行见 results.json / raw-logs.zip。

| 场景 | 最大相位误差 | RMS相位误差 |
|---|---:|---:|
| static | 0.000248121834 | 0.000248121834 |
| low-speed | 0.000379708519 | 0.000208692312 |
| constant | 0.000403097079 | 0.00020541562 |
| capture-delay | 0.000418697606 | 0.000227113525 |
| accelerate | 0.0027362943 | 0.00160897201 |
| reverse | 0.00277121836 | 0.0016515968 |
| stamp-bias | 0.0143876922 | 0.0140096043 |
| stamp-jitter | 0.01809588 | 0.0098306549 |
| adc-jitter | 0.000418346082 | 0.000227090364 |
| execution-jitter | 0.000418697606 | 0.000227113525 |
| clock-wrap | 0.000422336224 | 0.000226344343 |
| combined | 0.0290408771 | 0.0121325048 |
| virtual-poles | 0.000239168358 | 0.000129763366 |

低速实际电速度估计误差最大0.152217rad/s，其净速度预测项最大6.08864e-5rad；加速时对应4.08241rad/s、0.00222127rad。编码器量化、速度差分与LPF并非可独立相加的噪声源。

准确时间戳、100 mechanical rad/s、捕获延迟150us的同一批帧：若直接用未预测H1角度，ADC时刻最大误差 **0.385334725rad**、RMS **0.263595149rad**；实际预测后分别为 **0.000418698rad**、**0.000227114rad**。年龄包含150..550us的捕获及H1等待时间。只改变执行抖动而保留捕获/ADC时间时，全部相位误差逐样本一致；已知ADC采样抖动也不应直接算成错误的时间戳。

## 4. 最坏组合行与 dq 影响

复现参数：theta0=6.28rad、omega0=100 mechanical rad/s、alpha=250 mechanical rad/s²、p7、CPR65536、capture delay150us、stamp bias10us、三个抖动开关全开、反向Encoder、时钟起点UINT32_MAX-2000、上述固定种子。

最坏行：nominal25400us，真实capture24850、报告stamp24880、ADC25397、H1发布25020、H0执行25432us。模32位传入API，CSV显示相对时间以保留可读的 Double 真值。真实T=547us，报告Tr=517us。

| 量 | 值 |
|---|---:|
| theta_true / theta_h0 | -0.159571409152477 / 6.094573020935059 rad |
| 有符号phase error | **-0.0290408770920507 rad** |
| quant / mapping | -0.000262008106063 / +1.11124123237e-07 rad |
| raw-speed / LPF | -0.0200081492401 / +0.0134349947357 rad |
| timestamp / model / numeric | -0.0219441229248 / -0.000261807875 / +1.0519409166e-7 rad |
| 条件三角界 | 0.0291306527738 rad |
| id_true / iq_true | 0.987295475027 / 0.158895075417 A |
| id_measured / iq_measured | 0.982265353203 / 0.187495976686 A |
| id_error / iq_error | -0.005030121824 / +0.028600901270 A |

ADC输入固定为 `(1,-0.5,-0.5)A`，即alpha=1A、beta=0，排除ADC量化误差干扰本项归因。真实 Current 的dq逐样本与独立 Double 旋转比较，向量误差满足 `2*|I_ab|*abs(sin(e_phase/2))`，容差5e-7A。全矩阵最大Id绝对误差 **0.0233581373120A**、Iq **0.0286009012699A**（不一定同一行），最大dq向量误差 **0.0290398636191A**。这不是转矩损失、机械误差或电机控制品质指标。

另对每个接受周期复制同一pre-step PI状态，保持相电流、目标0/.45A、VBUS、dt完全一致，只替换理想角度作组件对照；实际分支dq与真实H0一致且duty_a逐值一致，CSV另存duty_a差异。强制运动/固定电流测试不建立电机输出反馈闭环，不从该差异推断电机响应。

## 5. 非法数据与旧新鲜源

用户明确澄清第10.6项：**接受旧快照仍新鲜时继续控制，明确记录边界**。未改 Runtime/Safety 策略。

- 传输错误、越界原始码：经过真实 Encoder，H1发布invalid；1000us起 UPDATE保持2，INHIBIT。
- 已接收源过期：最后捕获500us，1200us ADC年龄700us；UPDATE保持3，INHIBIT。
- 未来/重复/过期候选：H1返回ERR_ORDER，不修改原接收槽。1000us时原500us源仍新鲜，允许一次使用它的 UPDATE，计数3；到1200us仍按原源时间过期，计数不再增加。
- 后三例故意携带不同相位/速度，正式比较保留的全部逻辑payload、实际Current dq，并继续到源过期。被拒绝数据未进入控制链，也没有借到达事件刷新源年龄。

没有承诺“任一坏到达立即停机/锁存”。那是另一种策略，不是本轮已澄清的验收语义。

## 6. 与历史 Current 品质的关系

当前源码重新运行原10ms Current CLI，结果与历史CQ/Tuning基线一致。再在**隔离副本**中用真实Plant的ADC时刻理想相位替换角度源，保持默认参数/命令/初态，沿生产Current和Plant重跑。这个理想相位分支是明确的组件反事实，绕过Sensor相位用于归因，不算完整链验收，也不进入工作树生产代码。

| 当前基线 / 理想相位 | 基线 | 理想相位 |
|---|---:|---:|
| 全窗口RMSE A | 0.152791132289 | 0.152790605560 |
| RUNNING RMSE A | 0.089168893772 | 0.089167571490 |
| RUNNING尾段RMSE A | 0.005696916677 | 0.005697114201 |
| 接受UPDATE / 饱和 | 52 / 0 | 52 / 0 |
| 历史标签 | CONTROL_QUALITY_FAIL | CONTROL_QUALITY_FAIL |

理想相位仅改变RUNNING RMSE约1.32e-6A，不能解释此demo的历史FAIL。`tools/falconfoc-cli/falconfoc_cli.py:416` 的quality判据仍使用 **full_errors最后一项和full_settling**；STOP后最后误差约0.18518A，大于0.1A，直接使该判据失败。RUNNING稳态误差约0.00570A、settling4500us与这个全窗口标签分开保留。没有改写历史FAIL，也不能据此认定运行态不达标或PI最优。

理想相位下仍有运行瞬态；单凭本实验无法在PI参数、Plant参数、ADC采样等因素间唯一归因，饱和计数0也不支持把此demo归因为电压饱和。下一步先明确运行态控制品质判据、停止段语义和可执行限值，再决定是否整定；时间戳质量在高速/动态使用中另需预算。没有证据支持先替换预测算法。

## 7. 回归、反例与独立复核

| 本轮正式执行 | 结果 |
|---|---|
| Core Clang / Core ASan+UBSan | 各15/15 |
| Runtime Clang | 11/11 |
| 复核后Simulation Clang / GCC / Clang ASan+UBSan | 各15/15 |
| Source / 修改C/H格式 | 82 C/H、9 CMake、9 README，exit0 / exit0 |
| 严格Scope all -RequireCMakeFormatter -Summary | exit0，30个CMake格式检查，Legacy Host45/45，Total PASS |

保留原15个Simulation CTest，新的组由原periodic-chain入口调用。额外直接导出CSV。所有上表为本轮新执行，未将历史PASS充数；Gate的6个Legacy ELF/MAP是核对SHA的历史副本，不是新ARM构建。

隔离变异均成功编译：H1漏乘极对、把capture替换成H1发布时间，旧periodic-chain均1/1通过，新增组分别因预算/对齐断言失败。独立只读复核发现一项本轮P2测试遗漏：拒绝候选后只比元数据，可能漏掉保留相位污染。已补强；污染旧槽相位的变异在补强前exit0，补强后exit1，正式正常实现GREEN。没有确认生产公式缺陷，新增P2测试问题已关闭。

保留两次纠正记录：初版测试误按500us调用实际200us的Foreground，触发调度断言；首轮Source gate因新helper缺少规范头部段落exit1。分别修正测试事件节拍/文档布局后，受影响完整Simulation和真实Gate重跑通过。未放宽精度容差或修改门禁。

## 8. 差异、剩余限制与停止点

- `simulation/tests/test_foc_periodic_chain.c`
- `simulation/tests/foc_system_angle_oracle.h`
- `simulation/README.md`
- `docs/governance/Themis_工程维护日志.md`
- `docs/evidence/2026-11-17-next-system-angle-01.md`
- `docs/evidence/2026-11-17-next-system-angle-01/results.json`
- `docs/evidence/2026-11-17-next-system-angle-01/change-verification.json`
- `docs/evidence/2026-11-17-next-system-angle-01/raw-logs.zip`

全部生产源码、控制参数、Runtime调度合同、Safety、PWM/Board1权限、UART V1、整数相位来源、Golden和旧evidence未改。原2537文件除本轮三个旧文件外均未变；2427个原ignored产物（322054220B）、起始快照和主工作树保持不变，见 change-verification.json。

剩余：没有真实传感器捕获时间误差/电机标定，没有任意冷启动、全速度/全估计器初态和所有扰动组合证明，没有产品总角度限值。数据结论限定于声明的预热轨迹与假设；精度合同通过不能代替控制品质验收。旧未被live Gate调用的AT32F403A静态检查保持独立跟进，本轮不清理。

可复核：[results.json](2026-11-17-next-system-angle-01/results.json)、[change-verification.json](2026-11-17-next-system-angle-01/change-verification.json)、[raw-logs.zip](2026-11-17-next-system-angle-01/raw-logs.zip)。归档包含逐行CSV、argv/cwd/UTC/exit/日志SHA、独立Oracle/测试版本、隔离变异、理想相位补丁和A/B原始JSON。完整构建保留外部 `D:/study/Software/FalconFOC-system-angle-01-20261117`。

按本阶段第11节先交付结果与差异清单并停止，未提前暂存、commit或push；用户随后明确要求“github上传推送”。据此仅普通提交推送本任务8个文件，重新核对分支与远端身份、检查暂存差异，推送后读回远端HEAD核对。change-verification.json保留授权前的保护快照；最终SHA不写入自身提交内容，另存交付回执。不force、不合并或改写历史，不进入下一轮。保持：

```text
G3-SAFE HARDWARE NOT VERIFIED
G3-PWR LOCKED
AT32F415 WCET NOT VERIFIED
MOTOR PHYSICAL OPERATION UNVERIFIED
```
