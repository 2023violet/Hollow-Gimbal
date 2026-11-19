# NEXT-RELEASE-02A｜SDK 来源取证与所有者决策

**裁决：PROVENANCE REVIEW COMPLETE — UNRESOLVED FILES REMAIN**

**执行日期：2026-11-18**

## 项目所有者先决定的事项

1. **确认实际版权持有人和许可权限。** 优先请 Legacy 署名人和 16 个文件的首入库提交者核对创作过程、复用段落、其他作者及雇佣或合同权利。确认人如有权代表其他权利主体，应提供对应依据。
2. **确认发行范围及自有部分许可。** 当前完整 Servo 范围依赖全部 16 个疑点文件。只有实际权利证据支持的部分才进入公开清单。建议优先审查 Apache-2.0，给下游明确的贡献相关专利授权。MIT 仍可作为较简洁的许可选择。 [许可决策备忘录](../release/2026-11-18-sdk-0.1.0-rc.1/license-decision-memo.md)
3. **在范围和声明获批后批准精确版本。** 批准对象应含 SDK 版本、干净 Git Commit、最终 Manifest 和归档 SHA-256。公开源码、公开 ZIP、Tag、GitHub Release 和外部提供 SDK 分项签署。 [未签署审批表](../release/2026-11-18-sdk-0.1.0-rc.1/owner-approval-template.md)

## 档案身份与证据等级

分支 `codex/falconfoc-v2-vm01a`，HEAD `a07d7db6b3f2d439255724df05e77e343ae6a731`。未暂存任何路径。候选由 43 个跟踪文件及 9 个工作树快照文件组成。归档恰有 54 个成员，包含 Manifest 和 SDK 元数据。已核对所有 52 个清单文件的路径和 SHA-256，43 个跟踪文件匹配 HEAD blob，9 个未跟踪文件匹配实际工作树字节。

| 原内部候选 `0.1.0-rc.1` | 复核值 |
| --- | --- |
| ZIP SHA-256 | `4293dcd599859226ea52248d23262c134cffbf7d74fd09bd2a258aad8b8c5694` |
| Manifest SHA-256 | `34f3cfe5e54bf6daab9d182fac01b2a1fae594bba33d77a3ab852bfc5ca5f0c4` |
| 检查结果 | 原包路径及字节、内外 Manifest、原元数据和源码差异保持原验收身份。 |

本轮矩阵采用两个判断面，技术来源和合法公开分发分别记录。E0 表示仅知文件存在，E1 表示有可查线索，E2 表示能够核查来源关系或项目开发记录，E3 表示适用许可或权利基础得到证明，E4 表示完整发行授权和第三方义务已确认。

| 工程来源归类 | 文件数 | 本轮结论 |
| --- | ---: | --- |
| `UNRESOLVED` / E1 | 16 | 首入库和变更已查清，入库前来源及可再许可权不足。原疑点 **0/16 关闭**。 |
| `PROJECT_ORIGINAL` / E2 | 36 | 阶段 Git 和证据支持项目内实现或集成的工程归类。实际持有人与公开再分发依然待确认。 |
| 证实属于入包第三方授权、外部衍生或供应商源码 | 0 | 在本轮有界核查中未识别。参考项目和仓库其他目录中的第三方许可见 Notice 草案。 |
| E3/E4 和已获公开再分发授权 | 0 | 没有适用于这 52 个文件的已确认授权。 |

[逐文件来源、版权及许可矩阵](../release/2026-11-18-sdk-0.1.0-rc.1/source-provenance.tsv)

## 16 个早期文件与其余来源

16 个早期文件首次见于 `46bc963f5e5849e4440d1714a845fe485d338837`。首版 16 个完整文件、Git blob 哈希、可达历史中出现该 blob 的提交、后续路径历史和 HEAD 作者行统计已保存。Git 作者 `2023violet` 提供联系线索。父树和本地可达历史未发现更早的相同首版 blob。Legacy 源码中的 Zhang Jiaming 版权声明及历史作者记录提供了额外核对线索，无权利证明可以据此自动覆盖现有 SDK。

G2 与 G3-NP 的项目记录支持已知 Legacy 物理符号和 Board1 ADC/PWM 事实的参考关系。本地 48 个相关历史比较文件主要命中 C/C++ 头部封装和通用三相字段。固定版本的 VESC、ODrive、SimpleFOC、robot_mc 各比较两个相关实现，未有达到三行阈值的连续实现匹配。独立复核扩大 VESC 到同一版本的六个文件。两次筛查的覆盖范围和阈值不同，均保留早期文件的历史改编不确定性。 [逐项调查](../release/2026-11-18-sdk-0.1.0-rc.1/early-files.md) · [固定上游与许可材料](../release/2026-11-18-sdk-0.1.0-rc.1/upstream-and-notice-review.md)

