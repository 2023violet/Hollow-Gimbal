# Themis CMake 构建架构说明

## 1. 文档目标

本文固定 FalconFoc 当前 live 仓库的正式 CMake 口径，只描述仓库今天真实存在的 build owner、metadata contract、prepared inactive 策略与验证入口。

与本文配套的长期使用与施工手册见：

- `docs/architecture/Themis_FalconFoc_CMake构建策略与新产品落地规范.md`

这份文档回答 4 件事：

1. root `CMakeLists.txt` 到底负责什么，不负责什么。
2. `CPU class`、`platform/SoC`、`board`、`motor/product` 4 类真相分别归谁。
3. 当前 live baseline 与 prepared inactive 平台是怎样共存的。
4. 以后换芯时标准 CMake 路径应该怎么走。

## 2. 当前正式入口

### 2.1 顶层 `CMakeLists.txt`

职责：

- 定义 firmware / host-test 开关
- 定义 profile / flavor 入口
- 解析 `vendor/soc`
- 加载 platform metadata
- 加载 CPU-class metadata
- 创建 contract target
- 组装 firmware 与 host tests

不负责：

- 持有某颗芯片的 startup / linker / exact part 宏
- 持有某个 CPU class 的 `mcpu` / `mfpu` / FreeRTOS port 真相
- 承载 FalconFoc 产品 feature policy

### 2.2 `cmake/ThemisPlatformMetadata.cmake`

职责：

- 统一声明 platform selection cache
- 统一声明 expert metadata override cache
- 统一 root 与 toolchain 的 platform / CPU-class metadata 装配逻辑
- 固定 `THEMIS_CPU_CLASS_OVERRIDE > platform default` 的优先级

不负责：

- module / firmware DSL
- product feature policy
- source manifest owner

### 2.3 `cmake/ThemisBuild.cmake`

职责：

- 提供统一 helper / DSL
- 固定 module / interface-module / host-test / firmware 的声明方式
- 让子层 manifest 保持 declarative

### 2.4 `tools/cmake/arm-gcc-toolchain.cmake`

职责：

- 定义 ARM bare-metal 交叉编译身份
- 查找 `arm-none-eabi-*`
- 先解析 platform metadata，再解析 CPU-class metadata
- 生成工具链级 `mcpu` / `mfpu` / `mfloat-abi` baseline

不负责：

- 写死 live baseline SoC
- 写死 live CPU class
- 持有产品层行为

## 3. 四类 owner 口径

### 3.1 CPU class owner

固定目录：

- `platform/cpu/<cpu-class>/ThemisCpuClassDefaults.cmake`

职责：

- `mcpu`
- `mfpu`
- `mfloat-abi`
- FreeRTOS `portable` 路径
- `ARM_MATH_*`

当前正式 CPU-class：

- `cortex-m4f-hard`
- `cortex-m3-soft`

### 3.2 Platform / SoC owner

固定目录：

- `platform/<vendor>/<soc>/`

职责：

- startup
- linker script
- vendor headers
- generated bring-up
- SoC compile definitions
- `THEMIS_PLATFORM_CPU_CLASS_DEFAULT`
- board-visible SoC facts passed through compile definitions, such as:
  - `CMSIS_device_header`
  - `THEMIS_PLATFORM_FLASH_DRIVER_HEADER`
  - `THEMIS_PLATFORM_FLASH_*`
  - `THEMIS_PLATFORM_MOTOR_SOFTIRQ_*`
- 平台时序 seam，例如 `generated/inc/tm_platform_foc_timing.h`

不负责：

- FreeRTOS CPU-class 通用逻辑
- 产品 feature policy
- 电机默认参数

### 3.3 Board owner

当前 owner：

- `bsp/board`
- `bsp/ports`

职责：

- 引脚与外设实例
- 板级 flash / reset / console 事实
- 薄控制面 port

### 3.4 Motor / product owner

当前 owner：

- `applications/`
- 当前默认电机与保护参数仍由 `applications/algo/foc_config.h` 持有

职责：

- 极对数
- Rs / Ls / flux / J
- 默认 PID
- 保护阈值
- 校准默认值

