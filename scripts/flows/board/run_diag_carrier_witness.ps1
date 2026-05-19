param(
    [string]$RepoRoot = "",

    [string]$PortName = "auto",

    [string]$BuildDir = "build/fw-full-diag-carrier-debug",

    [string]$BuildType = "Debug",

    [string]$OutputDir = "",

    [string]$GitBashPath = "",

    [switch]$SkipBuildAndFlash,

    [switch]$SkipMemoryReport,

    [switch]$SkipReboot,

    [int]$BaudRate = 1000000,

    [int]$ReadSliceMs = 50,

    [int]$DefaultQuietMs = 500
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptsRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
$defaultRepoRoot = Split-Path -Parent $scriptsRoot
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

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

    Write-Host ("[diag_carrier_witness] bash: " + $Label)
    & $BashExecutable -lc $ScriptText
    if ($LASTEXITCODE -ne 0) {
        throw "Git Bash step failed: $Label"
    }
}

function Read-SerialChunk
{
    param([System.IO.Ports.SerialPort]$SerialPort)

    if ($SerialPort.BytesToRead -le 0) {
        return ""
    }

    $buffer = New-Object byte[] $SerialPort.BytesToRead
    $read = $SerialPort.Read($buffer, 0, $buffer.Length)
    if ($read -le 0) {
        return ""
    }

    return $SerialPort.Encoding.GetString($buffer, 0, $read)
}

function Collect-SerialUntilQuiet
{
    param(
        [System.IO.Ports.SerialPort]$SerialPort,
        [int]$TimeoutMs,
        [int]$QuietMs
    )

    $captured = New-Object System.Text.StringBuilder
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    $quietSince = [DateTime]::UtcNow

    while ([DateTime]::UtcNow -lt $deadline) {
        $chunk = Read-SerialChunk -SerialPort $SerialPort
        if (-not [string]::IsNullOrEmpty($chunk)) {
            [void]$captured.Append($chunk)
            $quietSince = [DateTime]::UtcNow
        } elseif (([DateTime]::UtcNow - $quietSince).TotalMilliseconds -ge $QuietMs) {
            break
        }

        Start-Sleep -Milliseconds $ReadSliceMs
    }

    return $captured.ToString()
}

function Invoke-ShellCapture
{
    param(
        [System.IO.Ports.SerialPort]$SerialPort,
        [string]$Command,
        [int]$TimeoutMs = 2400,
        [int]$QuietMs = 500
    )

    $SerialPort.Write($Command + "`r`n")
    Start-Sleep -Milliseconds 120
    return Collect-SerialUntilQuiet -SerialPort $SerialPort -TimeoutMs $TimeoutMs -QuietMs $QuietMs
}

function Add-CheckResult
{
    param(
        [System.Collections.Generic.List[object]]$Results,
        [string]$Name,
        [bool]$Passed,
        [string]$Detail
    )

    $Results.Add([pscustomobject]@{
            Name   = $Name
            Passed = $Passed
            Detail = $Detail
        })
}

function Test-ContainsAll
{
    param(
        [string]$Text,
        [string[]]$Tokens
    )

    foreach ($token in $Tokens) {
        if ($Text -notmatch [regex]::Escape($token)) {
            return $false
        }
    }

    return $true
}

