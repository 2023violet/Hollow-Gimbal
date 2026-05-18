param(
    [string]$RepoRoot = "",
    [switch]$RequireFormatter
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

function Resolve-AbsolutePath
{
    param([string]$Path)

    return (Resolve-Path -LiteralPath $Path).Path
}

function Get-RelativePathCompat
{
    param(
        [string]$BasePath,
        [string]$TargetPath
    )

    $resolvedBase   = (Resolve-AbsolutePath -Path $BasePath).TrimEnd('\') + '\'
    $resolvedTarget = Resolve-AbsolutePath -Path $TargetPath
    $baseUri        = New-Object System.Uri($resolvedBase)
    $targetUri      = New-Object System.Uri($resolvedTarget)

    return [System.Uri]::UnescapeDataString($baseUri.MakeRelativeUri($targetUri).ToString())
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

function Get-ThemisCMakeFiles
{
    param([string]$RepoRoot)

    $relativePaths = @(
        "CMakeLists.txt",
        "applications/CMakeLists.txt",
        "bsp/CMakeLists.txt",
        "bsp/board/CMakeLists.txt",
        "bsp/ports/CMakeLists.txt",
        "components/algorithm/CMakeLists.txt",
        "components/protocol/CMakeLists.txt",
        "components/utils/CMakeLists.txt",
        "kernel/freertos/CMakeLists.txt",
        "platform/at/at32f415/CMakeLists.txt",
        "platform/template/cortex_m4_ref/CMakeLists.txt",
        "subsys/CMakeLists.txt",
        "tests/CMakeLists.txt",
        "cmake/ThemisBuild.cmake",
        "tools/cmake/arm-gcc-toolchain.cmake"
    )

    return $relativePaths | ForEach-Object { Join-Path $RepoRoot $_ }
}

function Assert-ThemisHeaderComment
{
    param([string]$Path)

    $lines         = Get-Content -LiteralPath $Path -Encoding UTF8
    $headerWindow  = $lines | Select-Object -First 8
    $firstNonEmpty = $headerWindow | Where-Object { $_.Trim() -ne "" } | Select-Object -First 1

    if (($null -eq $firstNonEmpty) -or ($firstNonEmpty -notmatch '^#\s*Themis\b')) {
        throw "Missing Themis header comment in '$Path'."
    }
}

function Assert-ChildEntryRules
{
    param(
        [string]$Path,
        [string]$RepoRoot
    )

    $resolvedPath = Resolve-AbsolutePath -Path $Path
    $topLevelEntry = Resolve-AbsolutePath -Path (Join-Path $RepoRoot "CMakeLists.txt")
    $helperEntry   = Resolve-AbsolutePath -Path (Join-Path $RepoRoot "cmake\\ThemisBuild.cmake")
    $toolchainFile = Resolve-AbsolutePath -Path (Join-Path $RepoRoot "tools\\cmake\\arm-gcc-toolchain.cmake")
    $topLevelFiles = @(
        $topLevelEntry,
        $helperEntry,
        $toolchainFile
    )
    if ($resolvedPath -in $topLevelFiles) {
        return
    }

    $relativePath = (Get-RelativePathCompat -BasePath $RepoRoot -TargetPath $Path).Replace('\', '/')
    $content = Get-Content -LiteralPath $Path -Encoding UTF8 -Raw

    if ($content -match '(?im)^\s*cmake_minimum_required\s*\(') {
        throw "Child CMake file must not call cmake_minimum_required(): $relativePath"
    }

    if ($content -match '(?im)^\s*project\s*\(') {
        throw "Child CMake file must not call project(): $relativePath"
    }
}

function Get-CMakeFormatCommand
{
    $formatter = Get-Command cmake-format -ErrorAction SilentlyContinue
    if ($null -ne $formatter) {
        return @($formatter.Source)
    }

    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($null -eq $python) {
        return $null
    }

    & $python.Source -c "import cmakelang.format" 2>$null
    if ($LASTEXITCODE -eq 0) {
        return @($python.Source, "-m", "cmakelang.format")
    }

    return $null
}

function Invoke-ResolvedCommand
{
    param(
        [string[]]$CommandParts,
        [string[]]$Arguments
    )

    if (($null -eq $CommandParts) -or ($CommandParts.Count -eq 0)) {
        throw "Resolved command is empty."
    }

    $invokeArgs = @()
    if ($CommandParts.Count -gt 1) {
        $invokeArgs += $CommandParts[1..($CommandParts.Count - 1)]
    }
    $invokeArgs += $Arguments

    & $CommandParts[0] @invokeArgs
}

function Invoke-CMakeFormatCheck
{
    param(
        [string[]]$Files,
        [bool]$FormatterRequired,
        [string]$ConfigPath
    )

    $formatterCommand = Get-CMakeFormatCommand
    if ($null -eq $formatterCommand) {
        if ($FormatterRequired) {
            throw "cmake-format was not found. Install with 'python -m pip install --user cmakelang' or make the current Python environment importable as 'cmakelang.format'."
        }

        Write-Host "[cmake_quality] warn: cmake-format was not found, skip formatter check. Install with 'python -m pip install --user cmakelang' when formatter enforcement is desired."
        return
    }

    if (-not (Test-Path -LiteralPath $ConfigPath)) {
        throw "Missing cmake-format config: $ConfigPath"
    }

    foreach ($file in $Files) {
        Write-Host ("[cmake_quality] format-check: " + $file)
        Invoke-ResolvedCommand `
            -CommandParts $formatterCommand `
            -Arguments @("--config-files", $ConfigPath, "--check", $file)
        if ($LASTEXITCODE -ne 0) {
            throw "cmake-format --check failed: $file"
        }
    }
}

$paths      = Resolve-ThemisPaths -Root $RepoRoot
$cmakeFiles = Get-ThemisCMakeFiles -RepoRoot $paths.RepoRoot
$formatConfig = Join-Path $paths.RepoRoot ".cmake-format"

Write-Host ("[cmake_quality] git root : " + $paths.GitRoot)
Write-Host ("[cmake_quality] repo root: " + $paths.RepoRoot)

foreach ($file in $cmakeFiles) {
    if (-not (Test-Path -LiteralPath $file)) {
        throw "Declared Themis CMake file does not exist: $file"
    }

    Assert-ThemisHeaderComment -Path $file
    Assert-ChildEntryRules -Path $file -RepoRoot $paths.RepoRoot
}

Invoke-CMakeFormatCheck `
    -Files $cmakeFiles `
    -FormatterRequired ([bool]$RequireFormatter) `
    -ConfigPath $formatConfig

Write-Host "[cmake_quality] all requested checks passed."
