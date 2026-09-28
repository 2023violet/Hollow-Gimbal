param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 115200,
    [ValidateRange(1, 20)][int]$HoldSeconds = 20
)

$runner = Join-Path $PSScriptRoot "run_fixed_duty_probe.ps1"
& $runner -PortName $PortName -BaudRate $BaudRate -HoldSeconds $HoldSeconds `
    -TestName "RAW_SINE_20" `
    -OutputDescription "electrical_hz=1 duty_center=0.50 duty_amplitude=0.20" `
    -ObservationCommand "obs all"
exit $LASTEXITCODE
