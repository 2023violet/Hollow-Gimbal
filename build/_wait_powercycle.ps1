# Poll COM3 until the board disappears (power off) and reappears (power on).
param([int]$TimeoutSec = 120)
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$sawDown = $false
while ((Get-Date) -lt $deadline) {
    $reply = ""
    try {
        $sp = New-Object System.IO.Ports.SerialPort 'COM3', 115200, 'None', 8, 'One'
        $sp.ReadTimeout = 300
        $sp.Open()
        Start-Sleep -Milliseconds 200
        $sp.DiscardInBuffer()
        $sp.Write("sys status`r`n")
        Start-Sleep -Milliseconds 500
        $reply = $sp.ReadExisting()
        $sp.Close()
    } catch {
        $sawDown = $true
        Write-Output "port unavailable (power off?)"
        Start-Sleep -Milliseconds 900
        continue
    }
    if ($reply -match 'cmd=status') {
        if ($sawDown) {
            Write-Output "POWER-CYCLE DETECTED - board back online"
            exit 0
        }
        Write-Output "board alive"
    } else {
        $sawDown = $true
        Write-Output "board silent (power off?)"
    }
    Start-Sleep -Milliseconds 1200
}
if ($sawDown) { Write-Output "TIMEOUT: board still down after ${TimeoutSec}s" }
else { Write-Output "TIMEOUT: no power cycle observed in ${TimeoutSec}s" }
exit 1
