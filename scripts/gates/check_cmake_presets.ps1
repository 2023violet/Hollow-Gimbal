param(
    [string]$RepoRoot = ""
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

function Resolve-AbsolutePath
{
    param([string]$Path)

    return (Resolve-Path -LiteralPath $Path).Path
}

function Resolve-ThemisPaths
{
    param([string]$Root)

    if ($Root -ne "") {
        $baseRoot = Resolve-AbsolutePath -Path $Root
    } else {
        $baseRoot = (& git rev-parse --show-toplevel).Trim()
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to locate git repository root."
        }
    }

        if ((-not (Test-Path -LiteralPath (Join-Path $baseRoot "scripts"))) -or
        (-not (Test-Path -LiteralPath (Join-Path $baseRoot "CMakePresets.json")))) {
        throw "Unable to resolve Themis repository root from '$baseRoot'."
    }

    $gitRoot = (& git -C $baseRoot rev-parse --show-toplevel).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to resolve git root for '$baseRoot'."
    }

    return @{
        GitRoot  = $gitRoot
        RepoRoot = $baseRoot
    }

}

function Assert-PresetExists
{
    param(
        [object[]]$PresetList,
        [string]$PresetName,
        [string]$Kind
    )

    $match = $PresetList | Where-Object { $_.name -eq $PresetName } | Select-Object -First 1
    if ($null -eq $match) {
        throw "Missing required $Kind preset: $PresetName"
    }

    return $match
}

function Get-PresetProperty
{
    param(
        [object[]]$PresetList,
        [object]$Preset,
        [string]$PropertyName
    )

    if ($null -eq $Preset) {
        return $null
    }

    if ($Preset.PSObject.Properties.Name -contains $PropertyName) {
        return $Preset.$PropertyName
    }

    if (-not ($Preset.PSObject.Properties.Name -contains "inherits")) {
        return $null
    }

    $inheritList = @($Preset.inherits)
    foreach ($parentName in $inheritList) {
        $parentPreset = Assert-PresetExists -PresetList $PresetList -PresetName $parentName -Kind "configure"
        $resolvedValue = Get-PresetProperty -PresetList $PresetList `
                                            -Preset $parentPreset `
                                            -PropertyName $PropertyName
        if ($null -ne $resolvedValue) {
            return $resolvedValue
        }
    }

    return $null
}

function Get-PresetCacheVariable
{
    param(
        [object[]]$PresetList,
        [object]$Preset,
        [string]$VariableName
    )

    if ($null -eq $Preset) {
        return $null
    }

    if (($Preset.PSObject.Properties.Name -contains "cacheVariables") -and
        ($Preset.cacheVariables.PSObject.Properties.Name -contains $VariableName)) {
        return $Preset.cacheVariables.$VariableName
    }

    if (-not ($Preset.PSObject.Properties.Name -contains "inherits")) {
        return $null
    }

    $inheritList = @($Preset.inherits)
    foreach ($parentName in $inheritList) {
        $parentPreset = Assert-PresetExists -PresetList $PresetList -PresetName $parentName -Kind "configure"
        $resolvedValue = Get-PresetCacheVariable -PresetList $PresetList `
                                                 -Preset $parentPreset `
                                                 -VariableName $VariableName
        if ($null -ne $resolvedValue) {
            return $resolvedValue
        }
    }

    return $null
}

function Assert-ValueEquals
{
    param(
        [object]$ActualValue,
        [string]$ExpectedValue,
        [string]$Message
    )

    if ([string]$ActualValue -ne $ExpectedValue) {
        throw $Message
    }
}

function Assert-PresetHidden
{
    param(
        [object]$Preset,
        [string]$PresetName
    )

    if (-not ($Preset.PSObject.Properties.Name -contains "hidden") -or (-not [bool]$Preset.hidden)) {
        throw "Preset '$PresetName' must stay hidden."
    }
}

$paths = Resolve-ThemisPaths -Root $RepoRoot
$presetPath = Join-Path $paths.RepoRoot "CMakePresets.json"

