# FalconFoc shell serial probe
#
# Minimal interactive probe for the shared USART1 console lane.
# Sends the given shell commands sequentially and prints each reply.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/board/run_shell_serial_probe.ps1 `
#     -PortName COM3 -BaudRate 115200 -Commands @('help', 'sys status')
#
# Notes:
# - Use CRLF; the AT32F415 DMA/IDLE console path is exercised reliably with
#   the same line terminator used by the board witness script.
# - Read-only commands are always safe. Any command that can energize the
#   axis (`state start`, `diag set ...`, `cal ...`) must only be used with
#   the bench supply in current-limit mode.

param(
    [Parameter(Mandatory = $true)][string]$PortName,
    [int]$BaudRate = 115200,
    [string[]]$Commands = @(),
    [int]$QuietMs = 300,
    [int]$MaxWaitMs = 1500,
    [int]$OpenDrainMs = 300
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

function Read-UntilQuiet {
    param($Sp, [int]$QuietMs, [int]$MaxWaitMs)

    $sb = New-Object System.Text.StringBuilder
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $lastRx = $sw.ElapsedMilliseconds
    $receivedAny = $false

    while ($sw.ElapsedMilliseconds -lt $MaxWaitMs) {
        if ($sp.BytesToRead -gt 0) {
            $count = $sp.BytesToRead
            $buf = New-Object byte[] $count
            [void]$sp.Read($buf, 0, $count)
            [void]$sb.Append([System.Text.Encoding]::ASCII.GetString($buf))
            $lastRx = $sw.ElapsedMilliseconds
            $receivedAny = $true
        }
        if ($receivedAny -and (($sw.ElapsedMilliseconds - $lastRx) -gt $QuietMs)) { break }
        Start-Sleep -Milliseconds 10
    }

    return $sb.ToString()
}

$sp = New-Object System.IO.Ports.SerialPort $PortName, $BaudRate, "None", 8, "One"
$sp.ReadTimeout = 50
$sp.WriteTimeout = 500
$sp.DtrEnable = $false
$sp.RtsEnable = $false

try {
    $sp.Open()
    Start-Sleep -Milliseconds $OpenDrainMs
    if ($sp.BytesToRead -gt 0) { [void]$sp.ReadExisting() }

    Write-Output "=== PORT $PortName @$BaudRate ==="
    foreach ($cmd in $Commands) {
        $sp.Write("$cmd`r`n")
        Start-Sleep -Milliseconds 300
        $reply = Read-UntilQuiet -Sp $sp -QuietMs $QuietMs -MaxWaitMs $MaxWaitMs
        Write-Output "--- TX: $cmd"
        Write-Output $reply
    }
}
finally {
    if ($sp.IsOpen) { $sp.Close() }
}
