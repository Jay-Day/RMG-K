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
#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QScrollArea>
#include <QVector>

#include <functional>

class PlaybackTimeline;
class QProgressBar;
class QListWidget;

class KailleraPlaybackDialog : public QDialog
{
    Q_OBJECT

public:
    explicit KailleraPlaybackDialog(QWidget* parent = nullptr);
    ~KailleraPlaybackDialog() override;

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

#ifdef RMGK_GAME_STATS
    // One queued-but-not-yet-started .rmgr export - see onPlaybackExportReplay()/
    // enqueueReplayExport(). displayName is purely for the queue/history panel
    // (the source .krec's file name), not used to derive any path.
    struct ReplayExportQueueItem
    {
        QString displayName;
        QString recordingPath;
        QString romPath;
        QString outputPath;
        int     totalFrames = 0;
    };
#endif

    void setupUI();
    // replay metadata appended to the .krec (bookmarks; ranked replays add
    // their module and write journal). False when the recording has none.
    static bool loadReplayMetadata(const QString& krecPath, QJsonObject& metadata);
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
                            bool includeChat,
                            bool labelPorts,
                            int totalFrames);
#ifdef RMGK_GAME_STATS
    void startReplayFileExportProcess(const QString& recordingPath,
                                      const QString& romPath,
                                      const QString& outputPath,
                                      int totalFrames,
                                      const QString& displayName);
    // Validates nothing (onPlaybackExportReplay already did) - starts the
    // export immediately if idle, otherwise appends to m_replayExportQueue.
    // De-dupes against the in-flight export and the queue by recordingPath.
    void enqueueReplayExport(const ReplayExportQueueItem& item);
    // Pops the front of m_replayExportQueue (if any) and starts it. Called
    // once a replay export finishes (or fails to even start), so the queue
    // keeps draining without further user action.
    void startNextQueuedReplayExportIfAny();
    void ensureReplayExportPanelVisible();
    void updateReplayExportQueueLabel();
    void appendReplayExportLogLine(const QString& text);
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
#ifdef RMGK_GAME_STATS
    // Export Replays only: a FIFO of exports waiting for m_exportProcess to
    // free up, plus the in-flight export's display name/source path (for
    // the history line once it finishes, and for de-dup checks against new
    // Export Replays clicks) - see enqueueReplayExport().
    QList<ReplayExportQueueItem> m_replayExportQueue;
    QString m_exportDisplayName;
    QString m_replayExportInFlightPath;
    // Non-modal panel shown in place of Export MP4's modal QProgressDialog
    // for this path, so queuing more exports doesn't require closing
    // anything - see setupUI() and updateExportProgressDialog().
    QWidget* m_replayExportPanel = nullptr;
    QLabel* m_replayExportStatusLabel = nullptr;
    QProgressBar* m_replayExportProgressBar = nullptr;
    QLabel* m_replayExportQueueLabel = nullptr;
    QListWidget* m_replayExportHistoryList = nullptr;
#endif
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
