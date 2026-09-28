# Watch VBUS over the shell while the user power-cycles the 12 V rail.
# Detects the drop (vbus near 0 while MCU stays alive on debugger 3.3 V) and
# the recovery, then exits so the caller can proceed.
param([int]$TimeoutSec = 240)
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$sawLow = $false
$sawHigh = $false
while ((Get-Date) -lt $deadline) {
    $reply = ""
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
    } catch {
        Write-Output "port error"
        Start-Sleep -Milliseconds 800
        continue
    }
    $vbus = if ($reply -match 'vbus=([0-9.]+)') { [double]$Matches[1] } else { -1.0 }
    $axis = if ($reply -match 'axis=(\w+)') { $Matches[1] } else { '?' }
    if (($vbus -ge 0) -and ($vbus -lt 3.0)) {
        if (-not $sawLow) { Write-Output "VBUS LOW ($vbus V) - 12 V dropped, clearing DRV latch" }
        $sawLow = $true
    } elseif ($vbus -gt 8.0) {
        if ($sawLow -and -not $sawHigh) {
            Write-Output "VBUS RESTORED ($vbus V) - latch cleared, proceeding"
            $sawHigh = $true
            exit 0
        }
        if (-not $sawLow) { Write-Output "vbus=$vbus axis=$axis (waiting for power cycle)" }
    }
    Start-Sleep -Milliseconds 700
}
Write-Output "TIMEOUT after ${TimeoutSec}s (sawLow=$sawLow)"
exit 1
