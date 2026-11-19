# FalconFOC 只读配置诊断

## 数据来源与适用性

[公开 C11 接口](../../control_plane/include/foc_config_diagnostics.h) 由可选 `falconfoc-config-diagnostics` 提供。调用方持有输入和缓冲区，在 Foreground 串行执行检查，不与存储写入或 Runtime 初始化重叠。双槽扫描仅调用只读回调。Runtime 采集通过一次正式快照读取及同一实例不可变的配置完成。

| 来源 | 可得结果 |
| --- | --- |
| `HOST_STORAGE_IMAGE` | Host 模拟的 A/B 两槽各 256 B。能检查格式、提交、CRC、TLV、恢复决定和可信策略下的适用性。 |
| `HOST_RUNTIME` | 实际 Host Runtime 实例的一致快照和有效静态参数。可以检查候选和有效配置是否相符。 |
| `DEFAULT_CONFIG` / `PENDING_CONFIG` | 调用者提供的默认或待初始化配置，可用于字段对照。 |
| AT32F415 参考固件 | 通过原 UART V1 可查询其既有状态、故障、固定参数和遥测。Board1 尚未接入双槽 Flash 配置后端。 |

记录完整性要求已提交的正确外层头、CRC 和结构允许检查的 TLV。Schema、身份及参数组合决定应用资格。已恢复候选仍需应用方显式初始化，之后才能观测实际有效值。CRC 可检出损坏，无法证明来源或认证写入者。

## 双槽诊断和原因

`foc_config_inspect_slot()` 使用 Config Store 的公共编解码契约。`foc_config_inspect_pair()` 的选择来自 `foc_config_store_select()`，与正式加载共享同一实现。每槽报告自己的诊断，即使另一槽被选中。选中的槽是正式选择的 envelope，之后可能仍被 Schema 或身份拒绝。双槽未决定时不返回选中的槽。主要选择结果包括 `SELECTED`、`OLDER`、`EQUIVALENT`、`INELIGIBLE` 和 `UNDECIDED`。

检查用三态：通过、失败、未检查或不可判断。带可识别 Magic 的头可展示格式和长度，只有 Format 1 才按本项目当前契约解释 Schema、Sequence、数量和身份。CRC 失败时，这些原始值只能辅助调查。CRC 有效的未来 Format 无法提供当前格式特有字段。空槽的 Magic、CRC、Schema 和身份无观察值，损坏和未提交有各自原因。

| C11 诊断原因 | 含义 |
| --- | --- |
| `RECORD_NOT_FOUND` | 槽全为擦除值。 |
| `BAD_MAGIC` / `BAD_ENVELOPE` / `BAD_CRC` | 提交记录的 Magic、长度/数量或完整性不符合契约。 |
| `INCOMPLETE_COMMIT` | 提交字仍擦除或未完整编程。若记录已完整提交且保存确认丢失，正式扫描仍可选择它。 |
| `UNSUPPORTED_FORMAT` / `UNSUPPORTED_SCHEMA` | CRC 合法的未知格式，或此格式下不兼容的 Schema/参数集合。出现冲突时遵从正式恢复优先级。 |
| `BAD_TLV` / `DUPLICATE_PARAMETER` | TLV 长度、类型或标识重复。`tlv_reason` 可与更优先的恢复拒绝同时查看。 |
| `INVALID_PARAMETER` / `INVALID_CONFIGURATION` / `MISSING_PARAMETER` | 参数表示、单字段范围、组合规则或必需字段未通过。运动保护组继续使用 SOFT-03 完整约束。 |
| `IDENTITY_UNKNOWN` / `IDENTITY_MISMATCH` | 缺少可信设置，或存储身份与可信字段不符。位掩码可定位到 CPR、方向、极对数、机械坐标、编码器、转子零点、电机、IMPEDANCE 配置和 Kt。 |
| `MIGRATION_REQUIRED` | Schema 1 需要单独确认 SOFT-02 保护组。诊断可验证显式提供的迁移候选，不会保存。 |
| `IO` | 只读回调失败，不能判定最新状态。 |

序号回绕、相等以及半范围歧义与 [SOFT-03 存储规则](config-store.md) 一致。格式有效但有未知 Schema、语义或身份拒绝时，加载策略拒绝将旧配置当作当前配置。选择歧义、两个空槽和两个无有效记录是不同的顶层结果。

## 参数 Diff 和 Runtime

`foc_config_diff()` 逐项读取 Control Plane 的静态配置描述，返回 ID、名称、类型、单位、左右值、比较类别、保护组标记和重新初始化标记。当前共有 19 项，其中 9 项属运动保护。Float32 使用精确 IEEE 754 位比较，两个符号零可区分。缺失、无效或不可信配置的一侧没有有效值。独立候选、默认配置、两个槽和捕获的 Runtime 都可作为左右输入。

`foc_runtime_diagnose()` 采集单份一致快照。若 Runtime 正在发布导致 `FOC_RUNTIME_ERR_BUSY`，调用方输出保持原值，应报告快照不可用。成功结果包含原 Session、Fault、Safety、更新数、输出请求及确认信息。`foc_config_application_status()` 可报告未观测、需要重新初始化、有效值匹配或候选不可用。`MATCHES_EFFECTIVE` 表示本次可比较的静态参数值一致，初始化来源仍由应用方确认。Host 的确认范围限于软件适配器。

[外部 C11 示例](../../examples/config-diagnostics/README.md) 分别观测旧 Runtime 和合法初始化后的新 Runtime。可看到已保存的保护开启配置仍与旧 Runtime 的关闭配置不同，以及新实例相符、确认 INHIBIT、零次 UPDATE。

## CLI 与 JSON 版本

[离线工具运行手册](../../tools/falconfoc-config-inspector/README.md) 提供 `inspect`、`diff`、`explain` 的命令和生成测试镜像的方法。输入是恰好 512 B 的普通文件，按 A、B 排列。提供明确的 Host 测试配置 `host-fixture-v1` 才能做 CLI 适用性判断。未提供 profile 时可独立查看记录及正式 envelope 选择，可信适用性保持未知。其他 Host 配置可经 C11 API 提供实际可信策略。

JSON `schema_version: 1` 独立于存储 Format 和 Schema。`inspect` 返回 `source`、`profile`、`recovery`、`slots` 与 `runtime_applied`。每槽 `integrity_valid`、`applicable`、原因、选择和原始字段独立。`null` 指数据未读取、校验未执行或无法判断。离线输入始终没有 Runtime 应用证明，所以 `runtime_applied` 为 `null`。`diff` 返回左右来源和 `fields`，每项包括 `parameter_id`、`type`、`unit`、`left`、`right`、可用的 Float32 位模式和保护/重新初始化标记。`explain` 输出固定原因及解释，文本说明只存在于 Host 工具。
