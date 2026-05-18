# Themis CMake构建架构说明

## 1. 文档目标

本文固定 FalconFoc 当前 live CMake 构建口径，只描述当前仓库真实存在的构建轴、平台入口、contract target 和验证入口。

## 2. 正式入口

### 2.1 顶层 `CMakeLists.txt`

职责：

- 定义 firmware / host-test 开关
- 定义 profile / flavor 构建轴
- 定义 platform 入口与 override 入口
- 创建 contract target
- 组装最终 firmware 与 host tests

### 2.2 `cmake/ThemisBuild.cmake`

职责：

- 提供统一 helper / DSL
- 固定 module / interface-module / host-test / firmware 的声明方式
- 让子层 manifest 保持 declarative

当前正式 helper：

- `themis_add_module()`
- `themis_add_interface_module()`
- `themis_add_host_test()`
- `themis_add_firmware()`

### 2.3 `tools/cmake/arm-gcc-toolchain.cmake`

职责：

- 定义 ARM bare-metal 交叉编译身份
- 查找 `arm-none-eabi-*`
- 导出 MCU / FPU / ABI 基线 flags

## 3. 当前正式构建轴

### 3.1 产物类型

- `THEMIS_BUILD_FIRMWARE=ON`
  - 生成 MCU firmware ELF / HEX / BIN / MAP
- `THEMIS_BUILD_HOST_TESTS=ON`
  - 生成 native host tests 与 `ctest`

### 3.2 资源轴

- `THEMIS_DEVDRV_PROFILE`
  - `tiny`
  - `standard`
  - `full-diag`
- `THEMIS_PRODUCT_FLAVOR`
  - `carrier-demo`
  - `headless-tiny`

这两条轴共同决定：

- 功能裁剪
- 资源预算
- 默认验证口径

## 4. 平台解析模型

### 4.1 source of truth

顶层当前对外暴露：

- `THEMIS_PLATFORM_VENDOR`
- `THEMIS_PLATFORM_SOC`
- `THEMIS_PLATFORM_DIR_OVERRIDE`
- `THEMIS_PLATFORM_TARGET_OVERRIDE`
- `THEMIS_ENTRY_SOURCE_OVERRIDE`
- `THEMIS_STARTUP_SOURCE_OVERRIDE`
- `THEMIS_LINKER_SCRIPT_OVERRIDE`

### 4.2 解析结果

每次 configure 重新解析：

- `THEMIS_PLATFORM_DIR`
- `THEMIS_PLATFORM_TARGET`
- `THEMIS_ENTRY_SOURCE`
- `THEMIS_STARTUP_SOURCE`
- `THEMIS_LINKER_SCRIPT`

正式规则：

- 默认由 `vendor/soc` 推导
- 只有显式 `*_OVERRIDE` 才偏离默认
- 派生结果不单独写入 `CACHE`

### 4.3 当前正式默认值

- `THEMIS_PLATFORM_VENDOR=at`
- `THEMIS_PLATFORM_SOC=at32f403a`
- `THEMIS_PLATFORM_DIR=platform/at/at32f403a`
- `THEMIS_PLATFORM_TARGET=at32f403a-hal`
- `THEMIS_FIRMWARE_TARGET_NAME=falcon-foc-at32`

## 5. Contract Target

当前正式提供：

- `themis_root_headers`
  - 统一导出 `include/` 与 `include/config/`
- `themis_build_contract`
  - 统一导出语言标准、warning 基线、profile/flavor 语义
- `themis_platform_contract`
  - 统一导出平台 compile definitions
- `themis_host_test_contract`
  - 统一导出 host-test 口径

一句话理解：

**子模块只声明本层事实，contract target 负责统一全局语义。**

## 6. 当前 live build graph

当前 firmware 构建顺序为：

1. `components/utils`
2. `components/protocol`
3. `platform/<vendor>/<soc>`
4. `kernel/freertos`
5. `subsys`
6. `components/algorithm`
7. `bsp`
8. `applications`
9. final firmware target

这不是目录展示，而是当前 FalconFoc-lite 的正式 build ownership。

## 7. 当前 `subsys` 口径

当前 live 产物只要求：

- `subsys-core`
  - framework 核心机制
- `subsys`
  - 顶层编译聚合入口

本仓库当前不再把退役的 `device/`、`drivers/`、以及相关依赖链视为 live 构建面。

## 8. 目标命名

当前正式 canonical target：

- `at32f403a-hal`
- `subsys-core`
- `falcon-foc-at32`

文档、脚本、验证口径都必须使用这些 live 名称，不再写旧目标名。

## 9. Preset 入口

当前正式 configure preset：

- `host-debug`
- `host-relwithdebinfo`
- `fw-tiny-carrier-minsizerel`
- `fw-tiny-headless-minsizerel`
- `fw-standard-carrier-minsizerel`
- `fw-full-diag-carrier-debug`

正式要求：

- build tree 固定落在 `${sourceDir}/build/<preset>`
- host preset 显式关闭 firmware，打开 host tests
- firmware preset 显式打开 firmware，关闭 host tests，并声明 toolchain

## 10. 固件产物合同

当前正式 firmware basename：

- `falcon-foc-at32`

因此当前正式产物路径口径为：

- `build/<preset>/output/falcon-foc-at32.elf`
- `build/<preset>/output/falcon-foc-at32.hex`
- `build/<preset>/output/falcon-foc-at32.bin`
- `build/<preset>/output/falcon-foc-at32.map`

## 11. 当前正式验证入口

### 11.1 Host 验证

```bash
cmake --preset host-debug
cmake --build --preset build-host-debug
ctest --preset test-host-debug
```

### 11.2 Firmware 构建验证

```bash
cmake --preset fw-tiny-carrier-minsizerel
cmake --build --preset build-fw-tiny-carrier-minsizerel
```

### 11.3 构建系统质量门

```bash
pwsh ./scripts/check_cmake_quality.ps1 -RepoRoot <git-root>
pwsh ./scripts/check_platform_backend_contract.ps1 -RepoRoot <git-root>
pwsh ./scripts/check_themis_quality_gate.ps1 -RepoRoot <git-root> -Scope cmake
```

需要摘要时继续使用：

```bash
pwsh ./scripts/check_themis_quality_gate.ps1 -RepoRoot <git-root> -Scope all -Summary
```

## 12. 一句话结论

**FalconFoc 当前的正式 CMake 架构已经收口为：preset 驱动、contract 收口、platform 可替换、FalconFoc-lite build graph 固定，并由质量门脚本持续守住。**
