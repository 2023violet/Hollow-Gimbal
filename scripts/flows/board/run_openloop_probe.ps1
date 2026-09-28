# FalconFoc open-loop root-cause probe (T1/T2/T3)
#
# Companion to run_bench_matrix.ps1 for the 2026-08-30 open-loop extension plan
# (docs/planning/Themis_开环根因延申验证与修复计划.md). It stays a bounded
# command runner: allow-listed shell commands only, one pulse per power cycle,
# operator cuts the 12 V supply on any anomaly. It is not a protection device.
#
# Modes:
#   TriggerScan  - static vq=1.00 V, we=0, fine CH4 compare sweep at two theta0
#                  seeds with distinct CCR orderings (T1: window geometry +
#                  rank/phase pairing discrimination).
#   VILinearity  - static pulses at fixed theta0/trig across vq steps (T2:
#                  V-I slope vs 1/Rs, k factor).
#   ThetaSweep   - static vq=1.50 V, we=0 across theta0 seeds, measuring the
#                  encoder response to locate the rotor pull-in direction
#                  and offset (T3).
#   SpeedStep    - I-f launch then bidirectional 30/60/90 rpm VEL steps with
#                  serial/bus acceptance evidence (bench30s only).
#
# Every case requires exactly one operator power cycle between pulses because
# `state disable` intentionally lands in the FAULT-safe state and this script
# never sends `fault clear` or `state stop`.

param(
    [ValidateSet("TriggerScan", "VILinearity", "ThetaSweep", "SnapTest", "IfDrag", "Align", "VelClose", "VoltDrag", "PosClose", "Handoff", "TorqueSweep", "AdcBaseline", "AdcProof", "SpeedStep", "AutonomousDiag")]
    [string]$Mode = "TriggerScan",
    [string]$PortName = "COM3",
    [int]$BaudRate = 115200,
    [string]$RepoRoot = "",
    [string]$OutputRoot = "",
    [double]$MaxPeakAmps = 0.75,
    [double]$SweepVq = 3.00,
    [int]$SweepMs = 2000,
    [int]$SweepPwmMap = 0,
    [switch]$RunPulses,
    [switch]$AutoGate,
    [int]$GateTimeoutSec = 300,
    [int]$InitialDelaySec = 20,
    [ValidateSet("swd", "power")]
    [string]$ResetMode = "swd",
    [string]$ElfPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = (git rev-parse --show-toplevel)
}
$RepoRoot = [System.IO.Path]::GetFullPath($RepoRoot)

$OpenOcdBin = "C:/Program Files (x86)/OpenOCD_V2.0.6/bin/openocd.exe"
if (-not (Test-Path -LiteralPath $OpenOcdBin)) {
    $OpenOcdBin = "openocd"
}

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    if (($Mode -eq "SpeedStep") -or ($Mode -eq "AutonomousDiag")) {
        $OutputRoot = Join-Path $RepoRoot "tmp\bench\${stamp}_adc_then_speed"
    } else {
        $OutputRoot = Join-Path $RepoRoot "tmp\bench\${stamp}_openloop_probe_${Mode}"
    }
} else {
    $OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
}
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$script:SpeedStepPeakAbortAmps = 1.4
$script:BenchAdcTriggerDefault = 3740
$script:AutonomousDiag = ($Mode -eq "AutonomousDiag")
if ($script:AutonomousDiag -and ($ResetMode -ne "swd")) {
    throw "AutonomousDiag does not permit ResetMode=power"
}

@(
    "# Open-loop probe operator notes",
    "",
    "- One pulse per power cycle; the script prompts before every pulse after the first.",
    "- Cut the 12 V supply immediately on anomaly; do not wait for a serial reply.",
    "- Hard software envelope: any per-phase trace peak beyond $MaxPeakAmps A aborts the mode.",
    "- Normal stop is `state disable`; the axis lands in the FAULT-safe state by design.",
    "- Record motor motion, sound, heat, PSU current and stop reason per case below."
) | Set-Content -LiteralPath (Join-Path $OutputRoot "operator-notes.md") -Encoding UTF8

if ($script:AutonomousDiag) {
    @(
        "# Autonomous bench notes",
        "",
        "- physical_power_cycle=false",
        "- psu_telemetry=unavailable",
        "- This mode uses SWD reset, firmware protection, and the bench watchdog only.",
        "- No user prompt, fault clear, or manual meter entry is part of the result contract."
    ) | Set-Content -LiteralPath (Join-Path $OutputRoot "operator-notes.md") -Encoding UTF8
}

function Write-Log {
    param([string]$Name, [string]$Text)
    Add-Content -LiteralPath (Join-Path $OutputRoot $Name) -Value $Text -Encoding UTF8
}

function Assert-CommandAllowed {
    param([Parameter(Mandatory = $true)][string]$Command)

    $number = '[-+]?[0-9]+(?:\.[0-9]+)?'
    $allowed = @(
        '^sys status$',
        '^state show$',
        '^fault status$',
        '^obs all$',
        '^obs enc$',
        '^diag adc show$',
        '^diag adc zero$',
        '^diag bench keepalive$',
        '^diag bench status$',
        '^diag map reset$',
        '^diag map (pwm|adc) [0-5]$',
        '^diag trace clear$',
        '^diag trace show$',
        '^diag trace sample [0-9]+$',
        '^diag enc trace clear$',
        '^diag enc trace show$',
        '^diag enc trace sample [0-9]+$',
        '^cfg ctrl pos$',
        '^cfg ctrl vel$',
        '^diag set pos -?[0-9]+(?:\.[0-9]+)?$',
        '^diag param encoff -?[0-9]+(?:\.[0-9]+)?$',
        '^diag set vel -?[0-9]+(?:\.[0-9]+)?$'
        "^diag set iq $number $number$",
        '^cfg ctrl curr$',
        '^cfg input torque-ramp$',
        '^diag if2vel -?[0-9]+(?:\.[0-9]+)?$'
        '^diag velstep -?[0-9]+(?:\.[0-9]+)?$'
        '^diag map show$',
        "^diag set theta0 $number$",
        '^diag set adc_trig [0-9]+$',
        '^diag set enc_dir [01]$',
        '^diag enc setabs [01] [-+]?[0-9]+(?:\.[0-9]+)?$',
        "^diag set dp[0-2] $number$",
        "^diag set vq $number $number$",
        "^diag set if $number $number$",
        '^state start$',
        '^state disable$'
    )
    foreach ($pattern in $allowed) {
        if ($Command -match $pattern) { return }
    }
    throw "Command is outside the probe allowlist: $Command"
}

function Read-UntilQuiet {
    param([int]$QuietMs = 100, [int]$MaxWaitMs = 1500)

    $sb = New-Object System.Text.StringBuilder
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $lastRx = 0L
    $receivedAny = $false
    while ($sw.ElapsedMilliseconds -lt $MaxWaitMs) {
        if ($script:ProbeSerial.BytesToRead -gt 0) {
            $count = $script:ProbeSerial.BytesToRead
            $buffer = New-Object byte[] $count
            $read = $script:ProbeSerial.Read($buffer, 0, $count)
            if ($read -gt 0) {
                [void]$sb.Append([System.Text.Encoding]::ASCII.GetString($buffer, 0, $read))
                $lastRx = $sw.ElapsedMilliseconds
                $receivedAny = $true
            }
        }
        if ($receivedAny -and (($sw.ElapsedMilliseconds - $lastRx) -gt $QuietMs)) { break }
        Start-Sleep -Milliseconds 5
    }
    return $sb.ToString()
}

function Open-ProbeSerial {
    $script:ProbeSerial = New-Object System.IO.Ports.SerialPort $PortName, $BaudRate, "None", 8, "One"
    $script:ProbeSerial.ReadTimeout = 50
    $script:ProbeSerial.WriteTimeout = 500
    $script:ProbeSerial.DtrEnable = $false
    $script:ProbeSerial.RtsEnable = $false
    $script:ProbeSerial.Open()
    Start-Sleep -Milliseconds 300
    if ($script:ProbeSerial.BytesToRead -gt 0) { [void]$script:ProbeSerial.ReadExisting() }
}

function Close-ProbeSerial {
    if (($null -ne $script:ProbeSerial) -and $script:ProbeSerial.IsOpen) {
        $script:ProbeSerial.Close()
    }
}

function Send-ProbeCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Command,
        [int]$WaitMs = 120,
        [int]$QuietMs = 80,
        [int]$MaxWaitMs = 1500
    )

    Assert-CommandAllowed $Command
    $script:ProbeSerial.Write($Command + [string][char]13 + [string][char]10)
    Start-Sleep -Milliseconds $WaitMs
    $reply = Read-UntilQuiet -QuietMs $QuietMs -MaxWaitMs $MaxWaitMs
    if ([string]::IsNullOrWhiteSpace($reply)) {
        throw "Serial timeout or empty reply after: $Command"
    }
    return $reply
}

