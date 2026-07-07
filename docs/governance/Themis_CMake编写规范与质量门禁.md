# Themis CMake编写规范与质量门禁

## 1. 文档目标

这份文档用于把 Themis 当前的 CMake 写法、命名契约、注释视角、模板目录要求和质量门禁入口统一下来。

正式结论先写在最前面：

**Themis 的 CMake 不是“把代码拼起来能编过”就算完成，而是正式架构的一部分。**

它必须同时满足四件事：

- 能表达真实 ownership，而不是靠目录直觉和历史惯性。
- 能把 platform / backend / profile / flavor 这些构建轴稳定收口。
- 能让后来人从文件名、target 名和注释直接看懂为什么这么连。
- 能被脚本和门禁自动检查，而不是靠 review 时人工回忆。

## 2. 正式文件角色

### 2.1 顶层入口

代表文件：

- `CMakeLists.txt`

职责：

- 定义正式构建轴与默认值
- 暴露 override 入口
- 创建 contract target
- 组装分层 build graph
- 生成最终 firmware / host tests

禁止事项：

- 不把子模块 source 细节写回顶层
- 不在这里写板级业务逻辑
- 不把 vendor/backend 事实重新硬编码到别处

### 2.2 Helper / DSL

代表文件：

- `cmake/ThemisBuild.cmake`

职责：

- 统一路径解析
- 统一 module / interface-module / host-test / firmware 声明方式
- 降低子层样板噪音

禁止事项：

- 不承载产品策略
- 不硬编码特定 platform/backend
- 不把 leaf manifest 的 ownership 判断偷回 helper 层

### 2.3 Toolchain

代表文件：

- `tools/cmake/arm-gcc-toolchain.cmake`

职责：

- 定义交叉编译身份
- 查找 `arm-none-eabi-*`
- 建立 MCU 与链接基线 flags

禁止事项：

- 不承载模块依赖关系
- 不承载 profile / flavor 策略
- 不承载产品文件清单

### 2.4 子层 manifest

代表文件：

- `applications/CMakeLists.txt`
- `components/*/CMakeLists.txt`
- `device/CMakeLists.txt`
- `drivers/*/CMakeLists.txt`
- `platform/*/CMakeLists.txt`
- `subsys/CMakeLists.txt`
- `tests/CMakeLists.txt`

职责：

- 写清 source ownership
- 写清 public/private include 边界
- 写清真实 public/private 依赖

禁止事项：

- 不重复声明语言标准、全局 warning policy、toolchain 语义
- 不把“顺手能编过”的 include 泄露成正式 public surface

## 3. 零容忍规则

以下问题一律按缺陷处理，不接受“先这样”“以后再说”：

- 历史残留命名与现实不一致
  - 例如旧项目名、旧芯片名、旧 target 名继续出现在正式入口、正式脚本、正式文档里。
- 注释描述的是历史而不是当前设计
  - 例如“未来支持多后端”，但代码已经支持。
- 派生路径被独立缓存
  - `THEMIS_PLATFORM_DIR`、`THEMIS_ENTRY_SOURCE` 这类值如果本质上由 vendor/soc 推导，就不能再独立 `CACHE`，否则切换平台后会产生陈旧缓存穿透。
- public include 通过 `../` 泄露别层目录
  - 这会让依赖方向失真，长期一定演化成循环依赖和“哪里都能 include”的污染。
- 用自动搜子目录结果来定义正式 public surface
  - `themis_collect_subdirectories()` 可以作为工具存在，但正式 public include 列表必须显式写出。

## 4. 命名契约

### 4.1 变量

- CMake 变量统一使用 `THEMIS_` 前缀 + `SNAKE_CASE`
- 派生值和 override 值必须分开命名
  - 正式推荐：`THEMIS_PLATFORM_DIR_OVERRIDE`
  - 正式解析结果：`THEMIS_PLATFORM_DIR`

### 4.2 Target

- target 统一使用 `kebab-case`
- 单词 target 保持小写即可
- 当前正式多词 target 示例：
  - `themis-tec-at32`
  - `at32f423-hal`
  - `at-drivers`
  - `subsys-core`
  - `subsys-shared`

### 4.3 宏 / 函数

- helper 统一使用 `themis_` 前缀 + `snake_case`
- 新 helper 只有在能降低重复、提升一致性、且确实能被多处复用时才允许引入

### 4.4 兼容别名

- 允许为历史 target 提供临时 alias / compat target
- 兼容别名只用于迁移缓冲，不得再作为正式文档和正式脚本的主名称
- 正式文档只写 canonical 名

## 5. 构建轴与派生规则

### 5.1 正式 source of truth

下面这些值才是“用户应该改”的正式入口：

- `THEMIS_BUILD_FIRMWARE`
- `THEMIS_BUILD_HOST_TESTS`
- `THEMIS_DEVDRV_PROFILE`
- `THEMIS_PRODUCT_FLAVOR`
- `THEMIS_PLATFORM_VENDOR`
- `THEMIS_PLATFORM_SOC`
- `THEMIS_FIRMWARE_TARGET_NAME`
- 各类 `*_OVERRIDE`

