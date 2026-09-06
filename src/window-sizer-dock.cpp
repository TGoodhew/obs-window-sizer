/*
obs-window-sizer
Copyright (C) 2026 Tony Goodhew <tony@schnauzergroup.com>

SPDX-License-Identifier: MIT
See LICENSE in the project root for the full licence text.
*/

#include "window-sizer-dock.hpp"
#include "win32-window.hpp"
#include "recording-config.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QTimer>
#include <QVBoxLayout>

#include <cstring>
#include <string>

namespace {

/* The source type we drive. Confirmed registered as "window_capture" by
 * plugins/win-capture/window-capture.c in OBS 32.2.2. */
constexpr const char *kWindowCaptureId = "window_capture";

/* The name given to a capture source when the plugin has to create one. */
constexpr const char *kCreatedSourceName = "Window Sizer Capture";

/*
 * QComboBox truncates its closed-state text rather than eliding it, and
 * setTextElideMode() only affects the dropdown list. Window titles are
 * routinely longer than any sensible dock width, so paint the closed state
 * ourselves with middle elision - that keeps the "[executable.exe]" prefix and
 * the tail of the title, which is what tells two similar windows apart.
 */
class ElidingComboBox : public QComboBox {
public:
	explicit ElidingComboBox(QWidget *parent = nullptr) : QComboBox(parent) {}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QStylePainter painter(this);
		painter.setPen(palette().color(QPalette::Text));

		QStyleOptionComboBox opt;
		initStyleOption(&opt);
		painter.drawComplexControl(QStyle::CC_ComboBox, opt);

		const QRect textRect =
			style()->subControlRect(QStyle::CC_ComboBox, &opt, QStyle::SC_ComboBoxEditField, this);
		opt.currentText = painter.fontMetrics().elidedText(opt.currentText, Qt::ElideMiddle, textRect.width());
		painter.drawControl(QStyle::CE_ComboBoxLabel, opt);
	}
};

struct Preset {
	const char *label;
	int width;
	int height;
};

/*
 * Named presets, ascending. The consumer names are used deliberately: "2K"
 * colloquially means 1440p even though DCI 2K is 2048x1080, so both the name
 * and the pixel dimensions are shown and there is nothing left to guess at.
 * 4K here is UHD (3840x2160), not DCI 4K (4096x2160).
 */
constexpr Preset kPresets[] = {
	{"720p HD - 1280 x 720", 1280, 720},       {"1080p FHD - 1920 x 1080", 1920, 1080},
	{"1440p 2K QHD - 2560 x 1440", 2560, 1440}, {"2160p 4K UHD - 3840 x 2160", 3840, 2160},
	{"Vertical - 1080 x 1920", 1080, 1920},    {"Square - 1080 x 1080", 1080, 1080},
};

/* 1080p, the common case, rather than whichever preset happens to sort first. */
constexpr int kDefaultPresetIndex = 1;
constexpr int kPresetCount = (int)(sizeof(kPresets) / sizeof(kPresets[0]));

/* Result of hunting for a window capture source in the current scene. Both
 * members carry a reference the caller must release. */
struct FoundCapture {
	obs_source_t *source = nullptr;
	obs_sceneitem_t *item = nullptr;
};

bool findCaptureItem(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *found = static_cast<FoundCapture *>(param);

	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source)
		return true; /* keep looking */

	const char *id = obs_source_get_unversioned_id(source);
	if (!id || strcmp(id, kWindowCaptureId) != 0)
		return true;

	found->source = obs_source_get_ref(source);
	obs_sceneitem_addref(item);
	found->item = item;
	return false; /* stop enumerating */
}

/*
 * Translates an obs_reset_video() return code into something a person can act
 * on. Codes are from libobs/obs-defs.h.
 */
