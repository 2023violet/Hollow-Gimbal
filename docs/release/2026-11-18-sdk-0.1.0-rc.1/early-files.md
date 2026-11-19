# 16 个早期文件逐项调查

**状态：16/16 保持 `UNRESOLVED`。** 首次入库、当时完整字节、后续变动均已核对。

共同首入库提交为 `46bc963f5e5849e4440d1714a845fe485d338837`，Git 作者为 `2023violet`，作者日期为 `2026-11-11T02:00:52-08:00`，说明为 `feat: establish FalconFOC V2 software baseline`。唯一父提交为 `1f542521189d5e0ef25e64e6530608ece8d6e137`。本地非浅仓库，在本地分支和已保存的远端跟踪引用中核查。父提交无这些路径，父树及本地可达历史未找到更早的相同首版 blob。Git 记录和同期开发报告能说明技术过程。首入库前的实际贡献权利仍待可核验证据。

G2 明示沿用 Legacy 的物理符号约定，G3-NP 明示沿用 Board1 的 ADC/PWM 事实。Legacy 中 `applications/algo/foc_core.c`、`foc_core.h` 与 `foc_math.h` 的历史文件头有 Zhang Jiaming 的署名版权与保留权利声明。历史 Git 记录提供同一人的核对线索，尚不能由此确认当前 SDK 的法律主体、共同贡献或许可权。Legacy 声明不写入候选中的同名 V2 文件。

文本筛查去除注释、空白和独立花括号，检查父提交中 48 个 Legacy 应用、BSP 与框架文件，并与 VESC、SimpleFOC、ODrive、robot_mc 的固定提交各两个相关实现比较。Legacy 的较长命中是 C/C++ 链接与头文件样板及通用三相字段。选定外部实现没有达到三行阈值的连续命中。比对不涵盖所有版本、改名、重排、结构性改编或未存入 Git 的源文件。独立复核还对选定 VESC 版本的六个文件采用另一组规范化规则复查。 [外部证据索引](upstream-and-notice-review.md)

本页的首版字节、比较坐标与完整 Git 记录随[证据包](../../evidence/2026-11-18-next-release-02a/raw-evidence.zip)保存。同一文件的工程来源等级、法律权利和再分发状态单列在[逐文件矩阵](source-provenance.tsv)。

### `core/include/foc_core.h`

- 首版 Git blob `277dafaa1f5ddd62abd17446855ec69dd3581228`，SHA-256 `9387cf26b76f7b8b442e0332e2d69e8ffcd3842247f7982988cf877e1b69a6aa`。候选 SHA-256 `6002387edf6ab620463fbc8fe080c3094798ac61df1d1551f748d23fd07d696e`。
- 设计与来源线索。核心公开类型、状态、Clarke/Park/SVPWM API。Legacy 与该头部有通用三相字段和 C++ 链接封装。 [仓库记录](../../../docs/evidence/2026-11-09-g2-core-validation.md)
- 后续历史。02425e0 feat: publish virtual motor baseline and periodic phase migration。首版对候选的净行变更为 `22/3`。
- 比对摘要。父树无相同 blob；连续规范化行最长值（按比较组）为 legacy_parent 5 / ODrive 0 / robot_mc 0 / SimpleFOC 0 / VESC 1。首版和候选均无外部版权或许可标记。
- 发布判断。**UNRESOLVED / E1**。Core 的接口以及 Current、Motion、Device 的头文件依赖。 早期类型/API 设计记录与 Legacy 继承范围。 需创作者/共同作者及适用合同、上游条款与书面授权。 优先补取授权。若确认有上游段落，先界定段落及适用条款。

### `core/include/foc_current.h`

