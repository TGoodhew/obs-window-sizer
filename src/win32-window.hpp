/*
obs-window-sizer
Copyright (C) 2026 Tony Goodhew <tony@schnauzergroup.com>

SPDX-License-Identifier: MIT
See LICENSE in the project root for the full licence text.
*/

#pragma once

#include <string>

/*
 * All Win32 interop lives in win32-window.cpp. This header deliberately does
 * not include Windows.h so the rest of the plugin stays free of it; window
 * handles travel as opaque void pointers.
 */
namespace ws {

/*
 * Every measurement below is in PHYSICAL pixels - the units OBS captures in.
 * See win32-window.cpp for how that is guaranteed regardless of display scaling.
 */
struct WindowMetrics {
	/* GetWindowRect: includes the invisible resize/shadow border on Win10/11. */
	int outerWidth = 0;
	int outerHeight = 0;
	/* DWMWA_EXTENDED_FRAME_BOUNDS: the frame you actually see, and what a
	 * window capture with "client area" off produces. */
	int visibleWidth = 0;
	int visibleHeight = 0;
	/* GetClientRect: content area only, and what a window capture with
	 * "client area" on produces. */
	int clientWidth = 0;
	int clientHeight = 0;

	bool maximized = false;
	bool minimized = false;
};

struct ResizeOutcome {
	bool ok = false;
	/* Populated when ok is false. */
	std::string error;

	WindowMetrics before;
	WindowMetrics after;

	/* The size the caller asked for. */
	int requestedWidth = 0;
	int requestedHeight = 0;
	/* What was actually achieved, in the mode the caller asked for
	 * (client area or visible frame). */
	int achievedWidth = 0;
	int achievedHeight = 0;

	bool restoredFromMaximized = false;

	bool exact() const { return achievedWidth == requestedWidth && achievedHeight == requestedHeight; }
};

/*
 * Window match priority, mirroring libobs' enum window_priority. Passed
 * through so the dock can match with exactly the value it writes into the
 * capture source, keeping the two in agreement by construction.
 */
enum MatchPriority {
	MatchClass = 0,
	MatchTitle = 1,
	MatchExe = 2,
};

/*
 * Resolve one of OBS's own "title:class:executable" window setting strings to a
 * top-level HWND, using libobs' own matching so the result agrees with what the
 * capture source will pick. Returns nullptr if no window matches.
 */
void *findWindow(const std::string &obsWindowSetting, int priority);

/* Human-readable "Title (executable.exe)" for logging. Never fails. */
std::string describeWindow(void *hwnd);

bool getMetrics(void *hwnd, WindowMetrics &out);

/*
 * Resize so that either the client area or the visible frame ends up exactly
 * targetWidth x targetHeight physical pixels. Restores a maximized window
 * first, then re-measures and reports what was actually achieved.
 */
ResizeOutcome resizeWindow(void *hwnd, int targetWidth, int targetHeight, bool sizeClientArea);

} // namespace ws
