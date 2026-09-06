# Building from source

You only need this if you are changing the plugin or rebuilding it after an OBS
update. To just use it, download a release and follow
[deployment.md](deployment.md).

## Prerequisites

| | Needed | Notes |
|---|---|---|
| Visual Studio | 2022 or newer, **Desktop development with C++** | Built and tested with VS 2026 / MSVC v145 (14.51). VS 2022 / v143 also works. |
| Windows SDK | 10.0.22621 | Pinned by the CMake preset |
| CMake | 3.28+, on `PATH` | `winget install Kitware.CMake`. VS bundles one but does not put it on `PATH`. |
| Git | on `PATH` | |
| PowerShell 7 | `pwsh` | `deploy.ps1` and `package.ps1` require it; Windows PowerShell 5.1 will not do |
| VS Code (optional) | `ms-vscode.cpptools-extension-pack`, `ms-vscode.cmake-tools` | For the bundled tasks and debugger configuration |

Check what you have:

```powershell
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property displayName
cmake --version
git --version
pwsh --version
```

### Do not install Qt

The build fetches OBS's own prebuilt Windows dependency bundle, which contains
the exact Qt 6 build OBS ships with. **Using a system Qt is the most common
cause of ABI crashes in OBS plugins.** Let `buildspec.json` provide it.

## Build

```powershell
cmake --preset windows-x64          # configure
cmake --build --preset windows-x64  # build (RelWithDebInfo)
```

For a debug build:

```powershell
cmake --build --preset windows-x64-debug
```

### The first configure is slow

Expect several minutes and roughly **1 GB of downloads**. It:

1. Fetches the obs-deps and Qt 6 bundles into `.deps\` (~330 MB compressed)
2. Fetches the OBS source tree for the pinned version
3. Configures OBS and builds `libobs` and `obs-frontend-api` from it, in **both**
   Debug and Release, so the plugin has import libraries to link against

`.deps\` ends up around **5.3 GB**. It is git-ignored and entirely disposable —
delete it and the next configure rebuilds it.

Later configures are fast; the build system skips dependencies whose recorded
hash still matches `buildspec.json`.

## Output

`build_x64\rundir\<Configuration>\`:

```
obs-window-sizer.dll
obs-window-sizer.pdb
obs-window-sizer\locale\en-US.ini
```

`deploy.ps1` copies from there. See [deployment.md](deployment.md).

## VS Code

`.vscode\tasks.json` provides `configure`, `build`, `build-debug`, `deploy` and
`deploy-debug`. Ctrl+Shift+B runs `build`.

`.vscode\launch.json` provides three debug configurations:

- **Launch OBS with obs-window-sizer (RelWithDebInfo)** — builds, deploys, then
  starts `obs64.exe` under the MSVC debugger, so breakpoints in plugin sources
  work
- **Launch OBS with obs-window-sizer (Debug)** — the same for the debug build
- **Attach to running OBS** — pick the `obs64.exe` process

The launch configurations depend on the deploy tasks, which depend on the build
tasks, so F5 is enough. If a launch fails because the DLL is locked, close any
OBS you started by hand.

## Rebuilding after an OBS update

OBS rejects a plugin built against a **newer** libobs than the one running, and
a plugin built against a different major version can crash OBS at load rather
than fail gracefully. After an OBS update, rebuild.

1. Read the new version:

   ```powershell
   (Get-Item 'C:\Program Files\obs-studio\bin\64bit\obs64.exe').VersionInfo.FileVersion
   ```

2. Find the dependency versions that release used. OBS no longer keeps a root
   `buildspec.json`; the manifest now lives in its `CMakePresets.json` under the
   `dependencies` configure preset:

   ```powershell
   $v = '32.2.3'   # the new version
   irm "https://raw.githubusercontent.com/obsproject/obs-studio/$v/CMakePresets.json" |
     Select-Object -ExpandProperty configurePresets |
     Where-Object name -eq dependencies |
     ForEach-Object { $_.vendor.'obsproject.com/obs-studio'.dependencies } |
     ConvertTo-Json -Depth 6
   ```

3. Get the SHA-256 of the OBS source archive:

   ```powershell
   $v = '32.2.3'
   irm "https://github.com/obsproject/obs-studio/archive/refs/tags/$v.zip" -OutFile "$env:TEMP\$v.zip"
   (Get-FileHash "$env:TEMP\$v.zip" -Algorithm SHA256).Hash.ToLower()
   ```

4. Update `buildspec.json`: the `obs-studio` version and hash, plus the
   `prebuilt` and `qt6` versions and `windows-x64` hashes from step 2.

5. Reconfigure and rebuild. Changed hashes cause the stale `.deps\` content to
   be discarded and refetched automatically:

   ```powershell
   cmake --preset windows-x64
   cmake --build --preset windows-x64
   pwsh -File .\deploy.ps1
   ```

6. Update the version table in the README.

If a configure fails oddly after a version bump, delete `build_x64\` and
`.deps\` and start clean. Both are disposable.

## Project layout

```
buildspec.json          Plugin identity + pinned OBS/obs-deps/Qt versions
CMakePresets.json       windows-x64 preset (VS 2026 generator, SDK 10.0.22621)
CMakeLists.txt          Plugin target; ENABLE_FRONTEND_API and ENABLE_QT are on
deploy.ps1              Copies the built plugin into an OBS plugin path
package.ps1             Builds the release zips
cmake/                  Build system from obs-plugintemplate (Windows slice only)
docs/                   This documentation
src/
  plugin-main.cpp       Module entry points, dock registration
  window-sizer-dock.*   Qt dock UI and the Apply pipeline (libobs calls)
  win32-window.*        All Win32 interop: DPI, measurement, resize
  recording-config.*    Profile config and encoder settings (no Win32)
  plugin-support.*      obs_log(), prefixes every line with [window-sizer]
data/locale/en-US.ini   UI strings
.deps/                  Fetched dependencies (git-ignored, disposable)
build_x64/              Build tree (git-ignored, disposable)
```

The project started from
[obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) with the
CI workflows, packaging and the macOS/Linux build slices removed, since this is
a Windows-only plugin.
