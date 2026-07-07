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

$runScriptPath = Join-Path $RepoRoot "run.sh"
$memoryScriptPath = Join-Path $RepoRoot "memory.sh"
$falconRunWrapperPath = Join-Path $RepoRoot "falcon-run.sh"
$falconMemoryWrapperPath = Join-Path $RepoRoot "falcon-memory.sh"

if (-not (Test-Path -LiteralPath $runScriptPath)) {
    throw "Canonical firmware CLI entry must exist at run.sh."
}
if (-not (Test-Path -LiteralPath $memoryScriptPath)) {
    throw "Canonical memory CLI entry must exist at memory.sh."
}

if (Test-Path -LiteralPath $falconRunWrapperPath) {
    throw "falcon-run.sh must stay retired once run.sh becomes the only formal firmware CLI entry."
}

if (Test-Path -LiteralPath $falconMemoryWrapperPath) {
    throw "falcon-memory.sh must stay retired once memory.sh becomes the only formal memory CLI entry."
}

$tasksJsonRaw = [System.IO.File]::ReadAllText(
    (Join-Path $RepoRoot ".vscode\tasks.json"),
    [System.Text.UTF8Encoding]::new($false)
)
if ($tasksJsonRaw -notmatch '\./run\.sh') {
    throw "VS Code build tasks must use the canonical run.sh entry."
}

$tinyRegressionRaw = [System.IO.File]::ReadAllText(
    (Join-Path $RepoRoot "scripts\flows\board\run_tiny_carrier_regression.ps1"),
    [System.Text.UTF8Encoding]::new($false)
)
if ($tinyRegressionRaw -notmatch '\./run\.sh all') {
    throw "run_tiny_carrier_regression.ps1 must build through ./run.sh all."
}
if ($tinyRegressionRaw -notmatch '\./run\.sh memory') {
    throw "run_tiny_carrier_regression.ps1 must capture memory evidence through ./run.sh memory."
}

$diagWitnessWrapperPath = Join-Path $RepoRoot "scripts\run_diag_carrier_witness.ps1"
if (-not (Test-Path -LiteralPath $diagWitnessWrapperPath)) {
    throw "Canonical diag board witness wrapper must exist at scripts/run_diag_carrier_witness.ps1."
}

$diagWitnessWrapperRaw = [System.IO.File]::ReadAllText(
    $diagWitnessWrapperPath,
    [System.Text.UTF8Encoding]::new($false)
)
if ($diagWitnessWrapperRaw -notmatch 'flows\\board\\run_diag_carrier_witness\.ps1') {
    throw "scripts/run_diag_carrier_witness.ps1 must delegate to the canonical board flow."
}

$diagWitnessRaw = [System.IO.File]::ReadAllText(
    (Join-Path $RepoRoot "scripts\flows\board\run_diag_carrier_witness.ps1"),
    [System.Text.UTF8Encoding]::new($false)
)
foreach ($token in @(
        "./run.sh all",
        "./run.sh memory",
        "THEMIS_DEVDRV_PROFILE=full-diag",
        "THEMIS_PRODUCT_FLAVOR=carrier-demo",
        "find_themis_shell_port.ps1",
        "sys reboot",
        "help",
        "sys status",
        "state show",
        "obs all",
        "diag pid all show",
        "diag param show",
        "store save sys",
        "store status",
        "board_witness"
    )) {
    if ($diagWitnessRaw -notmatch [regex]::Escape($token)) {
        throw "run_diag_carrier_witness.ps1 must keep the FalconFoc diag board witness token '$token'."
    }
}

function Resolve-BashExecutable {
    $candidates = @()

    $bashCommand = Get-Command bash -ErrorAction SilentlyContinue
    if ($null -ne $bashCommand) {
        $candidates += $bashCommand.Source
    }

    $candidates += @(
        "D:\Git\Git\bin\bash.exe",
        "C:\Program Files\Git\bin\bash.exe",
        "C:\Program Files (x86)\Git\bin\bash.exe"
    )

    foreach ($candidate in $candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and (Test-Path -LiteralPath $candidate)) {
            return $candidate
        }
    }

    return $null
}

