param(
    [string]$RepoRoot = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
} else {
    $RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
}

$helperPath = Join-Path $RepoRoot "scripts\lib\cmake_cache_guard.ps1"
if (-not (Test-Path -LiteralPath $helperPath)) {
    throw "Host-test cache guard helper must exist at scripts\\lib\\cmake_cache_guard.ps1."
}

. $helperPath

$runHostTestsWrapperPath = Join-Path $RepoRoot "scripts\run_host_tests.ps1"
$runHostTestsFlowPath    = Join-Path $RepoRoot "scripts\flows\host\run_host_tests.ps1"
$tempRoot                = Join-Path ([System.IO.Path]::GetTempPath()) ("themis_cmake_cache_guard_" + [System.Guid]::NewGuid().ToString("N"))
$currentRoot             = Join-Path $tempRoot "current_repo"
$legacyRoot              = Join-Path $tempRoot "legacy_repo\retired_repo_root"
$buildDir                = Join-Path $currentRoot "build\host-debug"
$cachePath               = Join-Path $buildDir "CMakeCache.txt"

function New-TestCache {
    param(
        [string]$CachePath,
        [string]$CacheDirValue,
        [string]$HomeDirValue
    )

    $cacheDirValue = $CacheDirValue -replace '\\', '/'
    $homeDirValue  = $HomeDirValue -replace '\\', '/'

    New-Item -ItemType Directory -Force -Path (Split-Path -Path $CachePath -Parent) | Out-Null
    @(
        "# This is the CMakeCache file."
        ("CMAKE_CACHEFILE_DIR:INTERNAL={0}" -f $cacheDirValue)
        ("CMAKE_HOME_DIRECTORY:INTERNAL={0}" -f $homeDirValue)
    ) | Set-Content -LiteralPath $CachePath -Encoding UTF8
}

try {
    New-Item -ItemType Directory -Force -Path $currentRoot | Out-Null
    New-TestCache -CachePath $cachePath `
                  -CacheDirValue (Join-Path $legacyRoot "build\host-debug") `
                  -HomeDirValue $legacyRoot

    $repairResult = Repair-CMakeBuildCacheIfRelocated -ExpectedSourceDir $currentRoot `
                                                      -BuildDir $buildDir
    if (-not $repairResult.Repaired) {
        throw "Cache guard must repair relocated build trees."
    }
    if (Test-Path -LiteralPath $buildDir) {
        throw "Relocated build tree should be removed once the cache guard detects stale paths."
    }
    if (($repairResult.Reasons -join "`n") -notmatch "CMAKE_HOME_DIRECTORY") {
        throw "Cache guard must report the stale source-root marker."
    }
    if (($repairResult.Reasons -join "`n") -notmatch "CMAKE_CACHEFILE_DIR") {
        throw "Cache guard must report the stale build-root marker."
    }

    New-TestCache -CachePath $cachePath `
                  -CacheDirValue $buildDir `
                  -HomeDirValue $currentRoot

    $stableResult = Repair-CMakeBuildCacheIfRelocated -ExpectedSourceDir $currentRoot `
                                                      -BuildDir $buildDir
    if ($stableResult.Repaired) {
        throw "Cache guard must keep already-aligned build trees intact."
    }
    if (-not (Test-Path -LiteralPath $buildDir)) {
        throw "Aligned build tree should remain available after the guard runs."
    }

    $runHostTestsWrapperRaw = [System.IO.File]::ReadAllText($runHostTestsWrapperPath, [System.Text.UTF8Encoding]::new($false))
    if ($runHostTestsWrapperRaw -notmatch 'flows\\host\\run_host_tests\.ps1') {
        throw "scripts\\run_host_tests.ps1 must delegate to the canonical host flow."
    }

    $runHostTestsFlowRaw = [System.IO.File]::ReadAllText($runHostTestsFlowPath, [System.Text.UTF8Encoding]::new($false))
    if ($runHostTestsFlowRaw -notmatch 'scripts\\lib\\cmake_cache_guard\.ps1') {
        throw "Canonical run_host_tests flow must import the shared CMake cache guard helper."
    }
    if ($runHostTestsFlowRaw -notmatch 'Repair-CMakeBuildCacheIfRelocated -ExpectedSourceDir \$paths\.RepoRoot -BuildDir \$hostBuildDir') {
        throw "Canonical run_host_tests flow must repair relocated host preset caches before configuring CMake."
    }

    Write-Host "[test_cmake_cache_guard] all assertions passed."
}
finally {
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}
