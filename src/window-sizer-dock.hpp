/*
obs-window-sizer
Copyright (C) 2026 Tony Goodhew <tony@schnauzergroup.com>

SPDX-License-Identifier: MIT
See LICENSE in the project root for the full licence text.
*/

#pragma once

#include <QWidget>

#include <obs-frontend-api.h>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

/*
 * The dock. OBS takes ownership of this widget when it is handed to
 * obs_frontend_add_dock_by_id(), so it must be heap allocated and must not be
 * deleted by us - obs_frontend_remove_dock() disposes of it.
 */
class WindowSizerDock : public QWidget {
	Q_OBJECT

public:
	explicit WindowSizerDock(QWidget *parent = nullptr);
	~WindowSizerDock() override;

public slots:
	/* Repopulates the target-window combo from the capture source's own
	 * property list, preserving the current selection where possible. */
	void refreshWindows();

private slots:
	void onPresetChanged(int index);
	void onSizeEdited();
	void onApply();

private:
	void buildUi();
	void setStatus(const QString &text, bool isError);

	/* The Apply pipeline, split so each step can report its own failure. */
	bool applyCanvasSize(int width, int height, QString &error);
	bool applyCaptureSource(const QString &windowValue, bool clientArea, int priority, QString &error);

	static void onFrontendEvent(enum obs_frontend_event event, void *data);

	QComboBox *m_windowCombo = nullptr;
	QPushButton *m_refreshButton = nullptr;
	QComboBox *m_presetCombo = nullptr;
	QSpinBox *m_widthSpin = nullptr;
	QSpinBox *m_heightSpin = nullptr;
	QCheckBox *m_clientAreaCheck = nullptr;
	QCheckBox *m_configureRecordingCheck = nullptr;
	QSpinBox *m_cqSpin = nullptr;
	QPushButton *m_applyButton = nullptr;
	QLabel *m_statusLabel = nullptr;

	/* Guards the preset combo from flipping to Custom while we are the ones
	 * writing the spin boxes. */
	bool m_updatingSize = false;
};
