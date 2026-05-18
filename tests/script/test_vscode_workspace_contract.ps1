param(
    [string]$RepoRoot = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
} else {
    $RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
}

$gitRoot = (& git -C $RepoRoot rev-parse --show-toplevel).Trim()
if ([string]::IsNullOrWhiteSpace($gitRoot)) {
    throw "git root must be discoverable for the VS Code workspace contract."
}

$settingsPath         = Join-Path $RepoRoot ".vscode\settings.json"
$tasksPath            = Join-Path $RepoRoot ".vscode\tasks.json"
$launchPath           = Join-Path $RepoRoot ".vscode\launch.json"
$cppPropertiesPath    = Join-Path $RepoRoot ".vscode\c_cpp_properties.json"
$extensionsPath       = Join-Path $RepoRoot ".vscode\extensions.json"
$readmePath           = Join-Path $gitRoot "README.md"
$legacyReadmePath     = Join-Path $RepoRoot ([System.IO.Path]::Combine("at32" + "_tec", "README.md"))

$settingsRaw = [System.IO.File]::ReadAllText($settingsPath, [System.Text.UTF8Encoding]::new($false))
$tasksRaw = [System.IO.File]::ReadAllText($tasksPath, [System.Text.UTF8Encoding]::new($false))
$launchRaw = [System.IO.File]::ReadAllText($launchPath, [System.Text.UTF8Encoding]::new($false))
$cppPropertiesRaw = [System.IO.File]::ReadAllText($cppPropertiesPath, [System.Text.UTF8Encoding]::new($false))
$readmeRaw = [System.IO.File]::ReadAllText($readmePath, [System.Text.UTF8Encoding]::new($false))

if (-not (Test-Path -LiteralPath $readmePath)) {
    throw "git-root README.md must exist as the single GitHub-facing entry."
}

if (Test-Path -LiteralPath $legacyReadmePath) {
    throw "Legacy nested README.md must stay retired after the repository root is flattened."
}

if ($settingsRaw -notmatch '"cmake\.useCMakePresets"\s*:\s*"always"') {
    throw "VS Code settings must keep CMake presets as the formal workspace source of truth."
}

if ($settingsRaw -notmatch '"cmake\.configurePreset"\s*:\s*"fw-full-diag-carrier-debug"') {
    throw "VS Code settings must pin the firmware debug preset for the default firmware editing lane."
}

if ($settingsRaw -notmatch '"cmake\.buildPreset"\s*:\s*"build-fw-full-diag-carrier-debug"') {
    throw "VS Code settings must pin the matching firmware build preset."
}

if ($settingsRaw -notmatch '"cmake\.configureOnOpen"\s*:\s*false') {
    throw "VS Code settings must disable configure-on-open so opening the workspace does not mutate the build tree implicitly."
}

if ($settingsRaw -notmatch '"cmake\.configureOnEdit"\s*:\s*false') {
    throw "VS Code settings must disable configure-on-edit so saving CMake files does not trigger a configure popup."
}

if ($settingsRaw -notmatch '"C_Cpp\.default\.configurationProvider"\s*:\s*"ms-vscode\.cmake-tools"') {
    throw "VS Code settings must delegate IntelliSense to CMake Tools instead of a handwritten include glob."
}

if ($settingsRaw -notmatch '"CMakeLists\.txt"\s*:\s*"cmake"') {
    throw "VS Code settings must force CMakeLists.txt into the cmake language mode."
}

if ($settingsRaw -notmatch '"editor\.defaultFormatter"\s*:\s*"twxs\.cmake"') {
    throw "VS Code settings must select the installed twxs.cmake formatter for CMake files."
}

if ($settingsRaw -notmatch '"editor\.formatOnSave"\s*:\s*true') {
    throw "VS Code settings must enable format-on-save for CMake files."
}

if (-not (Test-Path -LiteralPath $extensionsPath)) {
    throw "The workspace must recommend the required VS Code extensions explicitly."
}

$extensionsRaw = [System.IO.File]::ReadAllText($extensionsPath, [System.Text.UTF8Encoding]::new($false))
foreach ($extensionId in @(
    'ms-vscode.cmake-tools',
    'ms-vscode.cpptools',
    'marus25.cortex-debug',
    'twxs.cmake'
)) {
    if ($extensionsRaw -notmatch [regex]::Escape($extensionId)) {
        throw "The workspace extension recommendations must include ${extensionId}."
    }
}

if ($cppPropertiesRaw -notmatch '"configurationProvider"\s*:\s*"ms-vscode\.cmake-tools"') {
    throw "c_cpp_properties.json must use CMake Tools as the configuration provider."
}

if ($cppPropertiesRaw -notmatch '"compileCommands"\s*:\s*"\$\{workspaceFolder\}/build/fw-full-diag-carrier-debug/compile_commands\.json"') {
    throw "c_cpp_properties.json must point at the default firmware debug compile_commands.json."
}

if ($tasksRaw -notmatch '"label"\s*:\s*"Themis: Build fw-full-diag-carrier-debug"') {
    throw "VS Code tasks must expose an explicit firmware preset build lane for F5/debug."
}

if ($tasksRaw -notmatch '"build-fw-full-diag-carrier-debug"') {
    throw "VS Code tasks must build the formal full-diag firmware preset explicitly."
}

if ($tasksRaw -notmatch '\./run\.sh') {
    throw "VS Code tasks must keep the canonical run.sh CLI lane discoverable."
}

if ($launchRaw -notmatch '"preLaunchTask"\s*:\s*"Themis: Build fw-full-diag-carrier-debug"') {
    throw "launch.json must build the same firmware preset that the workspace IntelliSense lane uses."
}

if ($launchRaw -notmatch 'build/fw-full-diag-carrier-debug/output/falcon-foc-at32\.elf') {
    throw "launch.json must debug the ELF produced by the formal firmware debug preset."
}

if ($readmeRaw -notmatch 'RT-Thread') {
    throw "README.md must explain the RT-Thread portability target."
}

if ($readmeRaw -notmatch 'Zephyr') {
    throw "README.md must explain the Zephyr portability target."
}

if ($readmeRaw -notmatch '## Architecture \|') {
    throw "README.md must expose a bilingual architecture section."
}

if ($readmeRaw -notmatch '## Workflows \|') {
    throw "README.md must expose a bilingual workflow section."
}

if ($readmeRaw -notmatch '## Documentation \|') {
    throw "README.md must expose a bilingual documentation section."
}

Write-Host "[test_vscode_workspace_contract] all assertions passed."
