# NEXT-CAL-QUALITY-01｜FalconFOC 校准精度、重复性与产品工作流验收

**NEXT-CAL-QUALITY-01 HOST CALIBRATION PRODUCT QUALITY PASS**

**PHYSICAL CALIBRATION NOT VERIFIED** · **PHYSICAL FLUX MEASUREMENT NOT VERIFIED** · **PHYSICAL TORQUE CALIBRATION NOT VERIFIED** · **TARGET MCU WCET NOT VERIFIED** · **G3-PWR LOCKED**

验证于 2026-11-21。Quick、Full、Selective 通过现有 Host CLI 进入同一个 Calibration Workflow、参数审核与 Runtime 应用链。参数精度根据独立 Oracle、相邻独立会话的冻结预测和新 Runtime Holdout 分别记录。明确保留端电压、速度、时钟、磁轴及扭矩来源的系统偏差限制。

[原始证据索引](2026-11-21-next-cal-quality-01/README.md) · [原始数据 ZIP](2026-11-21-next-cal-quality-01/raw-evidence.zip) · [SHA-256](2026-11-21-next-cal-quality-01/raw-evidence.sha256)

## 1. 基线与验收规范

目录 `D:/study/Software/Hollow-Gimbal-vm01a`，分支 `codex/falconfoc-v2-vm01a`。起始 HEAD `c93655efbce28f6a81fcce2e5c20ac61f45cab76`，索引为空，冻结了 57 个已修改或未跟踪文件的哈希和非 ZIP 内容。BOOT-01 证据包 228 个数据成员逐项复核。本轮开始后用当时源文件复跑 Clang、MinGW GCC 与 Clang ASan/UBSan 各 116/116、Python CLI 11/11、两电机三种 BOOT 路径及负向采集、锁定 ARM 输出。工作期间工作树 HEAD 前移到 `ab0187ef934e9d42559076e4855151dd87ff2600`，该提交包含前序成果和本轮部分中间改动。本轮执行者保留该提交并继续验证实际工作树。 [ARCH-02](2026-11-20-next-cal-arch-02.md) · [FLUX-01](2026-11-20-next-cal-flux-01.md) · [BOOT-01](2026-11-20-next-cal-boot-01.md)

| 评价维度 | 统一判定 |
| --- | --- |
| `NUMERICAL_ACCURACY` | ADC ≤1 count，离散参数准确，粗/细 Offset 的包裹误差分别 ≤0.10/0.002 rad，主要物理量和推导增益误差 ≤3%。增益比较使用相同设计目标下的独立解析参考。 |
| `REPEATABILITY` | 每台模型 6 次独立采集。保存逐次参考、估计、误差、均值、样本标准差、最大偏差、正反向差异、失败和不可辨识比例。 |
| `INDEPENDENT_VALIDATION` | 冻结模型预测相邻新会话的 ADC、双轴 R/L、Flux 和 J/B 数据。不用 Holdout 重拟合。LUT 和闭环各使用另外的运动起点、采样条件或负载。 |
| `ROBUSTNESS` | 在采集边界分别注入系统误差，记录原资格判定、独立 Oracle 偏差和能否检测。 |
| `CALIBRATION_DURATION` | Native 按阶段记录模拟区间、单调墙钟和进程 CPU。CLI 操作往返另计，用户交互与 ETA 未测。 |
| `CONTROL_QUALITY` | 独立新实例完成四模式双向 Holdout、约束与 STOP。V1 历史门槛和失败裁决沿用。 |
| `SAFETY` | 原 Runtime 绑定、输出确认抑制、明确审核、故障撤销、事务性新实例和 FCAL CRC/身份均有运行见证。 |
| `SOURCE_CONFIDENCE` | 新采集按 Host 模型数据标注。FCAL 只提供已经验证身份与 CRC 的数值，跨进程缺失来源另报。 |

## 2. 逐次精度和重复性

两台各执行 6 次，种子 `137, 509, 2017, 9661, 41719, 65353`，起点从 2,000 到 79,300 µs，初始机械角覆盖 −0.9 到 6.25 rad，首段正反向交替。零电流和电气采样的有界确定性 ADC 扰动、端电压 dither 和机械速度噪声位于 Host 观测适配器。去掉时间戳和会话号后，原始观测内容哈希仍逐次不同。既有正式使用路径沿用默认观测；测试没有更改两种标称电机的真值。

