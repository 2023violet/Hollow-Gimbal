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
$metadataHelperPath = Join-Path $paths.RepoRoot "cmake\ThemisPlatformMetadata.cmake"
$metadataHelperRaw = Get-Content -LiteralPath $metadataHelperPath -Encoding UTF8 -Raw

if ($rootCMake -notmatch [regex]::Escape("include(ThemisPlatformMetadata)")) {
    throw "Top-level CMakeLists.txt must load the shared ThemisPlatformMetadata helper."
}

foreach ($name in @(
        'THEMIS_PLATFORM_VENDOR',
        'THEMIS_PLATFORM_SOC',
        'THEMIS_PLATFORM_DIR_OVERRIDE',
        'THEMIS_PLATFORM_TARGET_OVERRIDE',
        'THEMIS_ENTRY_SOURCE_OVERRIDE',
        'THEMIS_STARTUP_SOURCE_OVERRIDE',
        'THEMIS_LINKER_SCRIPT_OVERRIDE',
        'THEMIS_PLATFORM_COMPILE_DEFINITIONS_OVERRIDE',
        'THEMIS_CPU_CLASS_OVERRIDE'
    )) {
    if ($metadataHelperRaw -notmatch $name) {
        throw "Shared ThemisPlatformMetadata helper must expose $name."
    }
}

if ($metadataHelperRaw -notmatch 'platform/\$\{THEMIS_PLATFORM_VENDOR\}/\$\{THEMIS_PLATFORM_SOC\}') {
    throw "Shared ThemisPlatformMetadata helper must derive the active platform path from THEMIS_PLATFORM_VENDOR/THEMIS_PLATFORM_SOC."
}

if ($rootCMake -notmatch '(?s)add_subdirectory\s*\(\s*"\$\{THEMIS_PLATFORM_DIR\}"') {
    throw "Top-level CMakeLists.txt must add the platform layer through THEMIS_PLATFORM_DIR."
}

if ($metadataHelperRaw -notmatch 'ThemisPlatformDefaults\.cmake') {
    throw "Shared ThemisPlatformMetadata helper must load platform metadata through ThemisPlatformDefaults.cmake."
}

foreach ($forbiddenRootToken in @(
        'startup_at32f415\.s',
        'AT32F415x._FLASH\.ld',
        'AT32F415C[BR]T7'
    )) {
    if ($rootCMake -match $forbiddenRootToken) {
        throw "Top-level CMakeLists.txt must not keep chip-specific startup/linker/part facts after platform metadata owner closure."
    }
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

Assert-PathExists -Path (Join-Path $realPlatformDir "CMakeLists.txt") `
                  -Message "Missing active FalconFoc-lite platform manifest."
Assert-PathExists -Path (Join-Path $realPlatformDir "ThemisPlatformDefaults.cmake") `
                  -Message "Missing active FalconFoc-lite platform metadata defaults."
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

$templateReadme = Get-Content -LiteralPath (Join-Path $templatePlatformDir "README.md") -Encoding UTF8 -Raw
if ($templateReadme -notmatch 'ThemisPlatformDefaults\.cmake') {
    throw "Template platform README must document the ThemisPlatformDefaults.cmake metadata contract."
}
if ($templateReadme -notmatch 'THEMIS_PLATFORM_CPU_CLASS_DEFAULT') {
    throw "Template platform README must document the THEMIS_PLATFORM_CPU_CLASS_DEFAULT metadata contract."
}

$cpuClassRoot = Join-Path $paths.RepoRoot "platform\cpu"
Assert-PathExists -Path $cpuClassRoot `
                  -Message "Missing CPU-class metadata root."

$requiredCpuClasses = @("cortex-m4f-hard", "cortex-m3-soft")
foreach ($cpuClassName in $requiredCpuClasses) {
    $cpuClassDir = Join-Path $cpuClassRoot $cpuClassName
    Assert-PathExists -Path $cpuClassDir `
                      -Message "Missing required CPU-class metadata directory '$cpuClassName'."
    Assert-PathExists -Path (Join-Path $cpuClassDir "ThemisCpuClassDefaults.cmake") `
                      -Message "Missing CPU-class metadata defaults for '$cpuClassName'."
}

function Assert-PlatformCpuClassBinding
{
    param(
        [string]$PlatformName,
        [string]$DefaultsPath
    )

    $raw = Get-Content -LiteralPath $DefaultsPath -Encoding UTF8 -Raw
    $match = [regex]::Match($raw, 'THEMIS_PLATFORM_CPU_CLASS_DEFAULT\s+"([^"]+)"')
    if (-not $match.Success) {
        throw "Platform '$PlatformName' must bind THEMIS_PLATFORM_CPU_CLASS_DEFAULT in $DefaultsPath."
    }

    $cpuClassName = $match.Groups[1].Value
    $cpuDefaults = Join-Path $cpuClassRoot (Join-Path $cpuClassName "ThemisCpuClassDefaults.cmake")
    Assert-PathExists -Path $cpuDefaults `
                      -Message "Platform '$PlatformName' binds missing CPU-class defaults '$cpuClassName'."

    foreach ($requiredToken in @(
            'CMSIS_device_header',
            'THEMIS_PLATFORM_FLASH_DRIVER_HEADER',
            'THEMIS_PLATFORM_FLASH_BASE_ADDR',
            'THEMIS_PLATFORM_FLASH_SIZE_BYTES',
            'THEMIS_PLATFORM_FLASH_SECTOR_SIZE_BYTES',
            'THEMIS_PLATFORM_FLASH_SECTOR_COUNT',
            'THEMIS_PLATFORM_MOTOR_SOFTIRQ_HANDLER',
            'THEMIS_PLATFORM_MOTOR_SOFTIRQ_INSTANCE'
        )) {
        if ($raw -notmatch $requiredToken) {
            throw "Platform '$PlatformName' must bind $requiredToken through platform compile definitions."
        }
    }
}

