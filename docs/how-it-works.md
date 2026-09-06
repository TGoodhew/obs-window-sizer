# How it works

The reasoning behind the implementation — the parts that are not obvious from
reading the code, and the mistakes they exist to avoid.

## The problem

OBS composites into a fixed canvas. If the captured window's pixel dimensions do
not match that canvas you get pillar or letterboxing, or OBS rescales the
capture and text turns soft. Two things make matching them by hand harder than
it looks.

### Windows lie about window size

Since Windows 10, `GetWindowRect()` returns a rectangle **larger than what you
see**. The extra area is an invisible resize border and drop shadow, typically
7px on each side and the bottom, and 0 at the top. Size a window to "1920 wide"
using `GetWindowRect` and the visible window is 1906.

The rectangle the compositor actually paints comes from
`DwmGetWindowAttribute()` with `DWMWA_EXTENDED_FRAME_BOUNDS` (attribute 9). The
plugin measures both and treats the difference as a delta to add to the
requested size:

```
shadow delta = outer (GetWindowRect) - visible (DWM extended frame bounds)
chrome delta = visible - client (GetClientRect)     [client-area mode only]
```

`SetWindowPos()` sizes the *outer* rectangle, so the target size is grown by the
shadow delta always, and by the chrome delta as well when sizing the client
area.

Both deltas are recomputed on each of up to three passes rather than measured
once, because a window that changes its own chrome in response to being resized
— a ribbon collapsing, a toolbar wrapping to a second row — would otherwise
leave the result one pass short. The loop stops as soon as the size is exact or
stops improving, which is also how a window with a minimum size or a fixed
aspect ratio is detected instead of being silently reported as a success.

### Display scaling makes "pixels" ambiguous

OBS captures **physical** pixels. Win32 only reports physical coordinates to a
thread that is per-monitor-DPI-aware; a system-aware or unaware thread gets
coordinates silently rescaled into its own virtual space. On a 150% display, a
window Windows describes as 1920x1080 is physically 2880x1620.

There is also an asymmetry that bites: **DWM always answers in physical pixels**,
while `GetWindowRect` answers in the calling thread's DPI space. Mix them
without care and the shadow-delta arithmetic is quietly wrong.

The OBS process happens to be Per-Monitor-V2 because Qt sets it that way, but
that is Qt's decision and not something a plugin should depend on. So every
measurement and every resize runs inside `DpiScope`, which pins the calling
thread to `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2` and restores the previous
context on the way out. Inside that scope all three measurements agree.

> This path is currently only tested at 100% scaling. See
> [issue #5](https://github.com/TGoodhew/obs-window-sizer/issues/5).

## Why the window list is not built here

The capture source's `window` setting is an escaped
`Title:WindowClass:executable.exe` string with escaping rules that are easy to
get wrong. Rather than construct or parse it, the dock reads the list straight
from the source that consumes it:

```cpp
obs_properties_t *props = obs_get_source_properties("window_capture");
obs_property_t *windowProp = obs_properties_get(props, "window");
// enumerate with obs_property_list_item_name() / _item_string()
```

That is the same code path that populates OBS's own dropdown, so the values are
correct by definition.

`obs_get_source_properties()` builds the list without needing a source instance.
That is safe for `window_capture` specifically: its `wc_properties()` null-checks
its data pointer, and both of its modified callbacks bail out early when the
param is null. Worth re-checking if OBS's implementation changes.

Mapping a chosen entry back to an `HWND` does **not** need `EnumWindows`. libobs
exports its own matching helpers — confirmed present in the shipped `obs.dll`
export table:

```cpp
ms_build_window_strings(setting, &windowClass, &title, &exe);
HWND hwnd = ms_find_window_top_level(INCLUDE_MINIMIZED, priority, windowClass, title, exe);
```

Using them means the window that gets resized and the window OBS captures are
chosen by identical logic, with the same match priority the plugin writes into
the source. They agree by construction rather than by two implementations
happening to agree.

## Client area versus visible frame

This maps onto how Windows Graphics Capture actually frames a window, which
`libobs-winrt/winrt-capture.cpp` makes explicit:

| Dock setting | Source `client_area` | Captured region |
|---|---|---|
| Size client area **on** | `true` | `GetClientRect` — content only |
| Size client area **off** | `false` | The DWM extended frame bounds |

The plugin always sets the source's `client_area` to match the checkbox, so the
thing being sized and the thing being captured are the same rectangle.

## Why recording is configured through profile config

The obvious approach — reach the frontend's encoder with
`obs_frontend_get_recording_output()` and call `obs_encoder_update()` — **cannot
work**.

In `OBSBasic_Recording.cpp`, `OBS_FRONTEND_EVENT_RECORDING_STARTING` is raised
at line 132, and `outputHandler->StartRecording()` is called at line 136. That
call immediately runs `UpdateRecording()`, which calls `obs_encoder_update()`
with settings rebuilt from profile config. Anything a plugin sets on the live
encoder — including from that event handler — is overwritten four lines later,
before the encoder initialises. `RECORDING_STARTED` fires after
`obs_output_start()`, far too late.

So the plugin writes the configuration the frontend reads *from* instead:

- `recordEncoder.json` in the profile folder, which
  `AdvancedOutput::UpdateRecordingSettings()` re-reads on **every** recording
  start — a two-line function, so a change takes effect on the next recording
  with no restart and no profile-switching dance.
- `[AdvOut] RecEncoder`, `RecType`, and the rescale keys via
  `obs_frontend_get_profile_config()`.

OBS's own Record button remains the thing that starts a recording, which is the
point of doing it this way rather than owning a private output.

The one seam: only **Advanced** output mode reads `recordEncoder.json` — Simple
mode builds encoder settings in code from a quality enum. Switching a profile
from Simple to Advanced needs one OBS restart, because the frontend only
rebuilds its output handler in `ResetOutputs()`, which no plugin can reach.

### Encoder IDs are enumerated, never hardcoded

OBS 30.2 introduced native NVENC implementations alongside the older
ffmpeg-based ones, with overlapping but not identical setting keys. Which are
registered depends on GPU and driver, and OBS's log only lists the user-visible
subset — on the development machine, twenty encoder types are registered while
the log shows five.

So the encoder is chosen at runtime with `obs_enum_encoder_types()`, preferring
`obs_nvenc_hevc_tex`, then any other HEVC NVENC, then H.264 NVENC.

The native settings keys differ from the ffmpeg ones in ways that fail silently
if assumed:

| Key | Value | Note |
|---|---|---|
| `rate_control` | `CQP` | |
| `cqp` | 16 | |
| `preset` | `p6` | A string `p1`–`p7`, not a named quality enum |
| `tune` | `hq` | |
| `multipass` | `qres` | This *is* two-pass quarter-resolution, not a bool |
| `adaptive_quantization` | `false` | Formerly `psycho_aq`; setting the old key does nothing |

## Keeping the canvas

`obs_reset_video()` changes the running pipeline only. Without also writing
`BaseCX`/`BaseCY`/`OutputCX`/`OutputCY` into the profile, the canvas reverts on
the next OBS start. Those are the same four keys OBS's own "Resize output
(source size)" feature writes.

Note that OBS's feature rounds width to a multiple of 4 and height to a multiple
of 2. This plugin deliberately does **not** round, because rounding would
silently break the 1:1 match it exists to provide. Instead it warns when the
achieved size is not a multiple of 4x2, since some encoders reject such
dimensions.
