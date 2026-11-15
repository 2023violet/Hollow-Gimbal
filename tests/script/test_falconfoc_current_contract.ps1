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
    param([string]$RelativePath)
    return [System.IO.File]::ReadAllText(
        (Join-Path $RepoRoot $RelativePath),
        [System.Text.UTF8Encoding]::new($false)
    )
}

function Require-Path {
    param([string]$RelativePath)
    if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot $RelativePath))) {
        throw "Current FalconFoc contract requires '$RelativePath'."
    }
}

function Require-Contains {
    param(
        [string]$Text,
        [string]$Token,
        [string]$Message = ""
    )
    if ($Text.IndexOf($Token, [System.StringComparison]::Ordinal) -lt 0) {
        if ([string]::IsNullOrWhiteSpace($Message)) { $Message = "Missing token '$Token'." }
        throw $Message
    }
}

function Require-NotContains {
    param(
        [string]$Text,
        [string]$Token,
        [string]$Message = ""
    )
    if ($Text.IndexOf($Token, [System.StringComparison]::Ordinal) -ge 0) {
        if ([string]::IsNullOrWhiteSpace($Message)) { $Message = "Forbidden token '$Token' present." }
        throw $Message
    }
}

function Require-Regex {
    param([string]$Text, [string]$Pattern, [string]$Message)
    if (-not [regex]::IsMatch($Text, $Pattern, [Text.RegularExpressions.RegexOptions]::Multiline)) {
        throw $Message
    }
}

function Require-NoRegex {
    param([string]$Text, [string]$Pattern, [string]$Message)
    if ([regex]::IsMatch($Text, $Pattern, [Text.RegularExpressions.RegexOptions]::Multiline)) {
        throw $Message
    }
}

function Resolve-Tool {
    param([string]$Name)
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $command) { throw "Required tool '$Name' was not found in PATH." }
    return $command.Source
}

function Read-SectionSize {
    param([string]$ObjdumpOutput, [string]$SectionName)
    $pattern = '(?m)^\s*\d+\s+' + [regex]::Escape($SectionName) + '\s+([0-9A-Fa-f]+)\b'
    $match = [regex]::Match($ObjdumpOutput, $pattern)
    if (-not $match.Success) { throw "Unable to find section '$SectionName'." }
    return [Convert]::ToUInt32($match.Groups[1].Value, 16)
}

function Require-FirmwareArtifactContracts {
    $nm = Resolve-Tool "arm-none-eabi-nm"
    $objdump = Resolve-Tool "arm-none-eabi-objdump"
    $sets = @(
        @{ Name = "fw-full-diag-carrier-debug"; Shell = $true },
        @{ Name = "fw-standard-carrier-minsizerel"; Shell = $false },
        @{ Name = "fw-tiny-carrier-minsizerel"; Shell = $false }
    )
    $ipc = '\b(tm_ipc_export_init|tm_ipc_init|tm_ipc_slab_stat|_ipc_broker_task|_slab_tiny|_slab_small|_slab_medium|_slab_large|_defer_ring|_pool_tiny|_pool_small|_pool_medium|_pool_large|_broker_attr|_broker_dispatch_count|_broker_error_count|_broker_last_progress_tick)\b'
    $shell = '\b(shell_task_init|shell_task_rx_notify|tm_shell_task_init|tm_shell_task_submit_line|tm_shell_task_notify_transport_ready|tm_shell_task_set_banner_hook|tm_shell_task_diag_snapshot|shell_init|shell_print|shell_printf|shell_process_line|shell_set_print)\b'
    foreach ($set in $sets) {
        $out = Join-Path $RepoRoot ("build\{0}\output" -f $set.Name)
        $elf = Join-Path $out "falcon-foc-at32.elf"
        $map = Join-Path $out "falcon-foc-at32.map"
        Require-Path ("build\{0}\output\falcon-foc-at32.elf" -f $set.Name)
        Require-Path ("build\{0}\output\falcon-foc-at32.map" -f $set.Name)
        $nmOut = (& $nm -a $elf 2>&1 | Out-String)
        if ($LASTEXITCODE -ne 0) { throw "nm failed for $($set.Name)." }
        $objdumpOut = (& $objdump -h $elf 2>&1 | Out-String)
        if ($LASTEXITCODE -ne 0) { throw "objdump failed for $($set.Name)." }
        $mapRaw = [IO.File]::ReadAllText($map, [Text.UTF8Encoding]::new($false))
        $shellBytes = Read-SectionSize $objdumpOut ".shell_cmd"
        if ($set.Shell -and $shellBytes -eq 0) { throw "$($set.Name) must retain .shell_cmd." }
        if ((-not $set.Shell) -and $shellBytes -ne 0) { throw "$($set.Name) must keep .shell_cmd empty." }
        Require-NoRegex $nmOut '\b(_strtod_l|__gethex|_scanf_float|strtof|strtod)\b' "$($set.Name) retains float parsing chain."
        Require-NoRegex $mapRaw '\b(_strtod_l|__gethex|_scanf_float)\b' "$($set.Name) map retains float parsing chain."
        Require-NoRegex $nmOut $ipc "$($set.Name) retains IPC runtime symbols."
        if (-not $set.Shell) {
            Require-NoRegex $nmOut $shell "$($set.Name) retains Shell symbols."
            Require-NoRegex $nmOut '\b_shell_cmd_[A-Za-z0-9_]+\b' "$($set.Name) retains Shell command symbols."
            Require-Regex $mapRaw '(?m)^\.shell_cmd\s+0x[0-9A-Fa-f]+\s+0x0\b' "$($set.Name) map must show empty .shell_cmd."
        }
    }
}

