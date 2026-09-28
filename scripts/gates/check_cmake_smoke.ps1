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

function Invoke-CMakeConfigureSmoke
{
    param(
        [string]$RepoRoot,
        [string]$PresetName,
        [string]$BinaryDir,
        [string[]]$ExpectedTokens,
        [string[]]$ExtraArguments = @()
    )

    $arguments = @("--preset", $PresetName, "-B", $BinaryDir) + $ExtraArguments

    Write-Host ("[cmake_smoke] configure: cmake " + ($arguments -join " "))
    $output = & cmake @arguments 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "CMake smoke configure failed for preset '$PresetName'."
    }

    foreach ($token in $ExpectedTokens) {
        if ($output -notmatch [regex]::Escape($token)) {
            throw "CMake smoke configure '$PresetName' must emit token '$token'."
        }
    }
}

$paths = Resolve-ThemisPaths -Root $RepoRoot
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("themis_cmake_smoke_" + [System.Guid]::NewGuid().ToString("N"))

Write-Host ("[cmake_smoke] git root : " + $paths.GitRoot)
Write-Host ("[cmake_smoke] repo root: " + $paths.RepoRoot)

New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null

try {
    Push-Location $paths.RepoRoot

    Invoke-CMakeConfigureSmoke `
        -RepoRoot $paths.RepoRoot `
        -PresetName "host-debug" `
        -BinaryDir (Join-Path $tempRoot "host-debug") `
        -ExpectedTokens @(
            "Themis build firmware : OFF",
            "Themis build host tests : ON"
        )

    Invoke-CMakeConfigureSmoke `
        -RepoRoot $paths.RepoRoot `
        -PresetName "fw-full-diag-carrier-debug" `
        -BinaryDir (Join-Path $tempRoot "fw-full-diag-carrier-debug") `
        -ExpectedTokens @(
            "Themis platform : at/at32f415",
            "Themis CPU class : cortex-m3-soft"
        )

    Invoke-CMakeConfigureSmoke `
        -RepoRoot $paths.RepoRoot `
        -PresetName "fw-full-diag-carrier-debug" `
        -BinaryDir (Join-Path $tempRoot "fw-full-diag-at32f415") `
        -ExpectedTokens @(
            "Themis platform : at/at32f415",
            "Themis CPU class : cortex-m3-soft"
        ) `
        -ExtraArguments @("-DTHEMIS_PLATFORM_SOC=at32f415")
}
finally {
    Pop-Location
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}

Write-Host "[cmake_smoke] all requested checks passed."