function Save-CommandResult {
    param(
        [string]$LogName,
        [Parameter(Mandatory = $true)][string]$Command,
        [int]$WaitMs = 120,
        [int]$MaxWaitMs = 1500
    )
    $reply = Send-ProbeCommand -Command $Command -WaitMs $WaitMs -MaxWaitMs $MaxWaitMs
    Write-Log -Name $LogName -Text ("--- TX: {0} ---`r`n{1}" -f $Command, $reply)
    return $reply
}

function Assert-Idle {
    $reply = Save-CommandResult -LogName "preflight.log" -Command "state show"
    if ($reply -notmatch 'axis=idle') {
        throw "Board is not idle (power-cycle required); refusing to continue."
    }
    $fault = Save-CommandResult -LogName "preflight.log" -Command "fault status"
    if ($fault -match 'active=([^0]|0[1-9])') {
        throw "Active fault bits present before the pulse; refusing to continue."
    }
}

function Invoke-ProbePulse {
    param(
        [Parameter(Mandatory = $true)][string]$LogName,
        [Parameter(Mandatory = $true)][string]$SetupCommand,
        [int]$DurationMs = 100
    )

    Assert-Idle
    $setupReply = Save-CommandResult -LogName $LogName -Command $SetupCommand
    if ($setupReply -notmatch 'result=ok') {
        throw "Probe setup was not accepted: $SetupCommand"
    }
    [void](Save-CommandResult -LogName $LogName -Command "diag trace clear")

    $started = $false
    try {
        Assert-CommandAllowed "state start"
        $script:PulseStart = [DateTime]::UtcNow
        $script:ProbeSerial.Write("state start" + [string][char]13 + [string][char]10)
        $started = $true
        # A short proof pulse must genuinely remain enabled for its requested
        # duration.  The former DurationMs - 400 deadline skipped the loop for
        # 100 ms pulses and sent disable immediately.
        if ($DurationMs -le 400) {
            Start-Sleep -Milliseconds $DurationMs
        } else {
            # Poll long pulses while reserving enough time to finish at the
            # exact requested deadline rather than extending the energy window.
            $deadline = [DateTime]::UtcNow.AddMilliseconds($DurationMs)
            $pollIndex = 0
            while ([DateTime]::UtcNow -lt $deadline) {
                $remainingMs = [int][Math]::Ceiling($deadline.Subtract([DateTime]::UtcNow).TotalMilliseconds)
                if ($remainingMs -le 400) {
                    Start-Sleep -Milliseconds $remainingMs
                    break
                }
                Start-Sleep -Milliseconds ([Math]::Min(700, $remainingMs - 300))
                if (-not $started) { break }
                $pollIndex++
                try {
                    # The bench heartbeat watchdog expires 1.5 s into a RUNNING
                    # axis; every poll cycle must also feed it or long pulses
                    # die at ~1.5 s with fault=comm-error (bench-30s image).
                    Send-BenchKeepAlive -LogName $LogName -NoWait
                    $script:ProbeSerial.Write("state show" + [string][char]13 + [string][char]10)
                    Start-Sleep -Milliseconds 120
                    $pollReply = Read-UntilQuiet -QuietMs 60 -MaxWaitMs 600
                    $pos = if ($pollReply -match 'pos_turn=(-?[0-9.]+)') { $Matches[1] } else { '?' }
                    $vel = if ($pollReply -match 'vel_rpm=(-?[0-9.]+)') { $Matches[1] } else { '?' }
                    Write-Output ("POLL {0} t~{1}s pos={2} vel_rpm={3}" -f $pollIndex, [int](([DateTime]::UtcNow - $script:PulseStart).TotalMilliseconds / 100), $pos, $vel)
                    Write-Log -Name $LogName -Text ("--- poll {0}: pos_turn={1} vel_rpm={2}" -f $pollIndex, $pos, $vel)
                } catch { }
            }
        }

        Assert-CommandAllowed "state disable"
        $script:ProbeSerial.Write("state disable" + [string][char]13 + [string][char]10)
        $started = $false
        Start-Sleep -Milliseconds 250
        $reply = Read-UntilQuiet -QuietMs 100 -MaxWaitMs 1500
        Write-Log -Name $LogName -Text ("--- pulse {0} ms, requested setup: {1} ---`r`n{2}" -f $DurationMs, $SetupCommand, $reply)

        [void](Save-CommandResult -LogName $LogName -Command "state show")
        [void](Save-CommandResult -LogName $LogName -Command "fault status")
        [void](Save-CommandResult -LogName $LogName -Command "obs all")
        [void](Save-CommandResult -LogName $LogName -Command "diag trace show")
        [void](Save-CommandResult -LogName $LogName -Command "diag trace sample 0")
        [void](Save-CommandResult -LogName $LogName -Command "diag trace sample 1")

        $trace = Get-Content -LiteralPath (Join-Path $OutputRoot $LogName) -Raw
        $peakMatch = [regex]::Match($trace, 'peak=([-0-9./]+)')
        if ($peakMatch.Success) {
            $peaks = $peakMatch.Groups[1].Value -split '/'
            foreach ($p in $peaks) {
                if ([Math]::Abs([double]$p) -gt $MaxPeakAmps) {
                    throw ("Per-phase software peak {0} A exceeded the {1} A envelope; mode aborted." -f $p, $MaxPeakAmps)
                }
            }
        }
    } finally {
        if ($started -and ($null -ne $script:ProbeSerial) -and $script:ProbeSerial.IsOpen) {
            try {
                $script:ProbeSerial.Write("state disable" + [string][char]13 + [string][char]10)
            } catch {
                # A lost serial link is a hard failure; the operator cuts the supply.
            }
        }
    }
}

function Invoke-SwdResetGate {
    param([Parameter(Mandatory = $true)][string]$NextCase)

    # No-operator gate: reset the MCU over SWD. A fresh boot restores the
    # boot-time offset calibration and the bench run budget, so no physical
    # power cycle is required between benign static cases. The operator still
    # owns the hard stop: cut the 12 V supply on any anomaly.
    Write-Output ("OPENLOOP_PROBE_SWD_RESET next={0}" -f $NextCase)
    Close-ProbeSerial
    $resetArgs = @(
        "-f", (Join-Path $RepoRoot "tools\openocd\openocd_daplink.cfg"),
        "-c", "init; reset run; shutdown"
    )
    # OpenOCD writes its banner to stderr; with $ErrorActionPreference="Stop"
    # PowerShell 5.1 turns that into a terminating error. Relax EAP locally.
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $resetLog = & $OpenOcdBin @resetArgs 2>&1 | Out-String
    } finally {
        $ErrorActionPreference = $prevEap
    }
    Write-Log -Name "swd_reset.log" -Text ("--- reset for {0} ---`r`n{1}" -f $NextCase, $resetLog)
    $deadline = [DateTime]::UtcNow.AddSeconds(40)
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 1500
        try {
            Open-ProbeSerial
        } catch {
            continue
        }
        try {
            $script:ProbeSerial.Write("state show" + [string][char]13 + [string][char]10)
            Start-Sleep -Milliseconds 300
            $reply = Read-UntilQuiet -QuietMs 120 -MaxWaitMs 1200
            if ($reply -match 'axis=idle') {
                Assert-Idle
                return
            }
        } catch {
            # Boot window: shell not up yet; keep polling.
        }
        Close-ProbeSerial
    }
    throw "SWD reset gate timed out before '$NextCase'."
}

function Invoke-PowerCycleGate {
    param([Parameter(Mandatory = $true)][string]$NextCase)

    if (-not $AutoGate) {
        Close-ProbeSerial
        [void](Read-Host "Power-cycle the 12 V supply, confirm the bench is safe, then press Enter to continue with '$NextCase'")
        Open-ProbeSerial
        Assert-Idle
        return
    }

    # Non-interactive gate: ask the operator to power-cycle, then poll the
    # console until the board reports axis=idle again. The operator remains in
    # control of the supply; the script only resumes on evidence.
    Write-Output ("OPENLOOP_PROBE_POWER_CYCLE_REQUIRED next={0}" -f $NextCase)
    Close-ProbeSerial
    Start-Sleep -Milliseconds 1500
    $deadline = [DateTime]::UtcNow.AddSeconds($GateTimeoutSec)
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 2000
        try {
            Open-ProbeSerial
        } catch {
            Start-Sleep -Milliseconds 1000
            continue
        }
        $reply = Read-UntilQuiet -QuietMs 120 -MaxWaitMs 1200
        $stateReply = $null
        if ($reply -match 'axis=(\w+)') {
            $stateReply = $Matches[1]
        } else {
            try {
                $script:ProbeSerial.Write("state show" + [string][char]13 + [string][char]10)
                Start-Sleep -Milliseconds 250
                $stateReplyText = Read-UntilQuiet -QuietMs 120 -MaxWaitMs 1200
                if ($stateReplyText -match 'axis=(\w+)') { $stateReply = $Matches[1] }
            } catch { $stateReply = $null }
        }
        if ($stateReply -eq 'idle') {
            Write-Output ("OPENLOOP_PROBE_POWER_CYCLE_DONE next={0}" -f $NextCase)
            Assert-Idle
            return
        }
        Close-ProbeSerial
    }
    throw "Power-cycle gate timed out after ${GateTimeoutSec}s before '$NextCase'."
}

