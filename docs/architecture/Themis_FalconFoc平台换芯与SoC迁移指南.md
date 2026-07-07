# Themis FalconFoc 平台换芯与 SoC 迁移指南

## 1. 文档定位

本文是 FalconFoc 当前 live 仓库的正式迁移指南。

它不再把“换芯”当成一个问题，而是拆成 4 个必须分别判断的 owner 维度：

1. `CPU class`
2. `platform / SoC`
3. `board`
4. `motor / product`

只有先把这 4 件事拆开，迁移动作才会简单、直接、高效、安全、稳定、可靠。

## 2. 当前 live baseline

当前唯一 live baseline：

- SoC：`AT32F403A`
- CPU class：`cortex-m4f-hard`
- live platform owner：`platform/at/at32f403a`
- live board owner：`bsp/board`
- live control-plane module owner：`bsp/ports`
- live product owner：`applications/`

当前 prepared inactive：

- `platform/at/at32f415`
- `platform/at/at32f423`

仓库长期策略固定为：

- 一个 live baseline
- 多个 prepared inactive

不做多 live 并列长期维护。

## 3. 先判断迁移属于哪一类

### 3.1 CPU class 变化

关注点：

- `mcpu`
- `mfpu`
- `mfloat-abi`
- FreeRTOS `portable`
- `ARM_MATH_*`

当前正式 owner：

- `platform/cpu/<cpu-class>/ThemisCpuClassDefaults.cmake`

### 3.2 Platform / SoC 变化

关注点：

- startup
- linker
- vendor headers
- generated bring-up
- SoC timing seam
- exact part compile definitions

当前正式 owner：

- `platform/<vendor>/<soc>/`

### 3.3 Board 变化

关注点：

- 引脚
- 外设实例
- 采样拓扑
- 分压
- NTC
- flash / reset / console

当前正式 owner：

- `bsp/board`
- `bsp/ports`

当前边界补充：

- board/port 代码可以消费 platform 提供的 MCU / Flash 事实
- 这些事实必须来自 platform metadata 或 platform seam
- board/port 代码不得硬编码 live `AT32F403A` 头文件名

### 3.4 Motor / product 变化

关注点：

- 极对数
- Rs / Ls / flux / J
- 默认 PID
- 保护阈值
- 校准默认值

当前正式 owner：

- `applications/algo/foc_config.h`

本轮仍然不引入新的 motor-profile 框架。

## 4. 当前仓库的正式迁移口径

### 4.1 CPU class 不应该再靠 root 猜

现在的正式路径是：

- platform defaults 绑定 `THEMIS_PLATFORM_CPU_CLASS_DEFAULT`
- toolchain 先读 platform defaults，再读 CPU-class defaults
- kernel/freertos 从 CPU-class contract 读取 `portable` 路径

因此：

- root 不再写死 `cortex-m4`
- root 不再写死 `ARM_CM4F`
- root 不再写死 `fpv4-sp-d16`

### 4.2 SoC 真相必须留在 platform 目录

每个真实平台目录现在都应自带：

- `ThemisPlatformDefaults.cmake`
- `generated/inc/tm_platform_foc_timing.h`
- `CMakeLists.txt`
- `generated/`
- `vendor/`

platform metadata 还必须提供 board/port 会消费的 SoC 事实：

- `CMSIS_device_header`
- `THEMIS_PLATFORM_FLASH_DRIVER_HEADER`
- `THEMIS_PLATFORM_FLASH_BASE_ADDR`
- `THEMIS_PLATFORM_FLASH_SIZE_BYTES`
- `THEMIS_PLATFORM_FLASH_SECTOR_SIZE_BYTES`
- `THEMIS_PLATFORM_FLASH_SECTOR_COUNT`
- `THEMIS_PLATFORM_MOTOR_SOFTIRQ_HANDLER`
- `THEMIS_PLATFORM_MOTOR_SOFTIRQ_INSTANCE`

root 只负责选中平台，不再持有平台真相。

## 5. FalconFoc 下的三个典型迁移判断

### 5.1 `AT32F403A -> AT32F423`

这是：

- CPU class 不变
- platform / SoC 变化
- board 大概率变化
- motor / product 视实际电机与功率板决定

标准动作：

1. 补 `platform/at/at32f423`
2. 绑定 `cortex-m4f-hard`
3. 校准板级事实
4. 最后再看 `foc_config.h` 是否需要改产品默认值

### 5.2 `AT32F403A -> AT32F415`

在本仓当前正式 truth 下，这条迁移按你们本地已迁移成功的路线处理为：