表中数值列为 **参考 → 均值，样本 σ，最大绝对误差**。角度按圆周误差统计。连续参数的逐次绝对/相对误差、三相独立统计、分轴 Rs、正反向拟合和个别增益见 `repeatability/repeatability-summary.json`。

| 参数 | Generic | Salient |
| --- | --- | --- |
| ADC A/B/C（count） | 2090/2030/2024 → 2089.906/2029.990/2024.017，三相 σ ≤0.146，峰误差 0.313 | 同一标称 ADC 与同种子下结果一致 |
| 极对数与原始编码器方向 | 6/6 为 4、+1 | 6/6 为 4、−1 |
| 粗 Offset（电角 rad） | 0.370000 → 0.370002，0.03008，0.03011 | 0.520000 → 0.513175，0.02342，0.03528 |
| 最终 Offset（电角 rad） | 0.370000 → 0.370017，0.00005289，0.00008962 | 0.520000 → 0.520003，0.00006999，0.00011011 |
| Rs（Ω） | 0.600000 → 0.599883，0.000358，0.000770 | 0.780000 → 0.779824，0.000641，0.001391 |
| Ld（mH） | 1.200000 → 1.199845，0.000733，0.001356 | 1.700000 → 1.699448，0.001295，0.002670 |
| Lq（mH） | 1.500000 → 1.499744，0.000976，0.001928 | 2.200000 → 2.199214，0.000979，0.002055 |
| Flux（Wb） | 0.025000 → 0.02500638，0.00002639，0.00003903 | 0.032000 → 0.03201282，0.00003071，0.00004621 |
| Ke（相峰值 V/(rad/s)） | 0.100000 → 0.10002551，0.0001056，0.0001561 | 0.128000 → 0.12805127，0.0001229，0.0001849 |
| Kt（N·m/A 峰值 q 电流） | 0.150000 → 0.15003826，0.0001584，0.0002342 | 0.192000 → 0.19207690，0.0001843，0.0002773 |
| J（kg·m²） | 0.002 → 0.002000044，2.66e−8，8.46e−8 | 0.0032 → 0.003200099，5.63e−8，1.66e−7 |
| B（N·m·s/rad） | 0.006 → 0.006000861，3.41e−7，1.43e−6 | 0.009 → 0.009001244，8.56e−7，2.75e−6 |
| Current PI（d/q Kp、Ki） | 最大相对误差 0.129% | 最大相对误差 0.179% |
| 速度、位置、阻抗 K/D 候选 | 速度 PI 最大 0.157%，位置 Kp 3.6 的差约 9.54e−8，K/D 最大 0.0043% | 速度 PI 最大 0.141%，位置 Kp 差约 9.54e−8，K/D 最大 0.0054% |

每次都有四段 256 个 Bootstrap Aperture，外部驱动期间有效 UPDATE 为零。正反向 Flux 最大差分别 2.48e−5 / 5.03e−5 Wb。例行重复实验完成 12/12，失败率和不可辨识率为 0/12。另行运行的身份、运动和参考负向样本没有把错误离散参数发布为候选。位置 Kp 的微差来自 Float32 表示。

## 3. 系统误差和热态模型

电气探针在真实虚拟采集后的独立观测副本上调用原估计器，原始输入和每项注入以标签存档。零电流资格收下范围内稳定的 24-count 偏置并准确观测其偏移，无法单凭这一组数据辨别实际偏移来源。无效 ADC mask、越界偏置被拒绝。

