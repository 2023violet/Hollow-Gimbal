# Audible duty-cycle probe: cycles a fixed stator vector on and off so the
# operator can watch a meter only during the announced ON windows.
#
# The rotor stays locked (dwell vector), so probe tips near the board are safe;
# no rotation is commanded.
#
# Usage:
#   _duty_cycle_probe.ps1 -Levels "3.0,6.0" [-OnSec 10] [-OffSec 5]
param(
    [Parameter(Mandatory = $true)][string]$Levels,
    [int]$OnSec = 10,
    [int]$OffSec = 5,
    [double]$SampleEvery = 3,
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

function Send-Only {
    param($Sp, [string]$Text)
    $Sp.Write("$Text`r`n")
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

    Write-Output "=== DUTY CYCLE PROBE levels='$Levels' on=${OnSec}s off=${OffSec}s ==="
    Write-Output "TONE: long 880 Hz = DRIVE ON,  short 440 Hz = DRIVE OFF"

    foreach ($lvl in ($Levels -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })) {
        [void](Send-Cmd $Sp "state disable")
        [void](Send-Cmd $Sp "fault clear")
        $r = Send-Cmd $Sp "diag set dp0 $lvl"
        Write-Output ("--- setup dp0=$lvl : " + (($r -replace '\s+', ' ').Trim()))

        Write-Output ""
        Write-Output ">>> ${OffSec}s 静默期（无驱动），准备 <<<"
        $sp.DiscardInBuffer()
        Start-Sleep -Seconds $OffSec

        [Console]::Beep(880, 900)
        $rs = Send-Cmd $Sp "state start"
        Write-Output ("*** DRIVE ON  dp0=$lvl  ${OnSec}s ***  " + (($rs -replace '\s+', ' ').Trim()))

        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $lastKeepalive = 0
        $lastSample = 0
        $idx = 0
        while ($sw.ElapsedMilliseconds -lt ($OnSec * 1000)) {
            $now = $sw.ElapsedMilliseconds
            if (($now - $lastKeepalive) -ge 500) {
                Send-Only $Sp "diag bench keepalive"
                $lastKeepalive = $now
            }
            if (($now - $lastSample) -ge ($SampleEvery * 1000)) {
                $idx++
                $r = Send-Cmd $Sp "obs all"
                $lastSample = $sw.ElapsedMilliseconds
                $amps = if ($r -match "amps\s*:\s*i=(-?[0-9.]+)/(-?[0-9.]+)/(-?[0-9.]+)") { "$($Matches[1])/$($Matches[2])/$($Matches[3])" } else { "?" }
                $vdq = if ($r -match "vdq=(-?[0-9.]+)/(-?[0-9.]+)") { "$($Matches[1])/$($Matches[2])" } else { "?" }
                $vbus = if ($r -match "vbus=([0-9.]+)") { $Matches[1] } else { "?" }
                Write-Output ("    [ON {0,5}s] sample={1} vdq={2} amps={3} vbus={4}" -f ($now / 1000), $idx, $vdq, $amps, $vbus)
            }
            Start-Sleep -Milliseconds 50
        }

        [Console]::Beep(440, 250)
        $rd = Send-Cmd $Sp "state disable"
        Write-Output ("*** DRIVE OFF ***  " + (($rd -replace '\s+', ' ').Trim()))

        $rt = Send-Cmd $Sp "diag trace show"
        foreach ($line in ($rt -split "`r?`n")) {
            if ($line -match "trace|foc|duty|angle|speed") { Write-Output ("    " + $line.Trim()) }
        }
        [void](Send-Cmd $Sp "fault clear")
    }

    Write-Output ""
    Write-Output "=== 全部周期结束，驱动已断开 ==="
}
finally {
    if ($sp.IsOpen) { $sp.Close() }
}