- 首版 Git blob `d29d15390bb683b46a7a3487bee3a417a9b23482`，SHA-256 `a6f36b6edb668ad5b4048bd2c86637b4652d491be4ef2e5f4d5c7e8269a76e46`。候选 SHA-256 `8cffeef494f06cefb7f91b8623edfd0e13fec53a029f9f849346b0c73a8c473c`。
- 设计与来源线索。独立 dq Current PI 实例、初始化和输出契约。Legacy 电流环的行为是历史背景。 [仓库记录](../../../docs/evidence/2026-11-10-next-foc-01-current-validation.md)
- 后续历史。278c691 feat: establish FalconFOC V2 RC0 software baseline。首版对候选的净行变更为 `62/0`。
- 比对摘要。父树无相同 blob；连续规范化行最长值（按比较组）为 legacy_parent 4 / ODrive 0 / robot_mc 0 / SimpleFOC 0 / VESC 1。首版和候选均无外部版权或许可标记。
- 发布判断。**UNRESOLVED / E1**。Core Current 公开头，由 Pipeline 接入。 PI 接口从设计到首轮编写的记录及作者许可。 需创作者/共同作者及适用合同、上游条款与书面授权。 优先补取授权。若确认有上游段落，先界定段落及适用条款。

### `core/include/foc_motion.h`

- 首版 Git blob `03b6aabd74a41b531cdca4d3fb4b2209e1a096d8`，SHA-256 `1fff5526d872526d21280f4514b8842ac39dfd971422b2044318626d0ff960c5`。候选 SHA-256 `df28d77f6080de395d785f0e94b80837a75d3c9cef790d94cc318b2e65c3349f`。
- 设计与来源线索。转子映射、速度 PI、位置 P 和预测接口。连续机械坐标由调用者建立。 [仓库记录](../../../docs/evidence/2026-11-10-next-foc-02-motion-validation.md)
- 后续历史。02425e0 feat: publish virtual motor baseline and periodic phase migration; f00e9a4 test(motion): lock overflow atomicity and prediction numeric contract。首版对候选的净行变更为 `42/1`。
- 比对摘要。父树无相同 blob；连续规范化行最长值（按比较组）为 legacy_parent 3 / ODrive 0 / robot_mc 0 / SimpleFOC 0 / VESC 1。首版和候选均无外部版权或许可标记。
- 发布判断。**UNRESOLVED / E1**。Core 公开运动头，被 Runtime 和 Virtual Motor 使用。 运动接口设计原稿、参考来源与贡献授权。 需创作者/共同作者及适用合同、上游条款与书面授权。 优先补取授权。若确认有上游段落，先界定段落及适用条款。

### `core/src/foc_core.c`

- 首版 Git blob `093246e0858aec9581da4399aea2e0d570877e9d`，SHA-256 `32a52889cc950b25a4dff37476087cdcfe5b63613288df35f46f4b68067b2bb5`。候选 SHA-256 `2b119a953aeb51d6de221b6a16674ec8b36a725421604494126f2d7f3d7deb60`。
- 设计与来源线索。变换、限幅与调制。G2 明示沿用 Legacy 符号约定，经独立测试向量验证。有限文本筛查未识别实现片段的直接转移。 [仓库记录](../../../docs/evidence/2026-11-09-g2-core-validation.md)
- 后续历史。278c691 feat: establish FalconFOC V2 RC0 software baseline。首版对候选的净行变更为 `87/63`。
- 比对摘要。父树无相同 blob；连续规范化行最长值（按比较组）为 legacy_parent 1 / ODrive 0 / robot_mc 1 / SimpleFOC 0 / VESC 1。首版和候选均无外部版权或许可标记。
- 发布判断。**UNRESOLVED / E1**。Core 静态库显式编译目标，供 Current 和 Host 使用。 原始变换、调制实现草稿及 Legacy 权利链。 需创作者/共同作者及适用合同、上游条款与书面授权。 优先补取授权。若确认有上游段落，先界定段落及适用条款。

### `core/src/foc_current.c`

