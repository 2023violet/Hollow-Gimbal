# Latch-hunt orchestrator: waits for the operator's 12 V power cycle, then runs
# the full evidence chain automatically.
#   1) wait for VBUS drop (12 V off) and restore (12 V on)
#   2) SWD reset (the MCU stays alive on debugger 3.3 V, so the FSM needs it)
#   3) static 2.0 V sanity  -> DRV must show ~0.57 A DC currents
#   4) latch hunt: voltloop 30 rpm 5.0 V, obs every ~1 s, 15 s window
#   5) static 2.0 V again  -> zero currents here re-proves the re-latch
param([string]$LogFile = "D:/study/Software/Hollow-Gimbal/tmp/bench/latch_hunt.log")

$ErrorActionPreference = "Continue"
$repo = "D:/study/Software/Hollow-Gimbal"
$openocd = "C:/Program Files (x86)/OpenOCD_V2.0.6/bin/openocd.exe"

function Log($text) {
    $line = "[{0:HH:mm:ss}] {1}" -f (Get-Date), $text
    Write-Output $line
    Add-Content -LiteralPath $LogFile -Value $line
}

function Read-StateVbus {
    # one state show exchange; returns @(vbus, axis) or @(-1, '?')
    try {
        $sp = New-Object System.IO.Ports.SerialPort 'COM3', 115200, 'None', 8, 'One'
        $sp.ReadTimeout = 300
        $sp.Open()
        Start-Sleep -Milliseconds 150
        $sp.DiscardInBuffer()
        $sp.Write("state show`r`n")
        Start-Sleep -Milliseconds 450
        $reply = $sp.ReadExisting()
        $sp.Close()
        $vbus = if ($reply -match 'vbus=([0-9.]+)') { [double]$Matches[1] } else { -1.0 }
        $axis = if ($reply -match 'axis=(\w+)') { $Matches[1] } else { '?' }
        return @($vbus, $axis)
    } catch { return @(-1.0, 'port-error') }
}

New-Item -ItemType Directory -Force -Path (Split-Path $LogFile) | Out-Null
Log "=== LATCH HUNT ORCHESTRATOR STARTED — waiting for operator 12 V cycle ==="

# Phase 1: wait for the drop (up to 10 min)
$deadline = (Get-Date).AddMinutes(10)
$sawLow = $false
while ((Get-Date) -lt $deadline) {
    $st = Read-StateVbus
    if ($st[0] -ge 0 -and $st[0] -lt 3.0) {
        Log ("VBUS DROPPED ({0} V) — 12 V off detected" -f $st[0])
        $sawLow = $true
        break
    }
    Start-Sleep -Milliseconds 700
}
if (-not $sawLow) { Log "TIMEOUT waiting for 12 V drop (10 min). Aborting."; exit 1 }

# Phase 2: wait for restore (long window: the operator holds power off on cue)
$deadline = (Get-Date).AddMinutes(8)
$sawHigh = $false
while ((Get-Date) -lt $deadline) {
    $st = Read-StateVbus
    if ($st[0] -gt 8.0) {
        Log ("VBUS RESTORED ({0} V) — latch should be cleared" -f $st[0])
        $sawHigh = $true
        break
    }
    Start-Sleep -Milliseconds 700
}
if (-not $sawHigh) { Log "TIMEOUT waiting for 12 V restore (60 s). Aborting."; exit 1 }
Start-Sleep -Seconds 2

# Phase 3: SWD reset (MCU never rebooted — debugger 3.3 V)
Log "SWD reset..."
& $openocd -f "$repo/tools/openocd/openocd_daplink.cfg" -c "init; reset run; shutdown" 2>&1 | Out-Null
Start-Sleep -Seconds 2

# Phase 4: static 2.0 V sanity
Log "--- SANITY: static 2.0 V parked vector (expect ~0.57 A DC pattern) ---"
& powershell -NoProfile -ExecutionPolicy Bypass -File "$repo/build/_spin_probe.ps1" `
    -InitCmd "diag set vq 2.0 0.00" -Seconds 3 -SampleEvery 1 2>&1 | Tee-Object -FilePath $LogFile -Append

# Phase 5: latch hunt
Log "--- HUNT: voltloop 30 rpm 5.0 V, obs every 1 s, 15 s window ---"
& powershell -NoProfile -ExecutionPolicy Bypass -File "$repo/build/_spin_probe.ps1" `
    -InitCmd "diag voltloop 30 5.0" -Seconds 15 -SampleEvery 1 2>&1 | Tee-Object -FilePath $LogFile -Append

# Phase 6: post-latch static confirm
Log "--- POST: static 2.0 V again (zero currents here = re-latched) ---"
& powershell -NoProfile -ExecutionPolicy Bypass -File "$repo/build/_spin_probe.ps1" `
    -InitCmd "diag set vq 2.0 0.00" -Seconds 3 -SampleEvery 1 2>&1 | Tee-Object -FilePath $LogFile -Append

Log "=== ORCHESTRATOR DONE ==="
