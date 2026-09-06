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
class QTimer;

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

	/* Repopulates the list of window capture sources in the current scene. */
	void refreshCaptureSources();

private slots:
	/* Coalesces refresh requests. OBS raises several frontend events in
	 * quick succession at startup and on scene changes, and refreshing on
	 * each one rebuilds the whole window list needlessly. */
	void scheduleRefresh();

	void onPresetChanged(int index);
	void onSizeEdited();
	void onApply();

	/* Any user-driven change makes the dock worth persisting. Programmatic
	 * updates go through QSignalBlocker, so they do not set this. */
	void markDirty();

private:
	void buildUi();
	void setStatus(const QString &text, bool isError);

	/* The Apply pipeline, split so each step can report its own failure. */
	bool applyCanvasSize(int width, int height, QString &error);
	bool applyCaptureSource(const QString &windowValue, bool clientArea, int priority, const QString &sourceName,
				QString &configured, bool &created, QString &error);

	static void onFrontendEvent(enum obs_frontend_event event, void *data);

	/* Persisted with the scene collection, since the target window tends to
	 * belong with the scene rather than with OBS as a whole. */
	static void onFrontendSave(obs_data_t *save_data, bool saving, void *data);
	void saveState(obs_data_t *obj) const;
	void loadState(obs_data_t *obj);

	QComboBox *m_windowCombo = nullptr;
	QPushButton *m_refreshButton = nullptr;
	QComboBox *m_sourceCombo = nullptr;
	QComboBox *m_presetCombo = nullptr;
	QSpinBox *m_widthSpin = nullptr;
	QSpinBox *m_heightSpin = nullptr;
	QCheckBox *m_clientAreaCheck = nullptr;
	QCheckBox *m_configureRecordingCheck = nullptr;
	QSpinBox *m_cqSpin = nullptr;
	QPushButton *m_applyButton = nullptr;
	QLabel *m_statusLabel = nullptr;
	QTimer *m_refreshTimer = nullptr;

	/* Guards the preset combo from flipping to Custom while we are the ones
	 * writing the spin boxes. */
	bool m_updatingSize = false;

	/* Nothing is written for a dock the user has never touched. */
	bool m_dirty = false;
	bool m_hasSavedState = false;

	/* Restored selections, applied once their lists have been populated -
	 * the window list arrives asynchronously after the scene collection has
	 * already loaded. */
	QString m_pendingWindow;
	QString m_pendingSource;
};
