/*
obs-window-sizer
Copyright (C) 2026 Tony Goodhew <tony@schnauzergroup.com>

SPDX-License-Identifier: MIT
See LICENSE in the project root for the full licence text.
*/

#include "recording-config.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/config-file.h>
#include <util/dstr.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ws {

namespace {

/*
 * Encoder settings keys, read from plugins/obs-nvenc/nvenc-properties.c in the
 * OBS sources rather than assumed. The native NVENC implementations that OBS
 * 30.2 introduced do NOT use the old ffmpeg-based key names:
 *
 *   rate_control          "CBR" | "CQP" | "VBR" | "CQVBR" | "lossless"
 *   cqp                   int, the constant-quality level
 *   preset                "p1".."p7"  (not "quality"/"hq" strings)
 *   tune                  "uhq" | "hq" | "ll" | "ull"
 *   multipass             "disabled" | "qres" | "fullres"
 *                         -> "qres" IS two-pass quarter resolution
 *   adaptive_quantization bool
 *                         -> this is what used to be called psycho_aq;
 *                            nvenc-compat.c maps the old key onto it. Off,
 *                            because spatial AQ softens static UI text.
 *   profile               "main" | "main10" for HEVC
 */
constexpr const char *kRateControl = "CQP";
constexpr const char *kPreset = "p6";
constexpr const char *kTune = "hq";
constexpr const char *kMultipass = "qres";
constexpr const char *kProfileMain = "main";

/*
 * Pick the best available HEVC NVENC encoder. Enumerated at runtime rather
 * than hardcoded: OBS 32 dropped the ffmpeg-based NVENC encoders on this
 * machine, and which ones are registered depends on driver and GPU.
 */
std::string pickEncoder(std::string &note, bool logAll)
{
	std::vector<std::string> ids;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); ++i) {
		if (id)
			ids.push_back(id);
	}

	std::string allIds;
	for (const std::string &candidate : ids) {
		if (!allIds.empty())
			allIds += ", ";
		allIds += candidate;
	}
	if (logAll)
		obs_log(LOG_INFO, "registered video/audio encoder types: %s", allIds.c_str());

	auto isNvenc = [](const std::string &candidate) {
		return candidate.find("nvenc") != std::string::npos;
	};
	auto codecIs = [](const std::string &candidate, const char *codec) {
		const char *c = obs_get_encoder_codec(candidate.c_str());
		return c && strcmp(c, codec) == 0;
	};

	/* Preferred: the native implementation, texture-based. */
	for (const std::string &candidate : ids) {
		if (candidate == "obs_nvenc_hevc_tex")
			return candidate;
	}
	/* Any other HEVC NVENC (including the older ffmpeg_hevc_nvenc). */
	for (const std::string &candidate : ids) {
		if (isNvenc(candidate) && codecIs(candidate, "hevc")) {
			note = "using " + candidate + " (obs_nvenc_hevc_tex not registered)";
			return candidate;
		}
	}
	/* Fall back to H.264 NVENC so the feature still does something useful
	 * on a GPU or driver without HEVC encode. */
	for (const std::string &candidate : ids) {
		if (isNvenc(candidate) && codecIs(candidate, "h264")) {
			note = "no HEVC NVENC encoder registered; fell back to " + candidate;
			return candidate;
		}
	}

	return std::string();
}

} // namespace

bool persistCanvasSize(int width, int height, std::string &error)
{
	config_t *config = obs_frontend_get_profile_config();
	if (!config) {
		error = "Could not open the profile configuration.";
		return false;
	}

	/* The same four keys, in the same section, that OBS's own
	 * "Resize output (source size)" feature writes. */
	config_set_uint(config, "Video", "BaseCX", (uint64_t)width);
	config_set_uint(config, "Video", "BaseCY", (uint64_t)height);
	config_set_uint(config, "Video", "OutputCX", (uint64_t)width);
	config_set_uint(config, "Video", "OutputCY", (uint64_t)height);

	if (config_save_safe(config, "tmp", nullptr) != CONFIG_SUCCESS) {
		error = "Could not save the profile configuration.";
		return false;
	}

	obs_log(LOG_INFO, "canvas %dx%d written to profile config (survives restart)", width, height);
	return true;
}

std::string describeRecordingEncoder()
{
	std::string note;
	const std::string id = pickEncoder(note, false);
	if (id.empty())
		return std::string();

	const char *display = obs_encoder_get_display_name(id.c_str());
	return display ? display : id;
}

std::string checkRecordingScaling(int canvasWidth, int canvasHeight)
{
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return std::string();

	const char *mode = config_get_string(config, "Output", "Mode");
	if (!mode || strcmp(mode, "Advanced") != 0)
		return std::string(); /* Simple mode has no recording rescale. */

	const char *recEncoder = config_get_string(config, "AdvOut", "RecEncoder");
	if (recEncoder && astrcmpi(recEncoder, "none") == 0)
		return std::string(); /* Uses the stream encoder; not our business here. */

	const int filter = (int)config_get_int(config, "AdvOut", "RecRescaleFilter");
	const char *res = config_get_string(config, "AdvOut", "RecRescaleRes");
	if (filter == OBS_SCALE_DISABLE || !res || !*res)
		return std::string();

	unsigned int cx = 0, cy = 0;
	if (sscanf(res, "%ux%u", &cx, &cy) != 2 || ((int)cx == canvasWidth && (int)cy == canvasHeight))
		return std::string();

	return std::string(" Warning: the profile rescales recordings to ") + res +
	       ", so the file will NOT be 1:1 with the window. Tick 'Configure recording' to clear it, "
	       "or clear Settings > Output > Recording > Rescale Output.";
}

