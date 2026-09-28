# FalconFoc comprehensive bench matrix runner
#
# This script is deliberately a bounded command runner, not a protection
# device. It only emits the allow-listed diagnostic commands below. The
# firmware and operator remain responsible for immediate power removal when a
# hard-stop condition appears.

param(
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [ValidateSet("Preflight", "Encoder", "Mapping", "Trigger", "Phase", "Voltage", "All")]
    [string]$Mode = "Preflight",
    [string]$PortName = "COM3",
    [int]$BaudRate = 115200,
    [ValidateRange(1, 20)][int]$PulseMs = 5,
    [ValidateRange(1, 1000)][int]$VoltagePulseMs = 400,
    [ValidateRange(20, 60)][double]$OpenLoopSpeedRpm = 30.0,
    [ValidateSet(50, 200, 400)][int]$BenchBudgetMs = 50,
    [string]$OutputRoot = "",
    [switch]$RunPulses,
    [switch]$WaitForPowerCycle
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$RepoRoot = [System.IO.Path]::GetFullPath($RepoRoot)
$benchDirectory = if ($BenchBudgetMs -eq 50) {
    "fw-full-diag-carrier-bench-debug"
} else {
    "fw-full-diag-carrier-bench-${BenchBudgetMs}ms-debug"
}
$BenchStopMarginMs = 25
$elfPath = Join-Path $RepoRoot "build\${benchDirectory}\output\falcon-foc-at32.elf"
$compileCommandsPath = Join-Path $RepoRoot "build\${benchDirectory}\compile_commands.json"
$probePath = Join-Path $RepoRoot "scripts\flows\board\run_shell_serial_probe.ps1"

if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutputRoot = Join-Path $RepoRoot "tmp\bench\${stamp}_comprehensive_root_cause"
} else {
    $OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
}

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
@(
    "# Bench operator notes",
    "",
    "- Record board ID, motor ID, PSU setting, VBUS, sound, movement, heat, and stop reason here.",
    "- Do not wait for a serial reply before cutting PSU on an anomaly.",
    "- A voltage-open-loop run uses 30 rpm mechanical speed by default; 5 rpm is not a rotation test.",
    "- Voltage mode requires the explicit 400 ms bench image and does not use the 5 ms mapping pulse."
) | Set-Content -LiteralPath (Join-Path $OutputRoot "operator-notes.md") -Encoding UTF8
"No fault context captured yet." | Set-Content -LiteralPath (Join-Path $OutputRoot "fault_context.txt") -Encoding UTF8

function Write-Log {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Text
    )

    $path = Join-Path $OutputRoot $Name
    Add-Content -LiteralPath $path -Value $Text -Encoding UTF8
}

function Assert-CommandAllowed {
    param([Parameter(Mandatory = $true)][string]$Command)

    $number = '[-+]?[0-9]+(?:\.[0-9]+)?'
    $allowed = @(
        '^sys status$',
        '^state show$',
        '^fault status$',
        '^obs all$',
        '^diag param show$',
        '^diag pid all show$',
        '^diag debug show$',
        '^diag trace clear$',
        '^diag trace show$',
        '^diag trace sample [0-9]+$',
        '^diag enc trace clear$',
        '^diag enc trace show$',
        '^diag enc trace sample [0-9]+$',
        '^diag map show$',
        '^diag map reset$',
        '^diag map pwm [0-5]$',
        '^diag map adc [0-5]$',
        "^diag set theta0 $number$",
        '^diag set enc_dir [01]$',
        '^diag set adc_trig [0-9]+$',
        "^diag set dp[0-2] $number$",
        "^diag set vq $number $number$",
        '^state start$',
        '^state disable$'
    )

    foreach ($pattern in $allowed) {
        if ($Command -match $pattern) {
            return
        }
    }

    throw "Command is outside the bench allowlist: $Command"
}

