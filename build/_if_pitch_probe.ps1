# Measures the true pole-pair count from the speed ratio of a very low speed
# I-f pull. At these electrical speeds the required voltage is only I*R, so the
# rotor cannot fall out of synchronism through the duty clamp, and
#   pole_pairs = we_cmd / (2*pi * rev_per_second)
# comes straight out of the encoder's mechanical turn counter.
#
# Usage: _if_pitch_probe.ps1 -WeValues "2,8" [-Seconds 24] [-Current 0.71]
param(
    [string]$WeValues = "2,8",
    [double]$Current = 0.71,
    [int]$Seconds = 24,
    [double]$SampleEvery = 1.0,
    [int]$LeadInSec = 4,
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

$results = @()

try {
    $sp.Open()
    Start-Sleep -Milliseconds 300
    [void]$sp.ReadExisting()

    Write-Output "=== I-F PITCH PROBE  I=$Current A  we='$WeValues'  ${Seconds}s each ==="
    Write-Output "TONE: long 880 Hz = DRIVE ON, short 440 Hz = DRIVE OFF"

    foreach ($we in ($WeValues -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })) {
        [void](Send-Cmd $Sp "state disable")
        [void](Send-Cmd $Sp "fault clear")
        $rs = Send-Cmd $Sp "diag set if $Current $we"
        Write-Output ("--- setup we=$we : " + (($rs -replace '\s+', ' ').Trim()))
        [void](Send-Cmd $Sp "diag trace clear")

        Write-Output ""
        Write-Output (">>> {0}s 后加驱（转子将缓慢旋转，离开轴线）<<<" -f $LeadInSec)
        Start-Sleep -Seconds $LeadInSec

        [Console]::Beep(880, 900)
        $rstart = Send-Cmd $Sp "state start"
        Write-Output ("*** DRIVE ON we=$we *** " + (($rstart -replace '\s+', ' ').Trim()))

        $pts = @()
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $lastKeepalive = 0
        $lastSample = 0
        while ($sw.ElapsedMilliseconds -lt ($Seconds * 1000)) {
            $now = $sw.ElapsedMilliseconds
            if (($now - $lastKeepalive) -ge 500) {
                $sp.Write("diag bench keepalive`r`n")
                $lastKeepalive = $now
            }
            if (($now - $lastSample) -ge ($SampleEvery * 1000)) {
                $lastSample = $now
                $r = Send-Cmd $Sp "state show"
                if ($r -match "pos_turn=(-?[0-9.]+)") {
                    $pts += [pscustomobject]@{ t = ($now / 1000.0); pos = [double]$Matches[1] }
                }
            }
            Start-Sleep -Milliseconds 50
        }

        [Console]::Beep(440, 250)
        [void](Send-Cmd $Sp "state disable")

        if ($pts.Count -lt 4) {
            Write-Output "!! 有效采样不足，跳过 this point"
            continue
        }

        # Least squares slope in turns/second.
        $n = $pts.Count
        $st = 0.0; $sp2 = 0.0; $spp = 0.0; $spv = 0.0
        foreach ($p in $pts) { $st += $p.t; $sp2 += $p.t * $p.t; $spp += $p.pos; $spv += $p.t * $p.pos }
        $slope = ($n * $spv - $st * $spp) / ($n * $sp2 - $st * $st)
        $revPerSec = [Math]::Abs($slope)
        $rpm = $revPerSec * 60.0
        $pp = if ($revPerSec -gt 1e-6) { [double]$we / (2 * [Math]::PI * $revPerSec) } else { 0 }
        $span = ($pts[-1].pos - $pts[0].pos)

        Write-Output ("    samples={0} first={1:N3} last={2:N3} span={3:N3} turn" -f $n, $pts[0].pos, $pts[-1].pos, $span)
        Write-Output ("    measured = {0:N2} rpm ({1:N4} rev/s)" -f $rpm, $revPerSec)
        Write-Output ("    *** pole_pairs inferred = {0:N2}  (config says 7) ***" -f $pp)

        $results += [pscustomobject]@{ we = [double]$we; rpm = $rpm; pp = $pp }
    }

    Write-Output ""
    Write-Output "=== 汇总 ==="
    foreach ($r in $results) {
        Write-Output ("  we={0,-5} -> {1,6:N2} rpm  => pp={2:N2}" -f $r.we, $r.rpm, $r.pp)
    }
    if ($results.Count -ge 2) {
        $avg = ($results | Measure-Object -Property pp -Average).Average
        Write-Output ("  线性度检查：两点 pp 差 = {0:N2}" -f ([Math]::Abs($results[0].pp - $results[-1].pp)))
        Write-Output ("  综合估计 pole_pairs ≈ {0:N2}" -f $avg)
    }
}
finally {
    if ($sp.IsOpen) { $sp.Close() }
}