QString describeVideoResetCode(int code)
{
	switch (code) {
	case OBS_VIDEO_SUCCESS:
		return QStringLiteral("success");
	case OBS_VIDEO_CURRENTLY_ACTIVE:
		return QStringLiteral("an output is still active, so the canvas is locked");
	case OBS_VIDEO_NOT_SUPPORTED:
		return QStringLiteral("the graphics adapter does not support that video configuration");
	case OBS_VIDEO_INVALID_PARAM:
		return QStringLiteral("the video configuration was rejected as invalid");
	case OBS_VIDEO_MODULE_NOT_FOUND:
		return QStringLiteral("the graphics module could not be found");
	case OBS_VIDEO_FAIL:
		return QStringLiteral("libobs reported a general failure");
	default:
		return QStringLiteral("unknown error code %1").arg(code);
	}
}

} // namespace

WindowSizerDock::WindowSizerDock(QWidget *parent) : QWidget(parent)
{
	buildUi();
	/* The first population happens on OBS_FRONTEND_EVENT_FINISHED_LOADING.
	 * Refreshing here would run before win-capture is necessarily loaded and
	 * would show a spurious error on startup. */
	obs_frontend_add_event_callback(onFrontendEvent, this);
}

WindowSizerDock::~WindowSizerDock()
{
	obs_frontend_remove_event_callback(onFrontendEvent, this);
}

void WindowSizerDock::scheduleRefresh()
{
	/* Restarting the timer on each request means a burst of frontend events
	 * collapses into one refresh shortly after the last of them. */
	m_refreshTimer->start();
}

