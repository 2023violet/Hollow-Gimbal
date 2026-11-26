# FalconFOC V2 代码规范指南

本文是 FalconFOC V2 的正式代码规范（2026-11-25 按 V2 重写；文件名保留，以免失效引用）。Legacy Themis 固件已于 2026-11-24 删除，只保留在 Git tag `legacy-final`，它的分层、RTOS、Shell 与 ops 句柄规则不再适用。

## 1. 定位与优先级

冲突时按下面顺序执行：

1. 系统级与仓库级强制规则、[AGENTS.md](../../AGENTS.md)
2. [项目章程](../architecture/FalconFOC_Project_Charter_and_Engineering_Memory_v1.0.md)与[裸机执行 ADR](../architecture/FalconFOC_BareMetal_Execution_ADR.md)
3. 仓库根目录 `.clang-format`
4. 本文
5. [C 与 H 文件布局与注释书写规范](./Themis_C与H文件布局与注释书写规范.md)（`.c/.h` 内部布局与注释写法）

术语："必须"是硬约束，违反就要整改；"建议"允许少数有理由的例外。

## 2. 目录与依赖方向

目录职责见 AGENTS.md 第 3 节。依赖只能沿下面的方向：

```text
core  <-  sensors, calibration, g3 (np, safe)  <-  runtime  <-  firmware/at32f415
                                                      ^
                                     simulation, control_plane, examples, tools (Host)
protocol  <-  firmware/at32f415, tools
```

- `core/`、`runtime/`、`calibration/`、`sensors/`、`g3/`、`protocol/` 必须可移植：C11，不包含 MCU 或厂商头文件，不访问寄存器，不分配堆内存，不阻塞。
- 板级事实（引脚、ADC 秩、换算系数、Flash 地址、默认配置）只放在 `firmware/at32f415`。
- FOC 热路径（H0 ADC、PWM、SPI 编码器）可以直接用厂商驱动和寄存器，不包一层 HAL。软任务外设（UART、CAN、Flash、复位、NTC 读取）通过 `firmware/at32f415` 里的薄端口结构（函数指针 + context）接入，SIL 用同一端口换成虚拟实现。
- 禁止反向依赖：可移植层不得包含 `firmware/` 或 `simulation/` 的头文件；Host 专用代码只能用 `FOC_RUNTIME_HOST_*` 这类宏隔离，ARM 镜像不编译它。
- 每个带 `CMakeLists.txt` 的目录是独立 CMake 工程，没有根工程；新增源文件时同步所有编译它的工程与 `scripts/sdk/source-map.json`。

## 3. 排版与文件结构

- `.clang-format` 是硬约束（4 空格、100 列、函数大括号另起一行）。只格式化你改动的、原本就符合格式的文件；生成文件（`foc_parameter_table.{h,c}` 等）不手改不格式化。
- 每个 `.c/.h` 以文档头开始：`@file`、`@brief`、`@version`、`@date`，头文件再写 Design overview、Usage example、Ownership / lifecycle / context、Failure / recovery；源文件写 Implementation role。
- 用分节注释组织文件：`/* Includes --- */`、`/* Public Macros --- */`、`/* Public Types --- */`、`/* Private Helpers --- */`、`/* Public Functions --- */`，以 `/* ---- end of file ---- */` 结束。细则见 C/H 布局规范。
- include 顺序：本文件配对的头文件、项目头文件、标准库头文件，组间空一行。
- 注释写"为什么"和契约（单位、范围、上下文、所有权），不复述代码。公开 API 的注释要让调用方不看 `.c` 就能接入。
- 注释与代码一律英文；文档（`docs/`、README）可以中文或英文，同一份文档内保持一种。

## 4. 命名

- 公开符号带模块前缀：函数 `foc_<module>_<verb>`，类型 `struct foc_<module>_<noun>`，枚举值与宏 `FOC_<MODULE>_<NAME>`。板级代码用 `foc_board_*`，串口 `foc_serial_*`，CAN `foc_can_*`。
- 文件内私有函数用 `static`，前缀用模块短名（例如 `cal_`、`store_`、`cogging_`），不必带 `foc_`。
- 物理量在名字里带单位后缀：`_a`、`_v`、`_ohm`、`_h`、`_wb`、`_nm`、`_rad`、`_rad_s`、`_rad_s2`、`_us`、`_s`、`_hz`、`_c`。
- 布尔量用能直接读成判断的名字（`enabled`、`valid`、`active`、`tripped`），不用 `flag`。
- 名字必须和行为一致：叫 `read` 就不改状态，叫 `try_` 就允许失败且不留副作用。

## 5. 函数