function Read-UntilQuiet {
    param([int]$QuietMs = 100, [int]$MaxWaitMs = 1200)

    $sb = New-Object System.Text.StringBuilder
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $lastRx = 0L
    $receivedAny = $false

    while ($sw.ElapsedMilliseconds -lt $MaxWaitMs) {
        if ($script:BenchSerial.BytesToRead -gt 0) {
            $count = $script:BenchSerial.BytesToRead
            $buffer = New-Object byte[] $count
            $read = $script:BenchSerial.Read($buffer, 0, $count)
            if ($read -gt 0) {
                [void]$sb.Append([System.Text.Encoding]::ASCII.GetString($buffer, 0, $read))
                $lastRx = $sw.ElapsedMilliseconds
                $receivedAny = $true
            }
        }

        if ($receivedAny -and (($sw.ElapsedMilliseconds - $lastRx) -gt $QuietMs)) {
            break
        }
        Start-Sleep -Milliseconds 5
    }

    return $sb.ToString()
}

function Open-BenchSerial {
    $script:BenchSerial = New-Object System.IO.Ports.SerialPort $PortName, $BaudRate, "None", 8, "One"
    $script:BenchSerial.ReadTimeout = 50
    $script:BenchSerial.WriteTimeout = 500
    $script:BenchSerial.DtrEnable = $false
    $script:BenchSerial.RtsEnable = $false
    $script:BenchSerial.Open()
    Start-Sleep -Milliseconds 300
    if ($script:BenchSerial.BytesToRead -gt 0) {
        [void]$script:BenchSerial.ReadExisting()
    }
}

function Invoke-BenchPowerCycleGate {
    param([Parameter(Mandatory = $true)][string]$NextCase)

    if (-not $WaitForPowerCycle) {
        throw "The previous pulse ended in the required FAULT-safe state. Power-cycle the board before '$NextCase' or rerun with -WaitForPowerCycle. No next case was sent."
    }

    Close-BenchSerial
    $prompt = "Power-cycle the 12 V supply and reset the board. Confirm the bench is idle, then press Enter to continue with '$NextCase'"
    [void](Read-Host $prompt)
    Open-BenchSerial
    $reply = Save-CommandResult -LogName "preflight.log" -Command "state show"
    if ($reply -notmatch 'axis=idle') {
        throw "The board did not return to axis=idle after the requested power cycle; refusing '$NextCase'."
    }
}

function Close-BenchSerial {
    if (($null -ne $script:BenchSerial) -and $script:BenchSerial.IsOpen) {
        $script:BenchSerial.Close()
    }
}

function Send-BenchCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Command,
        [int]$WaitMs = 120,
        [int]$QuietMs = 80,
        [int]$MaxWaitMs = 1200
    )

    Assert-CommandAllowed $Command
    $script:BenchSerial.Write($Command + [string][char]13 + [string][char]10)
    Start-Sleep -Milliseconds $WaitMs
    $reply = Read-UntilQuiet -QuietMs $QuietMs -MaxWaitMs $MaxWaitMs
    if ([string]::IsNullOrWhiteSpace($reply)) {
        throw "Serial timeout or empty reply after: $Command"
    }
    return $reply
}

function Save-CommandResult {
    param(
        [Parameter(Mandatory = $true)][string]$LogName,
        [Parameter(Mandatory = $true)][string]$Command,
        [int]$WaitMs = 120
    )

    $reply = Send-BenchCommand -Command $Command -WaitMs $WaitMs
    Write-Log -Name $LogName -Text ("--- TX: {0} ---`r`n{1}" -f $Command, $reply)
    return $reply
}

