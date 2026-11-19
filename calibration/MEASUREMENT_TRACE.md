# FalconFOC Measurement Trace v1

## 标定测量需求矩阵

| 阶段 | 观测、时间与可信前提 | 缺失或拒绝 |
| --- | --- | --- |
| ADC 零偏 | 相位标识明确的原始三相计数，同一次 ADC 有效采样窗口，独立零电流证据。偏置来源和增益由记录仪记录。 | `PRECONDITION_REQUIRED` |
| Motor R/L | 固定 d/q 轴的实际电流端点和前一区间平均端电压。电压覆盖两次电流样本的时间，另有静止、同坐标及区间输出确认。 | `DATA_REQUIRED` 或 `TIME_ALIGNMENT` |
| Rotor、Offset | 原始编码器码和机械坐标、独立电角参考、已知对齐误差。Offset 依赖 Rotor。选择 LUT 时，Rotor 和 Offset 使用已验收 LUT 解码。 | `REFERENCE_REQUIRED` 或 `TIME_ALIGNMENT` |
| LUT 训练和 Holdout | 原始码与独立机械参考共时，记录 CPR、方向和参考零点。Holdout 使用单独 Capture ID，并晚于训练。 | `REFERENCE_REQUIRED`、`TIME_ALIGNMENT` 或原生覆盖拒绝 |
| 机械 J/B | 同一采样点的速度和实测等效转矩，或由实测 dq 电流和独立 Kt 得到的有资格电磁转矩估计。负载需测得或经独立确认可忽略。 | `TORQUE_SOURCE_REQUIRED`、`LOAD_REQUIRED` 或 `DATA_REQUIRED` |
| K/D | 合格 J/B 与另一次采集出具的独立 Kt。 | `DATA_REQUIRED` |

计划分工为可移植 C11 适配器和故障测试、Host 文件完整性与诊断、现有 SOFT-05 合成样本导出与原生回放器。验收比对同一组观测的原生结果，复测时间和来源故障、外部消费者及既有 Runtime 门禁。物理来源真实性、仪表时钟误差和实际采集吞吐量需要另行取得测量证据。


## 数据包

目录包含 UTF-8 的 `manifest.json` 和 ASCII 的 `samples.csv`。V1 由 Host 检查 64 KiB 的 Manifest、16 MiB 的 CSV、最多 100,000 条记录，每条内容最多 512 字节。CSV 使用逗号、十进制数字和可选科学记数法，无引号、转义和空记录，可用 LF 或 CRLF。原文件以只读方式打开。SHA-256 与原始字节匹配后将这些字节交给 C11 Runner，文件顺序和数字对应关系由导入边界保留。

| Manifest 字段 | V1 含义 |
| --- | --- |
| `format_version`、`source`、`host_synthetic`、`session` | 版本 1，四种来源声明，合成标志和非零采集会话 ID。 |
| `identity`、`adc`、`encoder` | 设备、电机、编码器、机械坐标 ID。ADC 满量程、A/count、A/B/C 物理通道；编码器 CPR、采集符号、相序、极对数。 |
| `reference`、`torque_model_id` | 独立参考 ID、参考机械零点和目标坐标方向，转矩模型或校准标识。无此依赖时可以填 0 ID。 |
| `profile`、`stage_mask`、`stage_rows` | 完整或选定阶段。掩码使用 SOFT-05 的 0–6 位，八项数组按下表顺序声明各记录数。 |
| `time`、`units` | 微秒、非零时钟 ID、允许的超龄与配对偏差、回绕许可和 `SAME_CLOCK_PAIR`。单位转换在规范化前完成，V1 读取固定单位。 |
| `recorder`、`instruments`、`preconditions` | 记录工具及版本、仪表 ID/型号/校准记录，以及零电流、输出、电角或机械参考、负载、独立 Kt 前提。实机使用真实来源和校准证据。 |
| `files`、`generator` | CSV SHA-256、记录数和 Host 发生器 seed、采样率及参数。 |