- 首版 Git blob `53d1a48e96869a6e0430b0351adc02156a897610`，SHA-256 `73da3c0868b361c9f238823f580eb79e3edbbd984ef1045d0f551fd79ed26364`。候选 SHA-256 `7f73668807f87cc5993eb17984345475f860fec224d8af7bd7e1f7aca0a38e42`。
- 设计与来源线索。dq PI、最终占空比还原、抗饱和、事务保护。同期报告描述先写测试再实现的过程。 [仓库记录](../../../docs/evidence/2026-11-10-next-foc-01-current-validation.md)
- 后续历史。278c691 feat: establish FalconFOC V2 RC0 software baseline。首版对候选的净行变更为 `161/16`。
- 比对摘要。父树无相同 blob；连续规范化行最长值（按比较组）为 legacy_parent 1 / ODrive 0 / robot_mc 1 / SimpleFOC 0 / VESC 1。首版和候选均无外部版权或许可标记。
- 发布判断。**UNRESOLVED / E1**。Core 静态库显式编译目标，Pipeline 需要其 PI 路径。 抗饱和实现原稿与输入来源。 需创作者/共同作者及适用合同、上游条款与书面授权。 优先补取授权。若确认有上游段落，先界定段落及适用条款。

### `core/src/foc_motion.c`

- 首版 Git blob `1eb419bb1ab44aeae63da9b8b8bbaa465d3d69d7`，SHA-256 `43c10073540271ac2f762e07b435cdcf00e25fac3d55ee99a266af9e25bf16d3`。候选 SHA-256 `97502587939e7ac2dfd4297acdf94f30f53afe710e3e21b56ab8fc28dc73b710`。
- 设计与来源线索。转子电角换算和两个外环数值实现。标准控制关系本身没有给出文件级权利证明。 [仓库记录](../../../docs/evidence/2026-11-10-next-foc-02-motion-validation.md)
- 后续历史。02425e0 feat: publish virtual motor baseline and periodic phase migration。首版对候选的净行变更为 `31/0`。
- 比对摘要。父树无相同 blob；连续规范化行最长值（按比较组）为 legacy_parent 1 / ODrive 0 / robot_mc 1 / SimpleFOC 0 / VESC 1。首版和候选均无外部版权或许可标记。
- 发布判断。**UNRESOLVED / E1**。Core 静态库显式编译目标，Runtime 依赖。 外环及预测实现原稿、外部参考边界。 需创作者/共同作者及适用合同、上游条款与书面授权。 优先补取授权。若确认有上游段落，先界定段落及适用条款。

### `g3/np/include/foc_device.h`

- 首版 Git blob `1154e5ba1e081ffaf593a38e0bc5c0061e2ab320`，SHA-256 `765ad6c04fd10921cfb00afd050b711f72950b42e7a3484775c6283f2a84afb0`。候选 SHA-256 `765ad6c04fd10921cfb00afd050b711f72950b42e7a3484775c6283f2a84afb0`。
- 设计与来源线索。ADC 采样/相序、PWM Compare 和输出动作接口。Board1 参数与排序是报告中确认的设计输入。 [仓库记录](../../../docs/evidence/2026-11-09-g3-np-validation.md)
- 后续历史。无后续可达 HEAD 改动。首版对候选的净行变更为 `字节相同`。
- 比对摘要。父树无相同 blob；连续规范化行最长值（按比较组）为 legacy_parent 4 / ODrive 0 / robot_mc 0 / SimpleFOC 0 / VESC 1。首版和候选均无外部版权或许可标记。
- 发布判断。**UNRESOLVED / E1**。Pipeline、Device Sim、Safety 等包含该公开头。 接口设计草案与 Board1 继承材料说明。 需创作者/共同作者及适用合同、上游条款与书面授权。 优先补取授权。若确认有上游段落，先界定段落及适用条款。

### `g3/np/include/foc_device_sim.h`

