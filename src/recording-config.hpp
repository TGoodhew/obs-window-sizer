/*
obs-window-sizer
Copyright (C) 2026 Tony Goodhew <tony@schnauzergroup.com>

SPDX-License-Identifier: MIT
See LICENSE in the project root for the full licence text.
*/

#pragma once

#include <string>

namespace ws {

/*
 * Recording configuration, "Option C": rather than reaching into the
 * frontend's live encoder (which the frontend overwrites from profile config
 * on every recording start), write the configuration the frontend itself reads
 * from. AdvancedOutput::UpdateRecordingSettings() re-reads recordEncoder.json
 * out of the profile folder at every StartRecording, so the settings stick and
 * OBS's own Record button stays the thing that starts a recording.
 */
struct RecordingConfigResult {
	bool ok = false;
	std::string message;

	/* Encoder actually chosen, e.g. "obs_nvenc_hevc_tex". */
	std::string encoderId;

	/*
	 * True when the profile had to be switched from Simple to Advanced
	 * output mode. The frontend only rebuilds its output handler in
	 * ResetOutputs(), which a plugin cannot reach, so in that case the
	 * change does not take effect until OBS is restarted.
	 */
	bool requiresRestart = false;
};

/* Persist a canvas size into the profile so it survives an OBS restart.
 * obs_reset_video() alone only changes the running pipeline. */
bool persistCanvasSize(int width, int height, std::string &error);

/*
 * Friendly name of the encoder that configureRecording() would choose, e.g.
 * "NVIDIA NVENC HEVC", for showing in the UI so the user is not left guessing
 * what "GPU encoding" resolved to. Empty if no NVENC encoder is registered, in
 * which case the feature cannot do anything and the UI should say so.
 */
std::string describeRecordingEncoder();

/*
 * Point the profile's recording output at HEVC NVENC with constant-quality
 * settings tuned for sharp UI text, and disable the recording rescale so the
 * file is written at the canvas size. Returns what was chosen and whether a
 * restart is needed.
 */
RecordingConfigResult configureRecording(int cq);

/*
 * True when configureRecording() would have to switch the profile from Simple
 * to Advanced output mode. That switch also changes where STREAMING settings
 * come from, which is a surprise worth confirming before it happens rather
 * than reporting afterwards.
 */
bool wouldSwitchOutputMode();

/*
 * Read-only check that the profile would record at the canvas size rather than
 * resampling to something else. Returns a warning to show the user, or an empty
 * string when the recording is 1:1. Used when the dock has not been asked to
 * rewrite the recording configuration, so a pre-existing rescale is reported
 * rather than silently changed.
 */
std::string checkRecordingScaling(int canvasWidth, int canvasHeight);

} // namespace ws
