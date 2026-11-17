param([string]$RepoRoot = "")

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"
if ($RepoRoot -eq "") {
    $RepoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
}
$gate = Join-Path $RepoRoot "scripts\gates\check_themis_quality_gate.ps1"
$quality = Join-Path $RepoRoot "scripts\gates\check_cmake_quality.ps1"
$utf8 = [Text.UTF8Encoding]::new($false)
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ("falconfoc_quality_gate_" + [Guid]::NewGuid().ToString("N"))
$script:assertions = 0

function Assert-True {
    param([bool]$Condition, [string]$Message)
    $script:assertions++
    if (-not $Condition) { throw $Message }
}

function Read-Function {
    param([string]$Path, [string]$Name)
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
    if ($errors.Count -ne 0) { throw "Cannot parse $Path" }
    $found = $ast.Find({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $Name
    }, $true)
    if ($null -eq $found) { throw "Missing production function $Name" }
    return $found.Extent.Text
}

# Exercise the real function body without executing the gate's top-level build flow.
. ([scriptblock]::Create((Read-Function $gate "Invoke-ChildScript")))
$savedEnvironment = @{}
foreach ($name in @("CI", "GITHUB_ACTIONS", "QG_CAPTURE", "QG_FORMAT_TRACE", "QG_FORMAT_COMMAND", "QG_FORMAT_MODE", "QG_FAIL_STAGE")) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
}

try {
    New-Item -ItemType Directory -Path $testRoot | Out-Null
    $child = Join-Path $testRoot "child script with spaces.ps1"
    $receipt = Join-Path $testRoot "received parameters.json"
    [IO.File]::WriteAllText($child, @'
param([string]$RepoRoot, [switch]$RequireFormatter, [string]$SkipFormatter = "", [string]$ExitCode = "0")
$received = @{ Root = $RepoRoot; Required = [bool]$RequireFormatter; Skip = $SkipFormatter;
    SwitchType = $RequireFormatter.GetType().FullName; StringType = $SkipFormatter.GetType().FullName }
# A successful child must not leak the last native probe's exit 37 to its parent.
& $env:ComSpec /d /c exit 37
[IO.File]::WriteAllText($env:QG_CAPTURE, ($received | ConvertTo-Json -Compress))
exit ([int]$ExitCode)
'@, $utf8)
    $env:QG_CAPTURE = $receipt
    $spaced = Join-Path $testRoot "path containing spaces"
    Invoke-ChildScript $child @{ RepoRoot = $spaced; SkipFormatter = "true" }
    $value = Get-Content -LiteralPath $receipt -Raw | ConvertFrom-Json
    Assert-True (-not $value.Required -and $value.Root -eq $spaced) "Absent switch or spaced string was corrupted."
    Assert-True ($value.Skip -ceq "true" -and $value.StringType -eq "System.String") "String=true must remain a string."
    Assert-True ($LASTEXITCODE -eq 0) "Successful child leaked its internal native exit code."
    Invoke-ChildScript $child @{ RepoRoot = $spaced; RequireFormatter = $true; SkipFormatter = "true" }
    $value = Get-Content -LiteralPath $receipt -Raw | ConvertFrom-Json
    Assert-True ($value.Required -and $value.SwitchType -eq "System.Management.Automation.SwitchParameter") "True switch was not bound as a switch."
    Invoke-ChildScript $child @{ RepoRoot = $spaced; RequireFormatter = $false; SkipFormatter = "false" }
    $value = Get-Content -LiteralPath $receipt -Raw | ConvertFrom-Json
    Assert-True (-not $value.Required -and $value.Skip -ceq "false") "False switch or string=false was corrupted."
    $failed = $false
    try { Invoke-ChildScript $child @{ RepoRoot = $spaced; ExitCode = "7" } } catch { $failed = $true }
    Assert-True ($failed -and $LASTEXITCODE -eq 7) "Nonzero child exit was swallowed."
    Assert-True ($ErrorActionPreference -eq "Stop") "Child call did not restore error preference."

    # Keep the real gate, wrappers, scope/CI policy and formatter error handling.
    # Replace only expensive leaf checks and external formatter availability.
    $fixture = Join-Path $testRoot "gate fixture with spaces"
    New-Item -ItemType Directory -Path (Join-Path $fixture "scripts\gates") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $fixture "tests\script") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $fixture "scripts\flows\host") -Force | Out-Null
    Copy-Item -LiteralPath $gate -Destination (Join-Path $fixture "scripts\gates\check_themis_quality_gate.ps1")
    Copy-Item -LiteralPath (Join-Path $RepoRoot "scripts\check_themis_quality_gate.ps1") -Destination (Join-Path $fixture "scripts\check_themis_quality_gate.ps1")
    Copy-Item -LiteralPath (Join-Path $RepoRoot "scripts\check_cmake_quality.ps1") -Destination (Join-Path $fixture "scripts\check_cmake_quality.ps1")
    [IO.File]::WriteAllText((Join-Path $fixture "CMakePresets.json"), "{}", $utf8)
    & git init --quiet $fixture
    if ($LASTEXITCODE -ne 0) { throw "Cannot initialize isolated gate fixture." }
    $recorder = @'
