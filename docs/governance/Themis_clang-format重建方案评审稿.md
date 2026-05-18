# Themis `.clang-format` / `.cmake-format` 重建评审与落地稿

## 1. 目标

这份文档回答三件事：

1. 参考大型开源项目后，Themis 当前应采用哪一套正式 formatter 标准。
2. 为什么这套标准符合 Themis 当前阶段，而不是“看起来更潮”的模板。
3. 这轮代码里到底落地了什么，哪些事情仍然刻意没有硬推进。

## 2. 外部参考样本

本轮只参考正式项目仓库中的公开配置，不拿二手博客当依据。

### 2.1 `.clang-format`

- LLVM：根配置几乎直接继承 LLVM，强调“少配置、强统一”。
- Zephyr：官方 `.clang-format` 走 Linux 风格大括号、`ColumnLimit: 100`、更强的内核式宏/循环宏约束。
- RT-Thread：官方 `.clang-format` 走 `4` 空格、`ColumnLimit: 0`、`BreakBeforeBraces: Custom`、`PointerAlignment: Right`。
- Protocol Buffers：根配置更接近 Google 风格，`PointerAlignment: Left`。
- Kitware/CMake：更偏 Mozilla/桌面 C++ 风格，`ColumnLimit: 79`，并带重 include category 约束。

### 2.2 `.cmake-format`

- Apache Arrow：`2` 空格、`line_width = 90`、lowercase command、literal first comment block。
- fmt：极简 `.cmake-format`，只保留少量风格约束。
- Executorch：显式保留 `first_comment_is_literal` 和 `dangle_parens`。

## 3. Themis 当前真实证据

### 3.1 先纠偏：不能把 no-touch 边界混进统计

之前直接对全仓跟踪中的 `.c/.h` 跑 `clang-format --dry-run --Werror`，会把
`platform/`、`kernel/` 等 vendor / no-touch 边界一起算进去。

这会得出一个**方向正确但口径失真**的结论：问题存在，但“失真程度”被明显放大了。

### 3.2 Themis 自有代码口径

本轮真正用于裁决的口径是：

- `applications/`
- `bsp/`
- `components/`
- `device/`
- `drivers/`
- `include/`
- `subsys/`
- `tests/`

在这组受 Themis 正式治理的 `.c/.h` 上，当前根 `.clang-format` 的 dry-run 结果是：

- owned files：`337`
- mismatch files：`192`

这说明当前基线**仍有历史格式债务**，但已经不是“根配置完全撒谎”的级别。

### 3.3 与候选标准的对比

在同一组 owned files 上，本轮补跑了几个候选标准：

- 当前 Themis 根 `.clang-format`：`192` bad
- 按当前根配置重写注释后的等价 `4` 空格候选：`192` bad
- 直接切到 `2` 空格 + attach brace 候选：`311` bad
- 直接套 RT-Thread 官方 `.clang-format`：`315` bad
- 直接套 Zephyr 官方 `.clang-format`：`321` bad

结论非常明确：

- **Themis 当前正式可执行标准必须继续保持 repo-truthful 的 `4` 空格基线。**
- `2` 空格 / attach brace 更像一部分近期开启的上层文件风格，但还远没成为整个受管代码面的正式主流。

## 4. 两套标准裁决

### 4.1 标准 A：当前正式启用标准

这是本轮真正写入仓库并生效的标准。

#### C / H

- `4` 空格缩进
- 不使用 tab
- `ColumnLimit: 100`
- `PointerAlignment: Right`
- `SortIncludes: Never`
- 函数大括号继续使用当前 repo-truthful 的换行方式
- 连续对齐继续保留，因为它在当前 owned corpus 里比“全面关闭对齐”更贴近现状

#### CMake

- `2` 空格缩进
- `line_width = 100`
- `command_case = "lower"`
- `keyword_case = "unchanged"`
- `dangle_parens = True`
- `first_comment_is_literal = True`
- `enable_markup = False`

### 4.2 标准 B：已审查但不启用的备选标准

这是本轮明确保留为“未来可再讨论”的备选，不写入正式 gate。

#### C / H

- `2` 空格缩进
- attach brace
- 更克制的自动对齐

这套风格更适合一部分近期开启的上层 owner 文件，但当前一旦强行升格为全仓正式标准，会立刻制造更大的格式偏差。

#### CMake

- Arrow 风格的 `line_width = 90`
- `dangle_parens = False`
- 更严格的纵向包裹策略

这套风格没有错，但当前 Themis 的正式 CMake 清单更接近 `100` 列和悬挂右括号，不值得为了“更像某个大项目”而硬切。

## 5. 本轮实际落地

### 5.1 已落地

- 根 `.clang-format` 没有被激进改写，而是保留了当前正式 `4` 空格基线，并把注释重写成可维护的执行说明。
- `.cmake-format` 从“仅够 CI 勉强识别”升级成真正可用的保守基线。
- `scripts/check_cmake_quality.ps1` 现在会优先找 `cmake-format`，找不到时回退到
  `python -m cmakelang.format`，避免只因 `Scripts` 不在 `PATH` 就把本地体验做废。
- 新增 `scripts/format_themis.ps1`：
  - 默认只处理 changed/untracked 文件
  - `-AllTracked` 可一键覆盖全部受管文件
  - `-Scope c|cmake|all`
  - `-Mode format|check`
  - `-BootstrapCMakeFormatter` 可直接安装 `cmakelang`

### 5.2 刻意没有落地

- 没有把 `clang-format` 立刻并进 quality gate。
- 没有做全仓 `.c/.h` 大扫除。
- 没有为了追求“整齐统一”去伪造一套当前仓库并不真正拥有的 `2` 空格正式标准。

原因很简单：**当前 owned corpus 仍有 `192 / 337` 的历史格式债务，先把入口做真，比先把门禁做硬更重要。**

## 6. 当前正式使用方式

### 6.1 日常格式化改动文件

```powershell
pwsh ./scripts/format_themis.ps1 -RepoRoot <git-root> -Scope all
```

### 6.2 一键整理全部受管 CMake / C/H

```powershell
pwsh ./scripts/format_themis.ps1 -RepoRoot <git-root> -Scope all -AllTracked
```

### 6.3 只做检查，不改文件

```powershell
pwsh ./scripts/format_themis.ps1 -RepoRoot <git-root> -Scope cmake -Mode check -AllTracked
```

### 6.4 本地缺少 `cmake-format`

```powershell
pwsh ./scripts/format_themis.ps1 -RepoRoot <git-root> -Scope cmake -BootstrapCMakeFormatter
```

## 7. 一句话裁决

**Themis 这轮 formatter 最正确的动作不是“追最新潮模板”，而是把当前真正可执行的 repo-truthful 基线做实：C/H 继续坚持 `4` 空格正式标准，CMake 升级成真正可用的保守 `2` 空格基线，工具入口先打通，再逐步清理历史格式债务。**