Write-Host "[current_contract] FalconFoc current AT32F415 contract"

Require-FirmwareArtifactContracts

$root = Read-Utf8Raw "CMakeLists.txt"
$metadata = Read-Utf8Raw "cmake\ThemisPlatformMetadata.cmake"
foreach ($token in @(
        "THEMIS_FEATURE_SHELL", "THEMIS_FEATURE_IPC", "THEMIS_FEATURE_VOFA",
        "THEMIS_FEATURE_TASK_BUDGET", "THEMIS_FEATURE_VERBOSE_LOG",
        "THEMIS_LINKER_MIN_HEAP_SIZE", "THEMIS_LINKER_MIN_STACK_SIZE", "configure_file(",
        "startup_at32f415.s", "AT32F415xB_FLASH.ld", "AT32F415KBU7_4"
    )) { Require-NotContains $root $token "Root CMake owns forbidden current product token '$token'." }
foreach ($token in @(
        "include(ThemisPlatformMetadata)", "include(ThemisBuildAxes)",
        "include(ThemisFirmwareAssembly)", "include(ThemisContracts)",
        "include(ThemisStatusReport)", "themis_build_axes_declare_cache()",
        "themis_build_axes_validate()", "themis_define_contract_targets(",
        "themis_emit_status_report()"
    )) { Require-Contains $root $token "Root CMake missing current boundary token '$token'." }
foreach ($token in @(
        "THEMIS_PLATFORM_VENDOR", "THEMIS_PLATFORM_SOC", "THEMIS_PLATFORM_DIR_OVERRIDE",
        "THEMIS_PLATFORM_TARGET_OVERRIDE", "THEMIS_ENTRY_SOURCE_OVERRIDE",
        "THEMIS_STARTUP_SOURCE_OVERRIDE", "THEMIS_LINKER_SCRIPT_OVERRIDE",
        "THEMIS_PLATFORM_COMPILE_DEFINITIONS_OVERRIDE", "THEMIS_CPU_CLASS_OVERRIDE",
        "ThemisPlatformDefaults.cmake", "ThemisCpuClassDefaults.cmake"
    )) { Require-Contains $metadata $token "Metadata helper missing token '$token'." }

foreach ($helper in @(
        "cmake\ThemisBuildAxes.cmake", "cmake\ThemisFirmwareAssembly.cmake",
        "cmake\ThemisContracts.cmake", "cmake\ThemisStatusReport.cmake"
    )) { Require-Path $helper }

$platform = Read-Utf8Raw "platform\at\at32f415\ThemisPlatformDefaults.cmake"
foreach ($token in @(
        'THEMIS_PLATFORM_TARGET_DEFAULT "at32f415-hal"',
        'THEMIS_PLATFORM_ENTRY_SOURCE_DEFAULT "generated/src/main.c"',
        'startup_at32f415.s', 'AT32F415xB_FLASH.ld',
        'THEMIS_PLATFORM_CPU_CLASS_DEFAULT "cortex-m3-soft"',
        'AT32F415KBU7_4', 'CMSIS_device_header=<at32f415.h>',
        'THEMIS_PLATFORM_FLASH_DRIVER_HEADER=<at32f415_flash.h>',
        'THEMIS_PLATFORM_FLASH_BASE_ADDR=0x08000000U',
        'THEMIS_PLATFORM_FLASH_SIZE_BYTES=0x00020000U',
        'THEMIS_PLATFORM_FLASH_SECTOR_SIZE_BYTES=0x00000400U',
        'THEMIS_PLATFORM_FLASH_SECTOR_COUNT=128U',
        'THEMIS_PLATFORM_MOTOR_SOFTIRQ_HANDLER=TMR5_GLOBAL_IRQHandler',
        'THEMIS_PLATFORM_MOTOR_SOFTIRQ_INSTANCE=TMR5'
    )) { Require-Contains $platform $token "AT32F415 metadata missing '$token'." }
