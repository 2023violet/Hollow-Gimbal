param(
    [string]$RepoRoot = "",
    [switch]$RequireFormatter,
    [string]$SkipFormatter = ""
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
        (-not (Test-Path -LiteralPath (Join-Path $baseRoot "firmware\at32f415\CMakeLists.txt")))) {
        throw "Unable to resolve FalconFOC repository root from '$baseRoot'."
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
    # FalconFOC V2 standalone CMake projects and the shared ARM toolchain file.
    $relativePaths = @(
        "core/CMakeLists.txt",
        "g3/np/CMakeLists.txt",
        "g3/safe/CMakeLists.txt",
        "runtime/CMakeLists.txt",
        "simulation/CMakeLists.txt",
        "firmware/at32f415/CMakeLists.txt",
        "protocol/CMakeLists.txt",
        "tools/cmake/arm-gcc-toolchain.cmake"
    )
    return @($relativePaths | ForEach-Object { Join-Path $RepoRoot $_ })
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

    # The probe imports cmakelang to test availability; a failed import writes
    # a traceback to stderr, which PowerShell 5.1 turns into a terminating
    # error under $ErrorActionPreference="Stop". Relax EAP so the designed
    # "formatter absent -> skip enforcement" path stays reachable.
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $python.Source -c "import cmakelang.format" 2>$null
    } finally {
        $ErrorActionPreference = $prevEap
    }
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

    $effectiveConfig = $ConfigPath
    $temporaryConfig = $null
    if ([IO.Path]::GetFileName($ConfigPath) -eq ".cmake-format") {
        $temporaryConfig = Join-Path ([IO.Path]::GetTempPath()) ("themis-cmake-format-" + [Guid]::NewGuid().ToString("N") + ".py")
        Copy-Item -LiteralPath $ConfigPath -Destination $temporaryConfig
        $effectiveConfig = $temporaryConfig
    }
    try {
        foreach ($file in $Files) {
            Write-Host ("[cmake_quality] format-check: " + $file)
            $temporarySource = Join-Path ([IO.Path]::GetTempPath()) ("themis-cmake-input-" + [Guid]::NewGuid().ToString("N") + ".cmake")
            $crlf = [String]([char]13) + [String]([char]10)
            $lf = [String]([char]10)
            $sourceText = [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($file)).Replace($crlf, $lf)
            [IO.File]::WriteAllText($temporarySource, $sourceText, [Text.UTF8Encoding]::new($false))
            try {
                Invoke-ResolvedCommand -CommandParts $formatterCommand -Arguments @("--config-files", $effectiveConfig, "--check", $temporarySource)
                if ($LASTEXITCODE -ne 0) {
                    throw "cmake-format --check failed: $file"
                }
            } finally {
                Remove-Item -LiteralPath $temporarySource -Force -ErrorAction SilentlyContinue
            }
        }
    } finally {
        if ($null -ne $temporaryConfig) {
            Remove-Item -LiteralPath $temporaryConfig -Force -ErrorAction SilentlyContinue
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
}

$skipFormatterEnabled = $SkipFormatter -in @("1", "true", "True", "TRUE")
if ($skipFormatterEnabled) {
    Write-Host "[cmake_quality] formatter check skipped by the caller's scoped gate."
} else {
    Invoke-CMakeFormatCheck `
        -Files $cmakeFiles `
        -FormatterRequired ([bool]$RequireFormatter) `
        -ConfigPath $formatConfig
}

Write-Host "[cmake_quality] all requested checks passed."