### 5.2 正式派生值

下面这些值属于顶层解析结果，而不是独立配置入口：

- `THEMIS_PLATFORM_DIR`
- `THEMIS_PLATFORM_TARGET`
- `THEMIS_DRIVER_BACKEND_DIR`
- `THEMIS_DRIVER_BACKEND_TARGET`
- `THEMIS_ENTRY_SOURCE`
- `THEMIS_STARTUP_SOURCE`
- `THEMIS_LINKER_SCRIPT`

规则：

- 默认情况下由 vendor/soc 推导
- 只有显式提供 `*_OVERRIDE` 时才偏离默认
- 顶层必须在同一轮 configure 中重新计算，而不是沿用旧 cache

## 6. 依赖边界规则

### 6.1 `PUBLIC / PRIVATE` 必须诚实

判断标准不是“经常一起用”，而是：

- public header 是否直接暴露对方头文件、类型、宏或 ABI 语义

因此：

- public header 暴露到的依赖必须 `PUBLIC`
- 只在 `.c` 内部使用的依赖必须 `PRIVATE`

### 6.2 不允许 public include 泄露上层目录

禁止写法：

- `PUBLIC_INCLUDE_DIRS ../device`
- `PUBLIC_INCLUDE_DIRS ../device/flash`

正确做法：

- 让本层 target 直接 `PUBLIC_DEPS device`
- 由 `device` 自己导出它的 public include 目录

### 6.3 正式解环策略

遇到循环依赖时，优先用“拆 target、保 source tree”的办法解：

- 先拆出更底层、语义更纯的 target
- 再把 board-facing / product-facing / service-facing 部分放到上层 target

本仓库当前正式案例：

- `subsys-core`
  - 只承载 framework 核心合同与基础机制
- `subsys-shared`
  - 承载会触碰 `bsp` / `device` / `display` 等外部边界的共享服务
- `bsp`
  - 只依赖 `subsys-core`

这样可以避免：

- `bsp -> subsys`
- `subsys -> bsp`

形成互锁循环。

## 7. DSL 使用规则

当前正式 helper：

- `themis_add_module()`
- `themis_add_interface_module()`
- `themis_add_host_test()`
- `themis_add_firmware()`

使用要求：

- `themis_add_module()` 用于有真实 source 的 target
- `themis_add_interface_module()` 用于纯 contract / umbrella / template skeleton
- 子层 manifest 只写 source、include、依赖、必要局部 compile 语义
- 不要在 leaf manifest 里重新写全局 warning policy 或 toolchain 决策

### 7.1 host-test manifest 补充规则

`themis_add_host_test()` 不是“把测试二进制拼起来”的便捷入口，而是 host 白盒验证合同的一部分。

规则：

- 如果测试 target 直接编译产品 `.c`，manifest 必须显式补齐该实现文件真实依赖到的 include root，不能赌其它 target 的 `PUBLIC_INCLUDE_DIRS` 会顺手泄露进来。
- 如果测试文件直接 `#include "../../../applications/services/foo.c"` 这类实现文件，manifest 仍然要把 `applications`、`applications/config`、相邻 service 目录、以及该实现经由头文件继续依赖到的目录显式写清楚。
- 不要把“缺 include 就继续往上层目录开口子”当成修法；优先补真实缺失的 product/config/include 边界。
- host-test manifest 的职责是复刻白盒验证所需的最小编译面，而不是重新发明一个更宽松的 product build graph。

## 8. 模板目录要求

模板目录是给未来平台和未来 backend 抄作业的地方，所以要求比普通目录更严格。

正式模板目录：

- `drivers/template/`
- `platform/template/cortex_m4_ref/`

必须满足：

- 目标名明确、稳定、可直接写进文档
- README 里写清“这个 target 必须导出什么合同”
- 注释说明哪里能加源文件，哪里不能塞产品策略
- 先固定 ownership 和目录边界，再谈第二平台真正落地

## 9. 注释规则

### 9.1 统一头注释

每个 Themis 自有 CMake 文件都必须有统一头注释。

最低要求：

- 第一段非空行以 `# Themis ...` 开头
- 必须包含 `Role:`
- 必须写出 2 到 3 条职责

### 9.2 注释只解释“为什么”

不合格：

```cmake
# 设置编译选项
target_compile_options(foo PRIVATE -Wall)
```

合格：

```cmake
# 这里保持 PRIVATE，避免内部警告策略向上游 ABI 边界扩散。
target_compile_options(foo PRIVATE -Wall)
```

### 9.3 workaround 注释必须写出约束

以下场景必须写明原因：

- 编译器误报规避
- 链接顺序要求
- whole-archive 必须存在的原因
- 兼容别名保留的迁移目的

## 10. Preset 与产物合同

### 10.1 Preset 是正式入口

新增正式构建口径时，必须同步更新：

- `CMakePresets.json`
- 对应门禁脚本
- 对应正式文档

### 10.2 当前正式固件产物命名

当前正式固件 target / artifact basename：

- `themis-tec-at32`

历史兼容：