| 扰动 | 原资格与独立影响 |
| --- | --- |
| 电流增益 +7% | Flux 两夹具仍可接纳，各估计偏高约 7%。 |
| 母线重构或端电压 +4% | 对单独改变 Flux 电压的当前激励窗口，均值误差较小而残差上升。若 R/L 和 Flux 共用同一 +4% 标尺，Flux 随之约 +4%，残差仍小。 |
| 恒定端电压偏置 +12 mV、未见证的电压 | 信号偏置破坏正反向一致性，当前政策拒绝。未声明可信电压直接拒绝。 |
| 报告的作用区间错位 +15 µs | 时间相干性拒绝；保留原标签但隐去真实错位的一拍电压滞后可通过，不能用资格验证时间对准。 |
| 独立轴速标尺 +8% | 两夹具均通过，Flux 和推导 Ke/Kt 约 −7.4%。 |
| 共同时间标尺 +10% | 固定速度时 Generic Flux 约 −5.4%，若速度从同一时钟换算则约 +4.2%，测试中均可过门。 |
| R/L 输入失配、EMF 噪声与正反速差 | 当前矩阵 +30% Rs 或 Lq 被拒绝，+30% Ld 可通过且 Flux 有偏差。±60 mV 噪声和反向速度量值 −3% 部分通过。原定向一致性负例继续拒绝。 |
| 编码器和电角参考 | 注入共模 +0.08 rad 相位时，粗 Offset 同样平移 +0.08 rad，最大残差几乎不变。正反相位分别 ±0.09 rad 及仅 16 条/段均拒绝。重跑覆盖反转、非线性、量化、低跟随、有限跨度和候选混叠。 |
| J/B 转矩及机械来源 | 真实虚拟闭环采集上将 Kt×Iq 链的 Kt 放大 15%，J/B 仍被拟合器接受，分别偏 +15.10%/+14.94%，残差约 48.8 µN·m。声明并观测的变化负载通过，误标或遗漏负载、未建模库仑摩擦和静止激励拒绝。另运行原噪声、惯量变化、已知恒定负载和正反响应对照。 |

热模型只对声明的绕组 Rs 加 25%，独立新会话评估冻结冷/热两套模型。Generic 对 0.750 Ω 的新辨识为 0.750119 Ω，Salient 对 0.975 Ω 为 0.974630 Ω。Generic d/q 独立预测 RMS 从 62.06/54.06 mV 降到 1.93/3.06 mV，Salient 从 58.92/49.50 mV 降到 2.91/3.20 mV。热实验未覆盖磁饱和、磁铁或磁链温漂、机械摩擦变化。

## 4. 独立预测与闭环 Holdout

冻结第 n 次结果，用下一次不同会话的测量点按物理量单位计算独立积分预测残差，末次回评第一次。每次验证得到三相 ADC 误差、每轴 96 个 R/L 窗、16 个 Flux 窗和 160 个 J/B 窗。最坏 RMS 如下。

| 验证 | Generic | Salient |
| --- | ---: | ---: |
| ADC（count） | 0.241 | 0.241 |
| R/L d / q（mV） | 7.970 / 9.638 | 11.464 / 15.549 |
| Flux（mV） | 2.151 | 2.759 |
| J/B（mN·m） | 0.273 | 0.462 |
| 独立 LUT 原始 → 补偿后最坏 RMS（机械 rad） | 0.009352 → 0.00006257 | 0.009351 → 0.00006273 |
| Holdout 最大独立电角误差（rad） | 0.001092 | 0.000978 |

Salient 有 1 组 q 轴波形超过探索性 15 mV 线，使用精确模型 Oracle 作用于同一组测量也产生 15.550 mV。该项波形和原始预警随证据保存，正式门槛采用既有 R/L 25 mV，参数精度仍以独立 Oracle 误差评价。

Rotor/LUT Holdout 每次用两个独立 Plant 实例及新起点、正负速度、2,173 µs 采样孔径，共 1,200 点。每个审核候选进入新 Runtime，独立闭环从新起点和外负载时间线捕获，共 96 个方向/模式试验。最坏尾段 RMS 按 Current、Velocity、Position、Impedance 分别为 Generic 0.00552 A、0.01274 rad/s、0.00201 rad、0.00126 rad，Salient 0.00435 A、0.00644 rad/s、0.00242 rad、0.00082 rad。已确认电压峰值 1.488/2.385 V，最大 q 电流请求 1.610/2.039 A，受 2.4 V/2.5 A 策略约束。每个用例核对 Running/UPDATE 与 STOP 后更新计数。

上述收益和误差只适用于实际受验的 Host 输入范围。模型参数接近 Oracle 时仍可能出现瞬态和可达域问题。历史 10 ms Current、±0.03675 N·m 动态限幅、±0.039 N·m 持续超载及 Salient ±2.20 A 大阶跃均保留原裁决。 [Host Control Quality V1](../servo/host-control-quality-v1.md) · [历史裁决](2026-11-20-next-cq-03.md)

