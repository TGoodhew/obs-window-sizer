#Requires -Version 7
<#
.SYNOPSIS
    Builds the release zips for obs-window-sizer.

.DESCRIPTION
    Produces two archives in .\dist:

      obs-window-sizer-<version>-windows-x64.zip
          The plugin. Its internal layout mirrors the install destination
          exactly, so installing is "extract into %ProgramData%\obs-studio\plugins".

      obs-window-sizer-<version>-windows-x64-pdbs.zip
          Debug symbols, kept separate so the main download stays small.

    The version comes from buildspec.json, so there is one place to change it.

    Docs are placed INSIDE the plugin folder rather than at the archive root, so
    extracting cannot scatter loose files into the user's plugins directory.

.PARAMETER Configuration
    Build configuration to package. Defaults to RelWithDebInfo.

.PARAMETER SkipBuild
    Package whatever is already built instead of building first.

.EXAMPLE
    pwsh -File .\package.ps1
    pwsh -File .\package.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
    [ValidateSet('RelWithDebInfo', 'Release')]
    [string] $Configuration = 'RelWithDebInfo',

    [switch] $SkipBuild
)

$ErrorActionPreference = 'Stop'

$PluginName = 'obs-window-sizer'
$Root = $PSScriptRoot
$BuildDir = Join-Path $Root 'build_x64'
$DistDir = Join-Path $Root 'dist'

function Resolve-CMake {
    <#
        Prefer whatever is on PATH, but fall back to the usual install
        locations. A freshly installed CMake is not on PATH in shells that were
        already open, which otherwise fails here with a confusing error.
    #>
    $onPath = Get-Command cmake -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    $candidates = @(
        (Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe')
        (Join-Path ${env:ProgramFiles(x86)} 'CMake\bin\cmake.exe')
    )

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $vsPath = & $vswhere -latest -property installationPath 2>$null
        if ($vsPath) {
            $candidates += Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        }
    }

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) { return $candidate }
    }

    throw 'CMake not found. Install it (winget install Kitware.CMake) or put it on PATH.'
}

$CMake = Resolve-CMake
Write-Host "Using CMake: $CMake" -ForegroundColor DarkGray

# --- Version, from the single source of truth ---

$buildspec = Get-Content (Join-Path $Root 'buildspec.json') -Raw | ConvertFrom-Json
$version = $buildspec.version
if (-not $version) { throw 'No version in buildspec.json' }

Write-Host "Packaging $PluginName $version ($Configuration)" -ForegroundColor Cyan

# --- Build ---

if (-not $SkipBuild) {
    Write-Host 'Building...' -ForegroundColor Cyan
    & $CMake --build --preset windows-x64 --config $Configuration
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }
} elseif (-not (Test-Path -LiteralPath $BuildDir)) {
    throw "No build tree at $BuildDir. Run without -SkipBuild."
}

# --- Stage via CMake's own install rules ---

$stage = Join-Path ([System.IO.Path]::GetTempPath()) "obs-window-sizer-pkg-$([guid]::NewGuid().ToString('N'))"
try {
    New-Item -ItemType Directory -Path $stage -Force | Out-Null

    Write-Host 'Staging...' -ForegroundColor Cyan
    & $CMake --install $BuildDir --config $Configuration --prefix $stage | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "cmake --install failed with exit code $LASTEXITCODE." }

    $pluginDir = Join-Path $stage $PluginName
    $dll = Join-Path $pluginDir 'bin\64bit\obs-window-sizer.dll'
    if (-not (Test-Path -LiteralPath $dll)) {
        throw "Staging produced no DLL at $dll"
    }

    # Sanity: the DLL must be the one we just built, not a stale artefact.
    $dllInfo = Get-Item -LiteralPath $dll
    Write-Host ("  DLL {0:N0} bytes, built {1}" -f $dllInfo.Length, $dllInfo.LastWriteTime)

    # --- Split the symbols out ---

    $pdbStage = Join-Path $stage "__pdbs\$PluginName\bin\64bit"
    New-Item -ItemType Directory -Path $pdbStage -Force | Out-Null

    $pdb = Join-Path $pluginDir 'bin\64bit\obs-window-sizer.pdb'
    if (Test-Path -LiteralPath $pdb) {
        Move-Item -LiteralPath $pdb -Destination $pdbStage -Force
    } else {
        Write-Warning 'No PDB found; the symbols archive will be skipped.'
    }

    # --- Docs travel inside the plugin folder ---

    foreach ($doc in 'README.md', 'LICENSE', 'THIRD-PARTY-NOTICES.md') {
        $src = Join-Path $Root $doc
        if (Test-Path -LiteralPath $src) {
            Copy-Item -LiteralPath $src -Destination $pluginDir -Force
        } else {
            Write-Warning "Missing $doc; not included."
        }
    }

    # --- Archives ---

    if (-not (Test-Path -LiteralPath $DistDir)) {
        New-Item -ItemType Directory -Path $DistDir -Force | Out-Null
    }

    $mainZip = Join-Path $DistDir "$PluginName-$version-windows-x64.zip"
    $pdbZip = Join-Path $DistDir "$PluginName-$version-windows-x64-pdbs.zip"

    Remove-Item -LiteralPath $mainZip, $pdbZip -Force -ErrorAction SilentlyContinue

    Compress-Archive -Path $pluginDir -DestinationPath $mainZip -CompressionLevel Optimal

    $producedPdbZip = $false
    if (Test-Path -LiteralPath (Join-Path $pdbStage 'obs-window-sizer.pdb')) {
        Compress-Archive -Path (Join-Path $stage "__pdbs\$PluginName") -DestinationPath $pdbZip -CompressionLevel Optimal
        $producedPdbZip = $true
    }

    # --- Report ---

    Write-Host ''
    Write-Host 'Artifacts:' -ForegroundColor Green

    $results = @($mainZip)
    if ($producedPdbZip) { $results += $pdbZip }

    foreach ($zip in $results) {
        $item = Get-Item -LiteralPath $zip
        $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLower()
        Write-Host ("  {0}" -f $item.Name)
        Write-Host ("    {0:N0} bytes" -f $item.Length)
        Write-Host ("    sha256 {0}" -f $hash)
    }

    Write-Host ''
    Write-Host 'Contents of the plugin archive:' -ForegroundColor Green
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($mainZip)
    try {
        $archive.Entries | Sort-Object FullName | ForEach-Object { Write-Host "  $($_.FullName)" }
    } finally {
        $archive.Dispose()
    }
} finally {
    if (Test-Path -LiteralPath $stage) {
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
}
