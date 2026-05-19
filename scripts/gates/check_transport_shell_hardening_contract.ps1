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

$requiredFiles = @(
    "applications\tasks\shell_task.h",
    "applications\tasks\shell_task.c",
    "applications\tasks\app_shell_task.h",
    "applications\tasks\app_shell_task.c",
    "applications\shell\app_shell_banner.h",
    "applications\shell\app_shell_banner.c",
    "tests\host\subsys\test_tm_shell_task.c",
    "tests\CMakeLists.txt",
    "scripts\run_host_tests.ps1",
    "scripts\flows\host\run_host_tests.ps1",
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
                    "app_shell_task"
                ) `
                -Errors $errors

foreach ($entry in $errors) {
    Write-Host $entry
}

Write-Host ("[transport_shell_hardening] checked {0} files and {1} contract groups" -f `
    $requiredFiles.Count, `
    10)
Write-Host ("[transport_shell_hardening] {0} errors, 0 warnings" -f $errors.Count)

if ($errors.Count -gt 0) {
    exit 1
}
