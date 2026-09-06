# Troubleshooting

Everything the plugin does is logged with a `[window-sizer]` prefix. Start there:
**Help → Log Files → View Current Log**, or

```powershell
Select-String -Path "$env:APPDATA\obs-studio\logs\*.txt" -Pattern '\[window-sizer\]' |
    Select-Object -Last 30
```

---

## "Window Sizer" is not in the Docks menu

The module is not loading. In the current log, look for
`obs-window-sizer.dll` in the `Loaded Modules:` list.

**If it is not there at all**, the DLL is not where OBS looks:

- It must be at `%ProgramData%\obs-studio\plugins\obs-window-sizer\bin\64bit\`.
- **`%APPDATA%\obs-studio\plugins\` is not searched on Windows.** That is the
  macOS/Linux path, and a plugin there is ignored with no error whatsoever. This
  is the single most likely cause.
- See [deployment.md](deployment.md) for the exact layout.

**If it is listed but the dock is missing**, look for
`[window-sizer] dock registered as 'obs_window_sizer_dock'`. If a warning says
the dock id is already in use, an older copy of the plugin is installed as well —
check both plugin locations and remove one.

**If OBS crashes at startup after installing**, the plugin was built against a
different OBS version. Check the version table in the README against your OBS
version and rebuild, or grab the matching release.

## The window list is empty, or missing the window I want

- Press **Refresh**. The list is populated at OBS startup and on scene changes,
  not continuously.
- **Minimised windows are excluded.** That is OBS's own filter — the list comes
  from the capture source's property list, so it contains exactly what OBS's
  Window Capture dropdown contains. Restore the window and refresh.
- If the dock says it could not read the capture source properties, `win-capture`
  is not loaded, which is a broken OBS install rather than a plugin problem.

## "Could not find that window any more"

The window closed, or its title changed, between the list being populated and
Apply being pressed. Titles are part of how a window is matched, and many
applications put document names in them. Press **Refresh** and reselect.

## The window will not reach the size I asked for

The status line reports what was actually achieved, for example:

```
Window refused 3840 x 2160 and settled at 3840 x 1421
```

Common causes:

- **The window cannot be taller or wider than the display.** On a 2560x1440
  monitor, 4K is not reachable — the height clamps around 1400.
- **The application has a minimum size.** Paint will not go below roughly
  688x461 of client area.
- **The application enforces an aspect ratio**, so one dimension follows the
  other.

The canvas is set to whatever the window *actually* reached, so the capture is
still 1:1 and unletterboxed — just not at the size you asked for.

## "Recording is active" / "Streaming is active"

The canvas cannot be resized while an output is running; `obs_reset_video()`
returns `OBS_VIDEO_CURRENTLY_ACTIVE`. Stop the recording or stream and Apply
again. The target window is not touched when Apply is refused.

## "not a multiple of 4x2, which some encoders refuse"

The size the window settled on has an odd height, or a width that is not a
multiple of 4. Video encoders generally want even dimensions, and NV12 in
particular needs an even height.

The plugin does **not** silently round, because rounding would break the exact
1:1 match it exists to provide. If a recording then fails to start, nudge the
requested size by a pixel or two until the achieved size is even.

## The recording is not 1:1 with the window

Tick **Record at high fidelity** and Apply. Among other things that explicitly
clears OBS's recording rescale, which is a second downscale applied *after* the
canvas and is not visible anywhere in OBS's main window.

If you would rather not have the plugin touch your recording settings, check
**Settings → Output → Recording → Rescale Output** is unticked yourself. With
the box unticked the plugin still checks this and warns in the status line, but
changes nothing.

## I ticked "Record at high fidelity" and nothing changed

Look for this in the status line:

```
restart OBS for the Simple-to-Advanced output mode switch to take effect
```

Only **Advanced** output mode reads the encoder settings file; Simple mode
builds its settings in code from a quality enum. If your profile was on Simple,
the plugin switched it to Advanced, but OBS only rebuilds its output handler at
startup. **Restart OBS once.** Every change after that applies to the next
recording with no restart.

## My streaming settings changed

A side effect of that same Simple → Advanced switch: in Advanced mode, streaming
settings come from a different part of the profile. Check
**Settings → Output → Streaming**. This is
[issue #11](https://github.com/TGoodhew/obs-window-sizer/issues/11).

## The canvas went back to its old size

It should not any more — Apply writes the size into the profile. If it does
revert, check the log for:

```
[window-sizer] canvas applied but not persisted: ...
```

which means the profile config could not be written.

## Text in the dock is cut off

Known, tracked as
[issue #7](https://github.com/TGoodhew/obs-window-sizer/issues/7). The collapsed
target-window box still truncates long titles; hover it for the full text as a
tooltip.

## Reporting a bug

Please include:

- OBS version (**Help → About**) and the plugin version
- The relevant `[window-sizer]` log lines, and the `resize result` line in
  particular — it reports requested, achieved, outer, visible and client sizes
- The target application, and your display resolution and scaling percentage

Display scaling matters: the plugin works in physical pixels and that path is
currently only well tested at 100%
([issue #5](https://github.com/TGoodhew/obs-window-sizer/issues/5)).