function Invoke-BenchPulse {
    param(
        [Parameter(Mandatory = $true)][string]$LogName,
        [Parameter(Mandatory = $true)][string]$SetupCommand,
        [int]$DurationMs = 0
    )

    $runDurationMs = if ($DurationMs -gt 0) { $DurationMs } else { $PulseMs }
    $effectiveDurationMs = [Math]::Min($runDurationMs, ($BenchBudgetMs - $BenchStopMarginMs))
    if ($effectiveDurationMs -le 0) {
        throw "Bench stop margin leaves no runnable duration for budget $BenchBudgetMs ms."
    }
    $started = $false
    try {
        $setupReply = Save-CommandResult -LogName $LogName -Command $SetupCommand
        if ($setupReply -notmatch 'result=ok') {
            throw "Bench setup was not accepted: $SetupCommand"
        }

        Assert-CommandAllowed "state start"
        $script:BenchSerial.Write("state start" + [string][char]13 + [string][char]10)
        $started = $true
        Start-Sleep -Milliseconds $effectiveDurationMs

        Assert-CommandAllowed "state disable"
        $script:BenchSerial.Write("state disable" + [string][char]13 + [string][char]10)
        $started = $false
        Start-Sleep -Milliseconds 250
        $reply = Read-UntilQuiet -QuietMs 100 -MaxWaitMs 1500
        if ([string]::IsNullOrWhiteSpace($reply)) {
            throw "Serial timeout after state disable in $LogName"
        }
    Write-Log -Name $LogName -Text ("--- bench timing requested_ms={0} effective_ms={1} budget_ms={2} stop_margin_ms={3} ---" -f $runDurationMs, $effectiveDurationMs, $BenchBudgetMs, $BenchStopMarginMs)
    Write-Log -Name $LogName -Text ("--- TX: state start / state disable ---`r`n{0}" -f $reply)

        [void](Save-CommandResult -LogName $LogName -Command "state show")
        [void](Save-CommandResult -LogName $LogName -Command "fault status")
        [void](Save-CommandResult -LogName $LogName -Command "obs all")
        [void](Save-CommandResult -LogName $LogName -Command "diag trace show")
        [void](Save-CommandResult -LogName $LogName -Command "diag enc trace show")
    } finally {
        if ($started -and ($null -ne $script:BenchSerial) -and $script:BenchSerial.IsOpen) {
            try {
                $script:BenchSerial.Write("state disable" + [string][char]13 + [string][char]10)
            } catch {
                # A lost serial link is a hard failure; do not continue the matrix.
            }
        }
    }
}

function Invoke-Preflight {
    if (-not (Test-Path -LiteralPath $elfPath)) {
        throw "Bench ELF not found: $elfPath"
    }
    if (-not (Test-Path -LiteralPath $probePath)) {
        throw "Serial probe not found: $probePath"
    }

    $hash = (Get-FileHash -LiteralPath $elfPath -Algorithm SHA256).Hash
    Write-Log -Name "preflight.log" -Text ("repo={0}`r`nelf={1}`r`nsha256={2}`r`nmode={3}`r`nport={4}@{5}`r`npulse_ms={6}`r`nvoltage_pulse_ms={7}`r`nopen_loop_speed_rpm={8}`r`nbench_budget_ms={9}" -f `
        $RepoRoot, $elfPath, $hash, $Mode, $PortName, $BaudRate, $PulseMs, $VoltagePulseMs, $OpenLoopSpeedRpm, $BenchBudgetMs)

    if (($Mode -eq "Voltage" -or $Mode -eq "All") -and ($BenchBudgetMs -lt 400)) {
        throw "Voltage-open-loop mode requires -BenchBudgetMs 400 and the dedicated 400 ms bench image."
    }
    if (($Mode -eq "Voltage" -or $Mode -eq "All") -and ($VoltagePulseMs -gt $BenchBudgetMs)) {
        throw "Voltage pulse duration ($VoltagePulseMs ms) exceeds the selected bench budget ($BenchBudgetMs ms)."
    }
    if (-not (Test-Path -LiteralPath $compileCommandsPath)) {
        throw "Bench compile_commands.json not found: $compileCommandsPath"
    }
    $compileText = Get-Content -LiteralPath $compileCommandsPath -Raw
    $requiredDefinitions = @(
        'TM_BENCH_DIAG=1',
        'TM_FEATURE_VOFA=0',
        'FOC_ENERGIZED_CALIB_ENABLE=0',
        "TM_MOTOR_BENCH_RUN_BUDGET_MS=$BenchBudgetMs"
    )
    foreach ($definition in $requiredDefinitions) {
        if ($compileText -notmatch [regex]::Escape($definition)) {
            throw "Required bench compile definition is missing: $definition"
        }
    }
    $definitions = Select-String -LiteralPath $compileCommandsPath -Pattern "TM_BENCH_DIAG=1|TM_FEATURE_VOFA=0|FOC_ENERGIZED_CALIB_ENABLE=0|TM_MOTOR_BENCH_RUN_BUDGET_MS=$BenchBudgetMs"
    Write-Log -Name "preflight.log" -Text ("compile_definitions:`r`n{0}" -f (($definitions | Out-String).TrimEnd()))

    $probeCommands = @(
        "sys status",
        "state show",
        "fault status",
        "obs all",
        "diag param show",
        "diag pid all show",
        "diag debug show",
        "diag map show",
        "diag enc trace show"
    )
    $probeText = New-Object System.Text.StringBuilder
    foreach ($probeCommand in $probeCommands) {
        $singleProbeOutput = & powershell -NoProfile -ExecutionPolicy Bypass -File $probePath `
            -PortName $PortName -BaudRate $BaudRate -Commands $probeCommand -QuietMs 120 -MaxWaitMs 1500 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) {
            throw "Read-only serial probe failed after '$probeCommand'; do not start PWM tests."
        }
        [void]$probeText.Append($singleProbeOutput)
    }
    $probeOutput = $probeText.ToString()
    $probeOutput | Set-Content -LiteralPath (Join-Path $OutputRoot "baseline_current_image.log") -Encoding UTF8
    $requiredBenchReplies = @(
        'cmd=map action=show result=ok',
        'cmd=enc_trace action=show result=ok'
    )
    foreach ($marker in $requiredBenchReplies) {
        if ($probeOutput -notmatch [regex]::Escape($marker)) {
            throw "The connected board did not expose the expected bench command marker '$marker'; refusing PWM tests."
        }
    }
}

