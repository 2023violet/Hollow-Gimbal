# FalconFOC V2｜NEXT-QUALITY-GATE-01

日期：2026-11-17。任务：关闭 AUDIT-02B F2 / CR03 P2-B 的 PowerShell 严格参数转发缺陷。

**交付结论：NEXT-QUALITY-GATE-01 PASS。** 此裁决仅在本报告列出的真实验证完成、普通提交推送并核对同名远端 SHA 后成立；最终 Git 回执由交付记录和任务回复提供。没有修改控制器、正式 CMake 格式、Board1 参数、Safety、UART 或输出权限。

## 1. 实际基线与隔离

- 工作树：`D:/study/Software/Hollow-Gimbal-vm01a`。
- 分支：`codex/falconfoc-v2-vm01a`。
- 起始 HEAD 与远端同名分支均为 `f115cdf585e74aed512263ca66110a197cad9628`；起始工作树干净。
- PowerShell：Windows PowerShell **5.1.19041.6456 / Desktop**，不是以 PowerShell 7 的绑定行为代替验证。
- 外部证据根：`D:/study/Software/FalconFOC-quality-gate-01-20261117`。
- 捕获 2526 个项目文件及 2427 个既有 ignored 文件（322054220 B）；在新的完整源码快照、私有 Git clone 和新短 TEMP 根运行会写 build 的 Gate。
- 隔离目录和复制的三个 Legacy ELF/MAP SHA-256 见 `isolation.json`。这些是历史产物的逐字节复制，用于当前符号合同检查；本轮没有新编译 Legacy ARM，也没有把它当作新 ARM 证据。
- Host cache 仅在新 clone 通过正式 runner 加 `-CompilerPath C:/mingw64/bin/gcc.exe` 建立；不删除或重配原工作树的 GCC/Clang cache。

已读取 AGENTS、CR03、AUDIT02B、CMake 质量门禁文档、Scripts README、canonical gate/CMake quality、两个兼容入口和相关活跃脚本调用者。没有从旧 RC0 覆盖最新分支。

## 2. 缺陷复现与分类

所有真实入口验证均通过 `scripts/check_themis_quality_gate.ps1` 包装入口。工作目录为新 clone；未在原有 build 目录运行。

| 修复前调用 | exit | 实际阶段 / 分类 |
| --- | ---: | --- |
| Scope all | 0 | 全门禁通过；普通默认成功不能证明 strict 成功 |
| Scope all -RequireCMakeFormatter | 1 | CMake 子进程尚未进入格式检查即绑定失败；ARGUMENT_BINDING_FAILURE |
| CI=true，Scope all | 1 | 同上，ARGUMENT_BINDING_FAILURE |
| GITHUB_ACTIONS=true，Scope all | 1 | 同上，ARGUMENT_BINDING_FAILURE |
| 直接 CMake Quality -RequireFormatter | 0 | 已安装的真实 Formatter 完成全部 30 文件；排除缺依赖混淆 |
| 新参数契约测试 | 1 | 真正子脚本的 RequireFormatter 收到不可绑定的 String；不是人为制造的断言失败 |

原错误明确指出 `Cannot convert value System.String to System.Management.Automation.SwitchParameter`。原始 stdout/stderr、完整 argv/cwd、开始结束 UTC 和 exit 均保留；不是复制上次审计 PASS/FAIL 充数。

## 3. PowerShell 5.1 根因

原 `Invoke-ChildScript` 向外部原生进程执行：

```powershell
& powershell -NoProfile -ExecutionPolicy Bypass -File $ScriptPath @ParameterMap
```

该层不是同进程 PowerShell 函数的 typed hashtable binding。Hashtable 被序列化到外部命令行；`RequireFormatter=$true` 形成带字符串值的参数，Windows PowerShell 5.1 的 `-File` 不能按预期把这个值绑定到 Switch。应使用单独的 `-RequireFormatter`，false 时省略。

当前参数类型必须区分：`RepoRoot` 是 String；`RequireFormatter` 是 Switch；**`SkipFormatter` 是 String**。旧调用者以 bool true 写 SkipFormatter，跨进程后偶然变成可识别的 `True` 字符串。因此不能把所有 true 值改成裸参数后就结束；那会破坏 Scope falconfoc 的 String 参数。

原设计使用独立子进程是合理的：同进程 `& child.ps1` 可能让 child 内部 native probe 的 LASTEXITCODE 泄漏。新测试让子脚本先执行 native exit 37、最后显式 exit 0，验证父进程看到的是 0；直接 child exit 7 必须引发整体失败。保留独立进程、EAP 恢复和原 throw 机制，不改为 Invoke-Expression、拼接命令文本或通用命令框架。

## 4. 最小修改

实现只改 [canonical quality gate](../../scripts/gates/check_themis_quality_gate.ps1)：**16 行新增、2 行删除**。