本轮明确不引入新的 motor-profile 框架。

## 4. 当前 metadata contract

### 4.1 Platform metadata

每个真实平台目录必须自带：

- `ThemisPlatformDefaults.cmake`

必须定义：

- `THEMIS_PLATFORM_TARGET_DEFAULT`
- `THEMIS_PLATFORM_ENTRY_SOURCE_DEFAULT`
- `THEMIS_PLATFORM_STARTUP_SOURCE_DEFAULT`
- `THEMIS_PLATFORM_LINKER_SCRIPT_DEFAULT`
- `THEMIS_PLATFORM_COMPILE_DEFINITIONS_DEFAULT`
- `THEMIS_PLATFORM_CPU_CLASS_DEFAULT`

规则：

- 这些默认值属于 platform owner
- `cmake/ThemisPlatformMetadata.cmake` 统一装配
- root / toolchain 只解析和校验
- 显式 `*_OVERRIDE` 永远高于 defaults

### 4.2 CPU-class metadata

每个真实 CPU-class 目录必须自带：

- `ThemisCpuClassDefaults.cmake`

必须定义：

- `THEMIS_CPU_TOOLCHAIN_CPU_DEFAULT`
- `THEMIS_CPU_TOOLCHAIN_FPU_DEFAULT`
- `THEMIS_CPU_TOOLCHAIN_FLOAT_DEFAULT`
- `THEMIS_CPU_FREERTOS_PORT_GLOB_DEFAULT`
- `THEMIS_CPU_FREERTOS_PORT_INCLUDE_DIR_DEFAULT`
- `THEMIS_CPU_ARM_MATH_DEFINE_DEFAULT`

规则：

- 这些默认值属于 CPU-class owner
- `cmake/ThemisPlatformMetadata.cmake` 统一装配
- root / toolchain / kernel 只消费，不再写死
- `MCU_CPU` / `MCU_FPU` / `MCU_FLOAT` 继续保留，但降级为 emergency/debug 入口

## 5. 当前公开 CMake 入口

当前正式标准入口：

- `THEMIS_PLATFORM_VENDOR`
- `THEMIS_PLATFORM_SOC`
- `THEMIS_DEVDRV_PROFILE`
- `THEMIS_PRODUCT_FLAVOR`
- `THEMIS_ENABLE_BOARD_BENCH`
- `THEMIS_FIRMWARE_TARGET_NAME`
- `THEMIS_FREERTOS_HEAP_IMPL`

当前 expert metadata override 入口：

- `THEMIS_PLATFORM_DIR_OVERRIDE`
- `THEMIS_PLATFORM_TARGET_OVERRIDE`
- `THEMIS_ENTRY_SOURCE_OVERRIDE`
- `THEMIS_STARTUP_SOURCE_OVERRIDE`
- `THEMIS_LINKER_SCRIPT_OVERRIDE`
- `THEMIS_PLATFORM_COMPILE_DEFINITIONS_OVERRIDE`
- `THEMIS_CPU_CLASS_OVERRIDE`

紧急 toolchain/debug 入口：

- `MCU_CPU`
- `MCU_FPU`
- `MCU_FLOAT`

标准换芯路径不应依赖 `MCU_*`，也不应绕过 preset lane 扩散裸 `cmake -S/-B -DCMAKE_TOOLCHAIN_FILE=...`。

## 6. 当前 contract target

当前正式 contract target：

- `themis_root_headers`
- `themis_build_contract`
- `themis_cpu_class_contract`
- `themis_platform_contract`
- `themis_host_test_contract`

职责拆分：

- `themis_build_contract`
  - 语言标准、warning、profile/flavor
- `themis_cpu_class_contract`
  - `ARM_MATH_*`
- `themis_platform_contract`
  - SoC compile definitions

## 7. 当前 live baseline 与 prepared inactive

当前唯一 live baseline：

- `THEMIS_PLATFORM_VENDOR=at`
- `THEMIS_PLATFORM_SOC=at32f403a`
- `THEMIS_PLATFORM_CPU_CLASS_DEFAULT=cortex-m4f-hard`

