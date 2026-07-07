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
        "platform/at/at32f403a/CMakeLists.txt",
        "platform/template/cortex_m4_ref/CMakeLists.txt",
        "subsys/CMakeLists.txt",
        "tests/CMakeLists.txt",
        "cmake/ThemisBuild.cmake",
        "cmake/ThemisBuildAxes.cmake",
        "cmake/ThemisFirmwareAssembly.cmake",
        "cmake/ThemisContracts.cmake",
        "cmake/ThemisStatusReport.cmake",
        "cmake/ThemisPlatformMetadata.cmake",
        "tools/cmake/arm-gcc-toolchain.cmake"
    )

    $files = $relativePaths | ForEach-Object { Join-Path $RepoRoot $_ }
    $platformDefaults = Get-ChildItem -Path (Join-Path $RepoRoot "platform") `
                                      -Filter "ThemisPlatformDefaults.cmake" `
                                      -Recurse `
                                      -File `
                                      -ErrorAction SilentlyContinue |
        ForEach-Object { $_.FullName }
    $cpuDefaults = Get-ChildItem -Path (Join-Path $RepoRoot "platform") `
                                 -Filter "ThemisCpuClassDefaults.cmake" `
                                 -Recurse `
                                 -File `
                                 -ErrorAction SilentlyContinue |
        ForEach-Object { $_.FullName }

    return @($files + $platformDefaults + $cpuDefaults)
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

function Assert-NoFalconFocFeatureOptionsInSharedBuild
{
    param([string]$RepoRoot)

    $checks = @(
        @{
            Path = Join-Path $RepoRoot "CMakeLists.txt"
            Forbidden = @(
                "THEMIS_FEATURE_SHELL",
                "THEMIS_FEATURE_IPC",
                "THEMIS_FEATURE_VOFA",
                "THEMIS_FEATURE_TASK_BUDGET",
                "THEMIS_FEATURE_VERBOSE_LOG",
                "THEMIS_LINKER_MIN_HEAP_SIZE",
                "THEMIS_LINKER_MIN_STACK_SIZE",
                "configure_file("
            )
        },
        @{
            Path = Join-Path $RepoRoot "subsys\CMakeLists.txt"
            Forbidden = @(
                "THEMIS_FEATURE_SHELL",
                "THEMIS_FEATURE_IPC",
                "THEMIS_FEATURE_TASK_BUDGET"
            )
        },
        @{
            Path = Join-Path $RepoRoot "applications\CMakeLists.txt"
            Forbidden = @(
                "THEMIS_FEATURE_SHELL",
                "THEMIS_FEATURE_VOFA"
            )
        }
    )

    foreach ($check in $checks) {
        $raw = [System.IO.File]::ReadAllText($check.Path, [System.Text.UTF8Encoding]::new($false))
        foreach ($token in $check.Forbidden) {
            if ($raw -match [regex]::Escape($token)) {
                throw "FalconFoc product feature policy must not live in shared/root CMake: '$token' found in '$($check.Path)'."
            }
        }
    }
}

function Assert-SharedMetadataHelperUsage
{
    param([string]$RepoRoot)

    $helperRelativePath = "cmake/ThemisPlatformMetadata.cmake"
    $helperPath = Join-Path $RepoRoot $helperRelativePath
    if (-not (Test-Path -LiteralPath $helperPath)) {
        throw "Missing shared CMake metadata helper: $helperRelativePath"
    }

    $rootCMakePath = Join-Path $RepoRoot "CMakeLists.txt"
    $toolchainPath = Join-Path $RepoRoot "tools\cmake\arm-gcc-toolchain.cmake"
    $rootRaw = [System.IO.File]::ReadAllText($rootCMakePath, [System.Text.UTF8Encoding]::new($false))
    $toolchainRaw = [System.IO.File]::ReadAllText($toolchainPath, [System.Text.UTF8Encoding]::new($false))

    if ($rootRaw -notmatch [regex]::Escape("include(ThemisPlatformMetadata)")) {
        throw "Top-level CMakeLists.txt must load the shared ThemisPlatformMetadata helper."
    }

    if ($toolchainRaw -notmatch [regex]::Escape("ThemisPlatformMetadata.cmake")) {
        throw "ARM GCC toolchain entry must load the shared ThemisPlatformMetadata helper."
    }

    foreach ($duplicateToken in @(
            "function(themis_resolve_path_with_base",
            "function(themis_resolve_repo_path",
            "function(themis_require_default",
            "function(themis_toolchain_resolve_path_with_base",
            "function(themis_toolchain_require_default"
        )) {
        if ($rootRaw -match [regex]::Escape($duplicateToken)) {
            throw "Top-level CMakeLists.txt must not keep duplicated metadata helper token '$duplicateToken'."
        }
        if ($toolchainRaw -match [regex]::Escape($duplicateToken)) {
            throw "ARM GCC toolchain entry must not keep duplicated metadata helper token '$duplicateToken'."
        }
    }
}

