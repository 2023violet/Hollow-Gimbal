# Themis FalconFoc CMake 构建策略与新产品落地规范

## 1. 文档定位

本文是 FalconFoc 当前 live 仓库的正式 CMake 与新产品落地手册。

它同时承担两类职责：

1. 长期规则
   - 说明 FalconFoc 的 CMake 到底应该怎么分层、怎么写、谁拥有哪类真相。
2. 施工流程
   - 说明新产品从 FalconFoc 出发时该怎么起步。
   - 说明同一产品线换芯时该怎么替换。

本文重点回答下面 8 个问题：

1. FalconFoc 为什么不能把不同产品都继续塞进同一个仓库里。
2. FalconFoc 为什么又允许在同一仓库里保留少量 prepared inactive 平台。
3. `root CMake`、`toolchain`、`platform/cpu`、`platform/<vendor>/<soc>`、`kernel/freertos`、`bsp`、`applications` 的正式 owner 是谁。
4. `work/` 与 `platform/<vendor>/<soc>/generated` 的正式主从关系是什么。
5. FreeRTOS `portable/GCC/*` 到底该由谁选，什么时候改，什么时候不该手工乱改。
6. 新产品应该什么时候新开仓库，什么时候不应该继续在当前仓库扩展。
7. 同一产品线换芯时，哪些文件需要替换，哪些文件应该尽量保持稳定。
8. FalconFoc 后续新增 CMake 写法时，哪些写法是正式允许的，哪些写法必须禁止。

本文不替代下面这些文档，而是与它们配套：

- `docs/architecture/Themis_CMake构建架构说明.md`
- `docs/architecture/Themis_FalconFoc平台换芯与SoC迁移指南.md`
- `docs/governance/Themis_CMake编写规范与质量门禁.md`

一句话定位：

> **如果 `Themis_CMake构建架构说明.md` 讲的是“当前构建 owner 是什么”，那么本文讲的是“以后 FalconFoc 的构建体系应该怎么长期使用、怎么新开产品、怎么换芯”。**

## 2. 正式裁决

FalconFoc 当前关于 CMake 和新产品落地的正式裁决固定为下面 4 条：

1. 新产品默认新开仓库，不在当前 FalconFoc 仓库里继续堆第二个真实产品。
2. 同一产品线内的换芯、试换、prepared inactive 平台，允许保留在当前仓库。
3. `work/` 是图形化工程母本，`platform/<vendor>/<soc>/generated` 是仓库内实际编译输入。
4. 换芯与构建迁移必须按 `CPU class -> platform/SoC -> board -> product` 的顺序处理，而不是上来就改应用层。

一句话结论：

**FalconFoc 的 CMake 目标不是“能编过就行”，而是把产品边界、芯片边界、板级边界、构建入口边界都写成明确 owner。**

## 3. FalconFoc 当前最重要的工程事实

### 3.1 当前仓库不是 generic 平台仓

FalconFoc 当前仓库是：

- 一个真实产品仓库
- 一个 live baseline 仓库
- 一个允许少量 prepared inactive 平台存在的产品仓库

它不是：

- 多产品共用平台总仓
- 把所有 MCU 与所有产品都放进来共存的大平台仓
- 用 `profile / flavor / override` 掩盖多个真实产品的聚合仓

### 3.2 当前仓库允许的 prepared inactive 是什么

当前仓库允许存在：

- `platform/cpu/<cpu-class>`
- `platform/<vendor>/<soc>`

这类 prepared inactive owner，目的只有两个：

1. 为同一产品线的换芯做准备。
2. 让标准试换路径可以在当前仓库内成立。

它们不代表：

- 第二个 live 产品
- 第二块长期并列维护的 live 板子
- 第二条正式 board witness lane

### 3.3 当前 live baseline 是什么

当前唯一 live baseline：

- product: `FalconFoc`
- vendor: `at`
- SoC: `at32f403a`
- CPU class: `cortex-m4f-hard`
- live board owner: `bsp/board`
- live control-plane module owner: `bsp/ports`

## 4. “新产品新仓库” 与 “同产品换芯留在当前仓库” 的正式分界

### 4.1 什么情况属于“新产品”

下面任一情况成立时，应优先判断为“新产品”，正式建议新开仓库：

- 图形化工程已经不是同一个项目母本。
- `work/` 不再是同一个 WorkBench 工程。
- 任务编排、Shell 词表、参数语义、产品闭环明显变化。
- 外设组合和产品边界已经不是 FalconFoc 当前语义。
- `applications/` 层已经不是“同一产品，只是换芯或换板”。