- 首版 Git blob `af63a3a484662fe229e8ec813dc96bd6925a5659`，SHA-256 `3159a894f8d4ffb30cb7dcacf9d4fcdbca90f9d2668534c9c75947d79c831299`。候选 SHA-256 `3159a894f8d4ffb30cb7dcacf9d4fcdbca90f9d2668534c9c75947d79c831299`。
- 设计与来源线索。仿真寄存器和闭门状态契约。没有供应商寄存器头依赖。 [仓库记录](../../../docs/evidence/2026-11-09-g3-np-validation.md)
- 后续历史。无后续可达 HEAD 改动。首版对候选的净行变更为 `字节相同`。
- 比对摘要。父树无相同 blob；连续规范化行最长值（按比较组）为 legacy_parent 4 / ODrive 0 / robot_mc 0 / SimpleFOC 0 / VESC 1。首版和候选均无外部版权或许可标记。
- 发布判断。**UNRESOLVED / E1**。Pipeline 公共头和 Demo 直接包含该头。 仿真接口原始设计和独立开发记录。 需创作者/共同作者及适用合同、上游条款与书面授权。 优先补取授权。若确认有上游段落，先界定段落及适用条款。

### `g3/np/src/foc_device.c`

- 首版 Git blob `b556bea39d1c089124f9cf886fc811711ce33bd3`，SHA-256 `2cbcdf674013eb1443ee1f6940d611ec82fa5e7d1f8c1ac14887296bc992c27b`。候选 SHA-256 `2cbcdf674013eb1443ee1f6940d611ec82fa5e7d1f8c1ac14887296bc992c27b`。
- 设计与来源线索。原始 ADC 转换、ABC/ACB 与 PWM Compare。原报告逐项标明参考的 Board1 事实；构建闭包无供应商驱动。 [仓库记录](../../../docs/evidence/2026-11-09-g3-np-validation.md)
- 后续历史。无后续可达 HEAD 改动。首版对候选的净行变更为 `字节相同`。
- 比对摘要。父树无相同 blob；连续规范化行最长值（按比较组）为 legacy_parent 1 / ODrive 1 / robot_mc 1 / SimpleFOC 1 / VESC 1。首版和候选均无外部版权或许可标记。
- 发布判断。**UNRESOLVED / E1**。通过 Host Pipeline 静态库直接编译。 Board1 参考事实与实现表达的边界、实际作者权利依据。 需创作者/共同作者及适用合同、上游条款与书面授权。 优先补取授权。若确认有上游段落，先界定段落及适用条款。

### `g3/np/src/foc_device_sim.c`

- 首版 Git blob `a3bf603180a9ac8333927f76dcfbdec78996a153`，SHA-256 `2f91d08cd3f4a9d516a79bf4166ea86983af27b82449c27cf338f92a4dc4b822`。候选 SHA-256 `2f91d08cd3f4a9d516a79bf4166ea86983af27b82449c27cf338f92a4dc4b822`。
- 设计与来源线索。无功率仿真器与受控输出适配器。报告记录始终关闭的 bridge gate。 [仓库记录](../../../docs/evidence/2026-11-09-g3-np-validation.md)
- 后续历史。无后续可达 HEAD 改动。首版对候选的净行变更为 `字节相同`。
- 比对摘要。父树无相同 blob；连续规范化行最长值（按比较组）为 legacy_parent 1 / ODrive 1 / robot_mc 1 / SimpleFOC 1 / VESC 1。首版和候选均无外部版权或许可标记。
- 发布判断。**UNRESOLVED / E1**。Host Pipeline 静态库直接编译，示例调用模拟提交。 Host 模拟器原稿及其测试演化记录。 需创作者/共同作者及适用合同、上游条款与书面授权。 优先补取授权。若确认有上游段落，先界定段落及适用条款。

### `g3/safe/include/foc_safe.h`