if (-not (Test-Path -LiteralPath $presetPath)) {
    throw "CMakePresets.json was not found: $presetPath"
}

$presetJson = Get-Content -LiteralPath $presetPath -Encoding UTF8 -Raw | ConvertFrom-Json

if ([int]$presetJson.version -lt 6) {
    throw "CMakePresets.json must use preset schema version 6 or newer."
}

$requiredConfigurePresets = @(
    "host-debug",
    "host-relwithdebinfo",
    "fw-tiny-carrier-minsizerel",
    "fw-tiny-headless-minsizerel",
    "fw-standard-carrier-minsizerel",
    "fw-full-diag-carrier-debug"
)
$requiredBuildPresets = @(
    "build-host-debug",
    "build-host-relwithdebinfo",
    "build-fw-tiny-carrier-minsizerel",
    "build-fw-tiny-headless-minsizerel",
    "build-fw-standard-carrier-minsizerel",
    "build-fw-full-diag-carrier-debug"
)
$requiredTestPresets = @(
    "test-host-debug",
    "test-host-relwithdebinfo"
)

$hostBasePreset = Assert-PresetExists -PresetList $presetJson.configurePresets `
                                      -PresetName "host-base" `
                                      -Kind "configure"
Assert-PresetHidden -Preset $hostBasePreset -PresetName "host-base"
Assert-ValueEquals -ActualValue (Get-PresetCacheVariable -PresetList $presetJson.configurePresets `
                                                        -Preset $hostBasePreset `
                                                        -VariableName "THEMIS_BUILD_FIRMWARE") `
                   -ExpectedValue "OFF" `
                   -Message "host-base must own THEMIS_BUILD_FIRMWARE=OFF."
Assert-ValueEquals -ActualValue (Get-PresetCacheVariable -PresetList $presetJson.configurePresets `
                                                        -Preset $hostBasePreset `
                                                        -VariableName "THEMIS_BUILD_HOST_TESTS") `
                   -ExpectedValue "ON" `
                   -Message "host-base must own THEMIS_BUILD_HOST_TESTS=ON."

$fwBasePreset = Assert-PresetExists -PresetList $presetJson.configurePresets `
                                    -PresetName "fw-base" `
                                    -Kind "configure"
Assert-PresetHidden -Preset $fwBasePreset -PresetName "fw-base"
Assert-ValueEquals -ActualValue (Get-PresetCacheVariable -PresetList $presetJson.configurePresets `
                                                        -Preset $fwBasePreset `
                                                        -VariableName "THEMIS_BUILD_FIRMWARE") `
                   -ExpectedValue "ON" `
                   -Message "fw-base must own THEMIS_BUILD_FIRMWARE=ON."
Assert-ValueEquals -ActualValue (Get-PresetCacheVariable -PresetList $presetJson.configurePresets `
                                                        -Preset $fwBasePreset `
                                                        -VariableName "THEMIS_BUILD_HOST_TESTS") `
                   -ExpectedValue "OFF" `
                   -Message "fw-base must own THEMIS_BUILD_HOST_TESTS=OFF."
Assert-ValueEquals -ActualValue (Get-PresetCacheVariable -PresetList $presetJson.configurePresets `
                                                        -Preset $fwBasePreset `
                                                        -VariableName "THEMIS_FIRMWARE_TARGET_NAME") `
                   -ExpectedValue "falcon-foc-at32" `
                   -Message "fw-base must own the canonical firmware target name."
Assert-ValueEquals -ActualValue (Get-PresetCacheVariable -PresetList $presetJson.configurePresets `
                                                        -Preset $fwBasePreset `
                                                        -VariableName "THEMIS_PLATFORM_VENDOR") `
                   -ExpectedValue "at" `
                   -Message "fw-base must own the live platform vendor baseline."
Assert-ValueEquals -ActualValue (Get-PresetCacheVariable -PresetList $presetJson.configurePresets `
                                                        -Preset $fwBasePreset `
                                                        -VariableName "THEMIS_PLATFORM_SOC") `
                   -ExpectedValue "at32f415" `
                   -Message "fw-base must own the live platform SoC baseline."