- 构建独立 argv 数组。
- 内部 map 的 Boolean 约定代表 Switch：true 只追加参数名，false 不追加。
- String 参数追加参数名及真实字符串值，保留空格与字符串 `true`/`false`。
- `Invoke-CMakeQuality` 明确传 `SkipFormatter = "true"`，维持旧 scoped skip 的含义。
- 保留外部 powershell、错误首选项 finally 恢复、非零 LASTEXITCODE 检查和 throw。

核心逻辑：

```powershell
foreach ($name in $ParameterMap.Keys) {
    $value = $ParameterMap[$name]
    if ($value -is [bool]) {
        if ($value) { $childArgs += "-$name" }
    } else {
        $childArgs += "-$name"
        $childArgs += [string]$value
    }
}
& powershell @childArgs
```

`scripts/gates/check_cmake_quality.ps1`、`scripts/check_cmake_quality.ps1` 和 `scripts/check_themis_quality_gate.ps1` 均逐字节未变。没有把 SkipFormatter 改为 Switch、改 Formatter 安装策略或改变任何 CMake 文件。

其他变更仅为新测试、直接相关 Scripts README、此报告/验证摘要及追加维护日志。没有无关文件暂存。

## 5. 正式 RED → GREEN 与负向测试

正式入口：[test_quality_gate_arguments.ps1](../../tests/script/test_quality_gate_arguments.ps1)。不需要 Pester 或任何新系统依赖，只使用既有 Windows PowerShell、Git 与临时 Fixture。

测试通过 AST 读取**真实生产函数定义**，不复制 argv 修复逻辑作为 Oracle；子脚本写出收到值及运行时类型。随后在含空格路径的私有 Git fixture 运行真实完整 Gate/包装入口，保留真正 CMake Format Check 的失败处理函数，只替换外部工具可见性和昂贵叶检查。

| 正式检查 | 验收行为 |
| --- | --- |
| 无 Switch / true / false | 子进程分别收到 false / true / false，类型为 SwitchParameter |
| String=true / false | 真实 System.String 值原样保留，不被当成开关 |
| script/path 含空格 | 路径和值完整，不拆分 |
| 子脚本内部 native exit 37、最终 exit 0 | 父 helper 成功，LASTEXITCODE=0 |
| 子脚本 exit 7 | 父 helper 抛错，LASTEXITCODE 保持 7；聚合 Gate 对外非零 |
| ordinary / strict / CI / Actions | 完整检查顺序、收到的 Required 值、Formatter 的真实进程副作用及 Summary 同时达标 |
| Scope falconfoc | String Skip=true，保留 formatter / firmware CLI / Host 跳过范围 |
| Formatter 缺失 | required 时拒绝，不能进入后续叶检查 |
| Formatter 失败 | 实际执行临时工具并检查 --check 参数，非零被传播，停止后续检查 |
| 其他子 Gate 失败 | 即使 Formatter 成功，后续失败仍终止整体 |

旧生产实现同一正式测试退出 1（真实参数绑定错误）；最小修改后通过 **31 项断言**。完整 real clone 中再次执行同一测试也通过。child 返回 7 时聚合脚本最终通常为 1，这是保留的 throw 语义，不是承诺把数字 7 原样作为最外层进程 exit。

测试保存/恢复自己的 CI、GITHUB_ACTIONS 等 Process 环境值；外部实跑使用独立 child env map，从未清除用户进程既有 CI 设置。fixture 只清理本次创建的 GUID 目录，清理前验证绝对路径位于临时根且目录名符合专用前缀；不删除项目或旧 build。

## 6. 真实工具成功与失败路径

除正式 Fixture 测试外，再从真正兼容入口执行未替换的完整 canonical Gate/CMake Quality：

| 场景 | 结果 | 分类 / 证据 |
| --- | --- | --- |
| 正常可用 Formatter，普通全门禁 | exit 0 | 真实 30 文件格式检查、完整下游检查及 Host 45/45 |
| 显式 RequireCMakeFormatter + Summary | exit 0 | 同上，最终 Total PASS |
| CI=true | exit 0 | 自动启用 strict，真实 30 文件格式检查和完整下游流程 |
| GITHUB_ACTIONS=true | exit 0 | 同上 |
| PATH 中隐藏 formatter 与 Python，显式 strict | exit 1 | FORMATTER_NOT_INSTALLED；独立 preflight 确认两者确不可见 |
| 同一缺工具环境，CI=true | exit 1 | FORMATTER_NOT_INSTALLED；没有降级为 warn/skip |
| 临时 cmake-format 命令返回 9 | exit 1 | FORMATTER_CHECK_FAILED；保留实际 --config-files/--check 原生参数收据 |
| Formatter 正常完成后，临时 cmake 命令返回 7 | exit 1 | OTHER_GATE_FAILURE；已完成 30 格式检查，后续 CMake smoke 失败仍传播 |