- 首版 Git blob `f020435dcd069783f4d3f2c689de0d9defc74886`，SHA-256 `4db7aa1d47d3d0cc9ecaa4bcbd365bde8d46b7882b4c068323b04abd0f5e586e`。候选 SHA-256 `4db7aa1d47d3d0cc9ecaa4bcbd365bde8d46b7882b4c068323b04abd0f5e586e`。
- 设计与来源线索。软件状态、故障、时间新鲜度、心跳和许可数据结构。 [仓库记录](../../../docs/evidence/2026-11-09-g3-safe-validation.md)
- 后续历史。无后续可达 HEAD 改动。首版对候选的净行变更为 `字节相同`。
- 比对摘要。父树无相同 blob；连续规范化行最长值（按比较组）为 legacy_parent 5 / ODrive 0 / robot_mc 0 / SimpleFOC 0 / VESC 1。首版和候选均无外部版权或许可标记。
- 发布判断。**UNRESOLVED / E1**。Pipeline 公开契约依赖该头。 Safety 契约的原创技术记录和签署人。 需创作者/共同作者及适用合同、上游条款与书面授权。 优先补取授权。若确认有上游段落，先界定段落及适用条款。

### `g3/safe/src/foc_safe.c`

- 首版 Git blob `144137b6765e90252f66662a58f35a3c9e1e60b0`，SHA-256 `eb3cff528d0a85b93ff124bd712c16920ffc99813fcc075f479d01599f70dcf1`。候选 SHA-256 `eb3cff528d0a85b93ff124bd712c16920ffc99813fcc075f479d01599f70dcf1`。
- 设计与来源线索。软件故障优先级、序号回绕、输出拒绝和清除契约。报告把看门狗许可与实际硬件实现区分。 [仓库记录](../../../docs/evidence/2026-11-09-g3-safe-validation.md)
- 后续历史。无后续可达 HEAD 改动。首版对候选的净行变更为 `字节相同`。
- 比对摘要。父树无相同 blob；连续规范化行最长值（按比较组）为 legacy_parent 1 / ODrive 0 / robot_mc 1 / SimpleFOC 1 / VESC 1。首版和候选均无外部版权或许可标记。
- 发布判断。**UNRESOLVED / E1**。Host Pipeline 静态库直接编译，运行闭环依赖。 状态机设计稿、实现作者确认和贡献授权。 需创作者/共同作者及适用合同、上游条款与书面授权。 优先补取授权。若确认有上游段落，先界定段落及适用条款。

### `runtime/include/foc_pipeline.h`

- 首版 Git blob `a1cce057823ed8d791ed8dbb5c8fdc5148cd9430`，SHA-256 `5bae1fd83e689b9c35b8f138e9f25dc8eb9f60b90452fce9756042240c5bbae4`。候选 SHA-256 `5d18406585623fd3ba92bacb49f931b694329812501d24db8731497f5e744c6f`。
- 设计与来源线索。把 Device、Core 与 Safety 显式组合为单执行者 Pipeline 接口。同期测试先暴露缺少该头。 [仓库记录](../../../docs/evidence/2026-11-10-next-sw01a-pipeline-validation.md)
- 后续历史。278c691 feat: establish FalconFOC V2 RC0 software baseline。首版对候选的净行变更为 `37/1`。
- 比对摘要。父树无相同 blob；连续规范化行最长值（按比较组）为 legacy_parent 1 / ODrive 0 / robot_mc 0 / SimpleFOC 0 / VESC 0。首版和候选均无外部版权或许可标记。
- 发布判断。**UNRESOLVED / E1**。Runtime 公开头包含该接口。 早期 Pipeline 接口的实际创作记录和授权。 需创作者/共同作者及适用合同、上游条款与书面授权。 优先补取授权。若确认有上游段落，先界定段落及适用条款。

### `runtime/include/foc_runtime.h`

