/*
obs-window-sizer
Copyright (C) 2026 Tony Goodhew <tony@schnauzergroup.com>

SPDX-License-Identifier: MIT
See LICENSE in the project root for the full licence text.
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QMessageBox>
#include <QString>
#include <QWidget>

#include "window-sizer-dock.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/* Dock ids must be unique across all loaded plugins; OBS also uses this to
 * persist the dock's position between runs, so it must stay stable. */
static constexpr const char *kDockId = "obs_window_sizer_dock";

/*
 * ---------------------------------------------------------------------------
 * OBS version gating
 * ---------------------------------------------------------------------------
 * libobs already refuses a plugin built against a NEWER libobs than the one
 * running: obs-module.c compares the module's version against LIBOBS_API_VER
 * with the patch bits masked off and returns MODULE_INCOMPATIBLE_VER. That
 * direction is handled for us, and handled before any of this plugin's code
 * runs - so there is nothing we can do to make it friendlier than the log line
 * OBS writes for itself.
 *
 * The opposite direction has no check at all. A plugin built against an older
 * libobs loads happily into a newer OBS and then crashes if any struct it
 * passes across the boundary has changed shape. obs_video_info and
 * obs_transform_info are exactly the sort of thing that changes between major
 * versions, and this plugin passes both.
 *
 * So the gate below covers the one case that is genuinely ours to catch:
 * running on a newer OBS major than we were built against. It refuses to
 * register anything and explains itself, rather than working right up until the
 * moment it corrupts the stack.
 *
 * Minor differences within a major are not gated - OBS keeps the API stable
 * across minors - but they are logged, because they are useful in a bug report.
 */

static uint32_t versionMajor(uint32_t ver)
{
	return (ver >> 24) & 0xFF;
}

static uint32_t versionMinor(uint32_t ver)
{
	return (ver >> 16) & 0xFF;
}

/* Set during obs_module_load so the deferred dialog knows what to say. */
static bool g_incompatible = false;

static void onFinishedLoadingWarn(enum obs_frontend_event event, void *)
{
	if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING)
		return;

	/* One warning per session, not one per frontend event. */
	obs_frontend_remove_event_callback(onFinishedLoadingWarn, nullptr);

	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());

	const QString text =
		QStringLiteral(
			"<p><b>Window Sizer has disabled itself.</b></p>"
			"<p>This plugin was built for <b>OBS Studio %1.x</b>, but you are running "
			"<b>OBS %2</b>.</p>"
			"<p>Between major versions OBS can change the shape of the data structures "
			"this plugin hands to it. That crashes OBS rather than failing cleanly, so "
			"the dock has not been registered.</p>"
			"<p>Install a build made for your version of OBS, or rebuild from source:<br>"
			"<a href=\"https://github.com/TGoodhew/obs-window-sizer/releases\">"
			"github.com/TGoodhew/obs-window-sizer/releases</a></p>")
			.arg(LIBOBS_API_MAJOR_VER)
			.arg(QString::fromUtf8(obs_get_version_string()));

	QMessageBox box(mainWindow);
	box.setWindowTitle(QStringLiteral("Window Sizer - incompatible OBS version"));
	box.setIcon(QMessageBox::Warning);
	box.setTextFormat(Qt::RichText);
	box.setText(text);
	box.setStandardButtons(QMessageBox::Ok);
	box.exec();
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Resize a target window and the OBS canvas to matching exact pixel dimensions";
}

bool obs_module_load(void)
{
	const uint32_t runtime = obs_get_version();

	obs_log(LOG_INFO, "loaded successfully (version %s), built against libobs %u.%u, running on OBS %s",
		PLUGIN_VERSION, (unsigned)LIBOBS_API_MAJOR_VER, (unsigned)LIBOBS_API_MINOR_VER,
		obs_get_version_string());

	if (versionMajor(runtime) != (uint32_t)LIBOBS_API_MAJOR_VER) {
		g_incompatible = true;
		obs_log(LOG_ERROR,
			"INCOMPATIBLE: built for OBS %u.x but running on OBS %u.%u. "
			"Refusing to register the dock. Install a matching build from "
			"https://github.com/TGoodhew/obs-window-sizer/releases",
			(unsigned)LIBOBS_API_MAJOR_VER, versionMajor(runtime), versionMinor(runtime));

		/* Nothing else is registered, but stay loaded so the warning can be
		 * shown once the main window exists. Only the frontend event API is
		 * touched, which is plain C with a stable signature. */
		obs_frontend_add_event_callback(onFinishedLoadingWarn, nullptr);
		return true;
	}

	if (versionMinor(runtime) != (uint32_t)LIBOBS_API_MINOR_VER) {
		obs_log(LOG_WARNING,
			"built against OBS %u.%u but running on OBS %u.%u. Same major version, so "
			"this is expected to work; mention it in any bug report.",
			(unsigned)LIBOBS_API_MAJOR_VER, (unsigned)LIBOBS_API_MINOR_VER, versionMajor(runtime),
			versionMinor(runtime));
	}

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
	if (g_incompatible)
		return;

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
	if (g_incompatible) {
		obs_frontend_remove_event_callback(onFinishedLoadingWarn, nullptr);
		obs_log(LOG_INFO, "unloaded");
		return;
	}

	obs_frontend_remove_dock(kDockId);
	obs_log(LOG_INFO, "unloaded");
}