负向测试只在私有 PATH 前置临时命令或移除 child PATH 中工具；不卸载软件、不安装依赖、不修改任何真实 CMake 文件。真实 Formatter 的成功路径也确实执行，不能仅用 mock/日志字符串宣称严格检查通过。

CI/Actions 行指本机 Windows PowerShell 中的真实环境变量模式运行，**不是声称已在 GitHub-hosted runner 上执行过 workflow**。

## 7. 完整回归与范围保持

- 新参数/行为契约：31 assertions PASS。
- CMake Quality 普通与 RequireFormatter：分别 exit 0。
- Source Contract：81 C/H、9 CMake、9 README，exit 0。
- Scope falconfoc：exit 0，保留既有裁剪。
- Scope all 普通、显式 strict、CI、Actions：四条真实完整执行均 exit 0，分别包含 Legacy Host **45/45**。
- 全门禁中的 preset、平台 backend、CMake smoke、firmware CLI mock、cache guard、transport/shell、current artifact contract 均按原顺序执行；没有因 Formatter 通过而跳过其他失败。
- Summary 参数/输出保持；真实全门禁检查最终 `[Total] PASS`，正式 fixture 检查完整顺序与精确 Summary 标记。

Scope falconfoc 的原显式 SkipFormatter 策略优先保留，没有借此将快速 source lane 扩成全项目检查。完整 strict/CI 验收使用 Scope all。没有重定义原有 Scope 组合优先级。

Core/Runtime/Simulation/Board/CLI 和 ARM 验证入口、生产 C/H、控制参数均未变，逐文件核对见 change-verification。**本轮没有重跑这些独立控制项目的整套 CTest 或新 ARM 构建**；CR03/AUDIT02B 历史结果仅作为历史上下文，未列为本轮新执行。

## 8. 未关闭问题与运行器纠正记录

本阶段目标 P2-B 关闭；Motion 预测精度合同/测试建议不在本轮修改范围，保持原审查结论。

额外抽查的 `tests/script/test_falconfoc_themis_lite_contract.ps1` 是一条**未被当前 Scope all 调用的遗留静态检查**：它引用已经移除的 `platform/at/at32f403a/ThemisPlatformDefaults.cmake`。从 repo cwd 执行，在修改前不可变快照和修改后 clone 上均 exit 1。此旧问题保留为独立 legacy-contract 清理事项，没有修复或标记 PASS，不影响本阶段已验证的 live Gate 参数转发修复。开始时从日志目录运行该旧脚本还触发了其相对 cwd 的路径假设；该运行错误与随后独立确认的旧平台失败分开保留。

外部运行器最初沿用历史“29 个 formatter 文件”的计数假设；真实声明集合是 **30**，Gate 自己已经 exit 0，错误的是收集断言。已核对原始完整文件列表，修正运行器为 30 并继续未执行的模式，没有改门禁、删结果或把真实失败改成功。临时 cmake 注入的实际失败点是 smoke，preset contract 本身为静态检查；记录按实际失败阶段纠正。

原有 Quality 报告 EOF 空行、历史审计文本都没有修改。没有复活旧 AT32F403A 平台、修剪旧 evidence 或顺手修改 Motion Core。

## 9. 保护、差异和 Git 交付

所有旧项目文件的差异仅在本任务 allowlist：canonical Gate、Scripts README、追加维护记录；另有新正式测试及本报告/摘要。两个包装脚本、CMake Quality、全部控制与构建入口未变；2427 个既有 ignored 产物内容/数量未变；原始快照未变；主工作树保持干净。

提交前核对同名远端仍等于任务基线；仅显式暂存本任务文件，执行 `git diff --cached --check`。使用普通 commit/push，不 force、不 rebase/reset/stash、不修改主分支、不合并或改写历史。若远端变化或出现非快进，本轮交付应停止并报告，不能强推。

Git 自身最终 SHA 不写入其自身待提交内容以避免自引用；远端读回值与本地最终 HEAD 的一致性在最终交付回复及外部 `publication-verification.json` 中记录。源码/旧产物保护在提交前后分别核对。

## 10. 证据与停止点

仓库内附带 [results.json](2026-11-17-next-quality-gate-01/results.json) 与 [change-verification.json](2026-11-17-next-quality-gate-01/change-verification.json) 摘要。完整 argv/cwd、PowerShell 版本、stdout/stderr、exit、UTC 与日志 SHA-256、最小补丁、测试原始版本和隔离配置位于外部证据根；其中 red/、red-live/、green/、green-live/、real-negatives/、contracts/ 分别保存不同阶段，不覆盖旧记录。

完成门禁修复及授权 Git 交付后停止，不继续 Motion 或硬件任务。保持：

```text
G3-SAFE HARDWARE NOT VERIFIED
G3-PWR LOCKED
AT32F415 WCET NOT VERIFIED
MOTOR PHYSICAL OPERATION UNVERIFIED
```
