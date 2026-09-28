param(
    [Parameter(Mandatory = $true)][string]$RepoRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptPath = Join-Path (Resolve-Path -LiteralPath $RepoRoot).Path "scripts\flows\board\run_openloop_probe.ps1"
if (-not (Test-Path -LiteralPath $scriptPath)) {
    throw "SpeedStep runner not found: $scriptPath"
}

$tokens = $null
$parseErrors = $null
[void][System.Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) {
    throw "SpeedStep runner must remain valid PowerShell."
}

$scriptText = Get-Content -LiteralPath $scriptPath -Raw
foreach ($token in @(
        '$script:BenchAdcTriggerDefault = 2700',
        '("diag set adc_trig {0}" -f $script:BenchAdcTriggerDefault)',
        'restore ADC map0/trigger2700'
    )) {
    if ($scriptText -notmatch [regex]::Escape($token)) {
        throw "SpeedStep runner is missing the validated ADC trigger default: $token"
    }
}

foreach ($token in @(
        '"SpeedStep"',
        '^diag velstep -?[0-9]+(?:\.[0-9]+)?$',
        'function Invoke-SpeedStepStage',
        'function Invoke-SpeedStep',
        '"diag enc setabs 1 0.0"',
        '"diag adc show"',
        '"diag set if 0.50 {0}"',
        '"diag if2vel {0}.0"',
        '@(30, 60, 90)',
        '$repeat -le 3',
        '$sampleDue = $stageStart.AddMilliseconds(($sampleIndex - 1) * 200)',
        'Get-ReplyPhaseCurrent',
        '"sys status"',
        'queue_used,queue_depth',
        'ia_peak,ib_peak,ic_peak,vbus_min,vbus_max',
        'Start-Sleep -Seconds 60',
        'state disable',
        'speed-step-samples.csv',
        'speed-step-summary.csv',
        'speed-step-operator-notes.md',
        '${stamp}_adc_then_speed',
        'Invoke-AdcBaseline',
        'Invoke-AdcProof',
        'SpeedStepPeakAbortAmps = 1.4',
        '$sample.Vbus -lt 10.8',
        '$sample.Vbus -gt 13.2'
    )) {
    if ($scriptText -notmatch [regex]::Escape($token)) {
        throw "SpeedStep runner is missing required safety/contract token: $token"
    }
}

foreach ($token in @(
        '"AutonomousDiag"',
        'function Invoke-AutonomousDiag',
        'Invoke-SwdResetGate',
        'physical_power_cycle=false',
        'psu_telemetry=unavailable',
        'autonomous-manifest.json',
        'autonomous-result.json',
        'diag bench keepalive',
        'function Invoke-AdcZeroRepeat',
        'function Invoke-AutonomousAdcScan',
        'function Invoke-AutonomousPositionSweep',
        '$baselineCount = 20',
        '(0..5)',
        '@(512, 1024, 1536, 2048, 2560, 3072, 3584, 3740)',
        'source-facts.json',
        'adc-zero-repeat.csv',
        'adc-map-trigger.csv',
        'encoder-position.csv'
        'analyze_autonomous_bench.py',
        'analysis.json',
        'AutonomousDiag does not permit ResetMode=power',
        '$benchElfForOpenOcd = $BenchElf -replace ''\\'', ''/''',
        '(?:\[adc_raw\]|adc_raw:)',
        '-Command "diag adc zero" -MaxWaitMs 5000',
        '(?:\[amps\]|amps\s*:)',
        'Send-BenchKeepAlive -LogName $logName -NoWait',
        'Invoke-TriggerScan',
        'diag set adc_trig {0}'
        'Start-Sleep -Milliseconds $DurationMs'
    )) {
    if ($scriptText -notmatch [regex]::Escape($token)) {
        throw "AutonomousDiag runner is missing required no-operator contract token: $token"
    }
}

if ($scriptText -match 'Invoke-PowerCycleGate\s+-NextCase\s+\("ADC baseline') {
    throw "ADC baseline must not require a physical power-cycle gate."
}

$triggerScanBody = [regex]::Match(
    $scriptText,
    '(?s)function Invoke-TriggerScan \{(.*?)function Invoke-ViLinearity \{'
)
if ((-not $triggerScanBody.Success) -or
    ($triggerScanBody.Groups[1].Value -notmatch [regex]::Escape('diag set adc_trig {0}'))) {
    throw "TriggerScan must apply each requested ADC trigger before its pulse."
}

$speedStepBody = [regex]::Match(
    $scriptText,
    '(?s)function Invoke-SpeedStep \{(.*?)function Invoke-IfDrag \{'
)
if ((-not $speedStepBody.Success) -or
    ($speedStepBody.Groups[1].Value -notmatch [regex]::Escape('Resolve-IdleForNextCase -NextCase "SpeedStep ADC baseline"'))) {
    throw "SpeedStep must restore an idle board through the configured automatic gate before ADC preflight."
}

Write-Output "SPEED_STEP_PROBE_CONTRACT=PASS"
