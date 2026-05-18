param(
    [string]$RepoRoot = "",

    [string]$PortName = "auto",

    [string]$BuildDir = "build_tiny_runtime",

    [string]$BuildType = "MinSizeRel",

    [string]$OutputDir = "",

    [string]$GitBashPath = "",

    [switch]$SkipBuildAndFlash,

    [switch]$SkipMemoryReport,

    [switch]$SkipShellProbe,

    [switch]$SkipRuntimeScenarios,

    [switch]$SkipWatchdogShellTrip,

    [switch]$IncludeWatchdogOpenOcdInject,

    [string]$OpenOcdPath = "openocd",

    [string]$OpenOcdConfigPath = "tools/openocd/openocd_daplink.cfg"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptsRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
$defaultRepoRoot = Split-Path -Parent $scriptsRoot

function Resolve-RepoRootPath
{
    param([string]$Root)

    if (-not [string]::IsNullOrWhiteSpace($Root)) {
        return (Resolve-Path -LiteralPath $Root).Path
    }

    return $defaultRepoRoot
}

function Resolve-BashExecutable
{
    param([string]$PreferredPath)

    if (-not [string]::IsNullOrWhiteSpace($PreferredPath)) {
        return (Resolve-Path -LiteralPath $PreferredPath).Path
    }

    foreach ($commandName in @("bash.exe", "bash")) {
        $command = Get-Command $commandName -ErrorAction SilentlyContinue
        if ($command -ne $null) {
            return $command.Source
        }
    }

    foreach ($candidate in @(
                 "D:\\Git\\Git\\bin\\bash.exe",
                 "C:\\Program Files\\Git\\bin\\bash.exe",
                 "C:\\Program Files\\Git\\usr\\bin\\bash.exe"
             )) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    throw "Git Bash was not found. Pass -GitBashPath explicitly."
}

function Convert-ToBashPath
{
    param([string]$WindowsPath)

    $fullPath = [System.IO.Path]::GetFullPath($WindowsPath)
    $normalized = $fullPath.Replace("\", "/")
    if ($normalized -match "^([A-Za-z]):/(.*)$") {
        return "/" + $matches[1].ToLowerInvariant() + "/" + $matches[2]
    }

    return $normalized
}

function Invoke-GitBashStep
{
    param(
        [string]$BashExecutable,
        [string]$ScriptText,
        [string]$Label
    )

    Write-Host ("[tiny_carrier_regression] bash: " + $Label)
    & $BashExecutable -lc $ScriptText
    if ($LASTEXITCODE -ne 0) {
        throw "Git Bash step failed: $Label"
    }
}

function Invoke-CaptureScript
{
    param(
        [string]$ScriptPath,
        [object[]]$ScriptArguments,
        [string]$Label
    )

    Write-Host ("[tiny_carrier_regression] probe: " + $Label)
    & powershell -NoProfile -ExecutionPolicy Bypass -File $ScriptPath @ScriptArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Probe step failed: $Label"
    }
}

function Assert-RebuiltProbeLane
{
    param(
        [string]$ProbeName
    )

    throw ("FalconFoc board probe lane '{0}' is not rebuilt yet. " +
           "Current live regression flow only guarantees build + memory report." -f $ProbeName)
}

function Get-MarkdownBulletValue
{
    param(
        [string]$Path,
        [string]$Label
    )

    if ([string]::IsNullOrWhiteSpace($Path) -or (-not (Test-Path -LiteralPath $Path))) {
        return ""
    }

    $pattern = '^- {0}: `(?<value>.+)`$' -f [regex]::Escape($Label)
    foreach ($line in [System.IO.File]::ReadAllLines($Path, [System.Text.UTF8Encoding]::new($false))) {
        if ($line -match $pattern) {
            return $matches["value"]
        }
    }

    return ""
}

function New-RegressionSummary
{
    param(
        [string]$SummaryPath,
        [string]$Port,
        [string]$BuildDirectory,
        [string]$BuildConfiguration,
        [string]$ShellLog,
        [string]$ScenarioLog,
        [string]$ShellTripLog,
        [string]$ShellTripSummary,
        [string]$ShellTripBaseline,
        [string]$OpenOcdLog,
        [string]$OpenOcdSummary,
        [string]$OpenOcdBaseline,
        [bool]$DidBuildAndFlash,
        [bool]$DidMemoryReport,
        [bool]$DidShellProbe,
        [bool]$DidScenarioProbe,
        [bool]$DidShellTrip,
        [bool]$DidOpenOcdInject
    )

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("# Themis tiny + carrier-demo minimal board regression")
    $lines.Add("")
    $lines.Add("## 1. Run Context")
    $lines.Add("")
    $lines.Add(('- Port: `{0}`' -f $Port))
    $lines.Add(('- BuildDir: `{0}`' -f $BuildDirectory))
    $lines.Add(('- BuildType: `{0}`' -f $BuildConfiguration))
    $lines.Add('- Profile: `tiny`')
    $lines.Add('- Flavor: `carrier-demo`')
    $lines.Add(('- UTC: `{0:o}`' -f [DateTime]::UtcNow))
    $lines.Add("")
    $lines.Add("## 2. Executed Steps")
    $lines.Add("")
    $lines.Add(('- Build + flash: `{0}`' -f ($(if ($DidBuildAndFlash) { "done" } else { "skipped" }))))
    $lines.Add(('- Memory report: `{0}`' -f ($(if ($DidMemoryReport) { "done" } else { "skipped" }))))
    $lines.Add(('- Shell runtime probe: `{0}`' -f ($(if ($DidShellProbe) { "done" } else { "skipped" }))))
    $lines.Add(('- Runtime scenarios: `{0}`' -f ($(if ($DidScenarioProbe) { "done" } else { "skipped" }))))
    $lines.Add(('- Watchdog shell-trip: `{0}`' -f ($(if ($DidShellTrip) { "done" } else { "skipped" }))))
    $lines.Add(('- Watchdog OpenOCD inject: `{0}`' -f ($(if ($DidOpenOcdInject) { "done" } else { "skipped" }))))
    $lines.Add("")
    $lines.Add("## 3. Artifacts")
    $lines.Add("")
    if ($DidShellProbe) {
        $lines.Add(('- Shell runtime log: `{0}`' -f $ShellLog))
    }
    if ($DidScenarioProbe) {
        $lines.Add(('- Runtime scenarios log: `{0}`' -f $ScenarioLog))
    }
    if ($DidShellTrip) {
        $lines.Add(('- Watchdog shell-trip log: `{0}`' -f $ShellTripLog))
        $lines.Add(('- Watchdog shell-trip summary: `{0}`' -f $ShellTripSummary))
        $lines.Add(('- Fault recovery baseline: `{0}`' -f $ShellTripBaseline))
    }
    if ($DidOpenOcdInject) {
        $lines.Add(('- Watchdog OpenOCD log: `{0}`' -f $OpenOcdLog))
        $lines.Add(('- Watchdog OpenOCD summary: `{0}`' -f $OpenOcdSummary))
        $lines.Add(('- Fault recovery baseline (OpenOcdInject): `{0}`' -f $OpenOcdBaseline))
    }
    $lines.Add("")
    $lines.Add("## 4. Manual Checkpoints")
    $lines.Add("")
    $lines.Add("- Confirm boot banner and shell prompt are visible after flash.")
    $lines.Add('- Confirm `ver` reports `profile=tiny` and `flavor=carrier-demo`.')
    $lines.Add('- Confirm `stack / tasks / watchdog / mem` all return structured output.')
    $lines.Add('- Confirm runtime logs still show healthy `heap_min` and no unexpected reboot before the watchdog step.')
    if ($DidShellProbe -or $DidScenarioProbe -or $DidShellTrip -or $DidOpenOcdInject) {
        $lines.Add("- Confirm the rebuilt FalconFoc board probe lane documents its own expected runtime checks.")
    }

    [System.IO.File]::WriteAllLines($SummaryPath, $lines, [System.Text.UTF8Encoding]::new($false))
}

$resolvedRepoRoot = Resolve-RepoRootPath -Root $RepoRoot
$repoRoot = $resolvedRepoRoot

$needsSerialPort = (-not $SkipShellProbe) -or
    (-not $SkipRuntimeScenarios) -or
    (-not $SkipWatchdogShellTrip) -or
    $IncludeWatchdogOpenOcdInject

$resolvedPortName = $PortName
if ((-not $needsSerialPort) -and
    ([string]::IsNullOrWhiteSpace($resolvedPortName) -or ($resolvedPortName -eq "auto"))) {
    $resolvedPortName = "n/a"
}

if ($needsSerialPort -and
    ([string]::IsNullOrWhiteSpace($resolvedPortName) -or ($resolvedPortName -eq "auto"))) {
    $portFinderScript = Join-Path $scriptsRoot "find_themis_shell_port.ps1"
    Write-Host "[tiny_carrier_regression] probe: discover shell port"
    $resolvedPortName = & $portFinderScript `
        -ExpectedProfile "tiny" `
        -ExpectedFlavor "carrier-demo"
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($resolvedPortName)) {
        throw "Failed to auto-discover Themis shell port."
    }

    $resolvedPortName = $resolvedPortName.Trim()
    Write-Host ("[tiny_carrier_regression] port: " + $resolvedPortName)
}