- CPU class 变化
- platform / SoC 变化
- board 大概率变化
- motor / product 视实际电机与功率板决定

因此本仓正式口径是：

- `AT32F415` 绑定 `cortex-m3-soft`
- FreeRTOS 走 `ARM_CM3`
- 文档和 contract 以你们本地实迁 truth 为准

注意：

- 这条结论是 FalconFoc 本仓的交付 truth
- 不再让外部 marketing 口径覆盖本地实迁事实

### 5.3 非 AT vendor 迁移

标准顺序仍然一样：

1. 先确定 CPU class
2. 再补 `platform/<vendor>/<soc>`
3. 再改 board
4. 最后只在必要时改 motor / product defaults

不要直接从 app 层开始改。

## 6. 标准实施步骤

### Step 1：先判定 4 类 owner

先问清楚：

- CPU class 变没变
- platform / SoC 变没变
- board 变没变
- motor / product 变没变

这一步没做清楚，后面一定会越改越乱。

### Step 2：补 CPU-class owner

如果 CPU class 变化，先补：

- `platform/cpu/<cpu-class>/ThemisCpuClassDefaults.cmake`

里面只允许放：

- `mcpu`
- `mfpu`
- `mfloat-abi`
- FreeRTOS `portable`
- `ARM_MATH_*`

### Step 3：补 platform owner

再补：

- `platform/<vendor>/<soc>/`

最少要有：

- `ThemisPlatformDefaults.cmake`
- `CMakeLists.txt`
- `generated/inc/tm_platform_foc_timing.h`
- `generated/`
- `vendor/`

### Step 4：只用 `THEMIS_PLATFORM_SOC` 做标准试换

标准试换命令：

```bash
cmake --preset fw-full-diag-carrier-debug -DTHEMIS_PLATFORM_SOC=<soc>
```

标准路径不应再额外传：

- startup override
- linker override
- target override
- compile-definition override

这些 override 只保留给异常 bring-up 和诊断。

### Step 5：再改 board

如果板级变化，集中改：

- `bsp/board`
- `bsp/ports`

不要把板级差异塞回 shared layer。

### Step 6：最后才看 motor / product

如果电机、功率板、保护阈值真的变了，再改：

- `applications/algo/foc_config.h`

规则写死：

- CPU class 变化不得直接改 `foc_config.h`
- platform / SoC 变化不得直接改 shared layer 的产品逻辑

## 7. 当前 prepared inactive 的意义

当前 `at32f415` 与 `at32f423` 的仓库角色是：

- 准备好 metadata
- 准备好 vendor/generated 基础
- 准备好 timing seam
- 可以作为标准 `THEMIS_PLATFORM_SOC=<soc>` 试换入口

但它们暂时还不是：

- 默认 preset
- 正式 board witness lane
- live baseline

## 8. 与电机差异的边界

当前仓库不做多电机矩阵框架。

因此：

- 同芯片不同电机
  - 优先改 `foc_config.h`
  - 必要时补 board 事实
- 同电机不同芯片
  - 优先改 CPU class / platform / board
  - 不要先动 product 逻辑

只有当电机差异已经大到 `foc_config.h` 频繁分叉，才单独立项做 motor-profile owner。

## 9. 当前最小验收清单

### 9.1 Generic contract

```bash
pwsh ./scripts/gates/check_platform_backend_contract.ps1 -RepoRoot <git-root>
pwsh ./scripts/gates/check_cmake_quality.ps1 -RepoRoot <git-root>
pwsh ./scripts/gates/check_themis_quality_gate.ps1 -RepoRoot <git-root> -Scope cmake
```

### 9.2 Live baseline

```bash
cmake --preset host-debug
cmake --build --preset build-host-debug
ctest --preset test-host-debug

cmake --preset fw-full-diag-carrier-debug
cmake --build --preset build-fw-full-diag-carrier-debug
```

### 9.3 Prepared inactive smoke

后续拿到真实硬件时，标准 smoke 应该只需要：

```bash
cmake --preset fw-full-diag-carrier-debug -DTHEMIS_PLATFORM_SOC=at32f415
cmake --preset fw-full-diag-carrier-debug -DTHEMIS_PLATFORM_SOC=at32f423
```

不再需要手工拼 override 长串。

## 10. 一句话结论

**FalconFoc 今后的换芯标准动作不是“到处改代码”，而是按 `CPU class -> platform/SoC -> board -> motor/product` 的顺序补 owner。顺序对了，迁移就顺；顺序错了，再好的代码也会被架构噪声拖住。**