Require-NotContains $platform 'ARM_MATH_CM4' 'AT32F415 metadata must not use ARM_MATH_CM4.'

foreach ($cpu in @(
    @{ Path = "platform\cpu\cortex-m3-soft\ThemisCpuClassDefaults.cmake"; Tokens = @(
            'ARM_CM3', 'ARM_MATH_CM3',
            'set(THEMIS_CPU_TOOLCHAIN_CPU_DEFAULT "cortex-m3")',
            'set(THEMIS_CPU_TOOLCHAIN_FPU_DEFAULT "none")',
            'set(THEMIS_CPU_TOOLCHAIN_FLOAT_DEFAULT "soft")',
            'set(THEMIS_CPU_FREERTOS_PORT_GLOB_DEFAULT "portable/GCC/ARM_CM3/*.c")',
            'set(THEMIS_CPU_FREERTOS_PORT_INCLUDE_DIR_DEFAULT "portable/GCC/ARM_CM3")'
        ) },
    @{ Path = "platform\cpu\cortex-m4f-hard\ThemisCpuClassDefaults.cmake"; Tokens = @(
            'ARM_CM4F', 'ARM_MATH_CM4',
            'set(THEMIS_CPU_TOOLCHAIN_CPU_DEFAULT "cortex-m4")',
            'set(THEMIS_CPU_TOOLCHAIN_FPU_DEFAULT "fpv4-sp-d16")',
            'set(THEMIS_CPU_TOOLCHAIN_FLOAT_DEFAULT "hard")',
            'set(THEMIS_CPU_FREERTOS_PORT_GLOB_DEFAULT "portable/GCC/ARM_CM4F/*.c")',
            'set(THEMIS_CPU_FREERTOS_PORT_INCLUDE_DIR_DEFAULT "portable/GCC/ARM_CM4F")'
        ) }
)) {
    $raw = Read-Utf8Raw $cpu.Path
    foreach ($token in @(
            'THEMIS_CPU_TOOLCHAIN_CPU_DEFAULT', 'THEMIS_CPU_TOOLCHAIN_FPU_DEFAULT',
            'THEMIS_CPU_TOOLCHAIN_FLOAT_DEFAULT', 'THEMIS_CPU_FREERTOS_PORT_GLOB_DEFAULT',
            'THEMIS_CPU_FREERTOS_PORT_INCLUDE_DIR_DEFAULT', 'THEMIS_CPU_ARM_MATH_DEFINE_DEFAULT'
        ) + $cpu.Tokens) { Require-Contains $raw $token "CPU metadata '$($cpu.Path)' missing '$token'." }
}

$toolchain = Read-Utf8Raw "tools\cmake\arm-gcc-toolchain.cmake"
foreach ($token in @('ThemisPlatformMetadata.cmake','set(MCU_CPU','set(MCU_FPU','set(MCU_FLOAT')) {
    Require-Contains $toolchain $token "Toolchain missing '$token'."
}
foreach ($token in @('"cortex-m4"','"fpv4-sp-d16"','"hard"','"portable/GCC/ARM_CM4F/*.c"','"portable/GCC/ARM_CM4F"')) {
    Require-NotContains $toolchain $token "Generic toolchain hardcodes CPU-class token '$token'."
}

$freertos = Read-Utf8Raw "kernel\freertos\CMakeLists.txt"
Require-Contains $freertos 'THEMIS_CPU_FREERTOS_PORT_GLOB' 'FreeRTOS manifest must consume CPU class port glob.'
Require-Contains $freertos 'THEMIS_CPU_FREERTOS_PORT_INCLUDE_DIR' 'FreeRTOS manifest must consume CPU class include dir.'
Require-NotContains $freertos 'ARM_CM4F' 'FreeRTOS manifest must not hardcode ARM_CM4F.'
$rte = Read-Utf8Raw "kernel\freertos\CMSIS_RTOS_V2\RTE_Components.h"
Require-NotContains $rte 'at32f403a_407.h' 'RTE header must not retain the historical 403A header.'
Require-Contains $rte 'CMSIS_device_header' 'RTE header must use CMSIS_device_header.'

