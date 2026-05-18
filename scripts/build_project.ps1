param(
    [string]$RepoRoot = "",
    [string]$ProjectFile = "work/foc_at32.ATWP",
    [string]$Uv4Path = "",
    [string]$TargetName = "",
    [switch]$Rebuild
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

function Resolve-RepoRootPath
{
    param([string]$Root)

    if ($Root -ne "") {
        return (Resolve-Path -LiteralPath $Root).Path
    }

    $resolved = & git rev-parse --show-toplevel
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to locate git repository root."
    }

    return $resolved.Trim()
}

function Resolve-ProjectPath
{
    param(
        [string]$ResolvedRepoRoot,
        [string]$RelativeOrAbsolutePath
    )

    if ([System.IO.Path]::IsPathRooted($RelativeOrAbsolutePath)) {
        return (Resolve-Path -LiteralPath $RelativeOrAbsolutePath).Path
    }

    return (Resolve-Path -LiteralPath (Join-Path $ResolvedRepoRoot $RelativeOrAbsolutePath)).Path
}

function Resolve-Uv4Executable
{
    param([string]$PreferredPath)

    if ($PreferredPath -ne "") {
        return (Resolve-Path -LiteralPath $PreferredPath).Path
    }

    foreach ($commandName in @("UV4.exe", "UV4")) {
        $command = Get-Command $commandName -ErrorAction SilentlyContinue
        if ($command -ne $null) {
            return $command.Source
        }
    }

    foreach ($candidate in @(
                 "C:\\Keil_v5\\UV4\\UV4.exe",
                 "C:\\Program Files\\Keil_v5\\UV4\\UV4.exe",
                 "C:\\Program Files (x86)\\Keil_v5\\UV4\\UV4.exe"
             )) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    throw "UV4.exe was not found. Pass -Uv4Path explicitly."
}

function Resolve-TargetNameFromProject
{
    param(
        [string]$ResolvedProjectFile,
        [string]$PreferredTargetName
    )

    [xml]$projectXml = Get-Content -LiteralPath $ResolvedProjectFile

    if ($PreferredTargetName -ne "") {
        return $PreferredTargetName
    }

    $targetNode = $projectXml.SelectSingleNode("//Target/TargetName")
    if (($targetNode -ne $null) -and (-not [string]::IsNullOrWhiteSpace($targetNode.InnerText))) {
        return $targetNode.InnerText.Trim()
    }

    $projectNameNode = $projectXml.SelectSingleNode("//ProjectInfomation/ProjectName")
    if (($projectNameNode -eq $null) -or [string]::IsNullOrWhiteSpace($projectNameNode.InnerText)) {
        return ""
    }

    return $projectNameNode.InnerText.Trim()
}

$resolvedRepoRoot   = Resolve-RepoRootPath -Root $RepoRoot
$resolvedProject    = Resolve-ProjectPath -ResolvedRepoRoot $resolvedRepoRoot `
                                          -RelativeOrAbsolutePath $ProjectFile
$resolvedUv4        = Resolve-Uv4Executable -PreferredPath $Uv4Path
$resolvedTargetName = Resolve-TargetNameFromProject -ResolvedProjectFile $resolvedProject `
                                                    -PreferredTargetName $TargetName
$logPath            = Join-Path $env:TEMP ("themis_keil_build_{0}.log" -f
                        [DateTime]::Now.ToString("yyyyMMdd_HHmmss"))
$modeSwitch         = if ($Rebuild) { "-r" } else { "-b" }
$argumentList       = @(
    "-j0",
    $modeSwitch,
    $resolvedProject
)
if (-not [string]::IsNullOrWhiteSpace($resolvedTargetName)) {
    $argumentList += @("-t", $resolvedTargetName)
}
$argumentList += @("-o", $logPath)

Write-Host "[build_project] UV4: $resolvedUv4"
Write-Host "[build_project] Project: $resolvedProject"
Write-Host "[build_project] Target: $resolvedTargetName"
Write-Host "[build_project] Log: $logPath"

$process = Start-Process -FilePath $resolvedUv4 `
                         -ArgumentList $argumentList `
                         -WorkingDirectory (Split-Path -Parent $resolvedProject) `
                         -Wait `
                         -PassThru `
                         -NoNewWindow

$summaryLine = $null
if (Test-Path -LiteralPath $logPath) {
    $summaryLine = Select-String -LiteralPath $logPath `
                                 -Pattern "(\d+)\s+Error\(s\),\s+(\d+)\s+Warning\(s\)" |
        Select-Object -Last 1

    if ($summaryLine -ne $null) {
        Write-Host ("[build_project] " + $summaryLine.Matches[0].Value)
    } else {
        Get-Content -LiteralPath $logPath | Select-Object -Last 12 |
            ForEach-Object { Write-Host ("[build_project] " + $_) }
    }
}

if ($process.ExitCode -ne 0) {
    throw "Keil build failed with exit code $($process.ExitCode). See '$logPath'."
}

if ($summaryLine -ne $null) {
    $errorCount = [int]$summaryLine.Matches[0].Groups[1].Value
    if ($errorCount -ne 0) {
        throw "Keil build reported $errorCount error(s). See '$logPath'."
    }
}

Write-Host "[build_project] build succeeded."