function New-WitnessSummary
{
    param(
        [string]$SummaryPath,
        [string]$Port,
        [string]$BuildDirectory,
        [string]$BuildConfiguration,
        [string]$RawLogPath,
        [string]$MemoryReportPath,
        [bool]$DidBuildAndFlash,
        [bool]$DidMemoryReport,
        [System.Collections.Generic.List[object]]$Checks
    )

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("# FalconFoc diag/cal board witness")
    $lines.Add("")
    $lines.Add("## 1. Run Context")
    $lines.Add("")
    $lines.Add(('- Port: `{0}`' -f $Port))
    $lines.Add(('- BuildDir: `{0}`' -f $BuildDirectory))
    $lines.Add(('- BuildType: `{0}`' -f $BuildConfiguration))
    $lines.Add('- Profile: `full-diag`')
    $lines.Add('- Flavor: `carrier-demo`')
    $lines.Add(('- UTC: `{0:o}`' -f [DateTime]::UtcNow))
    $lines.Add("")
    $lines.Add("## 2. Executed Steps")
    $lines.Add("")
    $lines.Add(('- Build + flash: `{0}`' -f $(if ($DidBuildAndFlash) { "done" } else { "skipped" })))
    $lines.Add(('- Memory report: `{0}`' -f $(if ($DidMemoryReport) { "done" } else { "skipped" })))
    $lines.Add('- Witness flow: `sys reboot -> help -> sys status -> state/diag/cal boundary`')
    $lines.Add("")
    $lines.Add("## 3. Artifacts")
    $lines.Add("")
    $lines.Add(('- Raw shell witness: `{0}`' -f $RawLogPath))
    if ($DidMemoryReport) {
        $lines.Add(('- Memory report: `{0}`' -f $MemoryReportPath))
    }
    $lines.Add("")
    $lines.Add("## 4. Assertions")
    $lines.Add("")
    foreach ($check in $Checks) {
        $status = if ($check.Passed) { "PASS" } else { "FAIL" }
        $lines.Add(('- `{0}`: `{1}` ({2})' -f $check.Name, $status, $check.Detail))
    }

    [System.IO.File]::WriteAllLines($SummaryPath, $lines, $utf8NoBom)
}

$repoRoot = Resolve-RepoRootPath -Root $RepoRoot
$resolvedOutputDir = if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    Join-Path $repoRoot (Join-Path $BuildDir "output\\board_witness")
} elseif ([System.IO.Path]::IsPathRooted($OutputDir)) {
    $OutputDir
} else {
    Join-Path $repoRoot $OutputDir
}

New-Item -ItemType Directory -Force -Path $resolvedOutputDir | Out-Null

$timestamp = [DateTime]::Now.ToString("yyyyMMdd_HHmmss")
$rawLogPath = Join-Path $resolvedOutputDir ("diag_carrier_witness_{0}.log" -f $timestamp)
$summaryPath = Join-Path $resolvedOutputDir ("diag_carrier_witness_{0}.summary.md" -f $timestamp)
$memoryReportPath = Join-Path $repoRoot (Join-Path $BuildDir "output\\memory_report.txt")

$didBuildAndFlash = $false
$didMemoryReport = $false
$resolvedPortName = $PortName
$logLines = New-Object System.Collections.Generic.List[string]
$checks = New-Object System.Collections.Generic.List[object]
$errors = New-Object System.Collections.Generic.List[string]