$linker = Read-Utf8Raw "platform\at\at32f415\vendor\cmsis\cm4\device_support\startup\gcc\linker\AT32F415xB_FLASH.ld"
Require-Contains $linker '_Min_Heap_Size = 0;' 'AT32F415 linker heap reserve changed.'
Require-Contains $linker '_Min_Stack_Size = 0x400;' 'AT32F415 linker stack reserve changed.'
Require-NotContains $linker '@THEMIS_LINKER_MIN_HEAP_SIZE@' 'AT32F415 linker retains heap placeholder.'
Require-NotContains $linker '@THEMIS_LINKER_MIN_STACK_SIZE@' 'AT32F415 linker retains stack placeholder.'
Require-Regex $linker 'FLASH\s*\(rx\).*ORIGIN\s*=\s*0x08000000.*LENGTH\s*=\s*128K' 'AT32F415 linker Flash geometry changed.'
Require-Regex $linker 'RAM\s*\(xrw\).*ORIGIN\s*=\s*0x20000000.*LENGTH\s*=\s*32K' 'AT32F415 linker RAM geometry changed.'

$timing = Read-Utf8Raw "applications\algo\foc_config.h"
$platformTiming = Read-Utf8Raw "platform\at\at32f415\generated\inc\tm_platform_foc_timing.h"
Require-Contains $timing '#include "tm_platform_foc_timing.h"' 'Legacy FOC config must consume the AT32F415 platform timing seam.'
foreach ($token in @('#define TMR_CLK_HZ             TM_PLATFORM_TMR_CLK_HZ','#define TMR_CLK_MHZ            TM_PLATFORM_TMR_CLK_MHZ','VEL_LOOP_FREQ_HZ       (5000U)','POS_LOOP_FREQ_HZ       (1000U)','TRAJ_TASK_FREQ_HZ      (1000U)')) {
    Require-Contains $timing $token "Current Legacy timing token '$token' missing."
}
foreach ($token in @('#define TM_PLATFORM_TMR_CLK_HZ (150000000U)','#define TM_PLATFORM_TMR_CLK_MHZ (150U)','#define TM_PLATFORM_LEGACY_TMR1_CARRIER_HZ (20000U)','#define TM_PLATFORM_LEGACY_TMR5_SOFTIRQ_HZ (5000U)','#define TM_PLATFORM_V2_TIMEBASE_HZ (1000000U)')) {
    Require-Contains $platformTiming $token "AT32F415 platform timing fact '$token' missing."
}
Require-NotContains $timing 'TMR_CLK_HZ             (150000000U)' 'Legacy app config must not own the AT32F415 timer clock value.'
Require-NotContains $timing 'TMR_CLK_MHZ            (150U)' 'Legacy app config must not own the AT32F415 timer clock MHz value.'
Require-NotContains $platformTiming '240000000U' 'Current AT32F415 timing must not retain historical 240 MHz value.'
Write-Host '[current_contract] AT32F415 timer-clock and fixed peripheral cadence facts are platform-owned; Legacy control cadence remains application-owned.'

$bspManifest = Read-Utf8Raw "bsp\board\CMakeLists.txt"
Require-NotContains $bspManifest '../../applications/tasks' 'BSP must not export application task sources.'
foreach ($path in @(
    'bsp\ports\bsp_console.h','bsp\ports\bsp_system.h'
)) {
    $raw = Read-Utf8Raw $path
    foreach ($token in @('CMSIS_device_header','at32f403a_407.h','at32f415.h','at32f423.h')) {
        Require-NotContains $raw $token "$path leaked vendor token '$token'."
    }
}
foreach ($path in @('bsp\ports\bsp_console.c','bsp\ports\bsp_system.c','bsp\board\bsp_flash.c')) {
    $raw = Read-Utf8Raw $path
    Require-Contains $raw 'tm_runtime.h' "$path lost runtime seam."
}

