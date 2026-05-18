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

function Assert-PathExists
{
    param(
        [string]$Path,
        [string]$Message
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw $Message
    }
}

$paths = Resolve-ThemisPaths -Root $RepoRoot
$rootCMakePath = Join-Path $paths.RepoRoot "CMakeLists.txt"
$rootCMake = Get-Content -LiteralPath $rootCMakePath -Encoding UTF8 -Raw

if ($rootCMake -notmatch 'THEMIS_PLATFORM_DIR_OVERRIDE') {
    throw "Top-level CMakeLists.txt must expose THEMIS_PLATFORM_DIR_OVERRIDE."
}

if ($rootCMake -notmatch 'THEMIS_PLATFORM_TARGET_OVERRIDE') {
    throw "Top-level CMakeLists.txt must expose THEMIS_PLATFORM_TARGET_OVERRIDE."
}

foreach ($name in @(
        'THEMIS_ENTRY_SOURCE_OVERRIDE',
        'THEMIS_STARTUP_SOURCE_OVERRIDE',
        'THEMIS_LINKER_SCRIPT_OVERRIDE'
    )) {
    if ($rootCMake -notmatch $name) {
        throw "Top-level CMakeLists.txt must expose $name."
    }
}

if ($rootCMake -notmatch 'platform/\$\{THEMIS_PLATFORM_VENDOR\}/\$\{THEMIS_PLATFORM_SOC\}') {
    throw "Top-level CMakeLists.txt must derive the active platform path from THEMIS_PLATFORM_VENDOR/THEMIS_PLATFORM_SOC."
}

if ($rootCMake -notmatch '(?s)add_subdirectory\s*\(\s*"\$\{THEMIS_PLATFORM_DIR\}"') {
    throw "Top-level CMakeLists.txt must add the platform layer through THEMIS_PLATFORM_DIR."
}

foreach ($forbidden in @(
        'THEMIS_DRIVER_BACKEND_DIR',
        'THEMIS_DRIVER_BACKEND_TARGET'
    )) {
    if ($rootCMake -match $forbidden) {
        throw "FalconFoc-lite must not reintroduce $forbidden."
    }
}

if ($rootCMake -match 'add_subdirectory\s*\(\s*drivers/') {
    throw "FalconFoc-lite must not add a drivers/ backend layer."
}

$realPlatformDir = Join-Path $paths.RepoRoot "platform\at\at32f415"
$templatePlatformDir = Join-Path $paths.RepoRoot "platform\template\cortex_m4_ref"
$forbiddenPlatformDir = Join-Path $paths.RepoRoot "platform\at\at32f415"

Assert-PathExists -Path (Join-Path $realPlatformDir "CMakeLists.txt") `
                  -Message "Missing active FalconFoc-lite platform manifest."
Assert-PathExists -Path (Join-Path $realPlatformDir "generated\src\main.c") `
                  -Message "Missing active FalconFoc-lite generated entry source."
Assert-PathExists -Path (Join-Path $realPlatformDir "vendor\cmsis\cm4\device_support\startup\gcc\startup_at32f415.s") `
                  -Message "Missing active FalconFoc-lite startup source."
Assert-PathExists -Path (Join-Path $realPlatformDir "vendor\cmsis\cm4\device_support\startup\gcc\linker\AT32F415xB_FLASH.ld") `
                  -Message "Missing active FalconFoc-lite linker script."

Assert-PathExists -Path (Join-Path $templatePlatformDir "CMakeLists.txt") `
                  -Message "Missing template platform manifest."
Assert-PathExists -Path (Join-Path $templatePlatformDir "README.md") `
                  -Message "Missing template platform README."
Assert-PathExists -Path (Join-Path $templatePlatformDir "generated") `
                  -Message "Missing template platform generated/ boundary."
Assert-PathExists -Path (Join-Path $templatePlatformDir "vendor") `
                  -Message "Missing template platform vendor/ boundary."

if (Test-Path -LiteralPath $forbiddenPlatformDir) {
    throw "FalconFoc-lite must not keep stale active platform path platform/at/at32f415."
}

Write-Host ("[platform_backend_contract] git root : " + $paths.GitRoot)
Write-Host ("[platform_backend_contract] repo root: " + $paths.RepoRoot)
Write-Host "[platform_backend_contract] all requested checks passed."