function Write-Receipt {
    param([hashtable]$Value)
    [IO.File]::AppendAllText($env:QG_CAPTURE, ($Value | ConvertTo-Json -Compress) + "`n", [Text.UTF8Encoding]::new($false))
}
'@
    $leaves = @{
        "scripts\gates\check_falconfoc_source_contract.ps1" = "source"
        "scripts\gates\check_cmake_presets.ps1" = "presets"
        "scripts\gates\check_platform_backend_contract.ps1" = "platform"
        "scripts\gates\check_cmake_smoke.ps1" = "smoke"
        "tests\script\test_firmware_cli_contract.ps1" = "firmware-cli"
        "tests\script\test_cmake_cache_guard.ps1" = "cache"
        "scripts\gates\check_transport_shell_hardening_contract.ps1" = "transport"
        "tests\script\test_falconfoc_current_contract.ps1" = "current"
        "scripts\flows\host\run_host_tests.ps1" = "host"
    }
    foreach ($path in $leaves.Keys) {
        $body = 'param([string]$RepoRoot)' + "`n" + $recorder + "`n" + @'
Write-Receipt @{ Step = "@STEP@"; Root = $RepoRoot }
if ($env:QG_FAIL_STAGE -eq "@STEP@") { exit 7 }
exit 0
'@.Replace("@STEP@", $leaves[$path])
        [IO.File]::WriteAllText((Join-Path $fixture $path), $body, $utf8)
    }
    $definitions = (Read-Function $quality "Invoke-ResolvedCommand") + "`n" + (Read-Function $quality "Invoke-CMakeFormatCheck")
    $body = 'param([string]$RepoRoot, [switch]$RequireFormatter, [string]$SkipFormatter = "")' + "`n" + $recorder + "`n" + $definitions + "`n" + @'
function Get-CMakeFormatCommand {
    if ($env:QG_FORMAT_MODE -eq "missing") { return $null }
    return @($env:QG_FORMAT_COMMAND)
}
Write-Receipt @{ Step = "quality"; Required = [bool]$RequireFormatter; Skip = $SkipFormatter;
    Root = $RepoRoot; SwitchType = $RequireFormatter.GetType().FullName; StringType = $SkipFormatter.GetType().FullName }