$resolvedOutputDir = if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    Join-Path $repoRoot (Join-Path $BuildDir "output\\board_regression")
} elseif ([System.IO.Path]::IsPathRooted($OutputDir)) {
    $OutputDir
} else {
    Join-Path $repoRoot $OutputDir
}

New-Item -ItemType Directory -Force -Path $resolvedOutputDir | Out-Null

$timestamp = [DateTime]::Now.ToString("yyyyMMdd_HHmmss")
$shellLog = Join-Path $resolvedOutputDir ("tiny_carrier_shell_runtime_{0}.log" -f $timestamp)
$scenarioLog = Join-Path $resolvedOutputDir ("tiny_carrier_runtime_scenarios_{0}.log" -f $timestamp)
$shellTripLog = Join-Path $resolvedOutputDir ("tiny_carrier_watchdog_shelltrip_{0}.log" -f $timestamp)
$openOcdLog = Join-Path $resolvedOutputDir ("tiny_carrier_watchdog_openocd_{0}.log" -f $timestamp)
$summaryPath = Join-Path $resolvedOutputDir ("tiny_carrier_regression_{0}.md" -f $timestamp)

$didBuildAndFlash = $false
$didMemoryReport = $false
$didShellProbe = $false
$didScenarioProbe = $false
$didShellTrip = $false
$didOpenOcdInject = $false

