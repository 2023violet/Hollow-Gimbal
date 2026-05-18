# Themis Windows 主机开发与调试操作规范

## 1. 文档目标

这份文档用于固定：

- 在当前 Windows 开发机上，应该用哪一种 shell 调哪一类命令
- 哪些环境事实可以默认相信，不需要每轮都重新探测
- 新同学和 AI 在这个仓库里，应该按什么顺序做 build / host / board / debug / release

先给出正式结论：

**当前 Themis 在 Windows 上不是“随手拿一个 shell 就跑”，而是“PowerShell 管治理与实板脚本，Git Bash 管 firmware CLI，VS Code F5 管交互式 OpenOCD 调试”的三轨工作流。**

## 2. 当前 Windows 主机正式事实

当前仓库默认环境事实如下：

- Git 仓库根目录：`D:/study/Software/TEC_Thermostat_Water_Bath`
- 开发板串口：`COM3`
- 正式 OpenOCD 路径：`C:/Program Files (x86)/OpenOCD_V2.0.2/bin/openocd.exe`
- 正式 OpenOCD 配置：`tools/openocd/openocd_daplink.cfg`
- VS Code 默认调试 lane：`fw-full-diag-carrier-debug`
- VS Code 默认调试 ELF：
  `build/fw-full-diag-carrier-debug/output/themis-tec-at32.elf`
- firmware CLI 正式入口：
  - `run.sh`
  - `memory.sh`

对 AI 和新同学的正式要求：

- 如果上面这些事实没有被明确推翻，就**先直接使用**，不要每一轮都重新扫盘、扫串口、搜 OpenOCD
- 只有当正式路径失败时，才进入“替代路径探测”模式

## 3. Shell 选择矩阵

### 3.1 默认用 PowerShell 的场景

下面这些命令默认应由 `PowerShell` 执行：

- 所有 `scripts/*.ps1`
- `cmake --preset ...`
- `cmake --build --preset ...`
- `ctest --preset ...`
- 质量门禁、contract、report、release-evidence 编排
- 任何带 `-RepoRoot`、`-PortName`、`-OpenOcdPath` 的正式脚本入口

当前推荐前缀：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File
```

### 3.2 默认用 Git Bash 的场景

下面这些命令默认应由 `Git Bash` 执行：

- `run.sh`
- `memory.sh`
- `bash -n run.sh memory.sh scripts/lib/themis_firmware_cli.sh`

人类开发者如果已经在 Git Bash 里：

```bash
./run.sh build
./run.sh all
./run.sh memory
```

AI 或当前 shell 是 PowerShell 时，推荐直接走稳定包装：

```powershell
D:/Git/Git/bin/bash.exe -lc "cd /d/study/Software/TEC_Thermostat_Water_Bath && ./run.sh build"
D:/Git/Git/bin/bash.exe -lc "cd /d/study/Software/TEC_Thermostat_Water_Bath && ./run.sh all"
D:/Git/Git/bin/bash.exe -lc "cd /d/study/Software/TEC_Thermostat_Water_Bath && ./run.sh memory"
```

### 3.3 默认用 VS Code `F5` 的场景

下面这些场景优先走 `F5 / cortex-debug`：

- 需要交互式断点调试
- 需要看 live watch
- 需要和 `.vscode/launch.json` 保持同一条 preset lane

当前正式配置：

- `launch.json` 里使用 `fw-full-diag-carrier-debug`
- `preLaunchTask` 是 `Themis: Build fw-full-diag-carrier-debug`
- `serverpath` 是
  `C:/Program Files (x86)/OpenOCD_V2.0.2/bin/openocd.exe`

### 3.4 默认用直接 OpenOCD / GDB 的场景

只有下面这些场景才应该直接调用 OpenOCD / GDB：

- `capture_watchdog_fault_probe.ps1`
- `capture_gdb_ui_injection_probe.ps1`
- 需要证明板级 witness，而不是只需要 F5 交互体验

也就是说：

- **交互调试优先 F5**
- **证据采集优先正式脚本**
- **不要把“手敲 OpenOCD 命令”当成日常默认入口**

## 4. 当前正式命令阶梯

## 4.1 第一层：格式与静态门禁

优先顺序：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format_themis.ps1 -RepoRoot D:/study/Software/TEC_Thermostat_Water_Bath -Scope all -Mode check -AllTracked
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/check_themis_quality_gate.ps1 -RepoRoot D:/study/Software/TEC_Thermostat_Water_Bath -Scope cmake
```

适用场景：

- 改了 C/CMake/脚本治理文档
- 改了 build/preset/contract 入口

## 4.2 第二层：host 构建与 host 测试

优先顺序：

```powershell
cmake --preset host-debug
cmake --build --preset build-host-debug
ctest --preset test-host-debug --output-on-failure
```

