param(
    [string]$RepoRoot = "",
    [ValidateSet("cmake", "falconfoc", "components", "subsys", "bsp", "applications", "all")]
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
    # Run each child in a separate powershell process: `& script.ps1` executes
    # in-process, which leaves $LASTEXITCODE holding the child's last NATIVE
    # command exit code (e.g. an optional-tool probe) instead of the child's
    # own result. A child process makes $LASTEXITCODE the real child verdict.
    # Diagnostics on the child's stderr must not become terminating errors.
    # Across powershell -File, true switches need a bare name, not a bool value.
    # String-valued options (including SkipFormatter) must remain strings.
    $childArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $ScriptPath)
    foreach ($name in $ParameterMap.Keys) {
        $value = $ParameterMap[$name]
        if ($value -is [bool]) {
            if ($value) {
                $childArgs += "-$name"
            }
        } else {
            $childArgs += "-$name"
            $childArgs += [string]$value
        }
    }
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & powershell @childArgs
    } finally {
        $ErrorActionPreference = $prevEap
    }
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
        ":(exclude)build-host/**",
        # Vendor-generated sources carry tool-emitted trailing whitespace and
        # are not hand-reviewable code; keep them out of the whitespace gate.
        ":(exclude)platform/at/at32f415/generated/**",
        # work/ is out of scope per AGENTS.md (retired scratch/backup area).
        ":(exclude)work/**",
        ":(exclude)tmp/**"
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
        [bool]$FormatterRequired,
        [bool]$SkipFormatter
    )

    $scriptPath = Join-Path $ResolvedGitRoot "scripts\\gates\\check_cmake_quality.ps1"
    $params = @{ RepoRoot = $ResolvedGitRoot }
    if ($FormatterRequired) {
        $params.RequireFormatter = $true
    }
    if ($SkipFormatter) {
        $params.SkipFormatter = "true"
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

function Invoke-FalconFocCurrentContract
{
    param([string]$ResolvedRepoRoot)

    $scriptPath = Join-Path $ResolvedRepoRoot "tests\\script\\test_falconfoc_current_contract.ps1"
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

function Invoke-FalconFocSourceContract
{
    param([string]$ResolvedRepoRoot)

    $scriptPath = Join-Path $ResolvedRepoRoot "scripts\gates\check_falconfoc_source_contract.ps1"
    Invoke-ChildScript -ScriptPath $scriptPath -ParameterMap @{ RepoRoot = $ResolvedRepoRoot }
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
    if ($Scope -in @("falconfoc", "all")) {
        Invoke-FalconFocSourceContract -ResolvedRepoRoot $paths.RepoRoot
    }
    Invoke-CMakeQuality `
        -ResolvedGitRoot $paths.GitRoot `
        -FormatterRequired $requireFormatter `
        -SkipFormatter ($Scope -eq "falconfoc")
    Invoke-CMakePresetContract -ResolvedGitRoot $paths.GitRoot
    Invoke-PlatformBackendContract -ResolvedGitRoot $paths.GitRoot
    Invoke-CMakeSmoke -ResolvedRepoRoot $paths.RepoRoot
    if ($Scope -eq "falconfoc") {
        Write-Host "[quality_gate] skip firmware CLI contract for FalconFOC source scope."
    } else {
        Invoke-FirmwareCliContract -ResolvedRepoRoot $paths.RepoRoot
    }
    Invoke-HostCacheGuardRegression -ResolvedGitRoot $paths.GitRoot -ResolvedRepoRoot $paths.RepoRoot
    Invoke-TransportShellHardeningContract -ResolvedGitRoot $paths.GitRoot

    switch ($Scope) {
        "cmake" {
        }
        "falconfoc" {
        }
        "components" {
            Invoke-HostTests -ResolvedGitRoot $paths.GitRoot -ResolvedRepoRoot $paths.RepoRoot
        }
        "subsys" {
            Invoke-HostTests -ResolvedGitRoot $paths.GitRoot -ResolvedRepoRoot $paths.RepoRoot
        }
        "bsp" {
            Invoke-FalconFocCurrentContract -ResolvedRepoRoot $paths.RepoRoot
        }
        "applications" {
            Invoke-FalconFocCurrentContract -ResolvedRepoRoot $paths.RepoRoot
            Invoke-HostTests -ResolvedGitRoot $paths.GitRoot -ResolvedRepoRoot $paths.RepoRoot
        }
        "all" {
            Invoke-FalconFocCurrentContract -ResolvedRepoRoot $paths.RepoRoot
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
