# Servo SDK 打包与发布门禁

`0.1.0-rc.1` 是独立的 Servo SDK 内部候选号。根项目和固件继续沿用原版本约定。候选包含 portable Core 和 Servo Host 两层，具体文件、公开头文件和来源状态由 `scripts/sdk/source-map.json` 声明，打包时产生逐文件的 `SDK-SOURCE-MANIFEST.json`。

## 生成候选和检查

在源码工作树使用 Python 3.10+、Git、CMake 3.25+、Ninja、Clang 和 GCC。以下 PowerShell 命令使用两个尚未存在的输出目录。通过 `--clang` 和 `--gcc` 可指定编译器路径。打包日期应为这份源码快照的日期。

```powershell
$candidate = Join-Path ([IO.Path]::GetTempPath()) "falconfoc-candidate"
$verification = Join-Path ([IO.Path]::GetTempPath()) "falconfoc-sdk-verification"
python -B scripts/sdk/build_sdk_package.py --output "$candidate" --snapshot-date 2026-11-18
$archive = Join-Path "$candidate" "falconfoc-servo-sdk-0.1.0-rc.1.zip"
python -B scripts/sdk/check_sdk_package.py "$archive" --work "$verification"
```

脚本记录 `sdk-gate.json` 和各工具链的命令、退出码、时间及输出。退出码 **0** 表示四道发布门禁均通过，**1** 表示检查失败，**2** 表示技术通过但发行受阻。内部开发任务可指定 `--technical-only`，此时技术通过返回 0，JSON 仍记录所有门禁和实际发布状态。

SDK 消费者开启 `FALCONFOC_SDK_CORE_ONLY` 时只构建 `falconfoc-core`。Servo Host 公开提供 `falconfoc-virtual-motor`、`falconfoc-pipeline`、`falconfoc-encoder` 和可选的 `falconfoc-calibration`。根 SDK 工程默认构建 Servo 示例与 Host 验收用例。每个包的顶层 README 有独立 CMake 命令，应用集成与参数说明见 [Servo Quickstart](quickstart.md)。

## 复现与来源

包内 `SDK-METADATA.json` 记录版本、源码提交、工作树状态、快照日期与 Manifest 哈希。归档旁的 `.sha256` 校验 ZIP，`SDK-SOURCE-CHANGES.patch` 记录入包文件相对 HEAD 的差异。Manifest 为每个文件记录来源路径、包路径、首次纳入的 Git 提交（如果已提交）、状态和 SHA-256。

未变动的跟踪文件使用 Git blob 的规范字节，改动及新增文件使用当前源码快照。可复现的输入是这些文件、显式清单、版本、快照日期和 Git 身份。ZIP 固定路径顺序、时间、权限和无压缩存储。dirty 候选与其归档哈希共同标识真实输入。仅检出对应 HEAD 无法取得尚未提交的包内文件。

验证器只将经过成员、哈希和清单检查的文件写入独立目录。随后对同一包依次运行 Clang、MinGW GCC 和 Clang ASan/UBSan 的 SDK、Core-only 消费者、Servo 消费者，核对构建单元及项目头文件来自解压目录。包根 SHA-256 与配套文件应保存在可信内部位置。

## 发布门禁

| 门禁 | 通过条件 |
| --- | --- |
| TECHNICAL_GATE | 包内容、哈希和版本一致，干净解包后的构建、测试、Demo 和外部调用通过 |
| PROVENANCE_GATE | 文件来源逐项核实，并有该候选的权利链记录 |
| LICENSE_GATE | 包内文件都获适当再分发授权，项目许可和适用的版权、许可、Notice 已核对 |
| APPROVAL_GATE | 可复现的干净提交与包一致，项目所有者明确批准该候选公开发布 |

只有四项同时通过，脚本才输出 `RELEASE_READY`。权利审查和批准由所有者完成，验证器读取经确认的记录。以后可通过 `--release-review` 指定由项目所有者出具的 JSON，字段为 `sdk_version`、`source_git_commit`、`archive_sha256`、`provenance_record`、`license_notice_record` 和 `owner_release_approval`。后三项引用经核对的批准记录。文件级源清单仍须反映已确认来源与权利。

本轮许可和批准依据不足，状态为 **REDISTRIBUTION BLOCKED — OWNER APPROVAL REQUIRED**。本地内部候选的许可建议及阻断责任见 [验收报告](../evidence/2026-11-18-next-servo-03b.md)。