- 单一职责。超过约 80 行或出现第二个独立职责时拆分。
- 公开函数检查指针与数值输入：空指针、非有限值（`isfinite`）、越界都返回错误且不改状态。热路径内部的私有函数可以依赖调用方已检查。
- 返回值：模块状态枚举 `FOC_<MODULE>_OK = 0`，错误为负值（例如 `FOC_RUNTIME_ERR_INPUT`）；UART 层返回 `FOC_PROTO_*`。失败时要么不改状态，要么在注释里写明改了什么。
- 不阻塞、不睡眠、不分配。时间一律由调用方显式传入（`now_us`，单调微秒，回绕用无符号差值处理）。
- 只能在特定上下文调用的函数，在声明注释里写明（"H0 only"、"Foreground only"、"Any context"）。
- 回滚路径对称：先在副本上验证，全部通过再一次提交（参照 Runtime 的调参候选与 Board 的 reconfigure）。

## 6. 数值与资源（AT32F415 无 FPU）

- 所有浮点都是软件实现。镜像代码不用 `double`，不用 `sinf/cosf/hypotf/fmax/fmin` 这类重库函数：三角函数用 `foc_sincosf()`，模长用 `sqrtf(x*x + y*y)`，标定数学用 `foc_calibration_math`。fwinteg 的 `no_libm_heavy_math` 会检查。
- 热路径（H0、H1）里每加一次浮点运算都要想清楚代价；一次性计算放到 Foreground。
- 连续位置用 float32 时注意精度随幅值下降：取模一圈用整数部分减法，Runtime 对位置域有显式上限（如 ±1024 rad、±4096 rad）。
- 栈有限：Foreground 函数不在栈上放几百字节以上的数组，大缓冲用有明确所有者的静态或全局对象，并在注释里写清谁在什么时候写。
- 每次改动用 FirmwareSight `fwsight diff` 量 ROM/RAM，带存储镜像必须低于 `store-reserve.ld` 的上限。

## 7. 并发与执行域

- 执行域：H0（ADC 完成中断）、H1（编码器中断）、Foreground（主循环）。每个可变状态只有一个写者，在字段注释里写明所有者。
- 跨执行域发布用"双槽 + 原子下标"或 `stdatomic` 原子量（浮点通过 32 位字传递）；读者只读已发布的槽。
- H0 通过停机路径锁存故障（`foc_runtime_request_inhibit` 加故障位），同一原因只请求一次，避免饿死心跳。
- 中断里不阻塞、不打印、不访问 Flash；Flash 擦写只在 DISABLED 且输出已确认关闭时由 Foreground 发起。
- Foreground 修改 H0/H1 读取的数据（表、配置）时，要么只在对应功能关闭时修改，要么通过 quiesce + 重新初始化完成。

## 8. 结构体与接口演进

- 公开结构体只在末尾追加字段，并在注释里写"Appended field"：仓库里仍有按位置初始化的旧代码，插在中间会整体错位。
- 新代码一律用指定初始化（`.field = value`）。
- 接口要有明确的所有者与生命周期：调用方持有的存储（如齿槽表）在注释里写明可写时机。

## 9. 参数、协议与功能入口

- 参数唯一来源是 `firmware/at32f415/parameters.json`，改完运行 `python -B scripts/gen/gen_parameters.py` 并提交生成文件；质量门禁检查是否过期。
- UART/CAN 新增消息与参数只追加编号，不复用旧编号；改动写进 `protocol/README.md` 与参数契约。
- 每个功能同时提供 UART 入口、CLI 命令和 SIL 场景；标定只在 MCU 上运行，Host 只负责触发和读结果。
- ARM 镜像保持物理输出锁定，直到有台架签字记录。

## 10. 测试、验证与文档

- 新模块配单元测试，CTest 名登记到 `scripts/flows/fwinteg/expected_tests.tsv` 并说明原因；功能级行为用 Board1 SIL 场景验证。
- 提交前至少跑：受影响工程的 Host 测试、ARM 镜像编译、质量门禁；较大改动单独跑 fwinteg（`run_fwinteg.sh`，不要和其他构建并行）。
- 所有证据都是软件证据，不得写成硬件结论；证据记录放 `docs/evidence/`，带日期。
- 改变分层、启动链、协议、参数、持久化格式、标定阶段或脚本入口时，同一轮更新相关 README 与契约，并在[维护日志](./Themis_工程维护日志.md)末尾追加一条。

## 11. 提交前自检

- [ ] 依赖方向正确，可移植层没有厂商头文件，热路径没有被包成 HAL
- [ ] 新公开结构体字段追加在末尾，新代码用指定初始化
- [ ] 镜像代码没有 `double` 和重 libm 调用，ROM/RAM 增量已量过
- [ ] 跨执行域的数据有唯一写者和发布机制
- [ ] 参数改 JSON 并重新生成；协议与契约文档已同步
- [ ] UART 入口、CLI 命令、SIL 场景齐全，单元测试已登记
- [ ] 格式化只作用于原本符合格式的文件，生成文件未手改
- [ ] 维护日志已追加
