#Requires -Version 7
<#
.SYNOPSIS
    Deploys obs-window-sizer into an OBS Studio plugin search path.

.DESCRIPTION
    OBS 32 on Windows searches two places for plugins:

      1. %ProgramData%\obs-studio\plugins\<name>\bin\64bit\   (default here)
         Machine-wide, needs NO elevation, and survives an OBS reinstall
         or update because it lives outside the install directory.
         This is the path OBS's own CMake install target uses.

      2. <OBS install>\obs-plugins\64bit\                      (-Target ProgramFiles)
         Alongside the bundled first-party plugins. Requires administrator
         rights, and an OBS update can wipe it.

    Both are valid; ProgramData is the better default and is what the
    README documents.

.PARAMETER Configuration
    Build configuration to deploy. Defaults to RelWithDebInfo.

.PARAMETER Target
    ProgramData (default, unelevated) or ProgramFiles (elevated).

.PARAMETER ObsRoot
    OBS Studio install root, used by -Target ProgramFiles.

.EXAMPLE
    pwsh -File .\deploy.ps1
    pwsh -File .\deploy.ps1 -Configuration Debug
    pwsh -File .\deploy.ps1 -Target ProgramFiles
#>
[CmdletBinding()]
param(
    [ValidateSet('RelWithDebInfo', 'Debug', 'Release')]
    [string] $Configuration = 'RelWithDebInfo',

    [ValidateSet('ProgramData', 'ProgramFiles')]
    [string] $Target = 'ProgramData',

    [string] $ObsRoot = 'C:\Program Files\obs-studio',

    # Internal: set when the script has relaunched itself elevated.
    [switch] $Elevated
)

$ErrorActionPreference = 'Stop'

$PluginName = 'obs-window-sizer'
$RunDir = Join-Path $PSScriptRoot "build_x64\rundir\$Configuration"

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# --- Pre-flight (run unelevated so failures land in this console) ---

$dll = Join-Path $RunDir "$PluginName.dll"
if (-not (Test-Path -LiteralPath $dll)) {
    throw "Plugin DLL not found: $dll`nBuild first:  cmake --build --preset windows-x64"
}

if (Get-Process -Name 'obs64' -ErrorAction SilentlyContinue) {
    throw 'OBS Studio is running. Close it before deploying - the plugin DLL is locked while loaded.'
}

# --- Resolve destinations ---

if ($Target -eq 'ProgramData') {
    $pluginRoot = Join-Path $env:ProgramData "obs-studio\plugins\$PluginName"
    $binDest = Join-Path $pluginRoot 'bin\64bit'
    $dataDest = Join-Path $pluginRoot 'data'
    $needsElevation = $false
} else {
    if (-not (Test-Path -LiteralPath $ObsRoot)) {
        throw "OBS Studio not found at: $ObsRoot"
    }
    $binDest = Join-Path $ObsRoot 'obs-plugins\64bit'
    $dataDest = Join-Path $ObsRoot "data\obs-plugins\$PluginName"
    $needsElevation = $true
}

# --- Elevate if the chosen target needs it ---

if ($needsElevation -and -not (Test-Administrator)) {
    Write-Host 'Deploying into Program Files requires elevation; requesting via UAC...' -ForegroundColor Yellow

    $argList = @(
        '-NoProfile'
        '-ExecutionPolicy', 'Bypass'
        '-File', "`"$PSCommandPath`""
        '-Configuration', $Configuration
        '-Target', $Target
        '-ObsRoot', "`"$ObsRoot`""
        '-Elevated'
    )

    $proc = Start-Process -FilePath (Get-Process -Id $PID).Path `
        -ArgumentList $argList -Verb RunAs -Wait -PassThru

    if ($proc.ExitCode -ne 0) {
        throw "Elevated deploy failed with exit code $($proc.ExitCode)."
    }

    Write-Host 'Deploy complete.' -ForegroundColor Green
    return
}

# --- Copy ---

Write-Host "Deploying $PluginName ($Configuration) -> $Target" -ForegroundColor Cyan
Write-Host "  from $RunDir"

New-Item -ItemType Directory -Path $binDest -Force | Out-Null
Copy-Item -LiteralPath $dll -Destination $binDest -Force
Write-Host "  -> $binDest\$PluginName.dll"

$pdb = Join-Path $RunDir "$PluginName.pdb"
if (Test-Path -LiteralPath $pdb) {
    Copy-Item -LiteralPath $pdb -Destination $binDest -Force
    Write-Host "  -> $binDest\$PluginName.pdb"
}

# CMake stages plugin data in the rundir under a folder named after the plugin.
$dataSource = Join-Path $RunDir $PluginName
if (Test-Path -LiteralPath $dataSource) {
    if (Test-Path -LiteralPath $dataDest) {
        Remove-Item -LiteralPath $dataDest -Recurse -Force
    }
    New-Item -ItemType Directory -Path $dataDest -Force | Out-Null
    Copy-Item -Path (Join-Path $dataSource '*') -Destination $dataDest -Recurse -Force
    Write-Host "  -> $dataDest\"
} else {
    Write-Warning "No data folder at $dataSource - skipping data deploy."
}

Write-Host 'Done.' -ForegroundColor Green

if ($Elevated) {
    # Leave the transient elevated console up long enough to read.
    Start-Sleep -Seconds 2
}
