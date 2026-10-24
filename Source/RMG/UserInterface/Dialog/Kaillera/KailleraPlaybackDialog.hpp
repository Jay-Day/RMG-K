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
#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonObject>

class KailleraPlaybackDialog : public QDialog
{
    Q_OBJECT

public:
    explicit KailleraPlaybackDialog(QWidget* parent = nullptr);
    ~KailleraPlaybackDialog() override;

    // selects a recording in the records directory and plays it, from
    // `record` (a .krec input record, e.g. a game's bookmark) when > 0
    void playRecording(const QString& path, int record = 0);

    // whether `path` is the recording being played
    bool isPlaying(const QString& path) const;

    // jumps the playback to a .krec record (starts it when stopped)
    void seekTo(int record);

private slots:
    void onPlaybackPlay();
    void onPlaybackStop();
    void onPlaybackPause();
    void onPlaybackStepForward();
    void onPlaybackDelete();
    void onPlaybackRefresh();
    void onPlaybackOpenFolder();
    void onPlaybackExport();
    void onPlaybackDoubleClicked(int row, int column);
    void onPlaybackTimer();
    void onExportProcessOutput();
    void onExportProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onPlaybackSelectionChanged();

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
    // Ranked replays: metadata appended to the .krec (module, ruleset,
    // bookmarks, write journal). Returns false when the recording has none.
    static bool loadReplayMetadata(const QString& krecPath, QJsonObject& metadata);
    // stages the recording's ranked module in replay mode; false = cancelled
    bool stageRankedReplay(const QString& krecPath);
    void startPlayback(const QString& krecPath);
    void finishSeek();
    void updatePlaybackControls();
    void populatePlaybackList();
    QString getSelectedRecordingPath() const;
    QString getSelectedRecordingGameName(QString* recordingPath = nullptr, int* totalFrames = nullptr) const;
    bool promptForExportSettings(const QString& defaultOutputPath, ExportSettings& settings);
    QString resolveExportFfmpegPath();
    QString promptForFfmpegPath();
    QString downloadManagedFfmpeg();
    void showExportFinishedDialog(const QString& outputPath);
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
    QPushButton* m_btnOpenFolder = nullptr;
    QLabel* m_frameLabel = nullptr;
    QWidget* m_bookmarkRow = nullptr;      // one button per bookmark
    QHBoxLayout* m_bookmarkLayout = nullptr;
    bool m_modernButtons = false;
    QString m_activeRecordingPath;   // recording being played
    int m_seekTarget = -1;           // fast-forwarding to this record
    int m_restartSeekTarget = -1;    // seek after restarting the playback
    bool m_playbackWasActive = false;
    bool m_isPaused = false;
    bool m_exportCanceled = false;
    QString m_exportOutputPath;
    QString m_exportLog;
    QString m_exportPendingOutput;
    QString m_exportStatusLine;
    QString m_exportVideoEncoder;
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