function Assert-RootHelperBoundaries
{
    param([string]$RepoRoot)

    $rootCMakePath = Join-Path $RepoRoot "CMakeLists.txt"
    $rootRaw = [System.IO.File]::ReadAllText($rootCMakePath, [System.Text.UTF8Encoding]::new($false))

    $requiredHelperFiles = @(
        "cmake/ThemisBuildAxes.cmake",
        "cmake/ThemisFirmwareAssembly.cmake",
        "cmake/ThemisContracts.cmake",
        "cmake/ThemisStatusReport.cmake"
    )
    foreach ($helperRelativePath in $requiredHelperFiles) {
        $helperPath = Join-Path $RepoRoot $helperRelativePath
        if (-not (Test-Path -LiteralPath $helperPath)) {
            throw "Missing phase-2 CMake helper: $helperRelativePath"
        }
    }

    $orderedTokens = @(
        "include(ThemisBuild)",
        "include(ThemisPlatformMetadata)",
        "include(ThemisBuildAxes)",
        "include(ThemisFirmwareAssembly)",
        "include(ThemisContracts)",
        "include(ThemisStatusReport)",
        "themis_build_axes_declare_cache()",
        "themis_metadata_declare_platform_selection_cache()",
        "themis_metadata_declare_platform_override_cache()",
        "themis_build_axes_validate()",
        "themis_firmware_assemble(",
        "themis_define_contract_targets(",
        "themis_emit_status_report()"
    )

    $lastIndex = -1
    foreach ($token in $orderedTokens) {
        $tokenIndex = $rootRaw.IndexOf($token, [System.StringComparison]::Ordinal)
        if ($tokenIndex -lt 0) {
            throw "Top-level CMakeLists.txt must contain root helper token '$token'."
        }
        if ($tokenIndex -lt $lastIndex) {
            throw "Top-level CMakeLists.txt must keep helper token '$token' after the previous root assembly stage."
        }
        $lastIndex = $tokenIndex
    }

    foreach ($forbiddenToken in @(
            "themis_metadata_load_platform_defaults(",
            "themis_metadata_load_cpu_defaults(",
            "add_library(themis_root_headers",
            "add_library(themis_cpu_class_contract",
            "add_library(themis_build_contract",
            "add_library(themis_platform_contract",
            "add_library(themis_host_test_contract",
            'message(STATUS "Themis build firmware :',
            'message(STATUS "Themis platform :'
        )) {
        if ($rootRaw -match [regex]::Escape($forbiddenToken)) {
            throw "Top-level CMakeLists.txt must not keep phase-2 extracted implementation token '$forbiddenToken'."
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

Assert-NoFalconFocFeatureOptionsInSharedBuild -RepoRoot $paths.RepoRoot
Assert-SharedMetadataHelperUsage -RepoRoot $paths.RepoRoot
Assert-RootHelperBoundaries -RepoRoot $paths.RepoRoot

Invoke-CMakeFormatCheck `
    -Files $cmakeFiles `
    -FormatterRequired ([bool]$RequireFormatter) `
    -ConfigPath $formatConfig

Write-Host "[cmake_quality] all requested checks passed."