function Invoke-EncoderCapture {
    Save-CommandResult -LogName "encoder_forward.log" -Command "diag enc trace clear" | Out-Null
    for ($index = 0; $index -lt 32; $index++) {
        Save-CommandResult -LogName "encoder_forward.log" -Command "obs all" -WaitMs 40 | Out-Null
        Start-Sleep -Milliseconds 40
    }
    Save-CommandResult -LogName "encoder_forward.log" -Command "diag enc trace show" | Out-Null
    Save-CommandResult -LogName "encoder_forward.log" -Command "diag enc trace sample 0" | Out-Null

    Save-CommandResult -LogName "encoder_reverse.log" -Command "diag enc trace clear" | Out-Null
    for ($index = 0; $index -lt 32; $index++) {
        Save-CommandResult -LogName "encoder_reverse.log" -Command "obs all" -WaitMs 40 | Out-Null
        Start-Sleep -Milliseconds 40
    }
    Save-CommandResult -LogName "encoder_reverse.log" -Command "diag enc trace show" | Out-Null
    Save-CommandResult -LogName "encoder_reverse.log" -Command "diag enc trace sample 0" | Out-Null
}

function Invoke-MappingMatrix {
    for ($pwm = 0; $pwm -lt 6; $pwm++) {
        for ($adc = 0; $adc -lt 6; $adc++) {
            $log = "pwm_map_{0}_adc_map_{1}.log" -f $pwm, $adc
            Save-CommandResult -LogName $log -Command "diag map reset" | Out-Null
            Save-CommandResult -LogName $log -Command ("diag map pwm {0}" -f $pwm) | Out-Null
            Save-CommandResult -LogName $log -Command ("diag map adc {0}" -f $adc) | Out-Null
            Save-CommandResult -LogName $log -Command "diag set adc_trig 2700" | Out-Null
            Invoke-BenchPulse -LogName $log -SetupCommand "diag set dp0 0.05"
            if (($pwm -ne 5) -or ($adc -ne 5)) {
                $nextPwm = if ($adc -eq 5) { $pwm + 1 } else { $pwm }
                $nextAdc = ($adc + 1) % 6
                Invoke-BenchPowerCycleGate -NextCase ("pwm={0},adc={1}" -f $nextPwm, $nextAdc)
            }
        }
    }
}

function Invoke-TriggerMatrix {
    $points = @(512, 1024, 1536, 1874, 2300, 2812, 3200, 3500, 3740)
    foreach ($point in $points) {
        $log = "adc_trigger_{0}.log" -f $point
        Save-CommandResult -LogName $log -Command "diag map reset" | Out-Null
        Save-CommandResult -LogName $log -Command ("diag set adc_trig {0}" -f $point) | Out-Null
        Invoke-BenchPulse -LogName $log -SetupCommand "diag set dp0 0.05"
        if ($point -ne 3740) {
            Invoke-BenchPowerCycleGate -NextCase ("adc_trig={0}" -f $point)
        }
    }
}

