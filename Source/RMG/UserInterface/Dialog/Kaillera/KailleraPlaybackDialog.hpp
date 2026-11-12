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
#include <QJsonArray>
#include <QJsonObject>
#include <QScrollArea>
#include <QVector>

#include <functional>

class PlaybackTimeline;

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

    // jumps the playback to a .krec record (starts the selected recording
    // when stopped)
    void seekTo(int record);

private slots:
    void onPlaybackPlay();
    void onPlaybackStop();
    void onPlaybackPause();
    void onPlaybackStepForward();
    void onPlaybackBack();
    void onPlaybackForward();
    void onPlaybackAddBookmark();
    void onPlaybackSpeedChanged(int index);
    void onPlaybackSelectionChanged();
    void onPlaybackDelete();
    void onPlaybackRefresh();
    void onPlaybackOpenFolder();
    void onPlaybackExport();
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
        bool includeChat = false;
        bool labelPorts = false;
    };

    // a bookmark in a recording's metadata
    struct Bookmark
    {
        int record = 0;
        QString label;    // as shown
        QString kind;     // "user" for the ones added here
        int index = -1;   // in the metadata's bookmarks array
    };

    void setupUI();
    // replay metadata appended to the .krec (bookmarks; ranked replays add
    // their set and games). False when the recording has none.
    static bool loadReplayMetadata(const QString& krecPath, QJsonObject& metadata);
    // a ranked set's recording: stages its module read-only in replay mode,
    // to show the set's progress, when that version is installed
    void stageRankedReplay(const QString& krecPath);
    // changes a recording's bookmarks; false (after telling the user) when
    // they can't be saved
    bool editBookmarks(const QString& krecPath, const std::function<void(QJsonArray&)>& edit);
    void renameBookmark(const Bookmark& bookmark);
    void deleteBookmark(const Bookmark& bookmark);
    // the recording the bookmarks and timeline belong to: the one playing,
    // else the selected one
    QString timelineRecordingPath() const;
    int selectedRecordingFrames() const;
    void refreshBookmarks();
    void startPlayback(const QString& krecPath);
    // restarting = stopping to replay from the start (a jump back)
    void stopPlayback(bool restarting);
    void finishSeek();
    void updateTimeline();
    void syncSpeedCombo();
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
                            bool includeChat,
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
    QPushButton* m_btnBack = nullptr;
    QPushButton* m_btnForward = nullptr;
    QPushButton* m_btnAddBookmark = nullptr;
    QPushButton* m_btnStop = nullptr;
    QLabel* m_speedLabel = nullptr;
    QComboBox* m_speedCombo = nullptr;
    PlaybackTimeline* m_timeline = nullptr;
    QScrollArea* m_bookmarkScroll = nullptr;
    QWidget* m_bookmarkRow = nullptr;      // one button per bookmark
    QHBoxLayout* m_bookmarkLayout = nullptr;
    QVector<Bookmark> m_bookmarks;         // of timelineRecordingPath()
    QString m_bookmarksPath;
    QString m_activeRecordingPath;   // recording being played
    int m_seekTarget = -1;           // fast-forwarding to this record
    int m_seekFrom = 0;              // the record that jump started from
    int m_restartSeekTarget = -1;    // seek after restarting the playback
    bool m_seekPause = false;        // pause when the jump lands
    int m_speed = 100;               // playback speed picked here, in percent
    bool m_speedApplied = false;     // m_speed set on the running emulation
    int m_playbackTotal = 0;         // records in the recording being played
    bool m_stepping = false;         // a frame step's resume/pause is running
    QPushButton* m_btnPBDelete = nullptr;
    QPushButton* m_btnPBRefresh = nullptr;
    QPushButton* m_btnExport = nullptr;
    QPushButton* m_btnOpenFolder = nullptr;
    QLabel* m_frameLabel = nullptr;
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