bool wouldSwitchOutputMode()
{
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return false;

	const char *mode = config_get_string(config, "Output", "Mode");
	return !mode || strcmp(mode, "Advanced") != 0;
}

RecordingConfigResult configureRecording(int cq)
{
	RecordingConfigResult result;

	config_t *config = obs_frontend_get_profile_config();
	if (!config) {
		result.message = "Could not open the profile configuration.";
		return result;
	}

	std::string note;
	const std::string encoderId = pickEncoder(note, true);
	if (encoderId.empty()) {
		result.message = "No NVENC encoder is registered on this machine.";
		return result;
	}
	result.encoderId = encoderId;

	/* ---- 1. The encoder settings the frontend re-reads every start ---- */
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "rate_control", kRateControl);
	obs_data_set_int(settings, "cqp", cq);
	obs_data_set_string(settings, "preset", kPreset);
	obs_data_set_string(settings, "tune", kTune);
	obs_data_set_string(settings, "multipass", kMultipass);
	obs_data_set_bool(settings, "adaptive_quantization", false);
	obs_data_set_string(settings, "profile", kProfileMain);
	obs_data_set_int(settings, "bf", 2);
	obs_data_set_int(settings, "keyint_sec", 0);
	obs_data_set_bool(settings, "lookahead", false);

	char *profilePath = obs_frontend_get_current_profile_path();
	if (!profilePath) {
		obs_data_release(settings);
		result.message = "Could not locate the current profile folder.";
		return result;
	}

	const std::string encoderFile = std::string(profilePath) + "/recordEncoder.json";
	bfree(profilePath);

	const bool saved = obs_data_save_json_safe(settings, encoderFile.c_str(), "tmp", "bak");
	obs_log(LOG_INFO, "recordEncoder.json (%s): %s", saved ? "written" : "FAILED", obs_data_get_json(settings));
	obs_data_release(settings);

	if (!saved) {
		result.message = "Could not write recordEncoder.json into the profile folder.";
		return result;
	}

	/* ---- 2. Point the profile at that encoder ---- */
	const char *mode = config_get_string(config, "Output", "Mode");
	const bool wasSimple = !mode || strcmp(mode, "Advanced") != 0;

	if (wasSimple) {
		/*
		 * Simple mode builds its encoder settings in code from a
		 * quality enum, so there is no CQ / preset / multipass control
		 * to write. Advanced mode is the only one that reads
		 * recordEncoder.json.
		 *
		 * Carry the recording folder across so the switch does not
		 * silently move where files land.
		 */
		const char *simplePath = config_get_string(config, "SimpleOutput", "FilePath");
		if (simplePath && *simplePath)
			config_set_string(config, "AdvOut", "RecFilePath", simplePath);

		const char *simpleFormat = config_get_string(config, "SimpleOutput", "RecFormat2");
		if (simpleFormat && *simpleFormat)
			config_set_string(config, "AdvOut", "RecFormat2", simpleFormat);

		config_set_string(config, "Output", "Mode", "Advanced");
		result.requiresRestart = true;
	}

	config_set_string(config, "AdvOut", "RecType", "Standard");
	config_set_string(config, "AdvOut", "RecEncoder", encoderId.c_str());

	/*
	 * Guarantee the file is written at the canvas size - which is the
	 * window size - with no resampling anywhere in the chain.
	 *
	 * AdvancedOutput::SetupRecording() calls obs_encoder_set_scaled_size()
	 * whenever RecRescaleFilter is not OBS_SCALE_DISABLE and RecRescaleRes
	 * is a non-empty "WxH". That is a second, independent downscale sitting
	 * after the canvas, and nothing in OBS's main window shows it, so it is
	 * turned off explicitly rather than left to a default.
	 */
	config_set_int(config, "AdvOut", "RecRescaleFilter", OBS_SCALE_DISABLE);
	config_set_string(config, "AdvOut", "RecRescaleRes", "");

	if (config_save_safe(config, "tmp", nullptr) != CONFIG_SUCCESS) {
		result.message = "Could not save the profile configuration.";
		return result;
	}

	result.ok = true;
	result.message = "Recording set to " + encoderId + ", CQP " + std::to_string(cq) + ", preset " + kPreset +
			 ", two-pass quarter-res, adaptive quantization off, no rescale (records at "
			 "canvas size).";
	if (!note.empty())
		result.message += " (" + note + ")";

	obs_log(LOG_INFO, "%s%s", result.message.c_str(),
		result.requiresRestart ? " [profile switched Simple -> Advanced; restart required]" : "");

	return result;
}

} // namespace ws
