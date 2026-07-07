param(
    [string]$RepoRoot = "",
    [ValidateSet("cmake", "components", "subsys", "bsp", "applications", "all")]
    [string]$Scope = "all",
    [switch]$Summary,
    [switch]$RequireCMakeFormatter
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

$script:GateLayerSummary = [ordered]@{}

function Resolve-AbsolutePath
{
    param([string]$Path)

    return (Resolve-Path -LiteralPath $Path).Path
}

function Resolve-ThemisPaths
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

    $gitRoot = (& git -C $baseRoot rev-parse --show-toplevel).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to resolve git root for '$baseRoot'."
    }

    return @{
        GitRoot  = $gitRoot
        RepoRoot = $baseRoot
    }
}

function Invoke-ChildScript
{
    param(
        [string]$ScriptPath,
        [hashtable]$ParameterMap
    )

    Write-Host ("[quality_gate] run: " + $ScriptPath)
    & $ScriptPath @ParameterMap
    if ($LASTEXITCODE -ne 0) {
        throw "Child script failed: $ScriptPath"
    }
}

function Set-GateLayerSummary
{
    param(
        [string]$Label,
        [int]$Errors,
        [int]$Warnings
    )

    $script:GateLayerSummary[$Label] = @{
        Errors   = $Errors
        Warnings = $Warnings
    }
}

function Write-GateLayerSummary
{
    param([bool]$Passed)

    foreach ($label in @("App")) {
        if (-not $script:GateLayerSummary.Contains($label)) {
            continue
        }

        $entry = $script:GateLayerSummary[$label]
        Write-Host ("{0,-10}{1} errors, {2} warnings" -f ("[{0}]" -f $label),
                    $entry.Errors,
                    $entry.Warnings)
    }

    Write-Host ("{0,-10}{1}" -f "[Total]", ($(if ($Passed) { "PASS" } else { "FAIL" })))
}

function Invoke-DiffCheck
{
    param([string]$ResolvedGitRoot)

    $arguments = @(
        "-C",
        $ResolvedGitRoot,
        "diff",
        "--check",
        "--",
        ".",
        ":(exclude).codex_encrypted_work/**",
        ":(exclude)build/**",
        ":(exclude)build-arm/**",
        ":(exclude)build-host/**"
    )

    Write-Host "[quality_gate] run: git diff --check"
    & git @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "git diff --check failed."
    }
}

function Invoke-CMakeQuality
{
    param(
        [string]$ResolvedGitRoot,
        [bool]$FormatterRequired
    )

    $scriptPath = Join-Path $ResolvedGitRoot "scripts\\gates\\check_cmake_quality.ps1"
    $params = @{ RepoRoot = $ResolvedGitRoot }
    if ($FormatterRequired) {
        $params.RequireFormatter = $true
    }
    Invoke-ChildScript -ScriptPath $scriptPath -ParameterMap $params
}

function Invoke-CMakePresetContract
{
    param([string]$ResolvedGitRoot)

    $scriptPath = Join-Path $ResolvedGitRoot "scripts\\gates\\check_cmake_presets.ps1"
    Invoke-ChildScript -ScriptPath $scriptPath -ParameterMap @{ RepoRoot = $ResolvedGitRoot }
}

function Invoke-PlatformBackendContract
{
    param([string]$ResolvedGitRoot)

    $scriptPath = Join-Path $ResolvedGitRoot "scripts\\gates\\check_platform_backend_contract.ps1"
    Invoke-ChildScript -ScriptPath $scriptPath -ParameterMap @{ RepoRoot = $ResolvedGitRoot }
}

function Invoke-FirmwareCliContract
{
    param([string]$ResolvedRepoRoot)

    $scriptPath = Join-Path $ResolvedRepoRoot "tests\\script\\test_firmware_cli_contract.ps1"
    Invoke-ChildScript -ScriptPath $scriptPath -ParameterMap @{ RepoRoot = $ResolvedRepoRoot }
}

function Invoke-CMakeSmoke
{
    param([string]$ResolvedRepoRoot)

    $scriptPath = Join-Path $ResolvedRepoRoot "scripts\\gates\\check_cmake_smoke.ps1"
    Invoke-ChildScript -ScriptPath $scriptPath -ParameterMap @{ RepoRoot = $ResolvedRepoRoot }
}

function Invoke-TransportShellHardeningContract
{
    param([string]$ResolvedGitRoot)

    $scriptPath = Join-Path $ResolvedGitRoot "scripts\\gates\\check_transport_shell_hardening_contract.ps1"
    Invoke-ChildScript -ScriptPath $scriptPath -ParameterMap @{ RepoRoot = $ResolvedGitRoot }
}