这类场景下，继续在当前仓库加：

- 新 preset
- 新 flavor
- 新 app 目录分叉
- 新 build 条件分支

只会让 root CMake 越来越臃肿。

### 4.2 什么情况属于“同产品换芯”

下面情况才属于“同产品换芯”，允许留在当前仓库：

- 还是 FalconFoc 这套产品。
- 产品语义基本不变。
- 变化主要集中在：
  - `CPU class`
  - `platform / SoC`
  - `board`
  - 少量时序或默认参数

例如：

- `AT32F403A -> AT32F423`
- `AT32F403A -> AT32F415`
- 同一 FalconFoc 产品在 AT 系列内试换

### 4.3 为什么必须这样分

因为 FalconFoc 当前最重要的不是“理论可扩展”，而是：

- 简单
- 直接
- 高效
- 安全
- 稳定
- 可靠

如果把不同真实产品塞进同一仓库，最后 root CMake 会被迫知道越来越多下面这些信息：

- 这个产品默认哪个 shell
- 那个产品默认哪个图形化工程
- 另一套产品的 startup/linker/板级逻辑
- 不同应用层的 flavor 分支

这会直接破坏“root 只装配，不持有产品真相”的原则。

## 5. 构建 owner 地图

### 5.1 root `CMakeLists.txt`

拥有：

- 正式构建轴入口
- build firmware / host tests 开关
- profile / flavor 入口
- `vendor / soc` 选择
- platform metadata 解析
- CPU-class metadata 解析
- contract target 创建
- final firmware 组装

不拥有：

- 某颗芯片的 startup / linker / exact part 真相
- 某个 CPU class 的 FreeRTOS portable 真相
- 产品业务语义
- 图形化工程母本语义

补充：

- root 通过 `cmake/ThemisPlatformMetadata.cmake` 统一装配 platform / CPU-class metadata
- root 继续是装配入口，不再自己维护第二套 metadata helper

### 5.1.1 `cmake/ThemisPlatformMetadata.cmake`

拥有：

- `THEMIS_PLATFORM_VENDOR` / `THEMIS_PLATFORM_SOC` 等标准入口 cache
- expert metadata override cache
- root 与 toolchain 共用的 platform / CPU-class metadata 装配逻辑
- `THEMIS_CPU_CLASS_OVERRIDE > platform default` 优先级

不拥有：

- source manifest
- build preset 策略
- 产品 feature policy

### 5.2 `tools/cmake/arm-gcc-toolchain.cmake`

拥有：

- 交叉编译身份
- `arm-none-eabi-*` 工具链查找
- 在 `project()` 前解析 platform 与 CPU-class metadata
- `mcpu / mfpu / mfloat-abi` baseline

不拥有：

- source manifest
- preset 策略
- 产品 feature policy
- 板级或应用层语义

补充：

- toolchain 与 root 共用 `cmake/ThemisPlatformMetadata.cmake`
- `MCU_CPU` / `MCU_FPU` / `MCU_FLOAT` 继续保留，但只作 emergency/debug override

### 5.3 `platform/cpu/<cpu-class>/ThemisCpuClassDefaults.cmake`

拥有：

- `mcpu`
- `mfpu`
- `mfloat-abi`
- FreeRTOS `portable/GCC/*` 路径
- `ARM_MATH_*`

不拥有：

- vendor 头
- startup / linker
- board 真值
- product 真值

### 5.4 `platform/<vendor>/<soc>/ThemisPlatformDefaults.cmake`

拥有：

- default target
- entry source
- startup source
- linker script
- compile definitions
- `THEMIS_PLATFORM_CPU_CLASS_DEFAULT`
- board-visible SoC facts

不拥有：

- FreeRTOS CPU-class 通用逻辑
- 产品 shell 策略
- 电机参数与产品语义

### 5.5 `platform/<vendor>/<soc>/CMakeLists.txt`

拥有：

- 平台 target 的 source manifest
- generated 与 vendor 源文件的正式编译面
- platform include root

不拥有：

- final firmware owner
- board owner
- application owner

### 5.6 `kernel/freertos/CMakeLists.txt`

拥有：

- FreeRTOS 核心源文件编译
- heap backend 选择
- CMSIS-RTOS2 bridge
- 通过 CPU-class contract 选择 `portable/GCC/*`

不拥有：

- 决定当前 MCU 是 `ARM_CM3` 还是 `ARM_CM4F`
- 硬编码具体内核 port

