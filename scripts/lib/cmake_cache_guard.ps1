function Convert-ToNormalizedCMakePath
{
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ""
    }

    $normalized = $Path -replace '\\', '/'
    $normalized = $normalized.TrimEnd('/')
    return $normalized.ToLowerInvariant()
}

function Get-CMakeCacheInternalValue
{
    param(
        [string]$CachePath,
        [string]$Key
    )

    if (-not (Test-Path -LiteralPath $CachePath)) {
        return ""
    }

    $pattern = '^{0}:INTERNAL=(?<value>.*)$' -f [regex]::Escape($Key)
    foreach ($line in Get-Content -LiteralPath $CachePath -Encoding UTF8) {
        if ($line -match $pattern) {
            return $Matches.value
        }
    }

    return ""
}

function Repair-CMakeBuildCacheIfRelocated
{
    param(
        [string]$ExpectedSourceDir,
        [string]$BuildDir
    )

    $result = @{
        Repaired = $false
        Reasons  = @()
    }

    if (-not (Test-Path -LiteralPath $BuildDir)) {
        return $result
    }

    $cachePath = Join-Path $BuildDir "CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cachePath)) {
        return $result
    }

    $expectedSource = Convert-ToNormalizedCMakePath -Path (Resolve-Path -LiteralPath $ExpectedSourceDir).Path
    $expectedBuild  = Convert-ToNormalizedCMakePath -Path (Resolve-Path -LiteralPath $BuildDir).Path
    $cachedSource   = Convert-ToNormalizedCMakePath -Path (Get-CMakeCacheInternalValue -CachePath $cachePath -Key "CMAKE_HOME_DIRECTORY")
    $cachedBuild    = Convert-ToNormalizedCMakePath -Path (Get-CMakeCacheInternalValue -CachePath $cachePath -Key "CMAKE_CACHEFILE_DIR")

    if (($cachedSource -ne "") -and ($cachedSource -ne $expectedSource)) {
        $result.Reasons += ("CMAKE_HOME_DIRECTORY drifted from '{0}' to '{1}'." -f $cachedSource,
                            $expectedSource)
    }

    if (($cachedBuild -ne "") -and ($cachedBuild -ne $expectedBuild)) {
        $result.Reasons += ("CMAKE_CACHEFILE_DIR drifted from '{0}' to '{1}'." -f $cachedBuild,
                            $expectedBuild)
    }

    if ($result.Reasons.Count -eq 0) {
        return $result
    }

    Write-Host ("[cmake_cache_guard] removing relocated build tree: " + $BuildDir)
    foreach ($reason in $result.Reasons) {
        Write-Host ("[cmake_cache_guard] reason: " + $reason)
    }

    Remove-Item -LiteralPath $BuildDir -Recurse -Force
    $result.Repaired = $true
    return $result
}