$bashExecutable = Resolve-BashExecutable
if ($null -eq $bashExecutable) {
    throw "Bash is required to execute the firmware CLI contract smoke."
}

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("themis_cli_contract_" + [System.Guid]::NewGuid().ToString("N"))
$mockBin = Join-Path $tempRoot "bin"
$mockLogsDir = Join-Path $tempRoot "logs"
$mockBuildDir = "build_cli_contract_" + [System.Guid]::NewGuid().ToString("N")
$mockBuildLog = Join-Path $mockLogsDir "invocations.log"
$mockOpenOcdExe = Join-Path $mockBin "openocd.exe"
$mockRepoBuildPath = Join-Path $RepoRoot $mockBuildDir
$mockBuildWarningMarker = Join-Path $tempRoot "warn-once.marker"
$envBackup = @{
    PATH                   = $env:PATH
    THEMIS_BUILD_DIR       = $env:THEMIS_BUILD_DIR
    THEMIS_BUILD_TYPE      = $env:THEMIS_BUILD_TYPE
    THEMIS_DEVDRV_PROFILE  = $env:THEMIS_DEVDRV_PROFILE
    THEMIS_PRODUCT_FLAVOR  = $env:THEMIS_PRODUCT_FLAVOR
    THEMIS_ENABLE_BOARD_BENCH = $env:THEMIS_ENABLE_BOARD_BENCH
    THEMIS_BUILD_JOBS      = $env:THEMIS_BUILD_JOBS
    THEMIS_OPENOCD_BIN     = $env:THEMIS_OPENOCD_BIN
    THEMIS_CLI_LOG_DIR     = $env:THEMIS_CLI_LOG_DIR
    THEMIS_MOCK_LOG        = $env:THEMIS_MOCK_LOG
    THEMIS_MOCK_BUILD_OUTPUT_MODE = $env:THEMIS_MOCK_BUILD_OUTPUT_MODE
    THEMIS_MOCK_BUILD_WARNING_ONCE = $env:THEMIS_MOCK_BUILD_WARNING_ONCE
    THEMIS_MOCK_BUILD_WARNING_MARKER = $env:THEMIS_MOCK_BUILD_WARNING_MARKER
}

function New-BashStub {
    param(
        [string]$Path,
        [string]$Body
    )

    $content = @(
        "#!/usr/bin/env bash"
        "set -euo pipefail"
        $Body.Trim()
        ""
    ) -join "`n"
    [System.IO.File]::WriteAllText($Path, $content, [System.Text.UTF8Encoding]::new($false))
}

function Get-MockInvocationCount {
    param(
        [string]$LogPath,
        [string]$Prefix
    )

    if (-not (Test-Path -LiteralPath $LogPath)) {
        return 0
    }

    return @(
        Get-Content -LiteralPath $LogPath -Encoding UTF8 |
            Where-Object { $_ -like "$Prefix*" }
    ).Count
}

function New-TestFirmwareCliCache {
    param(
        [string]$CachePath,
        [string]$BuildType,
        [string]$Profile,
        [string]$Flavor,
        [string]$BoardBench,
        [string]$Firmware,
        [string]$HostTests,
        [string]$ToolchainPath,
        [string]$CacheDirValue,
        [string]$HomeDirValue
    )

    $toolchainPath = $ToolchainPath -replace '\\', '/'
    $cacheDirValue = $CacheDirValue -replace '\\', '/'
    $homeDirValue = $HomeDirValue -replace '\\', '/'

    New-Item -ItemType Directory -Force -Path (Split-Path -Path $CachePath -Parent) | Out-Null
    @(
        "CMAKE_BUILD_TYPE:STRING=$BuildType"
        "THEMIS_DEVDRV_PROFILE:STRING=$Profile"
        "THEMIS_PRODUCT_FLAVOR:STRING=$Flavor"
        "THEMIS_ENABLE_BOARD_BENCH:BOOL=$BoardBench"
        "THEMIS_BUILD_FIRMWARE:BOOL=$Firmware"
        "THEMIS_BUILD_HOST_TESTS:BOOL=$HostTests"
        "THEMIS_FIRMWARE_TARGET_NAME:STRING=falcon-foc-at32"
        "CMAKE_TOOLCHAIN_FILE:FILEPATH=$toolchainPath"
        "CMAKE_CACHEFILE_DIR:INTERNAL=$cacheDirValue"
        "CMAKE_HOME_DIRECTORY:INTERNAL=$homeDirValue"
    ) | Set-Content -LiteralPath $CachePath -Encoding UTF8
}