void WindowSizerDock::buildUi()
{
	/* Short enough to feel immediate, long enough to swallow the startup
	 * burst of FINISHED_LOADING + SCENE_CHANGED + SCENE_LIST_CHANGED. */
	m_refreshTimer = new QTimer(this);
	m_refreshTimer->setSingleShot(true);
	m_refreshTimer->setInterval(150);
	connect(m_refreshTimer, &QTimer::timeout, this, &WindowSizerDock::refreshWindows);

	auto *layout = new QVBoxLayout(this);

	auto *form = new QFormLayout();

	/* Target window ------------------------------------------------- */
	m_windowCombo = new ElidingComboBox(this);
	m_windowCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	m_windowCombo->setMinimumContentsLength(16);
	/* Window titles are routinely longer than any sensible dock width. Elide
	 * in the middle so the executable prefix and the tail of the title both
	 * stay visible, and keep the full text available as a tooltip. */
	m_windowCombo->view()->setTextElideMode(Qt::ElideMiddle);
	connect(m_windowCombo, &QComboBox::currentIndexChanged, this, [this](int) {
		/* Full title first, since that is the bit that gets elided, then the
		 * explanation of where the list comes from. */
		const QString full = m_windowCombo->currentText();
		m_windowCombo->setToolTip(full.isEmpty()
						  ? QString::fromUtf8(obs_module_text("WindowSizer.TargetWindow.Tip"))
						  : QStringLiteral("<p><b>%1</b></p>%2")
							    .arg(full.toHtmlEscaped(),
								 QString::fromUtf8(obs_module_text(
									 "WindowSizer.TargetWindow.Tip"))));
	});
	m_windowCombo->setToolTip(obs_module_text("WindowSizer.TargetWindow.Tip"));

	m_refreshButton = new QPushButton(obs_module_text("WindowSizer.Refresh"), this);
	m_refreshButton->setToolTip(obs_module_text("WindowSizer.Refresh.Tip"));

	auto *windowRow = new QHBoxLayout();
	windowRow->addWidget(m_windowCombo, 1);
	windowRow->addWidget(m_refreshButton, 0);
	form->addRow(obs_module_text("WindowSizer.TargetWindow"), windowRow);

	/* Size ----------------------------------------------------------- */
	m_presetCombo = new QComboBox(this);
	for (const Preset &preset : kPresets)
		m_presetCombo->addItem(QString::fromUtf8(preset.label));
	m_presetCombo->addItem(obs_module_text("WindowSizer.Custom"));
	m_presetCombo->setCurrentIndex(kDefaultPresetIndex);
	m_presetCombo->setToolTip(obs_module_text("WindowSizer.Size.Tip"));
	form->addRow(obs_module_text("WindowSizer.Size"), m_presetCombo);

	m_widthSpin = new QSpinBox(this);
	m_widthSpin->setRange(1, 16384);
	m_widthSpin->setValue(kPresets[kDefaultPresetIndex].width);

	m_heightSpin = new QSpinBox(this);
	m_heightSpin->setRange(1, 16384);
	m_heightSpin->setValue(kPresets[kDefaultPresetIndex].height);

	const QString sizeTip = QString::fromUtf8(obs_module_text("WindowSizer.Size.Spin.Tip"));
	m_widthSpin->setToolTip(sizeTip);
	m_heightSpin->setToolTip(sizeTip);

	auto *widthLabel = new QLabel(obs_module_text("WindowSizer.Width"), this);
	auto *heightLabel = new QLabel(obs_module_text("WindowSizer.Height"), this);
	widthLabel->setToolTip(sizeTip);
	heightLabel->setToolTip(sizeTip);

	auto *sizeRow = new QHBoxLayout();
	sizeRow->addWidget(widthLabel);
	sizeRow->addWidget(m_widthSpin, 1);
	sizeRow->addWidget(heightLabel);
	sizeRow->addWidget(m_heightSpin, 1);
	form->addRow(QString(), sizeRow);

	/* Client area ---------------------------------------------------- */
	m_clientAreaCheck = new QCheckBox(obs_module_text("WindowSizer.SizeClientArea"), this);
	/* Matches the window_capture source default, which is client_area on. */
	m_clientAreaCheck->setChecked(true);
	m_clientAreaCheck->setToolTip(obs_module_text("WindowSizer.SizeClientArea.Tip"));
	form->addRow(QString(), m_clientAreaCheck);

	/* Recording ------------------------------------------------------ */
	m_configureRecordingCheck = new QCheckBox(obs_module_text("WindowSizer.ConfigureRecording"), this);
	m_configureRecordingCheck->setChecked(false);

	/* Say which encoder this actually resolves to rather than leaving the
	 * user to guess what "GPU encoding" means on their machine. */
	const std::string encoderName = ws::describeRecordingEncoder();
	if (encoderName.empty()) {
		m_configureRecordingCheck->setEnabled(false);
		m_configureRecordingCheck->setToolTip(
			obs_module_text("WindowSizer.ConfigureRecording.Unavailable"));
	} else {
		m_configureRecordingCheck->setToolTip(
			QString::fromUtf8(obs_module_text("WindowSizer.ConfigureRecording.Tip"))
				.arg(QString::fromStdString(encoderName)));
	}
	form->addRow(QString(), m_configureRecordingCheck);

	/*
	 * The checkbox label has to stay short enough not to be clipped, so the
	 * consequence that actually matters - that ticking it rewrites settings
	 * the user may have chosen deliberately - goes in a dimmed line beneath
	 * it where it can wrap.
	 */
	auto *recordingHint = new QLabel(obs_module_text("WindowSizer.ConfigureRecording.Hint"), this);
	recordingHint->setWordWrap(true);
	recordingHint->setEnabled(false);
	/* A word-wrapped QLabel inside a QFormLayout is not given enough height
	 * for its wrapped lines, and the tail gets clipped. Reserve two lines. */
	recordingHint->setMinimumHeight(recordingHint->fontMetrics().height() * 2 + 4);
	form->addRow(QString(), recordingHint);

	m_cqSpin = new QSpinBox(this);
	m_cqSpin->setRange(10, 30);
	m_cqSpin->setValue(16);
	m_cqSpin->setEnabled(false);
	m_cqSpin->setToolTip(obs_module_text("WindowSizer.CQ.Tip"));

	auto *cqRow = new QHBoxLayout();
	auto *cqLabel = new QLabel(obs_module_text("WindowSizer.CQ"), this);
	cqLabel->setToolTip(m_cqSpin->toolTip());
	cqRow->addWidget(cqLabel);
	cqRow->addWidget(m_cqSpin, 1);
	cqRow->addStretch(1);
	form->addRow(QString(), cqRow);

	layout->addLayout(form);

	/* Apply ---------------------------------------------------------- */
	m_applyButton = new QPushButton(obs_module_text("WindowSizer.Apply"), this);
	m_applyButton->setToolTip(obs_module_text("WindowSizer.Apply.Tip"));
	layout->addWidget(m_applyButton);

	/* Status --------------------------------------------------------- */
	m_statusLabel = new QLabel(obs_module_text("WindowSizer.Status.Ready"), this);
	m_statusLabel->setWordWrap(true);
	m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	m_statusLabel->setToolTip(obs_module_text("WindowSizer.Status.Tip"));
	/* Apply can produce a long message - a refusal, plus an odd-dimension
	 * warning, plus the recording result. Reserve room for it rather than
	 * clipping, and let it grow beyond that if a message is longer still. */
	m_statusLabel->setMinimumHeight(m_statusLabel->fontMetrics().height() * 3 + 4);
	m_statusLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
	m_statusLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);
	layout->addWidget(m_statusLabel);

	layout->addStretch(1);

	/* Report an honest minimum to the dock area so the dock cannot be sized
	 * smaller than its contents can render. */
	layout->setSizeConstraint(QLayout::SetMinimumSize);

	connect(m_refreshButton, &QPushButton::clicked, this, &WindowSizerDock::refreshWindows);
	connect(m_applyButton, &QPushButton::clicked, this, &WindowSizerDock::onApply);
	connect(m_presetCombo, &QComboBox::currentIndexChanged, this, &WindowSizerDock::onPresetChanged);
	connect(m_widthSpin, &QSpinBox::valueChanged, this, &WindowSizerDock::onSizeEdited);
	connect(m_heightSpin, &QSpinBox::valueChanged, this, &WindowSizerDock::onSizeEdited);
	connect(m_configureRecordingCheck, &QCheckBox::toggled, m_cqSpin, &QSpinBox::setEnabled);
}

