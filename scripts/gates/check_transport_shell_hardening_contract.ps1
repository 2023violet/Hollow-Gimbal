param(
    [string]$RepoRoot = ""
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

function Resolve-AbsolutePath
{
    param([string]$Path)

    return (Resolve-Path -LiteralPath $Path).Path
}

function Resolve-ThemisRoot
{
    param([string]$Root)

    if ($Root -ne "") {
        $baseRoot = Resolve-AbsolutePath -Path $Root
    } else {
        $baseRoot = (& git rev-parse --show-toplevel).Trim()
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to locate git repository root."
        }
    }

    if ((-not (Test-Path -LiteralPath (Join-Path $baseRoot "scripts"))) -or
        (-not (Test-Path -LiteralPath (Join-Path $baseRoot "CMakePresets.json")))) {
        throw "Unable to resolve Themis repository root from '$baseRoot'."
    }

    return $baseRoot
}

function Assert-Exists
{
    param(
        [string]$Root,
        [string]$RelativePath,
        [System.Collections.Generic.List[string]]$Errors
    )

    if (-not (Test-Path -LiteralPath (Join-Path $Root $RelativePath))) {
        $Errors.Add("ERROR: missing required transport/shell file '$RelativePath'")
    }
}

function Assert-Contains
{
    param(
        [string]$Root,
        [string]$RelativePath,
        [string[]]$Markers,
        [System.Collections.Generic.List[string]]$Errors
    )

    $path = Join-Path $Root $RelativePath
    if (-not (Test-Path -LiteralPath $path)) {
        $Errors.Add("ERROR: missing file '$RelativePath'")
        return
    }

    $text = Get-Content -LiteralPath $path -Encoding UTF8 -Raw
    foreach ($marker in $Markers) {
        if ($text.IndexOf($marker, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
            $Errors.Add("ERROR: $RelativePath missing marker '$marker'")
        }
    }
}

function Assert-Omits
{
    param(
        [string]$Root,
        [string]$RelativePath,
        [string[]]$Markers,
        [System.Collections.Generic.List[string]]$Errors
    )

    $path = Join-Path $Root $RelativePath
    if (-not (Test-Path -LiteralPath $path)) {
        $Errors.Add("ERROR: missing file '$RelativePath'")
        return
    }

    $text = Get-Content -LiteralPath $path -Encoding UTF8 -Raw
    foreach ($marker in $Markers) {
        if ($text.IndexOf($marker, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
            $Errors.Add("ERROR: $RelativePath still contains retired marker '$marker'")
        }
    }
}

function To-RepoRelativePath
{
    param(
        [string]$Root,
        [string]$FullPath
    )

    $repoPrefix = ($Root.TrimEnd('\', '/')) + [System.IO.Path]::DirectorySeparatorChar
    if ($FullPath.StartsWith($repoPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $FullPath.Substring($repoPrefix.Length)
    }

    return $FullPath
}

function Resolve-GovernanceDocByTokens
{
    param(
        [string]$Root,
        [string[]]$Tokens
    )

    $matches = @(Get-ChildItem -Path (Join-Path $Root "docs\governance") -Filter "*.md" |
        Where-Object {
            $raw = Get-Content -LiteralPath $_.FullName -Encoding UTF8 -Raw
            foreach ($token in $Tokens) {
                if ($raw.IndexOf($token, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
                    return $false
                }
            }
            return $true
        })

    if ($matches.Count -ne 1) {
        return $null
    }

    return To-RepoRelativePath -Root $Root -FullPath $matches[0].FullName
}

function Resolve-ApplicationDocByTokens
{
    param(
        [string]$Root,
        [string[]]$Tokens
    )

    $matches = @(Get-ChildItem -Path (Join-Path $Root "docs\application") -Filter "*.md" |
        Where-Object {
            $raw = Get-Content -LiteralPath $_.FullName -Encoding UTF8 -Raw
            foreach ($token in $Tokens) {
                if ($raw.IndexOf($token, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
                    return $false
                }
            }
            return $true
        })

    if ($matches.Count -ne 1) {
        return $null
    }

    return To-RepoRelativePath -Root $Root -FullPath $matches[0].FullName
}

$repoRoot = Resolve-ThemisRoot -Root $RepoRoot
$errors   = New-Object "System.Collections.Generic.List[string]"
$transportDocRelativePath = Resolve-GovernanceDocByTokens -Root $repoRoot -Tokens @(
    "FalconFoc-lite transport-shell-hardening",
    "console-first",
    "debug-only",
    "COM54"
)
$shellDocRelativePath = Resolve-GovernanceDocByTokens -Root $repoRoot -Tokens @(
    "FalconFoc-lite shell-output-policy",
    "Repeated Row",
    "ASCII",
    "128"
)
$shellManualRelativePath = Resolve-ApplicationDocByTokens -Root $repoRoot -Tokens @(
    "COM54",
    "USART1",
    "full-diag",
    "shell=on",
    "reason=cmd_not_found rc=-2"
)

$requiredFiles = @(
    "applications\tasks\shell_task.h",
    "applications\tasks\shell_task.c",
    "applications\tasks\app_shell_task.h",
    "applications\tasks\app_shell_task.c",
    "applications\shell\app_shell_banner.h",
    "applications\shell\app_shell_banner.c",
    "bsp\ports\bsp_console.h",
    "bsp\ports\bsp_console.c",
    "tests\host\bsp\test_bsp_console_contract.c",
    "tests\host\subsys\test_tm_shell_task.c",
    "tests\CMakeLists.txt",
    "scripts\run_host_tests.ps1",
    "scripts\flows\host\run_host_tests.ps1",
    "scripts\flows\board\run_diag_carrier_witness.ps1",
    "scripts\flows\board\run_tiny_carrier_regression.ps1",
    "scripts\check_themis_quality_gate.ps1",
    "scripts\gates\check_themis_quality_gate.ps1"
)

foreach ($file in $requiredFiles) {
    Assert-Exists -Root $repoRoot -RelativePath $file -Errors $errors
}

if ($null -eq $transportDocRelativePath) {
    $errors.Add("ERROR: unable to resolve FalconFoc-lite transport governance doc by content tokens")
} else {
    Assert-Exists -Root $repoRoot -RelativePath $transportDocRelativePath -Errors $errors
}

if ($null -eq $shellDocRelativePath) {
    $errors.Add("ERROR: unable to resolve FalconFoc-lite shell output governance doc by content tokens")
} else {
    Assert-Exists -Root $repoRoot -RelativePath $shellDocRelativePath -Errors $errors
}

if ($null -eq $shellManualRelativePath) {
    $errors.Add("ERROR: unable to resolve FalconFoc-lite shell manual by content tokens")
} else {
    Assert-Exists -Root $repoRoot -RelativePath $shellManualRelativePath -Errors $errors
}

Assert-Contains -Root $repoRoot `
                -RelativePath "applications\tasks\shell_task.h" `
                -Markers @(
                    "USART1 DMA RX buffering",
                    "app_shell_task"
                ) `
                -Errors $errors

Assert-Contains -Root $repoRoot `
                -RelativePath "applications\tasks\shell_task.c" `
                -Markers @(
                    "tm_shell_task_init(",
                    "tm_shell_task_set_banner_hook(app_shell_banner_print);",
                    "tm_shell_task_submit_line(",
                    "tm_shell_task_notify_transport_ready();",
                    "USART1_IRQHandler",
                    "SHELL_TX_OVERFLOW_NOTICE"
                ) `
                -Errors $errors

Assert-Omits -Root $repoRoot `
             -RelativePath "applications\tasks\shell_task.c" `
             -Markers @(
                 "uart_task",
                 "tm_uart_read(",
                 "TM_UART_RX_NON_BLOCKING"
             ) `
             -Errors $errors

Assert-Contains -Root $repoRoot `
                -RelativePath "applications\tasks\app_shell_task.h" `
                -Markers @(
                    "tm_shell_task_diag_snapshot",
                    "tm_shell_task_set_banner_hook",
                    "tm_shell_task_notify_transport_ready"
                ) `
                -Errors $errors

Assert-Contains -Root $repoRoot `
                -RelativePath "applications\tasks\app_shell_task.c" `
                -Markers @(
                    "TM_SHELL_TASK_QUEUE_DEPTH",
                    "tm_shell_task_host_process_once",
                    "_tm_shell_task_wait_transport_ready",
                    'tm_task_budget_probe_register("shell"'
                ) `
                -Errors $errors

Assert-Contains -Root $repoRoot `
                -RelativePath "tests\host\bsp\test_bsp_console_contract.c" `
                -Markers @(
                    "bsp_console_shell_rx_start(",
                    "bsp_console_write_poll(",
                    "bsp_console_write_dma("
                ) `
                -Errors $errors

Assert-Contains -Root $repoRoot `
                -RelativePath "tests\host\subsys\test_tm_shell_task.c" `
                -Markers @(
                    "tm_shell_task_init()",
                    "tm_shell_task_notify_transport_ready()",
                    "tm_shell_task_submit_line(",
                    "tm_shell_task_host_process_once()"
                ) `
                -Errors $errors

Assert-Contains -Root $repoRoot `
                -RelativePath "tests\CMakeLists.txt" `
                -Markers @(
                    '"bsp/ports/bsp_console.c"',
                    'test_bsp_console_contract',
                    '"applications/tasks/app_shell_task.c"',
                    'test_tm_shell_task'
                ) `
                -Errors $errors

Assert-Contains -Root $repoRoot `
                -RelativePath "scripts\flows\host\run_host_tests.ps1" `
                -Markers @(
                    '"applications/tasks/app_shell_task.c"',
                    '"test_tm_shell_task"'
                ) `
                -Errors $errors

Assert-Contains -Root $repoRoot `
                -RelativePath "scripts\gates\check_themis_quality_gate.ps1" `
                -Markers @(
                    "Invoke-TransportShellHardeningContract",
                    "check_transport_shell_hardening_contract.ps1"
                ) `
                -Errors $errors

$shellCommandFiles = @(Get-ChildItem -Path (Join-Path $repoRoot "applications\shell") -Filter "app_shell_cmd_*.c")
foreach ($file in $shellCommandFiles) {
    $raw = Get-Content -LiteralPath $file.FullName -Encoding UTF8 -Raw
    if ($raw -match 'motor->') {
        $relative = To-RepoRelativePath -Root $repoRoot -FullPath $file.FullName
        $errors.Add("ERROR: $relative must not access live product state through direct motor-> writes or reads")
    }
}

$faultRuntimeRaw = Get-Content -LiteralPath (Join-Path $repoRoot "applications\runtime\motor_fault_runtime.c") -Encoding UTF8 -Raw
if ($faultRuntimeRaw -match 'axis_state\s*=') {
    $errors.Add("ERROR: applications/runtime/motor_fault_runtime.c must not write axis_state directly")
}

Assert-Contains -Root $repoRoot `
                -RelativePath $transportDocRelativePath `
                -Markers @(
                    "shell_task",
                    "app_shell_task",
                    "USART1",
                    "console-first",
                    "VOFA",
                    "debug-only",
                    "COM54"
                ) `
                -Errors $errors

Assert-Contains -Root $repoRoot `
                -RelativePath $shellDocRelativePath `
                -Markers @(
                    "console",
                    "repeated row",
                    "ASCII",
                    "shell_task",
                    "app_shell_task",
                    "reason=snapshot_unavailable",
                    "reason=idle_only",
                    "reason=redirect target=cfg",
                    "usage='Show command list'",
                    "next='help'",
                    "queue_used",
                    "submit_ok_count"
                ) `
                -Errors $errors

Assert-Contains -Root $repoRoot `
                -RelativePath "applications\shell\app_shell_banner.c" `
                -Markers @(
                    "build_date=%s build_time=%s shell=%s flavor=%s",
                    "uart=USART1 baud=1000000 transport=dma",
                    "next='help'"
                ) `
                -Errors $errors

Assert-Contains -Root $repoRoot `
                -RelativePath "subsys\shell\tm_shell.c" `
                -Markers @(
                    "group=%s usage='%s'",
                    "name=help group=Built-in usage='Show command list'",
                    "reason=%s rc=-2",
                    "cmd_not_found"
                ) `
                -Errors $errors

Assert-Contains -Root $repoRoot `
                -RelativePath $shellManualRelativePath `
                -Markers @(
                    "build_date=",
                    "build_time=",
                    "next='help'",
                    "version=",
                    "data=",
                    "shell=on",
                    "queue_used",
                    "queue_depth",
                    "submit_ok_count",
                    "submit_fail_count",
                    "reason=redirect target=cfg",
                    "reason=idle_only",
                    "reason=cmd_not_found rc=-2"
                ) `
                -Errors $errors

Assert-Omits -Root $repoRoot `
             -RelativePath $shellManualRelativePath `
             -Markers @(
                 "help=type 'help'",
                 "shell=1",
                 "queue=<used>/<depth>",
                 "owner=cfg",
                 "policy=idle-only"
             ) `
             -Errors $errors

Assert-Contains -Root $repoRoot `
                -RelativePath "scripts\flows\board\run_diag_carrier_witness.ps1" `
                -Markers @(
                    "shell=on",
                    "uart=USART1",
                    "baud=1000000",
                    "transport=dma",
                    "state show",
                    "obs all",
                    "diag pid all show",
                    "diag param show",
                    "store save sys",
                    "store status"
                ) `
                -Errors $errors

Assert-Omits -Root $repoRoot `
             -RelativePath "scripts\flows\board\run_diag_carrier_witness.ps1" `
             -Markers @(
                 "shell=1",
                 "policy=idle-only",
                 "[cal] cmd=start result=ok",
                 "transport=uart1-dma",
                 "diag pid vel kp 0.1000",
                 "diag param rs 0.2300"
             ) `
             -Errors $errors

Assert-Contains -Root $repoRoot `
                -RelativePath "scripts\flows\board\run_tiny_carrier_regression.ps1" `
                -Markers @(
                    "shell=on"
                ) `
                -Errors $errors

Assert-Omits -Root $repoRoot `
             -RelativePath "scripts\flows\board\run_tiny_carrier_regression.ps1" `
             -Markers @(
                 "shell=1"
             ) `
             -Errors $errors

foreach ($entry in $errors) {
    Write-Host $entry
}

Write-Host ("[transport_shell_hardening] checked {0} required files plus live shell contract markers" -f `
    $requiredFiles.Count)
Write-Host ("[transport_shell_hardening] {0} errors, 0 warnings" -f $errors.Count)

if ($errors.Count -gt 0) {
    exit 1
}
