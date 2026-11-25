param(
    [string]$RepoRoot = "",
    [ValidateSet("all", "c", "cmake")]
    [string]$Scope = "all",
    [ValidateSet("format", "check")]
    [string]$Mode = "format",
    [switch]$AllTracked,
    [switch]$BootstrapCMakeFormatter
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

function Get-RepoPathPrefix
{
    param(
        [string]$GitRoot,
        [string]$RepoRoot
    )

    if ((Resolve-AbsolutePath -Path $GitRoot) -ieq (Resolve-AbsolutePath -Path $RepoRoot)) {
        return ""
    }

    return ((Get-RelativePathCompat -BasePath $GitRoot -TargetPath $RepoRoot).Replace('\', '/').TrimEnd('/')) + "/"
}

function Get-GitRelativeCandidates
{
    param(
        [string]$GitRoot,
        [bool]$UseAllTracked
    )

    if ($UseAllTracked) {
        $tracked = (& git -C $GitRoot ls-files)
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to enumerate tracked files."
        }

        return $tracked | Where-Object { $_ -ne "" } | Sort-Object -Unique
    }

    $changed = (& git -C $GitRoot diff --name-only HEAD --)
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to enumerate changed tracked files."
    }

    $untracked = (& git -C $GitRoot ls-files --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to enumerate untracked files."
    }

    return ($changed + $untracked) | Where-Object { $_ -ne "" } | Sort-Object -Unique
}

function Convert-ToRepoRelativePaths
{
    param(
        [string[]]$GitRelativePaths,
        [string]$RepoPrefix
    )

    $result = @()
    foreach ($path in $GitRelativePaths) {
        $normalized = $path.Replace('\', '/')
        if (($RepoPrefix -eq "") -or $normalized.StartsWith($RepoPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            if ($RepoPrefix -eq "") {
                $result += $normalized
            } else {
                $result += $normalized.Substring($RepoPrefix.Length)
            }
        }
    }

    return $result | Sort-Object -Unique
}

function Get-ThemisOwnedCFiles
{
    param(
        [string]$RepoRoot,
        [string[]]$RepoRelativePaths
    )

    $result = @()
    foreach ($path in $RepoRelativePaths) {
        if ($path -notmatch '^(calibration|control_plane|core|examples|firmware/at32f415|g3/(np|safe)|protocol|runtime|sensors|simulation|tools)/.+\.(c|h)$') {
            continue
        }

        $candidate = Join-Path $RepoRoot $path
        if (Test-Path -LiteralPath $candidate) {
            $result += $candidate
        }
    }

    return $result | Sort-Object -Unique
}

function Get-ThemisCMakeFiles
{
    param(
        [string]$RepoRoot,
        [string[]]$RepoRelativePaths,
        [bool]$UseAllTracked
    )

    $officialPaths = @(
        "core/CMakeLists.txt",
        "calibration/CMakeLists.txt",
        "control_plane/CMakeLists.txt",
        "sensors/CMakeLists.txt",
        "g3/np/CMakeLists.txt",
        "g3/safe/CMakeLists.txt",
        "runtime/CMakeLists.txt",
        "simulation/CMakeLists.txt",
        "firmware/at32f415/CMakeLists.txt",
        "protocol/CMakeLists.txt",
        "tools/cmake/arm-gcc-toolchain.cmake"
    )

    $result = @()
    foreach ($relativePath in $officialPaths) {
        if ((-not $UseAllTracked) -and ($relativePath -notin $RepoRelativePaths)) {
            continue
        }

        $candidate = Join-Path $RepoRoot $relativePath
        if (Test-Path -LiteralPath $candidate) {
            $result += $candidate
        }
    }

    return $result | Sort-Object -Unique
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

    $invokeArgs = @()
    if ($CommandParts.Count -gt 1) {
        $invokeArgs += $CommandParts[1..($CommandParts.Count - 1)]
    }
    $invokeArgs += $Arguments

    & $CommandParts[0] @invokeArgs
}

function Install-CMakeFormatter
{
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($null -eq $python) {
        throw "python was not found. Install Python first or make cmake-format available on PATH."
    }

    Write-Host "[format_themis] installing cmakelang into the current user environment..."
    & $python.Source -m pip install --user cmakelang
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to install cmakelang."
    }
}

function New-SectionBanner
{
    param([string]$Title)

    $normalizedTitle = $Title.Trim()
    $targetWidth     = 85
    $prefix          = "/* " + $normalizedTitle + " "
    $suffix          = " */"
    $dashCount       = $targetWidth - $prefix.Length - $suffix.Length
    if ($dashCount -lt 8) {
        $dashCount = 8
    }

    return $prefix + ("-" * $dashCount) + $suffix
}

function Normalize-CSectionBanners
{
    param(
        [string[]]$Files,
        [string]$ModeName
    )

    if ($Files.Count -eq 0) {
        return
    }

    $brokenPattern = New-Object System.Text.RegularExpressions.Regex(
        '/\* (?<title>[^\r\n*]+?)\r?\n \* -+\r?\n \*/',
        [System.Text.RegularExpressions.RegexOptions]::Multiline
    )
    $eofPattern = New-Object System.Text.RegularExpressions.Regex(
        '/\* -+ end of file\r?\n \* -+ \*/',
        [System.Text.RegularExpressions.RegexOptions]::Multiline
    )
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)

    foreach ($file in $Files) {
        $relativePath = (Get-RelativePathCompat -BasePath $paths.RepoRoot -TargetPath $file).Replace('\', '/')
        $content      = [System.IO.File]::ReadAllText($file)
        $normalized   = $brokenPattern.Replace(
            $content,
            {
                param($match)
                return New-SectionBanner -Title $match.Groups['title'].Value
            }
        )
        $normalized   = $eofPattern.Replace(
            $normalized,
            '/* ---------------------------------------- end of file ----------------------------------------- */'
        )
        $normalized   = $normalized.TrimEnd("`r", "`n") + "`n"

        if ($ModeName -eq "check") {
            if ($normalized -cne $content) {
                throw ("banner normalization check failed: " + $relativePath)
            }
            continue
        }

        if ($normalized -cne $content) {
            Write-Host ("[format_themis] normalize-banners: " + $relativePath)
            [System.IO.File]::WriteAllText($file, $normalized, $utf8NoBom)
        }
    }
}

function Invoke-ClangFormatPass
{
    param(
        [string[]]$Files,
        [string]$ModeName,
        [string]$ClangFormatter
    )

    if ($Files.Count -eq 0) {
        Write-Host "[format_themis] no C/C header files selected."
        return
    }

    $failed = @()
    foreach ($file in $Files) {
        $relativePath = (Get-RelativePathCompat -BasePath $paths.RepoRoot -TargetPath $file).Replace('\', '/')
        Write-Host ("[format_themis] clang-" + $ModeName + ": " + $relativePath)

        if ($ModeName -eq "check") {
            & $ClangFormatter "--dry-run" "--Werror" "--style=file" $file
        } else {
            & $ClangFormatter "-i" "--style=file" $file
        }

        if ($LASTEXITCODE -ne 0) {
            $failed += $relativePath
        }
    }

    if ($failed.Count -gt 0) {
        throw ("clang-format " + $ModeName + " failed: " + ($failed -join ", "))
    }
}

function Invoke-CMakeFormatPass
{
    param(
        [string[]]$Files,
        [string]$ModeName,
        [string[]]$FormatterCommand,
        [string]$ConfigPath
    )

    if ($Files.Count -eq 0) {
        Write-Host "[format_themis] no official CMake files selected."
        return
    }

    if (-not (Test-Path -LiteralPath $ConfigPath)) {
        throw "Missing cmake-format config: $ConfigPath"
    }

    $failed = @()
    $effectiveConfig = $ConfigPath
    $temporaryConfig = $null
    if ([IO.Path]::GetFileName($ConfigPath) -eq ".cmake-format") {
        $temporaryConfig = Join-Path ([IO.Path]::GetTempPath()) ("themis-cmake-format-" + [Guid]::NewGuid().ToString("N") + ".py")
        Copy-Item -LiteralPath $ConfigPath -Destination $temporaryConfig
        $effectiveConfig = $temporaryConfig
    }
    try {
        foreach ($file in $Files) {
            $relativePath = (Get-RelativePathCompat -BasePath $paths.RepoRoot -TargetPath $file).Replace('\', '/')
            Write-Host ("[format_themis] cmake-" + $ModeName + ": " + $relativePath)

            $arguments = @("--config-files", $effectiveConfig)
            if ($ModeName -eq "check") {
                $arguments += @("--check", $file)
            } else {
                $arguments += @("-i", $file)
            }

            if ($ModeName -eq "check") {
                $temporarySource = Join-Path ([IO.Path]::GetTempPath()) ("themis-cmake-input-" + [Guid]::NewGuid().ToString("N") + ".cmake")
                $crlf = [String]([char]13) + [String]([char]10)
                $lf = [String]([char]10)
                $sourceText = [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($file)).Replace($crlf, $lf)
                [IO.File]::WriteAllText($temporarySource, $sourceText, [Text.UTF8Encoding]::new($false))
                try {
                    $arguments = @("--config-files", $effectiveConfig, "--check", $temporarySource)
                    Invoke-ResolvedCommand -CommandParts $FormatterCommand -Arguments $arguments
                } finally {
                    Remove-Item -LiteralPath $temporarySource -Force -ErrorAction SilentlyContinue
                }
            } else {
                Invoke-ResolvedCommand -CommandParts $FormatterCommand -Arguments $arguments
            }
            if ($LASTEXITCODE -ne 0) {
                $failed += $relativePath
            }
        }
    } finally {
        if ($null -ne $temporaryConfig) {
            Remove-Item -LiteralPath $temporaryConfig -Force -ErrorAction SilentlyContinue
        }
    }

    if ($failed.Count -gt 0) {
        throw ("cmake-format " + $ModeName + " failed: " + ($failed -join ", "))
    }
}

$paths         = Resolve-ThemisPaths -Root $RepoRoot
$repoPrefix    = Get-RepoPathPrefix -GitRoot $paths.GitRoot -RepoRoot $paths.RepoRoot
$gitCandidates = Get-GitRelativeCandidates -GitRoot $paths.GitRoot -UseAllTracked ([bool]$AllTracked)
$repoCandidates = Convert-ToRepoRelativePaths -GitRelativePaths $gitCandidates -RepoPrefix $repoPrefix
$clangFormat   = Join-Path $paths.RepoRoot "scripts\clang-format.cmd"
$cmakeConfig   = Join-Path $paths.RepoRoot ".cmake-format"

Write-Host ("[format_themis] git root : " + $paths.GitRoot)
Write-Host ("[format_themis] repo root: " + $paths.RepoRoot)
Write-Host ("[format_themis] scope    : " + $Scope)
Write-Host ("[format_themis] mode     : " + $Mode)
Write-Host ("[format_themis] target   : " + ($(if ($AllTracked) { "all tracked files" } else { "changed/untracked files" })))

if (-not (Test-Path -LiteralPath $clangFormat)) {
    throw "Missing clang-format launcher: $clangFormat"
}

if ($Scope -in @("all", "c")) {
    $cFiles = @(Get-ThemisOwnedCFiles -RepoRoot $paths.RepoRoot -RepoRelativePaths $repoCandidates)
    Normalize-CSectionBanners -Files $cFiles -ModeName $Mode
    Invoke-ClangFormatPass -Files $cFiles -ModeName $Mode -ClangFormatter $clangFormat
}

if ($Scope -in @("all", "cmake")) {
    $cmakeFiles = @(Get-ThemisCMakeFiles -RepoRoot $paths.RepoRoot -RepoRelativePaths $repoCandidates -UseAllTracked ([bool]$AllTracked))
    $cmakeCommand = Get-CMakeFormatCommand
    if (($null -eq $cmakeCommand) -and $BootstrapCMakeFormatter) {
        Install-CMakeFormatter
        $cmakeCommand = Get-CMakeFormatCommand
    }

    if ($null -eq $cmakeCommand) {
        throw "cmake-format is unavailable. Install with 'python -m pip install --user cmakelang' or rerun with -BootstrapCMakeFormatter."
    }

    Invoke-CMakeFormatPass -Files $cmakeFiles -ModeName $Mode -FormatterCommand $cmakeCommand -ConfigPath $cmakeConfig
}

Write-Host "[format_themis] completed."