void WindowSizerDock::onPresetChanged(int index)
{
	if (index < 0 || index >= kPresetCount)
		return; /* the Custom entry - leave the spin boxes alone */

	m_updatingSize = true;
	{
		QSignalBlocker blockWidth(m_widthSpin);
		QSignalBlocker blockHeight(m_heightSpin);
		m_widthSpin->setValue(kPresets[index].width);
		m_heightSpin->setValue(kPresets[index].height);
	}
	m_updatingSize = false;
}

void WindowSizerDock::onSizeEdited()
{
	if (m_updatingSize)
		return;

	const int width = m_widthSpin->value();
	const int height = m_heightSpin->value();

	for (int i = 0; i < kPresetCount; ++i) {
		if (kPresets[i].width == width && kPresets[i].height == height) {
			QSignalBlocker block(m_presetCombo);
			m_presetCombo->setCurrentIndex(i);
			return;
		}
	}

	QSignalBlocker block(m_presetCombo);
	m_presetCombo->setCurrentIndex(m_presetCombo->count() - 1); /* Custom */
}

void WindowSizerDock::setStatus(const QString &text, bool isError)
{
	m_statusLabel->setText(text);
	m_statusLabel->setStyleSheet(isError ? QStringLiteral("color: #d9534f;") : QString());
}

