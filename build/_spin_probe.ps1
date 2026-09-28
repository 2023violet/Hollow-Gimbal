# Debug-round spin observer: opens COM3 once, sends an init command, then
# keeps the bench heartbeat alive at a fixed cadence while sampling obs all
# periodically. Ends with state disable + fault status.
#
# Usage:
#   _spin_probe.ps1 -InitCmd "diag voltloop 30 3.6" -Seconds 8 [-SampleEvery 3] [-StartCmd "state start"]
param(
    [Parameter(Mandatory = $true)][string]$InitCmd,
    [int]$Seconds = 8,
    [double]$SampleEvery = 3,
    [string]$SampleCmd = "obs all",
    [string]$StartCmd = "state start",
    [string]$MidCmd = "",
    [int]$MidCmdAtSec = 0,
    [int]$LeadInSec = 0,
    [string]$PortName = "COM3",
    [int]$BaudRate = 115200
)

$ErrorActionPreference = "Stop"

function Send-Cmd {
    param($Sp, [string]$Text)
    $Sp.DiscardInBuffer()
    $Sp.Write("$Text`r`n")
    Start-Sleep -Milliseconds 80
    $sb = New-Object System.Text.StringBuilder
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $lastRx = $sw.ElapsedMilliseconds
    $got = $false
    while ($sw.ElapsedMilliseconds -lt 600) {
        if ($Sp.BytesToRead -gt 0) {
            $n = $Sp.BytesToRead
            $buf = New-Object byte[] $n
            [void]$Sp.Read($buf, 0, $n)
            [void]$sb.Append([System.Text.Encoding]::ASCII.GetString($buf))
            $lastRx = $sw.ElapsedMilliseconds
            $got = $true
        }
        if ($got -and (($sw.ElapsedMilliseconds - $lastRx) -gt 80)) { break }
        Start-Sleep -Milliseconds 10
    }
    return ($sb.ToString() -replace '[^\x20-\x7E\r\n]', '')
}

$sp = New-Object System.IO.Ports.SerialPort $PortName, $BaudRate, "None", 8, "One"
$sp.ReadTimeout = 20
$sp.WriteTimeout = 500
$sp.DtrEnable = $false
$sp.RtsEnable = $false

try {
    $sp.Open()
    Start-Sleep -Milliseconds 300
    [void]$sp.ReadExisting()

    Write-Output "=== SPIN PROBE: init='$InitCmd' seconds=$Seconds sample_every=$SampleEvery ==="
    foreach ($cmd in ($InitCmd -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })) {
        Write-Output (Send-Cmd $Sp $cmd)
    }

    if ($StartCmd -ne "") {
        if ($LeadInSec -gt 0) {
            Write-Output ""
            Write-Output (">>> {0}s 后加驱，退开表笔和手指 <<<" -f $LeadInSec)
            Start-Sleep -Seconds $LeadInSec
        }
        [Console]::Beep(880, 900)
        Write-Output "--- TX: $StartCmd  *** DRIVE ON ***"
        Write-Output (Send-Cmd $Sp $StartCmd)
    }

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $lastKeepalive = $sw.ElapsedMilliseconds
    $lastSample = $sw.ElapsedMilliseconds
    $sampleIdx = 0
    $midSent = ($MidCmd -eq "")

    while ($sw.ElapsedMilliseconds -lt ($Seconds * 1000)) {
        $now = $sw.ElapsedMilliseconds
        if (-not $midSent -and ($now -ge ($MidCmdAtSec * 1000))) {
            $r = Send-Cmd $Sp $MidCmd
            $midSent = $true
            Write-Output ("[{0,6}s] MIDCMD '{1}': {2}" -f ($now / 1000), $MidCmd, (($r -replace '\s+', ' ').Trim()))
        }
        if (($now - $lastKeepalive) -ge 500) {
            # Fire-and-forget heartbeat (runner-proven): reading the reply on
            # this noisy line costs more than the 1.5 s watchdog window.
            $Sp.Write("diag bench keepalive`r`n")
            $lastKeepalive = $sw.ElapsedMilliseconds
        }
        if (($now - $lastSample) -ge ($SampleEvery * 1000)) {
            $sampleIdx++
            $r = Send-Cmd $Sp $SampleCmd
            $lastSample = $sw.ElapsedMilliseconds
            if ($SampleCmd -ne "obs all") {
                Write-Output ("[{0,6}s] sample={1} {2}" -f ($now / 1000), $sampleIdx, (($r -replace '\s+', ' ').Trim()))
                continue
            }
            $vel = if ($r -match "out\s*:\s*vel_rpm=(-?[0-9.]+)\s+pos_turn=(-?[0-9.]+)") { $Matches[1] } else { "?" }
            $pos = if ($r -match "pos_turn=(-?[0-9.]+)") { $Matches[1] } else { "?" }
            $res = if ($r -match "residual:\s*i=(-?[0-9.]+)/(-?[0-9.]+)/(-?[0-9.]+)\s+sum=(-?[0-9.]+)") { $Matches[4] } else { "?" }
            $vdq = if ($r -match "vdq=(-?[0-9.]+)/(-?[0-9.]+)") { "$($Matches[1])/$($Matches[2])" } else { "?" }
            $amps = if ($r -match "amps\s*:\s*i=(-?[0-9.]+)/(-?[0-9.]+)/(-?[0-9.]+)") { "$($Matches[1])/$($Matches[2])/$($Matches[3])" } else { "?" }
            $encl = if ($r -match "elec_rad=(-?[0-9.]+)") { $Matches[1] } else { "?" }
            if ($amps -eq "?") {
                # serial noise garbled the parse; dump the raw reply so the
                # transition is still recoverable from the log
                Write-Output ("[{0,6}s] sample={1} RAW: {2}" -f ($now / 1000), $sampleIdx, (($r -replace '\s+', ' ').Trim()))
            } else {
                Write-Output ("[{0,6}s] sample={1} vel_rpm={2} pos={3} vdq={4} amps={5} kcl_sum={6} elec={7}" -f `
                    ($now / 1000), $sampleIdx, $vel, $pos, $vdq, $amps, $res, $encl)
            }
        }
        Start-Sleep -Milliseconds 50
    }

    Write-Output "--- stop"
    Write-Output (Send-Cmd $Sp "state disable")
    Write-Output (Send-Cmd $Sp "fault status")
}
finally {
    if ($sp.IsOpen) { $sp.Close() }
}
