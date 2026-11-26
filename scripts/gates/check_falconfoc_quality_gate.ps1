param(
    [string]$RepoRoot = "",
    [switch]$RequireCMakeFormatter
)

# FalconFOC V2 quality gate: whitespace diff check, generated parameter files current,
# C/H/CMake/README source contract and CMake header/format rules. Host tests, SIL and ARM
# images run in their own lanes (fwinteg).

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

function Resolve-GateRoot
{
    param([string]$Root)

    if ($Root -ne "") {
        $base = (Resolve-Path -LiteralPath $Root).Path
    } else {
        $base = (& git rev-parse --show-toplevel).Trim()
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to locate git repository root."
        }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $base "firmware\at32f415\CMakeLists.txt"))) {
        throw "Unable to resolve FalconFOC repository root from '$base'."
    }
    return $base
}

function Invoke-ChildScript
{
    param(
        [string]$ScriptPath,
        [string[]]$Arguments
    )

    # A child process makes $LASTEXITCODE the child's own verdict, not the exit code of
    # its last native command.
    Write-Host ("[quality_gate] run: " + $ScriptPath)
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & powershell -NoProfile -ExecutionPolicy Bypass -File $ScriptPath @Arguments
    } finally {
        $ErrorActionPreference = $prevEap
    }
    if ($LASTEXITCODE -ne 0) {
        throw "Child script failed: $ScriptPath"
    }
}

$root = Resolve-GateRoot -Root $RepoRoot
$requireFormatter = [bool]$RequireCMakeFormatter -or ($env:GITHUB_ACTIONS -eq "true") -or ($env:CI -eq "true")
$passed = $false
Write-Host ("[quality_gate] repo root: " + $root)

try {
    Write-Host "[quality_gate] run: git diff --check"
    & git -C $root diff --check -- . ":(exclude)build/**" ":(exclude)tmp/**"
    if ($LASTEXITCODE -ne 0) {
        throw "git diff --check failed."
    }

    Write-Host "[quality_gate] run: gen_parameters.py --check"
    & python -B (Join-Path $root "scripts\gen\gen_parameters.py") --check
    if ($LASTEXITCODE -ne 0) {
        throw "Generated parameter files are stale: run python -B scripts/gen/gen_parameters.py."
    }

    Write-Host "[quality_gate] run: check_image_double.py"
    & python -B (Join-Path $root "scripts\gates\check_image_double.py") $root
    if ($LASTEXITCODE -ne 0) {
        throw "double outside the allowed calibration estimators (code style guide section 6)."
    }

    Invoke-ChildScript -ScriptPath (Join-Path $root "scripts\gates\check_falconfoc_source_contract.ps1") `
                       -Arguments @("-RepoRoot", $root)

    $cmakeArguments = @("-RepoRoot", $root)
    if ($requireFormatter) {
        $cmakeArguments += "-RequireFormatter"
    }
    Invoke-ChildScript -ScriptPath (Join-Path $root "scripts\gates\check_cmake_quality.ps1") `
                       -Arguments $cmakeArguments

    Write-Host "[quality_gate] all requested checks passed."
    $passed = $true
} finally {
    Write-Host ("{0,-10}{1}" -f "[Total]", $(if ($passed) { "PASS" } else { "FAIL" }))
}
