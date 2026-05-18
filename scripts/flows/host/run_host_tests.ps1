param(
    [string]$RepoRoot = "",
    [string]$CompilerPath = "",
    [switch]$KeepArtifacts
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

# Shared-subsys manifest compatibility markers:
# "subsys/input/tm_input.c"
# "subsys/led/tm_led.c"
# "subsys/runtime/tm_task_budget_probe.c"
# "applications/tasks/app_shell_task.c"
# "subsys/ui/screen_manager.c"
# "subsys/ui/screen_row_editor.c"
# "subsys/input"
# "subsys/led"
# "subsys/ui"
# "test_led_service"
# "test_tm_shell_task"
# "test_screen_row_editor"
# "test_app_task_budget_probe"
# "test_app_shell_watchdog_cmd"
# "test_watchdog_service"
# "test_display_service"
# "test_tm_i2c_contract"

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

function Resolve-HostCompilerPair
{
    param([string]$PreferredPath)

    if ($PreferredPath -eq "") {
        return $null
    }

    $resolvedPath = Resolve-AbsolutePath -Path $PreferredPath
    $directory    = Split-Path -Path $resolvedPath -Parent
    $leaf         = (Split-Path -Path $resolvedPath -Leaf).ToLowerInvariant()
    $cCompiler    = $resolvedPath
    $cxxCompiler  = $null

    switch ($leaf) {
        "gcc.exe" {
            $cxxCompiler = Join-Path $directory "g++.exe"
        }
        "gcc" {
            $cxxCompiler = Join-Path $directory "g++"
        }
        "g++.exe" {
            $cCompiler   = Join-Path $directory "gcc.exe"
            $cxxCompiler = $resolvedPath
        }
        "g++" {
            $cCompiler   = Join-Path $directory "gcc"
            $cxxCompiler = $resolvedPath
        }
        default {
            $probe = Join-Path $directory "g++.exe"
            if (-not (Test-Path -LiteralPath $probe)) {
                $probe = Join-Path $directory "g++"
            }
            $cxxCompiler = $probe
        }
    }

    if (-not (Test-Path -LiteralPath $cCompiler)) {
        throw "Unable to resolve the host C compiler pair from '$resolvedPath'."
    }

    if (-not (Test-Path -LiteralPath $cxxCompiler)) {
        throw "Unable to resolve the host C++ compiler pair from '$resolvedPath'."
    }

    return @{
        CCompiler   = (Resolve-AbsolutePath -Path $cCompiler)
        CxxCompiler = (Resolve-AbsolutePath -Path $cxxCompiler)
    }
}

function Invoke-NativeCommand
{
    param(
        [string]$Executable,
        [string[]]$Arguments,
        [string]$DisplayCommand
    )

    Write-Host $DisplayCommand
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed: $Executable"
    }
}

$paths        = Resolve-ThemisPaths -Root $RepoRoot
$compilerPair = Resolve-HostCompilerPair -PreferredPath $CompilerPath
$hostBuildDir = Join-Path $paths.RepoRoot "build\host-debug"

. (Join-Path $paths.RepoRoot "scripts\lib\cmake_cache_guard.ps1")

$configureArgs = @("--preset", "host-debug")
if ($compilerPair -ne $null) {
    $configureArgs += @(
        ("-DCMAKE_C_COMPILER={0}" -f $compilerPair.CCompiler),
        ("-DCMAKE_CXX_COMPILER={0}" -f $compilerPair.CxxCompiler)
    )
}

Push-Location $paths.RepoRoot
try {
    Write-Host ("[run_host_tests] git root: " + $paths.GitRoot)
    Write-Host ("[run_host_tests] repo root: " + $paths.RepoRoot)
    Write-Host "[run_host_tests] flow: host-debug -> build-host-debug -> test-host-debug"

    if ($compilerPair -ne $null) {
        Write-Host ("[run_host_tests] C compiler: " + $compilerPair.CCompiler)
        Write-Host ("[run_host_tests] CXX compiler: " + $compilerPair.CxxCompiler)
    }

    if ($KeepArtifacts) {
        Write-Host "[run_host_tests] KeepArtifacts is implicit for preset builds; artifacts stay under build/host-debug."
    }

    Repair-CMakeBuildCacheIfRelocated -ExpectedSourceDir $paths.RepoRoot -BuildDir $hostBuildDir | Out-Null

    Invoke-NativeCommand -Executable "cmake" `
                         -Arguments $configureArgs `
                         -DisplayCommand ("[run_host_tests] run: cmake " + ($configureArgs -join " "))
    Invoke-NativeCommand -Executable "cmake" `
                         -Arguments @("--build", "--preset", "build-host-debug", "--parallel") `
                         -DisplayCommand "[run_host_tests] run: cmake --build --preset build-host-debug --parallel"
    Invoke-NativeCommand -Executable "ctest" `
                         -Arguments @("--preset", "test-host-debug") `
                         -DisplayCommand "[run_host_tests] run: ctest --preset test-host-debug"

    Write-Host "[run_host_tests] all host tests passed."
}
finally {
    Pop-Location
}