## 5. 校准阶段与模式耗时

下表来自实际产品 CLI 的同一组 Debug Host 采集。校准模拟时间求和实际采集区间，包含必要的机械稳定和扫描，控制安全与闭环试验另计。时间单位以列为准。不同进程或重载会重开虚拟时钟，未把绝对时间戳直接相减。墙钟为 Native 阶段合计，进程 CPU 包含用户态和内核态时间。Windows 本次读数常为 15.625 ms 的离散增量，单个短阶段的零值低于可观察分辨率。归档亦含包含启动和 JSON I/O 的 CLI 往返、审核后控制试验及逐阶段原始行。

| 采集流程 | 两模型模拟时间 | 相比 Full 节省 | Native Host 墙钟 Generic / Salient |
| --- | ---: | ---: | ---: |
| Full 预览 | 6.2840 s | 基准 | 34.379 / 39.544 ms |
| Quick 预览 | 0.0096 s | 99.85% | 0.336 / 0.304 ms |
| Selective 首次 `capture` | 6.2840 s | 建历史 | 27.619 / 27.267 ms |
| Selective 同身份 `measure` | 0 s | 100% | 0.042 / 0.040 ms |
| Selective 更换编码器 | 4.6742 s | 25.62% | 11.259 / 12.269 ms |
| Selective 更换负载 | 1.6002 s | 74.54% | 12.834 / 12.832 ms |
| Selective 更换 ADC 链 | 0.1126 s | 98.21% | 3.486 / 3.425 ms |

更换编码器时保留 ADC 和独立等效轴转矩 J/B，重做 Bootstrap、LUT/Offset、分轴 R/L 与 Flux。换负载保留电角和电气记录，只重新采集 J/B 并重算整定。换 ADC 重采零偏、R/L、Flux 并重算增益，保留换相与 J/B。Quick 现场复核 ADC/编码器，跳过运动扫描，其他阶段复用数值。每次组合均再次审核并跑闭环。分开的 Full 预览和审批是两次确定性采集，单次操作为 6.2840 s；同进程 Selective 可保留第一次已采集结果等待审核。

| Full 阶段 | 模拟时间 ms | Host 墙钟 Generic / Salient ms |
| --- | ---: | ---: |
| Bootstrap ADC / 扫描 / 拟合 | 9.6 / 2424 / 0 | 0.080 / 7.577 / 0.654 · 0.100 / 8.580 / 0.669 |
| Workflow ADC / Encoder LUT | 9.6 / 1945.6 | 0.071 / 2.940 · 0.057 / 2.839 |
| Rotor / 最终 Offset | 96 / 96 | 0.278 / 0.194 · 0.272 / 0.150 |
| R/L d / q | 38.6 / 38.6 | 1.221 / 1.090 · 0.894 / 0.927 |
| Flux/Kt / J/B | 25.8 / 1600.2 | 1.145 / 17.590 · 1.046 / 16.262 |
| 整定 / 组合候选 | 0 / 0 | 0.009 / 0.036 · 0.013 / 0.038 |
| 审核 / 新 Runtime 应用 | 0 / 0 | 0.012 / 0.078 · 0.010 / 0.064 |
| 控制安全 / 四模式 Holdout | 13.6 / 12684.8 | 0.390 / 368.140 · 0.407 / 301.404 |
| FCAL 保存 | 0 | 1.184 / 1.134 |

三个最长校准实验区间是开路 Bootstrap 扫描、Encoder LUT 和 J/B，只有开路 Bootstrap 需要本轮的独立外部机械驱动契约。轴速稳定与样本跨度集中在扫描和 LUT。初始参考独立拟合墙钟约 0.65–0.67 ms，完整路径以闭环验证和采集为主要消耗。未削减原采样预算。Full 审核应用总 Native 墙钟 402.711 / 334.882 ms，进程 CPU 296.875 / 343.750 ms，Quick 审核应用分别为 373.113 / 276.964 ms 和 312.500 / 250.000 ms，包含相同标准闭环。Quick FCAL 读取在 `timing/timing-stages.csv` 的 `FCAL_LOAD` 独立列出。取消在 Flux 后时已耗费 4.6838 s 模拟采集，候选尚未发布，重新 Full 需 6.2840 s。Quick 审核错或编码器失效在 9.6 ms 验证内结束，保持历史数值并重做当前检查。模型未覆盖实际人的操作用时，Host 时钟测量也没有推定实际电机校准工时。

