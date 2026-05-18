Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$canonicalScript = Join-Path $PSScriptRoot "flows\board\run_tiny_carrier_regression.ps1"
if (-not (Test-Path -LiteralPath $canonicalScript)) {
    throw "Canonical script not found: $canonicalScript"
}

& powershell -NoProfile -ExecutionPolicy Bypass -File $canonicalScript @args
exit $LASTEXITCODE