### 5.7 `work/`

拥有：

- 图形化工程母本
- 图形化工具原生工程结构
- 生成代码的上游输入工程

不拥有：

- FalconFoc live firmware 的正式 CMake 入口
- 平台 target owner
- 仓库内正式平台 metadata

### 5.8 `platform/<vendor>/<soc>/generated`

拥有：

- 仓库内正式编译输入的 generated 代码
- `wk_*`
- 中断向量辅助源
- entry source

不拥有：

- 图形化工程母本管理权
- 产品业务语义

### 5.9 `run.sh` 与 `scripts/lib/themis_firmware_cli.sh`

拥有：

- 人类和 CI 的 firmware CLI 包装入口
- `THEMIS_PLATFORM_VENDOR` / `THEMIS_PLATFORM_SOC` / `THEMIS_DEVDRV_PROFILE` / `THEMIS_PRODUCT_FLAVOR` 等环境变量桥接
- configure/build/flash/status/memory 的统一命令面

不拥有：

- 平台真相
- CPU class 真相
- source manifest owner

补充：

- `run.sh` 是参数化包装器，不是架构 owner
- 日常 build/flash 可以走 `run.sh`
- 人工/CI 的正式 configure smoke 与契约验证优先走 preset lane

## 6. `work/` 与 `platform/<vendor>/<soc>/generated` 的正式主从关系

### 6.1 正式关系

当前正式关系固定为：

- `work/`
  - 图形化工程母本
  - source of generation intent
- `platform/<vendor>/<soc>/generated`
  - 仓库内正式编译输入
  - source of repository build consumption

一句话：

> **`work/` 负责“上游怎么生成”，`platform/.../generated` 负责“仓库实际编什么”。**

### 6.2 为什么不能把它们当成双独立真相

因为一旦把两者都当成平级真相，就会出现：

- `work` 改了但仓库没同步
- `generated` 手工改了但回不到图形化母本
- root CMake / platform manifest 不知道该信哪一份
- 后续换芯或新产品复制时，把脏状态也一起带走

所以必须写死主从关系，而不是默契理解。

### 6.3 正式同步方向

正式同步方向应固定为：

`work/` 图形化工程更新  
-> 导出或整理生成代码  
-> 同步到 `platform/<vendor>/<soc>/generated`  
-> 通过平台 target 与 firmware 构建验证  

禁止把下面这条反向流程当成长期正式路径：

`platform/.../generated` 手工长期演化  
-> 期待以后再回填 `work/`

短期紧急修补可以存在，但不能作为正式日常流程。

### 6.4 当前 `work/CMakeLists.txt` 的正式理解

`work/CMakeLists.txt` 当前只应被理解为：

- 图形化工程自带的原生 CMake 工程入口

它不是：

- FalconFoc 仓库 live firmware 的正式入口
- root CMake 的上级 owner
- 新产品长期构建宪法的 owner

## 7. CPU class 与 FreeRTOS portable 的正式口径

### 7.1 CPU class 不是“芯片顺带信息”

FalconFoc 当前正式要求把下面这些东西看成 `CPU class` owner：

- `mcpu`
- `mfpu`
- `mfloat-abi`
- FreeRTOS `portable/GCC/*`
- `ARM_MATH_*`

原因很简单：

- 同一厂家不同 SoC，CPU class 可能不同。
- 同一厂家不同 SoC，FreeRTOS `portable` 也可能不同。
- 这些真相不应该塞回 root CMake，也不应该散落在 `kernel/freertos` 里猜。

### 7.2 FreeRTOS portable 到底由谁选

当前正式 owner：

- `platform/cpu/<cpu-class>/ThemisCpuClassDefaults.cmake`

例如：

- `cortex-m4f-hard`
  - `portable/GCC/ARM_CM4F`
- `cortex-m3-soft`
  - `portable/GCC/ARM_CM3`

### 7.3 正式禁止事项

后续正式禁止下面这些做法：

1. 直接去 `kernel/freertos/CMakeLists.txt` 改死某个 `ARM_CM4F` 或 `ARM_CM3`。
2. 在 root CMake 写死 `cortex-m4`、`fpv4-sp-d16` 之类字面真相。
3. 用 `MCU_CPU`、`MCU_FPU`、`MCU_FLOAT` 当日常标准入口。

`MCU_*` 继续保留，但只作为：

- 异常 bring-up
- 诊断
- 紧急 override

不作为正式换芯路径。

## 8. 正式入口关系

### 8.1 标准日常入口