void WindowSizerDock::refreshWindows()
{
	const QString previous = m_windowCombo->currentData().toString();

	QSignalBlocker block(m_windowCombo);
	m_windowCombo->clear();

	/*
	 * Take the list from the capture source's own "window" property rather
	 * than enumerating windows here. The value strings are escaped
	 * "title:class:executable" and the escaping rules are easy to get
	 * wrong; reading them from the code that populates OBS's own dropdown
	 * makes them correct by definition.
	 *
	 * obs_get_source_properties() builds the property list without a source
	 * instance. Verified safe for window_capture in OBS 32.2.2: its
	 * wc_properties() null-checks its data pointer, and both of its
	 * modified callbacks bail out early when the param is null.
	 */
	obs_properties_t *props = obs_get_source_properties(kWindowCaptureId);
	if (!props) {
		setStatus(QStringLiteral("Could not read the window capture source properties. "
					 "Is win-capture loaded?"),
			  true);
		return;
	}

	obs_property_t *windowProp = obs_properties_get(props, "window");
	if (!windowProp) {
		obs_properties_destroy(props);
		setStatus(QStringLiteral("The window capture source has no 'window' property on this "
					 "OBS build."),
			  true);
		return;
	}

	const size_t count = obs_property_list_item_count(windowProp);
	for (size_t i = 0; i < count; ++i) {
		if (obs_property_list_item_disabled(windowProp, i))
			continue;

		const char *name = obs_property_list_item_name(windowProp, i);
		const char *value = obs_property_list_item_string(windowProp, i);
		if (!name || !value || !*value)
			continue;

		m_windowCombo->addItem(QString::fromUtf8(name), QString::fromUtf8(value));
		/* Full title as a tooltip, so an elided entry in the dropdown is
		 * still identifiable when two windows share a prefix. */
		m_windowCombo->setItemData(m_windowCombo->count() - 1, QString::fromUtf8(name), Qt::ToolTipRole);
	}

	obs_properties_destroy(props);

	if (!previous.isEmpty()) {
		const int index = m_windowCombo->findData(previous);
		if (index >= 0)
			m_windowCombo->setCurrentIndex(index);
	}

	obs_log(LOG_INFO, "window list refreshed, %d capturable window(s)", m_windowCombo->count());
}

bool WindowSizerDock::applyCanvasSize(int width, int height, QString &error)
{
	struct obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi)) {
		error = QStringLiteral("Could not read the current video settings.");
		return false;
	}

	/* Only the dimensions change. FPS, colour space, range, scaling and the
	 * graphics module are carried through untouched. */
	ovi.base_width = (uint32_t)width;
	ovi.base_height = (uint32_t)height;
	ovi.output_width = (uint32_t)width;
	ovi.output_height = (uint32_t)height;

	const int code = obs_reset_video(&ovi);
	if (code != OBS_VIDEO_SUCCESS) {
		error = QStringLiteral("Canvas resize failed: %1.").arg(describeVideoResetCode(code));
		return false;
	}

	obs_log(LOG_INFO, "canvas set to %dx%d (base and output)", width, height);

	/* obs_reset_video() only changes the running pipeline; without this the
	 * canvas reverts to the profile's resolution on the next OBS start. */
	std::string persistError;
	if (!ws::persistCanvasSize(width, height, persistError)) {
		obs_log(LOG_WARNING, "canvas applied but not persisted: %s", persistError.c_str());
	}

	return true;
}