if (-not $SkipBuildAndFlash -or -not $SkipMemoryReport) {
    $bashExe = Resolve-BashExecutable -PreferredPath $GitBashPath
    $bashRepoRoot = Convert-ToBashPath -WindowsPath $repoRoot
}

if (-not $SkipBuildAndFlash) {
    $buildCommand = @"
cd '$bashRepoRoot' && \
THEMIS_BUILD_DIR='$BuildDir' \
THEMIS_BUILD_TYPE='$BuildType' \
THEMIS_DEVDRV_PROFILE=tiny \
THEMIS_PRODUCT_FLAVOR=carrier-demo \
./run.sh all
"@
    Invoke-GitBashStep -BashExecutable $bashExe -ScriptText $buildCommand -Label "run.sh all"
    $didBuildAndFlash = $true
}

if (-not $SkipMemoryReport) {
    $memoryCommand = @"
cd '$bashRepoRoot' && \
THEMIS_BUILD_DIR='$BuildDir' \
THEMIS_BUILD_TYPE='$BuildType' \
THEMIS_DEVDRV_PROFILE=tiny \
THEMIS_PRODUCT_FLAVOR=carrier-demo \
./run.sh memory
"@
    Invoke-GitBashStep -BashExecutable $bashExe -ScriptText $memoryCommand -Label "run.sh memory"
    $didMemoryReport = $true
}

$shellTripSummary = [System.IO.Path]::ChangeExtension($shellTripLog, ".summary.md")
$openOcdSummary = [System.IO.Path]::ChangeExtension($openOcdLog, ".summary.md")
$shellTripBaseline = ""
$openOcdBaseline = ""

if (-not $SkipShellProbe) {
    Write-Host "[tiny_carrier_regression] skip: shell runtime probe lane is not rebuilt for FalconFoc live board flow."
}

if (-not $SkipRuntimeScenarios) {
    Write-Host "[tiny_carrier_regression] skip: runtime scenario probe lane is not rebuilt for FalconFoc live board flow."
}

if (-not $SkipWatchdogShellTrip) {
    Assert-RebuiltProbeLane -ProbeName "watchdog shell-trip"
}

if ($IncludeWatchdogOpenOcdInject) {
    Assert-RebuiltProbeLane -ProbeName "watchdog OpenOCD inject"
}

New-RegressionSummary -SummaryPath $summaryPath `
                      -Port $resolvedPortName `
                      -BuildDirectory $BuildDir `
                      -BuildConfiguration $BuildType `
                      -ShellLog $shellLog `
                      -ScenarioLog $scenarioLog `
                      -ShellTripLog $shellTripLog `
                      -ShellTripSummary $shellTripSummary `
                      -ShellTripBaseline $shellTripBaseline `
                      -OpenOcdLog $openOcdLog `
                      -OpenOcdSummary $openOcdSummary `
                      -OpenOcdBaseline $openOcdBaseline `
                      -DidBuildAndFlash $didBuildAndFlash `
                      -DidMemoryReport $didMemoryReport `
                      -DidShellProbe $didShellProbe `
                      -DidScenarioProbe $didScenarioProbe `
                      -DidShellTrip $didShellTrip `
                      -DidOpenOcdInject $didOpenOcdInject

Write-Host ("[tiny_carrier_regression] summary: " + $summaryPath)