| 引入阶段 | 文件数 | 清单来源 |
| --- | ---: | --- |
| `46bc963` | 16 | Core 基础、Runtime/Pipeline、G3 Device 与 Safety |
| `278c691` | 7 | Core Trajectory/内部头、Encoder、基础 Calibration |
| `02425e0` | 4 | Virtual Motor、Virtual Current |
| `af8631b` | 2 | Motor-ID |
| `e6bd7f4` | 2 | Encoder Calibration |
| `a07d7db` | 12 | Impedance、Tuning、Feasibility、Feedforward、Mechanical-ID 及两项测试 |
| 原 03A、03B 工作树 | 9 | 4 个 Servo 示例与 Quickstart 文件、5 个包内入口与文档 |

## 最小补证与范围处理

| 路线 | 对当前清单的建议 |
| --- | --- |
| 取得补充权利依据 | 优先处理 16 个阻断文件。由首入库提交者提供对应路径的原始设计、未入 Git 草案、复用说明及实际作者确认。对另 36 个文件，由项目维护者和真实权利人补齐合同、贡献和许可范围。 |
| 保留上游条件 | 发现具体的复制或改编后，锁定上游版本、段落、实际权利人和适用许可证。兼容时保留原版权、许可、适用 Notice 和修改声明。疑似 GPL 派生或供应商受限材料进入 `LEGAL/PROVENANCE REVIEW REQUIRED`，确认发行方式或另获授权。 |
| 从候选范围排除 | 16 个文件都在现有 CMake 源列表或直接包含链上。8 个 C 源为明确的编译单元，8 个头有直接消费者。当前没有可直接移除又保持完整 Core、Runtime、Demo 和外部消费者的疑点文件。Core-only 可以不交付 10 个 Host 早期文件，但仍依赖 6 个 Core 文件，且形成的候选范围会改变，需单独批准和复验。 |
| 独立再实现 | 权利基础确实无法取得且功能必要时，先由独立规格方抽取公开数学关系、接口、单位、时序与错误契约，留下可审核输入来源，单独委派实现与审查。Core 以已公开算法规格和既有独立向量、极值/时序回归验收，Device/Safety/Pipeline/Runtime 以同源、拒绝、STOP、Fault 与软件输出事务验收，并重建候选跑消费者。 |

被排除的既有第三方源码保留各自原版权和条款。当前未发现随包的明确第三方源码，公开候选的正式版权与 NOTICE 仍需依据实际权利和路径审定。草案分别列出第三方组件、版权候选、许可证和 Notice/修改义务。 [版权与 NOTICE 审查草案](../release/2026-11-18-sdk-0.1.0-rc.1/upstream-and-notice-review.md)

## 技术范围、独立复核与门禁

- 已复核 03B 原始记录对原 ZIP 和 Manifest 哈希的绑定。Clang 22.1.8、MinGW GCC 16.2.0 及 Clang ASan/UBSan 的包内四项测试、Core-only 消费、Servo 消费和 Demo 故障日志均在，SDK 技术结论沿用 03B。原包、CMake、包清单和独立消费者未变，本轮未重跑编译、控制回归或硬件。 [03B 验收](2026-11-18-next-servo-03b.md)
- 本轮核对了归档中所有 CMake 源引用和 55 条项目头引入。早期 16 文件都能定位直接依赖，未发现当前选定源码目标引用已排除的供应商、RTOS 或 Legacy 文件。
- 独立复核核对原始 54 个 ZIP 成员、52 行路径与 SHA-256、16 个 `UNRESOLVED`、历史与上游证据、许可证与 Notice、依赖和现有 Gate；现有证据支持保留阻断结论。
- `SOURCE / PROVENANCE = BLOCKED`，`LICENSE = BLOCKED`，`APPROVAL = BLOCKED`。整体 `RELEASE_BLOCKED`。内部候选维持原状。

硬件证据沿用原验收范围。G3-SAFE HARDWARE NOT VERIFIED、G3-PWR LOCKED、AT32F415 WCET NOT VERIFIED、MOTOR PHYSICAL OPERATION UNVERIFIED。

## Git 修改与归档

| 路径 | 交付 |
| --- | --- |
| `docs/release/2026-11-18-sdk-0.1.0-rc.1/source-provenance.tsv` | 52 行逐文件矩阵 |
| `docs/release/2026-11-18-sdk-0.1.0-rc.1/early-files.md` | 16 条逐文件调查与整改建议 |
| `docs/release/2026-11-18-sdk-0.1.0-rc.1/license-decision-memo.md` | MIT/Apache-2.0 决策与执行清单 |
| `docs/release/2026-11-18-sdk-0.1.0-rc.1/upstream-and-notice-review.md` | 上游、许可、版权和 Notice 草案 |
| `docs/release/2026-11-18-sdk-0.1.0-rc.1/owner-approval-template.md` | G1–G5 未签署审批表 |
| `docs/governance/Themis_工程维护日志.md` | 仅追加本轮记录 |
| 本报告与 [raw-evidence.zip](2026-11-18-next-release-02a/raw-evidence.zip) | Git 与归档保护、原始首版、历史、筛查坐标、依赖、独立复核和完整文档补丁 |

下一轮最小动作是签署 16 个依赖文件的来源及权利材料，确认其余项目材料同一许可范围，批准 MIT 或 Apache-2.0，补齐适用声明。然后在单独授权下冻结和复验新的干净提交与归档，对各发布动作取得实际批准。
