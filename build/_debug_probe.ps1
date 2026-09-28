# Temporary debug-round probe wrapper (delete after the debug round).
# $Cmds: comma-separated shell commands, e.g. "sys status,fault status"
param(
    [Parameter(Mandatory = $true)][string]$Cmds,
    [int]$QuietMs = 300,
    [int]$MaxWaitMs = 1500
)
$repo = "D:/study/Software/Hollow-Gimbal"
$list = $Cmds -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' }
& "$repo/scripts/flows/board/run_shell_serial_probe.ps1" -PortName COM3 -BaudRate 115200 -Commands $list -QuietMs $QuietMs -MaxWaitMs $MaxWaitMs