## 6. 正式 CLI 工作流与失败恢复

构建 `falconfoc-virtual-calibrate` 后，设置 `$cli` 为 Python 市售入口脚本，`$exe` 为原生 Host runner。三个命令复用先前的审核号和 JSON 行事件。

```powershell
$cli = 'tools/falconfoc-cli/falconfoc_cli.py'
$exe = 'build/virtual-cal/falconfoc-virtual-calibrate.exe'
python -B $cli calibrate --simulator $exe --calibration-mode full --boot-trace full-boot.csv
python -B $cli calibrate --simulator $exe --calibration-mode full --approve-candidate $bootReview --save-config motor.fcal
python -B $cli calibrate --simulator $exe --calibration-mode quick --load-config motor.fcal
python -B $cli calibrate --simulator $exe --calibration-mode quick --load-config motor.fcal --approve-candidate $fcalReview
python -B $cli calibrate --simulator $exe --calibration-mode selective
```

`$bootReview`、`$fcalReview` 分别取对应预览事件的 `review_id`。Full 依次完成开路粗参考、LUT 和细化换相、R/L、Flux/Kt、J/B、整定及组合检查，审核后由原应用层初始化 Host 控制实例并完成标准四模式闭环。Quick 验证当前完整身份匹配的 v1 数值包，复核母线、无电流 ADC 零偏和带既有 LUT 的有效编码器读数。换装或测量资格冲突会拒绝。`selective` 从 `capture` 和 `approve <id>` 获得进程内已审核来源，再使用 `plan encoder`（或 `adc`、`load`、`motor`、`kt`）、`measure` 和新审核号。原始部分来源仍需在同一进程建立历史。跨进程的残缺来源不能作为 Quick 的历史证明。