- 首版 Git blob `a59e6c45dcc3441434679626d57be1e3b328423b`，SHA-256 `a36489f52cd998b21515b3687c8cbdf44d3dd0999c98ccec4c8d5d90e109f696`。候选 SHA-256 `4a9733554416608de895dc00b6b20277a07596cb10274f89a9abddd63512c883`。
- 设计与来源线索。H0/H1/Foreground、命令与 Sensor 源身份、生命周期及原子快照接口。 [仓库记录](../../../docs/evidence/2026-11-10-next-sw01b-runtime-validation.md)
- 后续历史。278c691 feat: establish FalconFOC V2 RC0 software baseline; 02425e0 feat: publish virtual motor baseline and periodic phase migration; a07d7db feat(servo): archive impedance and mechanical model workflows。首版对候选的净行变更为 `158/3`。
- 比对摘要。父树无相同 blob；连续规范化行最长值（按比较组）为 legacy_parent 1 / ODrive 0 / robot_mc 0 / SimpleFOC 1 / VESC 0。首版和候选均无外部版权或许可标记。
- 发布判断。**UNRESOLVED / E1**。Host 示例和 Virtual Motor 直接包含该头。 邮箱和所有权 API 的开发记录及后续贡献授权。 需创作者/共同作者及适用合同、上游条款与书面授权。 优先补取授权。若确认有上游段落，先界定段落及适用条款。

### `runtime/src/foc_pipeline.c`

- 首版 Git blob `22e1c127e6f37da6679a1e9417d832852bcec44e`，SHA-256 `3e81543143e9f7f01994cb6a7b7755f5ccb9b6405688dc63f695f9e29b12b698`。候选 SHA-256 `1e6dadcf6f9cd564ea438f83fd50fdc546df40040565c3a4d1d4e9b4439fb6b7`。
- 设计与来源线索。候选 PI、最终 Safety Gate、Mock 接纳后提交积分。与 Legacy 直接 ISR 的组织不同。 [仓库记录](../../../docs/evidence/2026-11-10-next-sw01a-pipeline-validation.md)
- 后续历史。278c691 feat: establish FalconFOC V2 RC0 software baseline; f9af51b perf(pipeline): remove redundant register snapshot copy。首版对候选的净行变更为 `41/4`。
- 比对摘要。父树无相同 blob；连续规范化行最长值（按比较组）为 legacy_parent 1 / ODrive 1 / robot_mc 1 / SimpleFOC 1 / VESC 1。首版和候选均无外部版权或许可标记。
- 发布判断。**UNRESOLVED / E1**。Host Pipeline 静态库直接编译，Runtime 调用。 事务门禁原稿、已知贡献者及新增改动的权利归属。 需创作者/共同作者及适用合同、上游条款与书面授权。 优先补取授权。若确认有上游段落，先界定段落及适用条款。

### `runtime/src/foc_runtime.c`

- 首版 Git blob `b9b1eb5642248f79544dc664fc5a8cfd87866486`，SHA-256 `80d8710f757dc5c4ed6dc4e8e5f88962b0ea9936a52522836c8e4f3f2d2d7d22`。候选 SHA-256 `8ed9e9839c5d1ee8bfe33082747e9f2e446519d46c3539bb1e21002e390f72b4`。
- 设计与来源线索。调度、原子发布、STOP 和恢复。后续按周期角、生命周期与 IMPEDANCE 扩展。 [仓库记录](../../../docs/evidence/2026-11-10-next-sw01b-runtime-validation.md)
- 后续历史。278c691 feat: establish FalconFOC V2 RC0 software baseline; 02425e0 feat: publish virtual motor baseline and periodic phase migration; 6948595 fix(runtime): preserve safety processing on lifecycle rejection; a07d7db feat(servo): archive impedance and mechanical model workflows。首版对候选的净行变更为 `523/35`。
- 比对摘要。父树无相同 blob；连续规范化行最长值（按比较组）为 legacy_parent 2 / ODrive 1 / robot_mc 1 / SimpleFOC 1 / VESC 1。首版和候选均无外部版权或许可标记。
- 发布判断。**UNRESOLVED / E1**。Host Runtime 的直接编译单元，Servo 软件输出契约依赖。 初版原子邮箱/调度开发证据，逐段识别后续改动与授权。 需创作者/共同作者及适用合同、上游条款与书面授权。 优先补取授权。若确认有上游段落，先界定段落及适用条款。
