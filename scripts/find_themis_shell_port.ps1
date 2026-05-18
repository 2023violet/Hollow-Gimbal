param(
    [string[]]$CandidatePorts = @(),

    [string]$ExpectedProfile = "tiny",

    [string]$ExpectedFlavor = "carrier-demo",

    [int]$BaudRate = 1000000,

    [int]$InitialDrainMs = 1200,

    [int]$ReadSliceMs = 100,

    [int]$QuietWindowMs = 400,

    [int]$ProbeTimeoutMs = 2500,

    [int]$WriteTimeoutMs = 1200
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-CandidatePortList
{
    param([string[]]$Ports)

    if ($Ports.Count -gt 0) {
        return $Ports | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique
    }

    return [System.IO.Ports.SerialPort]::GetPortNames() |
        Sort-Object {
            if ($_ -match "^COM([0-9]+)$") {
                return [int]$matches[1]
            }

            return [int]::MaxValue
        }
}

function Read-SerialChunk
{
    param(
        [System.IO.Ports.SerialPort]$SerialPort,
        [System.Text.Encoding]$Encoding
    )

    if ($SerialPort.BytesToRead -le 0) {
        return ""
    }

    $buffer = New-Object byte[] $SerialPort.BytesToRead
    $read = $SerialPort.Read($buffer, 0, $buffer.Length)
    if ($read -le 0) {
        return ""
    }

    return $Encoding.GetString($buffer, 0, $read)
}

function Collect-SerialFor
{
    param(
        [System.IO.Ports.SerialPort]$SerialPort,
        [System.Text.Encoding]$Encoding,
        [int]$DurationMs
    )

    $captured = New-Object System.Text.StringBuilder
    $deadline = [DateTime]::UtcNow.AddMilliseconds($DurationMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        $chunk = Read-SerialChunk -SerialPort $SerialPort -Encoding $Encoding
        if (-not [string]::IsNullOrEmpty($chunk)) {
            [void]$captured.Append($chunk)
        }

        Start-Sleep -Milliseconds $ReadSliceMs
    }

    return $captured.ToString()
}

function Collect-SerialUntilQuiet
{
    param(
        [System.IO.Ports.SerialPort]$SerialPort,
        [System.Text.Encoding]$Encoding,
        [int]$TimeoutMs
    )

    $captured = New-Object System.Text.StringBuilder
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    $quietSince = [DateTime]::UtcNow

    while ([DateTime]::UtcNow -lt $deadline) {
        $chunk = Read-SerialChunk -SerialPort $SerialPort -Encoding $Encoding
        if (-not [string]::IsNullOrEmpty($chunk)) {
            [void]$captured.Append($chunk)
            $quietSince = [DateTime]::UtcNow
        } elseif (([DateTime]::UtcNow - $quietSince).TotalMilliseconds -ge $QuietWindowMs) {
            break
        }

        Start-Sleep -Milliseconds $ReadSliceMs
    }

    return $captured.ToString()
}

function Get-PreviewText
{
    param([string]$Text)

    if ([string]::IsNullOrWhiteSpace($Text)) {
        return "<empty>"
    }

    $preview = ($Text -replace "\s+", " ").Trim()
    if ($preview.Length -gt 160) {
        return $preview.Substring(0, 160) + "..."
    }

    return $preview
}

function Test-ThemisShellPort
{
    param(
        [string]$PortName,
        [System.Text.Encoding]$Encoding
    )

    $result = [ordered]@{
        PortName = $PortName
        Matched  = $false
        Reason   = ""
        Preview  = ""
    }

    $port = New-Object System.IO.Ports.SerialPort $PortName, $BaudRate, "None", 8, "One"
    $port.Encoding = $Encoding
    $port.NewLine = "`r`n"
    $port.ReadTimeout = 200
    $port.WriteTimeout = $WriteTimeoutMs
    $port.DtrEnable = $false
    $port.RtsEnable = $false

    try {
        $port.Open()
        Start-Sleep -Milliseconds 250

        $initialText = Collect-SerialFor -SerialPort $port -Encoding $Encoding -DurationMs $InitialDrainMs
        $statusText = ""
        $helpText = ""
        $captured = $initialText
        $result.Preview = Get-PreviewText -Text $captured

        $profilePattern = "profile\s*[:=]\s*" + [regex]::Escape($ExpectedProfile)
        $flavorPattern = "flavor\s*[:=]\s*" + [regex]::Escape($ExpectedFlavor)
        $shellPattern = "\[(shell|sys|help)\]"

        if (($captured -match $profilePattern) -and ($captured -match $flavorPattern) -and
            ($captured -match $shellPattern)) {
            $result.Matched = $true
            $result.Reason = "matched banner signature"
            return [pscustomobject]$result
        }

        $port.Write("help`r`n")
        $helpText = Collect-SerialUntilQuiet -SerialPort $port -Encoding $Encoding -TimeoutMs $ProbeTimeoutMs
        $captured = $initialText + $helpText

        $port.Write("sys status`r`n")
        $statusText = Collect-SerialUntilQuiet -SerialPort $port -Encoding $Encoding -TimeoutMs $ProbeTimeoutMs

        $captured = $initialText + $helpText + $statusText
        $result.Preview = Get-PreviewText -Text $captured

        if (($captured -match $profilePattern) -and ($captured -match $flavorPattern) -and
            ($captured -match "\[sys\]\s*cmd=status")) {
            $result.Matched = $true
            $result.Reason = "matched sys status signature"
        } elseif (($helpText -match "\[help\]") -or ($statusText -match "\[sys\]")) {
            $result.Reason = "shell output seen, but profile/flavor mismatch"
        } else {
            $result.Reason = "no Themis shell status signature"
        }
    }
    catch {
        $result.Reason = $_.Exception.Message
    }
    finally {
        if ($port.IsOpen) {
            $port.Close()
        }
    }

    return [pscustomobject]$result
}

$ports = @(Get-CandidatePortList -Ports $CandidatePorts)
if ($ports.Count -eq 0) {
    throw "No serial ports were found on this machine."
}

$encoding = [System.Text.UTF8Encoding]::new($false)
$failures = New-Object System.Collections.Generic.List[string]

foreach ($portName in $ports) {
    $probe = Test-ThemisShellPort -PortName $portName -Encoding $encoding
    if ($probe.Matched) {
        Write-Output $probe.PortName
        exit 0
    }

    $failures.Add(("{0}: {1}; preview={2}" -f $probe.PortName, $probe.Reason, $probe.Preview))
}

$failureSummary = $failures -join [Environment]::NewLine
throw ("No Themis shell port matched expected profile='{0}' flavor='{1}'.{2}{3}" -f
    $ExpectedProfile,
    $ExpectedFlavor,
    [Environment]::NewLine,
    $failureSummary)
