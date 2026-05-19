param(
    [string]$RepoRoot = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
} else {
    $RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
}

function Read-Utf8Raw {
    param(
        [string]$RelativePath
    )

    $path = Join-Path $RepoRoot $RelativePath
    return [System.IO.File]::ReadAllText($path, [System.Text.UTF8Encoding]::new($false))
}

function To-RepoRelativePath {
    param(
        [string]$FullPath
    )

    $repoPrefix = ($RepoRoot.TrimEnd('\', '/')) + [System.IO.Path]::DirectorySeparatorChar
    if ($FullPath.StartsWith($repoPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $FullPath.Substring($repoPrefix.Length)
    }

    return $FullPath
}

function Resolve-GovernanceDocByContent {
    param(
        [string]$RequiredToken
    )

    $matches = @(Get-ChildItem -Path (Join-Path $RepoRoot "docs\governance") -Filter "*.md" |
        Where-Object {
            $raw = [System.IO.File]::ReadAllText($_.FullName, [System.Text.UTF8Encoding]::new($false))
            $raw.Contains($RequiredToken)
        }
    )

    if ($matches.Count -ne 1) {
        throw "Expected exactly one governance doc containing '$RequiredToken'."
    }

    return To-RepoRelativePath $matches[0].FullName
}

function Resolve-GovernanceDocByTokens {
    param(
        [string[]]$RequiredTokens
    )

    $matches = @(Get-ChildItem -Path (Join-Path $RepoRoot "docs\governance") -Filter "*.md" |
        Where-Object {
            $raw = [System.IO.File]::ReadAllText($_.FullName, [System.Text.UTF8Encoding]::new($false))
            foreach ($token in $RequiredTokens) {
                if ($raw.IndexOf($token, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
                    return $false
                }
            }
            return $true
        }
    )

    if ($matches.Count -ne 1) {
        throw "Expected exactly one governance doc containing token set: $($RequiredTokens -join ', ')."
    }

    return To-RepoRelativePath $matches[0].FullName
}

$bspManifestRaw = Read-Utf8Raw "bsp\board\CMakeLists.txt"
if ($bspManifestRaw -match [Regex]::Escape("../../applications/tasks")) {
    throw "BSP must not publicly export applications/tasks after startup ownership moved to app_main + motor_task."
}

$runtimeVisibleFiles = @(
    "applications\tasks\motor_task.c",
    "applications\tasks\motor_fsm.c"
)

$forbiddenRuntimeChars = @(
    [char]0x2014,
    [char]0x2013,
    [char]0x2026
)

foreach ($relativePath in $runtimeVisibleFiles) {
    $raw = Read-Utf8Raw $relativePath

    foreach ($match in [regex]::Matches($raw, '"[^"\r\n]*"')) {
        foreach ($ch in $forbiddenRuntimeChars) {
            if ($match.Value.Contains([string]$ch)) {
                throw "Runtime-visible strings in '$relativePath' must stay ASCII-only so COM54 shell captures do not degrade."
            }
        }
    }
}

$focParamRaw = Read-Utf8Raw "applications\algo\foc_param.c"
if ($focParamRaw -match '\*\(const uint32_t \*\)\(uintptr_t\)') {
    throw "foc_param.c must not depend on direct memory-mapped Flash dereference for seq_num reads. Use the flash read contract instead."
}

$shellTaskHeaderRaw = Read-Utf8Raw "applications\tasks\shell_task.h"
if ($shellTaskHeaderRaw -match [Regex]::Escape("Called automatically by the INIT_EXPORT system.")) {
    throw "shell_task.h must describe the real owner. shell_task_init() is called from motor_task_init(), not by INIT_EXPORT."
}

$motorTaskRaw = Read-Utf8Raw "applications\tasks\motor_task.c"
$motorTaskInitPos = $motorTaskRaw.IndexOf("void motor_task_init(void)", [System.StringComparison]::Ordinal)
$shellInitPos = $motorTaskRaw.IndexOf("shell_task_init();", [System.StringComparison]::Ordinal)
$motorInitPos = $motorTaskRaw.IndexOf("_motor_init(&_motor_ins);", [System.StringComparison]::Ordinal)
if (($motorTaskInitPos -lt 0) -or ($shellInitPos -lt $motorTaskInitPos) -or ($motorInitPos -lt $shellInitPos)) {
    throw "motor_task_init() must explicitly start shell_task_init() before _motor_init() so the startup owner chain stays visible."
}

$coreConfigRaw = Read-Utf8Raw "include\config\tm_core_config.h"
if ($coreConfigRaw -notmatch [Regex]::Escape("#define TM_APP_FEATURE_UART_TELEMETRY (0U)")) {
    throw "FalconFoc-lite must default UART telemetry to disabled so USART1 stays console-first unless diagnostics opt in explicitly."
}
if ($coreConfigRaw -notmatch [Regex]::Escape("#define TM_APP_FEATURE_UART_PASSIVE_TELEMETRY (0U)")) {
    throw "FalconFoc-lite must default passive UART telemetry to disabled on the shared USART1 transport."
}

$bspConfigRaw = Read-Utf8Raw "bsp\bsp_config.h"
foreach ($token in @(
        "BOARD_CONSOLE_UART_NAME",
        "BOARD_TELEMETRY_UART_NAME",
        "BOARD_TELEMETRY_UART_NAME   BOARD_CONSOLE_UART_NAME"
    )) {
    if ($bspConfigRaw -notmatch [Regex]::Escape($token)) {
        throw "bsp/bsp_config.h must keep the shared USART1 console/telemetry ownership token '$token'."
    }
}

$transportDocRaw = Read-Utf8Raw (Resolve-GovernanceDocByTokens @(
        "FalconFoc-lite transport-shell-hardening",
        "console-first",
        "debug-only",
        "COM54"
    ))
foreach ($token in @(
        "shell_task",
        "app_shell_task",
        "USART1",
        "console-first",
        "VOFA",
        "debug-only",
        "COM54"
    )) {
    if ($transportDocRaw -notmatch [Regex]::Escape($token)) {
        throw "The transport hardening doc must record the FalconFoc-lite serial strategy token '$token'."
    }
}

$shellGovernanceDocRaw = Read-Utf8Raw (Resolve-GovernanceDocByTokens @(
        "FalconFoc-lite shell-output-policy",
        "Repeated Row",
        "ASCII",
        "128"
    ))
foreach ($token in @(
        "shell_task",
        "app_shell_task",
        "console-first"
    )) {
    if ($shellGovernanceDocRaw -notmatch [Regex]::Escape($token)) {
        throw "The shell output governance doc must record the FalconFoc-lite shell policy token '$token'."
    }
}

$bootDocs = @(
    Get-ChildItem -Path (Join-Path $RepoRoot "docs\boot") -Filter "*.md" |
        Where-Object {
            $raw = [System.IO.File]::ReadAllText($_.FullName, [System.Text.UTF8Encoding]::new($false))
            $raw.Contains("applications/app_main.c")
        } |
        ForEach-Object { To-RepoRelativePath $_.FullName }
)

if ($bootDocs.Count -lt 2) {
    throw "Expected at least two boot docs that describe the live FalconFoc app_main startup bridge."
}

$requiredBootTokens = @(
    "platform/at/at32f415/generated/src/main.c",
    "applications/app_main.c",
    "motor_task_init",
    "vofa_task_init",
    "shell_task_init",
    "app_shell_task"
)

$staleBootTokens = @(
    "at32f423",
    "watchdog_task_init",
    "buzzer_task_init",
    "display_task_init",
    "uart_task_init",
    "bsp_devices_init",
    "bsp_subsys_init",
    "bsp_devices_late_init",
    "tm_hw_uart_prepare",
    "tm_hw_i2c_prepare",
    "motor_shell_cmds_init"
)

foreach ($relativePath in $bootDocs) {
    $raw = Read-Utf8Raw $relativePath

    foreach ($token in $requiredBootTokens) {
        if ($raw -notmatch [Regex]::Escape($token)) {
            throw "'$relativePath' must document the live FalconFoc boot chain token '$token'."
        }
    }

    foreach ($token in $staleBootTokens) {
        if ($raw -match [Regex]::Escape($token)) {
            throw "'$relativePath' still contains stale retired boot-chain token '$token'."
        }
    }
}

$maintenanceLogRaw = Read-Utf8Raw (Resolve-GovernanceDocByContent "FalconFoc AT32F415 Themis-lite platform/build alignment")
foreach ($token in @("COM54", "DAP-Link", "./run.sh flash", "store save sys")) {
    if ($maintenanceLogRaw -notmatch [Regex]::Escape($token)) {
        throw "Maintenance log must record the latest FalconFoc real-board witness token '$token'."
    }
}

Write-Host "[test_falconfoc_themis_lite_contract] all assertions passed."