function Resolve-IdleForNextCase {
    param([Parameter(Mandatory = $true)][string]$NextCase)

    # Self-healing: if the board is already idle (fresh boot or first case)
    # proceed directly; otherwise require a power cycle before continuing.
    try {
        $reply = Save-CommandResult -LogName "preflight.log" -Command "state show"
        if ($reply -match 'axis=idle') { return }
    } catch {
        Write-Output ("OPENLOOP_PROBE_SERIAL_UNAVAILABLE falling back to gate for {0}" -f $NextCase)
    }
    if ($ResetMode -eq "swd") {
        Invoke-SwdResetGate -NextCase $NextCase
    } else {
        Invoke-PowerCycleGate -NextCase $NextCase
    }
}

function Invoke-SnapTest {
    # Six fixed DC vectors (dp0/dp1/dp2, each +/-) at visible torque: the rotor
    # should snap towards six distinct positions about 60 deg electrical apart.
    # This verifies the power path is real and records the rotor response and
    # direction per axis for the pairing analysis (plan P1/P5).
    $cases = @(
        @{ axis = 0; value = "1.50" },
        @{ axis = 1; value = "1.50" },
        @{ axis = 2; value = "1.50" },
        @{ axis = 0; value = "-1.50" },
        @{ axis = 1; value = "-1.50" },
        @{ axis = 2; value = "-1.50" }
    )
    foreach ($c in $cases) {
        $log = "snap_dp{0}_{1}.log" -f $c["axis"], $c["value"].Replace('-', 'minus').Replace('.', '_')
        Resolve-IdleForNextCase -NextCase ("dp{0}={1}" -f $c["axis"], $c["value"])
        [void](Save-CommandResult -LogName $log -Command ("diag set adc_trig {0}" -f $script:BenchAdcTriggerDefault))
        Invoke-ProbePulse -LogName $log -SetupCommand ("diag set dp{0} {1}" -f $c["axis"], $c["value"]) -DurationMs 200
    }
}

function Invoke-Align {
    # Encoder electrical-zero calibration by d-axis alignment: a strong fixed
    # dp vector pulls the rotor d-axis onto the field angle; the encoder
    # electrical angle read at alignment IS the zero offset error.
    $cases = @(
        @{ axis = 0; value = "3.20" },
        @{ axis = 1; value = "3.20" },
        @{ axis = 2; value = "3.20" }
    )
    foreach ($c in $cases) {
        $log = "align_dp{0}_{1}.log" -f $c["axis"], $c["value"].Replace('-', 'minus').Replace('.', '_')
        Resolve-IdleForNextCase -NextCase ("dp{0}={1}" -f $c["axis"], $c["value"])
        [void](Save-CommandResult -LogName $log -Command ("diag set adc_trig {0}" -f $script:BenchAdcTriggerDefault))
        [void](Save-CommandResult -LogName $log -Command "diag set enc_dir 0")
        Assert-CommandAllowed "state start"
        $setupReply = Save-CommandResult -LogName $log -Command ("diag set dp{0} {1}" -f $c["axis"], $c["value"])
        if ($setupReply -notmatch 'result=ok') { throw "align setup rejected" }
        [void](Save-CommandResult -LogName $log -Command "state start")
        Start-Sleep -Milliseconds 1200
        for ($k = 0; $k -lt 3; $k++) {
            $encReply = Save-CommandResult -LogName $log -Command "obs enc" -WaitMs 150
            if ($encReply -match 'elec_rad=(-?[0-9.]+)') {
                Write-Output ("ALIGN {0} elec_rad={1}" -f $c["axis"], $Matches[1])
            }
            Start-Sleep -Milliseconds 150
        }
        Assert-CommandAllowed "state disable"
        $script:ProbeSerial.Write("state disable" + [string][char]13 + [string][char]10)
        $started = $false
        Start-Sleep -Milliseconds 400
        [void](Read-UntilQuiet -QuietMs 100 -MaxWaitMs 1200)
        [void](Save-CommandResult -LogName $log -Command "state show")
    }
}

function Invoke-VelClose {
    # Closed-loop velocity attempt with the dp-alignment zero offset applied.
    # The velocity PID actively damps the hunting that makes open-loop I-f
    # jerky. The direction sign is empirical: a wrong plant sign trips the
    # OCP and is reported; the opposite sign is then the correct command.
    $log = "velclose.log"
    Resolve-IdleForNextCase -NextCase "closed-loop velocity -30 rpm"
    [void](Save-CommandResult -LogName $log -Command "diag param encoff 2.535")
    [void](Save-CommandResult -LogName $log -Command "cfg ctrl vel")
    Invoke-ProbePulse -LogName $log -SetupCommand "diag set vel -30.0" -DurationMs 12000
    # Opposite sign attempt for plant-sign discrimination.
    Resolve-IdleForNextCase -NextCase "closed-loop velocity +30 rpm"
    [void](Save-CommandResult -LogName $log -Command "diag param encoff 2.535")
    [void](Save-CommandResult -LogName $log -Command "cfg ctrl vel")
    Invoke-ProbePulse -LogName $log -SetupCommand "diag set vel 30.0" -DurationMs 12000
}

function Invoke-VoltDrag {
    # Mecco-style PURE voltage open loop (current loop bypassed in the control
    # path; sampling only feeds the fault check). This isolates rotation
    # smoothness from the known current-sampling distortion.
    $cases = @(
        @{ vq = "3.00"; we = "21.9911" }
    )
    foreach ($c in $cases) {
        $log = "voltdrag_vq{0}_we{1}.log" -f $c["vq"].Replace('.', '_'), $c["we"].Replace('.', '_')
        Resolve-IdleForNextCase -NextCase ("vq={0}V we={1}" -f $c["vq"], $c["we"])
        [void](Save-CommandResult -LogName $log -Command "diag enc trace clear")
        Invoke-ProbePulse -LogName $log -SetupCommand ("diag set vq {0} {1}" -f $c["vq"], $c["we"]) -DurationMs 28000
        [void](Save-CommandResult -LogName $log -Command "diag enc trace show")
    }
}

function Invoke-PosClose {
    # Closed-loop position servo discriminator: step backward first (out of
    # the suspected jam zone), then forward (into it). Poll the timeline.
    foreach ($target in @("-0.05", "0.10")) {
        $log = "posclose_t{0}.log" -f $target.Replace('-', 'minus').Replace('.', '_')
        Resolve-IdleForNextCase -NextCase ("pos target {0}" -f $target)
        [void](Save-CommandResult -LogName $log -Command "diag param encoff 2.535")
        [void](Save-CommandResult -LogName $log -Command "cfg ctrl pos")
        Invoke-ProbePulse -LogName $log -SetupCommand ("diag set pos {0}" -f $target) -DurationMs 8000
    }
}