bool WindowSizerDock::applyCaptureSource(const QString &windowValue, bool clientArea, int priority, QString &error)
{
	obs_source_t *sceneSource = obs_frontend_get_current_scene();
	if (!sceneSource) {
		error = QStringLiteral("No current scene.");
		return false;
	}

	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene) {
		obs_source_release(sceneSource);
		error = QStringLiteral("The current scene is not a scene (a group is selected?).");
		return false;
	}

	FoundCapture found;
	obs_scene_enum_items(scene, findCaptureItem, &found);

	bool created = false;
	if (!found.source) {
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "window", windowValue.toUtf8().constData());
		obs_data_set_bool(settings, "client_area", clientArea);
		obs_data_set_int(settings, "priority", priority);

		found.source = obs_source_create(kWindowCaptureId, kCreatedSourceName, settings, nullptr);
		obs_data_release(settings);

		if (!found.source) {
			obs_source_release(sceneSource);
			error = QStringLiteral("Could not create a window capture source.");
			return false;
		}

		found.item = obs_scene_add(scene, found.source);
		if (found.item)
			obs_sceneitem_addref(found.item);
		created = true;
	} else {
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "window", windowValue.toUtf8().constData());
		obs_data_set_bool(settings, "client_area", clientArea);
		obs_data_set_int(settings, "priority", priority);
		obs_source_update(found.source, settings);
		obs_data_release(settings);
	}

	/* Log the settings actually in effect. The key names for capture
	 * method, client area and cursor have moved between OBS versions, so
	 * this is worth having in the log when something looks wrong. */
	obs_data_t *effective = obs_source_get_settings(found.source);
	if (effective) {
		obs_log(LOG_INFO, "capture source '%s' (%s) settings: %s", obs_source_get_name(found.source),
			created ? "created" : "updated", obs_data_get_json(effective));
		obs_data_release(effective);
	}

	/* Reset the transform so the capture lands 1:1 at the canvas origin. */
	if (found.item) {
		struct obs_transform_info info = {};
		obs_sceneitem_get_info2(found.item, &info);

		info.pos.x = 0.0f;
		info.pos.y = 0.0f;
		info.rot = 0.0f;
		info.scale.x = 1.0f;
		info.scale.y = 1.0f;
		info.alignment = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
		info.bounds_type = OBS_BOUNDS_NONE;
		info.bounds_alignment = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
		info.bounds.x = 0.0f;
		info.bounds.y = 0.0f;
		info.crop_to_bounds = false;
		obs_sceneitem_set_info2(found.item, &info);

		struct obs_sceneitem_crop crop = {};
		obs_sceneitem_set_crop(found.item, &crop);

		obs_log(LOG_INFO, "transform reset: pos 0,0 scale 1.0 align top-left bounds none crop 0");
	} else {
		obs_log(LOG_WARNING, "capture source has no scene item; transform not reset");
	}

	if (found.item)
		obs_sceneitem_release(found.item);
	obs_source_release(found.source);
	obs_source_release(sceneSource);
	return true;
}

