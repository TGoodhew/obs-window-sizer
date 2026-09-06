/*
obs-window-sizer
Copyright (C) 2026 Tony Goodhew <tony@schnauzergroup.com>

SPDX-License-Identifier: MIT
See LICENSE in the project root for the full licence text.
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include "window-sizer-dock.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/* Dock ids must be unique across all loaded plugins; OBS also uses this to
 * persist the dock's position between runs, so it must stay stable. */
static constexpr const char *kDockId = "obs_window_sizer_dock";

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Resize a target window and the OBS canvas to matching exact pixel dimensions";
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

/*
 * The dock is registered in post_load rather than load: obs_frontend_add_dock_by_id
 * needs the frontend up, and the dock reads its window list from the
 * window_capture source, which belongs to win-capture and may not have been
 * loaded yet at obs_module_load time.
 *
 * OBS takes ownership of the widget - it is reparented into an OBSDock - so we
 * deliberately do not hold or delete it. obs_frontend_remove_dock() disposes of
 * both.
 */
void obs_module_post_load(void)
{
	auto *dock = new WindowSizerDock();

	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("WindowSizer.Dock.Title"), dock)) {
		obs_log(LOG_WARNING, "failed to register dock '%s'", kDockId);
		delete dock;
		return;
	}

	obs_log(LOG_INFO, "dock registered as '%s' (find it under Docks in the menu bar)", kDockId);
}

void obs_module_unload(void)
{
	obs_frontend_remove_dock(kDockId);
	obs_log(LOG_INFO, "unloaded");
}
