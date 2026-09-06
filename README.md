# obs-window-sizer

An OBS Studio plugin that resizes a target application window to an exact
**physical** pixel size and sets the OBS canvas to the same size, so the window
capture is 1:1 — no letterboxing, no rescaling, no soft text.

It handles the two things that make this awkward by hand:

- Windows 10/11 windows carry an invisible drop-shadow border, so `GetWindowRect`
  returns a larger rectangle than what you see. The plugin measures the visible
  frame with `DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)` and corrects for
  the difference.
- Display scaling above 100% means a window Windows calls 1920x1080 is physically
  2400x1350 or larger, and OBS captures physical pixels. The plugin works in
  physical pixels throughout.

## Built against

| | |
|---|---|
| OBS Studio | **32.2.2** (`obs64.exe` file version) |
| OBS sources | tag `32.2.2`, fetched by `buildspec.json` |
| obs-deps / Qt 6 | `2026-07-15` (Qt 6.11.1) — the exact bundle OBS 32.2.2 uses |
| Toolchain | Visual Studio 2026 Enterprise 18.7, MSVC **14.51 (v145)** |
| Windows SDK | 10.0.22621 (pinned by the CMake preset) |
| CMake | 4.4.3 |

> **When an OBS update breaks the ABI**, this is the section to fix. OBS rejects
> plugins built against a newer libobs, and a plugin built against a different
> major version can crash OBS at load rather than fail gracefully. See
> [Rebuilding after an OBS update](#rebuilding-after-an-obs-update).

Note that Qt 6 must come from OBS's own prebuilt dependency bundle, which
`buildspec.json` fetches. Do **not** install Qt separately and do not point the
build at a system Qt — that is the most common cause of ABI crashes here.

## Prerequisites

- Visual Studio 2022 or newer with the **Desktop development with C++** workload
  (this project is built and tested with VS 2026 / MSVC v145).
- CMake 3.28+ on `PATH` (`winget install Kitware.CMake`).
- Git on `PATH`.
- PowerShell 7 (`pwsh`) — `deploy.ps1` requires it, Windows PowerShell 5.1 will not do.
- VS Code extensions `ms-vscode.cpptools-extension-pack` and `ms-vscode.cmake-tools`.

## Build

```powershell
cmake --preset windows-x64          # configure
cmake --build --preset windows-x64  # build (RelWithDebInfo)
```

The **first** configure takes several minutes and roughly 1 GB of downloads: it
fetches the obs-deps and Qt 6 bundles plus the OBS source tree into `.deps\`,
then builds `libobs` and `obs-frontend-api` from those sources in both Debug and
Release so the plugin has something to link against. Later configures are fast.

For a debug build:

```powershell
cmake --build --preset windows-x64-debug
```

Build output lands in `build_x64\rundir\<Configuration>\`:

```
obs-window-sizer.dll
obs-window-sizer.pdb
obs-window-sizer\locale\en-US.ini    <- plugin data folder
```

## Deploy

**OBS must be closed** — the DLL is locked while loaded.

```powershell
pwsh -File .\deploy.ps1                        # RelWithDebInfo -> ProgramData
pwsh -File .\deploy.ps1 -Configuration Debug   # Debug build
pwsh -File .\deploy.ps1 -Target ProgramFiles   # into the OBS install dir (elevates)
```

OBS 32 on Windows searches two locations, and `deploy.ps1` can write to either:

1. **`%ProgramData%\obs-studio\plugins\obs-window-sizer\`** — the default.
   Machine-wide, needs **no elevation**, and survives an OBS reinstall or update
   because it sits outside the install directory. Layout:
   `bin\64bit\obs-window-sizer.dll` and `data\locale\en-US.ini`.
   This is also where OBS's own CMake install target points.

2. **`C:\Program Files\obs-studio\obs-plugins\64bit\`** (`-Target ProgramFiles`) —
   alongside the bundled first-party plugins, with data in
   `C:\Program Files\obs-studio\data\obs-plugins\obs-window-sizer\`. Requires
   administrator rights, so the script relaunches itself through UAC. An OBS
   update can wipe this copy.

Do not deploy to both at once, or OBS will find the module twice.

Note that `%APPDATA%\obs-studio\plugins\` is **not** searched on Windows — that
is the macOS/Linux location. Only `%ProgramData%` works here.

### Confirming it loaded

OBS writes a log per run to `%APPDATA%\obs-studio\logs\`. Look for:

```
[window-sizer] loaded successfully (version 1.0.0)
```

and `obs-window-sizer.dll` in the `Loaded Modules:` list. Every line the plugin
logs is prefixed `[window-sizer]`.

## Using it

The dock registers as **Window Sizer** and starts hidden and floating, which is
how OBS adds any plugin dock. Turn it on from the menu bar: **Docks > Window
Sizer**. OBS remembers its position and visibility afterwards.

1. **Target window** - pick from the list, or press **Refresh** if the window
   you want has appeared since. The list comes from the window capture source's
   own property list, so it matches OBS's dropdown exactly.
2. **Size** - choose a preset or type into Width/Height. Picking a preset fills
   both boxes; typing your own updates the preset box, which shows *Custom* when
   nothing matches. The presets are:

   | Preset | Pixels |
   |---|---|
   | 720p HD | 1280 x 720 |
   | 1080p FHD | 1920 x 1080 |
   | 1440p 2K QHD | 2560 x 1440 |
   | 2160p 4K UHD | 3840 x 2160 |
   | Vertical | 1080 x 1920 |
   | Square | 1080 x 1080 |

   The names use the consumer meanings and show the pixels alongside, since "2K"
   colloquially means 1440p while DCI 2K is really 2048x1080. 4K here is UHD
   (3840x2160), not DCI 4K (4096x2160).

   Remember these are **physical** pixels, and a window cannot be made taller
   than the display it is on. On a 1440-tall monitor the 4K preset will be
   refused at 2160 and the status line will say what it settled at.
3. **Size client area** - on, the content area hits the target size and the
   capture source is set to capture the client area. Off, the visible window
   frame hits the target size instead, title bar and borders included. The
   checkbox and the capture source are always set to agree.
4. **Apply**.

The status line reports what actually happened, including the size achieved
versus the size requested. A window with a minimum size or a fixed aspect ratio
will refuse - Paint, for instance, will not go below 688x461 - and the status
says so rather than claiming success. The canvas is always set to the size the
window *actually* reached, so the capture stays 1:1 either way.

## Recording configuration

Tick **Configure recording (HEVC NVENC)** and Apply also points the profile's
recording output at NVENC HEVC with constant-quality settings chosen for sharp
UI text. **CQ level** sets the quality (16 by default; lower is bigger and
better).

It writes `recordEncoder.json` into the profile folder and sets
`[AdvOut] RecEncoder` in `basic.ini`, then leaves the rest to OBS. It does not
create its own output or its own record button - **OBS's own Record button
stays the thing that starts a recording**, which is the whole point of doing it
this way.

The settings written are:

| Key | Value | Why |
|---|---|---|
| `rate_control` | `CQP` | Constant quality, not bitrate-targeted |
| `cqp` | 16 (adjustable) | Visually lossless for UI content |
| `preset` | `p6` | Slow preset; the GPU has headroom at this resolution |
| `tune` | `hq` | High quality rather than low latency |
| `multipass` | `qres` | Two-pass, quarter resolution |
| `adaptive_quantization` | `false` | **Psycho-visual tuning off** - it softens static text |
| `profile` | `main` | HEVC 8-bit, matching an SDR NV12 pipeline |
| `RecRescaleFilter` | `OBS_SCALE_DISABLE` | **No rescale** - the file is written at the canvas size |
| `RecRescaleRes` | *(empty)* | Same |

The recording is **1:1 with the window**: the canvas is set to the window size,
base equals output so the canvas is not downscaled, the scene item sits at scale
1.0 with no bounds, and the recording rescale is switched off. Advanced mode
applies its own second downscale via `obs_encoder_set_scaled_size()` whenever
`RecRescaleFilter` is not `OBS_SCALE_DISABLE`, and nothing in OBS's main window
hints that it is on, so it is cleared explicitly rather than left to a default.

If you Apply *without* ticking the box, the dock does not touch your recording
configuration - but it still checks, and warns in the status line if the profile
would rescale recordings away from the canvas. It reports rather than silently
changing settings you did not ask it to change.

The encoder ID is **enumerated at runtime** with `obs_enum_encoder_types()`, not
hardcoded. It prefers `obs_nvenc_hevc_tex` (the native implementation OBS 30.2
introduced), falls back to any other HEVC NVENC encoder, then to H.264 NVENC,
and says which it chose. Note that this machine registers *both* the native and
the older ffmpeg-based NVENC encoders even though OBS's log only lists the
native ones as available - another reason not to hardcode an ID.

### Why not reconfigure the frontend's encoder directly

Because it cannot work. `OBSBasic::StartRecording()` raises
`OBS_FRONTEND_EVENT_RECORDING_STARTING` and then, four lines later, calls
`outputHandler->StartRecording()`, which immediately runs `obs_encoder_update()`
with settings rebuilt from profile config. Anything a plugin sets on the live
encoder - including from that event handler - is overwritten before the encoder
initialises, and `RECORDING_STARTED` fires too late to matter.

Writing the config the frontend reads from sidesteps all of it.
`AdvancedOutput::UpdateRecordingSettings()` re-reads `recordEncoder.json` from
disk on *every* recording start, so no profile switching or restart dance is
needed to make a change take - which is what made this impractical over
obs-websocket but straightforward in-process.

### Two things worth knowing

**Switching output mode needs one restart.** Advanced output mode is the only
mode that reads `recordEncoder.json` - Simple mode builds its encoder settings
in code from a quality enum. If your profile is on Simple, the plugin switches
it to Advanced and says so, but the frontend only rebuilds its output handler in
`ResetOutputs()`, which a plugin cannot reach. **Restart OBS once** and it takes
effect; every change after that is picked up on the next recording with no
restart. Be aware the switch changes which settings your *streaming* uses too -
check Settings > Output > Streaming if you stream.

**Apply is refused while recording or streaming.** The canvas cannot be resized
while an output is running - `obs_reset_video()` returns
`OBS_VIDEO_CURRENTLY_ACTIVE` - so the dock checks first and says so plainly
without touching the target window.

**The canvas is persisted.** `obs_reset_video()` alone only changes the running
pipeline, so the canvas would revert on the next OBS start. Apply also writes
`BaseCX`/`BaseCY`/`OutputCX`/`OutputCY` into the profile - the same four keys
OBS's own "Resize output (source size)" uses - so the size survives a restart.

**Odd sizes are kept, not rounded.** OBS's own resize feature rounds width to a
multiple of 4 and height to a multiple of 2. This plugin does not, because
rounding would silently break the 1:1 match it exists to provide. If the size a
window settles on is not a multiple of 4x2 the status line warns you, since some
encoders refuse such dimensions - nudge the size by a pixel if a recording
fails.

## Debugging in VS Code

`.vscode\launch.json` provides three configurations:

- **Launch OBS with obs-window-sizer (RelWithDebInfo)** — builds, deploys, then
  starts `obs64.exe` under the MSVC debugger. Breakpoints in plugin sources work.
- **Launch OBS with obs-window-sizer (Debug)** — same for the debug build.
- **Attach to running OBS** — pick the `obs64.exe` process.

The launch configurations run the `deploy` / `deploy-debug` tasks first, which in
turn depend on the build tasks, so F5 is enough. If a launch fails because the
DLL is locked, close any OBS instance you started by hand.

`.vscode\tasks.json` also exposes `configure`, `build`, `build-debug`, `deploy`
and `deploy-debug` directly (Ctrl+Shift+B runs `build`).

## Rebuilding after an OBS update

1. Read the new version: right-click
   `C:\Program Files\obs-studio\bin\64bit\obs64.exe` → Properties → Details, or

   ```powershell
   (Get-Item 'C:\Program Files\obs-studio\bin\64bit\obs64.exe').VersionInfo.FileVersion
   ```

2. Find the dependency versions that release used. OBS no longer keeps a root
   `buildspec.json`; the dependency manifest now lives in its `CMakePresets.json`
   under the `dependencies` configure preset:

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

4. Update `buildspec.json`: the `obs-studio` version and hash, and the `prebuilt`
   and `qt6` versions and `windows-x64` hashes copied from step 2.

5. Reconfigure and rebuild. The build system notices the changed hashes, discards
   the stale `.deps\` content and refetches:

   ```powershell
   cmake --preset windows-x64
   cmake --build --preset windows-x64
   pwsh -File .\deploy.ps1
   ```

6. Update the [Built against](#built-against) table above.

If a configure fails oddly after a version bump, delete `build_x64\` and `.deps\`
and start clean — both are disposable and git-ignored.

## Licence

MIT - see [LICENSE](LICENSE), with third-party notices in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

Two caveats worth stating plainly:

- `cmake/**`, `src/plugin-support.h` and `src/plugin-support.c.in` are retained
  from obs-plugintemplate and remain **GPL-2.0-or-later**. They are not covered
  by the MIT grant.
- The plugin links libobs and obs-frontend-api, which are GPL-2.0-or-later. MIT
  is GPL-compatible so this source can carry it, but a **compiled binary**
  distributed together with libobs is still subject to the GPL.

## Project layout

```
buildspec.json          Plugin identity + pinned OBS/obs-deps/Qt versions
CMakePresets.json       windows-x64 preset (VS 2026 generator, SDK 10.0.22621)
CMakeLists.txt          Plugin target; ENABLE_FRONTEND_API and ENABLE_QT are on
deploy.ps1              Copies the built plugin into an OBS plugin path
cmake/                  Build system from obs-plugintemplate (Windows slice only)
src/
  plugin-main.cpp       Module entry points, dock registration
  window-sizer-dock.*   Qt dock UI and the Apply pipeline (libobs calls)
  win32-window.*        All Win32 interop: DPI, measurement, resize
  recording-config.*    Profile config and encoder settings (no Win32)
  plugin-support.h      obs_log() - prefixes every line with [window-sizer]
  plugin-support.c.in
data/locale/en-US.ini   UI strings
.deps/                  Fetched dependencies (git-ignored, disposable)
build_x64/              Build tree (git-ignored, disposable)
```

The project started from [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)
with the CI workflows, packaging and the macOS/Linux build slices removed, since
this is a local Windows-only plugin.
