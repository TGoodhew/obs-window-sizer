/*
obs-window-sizer
Copyright (C) 2026 Tony Goodhew <tony@schnauzergroup.com>

SPDX-License-Identifier: MIT
See LICENSE in the project root for the full licence text.

------------------------------------------------------------------------------

The single translation unit holding Win32 interop. Everything that needs
Windows.h lives here; the rest of the plugin talks to it through
win32-window.hpp and opaque void* handles.
*/

#include "win32-window.hpp"

#include <windows.h>
#include <dwmapi.h>

#include <obs-module.h>
#include <plugin-support.h>

/*
 * libobs exports its own window matching helpers (verified present in the
 * shipped obs.dll export table). Using them means the escaped
 * "title:class:executable" strings taken from the capture source's property
 * list are decoded and matched by exactly the same code the capture source
 * itself uses, so we can never resize one window while OBS captures another.
 */
#include <util/windows/window-helpers.h>
#include <util/dstr.h>

#include <algorithm>
#include <climits>
#include <cstdlib>

namespace ws {

namespace {

/*
 * ---------------------------------------------------------------------------
 * DPI awareness
 * ---------------------------------------------------------------------------
 * OBS captures PHYSICAL pixels. Win32 only hands a caller physical
 * coordinates if the calling thread is Per-Monitor-DPI-aware; a system-aware
 * or unaware thread gets coordinates silently rescaled into its own virtual
 * space, so on a 150% display a "1920x1080" answer can really mean 2880x1620
 * on screen.
 *
 * The OBS process is Per-Monitor-V2 by way of Qt, but that is Qt's decision
 * and not something this plugin should depend on, so every measurement and
 * every resize below runs inside DpiScope, which pins the calling thread to
 * Per-Monitor-V2 and restores the previous context on the way out.
 *
 * DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is the pseudo-handle
 * ((DPI_AWARENESS_CONTEXT)-4). SetThreadDpiAwarenessContext arrived in
 * Windows 10 1607, so it is resolved dynamically - if it were ever missing we
 * simply run with whatever the thread already had, the same defensive shape
 * win-capture uses.
 */
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

typedef DPI_AWARENESS_CONTEXT(WINAPI *PFN_SetThreadDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);

static PFN_SetThreadDpiAwarenessContext getSetThreadDpiAwarenessContext()
{
	static PFN_SetThreadDpiAwarenessContext fn = []() -> PFN_SetThreadDpiAwarenessContext {
		HMODULE user32 = GetModuleHandleW(L"user32.dll");
		if (!user32)
			return nullptr;
		return (PFN_SetThreadDpiAwarenessContext)GetProcAddress(user32, "SetThreadDpiAwarenessContext");
	}();
	return fn;
}

class DpiScope {
public:
	DpiScope()
	{
		auto fn = getSetThreadDpiAwarenessContext();
		if (fn)
			previous_ = fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	}

	~DpiScope()
	{
		auto fn = getSetThreadDpiAwarenessContext();
		if (fn && previous_)
			fn(previous_);
	}

	DpiScope(const DpiScope &) = delete;
	DpiScope &operator=(const DpiScope &) = delete;

private:
	DPI_AWARENESS_CONTEXT previous_ = nullptr;
};

static std::string wideToUtf8(const wchar_t *text)
{
	if (!text || !*text)
		return std::string();

	const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
	if (bytes <= 1)
		return std::string();

	std::string out((size_t)bytes - 1, ' ');
	WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], bytes, nullptr, nullptr);
	return out;
}

/*
 * The visible frame. DWMWA_EXTENDED_FRAME_BOUNDS (attribute 9) reports the
 * rectangle the compositor actually paints, which since Windows 10 is smaller
 * than GetWindowRect: the difference is the invisible resize border / drop
 * shadow, typically 7px per side and 0 at the top. It is also the rectangle a
 * Windows Graphics Capture frame corresponds to, so it is what OBS captures
 * when the source's "client area" option is off.
 *
 * Note the asymmetry: DWM always answers in physical pixels, while
 * GetWindowRect answers in the calling thread's DPI space. The two agree only
 * inside a DpiScope.
 */
static bool visibleFrame(HWND hwnd, RECT &out)
{
	const HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &out, sizeof(out));
	return SUCCEEDED(hr);
}

static int rectWidth(const RECT &r)
{
	return (int)(r.right - r.left);
}

static int rectHeight(const RECT &r)
{
	return (int)(r.bottom - r.top);
}

static bool measure(HWND hwnd, WindowMetrics &out)
{
	if (!hwnd || !IsWindow(hwnd))
		return false;

	RECT outer = {};
	if (!GetWindowRect(hwnd, &outer))
		return false;

	RECT visible = {};
	if (!visibleFrame(hwnd, visible)) {
		/* A window DWM will not answer for: assume no invisible border. */
		visible = outer;
	}

	RECT client = {};
	if (!GetClientRect(hwnd, &client))
		return false;

	out.outerWidth = rectWidth(outer);
	out.outerHeight = rectHeight(outer);
	out.visibleWidth = rectWidth(visible);
	out.visibleHeight = rectHeight(visible);
	out.clientWidth = rectWidth(client);
	out.clientHeight = rectHeight(client);
	out.maximized = IsZoomed(hwnd) != FALSE;
	out.minimized = IsIconic(hwnd) != FALSE;
	return true;
}

