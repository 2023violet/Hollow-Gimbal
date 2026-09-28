param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 115200,
    [ValidateRange(1, 28)][int]$HoldSeconds = 28
)

$runner = Join-Path $PSScriptRoot "run_fixed_duty_probe.ps1"
& $runner -PortName $PortName -BaudRate $BaudRate -HoldSeconds $HoldSeconds `
    -TestName "RAW_SINE_MATCH_SIXSTEP" `
    -OutputDescription "electrical_hz=1 duty_center=0.50 duty_amplitude=0.115470 matched_vector=raw_sixstep" `
    -ObservationCommand "obs all"
exit $LASTEXITCODE
