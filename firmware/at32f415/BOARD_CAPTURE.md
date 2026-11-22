# Board1 锁定采集与离线导出

默认构建关闭 `FALCONFOC_BOARD_CAPTURE`。开启后，Board1 可在每次启动中采集一个 8 ms 窗口并冻结 SRAM 镜像。开启采集不授予输出权限。实板检查、SWD 连接、运行镜像部署和测试台供电须各自按现场授权执行。

## 离线准备

1. 在干净的输出目录构建锁定参考固件。沿用 [Board1 ARM 构建](README.md#build) 的工具链和 soft-float 选项，增加 `-DFALCONFOC_BOARD_CAPTURE=ON -DFALCONFOC_BOARD_CAPTURE_BUILD_TAG=0x20261119`。为实际见证轮次分配对应构建标签，保留 ELF、BIN、构建配置和 BIN SHA-256。时序对照需另编 `FALCONFOC_RT02_TIMING_TRACE=ON` 镜像。
2. 用 `arm-none-eabi-nm -S <ELF>` 确认 `falconfoc_capture_ram`、`falconfoc_capture_request`、`falconfoc_capture_cancel`。准备好板号、PCB 修订、编码器、驱动配置和机械参考系的登记记录。设备、马达、编码器和参考系的明确标识未知时填 `null`。
3. 现场见证员先复核已运行的完整镜像身份与部署记录。仅在另行授权允许读取固件区时，可离线比对相同装载区间的读回内容与记录中的镜像。文件摘要和构建标签只说明对应文件与自报身份。由现场仪器独立见证功率级隔离、Gate/Break/PWM 安全电平与电流路径，再允许调试器停核。采集窗口内严禁调试暂停、断点和单步。

## 受控现场读取

在新建的输出目录运行 GDB，载入同一构建的本地 ELF 符号。由现场操作人通过批准的 CMSIS-DAP / SWD 路径连接，使用仓库的 [DAP-Link 配置](../../tools/openocd/openocd_daplink.cfg)。GDB 的 `file` 载入本地符号。步骤中不执行 `load`、Flash program、自动 reset 或寄存器写入。

```gdb
file <exact-local-ELF-path>
source <repo-path>/tools/falconfoc-calibration-replay/board_capture_export.gdb
target extended-remote localhost:3333
monitor halt
falconfoc-capture-status
falconfoc-capture-arm 17
continue
```

为每次独立会话选择唯一的非零 32 位 ID。ARM 在这里指记录器命令，写入的唯一字是专用 SRAM 请求邮箱，与串口控制命令无关。前台接收后预留约 1 ms，再采集 8 ms。8 ms 后前台分片计算 CRC，只有状态 `3 READY` 才导出。让固件连续运行穿过整个窗口及封存时间，再通过 GDB 中断回到停核提示符：

```gdb
falconfoc-capture-status
falconfoc-capture-export
```

`board1.bin` 为 5,480 字节。脚本查验状态、魔数、版本和长度后才取数。如果仍在 `ACTIVE/SEALING`，恢复运行后重查。停核触及预定采集窗口的记录仅作诊断，即使镜像通过结构检查。受控中止可在 `ACTIVE` 时以 `falconfoc-capture-cancel` 将本次 ID 写入专用取消邮箱，再恢复执行直至 `READY`。已封存镜像在本次启动内保持固定。传输中断可在同一次 `READY` 上重新读入新的目录。出现硬件安全前提失效、异常电平、意外电流、连线或固件身份不符、目标重启、丢样或窗口内停核时，停止该轮资格判定，保留原始证据。

## Host 导入

从仓库根运行以下示例。文件参数使用本地路径。先运行 `describe` 读取镜像头、摘要和采集健康摘要。

```powershell
python -B tools/falconfoc-calibration-replay/board_capture.py describe board1.bin
python -B tools/falconfoc-calibration-replay/board_capture.py import board1.bin --metadata board1-source.json --out board1-archive
```

`board1-source.json` 使用以下键。将占位符替换成同一次会话的实际值和登记来源。

```json
{
  "format_version": 1,
  "source": "BOARD_RECORDED",
  "capture_id": 17,
  "raw_sha256": "<describe 输出的原始字节 SHA-256>",
  "build_tag": 539365657,
  "firmware_sha256": null,
  "identity": {"device_id": null, "motor_id": null, "encoder_id": null, "frame_id": null},
  "origin_note": "<导出人、板件登记和采集记录引用>"
}
```

`build_tag` 填 `describe` 返回的十进制值。归档包含原样的 `capture.bin`、`source.json`、`diagnostic.json`，以及所提供的资格材料。诊断报告包含文件 SHA-256、采集头、每条 ADC/Encoder 记录、范围、溢出、漏样和每阶段阻断原因。结构、身份或资格检查失败时返回 2，保留已读取原件和拒绝原因。归档使用新目录。用 `Get-FileHash <file> -Algorithm SHA256` 核对移交的文件与记录中的摘要；`describe` 可再次检查 SRAM 镜像的结构和 CRC。来源字段来自原始生产者类型和清单声明。Host fixture 始终是 Host 数据，Board 声明保持 `PROVENANCE_UNVERIFIED`，独立来源核实沿用 [SOFT-06 包外 Attestation](../../calibration/MEASUREMENT_TRACE.md)。

## ADC_ZERO 资格与 SOFT-06

先由独立仪器按同一采集窗口见证各相零电流、采样窗口和增益。审查其与 TMR2 窗口的关联方式、仪器 ID/型号/校准信息和证据引用。ADC 计数靠近中点、固件自报 INHIBIT 或缺少参考的编码器静态读数都不能赋予测量资格。

仅对满足要求的完整采集添加 `--qualification <witness.json>`。该 JSON 使用 `format_version: 1`、`stage: "ADC_ZERO"`、与来源清单相同的 `capture_id/raw_sha256/firmware_sha256`、与镜像头相同的 `covers_start_us/covers_end_us`、`witness_source: "EXTERNAL_INSTRUMENT"`、经独立确认的 `independent_zero_current: true` 和 `sampling_window_verified: true`、正的 `amp_per_count`、`instrument: {"id": "...", "model": "...", "calibration": "..."}`、`evidence_reference`。真实 ADC_ZERO 资格输入还须填明固件 SHA-256 和四项非零身份。Host fixture 使用自己的 `HOST_VIRTUAL` 前提声明。

检查通过才生成 `trace/manifest.json` 和 `trace/samples.csv`。在 [SOFT-06 Runner](../../tools/falconfoc-calibration-replay/README.md) 构建后运行：

```powershell
python -B tools/falconfoc-calibration-replay/replay.py validate board1-archive/trace --runner <runner> --json
python -B tools/falconfoc-calibration-replay/replay.py explain board1-archive/trace --runner <runner> --json
python -B tools/falconfoc-calibration-replay/replay.py replay board1-archive/trace --runner <runner> --json
```

只选择 ADC_ZERO 的回放能给出当前阶段的偏置和噪声诊断。完整配置候选、应用、持久化另受原有工作流限制。对外部来源使用 `--attestation` 前，可信审查人须核实来源和所用仪器、绑定 `manifest.json` 的摘要，并独立保存见证材料。

<a id="configuration-identity"></a>

## 配置身份合同

V1 布局、128 B 头和 5480 B 镜像不变。头部 `encoder_profile` 字作为配置区分符：原有镜像恒为 1，含义保持不变；校准启动写 2。

| 信息 | 声明方 | 核对方与依据 |
| --- | --- | --- |
| 板级硬件身份：`board_profile=1`、rank/逻辑通道、CPR 65536、原始编码器方向 −1、相序 ABC | 固件 Writer 常量 | `board_capture.py` 用冻结的 Board1 常量核对，不从镜像反读 |
| 配置区分符 `encoder_profile` | Writer，取自启动报告的生效配置（1 标称，2 校准；其他值拒绝启动采集） | 导入器要求 1 或 2，并与外部声明的 profile 一致 |
| 标称（1）：头部方向/极对数 = Board1 原始 −1/7（V1 原义） | Writer | 无声明时按冻结标称 −1/7 核对（V1 旧样本与旧元数据格式 1 照常读入）；声明 NOMINAL 时同样要求冻结值 |
| 校准（2）：头部方向 = Runtime 原始码→机械方向，极对数 = Runtime 极对数 | Writer，取自生效 Runtime/Encoder | 必须由元数据格式 2 的 `configuration` 外部声明提供期望值：`source_direction`（须为 Board −1）、`calibration_direction`、`runtime_direction`（须等于两者乘积）、`pole_pairs`、`coordinate`（只接受 `RAW_NO_LUT`）、非零 `supply_id` 与 `review_id`。头部值须与声明一致 |
| 采集来源 | `producer` 字与元数据 `source` | 两者须一致；Host 夹具标 HOST，`BOARD_RECORDED` 保持 `PROVENANCE_UNVERIFIED` |
| 完整性 | CRC32 | 只证明字节未变；重算 CRC 后的身份篡改仍被身份核对拒绝 |

`supply_id` 与 `review_id` 在声明中只作为引用记录，导入器不把它们与镜像比对，也不据此证明物理来源。电角 Offset 不在头部：当前唯一消费者 ADC_ZERO Trace 不需要它，Trace 清单中的 `raw_direction` 恒取 Board 原始方向 −1，`pole_pairs` 取已核对的头部值。

拒绝原因：无声明的校准镜像为 `CONFIGURATION_REQUIRED`；标称镜像声明为校准、或校准镜像声明为标称为 `CONFIGURATION_MISMATCH`；声明与头部方向或极对数不符、或硬件字段不符为 `RAW_IDENTITY`；坐标非 `RAW_NO_LUT` 为 `COORDINATE_UNSUPPORTED`；声明自身不自洽为 `CONFIGURATION_INVALID`。`board_capture.py describe --configuration` 与 `locked_timing.py --capture-configuration` 使用同一核对。

<a id="adc-zero-candidate"></a>

## ADC 零偏候选（FW-ADC-01，Host）

`tools/falconfoc-calibration-replay/board_adc_candidate.py` 串起既有组件，不新增统计：生产 `board_capture.py` 完成镜像、CRC、身份、配置声明、序列与时间校验并在独立零电流见证下生成 ADC_ZERO Trace（p1..p3 = rank 2/3/4 = 逻辑 A/B/C）；`foc-board-adc-candidate`（`tests/foc_board_adc_candidate.c`，仅 Host）逐行调用 `foc_calibration_adc_collect/finalize`，把合格候选与既有审核 Rotor/Offset 候选组成隔离供应，经 `foc_board_boot_select()` 初始化隔离实例，读回 Runtime Offset、`foc_device_decode_sample()` 解码和 UART，并核对另行启动的正式实例未变。

| 映射 | 依据 |
| --- | --- |
| rank 1 = ADC1_IN1 PA1 = VBUS | `main.c:foc_at32_adc_init`、头部 `ranks=0x06050401` |
| rank 2 = IN4 PA4 = 驱动 IC = 逻辑 A | 同上、`logical_channels=0x00060504`、`foc_device_decode_sample` 的 rank2→a |
| rank 3 = IN5 PA5 = 驱动 IB = 逻辑 B | 同上 |
| rank 4 = IN6 PA6 = 驱动 IA = 逻辑 C | 同上 |

两个电流 rank 在接线层面互换时，镜像中没有独立电流参考，现有证据无法识别该错误（测试保留该事实）。

零电流资格：`zero_current_confirmed` 只由调用方前提给出。Host 生产者加夹具见证得到 `QUALIFIED_MEASUREMENT_TRACE`，候选标为 `HOST_SYNTHETIC`/`HOST_VIRTUAL` 夹具声明；Board 生产者即使附外部仪器见证也只到 `TRACE_PREPARED_PROVENANCE_UNVERIFIED`，编排器报告 `PRECONDITION_UNVERIFIED` 且不生成候选；无见证为 `PRECONDITION_REQUIRED`。软件 INHIBIT 不是绕组零电流证明。候选只代表 ADC 零偏，正式 Runtime Offset 不被替换。

## 时间与物理见证

| 数据 | 合同 |
| --- | --- |
| ADC | TMR1 逻辑槽及其对齐后的起点，Token 的原槽和代际，软件触发请求时间，四个 ADC1 预转换数据寄存器，PCCE ISR 的 TMR2 服务时间代理。Host 保留原始 `VBUS / IC / IB / IA` 顺序。 |
| Encoder | 编码器事务序号，GPIO 写后 CS 代理，实际读 SPI1 接收寄存器后的 RX 服务代理，原始 16-bit 字、关闭与 H1 返回代理。芯片未被生产路径读取的字节保留缺失标记。 |
| Runtime | H0 时间参数沿用 PCCE 服务时间。采样被 Runtime 接纳以 watermark 观测为准，另记录 H0 返回后的 TMR2 代理。所用 Sensor 序号和源时间、会话、命令序号、Safety 状态、闩锁故障、STOP 与适配器确认各自保留。 |
| 同源与回绕 | ADC、SPI、H0 和 H1 的时间字段使用同次启动 TMR2、1 MHz、UInt32 半程排序。DWT 周期只在探针存在时使用。Host 取得文件的时刻不参与配对。 |

现场时序见证应对照同一构建的 Capture OFF/ON，使用合格示波器或逻辑分析仪核查 CS/SCK/RX 对应帧、ADC 采样代理相对物理窗口的误差、TMR1/PCCE/H0 与 TMR5/H1 的响应及最差抢占场景。物理 PWM/Gate/Break 见证仅限锁定状态。探针、CRC 分片和 ISR 写记录均有执行成本，合格期限需要实板证据。

Encoder LUT、Rotor、Offset 仍需独立机械角或电角基准，LUT 另需独立 holdout。R/L 需合格激励及同步实测端电压和电流。J/B 需合格转矩/负载证据。K/D 需合格 J/B 与独立 Kt。实际功率闭环须按 PRODUCT-01 单独授权，并补齐物理安全、采样与 PWM 装载、目标机 WCET 和测量链见证。