try {
    if (-not $SkipBuildAndFlash -or -not $SkipMemoryReport) {
        $bashExe = Resolve-BashExecutable -PreferredPath $GitBashPath
        $bashRepoRoot = Convert-ToBashPath -WindowsPath $repoRoot
    }

    if (-not $SkipBuildAndFlash) {
        $buildCommand = @"
cd '$bashRepoRoot' && \
THEMIS_BUILD_DIR='$BuildDir' \
THEMIS_BUILD_TYPE='$BuildType' \
THEMIS_DEVDRV_PROFILE=full-diag \
THEMIS_PRODUCT_FLAVOR=carrier-demo \
./run.sh all
"@
        Invoke-GitBashStep -BashExecutable $bashExe -ScriptText $buildCommand -Label "run.sh all"
        $didBuildAndFlash = $true
        Start-Sleep -Milliseconds 900
    }

    if (-not $SkipMemoryReport) {
        $memoryCommand = @"
cd '$bashRepoRoot' && \
THEMIS_BUILD_DIR='$BuildDir' \
THEMIS_BUILD_TYPE='$BuildType' \
THEMIS_DEVDRV_PROFILE=full-diag \
THEMIS_PRODUCT_FLAVOR=carrier-demo \
./run.sh memory
"@
        Invoke-GitBashStep -BashExecutable $bashExe -ScriptText $memoryCommand -Label "run.sh memory"
        $didMemoryReport = $true
    }

    if ([string]::IsNullOrWhiteSpace($resolvedPortName) -or ($resolvedPortName -eq "auto")) {
        $portFinderScript = Join-Path $scriptsRoot "find_themis_shell_port.ps1"
        Write-Host "[diag_carrier_witness] probe: discover shell port"
        $resolvedPortName = & $portFinderScript `
            -ExpectedFlavor "carrier-demo" `
            -RequireShell `
            -BaudRate $BaudRate
        if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($resolvedPortName)) {
            throw "Failed to auto-discover diag shell port."
        }

        $resolvedPortName = $resolvedPortName.Trim()
        Write-Host ("[diag_carrier_witness] port: " + $resolvedPortName)
    }

    $logLines.Add((("# FalconFoc diag/cal board witness {0:o}") -f [DateTime]::Now))
    $logLines.Add(("port={0} baud={1}" -f $resolvedPortName, $BaudRate))

    $serialPort = New-Object System.IO.Ports.SerialPort $resolvedPortName, $BaudRate, "None", 8, "One"
    $serialPort.Encoding = $utf8NoBom
    $serialPort.NewLine = "`r`n"
    $serialPort.ReadTimeout = 200
    $serialPort.WriteTimeout = 1000
    $serialPort.DtrEnable = $false
    $serialPort.RtsEnable = $false

    try {
        $serialPort.Open()
        Start-Sleep -Milliseconds 250
        [void](Collect-SerialUntilQuiet -SerialPort $serialPort -TimeoutMs 800 -QuietMs 250)

        $responses = [ordered]@{}

        if (-not $SkipReboot) {
            $logLines.Add("")
            $logLines.Add("> sys reboot")
            $responses["sys reboot"] = Invoke-ShellCapture -SerialPort $serialPort -Command "sys reboot" -TimeoutMs 1200 -QuietMs 350
            $logLines.Add($responses["sys reboot"])

            Start-Sleep -Milliseconds 1300
            $responses["<post-reboot-banner>"] = Collect-SerialUntilQuiet -SerialPort $serialPort -TimeoutMs 2200 -QuietMs 700
            $logLines.Add("")
            $logLines.Add("> <post-reboot-banner>")
            $logLines.Add($responses["<post-reboot-banner>"])
        }

        $commandSpecs = @(
            @{ Command = "help"; TimeoutMs = 2400; QuietMs = 500 },
            @{ Command = "sys status"; TimeoutMs = 2400; QuietMs = 500 },
            @{ Command = "fault status"; TimeoutMs = 2400; QuietMs = 500 },
            @{ Command = "cfg ctrl vel"; TimeoutMs = 2400; QuietMs = 500 },
            @{ Command = "state start"; TimeoutMs = 2400; QuietMs = 500 },
            @{ Command = "diag pid vel kp 0.1000"; TimeoutMs = 2400; QuietMs = 500 },
            @{ Command = "diag param rs 0.2300"; TimeoutMs = 2400; QuietMs = 500 },
            @{ Command = "state stop"; TimeoutMs = 2400; QuietMs = 500 },
            @{ Command = "cal enc"; TimeoutMs = 2400; QuietMs = 500 },
            @{ Command = "cal status"; TimeoutMs = 1200; QuietMs = 250 },
            @{ Command = "cal abort"; TimeoutMs = 2400; QuietMs = 500 },
            @{ Command = "fault status post-cal"; ActualCommand = "fault status"; TimeoutMs = 2400; QuietMs = 500 },
            @{ Command = "sys status final"; ActualCommand = "sys status"; TimeoutMs = 2400; QuietMs = 500 }
        )

        foreach ($spec in $commandSpecs) {
            $label = [string]$spec.Command
            $actual = if ($spec.ContainsKey("ActualCommand")) { [string]$spec.ActualCommand } else { $label }

            $logLines.Add("")
            $logLines.Add("> $actual")
            $responses[$label] = Invoke-ShellCapture -SerialPort $serialPort `
                -Command $actual `
                -TimeoutMs ([int]$spec.TimeoutMs) `
                -QuietMs ([int]$spec.QuietMs)
            $logLines.Add($responses[$label])

            if ($actual -eq "cal enc") {
                Start-Sleep -Milliseconds 250
            }
        }

        if (-not $SkipReboot) {
            $passed = Test-ContainsAll -Text $responses["sys reboot"] -Tokens @("[sys] cmd=reboot result=ok")
            Add-CheckResult -Results $checks -Name "sys reboot" -Passed $passed -Detail "expect reboot ack"
            if (-not $passed) { $errors.Add("Missing reboot acknowledgement.") }

            $passed = Test-ContainsAll -Text $responses["<post-reboot-banner>"] -Tokens @(
                    "[shell] name=FalconFoc version=",
                    "shell=on",
                    "flavor=carrier-demo",
                    "transport=uart1-dma"
                )
            Add-CheckResult -Results $checks -Name "post-reboot banner" -Passed $passed -Detail "expect shell-on carrier banner"
            if (-not $passed) { $errors.Add("Missing shell-on carrier reboot banner.") }
        }

        $assertionSpecs = @(
            @{ Name = "help"; Response = "help"; Tokens = @("[help] count=", "name=diag") ; Detail = "expect diag command visible in shell-on image" },
            @{ Name = "sys status"; Response = "sys status"; Tokens = @("[sys] cmd=status result=ok", "shell=1", "flavor=carrier-demo", "ready=1") ; Detail = "expect live shell status" },
            @{ Name = "fault status"; Response = "fault status"; Tokens = @("[fault] cmd=status result=ok", "active=0", "name=none") ; Detail = "expect clean fault state" },
            @{ Name = "cfg ctrl vel"; Response = "cfg ctrl vel"; Tokens = @("[cfg] cmd=ctrl result=ok", "ctrl=vel") ; Detail = "expect idle-only config write accepted" },
            @{ Name = "state start"; Response = "state start"; Tokens = @("[state] cmd=start result=ok", "axis=running") ; Detail = "expect axis running" },
            @{ Name = "diag pid runtime guard"; Response = "diag pid vel kp 0.1000"; Tokens = @("[diag] cmd=pid result=error", "axis=running", "policy=idle-only") ; Detail = "expect runtime pid write rejection" },
            @{ Name = "diag param runtime guard"; Response = "diag param rs 0.2300"; Tokens = @("[diag] cmd=param result=error", "axis=running", "policy=idle-only") ; Detail = "expect runtime param write rejection" },
            @{ Name = "state stop"; Response = "state stop"; Tokens = @("[state] cmd=stop result=ok", "axis=idle") ; Detail = "expect axis idle after stop" },
            @{ Name = "cal enc start"; Response = "cal enc"; Tokens = @("[cal] cmd=start result=ok", "stage=enc") ; Detail = "expect actual calib entry" },
            @{ Name = "cal status"; Response = "cal status"; Tokens = @("[cal] cmd=status result=ok", "axis=calib", "stage=enc") ; Detail = "expect calib status witness" },
            @{ Name = "cal abort"; Response = "cal abort"; Tokens = @("[cal] cmd=abort result=ok") ; Detail = "expect calib abort closeout" },
            @{ Name = "fault status post-cal"; Response = "fault status post-cal"; Tokens = @("[fault] cmd=status result=ok", "active=0", "name=none") ; Detail = "expect no fault after calib abort" },
            @{ Name = "sys status final"; Response = "sys status final"; Tokens = @("[sys] cmd=status result=ok", "ready=1") ; Detail = "expect shell still healthy" }
        )

        foreach ($spec in $assertionSpecs) {
            $passed = Test-ContainsAll -Text $responses[[string]$spec.Response] -Tokens ([string[]]$spec.Tokens)
            Add-CheckResult -Results $checks -Name ([string]$spec.Name) -Passed $passed -Detail ([string]$spec.Detail)
            if (-not $passed) {
                $errors.Add(("Assertion failed: {0}" -f [string]$spec.Name))
            }
        }
    }
    finally {
        if ($null -ne $serialPort -and $serialPort.IsOpen) {
            $serialPort.Close()
        }
    }
}
finally {
    [System.IO.File]::WriteAllLines($rawLogPath, $logLines, $utf8NoBom)
    New-WitnessSummary -SummaryPath $summaryPath `
                       -Port $(if ([string]::IsNullOrWhiteSpace($resolvedPortName)) { "unknown" } else { $resolvedPortName }) `
                       -BuildDirectory $BuildDir `
                       -BuildConfiguration $BuildType `
                       -RawLogPath $rawLogPath `
                       -MemoryReportPath $memoryReportPath `
                       -DidBuildAndFlash $didBuildAndFlash `
                       -DidMemoryReport $didMemoryReport `
                       -Checks $checks
}

if ($errors.Count -gt 0) {
    foreach ($entry in $errors) {
        Write-Host ("[diag_carrier_witness] ERROR: " + $entry)
    }

    throw ("FalconFoc diag/cal board witness failed. See '{0}' and '{1}'." -f $rawLogPath, $summaryPath)
}

Write-Host ("[diag_carrier_witness] summary: " + $summaryPath)
Write-Host ("[diag_carrier_witness] log    : " + $rawLogPath)
