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

function Read-Utf8Raw {
    param(
        [string]$RelativePath
    )

    $path = Join-Path $RepoRoot $RelativePath
    return [System.IO.File]::ReadAllText($path, [System.Text.UTF8Encoding]::new($false))
}

function To-RepoRelativePath {
    param(
        [string]$FullPath
    )

    $repoPrefix = ($RepoRoot.TrimEnd('\', '/')) + [System.IO.Path]::DirectorySeparatorChar
    if ($FullPath.StartsWith($repoPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $FullPath.Substring($repoPrefix.Length)
    }

    return $FullPath
}

function Resolve-GovernanceDocByContent {
    param(
        [string]$RequiredToken
    )

    $matches = @(Get-ChildItem -Path (Join-Path $RepoRoot "docs\governance") -Filter "*.md" |
        Where-Object {
            $raw = [System.IO.File]::ReadAllText($_.FullName, [System.Text.UTF8Encoding]::new($false))
            $raw.Contains($RequiredToken)
        }
    )

    if ($matches.Count -ne 1) {
        throw "Expected exactly one governance doc containing '$RequiredToken'."
    }

    return To-RepoRelativePath $matches[0].FullName
}

function Resolve-GovernanceDocByTokens {
    param(
        [string[]]$RequiredTokens
    )

    $matches = @(Get-ChildItem -Path (Join-Path $RepoRoot "docs\governance") -Filter "*.md" |
        Where-Object {
            $raw = [System.IO.File]::ReadAllText($_.FullName, [System.Text.UTF8Encoding]::new($false))
            foreach ($token in $RequiredTokens) {
                if ($raw.IndexOf($token, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
                    return $false
                }
            }
            return $true
        }
    )

    if ($matches.Count -ne 1) {
        throw "Expected exactly one governance doc containing token set: $($RequiredTokens -join ', ')."
    }

    return To-RepoRelativePath $matches[0].FullName
}

function Resolve-ApplicationDocByTokens {
    param(
        [string[]]$RequiredTokens
    )

    $matches = @(Get-ChildItem -Path (Join-Path $RepoRoot "docs\application") -Filter "*.md" |
        Where-Object {
            $raw = [System.IO.File]::ReadAllText($_.FullName, [System.Text.UTF8Encoding]::new($false))
            foreach ($token in $RequiredTokens) {
                if ($raw.IndexOf($token, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
                    return $false
                }
            }
            return $true
        }
    )

    if ($matches.Count -ne 1) {
        throw "Expected exactly one application doc containing token set: $($RequiredTokens -join ', ')."
    }

    return To-RepoRelativePath $matches[0].FullName
}

function Read-Utf8Absolute {
    param(
        [string]$Path
    )

    return [System.IO.File]::ReadAllText($Path, [System.Text.UTF8Encoding]::new($false))
}

function Resolve-RequiredTool {
    param(
        [string]$Name
    )

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        throw "Required tool '$Name' was not found in PATH."
    }

    return $command.Source
}

function Invoke-ExternalText {
    param(
        [string]$ToolPath,
        [string[]]$Arguments
    )

    $output = & $ToolPath @Arguments 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed: $ToolPath $($Arguments -join ' ')"
    }

    return $output
}

function Get-FirmwareArtifactSet {
    param(
        [string]$PresetName
    )

    $outputDir = Join-Path $RepoRoot "build\$PresetName\output"

    return @{
        Preset    = $PresetName
        OutputDir = $outputDir
        Elf       = Join-Path $outputDir "falcon-foc-at32.elf"
        Map       = Join-Path $outputDir "falcon-foc-at32.map"
    }
}

function Assert-FirmwareArtifactSetExists {
    param(
        [hashtable]$Artifacts
    )

    foreach ($path in @($Artifacts.OutputDir, $Artifacts.Elf, $Artifacts.Map)) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Missing FalconFoc firmware artifact '$path'. Build preset '$($Artifacts.Preset)' before running this contract."
        }
    }
}

function Get-SectionSizeBytes {
    param(
        [string]$ObjdumpOutput,
        [string]$SectionName
    )

    $pattern = '(?m)^\s*\d+\s+' + [regex]::Escape($SectionName) + '\s+([0-9A-Fa-f]+)\b'
    $match   = [regex]::Match($ObjdumpOutput, $pattern)
    if (-not $match.Success) {
        throw "Unable to find section '$SectionName' in objdump output."
    }

    return [Convert]::ToUInt32($match.Groups[1].Value, 16)
}

function Assert-RegexAbsent {
    param(
        [string]$Text,
        [string]$Pattern,
        [string]$Message
    )

    $options = [System.Text.RegularExpressions.RegexOptions]::IgnoreCase -bor
        [System.Text.RegularExpressions.RegexOptions]::Multiline
    if ([regex]::IsMatch($Text, $Pattern, $options)) {
        throw $Message
    }
}

function Assert-RegexPresent {
    param(
        [string]$Text,
        [string]$Pattern,
        [string]$Message
    )

    $options = [System.Text.RegularExpressions.RegexOptions]::IgnoreCase -bor
        [System.Text.RegularExpressions.RegexOptions]::Multiline
    if (-not [regex]::IsMatch($Text, $Pattern, $options)) {
        throw $Message
    }
}

function Assert-FalconFocFirmwareContracts {
    $nmTool      = Resolve-RequiredTool "arm-none-eabi-nm"
    $objdumpTool = Resolve-RequiredTool "arm-none-eabi-objdump"

    $fullDiagArtifacts = Get-FirmwareArtifactSet "fw-full-diag-carrier-debug"
    $standardArtifacts = Get-FirmwareArtifactSet "fw-standard-carrier-minsizerel"
    $tinyArtifacts     = Get-FirmwareArtifactSet "fw-tiny-carrier-minsizerel"

    foreach ($artifacts in @($fullDiagArtifacts, $standardArtifacts, $tinyArtifacts)) {
        Assert-FirmwareArtifactSetExists -Artifacts $artifacts
    }

    $ipcForbiddenPattern = '\b(tm_ipc_export_init|tm_ipc_init|tm_ipc_slab_stat|_ipc_broker_task|_slab_tiny|_slab_small|_slab_medium|_slab_large|_defer_ring|_pool_tiny|_pool_small|_pool_medium|_pool_large|_broker_attr|_broker_dispatch_count|_broker_error_count|_broker_last_progress_tick)\b'
    $shellOffForbiddenPattern = '\b(shell_task_init|shell_task_rx_notify|tm_shell_task_init|tm_shell_task_submit_line|tm_shell_task_notify_transport_ready|tm_shell_task_set_banner_hook|tm_shell_task_diag_snapshot|shell_init|shell_print|shell_printf|shell_process_line|shell_set_print)\b'

    $fullDiagNm       = Invoke-ExternalText -ToolPath $nmTool -Arguments @("-a", $fullDiagArtifacts.Elf)
    $fullDiagObjdump  = Invoke-ExternalText -ToolPath $objdumpTool -Arguments @("-h", $fullDiagArtifacts.Elf)
    $fullDiagMapRaw   = Read-Utf8Absolute $fullDiagArtifacts.Map
    $fullDiagCmdBytes = Get-SectionSizeBytes -ObjdumpOutput $fullDiagObjdump -SectionName ".shell_cmd"

    if ($fullDiagCmdBytes -eq 0) {
        throw "fw-full-diag-carrier-debug must keep a non-empty .shell_cmd section."
    }

    Assert-RegexAbsent -Text $fullDiagNm `
                       -Pattern '\b(_strtod_l|__gethex|_scanf_float|strtof|strtod)\b' `
                       -Message "Shell-on ELF must not retain the strtof/strtod float parsing chain."
    Assert-RegexAbsent -Text $fullDiagMapRaw `
                       -Pattern '\b(_strtod_l|__gethex|_scanf_float)\b' `
                       -Message "Shell-on map must not retain _strtod_l/__gethex/_scanf_float."
    Assert-RegexAbsent -Text $fullDiagNm `
                       -Pattern $ipcForbiddenPattern `
                       -Message "FalconFoc default full-diag image must keep IPC runtime fully out of the final ELF."

    foreach ($artifacts in @($standardArtifacts, $tinyArtifacts)) {
        $nmOutput      = Invoke-ExternalText -ToolPath $nmTool -Arguments @("-a", $artifacts.Elf)
        $objdumpOutput = Invoke-ExternalText -ToolPath $objdumpTool -Arguments @("-h", $artifacts.Elf)
        $mapRaw        = Read-Utf8Absolute $artifacts.Map
        $cmdBytes      = Get-SectionSizeBytes -ObjdumpOutput $objdumpOutput -SectionName ".shell_cmd"

        if ($cmdBytes -ne 0) {
            throw "Preset '$($artifacts.Preset)' must keep .shell_cmd empty when shell is off."
        }

        Assert-RegexPresent -Text $mapRaw `
                            -Pattern '(?m)^\.shell_cmd\s+0x[0-9A-Fa-f]+\s+0x0\b' `
                            -Message "Preset '$($artifacts.Preset)' must keep .shell_cmd at size 0 in the final map."
        Assert-RegexAbsent -Text $nmOutput `
                           -Pattern $shellOffForbiddenPattern `
                           -Message "Preset '$($artifacts.Preset)' must not retain live shell transport/session/core symbols when shell is off."
        Assert-RegexAbsent -Text $nmOutput `
                           -Pattern '\b_shell_cmd_[A-Za-z0-9_]+\b' `
                           -Message "Preset '$($artifacts.Preset)' must not retain exported shell command entries when shell is off."
        Assert-RegexAbsent -Text $mapRaw `
                           -Pattern '\b_shell_cmd_[A-Za-z0-9_]+\b' `
                           -Message "Preset '$($artifacts.Preset)' map must not retain exported shell command entries when shell is off."
        Assert-RegexAbsent -Text $nmOutput `
                           -Pattern $ipcForbiddenPattern `
                           -Message "Preset '$($artifacts.Preset)' must keep IPC slab/broker/runtime symbols out of the final ELF."
    }
}

$bspManifestRaw = Read-Utf8Raw "bsp\board\CMakeLists.txt"
if ($bspManifestRaw -match [Regex]::Escape("../../applications/tasks")) {
    throw "BSP must not publicly export applications/tasks after startup ownership moved to app_main + motor_task."
}

$runtimeVisibleFiles = @(
    "applications\tasks\motor_task.c",
    "applications\tasks\motor_fsm.c"
)

$forbiddenRuntimeChars = @(
    [char]0x2014,
    [char]0x2013,
    [char]0x2026
)

foreach ($relativePath in $runtimeVisibleFiles) {
    $raw = Read-Utf8Raw $relativePath

    foreach ($match in [regex]::Matches($raw, '"[^"\r\n]*"')) {
        foreach ($ch in $forbiddenRuntimeChars) {
            if ($match.Value.Contains([string]$ch)) {
                throw "Runtime-visible strings in '$relativePath' must stay ASCII-only so COM54 shell captures do not degrade."
            }
        }
    }
}

$motorStoreRaw = Read-Utf8Raw "applications\runtime\motor_store.c"
if ($motorStoreRaw -match '\*\(const uint32_t \*\)\(uintptr_t\)') {
    throw "applications/runtime/motor_store.c must not depend on direct memory-mapped Flash dereference for seq_num reads. Use the flash read contract instead."
}

if (Test-Path -LiteralPath (Join-Path $RepoRoot "applications\algo\foc_param.c")) {
    throw "applications/algo/foc_param.c must not remain as a live store owner. Keep only the header-level compatibility wrapper."
}
if (Test-Path -LiteralPath (Join-Path $RepoRoot "applications\algo\foc_restore.c")) {
    throw "applications/algo/foc_restore.c must not remain as a live restore owner. Keep only the header-level compatibility wrapper."
}

$shellTaskHeaderRaw = Read-Utf8Raw "applications\tasks\shell_task.h"
if ($shellTaskHeaderRaw -match [Regex]::Escape("Called automatically by the INIT_EXPORT system.")) {
    throw "shell_task.h must describe the real owner. shell_task_init() is called from motor_task_init(), not by INIT_EXPORT."
}

$motorTaskRaw = Read-Utf8Raw "applications\tasks\motor_task.c"
$motorTaskInitPos = $motorTaskRaw.IndexOf("void motor_task_init(void)", [System.StringComparison]::Ordinal)
$shellInitPos = $motorTaskRaw.IndexOf("shell_task_init();", [System.StringComparison]::Ordinal)
$runtimeInitPos = $motorTaskRaw.IndexOf("motor_runtime_init(&_motor_ins);", [System.StringComparison]::Ordinal)
if (($motorTaskInitPos -lt 0) -or ($shellInitPos -lt $motorTaskInitPos) -or ($runtimeInitPos -lt $shellInitPos)) {
    throw "motor_task_init() must explicitly start shell_task_init() before motor_runtime_init() so the startup owner chain stays visible."
}

$shellFiles = @(
    "applications/shell/app_shell_cmd_cfg.c",
    "applications/shell/app_shell_cmd_cal.c",
    "applications/shell/app_shell_cmd_diag.c",
    "applications/shell/app_shell_cmd_fault.c",
    "applications/shell/app_shell_cmd_state.c",
    "applications/shell/app_shell_cmd_store.c"
)

foreach ($shellFile in $shellFiles) {
    $shellRaw = Read-Utf8Raw $shellFile
    if ($shellRaw -match '\b_motor_ins\b') {
        throw "$shellFile must not reach the live motor through _motor_ins; use the runtime seam instead."
    }
    if ($shellRaw -match 'motor->') {
        throw "$shellFile must not reach the live motor through direct motor-> access; use the runtime snapshot/request seam instead."
    }
}

$faultRuntimeRaw = Read-Utf8Raw "applications\runtime\motor_fault_runtime.c"
if ($faultRuntimeRaw -match 'axis_state\s*=') {
    throw "applications/runtime/motor_fault_runtime.c must not write axis_state directly after fault/FSM owner closure."
}

$focCtrlRaw = Read-Utf8Raw "applications\periph\foc_ctrl.c"
foreach ($token in @(
        "void foc_cmd_passthrough(",
        "void foc_cmd_vel_ramp(",
        "void foc_cmd_torque_ramp(",
        "void foc_cmd_pos_traj(",
        "void foc_fault_check(",
        "void foc_process_input_task("
    )) {
    if ($focCtrlRaw -match ("(?m)^\s*" + [Regex]::Escape($token))) {
        throw "applications/periph/foc_ctrl.c must not remain the live owner of runtime token '$token'."
    }
}
if ($focCtrlRaw -match '(?m)^\s*#if 0\b') {
    throw "applications/periph/foc_ctrl.c must not keep #if 0 migration leftovers after runtime-owner closure."
}

$focCalibRaw = Read-Utf8Raw "applications\periph\foc_calib.c"
if ($focCalibRaw -match '(?m)^\s*#if 0\b') {
    throw "applications/periph/foc_calib.c must not keep #if 0 migration leftovers after runtime-owner closure."
}

$coreConfigRaw = Read-Utf8Raw "include\config\tm_core_config.h"
foreach ($token in @(
        "#define TM_FEATURE_IPC (0U)",
        "#define TM_FEATURE_VOFA (0U)",
        "#define TM_FEATURE_TASK_BUDGET (0U)",
        "#define TM_FEATURE_VERBOSE_LOG (0U)"
    )) {
    if ($coreConfigRaw -notmatch [Regex]::Escape($token)) {
        throw "FalconFoc-lite product defaults must be owned by tm_core_config.h token '$token'."
    }
}
if ($coreConfigRaw -notmatch [Regex]::Escape("#define TM_APP_FEATURE_UART_TELEMETRY (0U)")) {
    throw "FalconFoc-lite must default UART telemetry to disabled so USART1 stays console-first unless diagnostics opt in explicitly."
}
if ($coreConfigRaw -notmatch [Regex]::Escape("#define TM_APP_FEATURE_UART_PASSIVE_TELEMETRY (0U)")) {
    throw "FalconFoc-lite must default passive UART telemetry to disabled on the shared USART1 transport."
}
if ($coreConfigRaw -notmatch '(?s)#if TM_DEVDRV_PROFILE_FULL_DIAG\s*#define TM_FEATURE_SHELL \(1U\)\s*#else\s*#define TM_FEATURE_SHELL \(0U\)') {
    throw "FalconFoc-lite must derive TM_FEATURE_SHELL from the product profile inside tm_core_config.h."
}

$bspConfigRaw = Read-Utf8Raw "bsp\bsp_config.h"
foreach ($token in @(
        "BOARD_CONSOLE_UART_NAME",
        "BOARD_TELEMETRY_UART_NAME",
        "BOARD_TELEMETRY_UART_NAME   BOARD_CONSOLE_UART_NAME"
    )) {
    if ($bspConfigRaw -notmatch [Regex]::Escape($token)) {
        throw "bsp/bsp_config.h must keep the shared USART1 console/telemetry ownership token '$token'."
    }
}

$transportDocRaw = Read-Utf8Raw (Resolve-GovernanceDocByTokens @(
        "FalconFoc-lite transport-shell-hardening",
        "console-first",
        "debug-only",
        "COM54"
    ))
foreach ($token in @(
        "shell_task",
        "app_shell_task",
        "USART1",
        "console-first",
        "VOFA",
        "debug-only",
        "COM54"
    )) {
    if ($transportDocRaw -notmatch [Regex]::Escape($token)) {
        throw "The transport hardening doc must record the FalconFoc-lite serial strategy token '$token'."
    }
}

$rootCMakeRaw = Read-Utf8Raw "CMakeLists.txt"
$metadataHelperRaw = Read-Utf8Raw "cmake\ThemisPlatformMetadata.cmake"
foreach ($token in @(
        "THEMIS_FEATURE_SHELL",
        "THEMIS_FEATURE_IPC",
        "THEMIS_FEATURE_VOFA",
        "THEMIS_FEATURE_TASK_BUDGET",
        "THEMIS_FEATURE_VERBOSE_LOG",
        "THEMIS_LINKER_MIN_HEAP_SIZE",
        "THEMIS_LINKER_MIN_STACK_SIZE",
        "configure_file("
    )) {
    if ($rootCMakeRaw -match [Regex]::Escape($token)) {
        throw "FalconFoc root CMake must not own product feature policy token '$token'."
    }
}
foreach ($token in @(
        "startup_at32f403a_407.s",
        "AT32F403AxG_FLASH.ld",
        "AT32F403ACGT7"
    )) {
    if ($rootCMakeRaw -match [Regex]::Escape($token)) {
        throw "FalconFoc root CMake must not keep chip-specific startup/linker/part token '$token' after platform metadata owner closure."
    }
}
if ($rootCMakeRaw -notmatch [Regex]::Escape("include(ThemisPlatformMetadata)")) {
    throw "FalconFoc root CMake must load the shared ThemisPlatformMetadata helper."
}
foreach ($token in @(
        "include(ThemisBuildAxes)",
        "include(ThemisFirmwareAssembly)",
        "include(ThemisContracts)",
        "include(ThemisStatusReport)",
        "themis_build_axes_declare_cache()",
        "themis_build_axes_validate()",
        "themis_define_contract_targets(",
        "themis_emit_status_report()"
    )) {
    if ($rootCMakeRaw -notmatch [Regex]::Escape($token)) {
        throw "FalconFoc root CMake must keep phase-2 helper boundary token '$token'."
    }
}
foreach ($token in @(
        "themis_metadata_load_platform_defaults(",
        "themis_metadata_load_cpu_defaults(",
        "add_library(themis_root_headers",
        "add_library(themis_build_contract",
        "add_library(themis_cpu_class_contract",
        "add_library(themis_platform_contract",
        "add_library(themis_host_test_contract",
        'message(STATUS "Themis build firmware :',
        'message(STATUS "Themis platform :'
    )) {
    if ($rootCMakeRaw -match [Regex]::Escape($token)) {
        throw "FalconFoc root CMake must not keep phase-2 extracted implementation token '$token'."
    }
}
foreach ($token in @(
        "THEMIS_PLATFORM_VENDOR",
        "THEMIS_PLATFORM_SOC",
        "THEMIS_PLATFORM_DIR_OVERRIDE",
        "THEMIS_PLATFORM_TARGET_OVERRIDE",
        "THEMIS_ENTRY_SOURCE_OVERRIDE",
        "THEMIS_STARTUP_SOURCE_OVERRIDE",
        "THEMIS_LINKER_SCRIPT_OVERRIDE",
        "THEMIS_PLATFORM_COMPILE_DEFINITIONS_OVERRIDE",
        "THEMIS_CPU_CLASS_OVERRIDE",
        "ThemisPlatformDefaults.cmake",
        "ThemisCpuClassDefaults.cmake"
    )) {
    if ($metadataHelperRaw -notmatch [Regex]::Escape($token)) {
        throw "The shared ThemisPlatformMetadata helper must own token '$token'."
    }
}
$phase2HelperFiles = @(
    "cmake\\ThemisBuildAxes.cmake",
    "cmake\\ThemisFirmwareAssembly.cmake",
    "cmake\\ThemisContracts.cmake",
    "cmake\\ThemisStatusReport.cmake"
)
foreach ($helperPath in $phase2HelperFiles) {
    if (-not (Test-Path -LiteralPath $helperPath)) {
        throw "Missing phase-2 CMake helper '$helperPath'."
    }
}

$platformDefaultsRaw = Read-Utf8Raw "platform\at\at32f403a\ThemisPlatformDefaults.cmake"
foreach ($token in @(
        "THEMIS_PLATFORM_TARGET_DEFAULT",
        "THEMIS_PLATFORM_ENTRY_SOURCE_DEFAULT",
        "THEMIS_PLATFORM_STARTUP_SOURCE_DEFAULT",
        "THEMIS_PLATFORM_LINKER_SCRIPT_DEFAULT",
        "THEMIS_PLATFORM_COMPILE_DEFINITIONS_DEFAULT",
        "THEMIS_PLATFORM_CPU_CLASS_DEFAULT",
        "CMSIS_device_header",
        "THEMIS_PLATFORM_FLASH_DRIVER_HEADER",
        "THEMIS_PLATFORM_FLASH_BASE_ADDR",
        "THEMIS_PLATFORM_FLASH_SIZE_BYTES",
        "THEMIS_PLATFORM_FLASH_SECTOR_SIZE_BYTES",
        "THEMIS_PLATFORM_FLASH_SECTOR_COUNT",
        "THEMIS_PLATFORM_MOTOR_SOFTIRQ_HANDLER",
        "THEMIS_PLATFORM_MOTOR_SOFTIRQ_INSTANCE",
        "startup_at32f403a_407.s",
        "AT32F403AxG_FLASH.ld",
        "AT32F403ACGT7"
    )) {
    if ($platformDefaultsRaw -notmatch [Regex]::Escape($token)) {
        throw "The active platform metadata defaults must own token '$token'."
    }
}
if ($platformDefaultsRaw -match [Regex]::Escape("ARM_MATH_CM4")) {
    throw "The active platform metadata defaults must not keep ARM_MATH_CM4 after CPU-class owner closure."
}

$cpuClassFiles = @(
    @{
        Path = "platform\cpu\cortex-m4f-hard\ThemisCpuClassDefaults.cmake"
        Tokens = @(
            "THEMIS_CPU_TOOLCHAIN_CPU_DEFAULT",
            "THEMIS_CPU_TOOLCHAIN_FPU_DEFAULT",
            "THEMIS_CPU_TOOLCHAIN_FLOAT_DEFAULT",
            "THEMIS_CPU_FREERTOS_PORT_GLOB_DEFAULT",
            "THEMIS_CPU_FREERTOS_PORT_INCLUDE_DIR_DEFAULT",
            "THEMIS_CPU_ARM_MATH_DEFINE_DEFAULT",
            "ARM_CM4F",
            "ARM_MATH_CM4"
        )
    },
    @{
        Path = "platform\cpu\cortex-m3-soft\ThemisCpuClassDefaults.cmake"
        Tokens = @(
            "THEMIS_CPU_TOOLCHAIN_CPU_DEFAULT",
            "THEMIS_CPU_TOOLCHAIN_FPU_DEFAULT",
            "THEMIS_CPU_TOOLCHAIN_FLOAT_DEFAULT",
            "THEMIS_CPU_FREERTOS_PORT_GLOB_DEFAULT",
            "THEMIS_CPU_FREERTOS_PORT_INCLUDE_DIR_DEFAULT",
            "THEMIS_CPU_ARM_MATH_DEFINE_DEFAULT",
            "ARM_CM3",
            "ARM_MATH_CM3"
        )
    }
)

foreach ($cpuClassFile in $cpuClassFiles) {
    $raw = Read-Utf8Raw $cpuClassFile.Path
    foreach ($token in $cpuClassFile.Tokens) {
        if ($raw -notmatch [Regex]::Escape($token)) {
            throw "CPU-class metadata '$($cpuClassFile.Path)' must own token '$token'."
        }
    }
}

$toolchainRaw = Read-Utf8Raw "tools\cmake\arm-gcc-toolchain.cmake"
if ($toolchainRaw -notmatch [Regex]::Escape("ThemisPlatformMetadata.cmake")) {
    throw "The ARM GCC toolchain entry must load the shared ThemisPlatformMetadata helper."
}
foreach ($token in @(
        "THEMIS_CPU_CLASS_OVERRIDE",
        "THEMIS_PLATFORM_CPU_CLASS_DEFAULT",
        "ThemisCpuClassDefaults.cmake"
    )) {
    if ($metadataHelperRaw -notmatch [Regex]::Escape($token)) {
        throw "The shared ThemisPlatformMetadata helper must resolve CPU-class metadata token '$token'."
    }
}
foreach ($token in @(
        'set(MCU_CPU',
        'set(MCU_FPU',
        'set(MCU_FLOAT'
    )) {
    if ($toolchainRaw -notmatch [Regex]::Escape($token)) {
        throw "The ARM GCC toolchain entry must still expose emergency MCU override cache '$token'."
    }
}
foreach ($token in @(
        '"cortex-m4"',
        '"fpv4-sp-d16"',
        '"hard"',
        '"portable/GCC/ARM_CM4F/*.c"',
        '"portable/GCC/ARM_CM4F"'
    )) {
    if ($toolchainRaw -match [Regex]::Escape($token)) {
        throw "The generic ARM GCC toolchain entry must not hardcode live CPU-class token '$token'."
    }
}

$freertosManifestRaw = Read-Utf8Raw "kernel\freertos\CMakeLists.txt"
foreach ($token in @(
        "THEMIS_CPU_FREERTOS_PORT_GLOB",
        "THEMIS_CPU_FREERTOS_PORT_INCLUDE_DIR"
    )) {
    if ($freertosManifestRaw -notmatch [Regex]::Escape($token)) {
        throw "kernel/freertos/CMakeLists.txt must consume CPU-class token '$token'."
    }
}
if ($freertosManifestRaw -match [Regex]::Escape("ARM_CM4F")) {
    throw "kernel/freertos/CMakeLists.txt must not hardcode ARM_CM4F after CPU-class owner closure."
}

$rteComponentsRaw = Read-Utf8Raw "kernel\freertos\CMSIS_RTOS_V2\RTE_Components.h"
if ($rteComponentsRaw -match [Regex]::Escape('at32f403a_407.h')) {
    throw "RTE_Components.h must not hardcode the live SoC device header after platform metadata closure."
}
if ($rteComponentsRaw -notmatch [Regex]::Escape("CMSIS_device_header")) {
    throw "RTE_Components.h must consume CMSIS_device_header from platform compile definitions."
}

$preparedPlatforms = @(
    @{
        Path = "platform\at\at32f415\ThemisPlatformDefaults.cmake"
        CpuClass = 'THEMIS_PLATFORM_CPU_CLASS_DEFAULT "cortex-m3-soft"'
        TimingPath = "platform\at\at32f415\generated\inc\tm_platform_foc_timing.h"
    },
    @{
        Path = "platform\at\at32f423\ThemisPlatformDefaults.cmake"
        CpuClass = 'THEMIS_PLATFORM_CPU_CLASS_DEFAULT "cortex-m4f-hard"'
        TimingPath = "platform\at\at32f423\generated\inc\tm_platform_foc_timing.h"
    }
)

foreach ($preparedPlatform in $preparedPlatforms) {
    $raw = Read-Utf8Raw $preparedPlatform.Path
    if ($raw -notmatch [Regex]::Escape($preparedPlatform.CpuClass)) {
        throw "Prepared platform '$($preparedPlatform.Path)' must bind the expected CPU class."
    }
    foreach ($token in @(
            "CMSIS_device_header",
            "THEMIS_PLATFORM_FLASH_DRIVER_HEADER",
            "THEMIS_PLATFORM_FLASH_BASE_ADDR",
            "THEMIS_PLATFORM_FLASH_SIZE_BYTES",
            "THEMIS_PLATFORM_FLASH_SECTOR_SIZE_BYTES",
            "THEMIS_PLATFORM_FLASH_SECTOR_COUNT",
            "THEMIS_PLATFORM_MOTOR_SOFTIRQ_HANDLER",
            "THEMIS_PLATFORM_MOTOR_SOFTIRQ_INSTANCE"
        )) {
        if ($raw -notmatch [Regex]::Escape($token)) {
            throw "Prepared platform '$($preparedPlatform.Path)' must bind $token through platform compile definitions."
        }
    }

    $timingRaw = Read-Utf8Raw $preparedPlatform.TimingPath
    foreach ($token in @(
            "TM_PLATFORM_TMR_CLK_HZ",
            "TM_PLATFORM_PWM_FREQ_HZ",
            "TM_PLATFORM_VEL_LOOP_FREQ_HZ",
            "TM_PLATFORM_POS_LOOP_FREQ_HZ",
            "TM_PLATFORM_TRAJ_TASK_FREQ_HZ"
        )) {
        if ($timingRaw -notmatch [Regex]::Escape($token)) {
            throw "Prepared platform timing seam '$($preparedPlatform.TimingPath)' must own token '$token'."
        }
    }
}

$bspPlatformFiles = @(
    "bsp\ports\bsp_console.c",
    "bsp\ports\bsp_system.c",
    "bsp\board\bsp_init.c",
    "bsp\board\bsp_flash.h",
    "bsp\board\bsp_flash.c"
)

foreach ($relativePath in $bspPlatformFiles) {
    $raw = Read-Utf8Raw $relativePath
    foreach ($forbiddenToken in @("at32f403a_407.h", "at32f403a_407_flash.h")) {
        if ($raw -match [Regex]::Escape($forbiddenToken)) {
            throw "'$relativePath' must not hardcode live 403A device/header tokens after platform owner closure."
        }
    }
}

$bspFlashHeaderRaw = Read-Utf8Raw "bsp\board\bsp_flash.h"
foreach ($token in @(
        "THEMIS_PLATFORM_FLASH_BASE_ADDR",
        "THEMIS_PLATFORM_FLASH_SIZE_BYTES",
        "THEMIS_PLATFORM_FLASH_SECTOR_SIZE_BYTES",
        "THEMIS_PLATFORM_FLASH_SECTOR_COUNT"
    )) {
    if ($bspFlashHeaderRaw -notmatch [Regex]::Escape($token)) {
        throw "bsp/board/bsp_flash.h must consume platform-owned flash token '$token'."
    }
}
if ($bspFlashHeaderRaw -match [Regex]::Escape("THEMIS_PLATFORM_FLASH_DRIVER_HEADER")) {
    throw "bsp/board/bsp_flash.h must keep the vendor Flash driver token out of the public header."
}
foreach ($forbiddenPattern in @(
        '\bBSP_FLASH_ERR_[A-Za-z0-9_]+\b',
        '\bbsp_flash_get_sector\s*\(',
        '\bbsp_flash_is_empty\s*\('
    )) {
    if ($bspFlashHeaderRaw -match $forbiddenPattern) {
        throw "bsp/board/bsp_flash.h must keep legacy public Flash utility/error surfaces closed."
    }
}

$bspFlashSourceRaw = Read-Utf8Raw "bsp\board\bsp_flash.c"
foreach ($token in @(
        "THEMIS_PLATFORM_FLASH_DRIVER_HEADER",
        "tm_runtime.h",
        "tm_irq_critical_enter",
        "tm_irq_critical_exit"
    )) {
    if ($bspFlashSourceRaw -notmatch [Regex]::Escape($token)) {
        throw "bsp/board/bsp_flash.c must own Flash runtime/backend token '$token'."
    }
}
foreach ($forbiddenPattern in @(
        '\b__disable_irq\s*\(',
        '\b__enable_irq\s*\('
    )) {
    if ($bspFlashSourceRaw -match $forbiddenPattern) {
        throw "bsp/board/bsp_flash.c must use tm_runtime critical helpers instead of raw IRQ intrinsics."
    }
}

$systemPortHeaderRaw = Read-Utf8Raw "bsp\ports\bsp_system.h"
if ($systemPortHeaderRaw -notmatch '(?m)^\s*int\s+bsp_system_reset\s*\(\s*void\s*\)\s*;') {
    throw "bsp/ports/bsp_system.h must expose bsp_system_reset() as an int 0/-errno contract."
}
foreach ($forbiddenToken in @(
        "CMSIS_device_header",
        "at32f403a_407.h",
        "at32f415.h",
        "at32f423.h"
    )) {
    if ($systemPortHeaderRaw -match [Regex]::Escape($forbiddenToken)) {
        throw "bsp/ports/bsp_system.h must keep vendor/CMSIS headers out of the public northbound contract."
    }
}
if ($systemPortHeaderRaw -match '(?m)^\s*void\s+bsp_system_reset\s*\(') {
    throw "bsp/ports/bsp_system.h must not regress to the legacy void reset contract."
}

$systemPortSourceRaw = Read-Utf8Raw "bsp\ports\bsp_system.c"
foreach ($token in @(
        "tm_runtime.h",
        "tm_is_in_isr",
        "CMSIS_device_header",
        "NVIC_SystemReset"
    )) {
    if ($systemPortSourceRaw -notmatch [Regex]::Escape($token)) {
        throw "bsp/ports/bsp_system.c must own system-port template token '$token'."
    }
}
if ($systemPortSourceRaw -match '(?m)^\s*void\s+bsp_system_reset\s*\(') {
    throw "bsp/ports/bsp_system.c must not regress to the legacy void reset implementation."
}

$consolePortHeaderRaw = Read-Utf8Raw "bsp\ports\bsp_console.h"
foreach ($pattern in @(
        '(?m)^\s*int\s+bsp_console_shell_rx_start\s*\(',
        '(?m)^\s*uint16_t\s+bsp_console_shell_rx_position\s*\(',
        '(?m)^\s*void\s+bsp_console_shell_irq_ack_idle\s*\(',
        '(?m)^\s*bool\s+bsp_console_shell_irq_idle_pending\s*\(',
        '(?m)^\s*int\s+bsp_console_write_poll\s*\(',
        '(?m)^\s*int\s+bsp_console_write_dma\s*\('
    )) {
    if ($consolePortHeaderRaw -notmatch $pattern) {
        throw "bsp/ports/bsp_console.h must keep the formal six-function transport contract."
    }
}
foreach ($forbiddenToken in @(
        "CMSIS_device_header",
        "at32f403a_407.h",
        "at32f415.h",
        "at32f423.h"
    )) {
    if ($consolePortHeaderRaw -match [Regex]::Escape($forbiddenToken)) {
        throw "bsp/ports/bsp_console.h must keep vendor/CMSIS headers out of the public transport contract."
    }
}

$consolePortSourceRaw = Read-Utf8Raw "bsp\ports\bsp_console.c"
foreach ($token in @(
        "tm_runtime.h",
        "tm_is_in_isr",
        "CMSIS_device_header",
        "wk_dma.h",
        "bsp_console_shell_rx_start",
        "bsp_console_write_poll",
        "bsp_console_write_dma"
    )) {
    if ($consolePortSourceRaw -notmatch [Regex]::Escape($token)) {
        throw "bsp/ports/bsp_console.c must own transport-port template token '$token'."
    }
}
foreach ($forbiddenPattern in @(
        '(?m)^\s*void\s+bsp_console_shell_rx_start\s*\(',
        '(?m)^\s*void\s+bsp_console_write_poll\s*\(',
        '(?m)^\s*void\s+bsp_console_write_dma\s*\('
    )) {
    if ($consolePortSourceRaw -match $forbiddenPattern) {
        throw "bsp/ports/bsp_console.c must not regress action helpers back to void contracts."
    }
}

$platformFocTimingRaw = Read-Utf8Raw "platform\at\at32f403a\generated\inc\tm_platform_foc_timing.h"
foreach ($token in @(
        "TM_PLATFORM_TMR_CLK_HZ",
        "TM_PLATFORM_TMR_CLK_MHZ",
        "TM_PLATFORM_PWM_FREQ_HZ",
        "TM_PLATFORM_VEL_LOOP_FREQ_HZ",
        "TM_PLATFORM_POS_LOOP_FREQ_HZ",
        "TM_PLATFORM_TRAJ_TASK_FREQ_HZ"
    )) {
    if ($platformFocTimingRaw -notmatch [Regex]::Escape($token)) {
        throw "The active platform timing header must own token '$token'."
    }
}

$focConfigRaw = Read-Utf8Raw "applications\algo\foc_config.h"
if ($focConfigRaw -notmatch [Regex]::Escape('#include "tm_platform_foc_timing.h"')) {
    throw "applications/algo/foc_config.h must consume the platform timing seam."
}
foreach ($token in @(
        "240000000U",
        "TMR_CLK_MHZ            (240U)",
        "VEL_LOOP_FREQ_HZ       (5000U)",
        "POS_LOOP_FREQ_HZ       (1000U)",
        "TRAJ_TASK_FREQ_HZ      (1000U)"
    )) {
    if ($focConfigRaw -match [Regex]::Escape($token)) {
        throw "applications/algo/foc_config.h must not own SoC timing token '$token'."
    }
}

$subsysCMakeRaw = Read-Utf8Raw "subsys\CMakeLists.txt"
foreach ($token in @(
        '"ipc/tm_ipc.c"',
        '"runtime/tm_task_budget_probe.c"',
        '"shell/tm_shell.c"'
    )) {
    if ($subsysCMakeRaw -notmatch [Regex]::Escape($token)) {
        throw "subsys/CMakeLists.txt must keep the TEC-style shared manifest entry '$token'."
    }
}
foreach ($token in @(
        "THEMIS_FEATURE_IPC",
        "THEMIS_FEATURE_TASK_BUDGET",
        "THEMIS_FEATURE_SHELL"
    )) {
    if ($subsysCMakeRaw -match [Regex]::Escape($token)) {
        throw "subsys/CMakeLists.txt must not own FalconFoc feature gating token '$token'."
    }
}

$applicationsCMakeRaw = Read-Utf8Raw "applications\CMakeLists.txt"
if ($applicationsCMakeRaw -notmatch [Regex]::Escape('"shell/*.c"')) {
    throw "applications/CMakeLists.txt must always manifest the product shell sources."
}
if ($applicationsCMakeRaw -notmatch [Regex]::Escape('"tasks/*.c"')) {
    throw "applications/CMakeLists.txt must always manifest the product task sources."
}
foreach ($token in @(
        "THEMIS_FEATURE_SHELL",
        "THEMIS_FEATURE_VOFA",
        "app_shell_task.c",
        "shell_task.c",
        "vofa_task.c"
    )) {
    if ($applicationsCMakeRaw -match [Regex]::Escape($token)) {
        throw "applications/CMakeLists.txt must not own FalconFoc feature gating token '$token'."
    }
}

$linkerScriptRaw = Read-Utf8Raw "platform\at\at32f403a\vendor\cmsis\cm4\device_support\startup\gcc\linker\AT32F403AxG_FLASH.ld"
if ($linkerScriptRaw -notmatch [Regex]::Escape("_Min_Heap_Size = 0;")) {
    throw "FalconFoc linker reserve must fix _Min_Heap_Size = 0 in the platform linker script."
}
if ($linkerScriptRaw -notmatch [Regex]::Escape("_Min_Stack_Size = 0x400;")) {
    throw "FalconFoc linker reserve must fix _Min_Stack_Size = 0x400 in the platform linker script."
}
if ($linkerScriptRaw -match [Regex]::Escape("@THEMIS_LINKER_MIN_HEAP_SIZE@") -or
    $linkerScriptRaw -match [Regex]::Escape("@THEMIS_LINKER_MIN_STACK_SIZE@")) {
    throw "FalconFoc linker reserve must no longer depend on root-CMake configure_file placeholders."
}

$shellManualRaw = Read-Utf8Raw (Resolve-ApplicationDocByTokens @(
        "COM54",
        "USART1",
        "12.34rpm",
        "0x1.8",
        "full-diag"
    ))
foreach ($token in @("1e3", "nan", "inf", "123", "12.34", "12.34rpm", "0x1.8")) {
    if ($shellManualRaw -notmatch [Regex]::Escape($token)) {
        throw "Shell manual must document the narrowed decimal parsing contract token '$token'."
    }
}
foreach ($token in @(
        "build_date=",
        "build_time=",
        "hint   : next='help'",
        "version=",
        "data=",
        "shell=on",
        "queue_used=",
        "queue_depth=",
        "submit_ok_count=",
        "submit_fail_count=",
        "reason=redirect target=cfg",
        "reason=idle_only",
        "reason=cmd_not_found rc=-2"
    )) {
    if ($shellManualRaw -notmatch [Regex]::Escape($token)) {
        throw "Shell manual must reflect the live shell output token '$token'."
    }
}
foreach ($token in @(
        "help=type 'help'",
        "shell=1",
        "queue=<used>/<depth>",
        "owner=cfg",
        "policy=idle-only"
    )) {
    if ($shellManualRaw -match [Regex]::Escape($token)) {
        throw "Shell manual must not retain retired shell output token '$token'."
    }
}

$shellGovernanceDocRaw = Read-Utf8Raw (Resolve-GovernanceDocByTokens @(
        "FalconFoc-lite shell-output-policy",
        "Repeated Row",
        "ASCII",
        "128"
    ))
foreach ($token in @(
        "shell_task",
        "app_shell_task",
        "console-first",
        "reason=snapshot_unavailable",
        "reason=idle_only",
        "reason=redirect target=cfg",
        "usage='Show command list'",
        "next='help'",
        "queue_used",
        "submit_ok_count"
    )) {
    if ($shellGovernanceDocRaw -notmatch [Regex]::Escape($token)) {
        throw "The shell output governance doc must record the FalconFoc-lite shell policy token '$token'."
    }
}

$diagWitnessRaw = Read-Utf8Raw "scripts\flows\board\run_diag_carrier_witness.ps1"
foreach ($token in @(
        "shell=on",
        "uart=USART1",
        "baud=1000000",
        "transport=dma",
        "help",
        "sys status",
        "state show",
        "obs all",
        "diag pid all show",
        "diag param show",
        "store save sys",
        "store status"
    )) {
    if ($diagWitnessRaw -notmatch [Regex]::Escape($token)) {
        throw "Diag witness flow must reflect the live shell token '$token'."
    }
}
foreach ($token in @(
        "shell=1",
        "policy=idle-only",
        "[cal] cmd=start result=ok",
        "diag pid vel kp 0.1000",
        "diag param rs 0.2300"
    )) {
    if ($diagWitnessRaw -match [Regex]::Escape($token)) {
        throw "Diag witness flow must not retain retired shell token '$token'."
    }
}

$bootDocs = @(
    Get-ChildItem -Path (Join-Path $RepoRoot "docs\boot") -Filter "*.md" |
        Where-Object {
            $raw = [System.IO.File]::ReadAllText($_.FullName, [System.Text.UTF8Encoding]::new($false))
            $raw.Contains("applications/app_main.c")
        } |
        ForEach-Object { To-RepoRelativePath $_.FullName }
)

if ($bootDocs.Count -lt 2) {
    throw "Expected at least two boot docs that describe the live FalconFoc app_main startup bridge."
}

$requiredBootTokens = @(
    "platform/at/at32f403a/generated/src/main.c",
    "applications/app_main.c",
    "motor_task_init",
    "vofa_task_init",
    "shell_task_init",
    "app_shell_task"
)

$staleBootTokens = @(
    "at32f423",
    "watchdog_task_init",
    "buzzer_task_init",
    "display_task_init",
    "uart_task_init",
    "bsp_devices_init",
    "bsp_subsys_init",
    "bsp_devices_late_init",
    "tm_hw_uart_prepare",
    "tm_hw_i2c_prepare",
    "motor_shell_cmds_init"
)

foreach ($relativePath in $bootDocs) {
    $raw = Read-Utf8Raw $relativePath

    foreach ($token in $requiredBootTokens) {
        if ($raw -notmatch [Regex]::Escape($token)) {
            throw "'$relativePath' must document the live FalconFoc boot chain token '$token'."
        }
    }

    foreach ($token in $staleBootTokens) {
        if ($raw -match [Regex]::Escape($token)) {
            throw "'$relativePath' still contains stale retired boot-chain token '$token'."
        }
    }
}

$maintenanceLogRaw = Read-Utf8Raw (Resolve-GovernanceDocByContent "FalconFoc AT32F403A Themis-lite platform/build alignment")
foreach ($token in @("COM54", "J-Link", "./run.sh flash", "store save sys")) {
    if ($maintenanceLogRaw -notmatch [Regex]::Escape($token)) {
        throw "Maintenance log must record the latest FalconFoc real-board witness token '$token'."
    }
}

Assert-FalconFocFirmwareContracts

Write-Host "[test_falconfoc_themis_lite_contract] all assertions passed."
