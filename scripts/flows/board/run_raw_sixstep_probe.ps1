param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 115200,
    [ValidateRange(1, 28)][int]$HoldSeconds = 28
)

$runner = Join-Path $PSScriptRoot "run_fixed_duty_probe.ps1"
& $runner -PortName $PortName -BaudRate $BaudRate -HoldSeconds $HoldSeconds `
    -TestName "RAW_SIXSTEP" -OutputDescription "electrical_hz=1 duty_span=0.40/0.60"
exit $LASTEXITCODE