日常正式入口只有两类：

1. `CMakePresets.json`
2. `run.sh` / `memory.sh`

补充规则：

- 人工与 CI 的正式 configure 验证优先走 `preset`
- `run.sh` 保留为参数化 firmware CLI 包装器
- 裸 `cmake -S/-B -DCMAKE_TOOLCHAIN_FILE=...` 只视为 expert 排障入口，不作为正式验证口径

### 8.2 root CMake 的正式 cache 入口

当前正式入口包括：

- `THEMIS_BUILD_FIRMWARE`
- `THEMIS_BUILD_HOST_TESTS`
- `THEMIS_DEVDRV_PROFILE`
- `THEMIS_PRODUCT_FLAVOR`
- `THEMIS_PLATFORM_VENDOR`
- `THEMIS_PLATFORM_SOC`
- `THEMIS_PLATFORM_DIR_OVERRIDE`
- `THEMIS_PLATFORM_TARGET_OVERRIDE`
- `THEMIS_ENTRY_SOURCE_OVERRIDE`
- `THEMIS_STARTUP_SOURCE_OVERRIDE`
- `THEMIS_LINKER_SCRIPT_OVERRIDE`
- `THEMIS_PLATFORM_COMPILE_DEFINITIONS_OVERRIDE`
- `THEMIS_CPU_CLASS_OVERRIDE`

### 8.3 `run.sh` 的正式理解

`run.sh` 不重新定义平台真相，它只是把下面这些正式变量桥接给 root CMake：

- `THEMIS_PLATFORM_VENDOR`
- `THEMIS_PLATFORM_SOC`
- `THEMIS_DEVDRV_PROFILE`
- `THEMIS_PRODUCT_FLAVOR`
- `THEMIS_ENABLE_BOARD_BENCH`
- `THEMIS_FIRMWARE_TARGET_NAME`

所以：

> **`run.sh` 是调用入口，不是架构 owner。**

prepared inactive 的正式 smoke 路径固定为：

```bash
cmake --preset fw-full-diag-carrier-debug -DTHEMIS_PLATFORM_SOC=at32f415
cmake --preset fw-full-diag-carrier-debug -DTHEMIS_PLATFORM_SOC=at32f423
```

## 9. 新增 CMake 写法时的正式原则

### 9.1 root 只装配，不吞真相

root CMake 只负责：

- 选择
- 解析
- 装配

不负责：

- 持有芯片私有事实
- 持有应用层产品事实

### 9.2 toolchain 只管 toolchain

toolchain 文件只允许拥有：

- 交叉编译工具链
- 架构 flags
- find policy
- 在 `project()` 之前必须建立的 CPU-class baseline

不允许把 source manifest、产品策略、平台 source 列表放进去。

### 9.3 platform metadata 必须是单平台真相

每个 `platform/<vendor>/<soc>/ThemisPlatformDefaults.cmake` 必须只回答：

- 这颗 SoC 的默认 target 是什么
- entry/startup/linker 是什么
- compile definitions 是什么
- 它绑定哪个 CPU class

不允许把产品层策略塞进来。

### 9.4 不要用 flavor / profile 藏第二个真实产品

`profile` 的作用是：

- 资源档位
- 诊断档位

`flavor` 的作用是：

- 同一产品内的少量正式变体

它们不应该用来承载：

- 第二个真实产品
- 第二套业务闭环
- 第二个图形化工程母本

## 10. 新产品正式起步流程

### 10.1 总原则

新产品正式流程：

> **从 FalconFoc 仓库派生新仓库，而不是继续把新产品加进当前仓库。**

### 10.2 Step 1：复制仓库并先收口产品边界

新产品第一步不是改 CMake，而是先明确：

- 新产品名称
- live baseline 芯片
- CPU class
- 板级外设组合
- 是否还属于 FalconFoc 产品语义

如果已经不是 FalconFoc 语义，就不要继续在当前仓库内做 flavor 扩展。

### 10.3 Step 2：替换 `work/`

用新产品的图形化工程替换：

- `work/`

要求：

- `work/` 必须始终保持为该新产品的图形化母本
- 不要沿用旧产品的图形化工程再硬改

### 10.4 Step 3：建立或替换 live platform owner

在新仓库里建立：

- `platform/<vendor>/<soc>/`

最少包括：

- `CMakeLists.txt`
- `ThemisPlatformDefaults.cmake`
- `generated/`
- `vendor/`

其中：

- `generated/` 来自新 `work/` 的正式同步结果
- `vendor/` 来自该 SoC 对应的厂商基线