来源使用 `HOST_SYNTHETIC`、`HOST_VIRTUAL`、`EXTERNAL_RECORDED`、`BOARD_RECORDED`。Host 来源为 `PROVENANCE_DECLARED`，外部记录默认 `PROVENANCE_UNVERIFIED`。调用者已核实采集链时，可提供包外的 attestation，包含 `manifest_sha256`、`source`、`identity`、`attested_by`。`PROVENANCE_VERIFIED` 表示验证过这份调用方信任的材料与对应记录的绑定，工具没有连接设备验证真实性。包内 SHA-256 验证的是文件完整性。未核实的外部记录可以得到数值诊断，CLI 不给出可提交的候选。应用资格仍须经 SOFT-05 显式审核和禁能状态下的新实例初始化。

### CSV 字段

```text
stage,capture,sequence,source_us,completion_us,trigger_us,reference_us,clock_id,reference_clock_id,unit,channel,flags,evidence,p1,p2,p3,p4,p5,p6
```

每个 Capture 连续存放，ID 在整个会话唯一，允许在独立阶段重新编号样本。LUT 训练紧接 Holdout，Rotor 与 Offset 等待 Holdout，Offset 还等待 Rotor。使用 dq 电流估计 J/B 转矩时，独立 Kt 行放在 J/B 阶段之前。已输入的 Kt 也可在最后提交给原生 K/D 整定。允许在只选机械阶段时附加 Kt 行作为辨识依据。

| stage | unit | channel | 必需时间和 payload |
| --- | --- | --- | --- |
| 0 ADC | 1 count | `A + 16×B + 256×C`，三个互不相同的 0–15 通道 | `p1,p2,p3` 为 A/B/C 原始整数，`reference_us` 和 `reference_clock_id` 留空。 |
| 1 R/L | 2 A、V、rad/s | 1 d 轴、2 q 轴 | 当前 ADC 采样；`reference_us` 为电压区间末端，`p1,p2,p3,p4` 为实际电流、平均电压、区间起点和电角速度。使用编码器取得帧时填可选 `p5,p6` 为 Encoder 原始采样时间与时钟 ID。静止试验使用独立固定帧时可省略两项。 |
| 2 Rotor、3 Offset | 3 count、rad | 编码器 ID | Encoder 捕获和独立电角基准的时间；`p1,p2,p3` 为原始码、同采集坐标的连续机械角、电角参考。带 LUT 时用已冻结表从原始码解算机械角。 |
| 4 LUT 训练、7 LUT Holdout | 4 count、rad | 编码器 ID | Encoder 与参考仪器的同一采样时刻；`p1,p2` 为原始码和独立机械参考角。 |
| 5 J/B | 5 rad/s、A、N·m | 转矩模型 ID | 速度和转矩两路同一时刻；`p1…p6` 为速度、Iq、Id、等效实测转矩、已知负载转矩和此前区间起点。不参与该来源的电流或转矩值填 0。 |
| 6 Kt | 6 N·m/A | 转矩模型 ID | `p1` 为已资格化 Kt，参考时间及参考时钟留空。 |

未列出的 payload 留空。flags 为位掩码，1 有效、2 区间输出已确认、4 饱和、8 同一帧、16 采样窗口已确认、32 负载已测、64 负载可忽略、128 使用采样时刻代理、256 已限流。evidence 为 0 缺省、1 独立确认零电流、2 实测端电压、3 虚拟端电压、4 独立角度、5 实际 dq 电流模型、6 已测等效轴端转矩、7 独立 Kt、8 控制指令、9 其他派生值。R/L 首个电流端点可用 evidence 0，因为此前没有区间。指令和未采用的估算类型不能替代所需仪表观测。`evidence_mask` 的第 n 位表示该 Capture 出现过 evidence n。

## 时间与完整性