如果想走正式包装入口：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_host_tests.ps1 -RepoRoot D:/study/Software/TEC_Thermostat_Water_Bath
```

适用场景：

- 改了 `components / subsys / device / drivers / applications`
- 改了 host contract / gate / summary helper

## 4.3 第三层：firmware CLI 构建、下载、memory

PowerShell / AI 正式写法：

```powershell
D:/Git/Git/bin/bash.exe -lc "cd /d/study/Software/TEC_Thermostat_Water_Bath && ./run.sh build"
D:/Git/Git/bin/bash.exe -lc "cd /d/study/Software/TEC_Thermostat_Water_Bath && ./run.sh all"
D:/Git/Git/bin/bash.exe -lc "cd /d/study/Software/TEC_Thermostat_Water_Bath && ./run.sh memory"
```

适用场景：

- 需要 firmware cross build
- 需要 OpenOCD 下载
- 需要当前 ELF 的 memory report

## 4.4 第四层：最小实板回归与阶段回归

最小实板回归：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_tiny_carrier_regression.ps1 -RepoRoot D:/study/Software/TEC_Thermostat_Water_Bath -PortName COM3
```

mixed-path：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_mixed_path_regression.ps1 -RepoRoot D:/study/Software/TEC_Thermostat_Water_Bath -PortName COM3
```

AT32 steady-state：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_at32_steady_state_regression.ps1 -RepoRoot D:/study/Software/TEC_Thermostat_Water_Bath -PortName COM3
```

适用场景：

- 改了 UART / shell / watchdog / display / input / probe
- 改了 Stage 4/6/7 实板证据链

## 4.5 第五层：release evidence

正式入口：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_at32_release_evidence_pack.ps1 -RepoRoot D:/study/Software/TEC_Thermostat_Water_Bath -PortName COM3
```

适用场景：

- 要形成一轮正式交付证据
- 要做 release-grade 回归，而不是一次性烟雾验证

## 5. AI 与新同学的硬规则

### 5.1 不要混 shell

不允许：

- 在 PowerShell 里把 `./run.sh` 当成 `.ps1` 那样直接乱跑
- 在 Git Bash 里跑 `scripts/*.ps1`
- 把 `pwsh`、`powershell`、`bash` 三种入口随机混搭

正式裁决：

- `.ps1` 走 PowerShell
- `.sh` 走 Bash
- `F5` 走 `.vscode/launch.json`

### 5.2 不要重复探测已知事实

默认不需要重复探测：

- repo root
- `COM3`
- OpenOCD 正式路径
- OpenOCD cfg 正式路径
- VS Code 默认 debug preset

只有下面情况才允许重新探测：

- 正式路径不存在
- 正式端口失效
- 用户明确说硬件或工具位置变了
- 当前任务就是在做“环境迁移 / 环境自愈”

### 5.3 不要把 `auto` 当成默认美德

当前开发板串口已经正式固定为 `COM3`。

因此：

- 人工/AI 默认应该显式传 `-PortName COM3`
- `auto` 只用于：
  - 新环境首次接板
  - 串口漂移排查
  - 脚本本身的自动发现能力验证

### 5.4 不要跳步骤

推荐固定顺序：

1. `check / format`
2. `host`
3. `run.sh build/all/memory`
4. `board regression`
5. `release evidence`

不推荐：

- 一上来就实板刷机
- 一上来就搜 OpenOCD / 串口 / preset
- 还没过 host gate 就先怀疑板子坏了

## 6. AI 专用低耗用法

为了减少 token 和时间浪费，AI 在当前仓库应遵守下面这组默认操作法：

### 6.1 静态门禁

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/check_themis_quality_gate.ps1 -RepoRoot D:/study/Software/TEC_Thermostat_Water_Bath -Scope all -Summary
```

### 6.2 firmware CLI

```powershell
D:/Git/Git/bin/bash.exe -lc "cd /d/study/Software/TEC_Thermostat_Water_Bath && ./run.sh build"
```

### 6.3 memory report

```powershell
D:/Git/Git/bin/bash.exe -lc "cd /d/study/Software/TEC_Thermostat_Water_Bath && ./run.sh memory"
```

### 6.4 最小实板回归

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_tiny_carrier_regression.ps1 -RepoRoot D:/study/Software/TEC_Thermostat_Water_Bath -PortName COM3
```

### 6.5 发布证据

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_at32_release_evidence_pack.ps1 -RepoRoot D:/study/Software/TEC_Thermostat_Water_Bath -PortName COM3
```

这五条命令已经覆盖了大部分正式工作流，不应每轮重新发明调用方式。

## 7. 出错时先判断哪一层坏了

先判断下面哪一层出错，再决定是否继续探测：

1. shell 选错了
2. repo root / preset / build tree 选错了
3. OpenOCD / Git Bash / PowerShell 工具缺失
4. 串口或硬件连接失败
5. 代码或配置真的回归了

也就是说：

- **先排使用方式**
- **再排环境**
- **最后才判代码**

## 8. 一句话结论

**当前 Themis 在 Windows 上的正式用法不是“什么都试一遍”，而是“PowerShell 管治理和实板脚本，Git Bash 管 firmware CLI，F5 管交互式调试；repo root、COM3、OpenOCD 路径默认固定，只有正式路径失效时才重新探测”。**