### 10.5 Step 4：确认 CPU class owner

如果新产品 SoC 属于仓库里已有 CPU class：

- 直接复用 `platform/cpu/<cpu-class>/`

如果不属于：

- 新增 `platform/cpu/<cpu-class>/ThemisCpuClassDefaults.cmake`

这里必须明确写出：

- `mcpu`
- `mfpu`
- `mfloat-abi`
- FreeRTOS `portable/GCC/*`
- `ARM_MATH_*`

### 10.6 Step 5：更新 platform defaults

在 `ThemisPlatformDefaults.cmake` 中正式绑定：

- target name
- entry source
- startup source
- linker script
- compile definitions
- `THEMIS_PLATFORM_CPU_CLASS_DEFAULT`

这里是新产品 live SoC 的正式 build metadata owner。

### 10.7 Step 6：再改 `bsp/board` 与 `bsp/ports`

只在 platform 和 CPU class 就位后，再改：

- 板级引脚
- 外设实例
- `bsp_config.h`
- `bsp/board`
- `bsp/ports`

### 10.8 Step 7：最后才改产品层

最后才处理：

- `applications/`
- 电机参数
- shell 词表
- 参数策略
- 板级业务语义

这样可以避免“图还没搭好，先从 app 层硬改”的混乱路线。

### 10.9 Step 8：整理 preset 与 CLI 默认值

新仓库确定 live baseline 后，更新：

- `CMakePresets.json`
- `run.sh`
- `scripts/lib/themis_firmware_cli.sh`

重点只更新默认：

- `THEMIS_PLATFORM_VENDOR`
- `THEMIS_PLATFORM_SOC`
- `THEMIS_FIRMWARE_TARGET_NAME`
- 需要保留的 profile / flavor

### 10.10 Step 9：完成最小验证

至少完成：

```bash
cmake --preset host-debug
cmake --build --preset build-host-debug
ctest --preset test-host-debug

cmake --preset fw-full-diag-carrier-debug
cmake --build --preset build-fw-full-diag-carrier-debug
```

以及：

```powershell
pwsh ./scripts/gates/check_platform_backend_contract.ps1 -RepoRoot <repo-root>
pwsh ./scripts/gates/check_cmake_quality.ps1 -RepoRoot <repo-root>
pwsh ./scripts/gates/check_themis_quality_gate.ps1 -RepoRoot <repo-root> -Scope cmake
```

## 11. 同一产品线换芯的正式流程

### 11.1 Step 1：先分类，不要先改代码

先判断变化落在哪几类：

- `CPU class`
- `platform / SoC`
- `board`
- `product`

### 11.2 Step 2：如果 CPU class 变化，先补 CPU-class owner

只有先确定：

- `mcpu`
- `mfpu`
- `mfloat-abi`
- FreeRTOS `portable/GCC/*`

后面才不会越改越乱。

### 11.3 Step 3：再补 `platform/<vendor>/<soc>`

新增或完善：

- `ThemisPlatformDefaults.cmake`
- `CMakeLists.txt`
- `generated/`
- `vendor/`

### 11.4 Step 4：从 `work/` 同步 generated

如果该 SoC 对应图形化工程已经准备好：

- 从新 `work/` 或对应母本同步到 `platform/<vendor>/<soc>/generated`

这里要明确：

- `work/` 是上游母本
- `platform/.../generated` 是仓库编译输入

### 11.5 Step 5：只用标准入口试换

标准试换命令例如：

```bash
cmake --preset fw-full-diag-carrier-debug -DTHEMIS_PLATFORM_SOC=at32f415
cmake --preset fw-full-diag-carrier-debug -DTHEMIS_PLATFORM_SOC=at32f423
```

不要先走一长串：

- `startup override`
- `linker override`
- `manual FreeRTOS port patch`
- `root CMake hardcode`

### 11.6 Step 6：再改 board

如果换芯带来板级变化，再改：

- `bsp/board`
- `bsp/ports`
- `bsp_config.h`

### 11.7 Step 7：最后才碰产品参数

只有当电机、功率板、保护阈值真的变化时，才改：

- `applications/algo/foc_config.h`

不要把 CPU class 或 SoC 差异直接改写成产品层逻辑。

## 11.8 第二阶段 root 瘦身补充规则

第二阶段之后，FalconFoc 的 root `CMakeLists.txt` 只保留下面 5 类职责：

- `project()` 与 module path 初始化
- public cache 入口声明
- helper 调用顺序
- `add_subdirectory()` 挂接
- firmware / host tests 最终组装

