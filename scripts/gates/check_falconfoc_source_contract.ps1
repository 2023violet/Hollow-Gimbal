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

function Resolve-RepoRoot
{
    param([string]$Root)

    if ($Root -ne "") {
        return Resolve-AbsolutePath -Path $Root
    }

    return (& git rev-parse --show-toplevel).Trim()
}

function Assert-NoTrailingWhitespace
{
    param(
        [string]$Path,
        [string[]]$Lines
    )

    for ($index = 0; $index -lt $Lines.Count; $index++) {
        if ($Lines[$index] -match "`t") {
            throw "Tab character found in '$Path' at line $($index + 1)."
        }
        if ($Lines[$index] -match "\s+$") {
            throw "Trailing whitespace found in '$Path' at line $($index + 1)."
        }
    }
}

function Assert-CSourceContract
{
    param([string]$Path)

    $content = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
    $lines = Get-Content -LiteralPath $Path -Encoding UTF8
    Assert-NoTrailingWhitespace -Path $Path -Lines $lines

    foreach ($token in @("@file", "@brief", "@version", "@date")) {
        if ($content -notmatch [regex]::Escape($token)) {
            throw "Missing '$token' in source header '$Path'."
        }
    }

    if ($content -notmatch '(?m)^/\* ---------------------------------------- end of file ----------------------------------------- \*/\s*$') {
        throw "Missing Themis end-of-file marker in '$Path'."
    }

    if ($Path -like "*.h") {
        foreach ($token in @(
                "Design overview",
                "Usage example",
                "Ownership / lifecycle / context",
                "Failure / recovery",
                "Public Types",
                "Public Functions"
            )) {
            if ($content -notmatch [regex]::Escape($token)) {
                throw "Missing public-header contract section '$token' in '$Path'."
            }
        }
        if ($content -notmatch '(?m)^#pragma once\s*$') {
            throw "Public header '$Path' must use #pragma once."
        }
    } else {
        foreach ($token in @("Implementation role|Test role", "Includes", "Private Helpers", "Public Functions|Test Entry")) {
            if ($content -notmatch $token) {
                throw "Missing source section '$token' in '$Path'."
            }
        }
    }
}

function Assert-CMakeContract
{
    param([string]$Path)

    $content = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
    $lines = Get-Content -LiteralPath $Path -Encoding UTF8
    Assert-NoTrailingWhitespace -Path $Path -Lines $lines

    $firstNonEmpty = $lines | Where-Object { $_.Trim() -ne "" } | Select-Object -First 1
    if (($null -eq $firstNonEmpty) -or ($firstNonEmpty -notmatch '^#\s*Themis\b')) {
        throw "Missing Themis CMake header in '$Path'."
    }
    if ($content -notmatch '(?im)^#.*Role:') {
        throw "Missing Role: declaration in CMake header '$Path'."
    }

    foreach ($match in [regex]::Matches($content, '(?im)^\s*add_(?:library|executable)\(\s*([A-Za-z0-9_-]+)')) {
        $targetName = $match.Groups[1].Value
        if ($targetName -match '_') {
            throw "CMake target '$targetName' in '$Path' is not kebab-case."
        }
    }
}

$resolvedRoot = Resolve-RepoRoot -Root $RepoRoot
$sourceRoots = @(
    (Join-Path $resolvedRoot "core"),
    (Join-Path $resolvedRoot "calibration"),
    (Join-Path $resolvedRoot "sensors"),
    (Join-Path $resolvedRoot "g3\np"),
    (Join-Path $resolvedRoot "g3\safe"),
    (Join-Path $resolvedRoot "runtime"),
    (Join-Path $resolvedRoot "firmware\at32f415"),
    (Join-Path $resolvedRoot "protocol")
)
$sourceFiles = @()
foreach ($sourceRoot in $sourceRoots) {
    if (-not (Test-Path -LiteralPath $sourceRoot)) {
        throw "Missing FalconFOC source root '$sourceRoot'."
    }
    $sourceFiles += Get-ChildItem -LiteralPath $sourceRoot -Recurse -File |
        Where-Object { $_.Extension -in @(".c", ".h") } |
        ForEach-Object { $_.FullName }
}

foreach ($sourceFile in ($sourceFiles | Sort-Object)) {
    Assert-CSourceContract -Path $sourceFile
}

foreach ($sourceRoot in $sourceRoots) {
    Assert-CMakeContract -Path (Join-Path $sourceRoot "CMakeLists.txt")
    $readmePath = Join-Path $sourceRoot "README.md"
    $readmeContent = Get-Content -LiteralPath $readmePath -Raw -Encoding UTF8
    if ($readmeContent -notmatch 'cmake -S') {
        throw "README '$readmePath' is missing a standalone build command."
    }
}

Write-Host ("[falconfoc_source_contract] checked C/H files: " + $sourceFiles.Count)
Write-Host ("[falconfoc_source_contract] checked CMake files: " + $sourceRoots.Count)
Write-Host ("[falconfoc_source_contract] checked README files: " + $sourceRoots.Count)
Write-Host "[falconfoc_source_contract] all requested checks passed."