`source_us` 为实采时间或标明的硬件代理。ADC 触发、实际采样、完成时间有各自语义，SPI 片选断言代理和传输完成也分别记录。`trigger_us` 和 `completion_us` 留空表示缺少观测。配对数据的完成时间须同时晚于两路采样或与第二路同一时刻。它用于回放年龄，无记录时回放使用源采样时间，增加 `completion_unobserved` 计数。已观测完成时间在同一采集内允许相等，递减则拒绝。Host 接收或文件导入的时间不参与这些判断。

单路源时间严格递增且小于 UInt32 半程。时钟回绕须经 Manifest 授权，样本序号必须连续且可按 UInt32 回绕。异步通道分别比较自身序列和采样时间。不同采样率本身合法。V1 只配对已共享同一时钟的观测，不做跨域偏移、插值、重采样或外推。LUT 与 J/B 按原生规则要求两个时间相等。Rotor、Offset 和 R/L 的可选 ADC–Encoder 配对可使用声明的有界偏差，标准 Host 策略的上限是 50 µs。并行时钟若需归一，采集方需保留两种原始时间和同步误差证据，先另行验证转换。补签 Host 时间无法弥补物理采样对齐。

LUT 的参考 ID 不能等同于编码器 ID，训练与 Holdout 使用不同且按时间分离的 Capture。Host Runner 拒绝 Holdout 整组与等长训练前缀复用的原始码和参考角组合。仅用数值不能证明两次物理采集独立，采集依据仍需要核实。

## C11 适配与软件应用

链接 `falconfoc-measurement-trace`。调用方以 `foc_measurement_trace_init()` 绑定版本、来源、身份、通道、时间、起始序列和期望记录数。每次 `convert()` 传入一个规范化记录，读取成功输出中的 SOFT-05 样本或 Kt，使用返回的记录时间提交 `foc_cal_workflow_push()`。用 `finish()` 检查捕获完整性，`cancel()` 终止捕获。输出发生错误时保持调用方上次成功结果。并行或重复回放分别持有 Trace 和新 Workflow；在完整性和原生质量同时通过后再发布结果。对上次成功结果的存储采用独立所有权，失败或取消无需恢复旧实例。接口不分配堆、不读写文件。Host Runner 的固定估计器参数是已验证示例策略，调用方可使用原生 SOFT-05 API 为其他电机和采样率明确选择限值。

Runner 报告 Trace 与 Workflow 两级接受/拒绝数、各失败层级、原生质量指标和候选。复用 Config Inspector 所用的 Control Plane 参数表输出稳定 ID 和候选值。软件离线报告没有有效 Runtime 读回，只有审核后才进入 [SOFT-05 的显式应用流程](WORKFLOW.md)。参数存储事务独立。

## Physical Measurement Source Requirements

- ADC 在采集源保留相线与原始 ADC Rank/计数、量程、增益和偏置校准记录、三相相对采样窗和饱和/无效/丢帧标记。锁定 Board1 当前只有软件触发代理，仪表需说明触发到实际采样窗的不确定度，并记录完成时刻。
- SPI 保留原始帧、CPR、位域和有效性、来源 ID、片选断言代理、传输完成、超时、重叠、序号和丢样。角度/转速提供带机械参考、校准等级与相位延时的捕获记录。
- R/L 记录实际端电压的时间区间，以及有效电流、固定坐标/静止依据、区间输出确认。VBUS 是母线观测，PWM 或电流请求是控制量，需要独立实证才能替代真实端电压和电流。
- J/B 记录同轴实测转矩或已测 dq 电流、可信 Kt、磁链条件、滤波延时和实际限流/饱和状态。负载保留来源、方向与单位，独立记录转矩计或可忽略负载的依据。
- 采集工具须带版本、设备、电机、编码器和机械坐标身份，以及仪器校准和与设备时钟同步的记录。以已有许可记录实际输出状态。Board1 锁定镜像的 INHIBIT 软件确认不能证明产生过激励。
- 首个接入适合读出既有采样状态、有界缓冲、明确丢样、帧内时间标签和独立本地文件哈希。UART V1 线格式保留原样。115200 8N1 纯净载荷上限约 11.5 kB/s，缓冲与可持续速率须单独验收。