foreach ($path in @(
    'applications\shell\app_shell_cmd_cfg.c','applications\shell\app_shell_cmd_cal.c',
    'applications\shell\app_shell_cmd_diag.c','applications\shell\app_shell_cmd_fault.c',
    'applications\shell\app_shell_cmd_state.c','applications\shell\app_shell_cmd_store.c'
)) {
    $raw = Read-Utf8Raw $path
    Require-NoRegex $raw '\b_motor_ins\b' "$path reached _motor_ins directly."
    Require-NoRegex $raw 'motor->' "$path reached motor-> directly."
}
$fault = Read-Utf8Raw 'applications\runtime\motor_fault_runtime.c'
Require-NoRegex $fault 'motor->axis_state\s*=[^=]' 'Fault runtime writes axis_state directly.'
$focCtrl = Read-Utf8Raw 'applications\periph\foc_ctrl.c'
foreach ($token in @('void foc_cmd_passthrough(','void foc_cmd_vel_ramp(','void foc_cmd_torque_ramp(','void foc_cmd_pos_traj(','void foc_fault_check(','void foc_process_input_task(')) {
    Require-NoRegex $focCtrl ('(?m)^\s*' + [Regex]::Escape($token)) "foc_ctrl still owns '$token'."
}
Require-NoRegex $focCtrl '(?m)^\s*#if 0\b' 'foc_ctrl retains migration leftovers.'

$config = Read-Utf8Raw 'include\config\tm_core_config.h'
foreach ($token in @('#define TM_FEATURE_IPC (0U)','#define TM_FEATURE_VOFA (0U)','#define TM_FEATURE_TASK_BUDGET (0U)','#define TM_FEATURE_VERBOSE_LOG (0U)','#define TM_APP_FEATURE_UART_TELEMETRY (0U)','#define TM_APP_FEATURE_UART_PASSIVE_TELEMETRY (0U)')) {
    Require-Contains $config $token "Current product default missing '$token'."
}
$bspConfig = Read-Utf8Raw 'bsp\bsp_config.h'
foreach ($token in @('BOARD_CONSOLE_UART_NAME','BOARD_TELEMETRY_UART_NAME','BOARD_TELEMETRY_UART_NAME   BOARD_CONSOLE_UART_NAME')) {
    Require-Contains $bspConfig $token "BSP config missing '$token'."
}

$main = Read-Utf8Raw 'platform\at\at32f415\generated\src\main.c'
Require-Contains $main '#include "app_main.h"' 'AT32F415 startup lost app_main bridge.'
Require-Contains $main 'app_main_bootstrap();' 'AT32F415 startup lost app_main bootstrap.'
$motorTask = Read-Utf8Raw 'applications\tasks\motor_task.c'
$motorInit = $motorTask.IndexOf('void motor_task_init(void)', [StringComparison]::Ordinal)
$shellInit = $motorTask.IndexOf('shell_task_init();', [StringComparison]::Ordinal)
$runtimeInit = $motorTask.IndexOf('motor_runtime_init(&_motor_ins);', [StringComparison]::Ordinal)
if (($motorInit -lt 0) -or ($shellInit -lt $motorInit) -or ($runtimeInit -lt $shellInit)) { throw 'Current startup owner order changed.' }
$bootDocs = @(Get-ChildItem -LiteralPath (Join-Path $RepoRoot 'docs\boot') -Filter '*.md' -File)
if ($bootDocs.Count -lt 2) { throw 'Current contract requires at least two boot documents.' }
foreach ($docFile in $bootDocs) {
    $doc = $docFile.FullName
    $raw = [IO.File]::ReadAllText($doc, [Text.UTF8Encoding]::new($false))
    Require-Contains $raw 'platform/at/at32f415/generated/src/main.c' "$doc has wrong current platform path."
    Require-NotContains $raw 'platform/at/at32f403a/generated/src/main.c' "$doc retains historical live path."
    foreach ($token in @('applications/app_main.c','motor_task_init','shell_task_init','app_shell_task','vofa_task_init')) {
        Require-Contains $raw $token "$doc missing '$token'."
    }
}

$serial = Read-Utf8Raw 'firmware\at32f415\src\foc_serial.c'
$board = Read-Utf8Raw 'firmware\at32f415\src\foc_board.c'
Require-Contains $serial 'FOC_PROTO_OUTPUT_LOCKED' 'Board1 serial output lock missing.'
Require-Contains $board 'FOC_BOARD_UPDATE_NOT_APPLIED' 'Board1 update lock missing.'
Require-Contains $board 'int foc_board_init(struct foc_runtime' 'Board1 default init missing.'

Write-Host '[current_contract] PASS'