try {
    New-Item -ItemType Directory -Force -Path $mockBin | Out-Null
    New-Item -ItemType Directory -Force -Path $mockLogsDir | Out-Null
    if (Test-Path -LiteralPath $mockRepoBuildPath) {
        Remove-Item -LiteralPath $mockRepoBuildPath -Recurse -Force
    }

    New-BashStub -Path (Join-Path $mockBin "cmake") -Body @'
printf 'cmake:%s\n' "$*" >> "${THEMIS_MOCK_LOG}"
if [[ "${1:-}" == "--build" ]]; then
    build_dir="${2:-}"
    mkdir -p "${build_dir}/output"
    : > "${build_dir}/.ninja_deps"
    : > "${build_dir}/.ninja_log"
    printf 'ELF\n' > "${build_dir}/output/falcon-foc-at32.elf"

    if [[ "${THEMIS_MOCK_BUILD_OUTPUT_MODE:-}" == "no-work" ]]; then
        printf '[0/2] Re-checking globbed directories...\n'
        printf 'ninja: no work to do.\n'
    fi

    if [[ "${THEMIS_MOCK_BUILD_WARNING_ONCE:-}" == "1" ]] && [[ -n "${THEMIS_MOCK_BUILD_WARNING_MARKER:-}" ]] && [[ ! -f "${THEMIS_MOCK_BUILD_WARNING_MARKER}" ]]; then
        : > "${THEMIS_MOCK_BUILD_WARNING_MARKER}"
        printf 'ninja: warning: premature end of file; recovering\n'
    fi
    exit 0
fi

build_dir=""
build_type=""
profile=""
flavor=""
board_bench="OFF"
firmware="ON"
host_tests="OFF"
toolchain=""
source_dir="."
for ((idx = 1; idx <= $#; idx++)); do
    arg="${!idx}"
    case "${arg}" in
        -S)
            next=$((idx + 1))
            source_dir="${!next}"
            ;;
        -B)
            next=$((idx + 1))
            build_dir="${!next}"
            ;;
        -DCMAKE_BUILD_TYPE=*)
            build_type="${arg#-DCMAKE_BUILD_TYPE=}"
            ;;
        -DTHEMIS_DEVDRV_PROFILE=*)
            profile="${arg#-DTHEMIS_DEVDRV_PROFILE=}"
            ;;
        -DTHEMIS_PRODUCT_FLAVOR=*)
            flavor="${arg#-DTHEMIS_PRODUCT_FLAVOR=}"
            ;;
        -DTHEMIS_ENABLE_BOARD_BENCH=*)
            board_bench="${arg#-DTHEMIS_ENABLE_BOARD_BENCH=}"
            ;;
        -DTHEMIS_BUILD_FIRMWARE=*)
            firmware="${arg#-DTHEMIS_BUILD_FIRMWARE=}"
            ;;
        -DTHEMIS_BUILD_HOST_TESTS=*)
            host_tests="${arg#-DTHEMIS_BUILD_HOST_TESTS=}"
            ;;
        -DCMAKE_TOOLCHAIN_FILE=*)
            toolchain="${arg#-DCMAKE_TOOLCHAIN_FILE=}"
            ;;
    esac
done

mkdir -p "${build_dir}"
build_dir_abs="$(cd "${build_dir}" && pwd)"
source_dir_abs="$(cd "${source_dir}" && pwd)"
if [[ -n "${toolchain}" ]] && [[ ! "${toolchain}" =~ ^([A-Za-z]:[\\/]|/) ]]; then
    toolchain="$(cd "$(dirname "${toolchain}")" && pwd)/$(basename "${toolchain}")"