if ($SkipFormatter -notin @("1", "true", "True", "TRUE")) {
    Invoke-CMakeFormatCheck -Files @((Join-Path $RepoRoot "sample file.cmake")) -FormatterRequired ([bool]$RequireFormatter) -ConfigPath (Join-Path $RepoRoot "format.py")
}
exit 0
'@
    [IO.File]::WriteAllText((Join-Path $fixture "scripts\gates\check_cmake_quality.ps1"), $body, $utf8)
    [IO.File]::WriteAllText((Join-Path $fixture "sample file.cmake"), "set(TEST ON)`n", $utf8)
    [IO.File]::WriteAllText((Join-Path $fixture "format.py"), "# temporary formatter configuration`n", $utf8)
    $env:QG_FORMAT_COMMAND = Join-Path $testRoot "formatter fixture.cmd"
    [IO.File]::WriteAllText($env:QG_FORMAT_COMMAND, "@echo off`r`n>>`"%QG_FORMAT_TRACE%`" echo %*`r`nif `"%QG_FORMAT_MODE%`"==`"fail`" exit /b 9`r`nexit /b 0`r`n", [Text.Encoding]::ASCII)
    $env:QG_CAPTURE = Join-Path $testRoot "gate receipts.jsonl"
    $env:QG_FORMAT_TRACE = Join-Path $testRoot "formatter calls.txt"

    function Invoke-GateCase {
        param([string]$Name, [string]$Scope = "all", [string[]]$Extra = @(), [string]$Ci = "", [string]$Actions = "", [string]$Formatter = "ok", [string]$FailStage = "")
        $env:CI = $Ci
        $env:GITHUB_ACTIONS = $Actions
        $env:QG_FORMAT_MODE = $Formatter
        $env:QG_FAIL_STAGE = $FailStage
        [IO.File]::WriteAllText($env:QG_CAPTURE, "", $utf8)
        [IO.File]::WriteAllText($env:QG_FORMAT_TRACE, "", $utf8)
        $invoke = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $fixture "scripts\check_themis_quality_gate.ps1"), "-RepoRoot", $fixture, "-Scope", $Scope) + $Extra
        $previous = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try { $output = (& powershell @invoke 2>&1 | Out-String); $code = $LASTEXITCODE }
        finally { $ErrorActionPreference = $previous }
        $rows = @(Get-Content -LiteralPath $env:QG_CAPTURE | Where-Object { $_ -ne "" } | ForEach-Object { $_ | ConvertFrom-Json })
        Write-Host ("[quality_gate_arguments] case={0} exit={1}" -f $Name, $code)
        return @{ Exit = $code; Output = $output; Rows = $rows; Format = [IO.File]::ReadAllText($env:QG_FORMAT_TRACE) }
    }

    foreach ($mode in @("ordinary", "strict", "ci", "actions")) {
        $extra = @("-Summary")
        if ($mode -eq "strict") { $extra += "-RequireCMakeFormatter" }
        $ci = if ($mode -eq "ci") { "true" } else { "" }
        $actions = if ($mode -eq "actions") { "true" } else { "" }
        $result = Invoke-GateCase $mode -Extra $extra -Ci $ci -Actions $actions
        $received = @($result.Rows | Where-Object { $_.Step -eq "quality" })
        Assert-True ($result.Exit -eq 0 -and $received.Count -eq 1) "$mode did not reach the real child invocation."
        Assert-True ($received[0].Required -eq ($mode -ne "ordinary")) "$mode strict policy was not bound correctly."
        Assert-True ($result.Format.Contains("--check") -and $result.Output.Contains("[Total]   PASS")) "$mode skipped formatter execution or summary."
        Assert-True (($result.Rows.Step -join ",") -eq "source,quality,presets,platform,smoke,firmware-cli,cache,transport,current,host") "$mode changed full-gate ordering or skipped checks."
    }
    $result = Invoke-GateCase "falconfoc" -Scope "falconfoc"
    $received = @($result.Rows | Where-Object { $_.Step -eq "quality" })[0]
    Assert-True ($result.Exit -eq 0 -and $received.Skip -ceq "true" -and $received.StringType -eq "System.String") "Scoped SkipFormatter was not a true string."
    Assert-True ($result.Format -eq "" -and ($result.Rows.Step -join ",") -eq "source,quality,presets,platform,smoke,cache,transport") "FalconFOC scope expanded into full checks."
    $result = Invoke-GateCase "missing" -Extra @("-RequireCMakeFormatter") -Formatter "missing"
    Assert-True ($result.Exit -ne 0 -and $result.Output.Contains("cmake-format was not found") -and $result.Format -eq "") "Missing formatter was not rejected as a dependency failure."
    Assert-True (($result.Rows.Step -join ",") -eq "source,quality") "Missing formatter continued to downstream gates."
    $result = Invoke-GateCase "bad-format" -Extra @("-RequireCMakeFormatter") -Formatter "fail"
    Assert-True ($result.Exit -ne 0 -and $result.Output.Contains("cmake-format --check failed") -and $result.Format.Contains("--check")) "Formatter failure was swallowed or never executed."
    Assert-True (($result.Rows.Step -join ",") -eq "source,quality") "Bad format continued to downstream gates."
    $result = Invoke-GateCase "other-child-failure" -Extra @("-RequireCMakeFormatter") -FailStage "platform"
    Assert-True ($result.Exit -ne 0 -and $result.Format.Contains("--check")) "Other gate failure was swallowed."
    Assert-True (($result.Rows.Step -join ",") -eq "source,quality,presets,platform") "Failure did not stop the gate chain."
    Write-Host ("[quality_gate_arguments] PASS assertions={0}" -f $script:assertions)
} finally {
    foreach ($name in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], "Process")
    }
    # Only remove this invocation's GUID fixture, never a repository/build tree.
    $resolved = [IO.Path]::GetFullPath($testRoot)
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($temp, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolved) -notmatch '^falconfoc_quality_gate_[0-9a-f]{32}$') {
        throw "Refusing to remove a fixture outside the private temp root."
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
exit 0