function Invoke-Handoff {
    # I-f spin-up for 3 s (breaks static friction), then the in-run handoff
    # to closed-loop velocity control at the commanded speed. The velocity
    # PID should actively damp the open-loop hunting: compare the poll
    # timeline before and after the handoff.
    $log = "handoff.log"
    Resolve-IdleForNextCase -NextCase "if start -> if2vel handoff"
    [void](Save-CommandResult -LogName $log -Command "diag enc trace clear")
    [void](Save-CommandResult -LogName $log -Command "diag set if 0.50 4.000000")

    Assert-CommandAllowed "state start"
    $script:PulseStart = [DateTime]::UtcNow
    $script:ProbeSerial.Write("state start" + [string][char]13 + [string][char]10)
    $started = $true
    try {
        # Let the I-f crawl break static friction and lock (we=4 tracks 1:1),
        # then hand off to the closed velocity loop in the same direction.
        Start-Sleep -Milliseconds 2500
        [void](Save-CommandResult -LogName $log -Command "diag if2vel -30.0")

        $pollDeadline = [DateTime]::UtcNow.AddSeconds(28)
        $pollIndex = 0
        while ([DateTime]::UtcNow -lt $pollDeadline) {
            Start-Sleep -Milliseconds 700
            $pollIndex++
            try {
                $script:ProbeSerial.Write("state show" + [string][char]13 + [string][char]10)
                Start-Sleep -Milliseconds 120
                $pollReply = Read-UntilQuiet -QuietMs 60 -MaxWaitMs 600
                $pos = if ($pollReply -match 'pos_turn=(-?[0-9.]+)') { $Matches[1] } else { '?' }
                $vel = if ($pollReply -match 'vel_rpm=(-?[0-9.]+)') { $Matches[1] } else { '?' }
                $iqs = if ($pollReply -match 'iq=(-?[0-9.]+)') { $Matches[1] } else { '?' }
                Write-Output ("POLL {0} pos={1} vel_rpm={2} iq={3}" -f $pollIndex, $pos, $vel, $iqs)
                Write-Log -Name $log -Text ("--- poll {0}: pos_turn={1} vel_rpm={2} iq={3}" -f $pollIndex, $pos, $vel, $iqs)
            } catch { }
        }
    } finally {
        if ($started -and ($null -ne $script:ProbeSerial) -and $script:ProbeSerial.IsOpen) {
            try {
                $script:ProbeSerial.Write("state disable" + [string][char]13 + [string][char]10)
            } catch { }
        }
    }
    Start-Sleep -Milliseconds 400
    [void](Read-UntilQuiet -QuietMs 100 -MaxWaitMs 1500)
    [void](Save-CommandResult -LogName $log -Command "state show")
    [void](Save-CommandResult -LogName $log -Command "fault status")
    [void](Save-CommandResult -LogName $log -Command "obs all")
    [void](Save-CommandResult -LogName $log -Command "diag enc trace show")
}