function Invoke-HostCacheGuardRegression
{
    param(
        [string]$ResolvedGitRoot,
        [string]$ResolvedRepoRoot
    )

    $scriptPath = Join-Path $ResolvedRepoRoot "tests\\script\\test_cmake_cache_guard.ps1"
    Invoke-ChildScript -ScriptPath $scriptPath -ParameterMap @{ RepoRoot = $ResolvedGitRoot }
}

function Invoke-FalconFocLiteContract
{
    param([string]$ResolvedRepoRoot)

    $scriptPath = Join-Path $ResolvedRepoRoot "tests\\script\\test_falconfoc_themis_lite_contract.ps1"
    Invoke-ChildScript -ScriptPath $scriptPath -ParameterMap @{ RepoRoot = $ResolvedRepoRoot }
    Set-GateLayerSummary -Label "App" -Errors 0 -Warnings 0
}

function Invoke-HostTests
{
    param(
        [string]$ResolvedGitRoot,
        [string]$ResolvedRepoRoot
    )

    $scriptPath = Join-Path $ResolvedRepoRoot "scripts\\flows\\host\\run_host_tests.ps1"
    Invoke-ChildScript -ScriptPath $scriptPath -ParameterMap @{ RepoRoot = $ResolvedGitRoot }
}

function Write-ManualGateReminder
{
    Write-Host "[quality_gate] automatic checks finished. Please still confirm:"
    Write-Host "[quality_gate] 1. Design docs, README entry points, and maintenance log are updated."
    Write-Host "[quality_gate] 2. Owner, lifecycle, and resource budget were reviewed."
    Write-Host "[quality_gate] 3. Real hardware paths still need the minimal board-level smoke witness."
}

$paths = Resolve-ThemisPaths -Root $RepoRoot
$requireFormatter = [bool]$RequireCMakeFormatter

if ((-not $requireFormatter) -and (($env:GITHUB_ACTIONS -eq "true") -or ($env:CI -eq "true"))) {
    $requireFormatter = $true
}

Write-Host ("[quality_gate] git root : " + $paths.GitRoot)
Write-Host ("[quality_gate] repo root: " + $paths.RepoRoot)
Write-Host ("[quality_gate] scope    : " + $Scope)
Write-Host ("[quality_gate] mode     : falconfoc-lite")

$emitSummary = [bool]$Summary -or ($Scope -in @("subsys", "bsp", "applications", "all"))
$gatePassed  = $false

try {
    Invoke-DiffCheck -ResolvedGitRoot $paths.GitRoot
    Invoke-CMakeQuality -ResolvedGitRoot $paths.GitRoot -FormatterRequired $requireFormatter
    Invoke-CMakePresetContract -ResolvedGitRoot $paths.GitRoot
    Invoke-PlatformBackendContract -ResolvedGitRoot $paths.GitRoot
    Invoke-CMakeSmoke -ResolvedRepoRoot $paths.RepoRoot
    Invoke-FirmwareCliContract -ResolvedRepoRoot $paths.RepoRoot
    Invoke-HostCacheGuardRegression -ResolvedGitRoot $paths.GitRoot -ResolvedRepoRoot $paths.RepoRoot
    Invoke-TransportShellHardeningContract -ResolvedGitRoot $paths.GitRoot

    switch ($Scope) {
        "cmake" {
        }
        "components" {
            Invoke-HostTests -ResolvedGitRoot $paths.GitRoot -ResolvedRepoRoot $paths.RepoRoot
        }
        "subsys" {
            Invoke-HostTests -ResolvedGitRoot $paths.GitRoot -ResolvedRepoRoot $paths.RepoRoot
        }
        "bsp" {
            Invoke-FalconFocLiteContract -ResolvedRepoRoot $paths.RepoRoot
        }
        "applications" {
            Invoke-FalconFocLiteContract -ResolvedRepoRoot $paths.RepoRoot
            Invoke-HostTests -ResolvedGitRoot $paths.GitRoot -ResolvedRepoRoot $paths.RepoRoot
        }
        "all" {
            Invoke-FalconFocLiteContract -ResolvedRepoRoot $paths.RepoRoot
            Invoke-HostTests -ResolvedGitRoot $paths.GitRoot -ResolvedRepoRoot $paths.RepoRoot
        }
    }

    Write-ManualGateReminder
    Write-Host "[quality_gate] all requested checks passed."
    $gatePassed = $true
} finally {
    if ($emitSummary -and ($script:GateLayerSummary.Count -gt 0)) {
        Write-GateLayerSummary -Passed $gatePassed
    }
}