void WindowSizerDock::onApply()
{
	/*
	 * Step 1 - refuse while an output is running. obs_reset_video() would
	 * return OBS_VIDEO_CURRENTLY_ACTIVE anyway, but saying so up front is
	 * clearer than resizing the window and then failing on the canvas.
	 */
	if (obs_frontend_recording_active()) {
		setStatus(QStringLiteral("Recording is active - stop it before changing the canvas size."), true);
		return;
	}
	if (obs_frontend_streaming_active()) {
		setStatus(QStringLiteral("Streaming is active - stop it before changing the canvas size."), true);
		return;
	}

	const QString windowValue = m_windowCombo->currentData().toString();
	if (windowValue.isEmpty()) {
		setStatus(QStringLiteral("Choose a target window first."), true);
		return;
	}

	const int targetWidth = m_widthSpin->value();
	const int targetHeight = m_heightSpin->value();
	const bool clientArea = m_clientAreaCheck->isChecked();

	/*
	 * Match with the same priority we write into the source, so the window
	 * we resize and the window OBS captures agree by construction. The
	 * window_capture source defaults "priority" to 0 (WINDOW_PRIORITY_CLASS)
	 * by leaving it unset, so mirror that.
	 */
	const int priority = ws::MatchClass;

	/* Step 2 - resize the target window. */
	void *hwnd = ws::findWindow(windowValue.toUtf8().constData(), priority);
	if (!hwnd) {
		setStatus(QStringLiteral("Could not find that window any more. Press Refresh and try again."), true);
		return;
	}

	obs_log(LOG_INFO, "applying %dx%d (%s) to window: %s", targetWidth, targetHeight,
		clientArea ? "client area" : "visible frame", ws::describeWindow(hwnd).c_str());

	const ws::ResizeOutcome outcome = ws::resizeWindow(hwnd, targetWidth, targetHeight, clientArea);
	if (!outcome.ok) {
		setStatus(QStringLiteral("Resize failed: %1").arg(QString::fromStdString(outcome.error)), true);
		return;
	}

	obs_log(LOG_INFO,
		"resize result: requested %dx%d, achieved %dx%d "
		"(outer %dx%d, visible %dx%d, client %dx%d)",
		outcome.requestedWidth, outcome.requestedHeight, outcome.achievedWidth, outcome.achievedHeight,
		outcome.after.outerWidth, outcome.after.outerHeight, outcome.after.visibleWidth,
		outcome.after.visibleHeight, outcome.after.clientWidth, outcome.after.clientHeight);

	/* Step 3 - canvas follows whatever the window actually became, so the
	 * two match even when the window refused the requested size. */
	QString error;
	if (!applyCanvasSize(outcome.achievedWidth, outcome.achievedHeight, error)) {
		setStatus(error, true);
		return;
	}

	/* Step 4 and 5 - capture source settings and transform. */
	if (!applyCaptureSource(windowValue, clientArea, priority, error)) {
		setStatus(error, true);
		return;
	}

	/*
	 * Step 6 - optionally point the profile's recording output at HEVC
	 * NVENC. Done after the canvas so the encoder is configured for the
	 * size actually in use.
	 */
	QString recordingNote;
	if (!m_configureRecordingCheck->isChecked()) {
		/* Not asked to rewrite the recording config, so report a
		 * pre-existing rescale rather than silently changing it. */
		recordingNote = QString::fromStdString(
			ws::checkRecordingScaling(outcome.achievedWidth, outcome.achievedHeight));
	} else {
		const ws::RecordingConfigResult rec = ws::configureRecording(m_cqSpin->value());
		if (!rec.ok) {
			recordingNote = QStringLiteral(" Recording NOT configured: %1")
						.arg(QString::fromStdString(rec.message));
		} else if (rec.requiresRestart) {
			recordingNote = QStringLiteral(" Recording set to %1 - restart OBS for the "
						       "Simple-to-Advanced output mode switch to take effect.")
						.arg(QString::fromStdString(rec.encoderId));
		} else {
			recordingNote = QStringLiteral(" Recording set to %1 (CQ %2).")
						.arg(QString::fromStdString(rec.encoderId))
						.arg(m_cqSpin->value());
		}
	}

	/*
	 * NV12 needs even dimensions, and OBS's own "resize output to source"
	 * rounds width to a multiple of 4 and height to a multiple of 2. We
	 * keep the canvas pixel-exact instead of rounding, because rounding
	 * would quietly break the 1:1 promise - but say so, because an odd
	 * canvas can upset an encoder.
	 */
	QString parityNote;
	if ((outcome.achievedWidth % 4) != 0 || (outcome.achievedHeight % 2) != 0) {
		parityNote = QStringLiteral(" Note: %1x%2 is not a multiple of 4x2, which some encoders "
					    "refuse; nudge the size if recording fails.")
				     .arg(outcome.achievedWidth)
				     .arg(outcome.achievedHeight);
	}

	/* Step 7 - report. */
	QString status;
	if (outcome.exact()) {
		status = QStringLiteral("Applied: %1 x %2 (%3), canvas matched 1:1.")
				 .arg(outcome.achievedWidth)
				 .arg(outcome.achievedHeight)
				 .arg(clientArea ? QStringLiteral("client area") : QStringLiteral("visible frame"));
		setStatus(status + parityNote + recordingNote, false);
	} else {
		status = QStringLiteral("Window refused %1 x %2 and settled at %3 x %4 "
					"(minimum size or fixed aspect ratio). Canvas set to %3 x %4 to match.")
				 .arg(outcome.requestedWidth)
				 .arg(outcome.requestedHeight)
				 .arg(outcome.achievedWidth)
				 .arg(outcome.achievedHeight);
		setStatus(status + parityNote + recordingNote, true);
	}

	if (outcome.restoredFromMaximized)
		obs_log(LOG_INFO, "target window was maximized or minimized and was restored first");
}

void WindowSizerDock::onFrontendEvent(enum obs_frontend_event event, void *data)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		break;
	default:
		return;
	}

	auto *dock = static_cast<WindowSizerDock *>(data);
	/* Queued so the work happens on the Qt thread no matter which thread the
	 * frontend raised the event on, and debounced because OBS raises several
	 * of these within a few milliseconds at startup. */
	QMetaObject::invokeMethod(dock, "scheduleRefresh", Qt::QueuedConnection);
}