当前允许存在的 prepared inactive：

- `platform/at/at32f415`
- `platform/at/at32f423`
- `platform/cpu/cortex-m3-soft`
- `platform/cpu/cortex-m4f-hard`

prepared inactive 的定义：

- 可以 configure / build
- 不自动进入默认 preset
- 不自动成为 board witness lane
- 不改变 live 文档默认口径

## 8. 当前 build graph

firmware 构建顺序：

1. `components/utils`
2. `components/protocol`
3. `platform/<vendor>/<soc>`
4. `kernel/freertos`
5. `subsys`
6. `components/algorithm`
7. `bsp`
8. `applications`
9. final firmware target

这个顺序表达的是当前 build ownership，不是目录展示。

## 9. 标准换芯路径

### 9.1 同 CPU-class 的 SoC 替换

标准命令：

```bash
cmake --preset fw-full-diag-carrier-debug -DTHEMIS_PLATFORM_SOC=<soc>
```

前提：

- `platform/<vendor>/<soc>/` 已存在
- 该目录带 `ThemisPlatformDefaults.cmake`
- 该 metadata 已绑定 CPU class

### 9.2 跨 CPU-class 的 SoC 替换

标准顺序：

1. 先补 `platform/cpu/<cpu-class>/ThemisCpuClassDefaults.cmake`
2. 再补 `platform/<vendor>/<soc>/ThemisPlatformDefaults.cmake`
3. 最后只切 `THEMIS_PLATFORM_SOC`

只有异常 bring-up 才回退到 `MCU_*` override。

## 9.3 第二阶段 root+helper 收口补充

第二阶段之后，root `CMakeLists.txt` 的正式理解进一步收口为：

- 声明 public build cache 入口
- 调用 helper 完成装配
- 挂接 layer manifest
- 组装最终 firmware / host tests

当前 root 不再直接持有下面这些实现细节：

- `THEMIS_BUILD_FIRMWARE` / `THEMIS_BUILD_HOST_TESTS` / `profile` / `flavor` 校验
- firmware entry / startup / linker / platform target 的最终派生
- contract target 的逐条创建
- configure 状态输出与 metadata lane 判定

第二阶段新增的正式内部 helper 为：

- `cmake/ThemisBuildAxes.cmake`
  - 负责 build 轴 cache 声明、合法性校验、profile/flavor 派生
- `cmake/ThemisFirmwareAssembly.cmake`
  - 负责 firmware-only metadata 装配与路径校验
- `cmake/ThemisContracts.cmake`
  - 负责 `themis_*_contract` target 创建与共享 compile policy
- `cmake/ThemisStatusReport.cmake`
  - 负责 host / firmware lane 的统一 configure 状态输出

这些 helper 的边界固定为“单一职责 internal owner”：

- 可以继续压薄 root
- 不能重新长成第二个 root
- 不对 leaf manifest 或脚本层承诺公共扩展 API

## 10. 当前验证入口

### 10.1 Host

```bash
cmake --preset host-debug
cmake --build --preset build-host-debug
ctest --preset test-host-debug
```

### 10.2 Firmware

```bash
cmake --preset fw-full-diag-carrier-debug
cmake --build --preset build-fw-full-diag-carrier-debug
```

### 10.3 CMake / platform gate

```bash
pwsh ./scripts/gates/check_platform_backend_contract.ps1 -RepoRoot <git-root>
pwsh ./scripts/gates/check_cmake_quality.ps1 -RepoRoot <git-root>
pwsh ./scripts/gates/check_cmake_smoke.ps1 -RepoRoot <git-root>
pwsh ./scripts/gates/check_themis_quality_gate.ps1 -RepoRoot <git-root> -Scope cmake
```

## 11. 一句话结论

**FalconFoc 当前正式 CMake 口径已经收口为：root 只装配，CPU class 管 core/FPU/RTOS，platform 管 SoC 真相，board 管板级真相，applications 管电机与产品真相。未来换芯的标准动作是补 owner，不是把特例塞回 shared layer。**