$fwBaseToolchain = Get-PresetProperty -PresetList $presetJson.configurePresets `
                                      -Preset $fwBasePreset `
                                      -PropertyName "toolchainFile"
if ([string]::IsNullOrWhiteSpace([string]$fwBaseToolchain)) {
    throw "fw-base must own the firmware toolchainFile."
}

foreach ($presetName in $requiredConfigurePresets) {
    $preset = Assert-PresetExists -PresetList $presetJson.configurePresets -PresetName $presetName -Kind "configure"
    $binaryDir = Get-PresetProperty -PresetList $presetJson.configurePresets `
                                    -Preset $preset `
                                    -PropertyName "binaryDir"
    if ([string]$binaryDir -notlike '${sourceDir}/build/*') {
        throw "Configure preset '$presetName' must place build trees under `${sourceDir}/build/."
    }
}

foreach ($preset in $presetJson.configurePresets) {
    if ($preset.name -like 'host-*') {
        Assert-ValueEquals -ActualValue (Get-PresetCacheVariable -PresetList $presetJson.configurePresets `
                                                                -Preset $preset `
                                                                -VariableName "THEMIS_BUILD_FIRMWARE") `
                           -ExpectedValue "OFF" `
                           -Message ("{0} must disable firmware builds." -f $preset.name)
        Assert-ValueEquals -ActualValue (Get-PresetCacheVariable -PresetList $presetJson.configurePresets `
                                                                -Preset $preset `
                                                                -VariableName "THEMIS_BUILD_HOST_TESTS") `
                           -ExpectedValue "ON" `
                           -Message ("{0} must enable host tests." -f $preset.name)
    }

    if ($preset.name -like 'fw-*') {
        $toolchainFile = Get-PresetProperty -PresetList $presetJson.configurePresets `
                                            -Preset $preset `
                                            -PropertyName "toolchainFile"
        if ([string]::IsNullOrWhiteSpace([string]$toolchainFile)) {
            throw "Firmware preset '$($preset.name)' must declare a toolchainFile."
        }

        Assert-ValueEquals -ActualValue (Get-PresetCacheVariable -PresetList $presetJson.configurePresets `
                                                                -Preset $preset `
                                                                -VariableName "THEMIS_BUILD_FIRMWARE") `
                           -ExpectedValue "ON" `
                           -Message ("{0} must enable firmware builds." -f $preset.name)
        Assert-ValueEquals -ActualValue (Get-PresetCacheVariable -PresetList $presetJson.configurePresets `
                                                                -Preset $preset `
                                                                -VariableName "THEMIS_BUILD_HOST_TESTS") `
                           -ExpectedValue "OFF" `
                           -Message ("{0} must disable host tests." -f $preset.name)
    }
}

$hostPreset = Assert-PresetExists -PresetList $presetJson.configurePresets `
                                  -PresetName "host-debug" `
                                  -Kind "configure"
Assert-ValueEquals -ActualValue (Get-PresetCacheVariable -PresetList $presetJson.configurePresets `
                                                        -Preset $hostPreset `
                                                        -VariableName "THEMIS_BUILD_FIRMWARE") `
                   -ExpectedValue "OFF" `
                   -Message "host-debug must disable firmware builds."
Assert-ValueEquals -ActualValue (Get-PresetCacheVariable -PresetList $presetJson.configurePresets `
                                                        -Preset $hostPreset `
                                                        -VariableName "THEMIS_BUILD_HOST_TESTS") `
                   -ExpectedValue "ON" `
                   -Message "host-debug must enable host tests."

$tinyCarrierPreset = Assert-PresetExists -PresetList $presetJson.configurePresets `
                                         -PresetName "fw-tiny-carrier-minsizerel" `
                                         -Kind "configure"