- `FalconFoc_AT32` 只允许作为迁移兼容 target 存在
- 新脚本、新文档、新口径必须全部切到 `themis-tec-at32`

## 11. Formatter 与质量门禁

### 11.1 formatter

当前正式 formatter 配置：

- `.cmake-format`

当前正式落地策略：

- `.cmake-format` 保持保守 2 空格 / `100` 列 / lowercase command / literal first comment block
- 本地统一入口使用 `scripts/format_themis.ps1`
- `check_cmake_quality.ps1` 会优先找 `cmake-format` 可执行文件；如果当前 Python
  环境已经能导入 `cmakelang.format`，即便 `Scripts` 目录没进 `PATH`，也允许回退到
  `python -m cmakelang.format`

当前正式安装方式：

```bash
python -m pip install --user cmakelang
```

Windows 补充：

- 如果当前 Python 已安装 `cmakelang`，但 `Scripts` 目录尚未进入 `PATH`，`check_cmake_quality.ps1`
  与 `format_themis.ps1` 仍可通过 `python -m cmakelang.format` 继续工作。
- 但为了和其它终端工具保持一致，仍然建议把当前 Python 的 `Scripts` 目录加入 `PATH`。
- 典型目录形态为 `%APPDATA%\Python\Python311\Scripts`，但正式门禁应以当前解释器实际安装位置为准。

### 11.2 本地检查入口

```bash
pwsh ./scripts/check_cmake_quality.ps1 -RepoRoot <git-root>
pwsh ./scripts/check_cmake_presets.ps1 -RepoRoot <git-root>
pwsh ./scripts/check_platform_backend_contract.ps1 -RepoRoot <git-root>
pwsh ./scripts/format_themis.ps1 -RepoRoot <git-root> -Scope cmake
pwsh ./scripts/check_themis_quality_gate.ps1 -RepoRoot <git-root> -Scope cmake
```

如果你要直接跑 host tests，正式入口是：

```bash
cmake --preset host-debug
cmake --build --preset build-host-debug
ctest --preset test-host-debug
```

不要把默认 `build/` 当成 host tree。默认 `build/` 是 firmware cross build；
只有 host preset 才会显式打开 `THEMIS_BUILD_HOST_TESTS` 并让 `ctest` 发现宿主测试。

`scripts/run_host_tests.ps1` 现在只是这条 preset 链的兼容包装器；它直接委托
`host-debug -> build-host-debug -> test-host-debug`，不再维护独立 host graph。

`check_themis_quality_gate.ps1` 仍然是唯一正式总入口；当 scope 涉及
`bsp/subsys/devdrv/applications/all` 时，脚本会在末尾输出统一摘要：

```text
[BSP]     0 errors, 0 warnings
[Subsys]  0 errors, 0 warnings
[DevDrv]  0 errors, 0 warnings
[App]     0 errors, 0 warnings
[Total]   PASS
```

### 11.3 严格 formatter 门禁

以下场景必须启用 formatter 强约束：

- CI
- 发布前基线核查
- 构建系统专项 PR

正式入口：

```bash
pwsh ./scripts/check_cmake_quality.ps1 -RepoRoot <git-root> -RequireFormatter
pwsh ./scripts/check_themis_quality_gate.ps1 -RepoRoot <git-root> -Scope cmake -RequireCMakeFormatter
```

规则：

- 本地开发环境允许在未安装 formatter 时给出显式 warn
- CI / 发布门禁必须把 formatter 升格为硬失败

### 11.4 第二阶段 root helper 边界

第二阶段之后，root `CMakeLists.txt` 的质量门禁还要额外检查：

- 是否按顺序加载：
  - `ThemisBuild`
  - `ThemisPlatformMetadata`
  - `ThemisBuildAxes`
  - `ThemisFirmwareAssembly`
  - `ThemisContracts`
  - `ThemisStatusReport`
- 是否按顺序调用：
  - build-axis cache 声明
  - platform selection / override cache 声明
  - build-axis 校验
  - firmware assembly
  - contract target 装配
  - status report 输出
- root 是否重新出现本应下沉的实现细节：
  - metadata load
  - contract target 创建
  - 直接 `message(STATUS "Themis ...")`

正式要求不是“root 行数越少越好”，而是：

- root 只保留装配叙事
- helper 保持单一职责
- gate 能阻止 helper 回流到 root

## 12. Review 清单

以后 review CMake，至少检查以下项目：

1. 文件角色是否写清楚。
2. 是否把正式构建入口、helper、toolchain、leaf manifest 的职责混在一起。
3. `PUBLIC / PRIVATE` 是否诚实。
4. 是否出现 public include 泄露别层目录。
5. 是否把派生值又做成独立 cache。
6. 是否还残留历史项目名、旧 target 名、旧芯片名。
7. 是否通过显式列表冻结了 public include contract。
8. preset、脚本、文档是否同步更新。
9. 是否补了质量门禁，而不是只改代码。

## 13. 一句话原则

**模块 CMake 只写模块事实，顶层入口负责构建轴与派生，helper 负责统一样板，门禁把这些规则变成长期可执行的仓库纪律。**
