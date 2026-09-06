# Deployment

How to get `obs-window-sizer` into OBS Studio, where it has to go, and how to
confirm it actually loaded.

> **OBS must be closed while you deploy.** Windows locks the DLL for as long as
> OBS has the module loaded, so a copy over a running OBS fails with a sharing
> violation. `deploy.ps1` checks for a running `obs64` and refuses rather than
> half-copying.

## Where plugins live on Windows

OBS 32 searches exactly two places on Windows. Both work; they are not
equivalent.

### 1. `%ProgramData%\obs-studio\plugins\` — the default, and the one to use

```
C:\ProgramData\obs-studio\plugins\obs-window-sizer\
├── bin\64bit\
│   ├── obs-window-sizer.dll
│   └── obs-window-sizer.pdb      (optional, for symbolised crash reports)
└── data\
    └── locale\
        └── en-US.ini
```

- **No elevation.** A standard user can create and write this tree, so no UAC
  prompt on every deploy.
- **Survives OBS updates and reinstalls**, because it lives outside the install
  directory.
- It is where OBS's own CMake install target points, so it is the path the
  project's `install()` rules already produce.

### 2. `C:\Program Files\obs-studio\` — alongside the bundled plugins

```
C:\Program Files\obs-studio\obs-plugins\64bit\obs-window-sizer.dll
C:\Program Files\obs-studio\data\obs-plugins\obs-window-sizer\locale\en-US.ini
```

- **Requires administrator rights**, so deploying prompts for UAC.
- **An OBS update can wipe it**, since the updater rewrites the install tree.

Use this only if you have a specific reason to want the plugin sitting with the
first-party ones.

> **Never deploy to both at once.** OBS would find the module twice and log a
> duplicate-module warning; which copy wins is not something to rely on.

### The trap: `%APPDATA%` is not searched

`%APPDATA%\obs-studio\plugins\` looks like the obvious per-user location and is
wrong on Windows. It is the macOS and Linux path.

In `OBSBasic.cpp`, `AddExtraModulePaths()` calls `GetProgramDataPath()` on
Windows and `GetAppConfigPath()` on the other platforms. A plugin dropped into
`%APPDATA%` is **silently ignored** — no error, no log line, nothing in
`Loaded Modules`. It simply never appears.

If your plugin is not loading and the path looks right, check which of the two
you actually used.

## Deploying from a build

```powershell
pwsh -File .\deploy.ps1                        # RelWithDebInfo -> ProgramData
pwsh -File .\deploy.ps1 -Configuration Debug   # deploy the debug build
pwsh -File .\deploy.ps1 -Target ProgramFiles   # into the OBS install dir (elevates)
```

| Parameter | Values | Default | Notes |
|---|---|---|---|
| `-Configuration` | `RelWithDebInfo`, `Debug`, `Release` | `RelWithDebInfo` | Must match a build you have actually produced |
| `-Target` | `ProgramData`, `ProgramFiles` | `ProgramData` | `ProgramFiles` relaunches the script through UAC |
| `-ObsRoot` | path | `C:\Program Files\obs-studio` | Only used by `-Target ProgramFiles` |

The script requires **PowerShell 7** (`pwsh`). Windows PowerShell 5.1 will not
run it.

It copies from `build_x64\rundir\<Configuration>\`, which CMake populates as a
post-build step with the DLL, the PDB and a folder of plugin data named after
the plugin.

## Installing from a release zip

For anyone who does not want to build from source:

1. Download `obs-window-sizer-<version>-windows-x64.zip` from the
   [releases page](https://github.com/TGoodhew/obs-window-sizer/releases).
2. Close OBS Studio.
3. Extract the zip into `C:\ProgramData\obs-studio\plugins\`. The zip contains a
   top-level `obs-window-sizer\` folder, so the result is
   `C:\ProgramData\obs-studio\plugins\obs-window-sizer\bin\64bit\...`.
4. Start OBS and enable the dock from **Docks → Window Sizer**.

Type `%ProgramData%` into the Explorer address bar to get there; the folder is
hidden by default.

## Confirming it loaded

OBS writes a log per run to `%APPDATA%\obs-studio\logs\`. Open the newest, or
use **Help → Log Files → View Current Log**, and look for:

```
[window-sizer] loaded successfully (version 1.0.0)
[window-sizer] dock registered as 'obs_window_sizer_dock' (find it under Docks in the menu bar)
```

and `obs-window-sizer.dll` in the `Loaded Modules:` list.

Every line the plugin writes is prefixed `[window-sizer]`, so this finds all of
them:

```powershell
Select-String -Path "$env:APPDATA\obs-studio\logs\*.txt" -Pattern '\[window-sizer\]' |
    Select-Object -Last 20
```

If neither line appears, the DLL is not being found — recheck the path against
the two locations above.

## Uninstalling

Close OBS, then delete whichever tree you installed:

```powershell
Remove-Item -Recurse -Force "$env:ProgramData\obs-studio\plugins\obs-window-sizer"
```

Or, for a Program Files install (needs elevation):

```powershell
Remove-Item -Recurse -Force "C:\Program Files\obs-studio\obs-plugins\64bit\obs-window-sizer.*"
Remove-Item -Recurse -Force "C:\Program Files\obs-studio\data\obs-plugins\obs-window-sizer"
```

The plugin stores no settings of its own, but note that it *does* write to your
OBS profile when used — canvas resolution, and the recording encoder settings if
you asked for those. Removing the plugin does not undo those; change them in
**Settings → Video** and **Settings → Output** as normal.