if (@($tinyCarrierPreset.inherits) -notcontains "fw-base") {
    throw "fw-tiny-carrier-minsizerel must inherit fw-base."
}
Assert-ValueEquals -ActualValue (Get-PresetCacheVariable -PresetList $presetJson.configurePresets `
                                                        -Preset $tinyCarrierPreset `
                                                        -VariableName "THEMIS_DEVDRV_PROFILE") `
                   -ExpectedValue "tiny" `
                   -Message "fw-tiny-carrier-minsizerel must target profile=tiny."
Assert-ValueEquals -ActualValue (Get-PresetCacheVariable -PresetList $presetJson.configurePresets `
                                                        -Preset $tinyCarrierPreset `
                                                        -VariableName "THEMIS_PRODUCT_FLAVOR") `
                   -ExpectedValue "carrier-demo" `
                   -Message "fw-tiny-carrier-minsizerel must target flavor=carrier-demo."

$tinyHeadlessPreset = Assert-PresetExists -PresetList $presetJson.configurePresets `
                                          -PresetName "fw-tiny-headless-minsizerel" `
                                          -Kind "configure"
if (@($tinyHeadlessPreset.inherits) -notcontains "fw-base") {
    throw "fw-tiny-headless-minsizerel must inherit fw-base."
}
Assert-ValueEquals -ActualValue (Get-PresetCacheVariable -PresetList $presetJson.configurePresets `
                                                        -Preset $tinyHeadlessPreset `
                                                        -VariableName "THEMIS_PRODUCT_FLAVOR") `
                   -ExpectedValue "headless-tiny" `
                   -Message "fw-tiny-headless-minsizerel must target flavor=headless-tiny."

$standardCarrierPreset = Assert-PresetExists -PresetList $presetJson.configurePresets `
                                             -PresetName "fw-standard-carrier-minsizerel" `
                                             -Kind "configure"
if (@($standardCarrierPreset.inherits) -notcontains "fw-base") {
    throw "fw-standard-carrier-minsizerel must inherit fw-base."
}
Assert-ValueEquals -ActualValue (Get-PresetCacheVariable -PresetList $presetJson.configurePresets `
                                                        -Preset $standardCarrierPreset `
                                                        -VariableName "THEMIS_DEVDRV_PROFILE") `
                   -ExpectedValue "standard" `
                   -Message "fw-standard-carrier-minsizerel must target profile=standard."

$fullDiagCarrierPreset = Assert-PresetExists -PresetList $presetJson.configurePresets `
                                             -PresetName "fw-full-diag-carrier-debug" `
                                             -Kind "configure"
if (@($fullDiagCarrierPreset.inherits) -notcontains "fw-base") {
    throw "fw-full-diag-carrier-debug must inherit fw-base."
}
Assert-ValueEquals -ActualValue (Get-PresetCacheVariable -PresetList $presetJson.configurePresets `
                                                        -Preset $fullDiagCarrierPreset `
                                                        -VariableName "THEMIS_DEVDRV_PROFILE") `
                   -ExpectedValue "full-diag" `
                   -Message "fw-full-diag-carrier-debug must target profile=full-diag."
Assert-ValueEquals -ActualValue (Get-PresetCacheVariable -PresetList $presetJson.configurePresets `
                                                        -Preset $fullDiagCarrierPreset `
                                                        -VariableName "THEMIS_PRODUCT_FLAVOR") `
                   -ExpectedValue "carrier-demo" `
                   -Message "fw-full-diag-carrier-debug must target flavor=carrier-demo."

foreach ($presetName in $requiredBuildPresets) {
    $null = Assert-PresetExists -PresetList $presetJson.buildPresets -PresetName $presetName -Kind "build"
}

foreach ($presetName in $requiredTestPresets) {
    $null = Assert-PresetExists -PresetList $presetJson.testPresets -PresetName $presetName -Kind "test"
}

Write-Host ("[cmake_presets] git root : " + $paths.GitRoot)
Write-Host ("[cmake_presets] repo root: " + $paths.RepoRoot)
Write-Host "[cmake_presets] all requested checks passed."