两台模型均实跑 Full、Quick、首次 Selective、同身份复用、更换编码器、负载和 ADC，以及故障恢复。C 进程先发阶段起始和结束事件，CLI 默认实时输出。带 `--json` 的 Quick 和 Full 输出一次性报告。阶段已完成、已复用、已跳过、剩余和审核状态均可读取，与现有候选和失败诊断配套。ETA 与实际人工交互均为 `null`。 [CLI 操作](../../tools/falconfoc-cli/README.md#host-calibration-product-modes)

| 失败边界 | 实际验证 |
| --- | --- |
| ADC 数据无效、Encoder 丢失 | ADC 观测拒绝发生于采样边界，Quick 在编码器观测失效时即使提供正确审核号也没有应用。 |
| Bootstrap 不可辨识、极对数混叠、LUT Holdout | 四扫正反偏差、跨度、位置/时间原生负例与旧 Workflow LUT Holdout 拒绝均复跑。未发布可用的新组合。 |
| R/L 电压来源、Flux 正反一致性、Kt、J/B | 源不可信、时间错位及方向冲突由原输入合约拒绝，未可信 Kt 和 J/B 未知负载见 Workflow 与传感器源测试。符合识别模型的共同尺度偏差作为来源限制记录。 |
| 错身份、CRC、旧审核号及过期会话 | 产品 CLI 实跑异设备 FCAL、损坏 FCAL 和混用审核号。独立 Workflow/Application 测试覆盖陈旧及跨会话采集、依赖撤销。错误状态有可读原因。 |
| 用户取消、STOP、FAULT、初始化失败 | Full 在 Flux 后取消，未应用及存盘。BOOT 测试在开路采集中中断，有效 UPDATE 为零。Selective 在新审核后注入 FAULT，原应用消息被撤销，恢复后需重审。Control Plane 独立注入新 Runtime 初始化失败，原快照和适用候选保留。 |

已被撤销的复用记录继续保留原诊断信息。复用者根据 `REUSE_PARAMETER` 和 `remaining_stages` 重启受影响采集。输出准入沿用 Runtime 与 Calibration Application。Host 模型精度等级为 `HOST_MODEL_VALIDATED`，共同标尺和失配的适用范围为 `HOST_ROBUSTNESS_LIMITED`，实板需 `PHYSICAL_VALIDATION_REQUIRED`。

## 7. FCAL v1 与跨进程来源

BOOT 后产生的 FCAL 和历史 36 Hz/现行 130 Hz v1 数值通过读取、CRC、身份、重审、配置读回与闭环比对。图式保持 548 字节和既有 CRC32。Quick 以 `PRODUCT_FCAL_SOURCE` 公开 `SOURCE_CONFIDENCE_INSUFFICIENT`，明确数值恢复已发生，采集来源、依赖图、原独立 Holdout 和原审核仍未恢复。新 Selective 进程加载同一 v1 后执行 `plan same`，返回 `HISTORY_REQUIRED`。重载只校验当前设备与数值，不导入数据来源。

后续最小设计为单独、可选的 Host evidence JSON，命名 `motor.fcal-evidence.v1.json`。首版带 `schema_version:1`、FCAL 原始字节 SHA-256、设备/装配/相序/机械帧及采集配置身份、构建版本和取证时间轴。每个参数记录来源类别、原 Session/Capture/Revision、依赖修订、验证范围、质量摘要、训练及 Holdout 原始数据摘要、审核记录。读方检查版本、容量、重复项、身份、有效范围、哈希、缺失依赖和训练/验证捕获是否独立；原有 Safety 与重新审核仍执行。哈希只证明记录与数值绑定，证据持有者身份仍需单独可信来源。缺失、损坏、版本未知、身份/指纹不符或修订失效时应走已有数值验证与必要重采。保留独立 Host 契约测试的 FCAL 数值/CRC/身份、跨进程不自动恢复来源，以及无历史时拒绝复用；待设计实现后增加 sidecar 各失败组合和原输出事务测试。本轮未引入持久化扩展或迁移。

## 8. 回归、构建与复现

最终 Clang、MinGW GCC、Clang ASan/UBSan 各 120/120。每套包含 Core 21、Runtime 13、Calibration 10、Control Plane 5、Sensors 2、Simulation 50、Servo 7、Protocol 1、Board Host 9、示例 1、回放 1。Python CLI 11/11，Legacy Host 45/45，Strict Scope all 通过。Source Contract 检查 200 个 C/H、15 个 CMake 和 15 个 README。

两次正确的 Soft-float `cortex-m4` 锁定构建 Flash 44,452 B、RAM 4,184 B，BIN SHA-256 均为 `78d055e1135d23a5c2d08f6db17fbf64622333f429381efa2632401eb58cbda2`，与 BOOT-01 完全匹配。首次配置缺少显式 CPU 参数，重试采用一个新构建目录，避免复用错误的 cache。原始 ARM 日志保留既有 newlib 和 RWX 链接警告。Host 代码和诊断没有进入锁定输出路径。

历史 Current 原轨迹与反向控制全窗继续 FAIL。CQ-03 的近边界及超载用例沿冻结探针实跑，48 Hz 近边界正反最长限幅 681.5/682.0 ms，超载为 770.5 ms，原 mask 16 延续。Current Startup V1 的 Generic/Salient 冻结 CSV 保持相同哈希，Salient 原退出码 1。FLUX-01 理想参考的兼容通道保留原区间和数值。新 Quick 也实跑恢复两份旧 36 Hz 数值包并分别完成 16 个控制试验。两电机各 512 个区间、16 个闭环结果与 v1 字节匹配。48 Hz Servo 冻结 Holdout 原门槛通过。

```powershell
cmake -S simulation -B build/virtual-cal -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug
cmake --build build/virtual-cal
ctest --test-dir build/virtual-cal --output-on-failure
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_themis_quality_gate.ps1 -RepoRoot $PWD -Scope all -RequireCMakeFormatter -Summary
cmake -S firmware/at32f415 -B build/quality-locked-arm -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/tools/cmake/arm-gcc-toolchain.cmake" -DMCU_CPU=cortex-m4 -DMCU_FLOAT=soft -DMCU_FPU=none -DFALCONFOC_BOARD_CAPTURE=OFF -DFALCONFOC_RT02_TIMING_TRACE=OFF
cmake --build build/quality-locked-arm
```

实物仍需要独立标定的 ADC/端电压/速度和时钟、带失效停机的开路轴驱动、实测扭矩及温度影响、独立轴角和实际板级保护动作与 WCET 测试。本文所有控制、采集、存储和计时证据均来自 Host 或锁定固件离线构建。
