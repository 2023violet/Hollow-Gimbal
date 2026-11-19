# 上游核查、版权声明与 NOTICE 草案

**仅供权利人审查。当前候选中没有已确认的公开再分发授权。**

当前范围以 [52 文件来源矩阵](source-provenance.tsv) 为准。包内 C/H 未发现版权、SPDX、外部许可证或明确的移植声明。根目录和扫描的本地可达引用没有找到项目级 `LICENSE`、`COPYING` 或 `NOTICE`。包内 `NOTICE-REVIEW.md` 是既有待审说明。Git 作者、文件头空白和平台上的可见性均不给出指定 SDK 的许可。 [GitHub 许可说明](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/licensing-a-repository)

## 第三方与参考来源矩阵

| 组件或线索 | 可核查的许可和来源 | 对当前候选的处理 | 确认有再分发时的动作 |
| --- | --- | --- | --- |
| FalconFOC V2 项目实现与集成材料 | 36 个文件有分阶段的项目 Git 或快照记录。16 个早期文件首次见于 V2 基线，仍缺入库前权利链。 | 全部内部评估，逐项审查文件头。没有预填持有人，也没有统一 SPDX。 | 由有权许可者确认路径与内容、事实上的作者/共同作者、合同权利和许可。填写适用的版权与许可文本。 |
| Legacy FalconFOC | 历史 `1f54252` 的 `applications/algo/foc_core.c`、`foc_core.h`、`foc_math.h` 显示 Zhang Jiaming 署名和保留权利声明。G2 原报告记录物理符号约定，G3-NP 记录 Board1 事实。 | Legacy 文件排除。仅将署名用作向原作者核实的线索。若有 Legacy 具体实现进入 V2，必须确定段落和持有人。 | 保留 Legacy 原有声明。针对已确认复用的表达取得明确许可或考虑排除、独立实现。 |
| VESC | 固定提交 `cb79df8960f77fcdf368f3b921d24d5a3ed86345`，所查看的 VESC 实现源头声明 GPL-3.0-or-later。 [固定版 foc_math.c 文件头](https://raw.githubusercontent.com/vedderb/bldc/cb79df8960f77fcdf368f3b921d24d5a3ed86345/motor/foc_math.c) | 未识别出 VESC 源文件入包。固定样本的有限文本筛查不能排除历史或结构性改编。 | 若确认 GPL 代码的衍生关系，进入 `LEGAL/PROVENANCE REVIEW REQUIRED`，按适用范围处理许可证及源码义务，或取得另行授权、排除或独立实现。 |
| SimpleFOC | 固定 `v2.3.4` 提交 `c72f063f3d2d2e26d49a22a7383310cf8f88134b` 的根许可为 MIT。Legacy 维护记录提到的是 CawFOC/SimpleFOC 系参考项目。 [原许可](https://raw.githubusercontent.com/simplefoc/Arduino-FOC/c72f063f3d2d2e26d49a22a7383310cf8f88134b/LICENSE) | 所选实现参与筛查。未识别出该源码入包。 | 若确认复制，核实确切版本与文件级声明，并保留适用版权和 MIT 许可文本。 |
| ODrive `fw-v0.5.6` | `a308314ed2ca613164b81e7bbdfacc53cd1859ff` 的 Firmware 独立许可证为 MIT。 [Firmware 许可](https://raw.githubusercontent.com/odriverobotics/ODrive/a308314ed2ca613164b81e7bbdfacc53cd1859ff/Firmware/LICENSE) | 所选实现参与筛查。该许可结论限于此固定版本。FalconFOC Motor-ID 报告记录工程思路参考与独立实现。 | 若确认复制，逐文件保留适用版权和许可。实际用到的版本应另行锁定。 |
| CyberBeast `robot_mc` | `2f1fb61ec081eb7a42ab8436ba8f1504fc529da7` 根 MIT 文本含 Ben Katz 与 CyberBeast 署名。V2 纲领同时提醒供应商目录另有条件。 [原许可](https://raw.githubusercontent.com/cyberbeast0829/robot_mc/2f1fb61ec081eb7a42ab8436ba8f1504fc529da7/LICENSE) | Motor-ID 和 Encoder 报告记录参考设计思路。已审实现和候选中未识别第三方文件。 | 确有分发的源码再核对文件级权利，必要时同时保留 ODrive、Ben Katz 等上游适用声明。 |
| SguanFOC、moteus、CawFOC/OSHWHub | 项目纲领及 Legacy 治理日志记载外部设计、行为、板卡与测试参考。实际适用版本和许可未从这些记录确定。 | 仅为线索，未认定派生关系，也未在固定版本文本筛查中纳入。 | 若实际证实复制或适配，取得确切 URL、版本、文件、许可证和对应权利证明。 |
| FreeRTOS | 仓库 `kernel/freertos/LICENSE` 含 MIT 和 Amazon.com 或其关联方版权。 | 排除，现有 SDK CMake 和头文件闭包未引用。 | 将来纳入时保留原版权/许可文本。 |
| CMSIS/CMSIS-DSP | 仓库 `platform/at/at32f415/vendor/cmsis/dsp/ComputeLibrary/README.md` 区分 Compute Library MIT 与其他 CMSIS-DSP 部分 Apache-2.0。 | 排除当前 SDK。尚未为厂商树锁定每个上游版本。 | 将来逐路径匹配版权、上游许可证、适用 Notice 及修改标记。 |
| Artery AT32 | 仓库 `platform/at/at32f415/vendor/drivers/src/at32f415_adc.c` 头部列 Artery 版权、与其 MCU 配套使用及分发条件。 | Artery 代码、启动和板实现均排除。 | 若未来加入，确认实际允许用途与发布范围，保留供应商条件和声明。 |
| CMake/Ninja/编译器/运行库 | SDK 构建的外部环境。 | 无随包工具二进制或系统库源码。 | 如改为随包分发工具或编译产物，另审实际链接、来源与条款。 |

本轮比对的项目、精确提交、文件路径和内容哈希见证据包的 `upstream.json`。应保留已经存在于仓库中的适用第三方文件原声明。对当前 52 文件，尚无依据定稿一份声明“这些第三方材料随包发行”的 NOTICE。普通技术参考可列为致谢，具体分发义务取决于真实包含、版权权利和对应许可证。

## 项目声明候选

下列字段供权利人填妥。只可用于已核实权利和具体路径的文件类目。

```text
Copyright (c) [已核实的年份] [实际版权持有人]
覆盖范围 [具体路径、Manifest SHA-256 或批准附件]
许可 [权利人签署批准的 MIT 或 Apache-2.0]
例外 [另行保留的上游版权、许可和适用 Notice]
```

## 文件头、修改记录与 Notice 审核

| 范围 | 待确认事项 |
| --- | --- |
| 16 个早期文件 | 首版和当前候选均无明确第三方源码声明。由原开发者标注是否沿用了其他实现表达，并证明实际版权持有人。发现上游后逐路径保留适用声明和条款。 |
| 36 个项目实现及集成材料 | 核对贡献、共同作者、雇佣/合同可能产生的权利；补入经许可的自有版权和许可说明，范围以最终 Manifest 为准。 |
| 已引用思路、尚无入包复制证据的参考项目 | 可在文档保留准确技术致谢。审批时继续分别列出第三方来源、实际作品和许可义务。 |
| 确认存在 MIT 上游材料时 | 随复制或实质部分保留版权和许可文本。MIT 文本本身没有 Apache-2.0 的改动声明和 NOTICE 机制。 |
| 确认存在 Apache-2.0 上游材料时 | 随分发提供许可证，保留适用声明，显著标注改动文件。原作品有适用 NOTICE 时提供相关可读声明，按其 §4 选择保留位置。 |
| 公开版本的 NOTICE `[待审]` | 项目信息、第三方组件及固定版本、适用上游归属、厂商限制、改动说明和可选致谢分别列项。等真实权利及分发范围确认再定稿。 |

[MIT 原文](https://opensource.org/license/mit) · [Apache-2.0 原文](https://www.apache.org/licenses/LICENSE-2.0)