fi
cat > "${build_dir}/CMakeCache.txt" <<EOF
CMAKE_BUILD_TYPE:STRING=${build_type}
THEMIS_DEVDRV_PROFILE:STRING=${profile}
THEMIS_PRODUCT_FLAVOR:STRING=${flavor}
THEMIS_ENABLE_BOARD_BENCH:BOOL=${board_bench}
THEMIS_BUILD_FIRMWARE:BOOL=${firmware}
THEMIS_BUILD_HOST_TESTS:BOOL=${host_tests}
THEMIS_FIRMWARE_TARGET_NAME:STRING=falcon-foc-at32
CMAKE_TOOLCHAIN_FILE:FILEPATH=${toolchain}
CMAKE_CACHEFILE_DIR:INTERNAL=${build_dir_abs}
CMAKE_HOME_DIRECTORY:INTERNAL=${source_dir_abs}
EOF
'@

    New-BashStub -Path (Join-Path $mockBin "ninja") -Body @'
printf 'ninja:%s\n' "$*" >> "${THEMIS_MOCK_LOG}"
'@

    New-BashStub -Path (Join-Path $mockBin "arm-none-eabi-gcc") -Body @'
printf 'arm-none-eabi-gcc:%s\n' "$*" >> "${THEMIS_MOCK_LOG}"
'@

    New-BashStub -Path (Join-Path $mockBin "arm-none-eabi-size") -Body @'
if [[ "${1:-}" == "-A" ]]; then
    cat <<EOF
.text 100
.data 20
.bss 30
._user_heap_stack 40
EOF
else
    cat <<EOF
text data bss dec hex filename
100 20 30 150 96 falcon-foc-at32.elf
EOF
fi
'@

    New-BashStub -Path (Join-Path $mockBin "arm-none-eabi-nm") -Body @'
cat <<EOF
00000000 50 T app_main
00000010 20 D g_config
00000020 30 B g_state
00000030 40 R kBanner
EOF
'@

    New-BashStub -Path (Join-Path $mockBin "cygpath") -Body @'
if [[ "${1:-}" == "-m" ]]; then
    printf '%s\n' "${2:-}"
else
    printf '%s\n' "${1:-}"
fi
'@

    New-BashStub -Path $mockOpenOcdExe -Body @'