当前 root 正式不再直接持有：

- build 轴合法性校验
- profile / flavor 到 compile definition 的派生
- firmware entry / startup / linker / platform target 的最终解析
- contract target 创建细节
- configure 状态输出

第二阶段新增 4 个 internal helper：

- `cmake/ThemisBuildAxes.cmake`
- `cmake/ThemisFirmwareAssembly.cmake`
- `cmake/ThemisContracts.cmake`
- `cmake/ThemisStatusReport.cmake`

这 4 个 helper 的正式理解是：

- 用来继续压薄 root
- 用来固定 owner 边界
- 不构成新的公共扩展层
- 不允许重新合并成 mega-helper

第二阶段继续保持的外部兼容规则：

- 不改已有 preset 名
- 不删已有 public cache 入口
- `run.sh` 继续只是参数化包装器
- prepared inactive SoC 继续走 preset-first configure smoke 验收

## 12. 当前正式禁止事项

后续 FalconFoc 的 CMake 写法，正式禁止下面这些路线：

1. 把第二个真实产品继续塞进当前仓库。
2. 用 `profile / flavor` 冒充第二个真实产品。
3. 把 `work/` 与 `platform/.../generated` 当双独立真相长期并存。
4. 在 root CMake 写死 startup / linker / exact part / FreeRTOS port。
5. 直接改 `kernel/freertos/CMakeLists.txt` 来切 `ARM_CM3` 或 `ARM_CM4F`。
6. 把 `MCU_CPU` / `MCU_FPU` / `MCU_FLOAT` 当成标准换芯入口。
7. 让 `run.sh` 或脚本层反向持有平台真相。
8. 让 prepared inactive 平台悄悄变成第二个 live baseline。
9. 把 platform truth 推回 shared layer 或 applications。
10. 为了少改几个文件，把本该新开仓库的新产品继续堆进当前仓库。
11. 把第二阶段 helper 又重新塞回 root。
12. 让 build-axis / firmware-assembly / contracts / status-report 四类职责重新混写。

## 13. 当前最小验收口径

### 13.1 对文档与写法的验收

至少要求：

- root CMake 只装配，不持有 SoC 私有事实
- CPU class owner 明确持有 FreeRTOS portable
- platform metadata 明确绑定 CPU class
- `work` 与 `generated` 的主从关系明确
- 新产品流程与换芯流程分开表述

### 13.2 对实际构建的验收

至少要求下面几条命令仍成立：

```bash
cmake --preset host-debug
cmake --build --preset build-host-debug
ctest --preset test-host-debug
```

```bash
cmake --preset fw-full-diag-carrier-debug
cmake --build --preset build-fw-full-diag-carrier-debug
```

```bash
cmake --preset fw-full-diag-carrier-debug -DTHEMIS_PLATFORM_SOC=at32f415
cmake --preset fw-full-diag-carrier-debug -DTHEMIS_PLATFORM_SOC=at32f423
```

```powershell
pwsh ./scripts/gates/check_platform_backend_contract.ps1 -RepoRoot <repo-root>
pwsh ./scripts/gates/check_cmake_quality.ps1 -RepoRoot <repo-root>
pwsh ./scripts/gates/check_cmake_smoke.ps1 -RepoRoot <repo-root>
pwsh ./scripts/gates/check_themis_quality_gate.ps1 -RepoRoot <repo-root> -Scope cmake
```

## 14. 最终口径

FalconFoc 当前关于 CMake 和新产品落地的正式口径可以压成下面 7 条：

1. FalconFoc 是产品仓，不是多产品平台总仓。
2. 新产品默认新开仓库，不继续塞进当前仓库。
3. 同一产品线换芯允许在当前仓库保留 prepared inactive 平台。
4. `work/` 是图形化母本，`platform/<vendor>/<soc>/generated` 是仓库内正式编译输入。
5. CPU class owner 负责 `mcpu / mfpu / mfloat-abi / FreeRTOS portable / ARM_MATH_*`。
6. platform owner 负责 SoC build metadata，board owner 负责板级真相，product owner 负责产品语义。
7. 所有换芯动作都必须按 `CPU class -> platform/SoC -> board -> product` 的顺序推进。

一句话总结：

> **FalconFoc 的 CMake 不是为了把一切都塞进同一个仓库，而是为了让“新产品怎么起、同产品怎么换芯、图形化工程怎么同步、谁应该改什么”都有一条长期稳定的正式路线。**