static int achievedWidthFor(const WindowMetrics &m, bool clientArea)
{
	return clientArea ? m.clientWidth : m.visibleWidth;
}

static int achievedHeightFor(const WindowMetrics &m, bool clientArea)
{
	return clientArea ? m.clientHeight : m.visibleHeight;
}

} // namespace

void *findWindow(const std::string &obsWindowSetting, int priority)
{
	if (obsWindowSetting.empty())
		return nullptr;

	char *windowClass = nullptr;
	char *title = nullptr;
	char *exe = nullptr;

	/* Decodes the escaped "title:class:executable" form. Never build that
	 * string by hand - the escaping rules are easy to get wrong. */
	ms_build_window_strings(obsWindowSetting.c_str(), &windowClass, &title, &exe);

	DpiScope dpi;
	HWND hwnd = ms_find_window_top_level(INCLUDE_MINIMIZED, (enum window_priority)priority, windowClass, title,
					     exe);

	bfree(windowClass);
	bfree(title);
	bfree(exe);

	return hwnd;
}

std::string describeWindow(void *handle)
{
	HWND hwnd = (HWND)handle;
	if (!hwnd || !IsWindow(hwnd))
		return "<no window>";

	wchar_t title[512] = {};
	GetWindowTextW(hwnd, title, (int)(sizeof(title) / sizeof(title[0])));

	struct dstr exe = {};
	std::string exeName;
	if (ms_get_window_exe(&exe, hwnd) && exe.array)
		exeName = exe.array;
	dstr_free(&exe);

	std::string out = wideToUtf8(title);
	if (out.empty())
		out = "<untitled>";
	if (!exeName.empty())
		out += " (" + exeName + ")";
	return out;
}

bool getMetrics(void *handle, WindowMetrics &out)
{
	DpiScope dpi;
	return measure((HWND)handle, out);
}

ResizeOutcome resizeWindow(void *handle, int targetWidth, int targetHeight, bool sizeClientArea)
{
	ResizeOutcome result;
	result.requestedWidth = targetWidth;
	result.requestedHeight = targetHeight;

	HWND hwnd = (HWND)handle;
	if (!hwnd || !IsWindow(hwnd)) {
		result.error = "The target window no longer exists. Refresh the list.";
		return result;
	}

	if (targetWidth <= 0 || targetHeight <= 0) {
		result.error = "Target size must be positive.";
		return result;
	}

	DpiScope dpi;

	/* A maximized window ignores SetWindowPos sizing, and a minimized one
	 * cannot be measured meaningfully, so restore in both cases first. */
	if (IsZoomed(hwnd) || IsIconic(hwnd)) {
		ShowWindow(hwnd, SW_RESTORE);
		result.restoredFromMaximized = true;
	}

	if (!measure(hwnd, result.before)) {
		result.error = "Could not measure the target window.";
		return result;
	}

	WindowMetrics current = result.before;

	/*
	 * SetWindowPos sizes the OUTER rect, so the requested size has to be
	 * grown by everything sitting between it and what we are aiming at:
	 *
	 *   shadow delta = outer - visible   (the invisible border, always)
	 *   chrome delta = visible - client  (title bar and borders, client mode only)
	 *
	 * Both are recomputed on each pass rather than assumed once, because a
	 * window that changes its own chrome in response to being resized (a
	 * ribbon collapsing, a toolbar wrapping to a second row) would
	 * otherwise leave us one pass short. Three passes is plenty. We stop as
	 * soon as the result is exact or stops improving, which is also how a
	 * window with a minimum size or a fixed aspect ratio gets detected
	 * rather than silently reported as a success.
	 */
	int bestError = INT_MAX;

	for (int pass = 0; pass < 3; ++pass) {
		const int shadowW = current.outerWidth - current.visibleWidth;
		const int shadowH = current.outerHeight - current.visibleHeight;

		int outerW = targetWidth + shadowW;
		int outerH = targetHeight + shadowH;

		if (sizeClientArea) {
			outerW += current.visibleWidth - current.clientWidth;
			outerH += current.visibleHeight - current.clientHeight;
		}

		if (outerW <= 0 || outerH <= 0) {
			result.error = "Computed a non-positive window size; the target window "
				       "reported implausible metrics.";
			return result;
		}

		SetWindowPos(hwnd, nullptr, 0, 0, outerW, outerH,
			     SWP_NOMOVE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);

		if (!measure(hwnd, current)) {
			result.error = "The target window went away while being resized.";
			return result;
		}

		const int dw = achievedWidthFor(current, sizeClientArea) - targetWidth;
		const int dh = achievedHeightFor(current, sizeClientArea) - targetHeight;
		const int error = std::abs(dw) + std::abs(dh);

		if (error == 0)
			break;
		if (error >= bestError)
			break; /* not converging - the window is refusing the size */
		bestError = error;
	}

	result.after = current;
	result.achievedWidth = achievedWidthFor(current, sizeClientArea);
	result.achievedHeight = achievedHeightFor(current, sizeClientArea);
	result.ok = true;
	return result;
}

} // namespace ws