printf 'openocd:%s\n' "$*" >> "${THEMIS_MOCK_LOG}"
'@

    $env:PATH = "{0};{1}" -f $mockBin, $env:PATH
    $env:THEMIS_BUILD_DIR = $mockBuildDir
    $env:THEMIS_BUILD_TYPE = "MinSizeRel"
    $env:THEMIS_DEVDRV_PROFILE = "full-diag"
    $env:THEMIS_PRODUCT_FLAVOR = "carrier-demo"
    $env:THEMIS_ENABLE_BOARD_BENCH = "ON"
    $env:THEMIS_BUILD_JOBS = "4"
    $env:THEMIS_OPENOCD_BIN = $mockOpenOcdExe
    $env:THEMIS_CLI_LOG_DIR = $mockLogsDir
    $env:THEMIS_MOCK_LOG = $mockBuildLog
    $env:THEMIS_MOCK_BUILD_OUTPUT_MODE = ""
    $env:THEMIS_MOCK_BUILD_WARNING_ONCE = ""
    $env:THEMIS_MOCK_BUILD_WARNING_MARKER = $mockBuildWarningMarker

    & $bashExecutable -lc "cd '$RepoRoot' && ./run.sh all" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "First ./run.sh all smoke failed."
    }

    $firstCmakeCount = Get-MockInvocationCount -LogPath $mockBuildLog -Prefix "cmake:"
    if ($firstCmakeCount -ne 2) {
        throw "First ./run.sh all must issue exactly one configure and one build cmake invocation. Actual count: $firstCmakeCount."
    }
    $firstConfigureLine = Get-Content -LiteralPath $mockBuildLog -Encoding UTF8 |
        Where-Object { $_ -like "cmake:*" } |
        Select-Object -First 1
    if ($firstConfigureLine -notmatch '-DTHEMIS_DEVDRV_PROFILE=full-diag') {
        throw "First ./run.sh all must default the firmware profile to full-diag."
    }
    if ($firstConfigureLine -notmatch '-DTHEMIS_ENABLE_BOARD_BENCH=ON') {
        throw "First ./run.sh all must forward -DTHEMIS_ENABLE_BOARD_BENCH=ON into configure."
    }
    if ($firstConfigureLine -notmatch '-DTHEMIS_PLATFORM_VENDOR=at') {
        throw "First ./run.sh all must keep the live platform vendor default at 'at'."
    }
    if ($firstConfigureLine -notmatch '-DTHEMIS_PLATFORM_SOC=at32f403a') {
        throw "First ./run.sh all must keep the live platform SoC default at 'at32f403a'."
    }
    if ($firstConfigureLine -notmatch '-DTHEMIS_FIRMWARE_TARGET_NAME=falcon-foc-at32') {
        throw "First ./run.sh all must keep the canonical firmware target name aligned with the live preset baseline."
    }
    $firstOpenOcdCount = Get-MockInvocationCount -LogPath $mockBuildLog -Prefix "openocd:"
    if ($firstOpenOcdCount -ne 1) {
        throw "First ./run.sh all must flash exactly once. Actual count: $firstOpenOcdCount."
    }

    & $bashExecutable -lc "cd '$RepoRoot' && ./run.sh all" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Second ./run.sh all smoke failed."
    }

    $secondCmakeCount = Get-MockInvocationCount -LogPath $mockBuildLog -Prefix "cmake:"
    if ($secondCmakeCount -ne 3) {
        throw "Second ./run.sh all must reuse the build tree and avoid a second configure invocation. Actual count: $secondCmakeCount."
    }
    $secondOpenOcdCount = Get-MockInvocationCount -LogPath $mockBuildLog -Prefix "openocd:"
    if ($secondOpenOcdCount -ne 2) {
        throw "Second ./run.sh all must still flash exactly once per invocation. Actual count: $secondOpenOcdCount."
    }

    $env:THEMIS_MOCK_BUILD_OUTPUT_MODE = "no-work"
    $incrementalOutput = & $bashExecutable -lc "cd '$RepoRoot' && ./run.sh build" | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "Incremental ./run.sh build smoke failed."
    }
    if ($incrementalOutput -notmatch 'Incremental.*up to date') {
        throw "Incremental ./run.sh build must report that the build tree is already up to date."
    }

    [System.IO.File]::WriteAllText($mockBuildLog, "", [System.Text.UTF8Encoding]::new($false))
    $env:THEMIS_MOCK_BUILD_OUTPUT_MODE = ""
    if (Test-Path -LiteralPath $mockRepoBuildPath) {
        Remove-Item -LiteralPath $mockRepoBuildPath -Recurse -Force
    }

    $legacyRepoRoot = Join-Path $tempRoot "legacy_repo\retired_repo_root"
    New-TestFirmwareCliCache -CachePath (Join-Path $mockRepoBuildPath "CMakeCache.txt") `
                             -BuildType $env:THEMIS_BUILD_TYPE `
                             -Profile $env:THEMIS_DEVDRV_PROFILE `
                             -Flavor $env:THEMIS_PRODUCT_FLAVOR `
                             -BoardBench $env:THEMIS_ENABLE_BOARD_BENCH `
                             -Firmware "ON" `
                             -HostTests "OFF" `
                             -ToolchainPath (Join-Path $legacyRepoRoot "tools\cmake\arm-gcc-toolchain.cmake") `
                             -CacheDirValue (Join-Path $legacyRepoRoot $mockBuildDir) `
                             -HomeDirValue $legacyRepoRoot

    $relocatedOutput = & $bashExecutable -lc "cd '$RepoRoot' && ./run.sh build" | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "Relocated-cache ./run.sh build smoke failed."
    }
    if ($relocatedOutput -notmatch 'Resetting stale CMake configure metadata') {
        throw "Relocated-cache path must report stale CMake metadata recovery."
    }

    $relocatedCmakeCount = Get-MockInvocationCount -LogPath $mockBuildLog -Prefix "cmake:"
    if ($relocatedCmakeCount -ne 2) {
        throw "Relocated-cache ./run.sh build must issue one configure and one build. Actual count: $relocatedCmakeCount."
    }

    $relocatedCacheRaw = [System.IO.File]::ReadAllText(
        (Join-Path $mockRepoBuildPath "CMakeCache.txt"),
        [System.Text.UTF8Encoding]::new($false)
    )
    if ($relocatedCacheRaw -match [regex]::Escape(($legacyRepoRoot -replace '\\', '/'))) {
        throw "Relocated-cache recovery must rewrite the active cache away from the retired repo root."
    }

    [System.IO.File]::WriteAllText($mockBuildLog, "", [System.Text.UTF8Encoding]::new($false))
    $env:THEMIS_MOCK_BUILD_OUTPUT_MODE = ""
    $env:THEMIS_MOCK_BUILD_WARNING_ONCE = "1"
    if (Test-Path -LiteralPath $mockBuildWarningMarker) {
        Remove-Item -LiteralPath $mockBuildWarningMarker -Force
    }
    if (Test-Path -LiteralPath $mockRepoBuildPath) {
        Remove-Item -LiteralPath $mockRepoBuildPath -Recurse -Force
    }

    $repairOutput = & $bashExecutable -lc "cd '$RepoRoot' && ./run.sh all" | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "Auto-repair ./run.sh all smoke failed."
    }
    if ($repairOutput -notmatch 'Repairing Ninja build metadata') {
        throw "Auto-repair path must report Ninja metadata recovery."
    }

    $repairCmakeCount = Get-MockInvocationCount -LogPath $mockBuildLog -Prefix "cmake:"
    if ($repairCmakeCount -ne 4) {
        throw "Auto-repair ./run.sh all must issue configure+build twice when Ninja metadata drift is detected. Actual count: $repairCmakeCount."
    }
    $repairOpenOcdCount = Get-MockInvocationCount -LogPath $mockBuildLog -Prefix "openocd:"
    if ($repairOpenOcdCount -ne 1) {
        throw "Auto-repair ./run.sh all must still flash exactly once. Actual count: $repairOpenOcdCount."
    }

    [System.IO.File]::WriteAllText($mockBuildLog, "", [System.Text.UTF8Encoding]::new($false))
    $env:THEMIS_MOCK_BUILD_WARNING_ONCE = ""
    $env:THEMIS_MOCK_BUILD_OUTPUT_MODE = "no-work"

    & $bashExecutable -lc "cd '$RepoRoot' && ./run.sh repair" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Explicit ./run.sh repair smoke failed."
    }

    $manualRepairCmakeCount = Get-MockInvocationCount -LogPath $mockBuildLog -Prefix "cmake:"
    if ($manualRepairCmakeCount -ne 2) {
        throw "Explicit ./run.sh repair must issue one configure and one build. Actual count: $manualRepairCmakeCount."
    }
    $manualRepairOpenOcdCount = Get-MockInvocationCount -LogPath $mockBuildLog -Prefix "openocd:"
    if ($manualRepairOpenOcdCount -ne 0) {
        throw "Explicit ./run.sh repair must not flash. Actual count: $manualRepairOpenOcdCount."
    }

    & $bashExecutable -lc "cd '$RepoRoot' && ./run.sh memory" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Canonical ./run.sh memory smoke failed."
    }

    $memoryReportPath = Join-Path $RepoRoot (Join-Path $mockBuildDir "output\memory_report.txt")
    if (-not (Test-Path -LiteralPath $memoryReportPath)) {
        throw "Canonical memory path must still generate build/output memory_report.txt under the selected build dir."
    }
} finally {
    foreach ($entry in $envBackup.GetEnumerator()) {
        if ($null -eq $entry.Value) {
            Remove-Item "Env:$($entry.Key)" -ErrorAction SilentlyContinue
        } else {
            Set-Item "Env:$($entry.Key)" $entry.Value
        }
    }

    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
    if (Test-Path -LiteralPath $mockRepoBuildPath) {
        Remove-Item -LiteralPath $mockRepoBuildPath -Recurse -Force
    }
}

Write-Host "[test_firmware_cli_contract] all assertions passed."
