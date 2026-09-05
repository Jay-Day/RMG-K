/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef KAILLERAPLAYBACKDIALOG_HPP
#define KAILLERAPLAYBACKDIALOG_HPP

#ifdef NETPLAY

#include <QDialog>
#include <QTimer>
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>
#include <QProcess>
#include <QProgressDialog>
#include <QElapsedTimer>
#include <QCheckBox>

class KailleraPlaybackDialog : public QDialog
{
    Q_OBJECT

public:
    explicit KailleraPlaybackDialog(QWidget* parent = nullptr);
    ~KailleraPlaybackDialog() override;

private slots:
    void onPlaybackPlay();
    void onPlaybackStop();
    void onPlaybackPause();
    void onPlaybackStepForward();
    void onPlaybackDelete();
    void onPlaybackRefresh();
    void onPlaybackOpenFolder();
    void onPlaybackExport();
#ifdef RMGK_GAME_STATS
    void onPlaybackExportReplay();
#endif
    void onPlaybackDoubleClicked(int row, int column);
    void onPlaybackTimer();
    void onExportProcessOutput();
    void onExportProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    struct ExportSettings
    {
        QString outputPath;
        int renderWidth = 960;
        int renderHeight = 720;
        bool includeKailleraChat = true;
        bool labelPorts = false;
    };

    void setupUI();
    void updatePlaybackControls();
#ifdef RMGK_GAME_STATS
    void updateExportReplayVisibility();
#endif
    void populatePlaybackList();
    QString getSelectedRecordingPath() const;
    QString getSelectedRecordingGameName(QString* recordingPath = nullptr, int* totalFrames = nullptr) const;
    bool promptForExportSettings(const QString& defaultOutputPath, ExportSettings& settings);
    QString resolveExportFfmpegPath();
    QString promptForFfmpegPath();
    QString downloadManagedFfmpeg();
    void showExportFinishedDialog(const QString& outputPath, const QString& logText);
    void resetExportUi();
    void startExportProcess(const QString& recordingPath,
                            const QString& romPath,
                            const QString& outputPath,
                            const QString& ffmpegPath,
                            int renderWidth,
                            int renderHeight,
                            bool includeKailleraChat,
                            bool labelPorts,
                            int totalFrames);
#ifdef RMGK_GAME_STATS
    void startReplayFileExportProcess(const QString& recordingPath,
                                      const QString& romPath,
                                      const QString& outputPath,
                                      int totalFrames);
#endif
    QString exportDialogTitle() const;
    void processExportOutputText(const QString& text, bool finalizePartialLine = false);
    void processExportOutputLine(const QString& line);
    void updateExportProgressDialog();
    QString buildExportProgressSummary() const;
    double estimateExportFinalizeSeconds() const;
    bool isExportCaptureComplete() const;
    bool isExportFinalizing() const;
    double exportProgressFraction() const;

    QTableWidget* m_playbackTable = nullptr;
    QPushButton* m_btnPlay = nullptr;
    QPushButton* m_btnPause = nullptr;
    QPushButton* m_btnStepForward = nullptr;
    QPushButton* m_btnStop = nullptr;
    QPushButton* m_btnPBDelete = nullptr;
    QPushButton* m_btnPBRefresh = nullptr;
    QPushButton* m_btnExport = nullptr;
#ifdef RMGK_GAME_STATS
    // Headless/fast .rmgr-only export, sharing m_exportProcess/
    // m_exportProgressDialog with Export MP4 below (mutually exclusive,
    // never both running at once) but its own button and CLI dispatch -
    // only shown when the selected recording's stored game name is Smash
    // Remix 2.0.1, and Windows-only like Export MP4 (see
    // onPlaybackExportReplay()).
    QPushButton* m_btnExportReplay = nullptr;
#endif
    QPushButton* m_btnOpenFolder = nullptr;
    QLabel* m_frameLabel = nullptr;
    bool m_playbackWasActive = false;
    bool m_isPaused = false;
    bool m_exportCanceled = false;
    // Which of Export MP4 / Export Replays is in flight - only meaningful
    // while m_exportProcess is non-null. Used purely for dialog/message
    // wording (see exportDialogTitle()); the two share every other bit of
    // export-process state below since they're mutually exclusive.
    bool m_exportIsReplayFile = false;
    QString m_exportOutputPath;
    QString m_exportLog;
    QString m_exportPendingOutput;
    QString m_exportStatusLine;
    QString m_exportVideoEncoder;
    QString m_exportTargetSpeed;
    int m_exportCapturedFrames = 0;
    int m_exportTotalFrames = 0;
    qint64 m_exportCaptureCompleteElapsedMs = -1;
    QElapsedTimer m_exportElapsedTimer;

    QTimer* m_playbackTimer = nullptr;
    QProcess* m_exportProcess = nullptr;
    QProgressDialog* m_exportProgressDialog = nullptr;
};

#endif // NETPLAY
#endif // KAILLERAPLAYBACKDIALOG_HPP