function Assert-PlatformManifestStyle
{
    param(
        [string]$PlatformName,
        [string]$ManifestPath
    )

    $raw = Get-Content -LiteralPath $ManifestPath -Encoding UTF8 -Raw

    if ($raw -notmatch '(?im)^\s*SOURCES\s*$') {
        throw "Platform '$PlatformName' manifest must use an explicit SOURCES list."
    }

    if ($raw -match '(?im)^\s*GLOBS\s*$') {
        throw "Platform '$PlatformName' manifest must not use broad GLOBS source discovery."
    }

    if ($raw -notmatch [regex]::Escape('EXCLUDE')) {
        throw "Platform '$PlatformName' manifest must explicitly exclude generated/src/main.c from the platform static library."
    }

    if ($raw -notmatch [regex]::Escape('generated/src/main.c')) {
        throw "Platform '$PlatformName' manifest must explicitly reference generated/src/main.c in its EXCLUDE list."
    }
}

Assert-PlatformCpuClassBinding -PlatformName "at32f415" `
                               -DefaultsPath (Join-Path $realPlatformDir "ThemisPlatformDefaults.cmake")
Assert-PlatformManifestStyle -PlatformName "at32f415" `
                             -ManifestPath (Join-Path $realPlatformDir "CMakeLists.txt")

$preparedPlatformDirs = @(Get-ChildItem -Path (Join-Path $paths.RepoRoot "platform\at") -Directory |
    Where-Object { $_.Name -ne "at32f415" })

foreach ($platformDir in $preparedPlatformDirs) {
    Assert-PathExists -Path (Join-Path $platformDir.FullName "CMakeLists.txt") `
                      -Message "Prepared platform '$($platformDir.Name)' must carry its own platform manifest."
    Assert-PathExists -Path (Join-Path $platformDir.FullName "ThemisPlatformDefaults.cmake") `
                      -Message "Prepared platform '$($platformDir.Name)' must carry its own platform metadata defaults."
    Assert-PathExists -Path (Join-Path $platformDir.FullName "generated\inc\tm_platform_foc_timing.h") `
                      -Message "Prepared platform '$($platformDir.Name)' must carry its platform timing seam."
    Assert-PlatformCpuClassBinding -PlatformName $platformDir.Name `
                                   -DefaultsPath (Join-Path $platformDir.FullName "ThemisPlatformDefaults.cmake")
    Assert-PlatformManifestStyle -PlatformName $platformDir.Name `
                                 -ManifestPath (Join-Path $platformDir.FullName "CMakeLists.txt")
}

Write-Host ("[platform_backend_contract] git root : " + $paths.GitRoot)
Write-Host ("[platform_backend_contract] repo root: " + $paths.RepoRoot)
Write-Host "[platform_backend_contract] all requested checks passed."