function Invoke-PhaseMatrix {
    # The shell decimal parser accepts at most six fractional digits.
    $offsets = @("-3.141592", "-2.356194", "-1.570796", "-0.785398", "0.000000", "0.785398", "1.570796", "2.356194", "3.141592")
    foreach ($offset in $offsets) {
        $safeName = $offset.Replace('-', 'minus').Replace('.', '_')
        $log = "theta0_{0}.log" -f $safeName
        $thetaReply = Save-CommandResult -LogName $log -Command ("diag set theta0 {0}" -f $offset)
        if ($thetaReply -notmatch 'result=ok') {
            throw "Phase seed setup was not accepted: theta0=$offset"
        }
        Invoke-BenchPulse -LogName $log -SetupCommand "diag set vq 0.05 0.00"
        if ($offset -ne "3.141592") {
            Invoke-BenchPowerCycleGate -NextCase ("theta0={0}" -f $offset)
        }
    }
}

function Invoke-VoltageGradient {
    $voltages = @("0.25", "0.50", "0.75", "1.00")
    $we = (2.0 * [math]::PI * 7.0 * $OpenLoopSpeedRpm / 60.0)
    $weText = $we.ToString("0.####", [System.Globalization.CultureInfo]::InvariantCulture)
    foreach ($voltage in $voltages) {
        $log = "voltage_{0}.log" -f $voltage.Replace('.', '_')
        Save-CommandResult -LogName $log -Command "diag set theta0 0.0000" | Out-Null
        Invoke-BenchPulse -LogName $log -SetupCommand ("diag set vq {0} {1}" -f $voltage, $weText) -DurationMs $VoltagePulseMs
        if ($voltage -ne "1.00") {
            Invoke-BenchPowerCycleGate -NextCase ("vq={0}" -f $voltage)
        }
    }
}

try {
    Invoke-Preflight

    if ($Mode -ne "Preflight" -and -not $RunPulses) {
        throw "Mode '$Mode' requires explicit -RunPulses; preflight completed without energizing the axis."
    }

    if ($Mode -ne "Preflight") {
        Open-BenchSerial
        try {
            switch ($Mode) {
                "Encoder" { Invoke-EncoderCapture }
                "Mapping" { Invoke-MappingMatrix }
                "Trigger" { Invoke-TriggerMatrix }
                "Phase"   { Invoke-PhaseMatrix }
                "Voltage" { Invoke-VoltageGradient }
                "All"     {
                    Invoke-EncoderCapture
                    Invoke-MappingMatrix
                    Invoke-BenchPowerCycleGate -NextCase "adc-trigger-matrix"
                    Invoke-TriggerMatrix
                    Invoke-BenchPowerCycleGate -NextCase "theta0-matrix"
                    Invoke-PhaseMatrix
                    Invoke-BenchPowerCycleGate -NextCase "voltage-gradient"
                    Invoke-VoltageGradient
                }
            }
        } finally {
            Close-BenchSerial
        }
    }

    @(
        "# Bench matrix result",
        "",
        "- script_mode: $Mode",
        "- elf: $elfPath",
        "- sha256: $((Get-FileHash -LiteralPath $elfPath -Algorithm SHA256).Hash)",
        "- bench_budget_ms: $BenchBudgetMs",
        "- voltage_pulse_ms: $VoltagePulseMs",
        "- open_loop_speed_rpm: $OpenLoopSpeedRpm",
        "- status: OPEN_LOOP_SOFTWARE_PATH=PASS",
        "- physical_status: MOTOR_PHYSICAL_OPERATION=UNVERIFIED",
        "- root_cause_status: OPEN_LOOP_ROOT_CAUSE_STATUS=UNRESOLVED",
        "- real_bench_status: REAL_BENCH_TEST_STATUS=BLOCKED",
        "",
        "Host/build/serial command results are evidence only; they do not prove PWM waveforms, thermal stability, or product-level motor operation."
    ) | Set-Content -LiteralPath (Join-Path $OutputRoot "result.md") -Encoding UTF8
    Write-Output "BENCH_MATRIX_STATUS=COMPLETE"
    Write-Output "BENCH_MATRIX_LOG_DIR=$OutputRoot"
} catch {
    Write-Log -Name "result.md" -Text ("# Bench matrix aborted`r`n`r`n- reason: {0}`r`n- status: REAL_BENCH_TEST_STATUS=BLOCKED" -f $_.Exception.Message)
    Write-Error $_.Exception.Message
    exit 1
}