function Invoke-FlashBenchElf {
    param([Parameter(Mandatory = $true)][string]$BenchElf)

    # OpenOCD evaluates backslashes in its command string as escapes.  Use a
    # forward-slash path so the Windows ELF path reaches the programmer intact.
    $benchElfForOpenOcd = $BenchElf -replace '\\', '/'
    $flashArgs = @(
        "-f", (Join-Path $RepoRoot "tools\openocd\openocd_daplink.cfg"),
        "-c", ("init; program `"{0}`" verify reset exit" -f $benchElfForOpenOcd)
    )
    $previousEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $flashLog = & $OpenOcdBin @flashArgs 2>&1 | Out-String
        $flashExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousEap
    }
    Write-Log -Name "flash.log" -Text $flashLog
    if (($flashExit -ne 0) -or ($flashLog -notmatch 'Verified OK')) {
        throw "AutonomousDiag bench ELF flash/verify failed."
    }
}

function Send-BenchKeepAlive {
    param(
        [Parameter(Mandatory = $true)][string]$LogName,
        [switch]$NoWait
    )

    if ($NoWait) {
        Assert-CommandAllowed "diag bench keepalive"
        $script:ProbeSerial.Write("diag bench keepalive" + [string][char]13 + [string][char]10)
        return
    }

    $reply = Save-CommandResult -LogName $LogName -Command "diag bench keepalive" -WaitMs 10
    if ($reply -notmatch 'cmd=bench action=keepalive result=ok') {
        throw "Bench keepalive was rejected while the axis is running."
    }
}

function Invoke-AdcProof {
    # Six low-energy vector pulses confirm that the corrected zero chain still
    # observes the established rank/PWM pairing before any closed-loop run.
    $cases = @(
        @{ axis = 0; value = "0.20" }, @{ axis = 1; value = "0.20" }, @{ axis = 2; value = "0.20" },
        @{ axis = 0; value = "-0.20" }, @{ axis = 1; value = "-0.20" }, @{ axis = 2; value = "-0.20" }
    )
    foreach ($c in $cases) {
        $log = "adcproof_dp{0}_{1}.log" -f $c["axis"], $c["value"].Replace('-', 'minus').Replace('.', '_')
        Resolve-IdleForNextCase -NextCase ("AdcProof dp{0}={1}" -f $c["axis"], $c["value"])
        [void](Save-CommandResult -LogName $log -Command "diag adc show")
        [void](Save-CommandResult -LogName $log -Command "diag map show")
        [void](Save-CommandResult -LogName $log -Command ("diag set adc_trig {0}" -f $script:BenchAdcTriggerDefault))
        Invoke-ProbePulse -LogName $log -SetupCommand ("diag set dp{0} {1}" -f $c["axis"], $c["value"]) -DurationMs 100
        [void](Save-CommandResult -LogName $log -Command "diag adc show")
    }
}

function Invoke-AdcBaseline {
    $path = Join-Path $OutputRoot "adc-baseline.csv"
    $baselineCount = 5
    if ($script:AutonomousDiag) {
        $baselineCount = 20
    }
    "boot,ia_raw,ib_raw,ic_raw,ia_off,ib_off,ic_off,ia_res,ib_res,ic_res,result" |
        Set-Content -LiteralPath $path -Encoding UTF8
    for ($boot = 1; $boot -le $baselineCount; $boot++) {
        if ($boot -gt 1) {
            Invoke-SwdResetGate -NextCase ("ADC baseline SWD boot {0}" -f $boot)
        }
        $log = "adc-baseline-boot{0}.log" -f $boot
        Assert-Idle
        $reply = Save-CommandResult -LogName $log -Command "diag adc show"
        [void](Save-CommandResult -LogName $log -Command "obs all")
        $raw = [regex]::Match($reply, '(?m)(?:\[adc_raw\]|adc_raw:)\s+raw=([-0-9.]+)/([-0-9.]+)/([-0-9.]+)\s+off=([-0-9.]+)/([-0-9.]+)/([-0-9.]+)')
        $res = [regex]::Match($reply, '(?m)(?:\[adc_res\]|adc_res:)\s+a=([-0-9.]+)/([-0-9.]+)/([-0-9.]+)')
        if ((-not $raw.Success) -or (-not $res.Success) -or ($reply -notmatch 'valid=1') -or ($reply -notmatch 'samples=2000 fresh=2000')) {
            throw "ADC baseline boot $boot is incomplete or invalid; do not run AdcProof or SpeedStep."
        }
        $values = @($raw.Groups | Select-Object -Skip 1 | ForEach-Object { [double]::Parse($_.Value, [System.Globalization.CultureInfo]::InvariantCulture) })
        $residuals = @($res.Groups | Select-Object -Skip 1 | ForEach-Object { [double]::Parse($_.Value, [System.Globalization.CultureInfo]::InvariantCulture) })
        $pass = (([Math]::Abs($values[0] - $values[3])) -le 30.0 -and
                 ([Math]::Abs($values[1] - $values[4])) -le 30.0 -and
                 ([Math]::Abs($values[2] - $values[5])) -le 30.0 -and
                 (([Math]::Abs($residuals[0] + $residuals[1] + $residuals[2])) -le 0.06))
        (@($boot) + $values + $residuals + $(if ($pass) { 'pass' } else { 'fail' })) -join ',' |
            Add-Content -LiteralPath $path -Encoding UTF8
        if (-not $pass) { throw "ADC baseline boot $boot violates residual acceptance; stop the test lane." }
    }
}

function Invoke-AdcZeroRepeat {
    $path = Join-Path $OutputRoot "adc-zero-repeat.csv"
    "cycle,result,valid,samples,fresh" | Set-Content -LiteralPath $path -Encoding UTF8
    for ($cycle = 1; $cycle -le 20; $cycle++) {
        $log = "adc-zero-repeat-{0}.log" -f $cycle
        Assert-Idle
        $zero = Save-CommandResult -LogName $log -Command "diag adc zero" -MaxWaitMs 5000
        $show = Save-CommandResult -LogName $log -Command "diag adc show"
        $pass = ($zero -match 'cmd=adc action=zero result=ok') -and
                ($show -match 'valid=1') -and
                ($show -match 'samples=2000 fresh=2000')
        "{0},{1},{2},{3},{4}" -f $cycle,
            ($(if ($pass) { 'pass' } else { 'fail' })),
            ($(if ($show -match 'valid=1') { '1' } else { '0' })),
            ($(if ($show -match 'samples=2000') { '2000' } else { '0' })),
            ($(if ($show -match 'fresh=2000') { '2000' } else { '0' })) |
            Add-Content -LiteralPath $path -Encoding UTF8
        if (-not $pass) {
            throw "ADC zero repeat $cycle failed; stop the autonomous lane."
        }
    }
}

function Invoke-AutonomousAdcScan {
    $path = Join-Path $OutputRoot "adc-map-trigger.csv"
    "adc_map,trigger,theta0,phase,voltage,result" | Set-Content -LiteralPath $path -Encoding UTF8
    $triggers = @(512, 1024, 1536, 2048, 2560, 3072, 3584, 3740)
    $seeds = @("0.400000", "2.494400")
    $vectors = @(
        @{ Phase = 0; Voltage = "0.20" }, @{ Phase = 1; Voltage = "0.20" }, @{ Phase = 2; Voltage = "0.20" },
        @{ Phase = 0; Voltage = "-0.20" }, @{ Phase = 1; Voltage = "-0.20" }, @{ Phase = 2; Voltage = "-0.20" }
    )

    foreach ($mapId in (0..5)) {
        foreach ($trigger in $triggers) {
            foreach ($seed in $seeds) {
                foreach ($vector in $vectors) {
                    $log = "adc-map{0}-trig{1}-theta{2}-dp{3}-{4}.log" -f $mapId, $trigger,
                        $seed.Replace('.', '_'), $vector.Phase, $vector.Voltage.Replace('-', 'minus').Replace('.', '_')
                    Invoke-SwdResetGate -NextCase ("ADC map={0} trigger={1}" -f $mapId, $trigger)
                    [void](Save-CommandResult -LogName $log -Command ("diag map adc {0}" -f $mapId))
                    [void](Save-CommandResult -LogName $log -Command ("diag set adc_trig {0}" -f $trigger))
                    [void](Save-CommandResult -LogName $log -Command ("diag set theta0 {0}" -f $seed))
                    Invoke-ProbePulse -LogName $log -SetupCommand ("diag set dp{0} {1}" -f $vector.Phase, $vector.Voltage) -DurationMs 100
                    $adc = Save-CommandResult -LogName $log -Command "diag adc show"
                    $trace = Save-CommandResult -LogName $log -Command "diag trace show"
                    $pass = ($adc -match 'valid=1') -and ($trace -match 'result=ok')
                    "{0},{1},{2},{3},{4},{5}" -f $mapId, $trigger, $seed, $vector.Phase, $vector.Voltage,
                        ($(if ($pass) { 'recorded' } else { 'fail' })) | Add-Content -LiteralPath $path -Encoding UTF8
                    if (-not $pass) {
                        throw "ADC map/trigger evidence collection failed."
                    }
                }
            }
        }
    }
    Invoke-SwdResetGate -NextCase "restore ADC map0/trigger2700"
    [void](Save-CommandResult -LogName "adc-map-restore.log" -Command "diag map reset")
    [void](Save-CommandResult -LogName "adc-map-restore.log" -Command ("diag set adc_trig {0}" -f $script:BenchAdcTriggerDefault))
}

function Invoke-AutonomousPositionSweep {
    $path = Join-Path $OutputRoot "encoder-position.csv"
    "theta0,we,repeat,pos_start,pos_end,iq,result" | Set-Content -LiteralPath $path -Encoding UTF8
    $seeds = @("0.000000", "0.523599", "1.047198", "1.570796", "2.094395", "2.617994",
               "3.141592", "-2.617994", "-2.094395", "-1.570796", "-1.047198", "-0.523599")
    foreach ($seed in $seeds) {
        foreach ($we in @("4.000000", "-4.000000")) {
            for ($repeat = 1; $repeat -le 2; $repeat++) {
                $log = "position-theta{0}-we{1}-run{2}.log" -f $seed.Replace('.', '_').Replace('-', 'minus'), $we.Replace('-', 'minus').Replace('.', '_'), $repeat
                Invoke-SwdResetGate -NextCase ("position theta0={0} we={1} run={2}" -f $seed, $we, $repeat)
                [void](Save-CommandResult -LogName $log -Command "diag enc setabs 1 0.0")
                [void](Save-CommandResult -LogName $log -Command ("diag set theta0 {0}" -f $seed))
                [void](Save-CommandResult -LogName $log -Command ("diag set if 0.50 {0}" -f $we))
                $before = Save-CommandResult -LogName $log -Command "obs all"
                $started = $false
                try {
                    $script:ProbeSerial.Write("state start" + [string][char]13 + [string][char]10)
                    $started = $true
                    $deadline = [DateTime]::UtcNow.AddMilliseconds(1500)
                    $during = $before
                    while ([DateTime]::UtcNow -lt $deadline) {
                        Send-BenchKeepAlive -LogName $log
                        Start-Sleep -Milliseconds 200
                        $during = Save-CommandResult -LogName $log -Command "obs all" -WaitMs 10
                    }
                    $after = $during
                } finally {
                    if ($started -and ($null -ne $script:ProbeSerial) -and $script:ProbeSerial.IsOpen) {
                        try { $script:ProbeSerial.Write("state disable" + [string][char]13 + [string][char]10) } catch { }
                    }
                }
                $posStart = Get-ReplyFloat -Reply $before -Field "pos_turn"
                $posEnd = Get-ReplyFloat -Reply $after -Field "pos_turn"
                $iq = Get-ReplyFloat -Reply $after -Field "iq"
                $result = if (([Math]::Abs($iq) -gt 0.30) -and ([Math]::Abs($posEnd - $posStart) -lt 0.03)) { "freeze_candidate" } else { "recorded" }
                "{0},{1},{2},{3},{4},{5},{6}" -f $seed, $we, $repeat, (Format-SpeedStepNumber $posStart),
                    (Format-SpeedStepNumber $posEnd), (Format-SpeedStepNumber $iq), $result |
                    Add-Content -LiteralPath $path -Encoding UTF8
            }
        }
    }
}

function Get-ReplyFloat {
    param(
        [Parameter(Mandatory = $true)][string]$Reply,
        [Parameter(Mandatory = $true)][string]$Field
    )

    $match = [regex]::Match($Reply, ("(?<![A-Za-z0-9_]){0}=(-?[0-9]+(?:\.[0-9]+)?)(?![A-Za-z0-9_])" -f [regex]::Escape($Field)))
    if (-not $match.Success) {
        throw "Required field '$Field' is absent from the board reply."
    }
    return [double]::Parse($match.Groups[1].Value, [System.Globalization.CultureInfo]::InvariantCulture)
}

function Get-ReplyPhaseCurrent {
    param(
        [Parameter(Mandatory = $true)][string]$Reply,
        [Parameter(Mandatory = $true)][ValidateSet("ia", "ib", "ic")][string]$Phase
    )

    $number = '-?[0-9]+(?:\.[0-9]+)?'
    $match = [regex]::Match($Reply, ("(?m)(?:\[amps\]|amps\s*:)\s+i=({0})/({0})/({0})" -f $number))
    if (-not $match.Success) {
        throw "Required three-phase current row is absent from the board reply."
    }
    $index = @{ ia = 1; ib = 2; ic = 3 }[$Phase]
    return [double]::Parse($match.Groups[$index].Value, [System.Globalization.CultureInfo]::InvariantCulture)
}

function Get-Median {
    param([Parameter(Mandatory = $true)][double[]]$Values)

    if ($Values.Count -eq 0) { throw "Median requires at least one sample." }
    $sorted = @($Values | Sort-Object)
    $middle = [int]($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 0) {
        return ($sorted[$middle - 1] + $sorted[$middle]) / 2.0
    }
    return $sorted[$middle]
}

function Get-StdDev {
    param([Parameter(Mandatory = $true)][double[]]$Values)

    if ($Values.Count -eq 0) { throw "Standard deviation requires at least one sample." }
    $mean = ($Values | Measure-Object -Average).Average
    $sumSquares = 0.0
    foreach ($value in $Values) {
        $delta = $value - $mean
        $sumSquares += $delta * $delta
    }
    return [Math]::Sqrt($sumSquares / $Values.Count)
}

function Get-MaxAbs {
    param([Parameter(Mandatory = $true)][double[]]$Values)

    if ($Values.Count -eq 0) { throw "Peak requires at least one sample." }
    return (($Values | ForEach-Object { [Math]::Abs($_) } | Measure-Object -Maximum).Maximum)
}

function Get-MinValue {
    param([Parameter(Mandatory = $true)][double[]]$Values)

    if ($Values.Count -eq 0) { throw "Minimum requires at least one sample." }
    return (($Values | Measure-Object -Minimum).Minimum)
}

function Get-MaxValue {
    param([Parameter(Mandatory = $true)][double[]]$Values)

    if ($Values.Count -eq 0) { throw "Maximum requires at least one sample." }
    return (($Values | Measure-Object -Maximum).Maximum)
}

function Format-SpeedStepNumber {
    param([Parameter(Mandatory = $true)][double]$Value)
    return $Value.ToString("0.000", [System.Globalization.CultureInfo]::InvariantCulture)
}

function Write-SpeedStepSample {
    param(
        [Parameter(Mandatory = $true)][string]$Direction,
        [Parameter(Mandatory = $true)][int]$Repeat,
        [Parameter(Mandatory = $true)][int]$TargetRpm,
        [Parameter(Mandatory = $true)][int]$SampleIndex,
        [Parameter(Mandatory = $true)][double]$ElapsedMs,
        [Parameter(Mandatory = $true)][double]$PosTurns,
        [Parameter(Mandatory = $true)][double]$VelRpm,
        [Parameter(Mandatory = $true)][double]$Iq,
        [Parameter(Mandatory = $true)][double]$Ia,
        [Parameter(Mandatory = $true)][double]$Ib,
        [Parameter(Mandatory = $true)][double]$Ic,
        [Parameter(Mandatory = $true)][double]$Vbus
    )

    @($Direction, $Repeat, $TargetRpm, $SampleIndex,
      (Format-SpeedStepNumber $ElapsedMs), (Format-SpeedStepNumber $PosTurns),
      (Format-SpeedStepNumber $VelRpm), (Format-SpeedStepNumber $Iq),
      (Format-SpeedStepNumber $Ia), (Format-SpeedStepNumber $Ib),
      (Format-SpeedStepNumber $Ic), (Format-SpeedStepNumber $Vbus)) -join ',' |
        Add-Content -LiteralPath $script:SpeedStepSamplesPath -Encoding UTF8
}

function Invoke-SpeedStepStage {
    param(
        [Parameter(Mandatory = $true)][string]$Direction,
        [Parameter(Mandatory = $true)][int]$Repeat,
        [Parameter(Mandatory = $true)][int]$TargetRpm
    )

    $logName = "speedstep_{0}_run{1}.log" -f $Direction, $Repeat
    $stepReply = Save-CommandResult -LogName $logName -Command ("diag velstep {0}" -f $TargetRpm.ToString("0.0", [System.Globalization.CultureInfo]::InvariantCulture))
    if ($stepReply -notmatch 'cmd=velstep result=ok') {
        throw "Velocity step to $TargetRpm rpm was rejected; cut 12 V if the axis is not safe."
    }

    $samples = @()
    $stageStart = [DateTime]::UtcNow
    for ($sampleIndex = 1; $sampleIndex -le 25; $sampleIndex++) {
        $sampleDue = $stageStart.AddMilliseconds(($sampleIndex - 1) * 200)
        $sleepMs = [Math]::Ceiling($sampleDue.Subtract([DateTime]::UtcNow).TotalMilliseconds)
        if ($sleepMs -gt 0) {
            Start-Sleep -Milliseconds $sleepMs
        }
        Send-BenchKeepAlive -LogName $logName -NoWait
        $reply = Save-CommandResult -LogName $logName -Command "obs all" -WaitMs 10
        if ($reply -notmatch 'cmd=bench action=keepalive result=ok') {
            throw "Bench keepalive was not acknowledged by the next board reply."
        }
        $sample = [pscustomobject]@{
            PosTurns = Get-ReplyFloat -Reply $reply -Field "pos_turn"
            VelRpm = Get-ReplyFloat -Reply $reply -Field "vel_rpm"
            Iq = Get-ReplyFloat -Reply $reply -Field "iq"
            Ia = Get-ReplyPhaseCurrent -Reply $reply -Phase "ia"
            Ib = Get-ReplyPhaseCurrent -Reply $reply -Phase "ib"
            Ic = Get-ReplyPhaseCurrent -Reply $reply -Phase "ic"
            Vbus = Get-ReplyFloat -Reply $reply -Field "vbus"
        }
        $samples += $sample
        Write-SpeedStepSample -Direction $Direction -Repeat $Repeat -TargetRpm $TargetRpm -SampleIndex $sampleIndex `
            -ElapsedMs ([DateTime]::UtcNow.Subtract($stageStart).TotalMilliseconds) -PosTurns $sample.PosTurns `
            -VelRpm $sample.VelRpm -Iq $sample.Iq -Ia $sample.Ia -Ib $sample.Ib -Ic $sample.Ic -Vbus $sample.Vbus

        if (([Math]::Abs($sample.Ia) -ge $script:SpeedStepPeakAbortAmps) -or ([Math]::Abs($sample.Ib) -ge $script:SpeedStepPeakAbortAmps) -or
            ([Math]::Abs($sample.Ic) -ge $script:SpeedStepPeakAbortAmps) -or ($sample.Vbus -lt 10.8) -or ($sample.Vbus -gt 13.2)) {
            throw "SpeedStep safety envelope breached at $TargetRpm rpm; cut the 12 V supply immediately."
        }
    }

    $stateReply = Save-CommandResult -LogName $logName -Command "state show"
    $faultReply = Save-CommandResult -LogName $logName -Command "fault status"
    $queueReply = Save-CommandResult -LogName $logName -Command "sys status"
    if (($stateReply -notmatch 'axis=running') -or ($faultReply -match 'active=([^0]|0[1-9])')) {
        throw "SpeedStep stage $TargetRpm rpm did not remain running and fault-free; cut the 12 V supply immediately."
    }

    $velocities = [double[]]@($samples | ForEach-Object { $_.VelRpm })
    $median = Get-Median $velocities
    $stddev = Get-StdDev $velocities
    $iaPeak = Get-MaxAbs ([double[]]@($samples | ForEach-Object { $_.Ia }))
    $ibPeak = Get-MaxAbs ([double[]]@($samples | ForEach-Object { $_.Ib }))
    $icPeak = Get-MaxAbs ([double[]]@($samples | ForEach-Object { $_.Ic }))
    $vbusMin = Get-MinValue ([double[]]@($samples | ForEach-Object { $_.Vbus }))
    $vbusMax = Get-MaxValue ([double[]]@($samples | ForEach-Object { $_.Vbus }))
    $queueUsed = Get-ReplyFloat -Reply $queueReply -Field "queue_used"
    $queueDepth = Get-ReplyFloat -Reply $queueReply -Field "queue_depth"
    $posDelta = $samples[$samples.Count - 1].PosTurns - $samples[0].PosTurns
    $pass = (([Math]::Abs($median - $TargetRpm) -le ([Math]::Abs($TargetRpm) * 0.10)) -and
             ($stddev -le ([Math]::Abs($TargetRpm) * 0.10)) -and
             (($TargetRpm -gt 0 -and $posDelta -gt 0.0) -or ($TargetRpm -lt 0 -and $posDelta -lt 0.0)))
    @($Direction, $Repeat, $TargetRpm, (Format-SpeedStepNumber $median), (Format-SpeedStepNumber $stddev),
      (Format-SpeedStepNumber $posDelta), (Format-SpeedStepNumber $iaPeak), (Format-SpeedStepNumber $ibPeak),
      (Format-SpeedStepNumber $icPeak), (Format-SpeedStepNumber $vbusMin), (Format-SpeedStepNumber $vbusMax),
      "running", "none", $queueUsed, $queueDepth, ($(if ($pass) { 'pass' } else { 'fail' }))) -join ',' |
        Add-Content -LiteralPath $script:SpeedStepSummaryPath -Encoding UTF8
    if (-not $pass) {
        throw "SpeedStep acceptance failed at $TargetRpm rpm; retain the logs and do not claim a hardware root cause."
    }
}

function Invoke-SpeedStep {
    param([switch]$SkipAdcPreflight)

    if (-not $SkipAdcPreflight) {
        Resolve-IdleForNextCase -NextCase "SpeedStep ADC baseline"
        Invoke-AdcBaseline
        Invoke-AdcProof
    }

    $script:SpeedStepSamplesPath = Join-Path $OutputRoot "speed-step-samples.csv"
    $script:SpeedStepSummaryPath = Join-Path $OutputRoot "speed-step-summary.csv"
    "direction,repeat,target_rpm,sample_index,elapsed_ms,pos_turn,vel_rpm,iq,ia,ib,ic,vbus" |
        Set-Content -LiteralPath $script:SpeedStepSamplesPath -Encoding UTF8
    "direction,repeat,target_rpm,median_vel_rpm,stddev_vel_rpm,pos_delta_turn,ia_peak,ib_peak,ic_peak,vbus_min,vbus_max,state,fault,queue_used,queue_depth,result" |
        Set-Content -LiteralPath $script:SpeedStepSummaryPath -Encoding UTF8
    @(
        "# SpeedStep operator observations",
        "",
        "- PSU: set 12 V and current limit >=2 A; record each plateau's front-panel current manually.",
        "- Stop immediately on abnormal sound, vibration, heat, smell, PSU current-limit indication, or lost serial.",
        "- No oscilloscope is present: a failure remains a serial/power observation, not a magnetic-vs-power-stage diagnosis.",
        "- Fill manually: direction, repeat, 30/60/90 rpm PSU current, sound, vibration, and stop reason.",
        "",
        "| direction | repeat | 30 rpm PSU A | 60 rpm PSU A | 90 rpm PSU A | sound/vibration | stop reason |",
        "| --- | --- | --- | --- | --- | --- | --- |",
        "| negative | 1 | | | | | |",
        "| negative | 2 | | | | | |",
        "| negative | 3 | | | | | |",
        "| positive | 1 | | | | | |",
        "| positive | 2 | | | | | |",
        "| positive | 3 | | | | | |"
    ) | Set-Content -LiteralPath (Join-Path $OutputRoot "speed-step-operator-notes.md") -Encoding UTF8
    if ($script:AutonomousDiag) {
        @(
            "# Autonomous SpeedStep evidence",
            "",
            "- physical_power_cycle=false",
            "- psu_telemetry=unavailable",
            "- The machine result is derived from serial, VBUS, current, and watchdog fields only.",
            "- Sound, vibration, thermal state, waveform, and magnet gap remain unverified."
        ) | Set-Content -LiteralPath (Join-Path $OutputRoot "speed-step-operator-notes.md") -Encoding UTF8
    }

    $cases = @(
        @{ Direction = "negative"; We = "4.000000"; HandoffRpm = -30 },
        @{ Direction = "positive"; We = "-4.000000"; HandoffRpm = 30 }
    )
    foreach ($case in $cases) {
        for ($repeat = 1; $repeat -le 3; $repeat++) {
            $logName = "speedstep_{0}_run{1}.log" -f $case.Direction, $repeat
            Resolve-IdleForNextCase -NextCase ("SpeedStep {0} run {1}" -f $case.Direction, $repeat)
            [void](Save-CommandResult -LogName $logName -Command "diag enc setabs 1 0.0")
            [void](Save-CommandResult -LogName $logName -Command "diag adc show")
            [void](Save-CommandResult -LogName $logName -Command "diag trace clear")
            [void](Save-CommandResult -LogName $logName -Command "diag enc trace clear")
            [void](Save-CommandResult -LogName $logName -Command ("diag set if 0.50 {0}" -f $case.We))

            $started = $false
            try {
                Assert-CommandAllowed "state start"
                $script:ProbeSerial.Write("state start" + [string][char]13 + [string][char]10)
                $started = $true
                $launchDeadline = [DateTime]::UtcNow.AddMilliseconds(2500)
                while ([DateTime]::UtcNow -lt $launchDeadline) {
                    Send-BenchKeepAlive -LogName $logName
                    Start-Sleep -Milliseconds 200
                }
                $handoffReply = Save-CommandResult -LogName $logName -Command ("diag if2vel {0}.0" -f $case.HandoffRpm)
                if ($handoffReply -notmatch 'cmd=if2vel result=ok') {
                    throw "I-f handoff was rejected for $($case.Direction); cut 12 V if the axis is not safe."
                }
                foreach ($rpm in @(30, 60, 90)) {
                    Invoke-SpeedStepStage -Direction $case.Direction -Repeat $repeat -TargetRpm ($rpm * [Math]::Sign($case.HandoffRpm))
                }
            } finally {
                if ($started -and ($null -ne $script:ProbeSerial) -and $script:ProbeSerial.IsOpen) {
                    try { $script:ProbeSerial.Write("state disable" + [string][char]13 + [string][char]10) } catch { }
                }
            }
            Start-Sleep -Milliseconds 400
            [void](Read-UntilQuiet -QuietMs 100 -MaxWaitMs 1500)
            [void](Save-CommandResult -LogName $logName -Command "state show")
            [void](Save-CommandResult -LogName $logName -Command "fault status")
            [void](Save-CommandResult -LogName $logName -Command "diag trace show")
            [void](Save-CommandResult -LogName $logName -Command "diag enc trace show")
            Start-Sleep -Seconds 60
        }
    }
}

function Invoke-TorqueSweep {
    # Torque-angle sweep: with the current loop running in the encoder frame,
    # sweep the electrical-zero offset in 45 deg steps. The offset that
    # produces the largest rotor travel IS the true zero offset (the value to
    # keep in diag param encoff). iq 0.45 A, 2.5 s per case.
    $number = '[-+]?[0-9]+(?:\.[0-9]+)?'
    $offsetsDeg = @(0, 45, 90, 135, 180, -135, -90, -45)
    foreach ($deg in $offsetsDeg) {
        $rad = [math]::Round($deg * [math]::PI / 180.0, 4)
        $radText = $rad.ToString("0.0000", [System.Globalization.CultureInfo]::InvariantCulture)
        if ($rad -ge 0) { $radText = $radText } else { $radText = $radText }
        $log = "sweep_off{0}.log" -f $deg.ToString("0").Replace('-', 'minus')
        Resolve-IdleForNextCase -NextCase ("encoff offset {0} deg" -f $deg)
        [void](Save-CommandResult -LogName $log -Command "diag param encoff $radText")
        [void](Save-CommandResult -LogName $log -Command "cfg ctrl curr")
        [void](Save-CommandResult -LogName $log -Command "cfg input torque-ramp")
        Invoke-ProbePulse -LogName $log -SetupCommand "diag set iq 0.45 2000.0" -DurationMs 2500
    }
}

function Invoke-IfDrag {
    # I-f open-loop drag with the corrected current chain: the current loop
    # holds iq on the rotating theta_sim, so the rotor should physically
    # follow. Encoder trace before/after captures direction and tracking.
    # we is ELECTRICAL rad/s: 21.9911 ~= 30 rpm mechanical at 7 pole pairs.
    $cases = @(
        @{ iq = "1.20"; we = "21.9911" }
    )
    foreach ($c in $cases) {
        $log = "ifdrag_iq{0}_we{1}.log" -f $c["iq"].Replace('.', '_'), $c["we"].Replace('.', '_')
        Resolve-IdleForNextCase -NextCase ("if={0}A we={1}" -f $c["iq"], $c["we"])
        [void](Save-CommandResult -LogName $log -Command ("diag set adc_trig {0}" -f $script:BenchAdcTriggerDefault))
        [void](Save-CommandResult -LogName $log -Command "diag enc trace clear")
        Invoke-ProbePulse -LogName $log -SetupCommand ("diag set if {0} {1}" -f $c["iq"], $c["we"]) -DurationMs 28000
        [void](Save-CommandResult -LogName $log -Command "diag enc trace show")
        [void](Save-CommandResult -LogName $log -Command "diag enc trace sample 0")
        [void](Save-CommandResult -LogName $log -Command "diag enc trace sample 31")
    }
}

function Invoke-TriggerScan {
    # Two theta0 seeds whose CCR orderings rotate; the measured CCRs are read
    # back from each trace, so exact duty prediction is not required.
    $seeds = @("0.400000", "2.494400")
    # Transition band first: if the operator can only spare a few power
    # cycles, the first eight points already bracket the predicted
    # aperture/CCR crossing region (plan P4).
    $triggers = @(1950, 1874, 2050, 2150, 1800, 2250, 1700, 2400, 1400, 2700, 900, 3100)
    foreach ($seed in $seeds) {
        foreach ($trig in $triggers) {
            $log = "scan_theta{0}_trig{1}.log" -f $seed.Replace('.', '_'), $trig
            Resolve-IdleForNextCase -NextCase ("theta0={0}, adc_trig={1}" -f $seed, $trig)
            [void](Save-CommandResult -LogName $log -Command "diag map show")
            [void](Save-CommandResult -LogName $log -Command ("diag set adc_trig {0}" -f $trig))
            [void](Save-CommandResult -LogName $log -Command ("diag set theta0 {0}" -f $seed))
            Invoke-ProbePulse -LogName $log -SetupCommand ("diag set vq 1.00 0.00") -DurationMs 100
        }
    }
}

function Invoke-ViLinearity {
    $voltages = @("0.75", "1.25", "1.75", "2.25")
    foreach ($v in $voltages) {
        $log = "vi_vq_{0}.log" -f $v.Replace('.', '_')
        Resolve-IdleForNextCase -NextCase ("vq={0}" -f $v)
        [void](Save-CommandResult -LogName $log -Command ("diag set adc_trig {0}" -f $script:BenchAdcTriggerDefault))
        [void](Save-CommandResult -LogName $log -Command "diag set theta0 0.400000")
        Invoke-ProbePulse -LogName $log -SetupCommand ("diag set vq {0} 0.00" -f $v) -DurationMs 100
    }
}

function Invoke-ThetaSweep {
    # Torque-angle / alignment sweep: park a fixed stator vector at
    # theta_elec + seed and let the rotor settle.  In a healthy band the rotor
    # aligns its d-axis to the field axis (theta_sim + 90 deg) and rests, so
    #
    #   delta = entry_elec + seed + 90deg - end_elec   (mod 2pi)
    #
    # is the encoder electrical-angle error at that spot; it must be CONSTANT
    # across seeds for a healthy encoder.  Seeds with no motion mark low/zero
    # torque regions (below breakaway or an output-stage torque hole).  With
    # -SweepPwmMap N the PWM+ADC bench maps are re-applied after each SWD
    # reset (maps do not survive reboot), mirroring the excitation for the
    # map A/B discrimination.
    #
    # theta0 must stay within [-pi, +pi] (firmware range check); twelve seeds
    # cover the full electrical circle every 30 degrees.  -SweepVq must be
    # high enough for the torque to approach the measured breakaway (~30 mN*m
    # at 3.5 ohm phase resistance => ~3.0 V) or every seed reads "no motion".
    $seeds = @(
        "0.000000", "0.523599", "1.047198", "1.570796",
        "2.094395", "2.617994", "3.141592",
        "-2.617994", "-2.094395", "-1.570796", "-1.047198", "-0.523599"
    )
    foreach ($seed in $seeds) {
        $log = "sweep_theta{0}.log" -f $seed.Replace('.', '_').Replace('-', 'm')
        Resolve-IdleForNextCase -NextCase ("theta0={0}" -f $seed)
        [void](Save-CommandResult -LogName $log -Command ("diag set adc_trig {0}" -f $script:BenchAdcTriggerDefault))
        if ($SweepPwmMap -gt 0) {
            [void](Save-CommandResult -LogName $log -Command ("diag map set pwm {0}" -f $SweepPwmMap))
            [void](Save-CommandResult -LogName $log -Command ("diag map set adc {0}" -f $SweepPwmMap))
        }
        [void](Save-CommandResult -LogName $log -Command ("diag set theta0 {0}" -f $seed))
        [void](Save-CommandResult -LogName $log -Command "obs all")
        Invoke-ProbePulse -LogName $log -SetupCommand ("diag set vq {0} 0.00" -f $SweepVq) -DurationMs $SweepMs
        [void](Save-CommandResult -LogName $log -Command "obs all")
    }
}

function Invoke-AutonomousDiag {
    $script:ResetMode = "swd"
    $adcConfigPath = Join-Path $RepoRoot "platform\at\at32f415\generated\src\wk_adc.c"
    $adcConfig = Get-Content -LiteralPath $adcConfigPath -Raw
    $sourceFacts = [ordered]@{
        rank2_ch4 = ($adcConfig -match 'ADC_CHANNEL_4,\s*2')
        rank3_ch5 = ($adcConfig -match 'ADC_CHANNEL_5,\s*3')
        rank4_ch6 = ($adcConfig -match 'ADC_CHANNEL_6,\s*4')
        trigger_tmr1_ch4 = ($adcConfig -match 'ADC12_PREEMPT_TRIG_TMR1CH4')
        production_map = 0
    }
    $sourceFacts | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputRoot "source-facts.json") -Encoding UTF8
    if ((-not $sourceFacts.rank2_ch4) -or (-not $sourceFacts.rank3_ch5) -or
        (-not $sourceFacts.rank4_ch6) -or (-not $sourceFacts.trigger_tmr1_ch4)) {
        throw "Generated ADC source facts do not match the autonomous-test contract."
    }
    $manifest = [ordered]@{
        mode = "AutonomousDiag"
        physical_power_cycle = $false
        psu_telemetry = "unavailable"
        reset_mode = "swd"
        port = $PortName
        baud = $BaudRate
        elf_sha256 = $script:BenchElfHash
    }
    $manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputRoot "autonomous-manifest.json") -Encoding UTF8

    [void](Save-CommandResult -LogName "preflight.log" -Command "diag bench status")
    Invoke-AdcBaseline
    Invoke-AdcZeroRepeat
    Invoke-AdcProof
    Invoke-AutonomousAdcScan
    Invoke-AutonomousPositionSweep
    Invoke-SpeedStep -SkipAdcPreflight

    $analyzer = Join-Path $RepoRoot "scripts\flows\board\analyze_autonomous_bench.py"
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($null -eq $python) {
        throw "AutonomousDiag requires python to run analyze_autonomous_bench.py."
    }
    & $python.Source $analyzer "--input-dir" $OutputRoot
    if ($LASTEXITCODE -ne 0) {
        throw "AutonomousDiag analysis.json reports a failed acceptance gate."
    }

    [ordered]@{
        result = "pass"
        physical_power_cycle = $false
        psu_telemetry = "unavailable"
        reset_mode = "swd"
        elf_sha256 = $script:BenchElfHash
        evidence = @("adc-baseline.csv", "speed-step-summary.csv", "analysis.json")
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputRoot "autonomous-result.json") -Encoding UTF8
}

try {
    # Default to the 30 s endurance image; override with -ElfPath for other
    # bench builds (trigger scans etc. used the 400 ms image).
    if ([string]::IsNullOrWhiteSpace($ElfPath)) {
        $ElfPath = Join-Path $RepoRoot "build\fw-full-diag-carrier-bench-30s-debug\output\falcon-foc-at32.elf"
    }
    $elfPath = $ElfPath
    if (-not (Test-Path -LiteralPath $elfPath)) {
        throw "Bench ELF not found: $elfPath"
    }
    $hash = (Get-FileHash -LiteralPath $elfPath -Algorithm SHA256).Hash
    $script:BenchElfHash = $hash
    Write-Log -Name "preflight.log" -Text ("mode={0}`r`nport={1}@{2}`r`nelf={3}`r`nsha256={4}`r`nmax_peak_amps={5}`r`nspeed_step_peak_abort_amps={6}" -f $Mode, $PortName, $BaudRate, $elfPath, $hash, $MaxPeakAmps, $script:SpeedStepPeakAbortAmps)

    if ($script:AutonomousDiag) {
        Invoke-FlashBenchElf -BenchElf $elfPath
    }

    if ((-not $RunPulses) -and (-not $script:AutonomousDiag)) {
        Open-ProbeSerial
        try {
            Assert-Idle
            [void](Save-CommandResult -LogName "preflight.log" -Command "diag map show")
            [void](Save-CommandResult -LogName "preflight.log" -Command "diag adc show")
            [void](Save-CommandResult -LogName "preflight.log" -Command "obs all")
        } finally {
            Close-ProbeSerial
        }
        Write-Output "OPENLOOP_PROBE_PREFLIGHT=COMPLETE"
        Write-Output "OPENLOOP_PROBE_LOG_DIR=$OutputRoot"
        exit 0
    }

    Open-ProbeSerial
    try {
        if ($InitialDelaySec -gt 0) {
            Write-Output ("OPENLOOP_PROBE_STARTING_IN {0}s" -f $InitialDelaySec)
            Start-Sleep -Seconds $InitialDelaySec
        }
        switch ($Mode) {
            "SnapTest"    { Invoke-SnapTest }
            "IfDrag"      { Invoke-IfDrag }
            "Align"       { Invoke-Align }
            "VelClose"    { Invoke-VelClose }
            "PosClose"    { Invoke-PosClose }
            "Handoff"     { Invoke-Handoff }
            "AdcBaseline" { Invoke-AdcBaseline }
            "AdcProof"    { Invoke-AdcProof }
            "SpeedStep"   { Invoke-SpeedStep }
            "AutonomousDiag" { Invoke-AutonomousDiag }
            "TorqueSweep" { Invoke-TorqueSweep }
            "VoltDrag"    { Invoke-VoltDrag }
            "TriggerScan" { Invoke-TriggerScan }
            "VILinearity" { Invoke-ViLinearity }
            "ThetaSweep"  { Invoke-ThetaSweep }
        }
    } finally {
        Close-ProbeSerial
    }

    @(
        "# Open-loop probe result",
        "",
        "- script_mode: $Mode",
        "- elf: $elfPath",
        "- sha256: $hash",
        "- status: pulses completed, see per-case logs",
        "- physical current/thermal/PWM waveform remain unverified"
    ) | Set-Content -LiteralPath (Join-Path $OutputRoot "result.md") -Encoding UTF8
    Write-Output "OPENLOOP_PROBE_STATUS=COMPLETE"
    Write-Output "OPENLOOP_PROBE_LOG_DIR=$OutputRoot"
} catch {
    if ($script:AutonomousDiag) {
        [ordered]@{
            result = "fail"
            physical_power_cycle = $false
            psu_telemetry = "unavailable"
            reset_mode = "swd"
            reason = $_.Exception.Message
        } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputRoot "autonomous-result.json") -Encoding UTF8
    }
    Write-Log -Name "result.md" -Text ("# Open-loop probe aborted`r`n`r`n- reason: {0}" -f $_.Exception.Message)
    Write-Error $_.Exception.Message
    exit 1
}
