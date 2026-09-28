# Fixed 40/50/60 percent PWM hardware-isolation runner.
#
# This script is valid only with the dedicated fixed-duty bench ELF. It starts
# one bounded run, keeps the bench watchdog alive, and deliberately does not
# send a software stop. The board's 30 s energy limit remains the stop path.

param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 115200,
    [ValidateRange(1, 28)][int]$HoldSeconds = 28,
    [string]$TestName = "FIXED_DUTY",
    [string]$OutputDescription = "duty=0.40/0.50/0.60",
    [string]$ObservationCommand = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Read-UntilQuiet {
    param(
        [Parameter(Mandatory = $true)]$Serial,
        [int]$MaxWaitMs = 900
    )

    $text = New-Object System.Text.StringBuilder
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $lastRxMs = 0L
    $received = $false
    while ($timer.ElapsedMilliseconds -lt $MaxWaitMs) {
        if ($Serial.BytesToRead -gt 0) {
            [void]$text.Append($Serial.ReadExisting())
            $lastRxMs = $timer.ElapsedMilliseconds
            $received = $true
        }
        if ($received -and (($timer.ElapsedMilliseconds - $lastRxMs) -gt 100)) {
            break
        }
        Start-Sleep -Milliseconds 10
    }
    return $text.ToString()
}

function Send-BoardCommand {
    param(
        [Parameter(Mandatory = $true)]$Serial,
        [Parameter(Mandatory = $true)][string]$Command,
        [int]$MaxWaitMs = 900
    )

    $Serial.Write($Command + "`r`n")
    return Read-UntilQuiet -Serial $Serial -MaxWaitMs $MaxWaitMs
}

$serial = New-Object System.IO.Ports.SerialPort $PortName, $BaudRate, "None", 8, "One"
$serial.ReadTimeout = 50
$serial.WriteTimeout = 500
$serial.DtrEnable = $false
$serial.RtsEnable = $false

try {
    $serial.Open()
    Start-Sleep -Milliseconds 300
    if ($serial.BytesToRead -gt 0) { [void]$serial.ReadExisting() }

    [void](Send-BoardCommand -Serial $serial -Command "sys reboot")
    Start-Sleep -Seconds 2
    if ($serial.BytesToRead -gt 0) { [void]$serial.ReadExisting() }
    $state = Send-BoardCommand -Serial $serial -Command "state show"
    if ($state -notmatch 'axis=idle') {
        throw "Board is not idle. Reset it over SWD or power-cycle before the fixed-duty test."
    }
    $adc = Send-BoardCommand -Serial $serial -Command "diag adc show"
    if ($adc -notmatch 'valid=1') {
        throw "ADC calibration is not valid; fixed-duty PWM was not started."
    }
    $velmax = Send-BoardCommand -Serial $serial -Command "diag param velmax 1000000"
    if ($velmax -notmatch 'result=ok') {
        throw "Could not suppress detached-encoder over-velocity protection: $velmax"
    }
    $start = Send-BoardCommand -Serial $serial -Command "state start"
    if ($start -notmatch 'cmd=start result=ok') {
        throw "Fixed-duty bench image did not accept state start: $start"
    }

    Write-Output "$TestName`_STARTED $OutputDescription hold_seconds=$HoldSeconds"
    $deadline = [DateTime]::UtcNow.AddSeconds($HoldSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $heartbeat = Send-BoardCommand -Serial $serial -Command "diag bench keepalive" -MaxWaitMs 600
        if ($heartbeat -notmatch 'result=ok') {
            throw "Bench keepalive was rejected: $heartbeat"
        }
        if (-not [string]::IsNullOrWhiteSpace($ObservationCommand)) {
            $observation = Send-BoardCommand -Serial $serial -Command $ObservationCommand -MaxWaitMs 600
            if ($observation -notmatch 'result=ok') {
                throw "Bench observation was rejected: $observation"
            }
            Write-Output "$TestName`_OBS $observation"
        }
        Start-Sleep -Milliseconds 450
    }
    Write-Output "$TestName`_HOLD_COMPLETE software_stop=not_sent"
}
finally {
    if ($serial.IsOpen) { $serial.Close() }
}
