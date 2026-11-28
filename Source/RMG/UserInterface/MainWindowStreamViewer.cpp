/*
 * MainWindow as a live replay viewer window (--stream-viewer): one stream,
 * fed by the main window's lobby connection over StreamViewerClient, played
 * by the same spectate code as the main window's watch. The window is
 * stripped to watching, and writes none of the user's files.
 */
#ifdef NETPLAY

#include "MainWindow.hpp"

#include "Dialog/Lobby/LobbyClient.hpp"
#include "StreamViewer/StreamViewerClient.hpp"
#include "OnScreenDisplay.hpp"

#include <RMG-Core/Callback.hpp>
#include <RMG-Core/Emulation.hpp>
#include <RMG-Core/Volume.hpp>

#include <QAction>
#include <QCoreApplication>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace UserInterface;

namespace
{
    // How long a finished stream's window says so before it closes.
    constexpr int kEndedMessageMs = 3000;
} // namespace

void MainWindow::SetStreamViewer(const QString& serverName, const QByteArray& token)
{
    this->ui_StreamViewerServer = serverName;
    this->ui_StreamViewerToken = token;
}

void MainWindow::configureStreamViewerUI(void)
{
    // Watching needs a few things, from a menu of its own: none of the main
    // window's menus (and so none of their shortcuts) apply here.
    for (QAction* action : this->menuBar()->actions())
        action->setVisible(false);

    // Not checkable: a checked tool button reads as switched off. Its text
    // and icon say what it does instead.
    this->action_StreamViewer_Mute = new QAction(QIcon::fromTheme("volume-up-line"), tr("&Mute"), this);
    this->action_StreamViewer_Close = new QAction(tr("&Close"), this);
    this->action_StreamViewer_Close->setShortcut(QKeySequence::Close);

    QMenu* menu = this->menuBar()->addMenu(tr("&Watch"));
    menu->addAction(this->action_View_Fullscreen);
    menu->addAction(this->action_StreamViewer_Mute);
    this->action_Audio_IncreaseVolume->setText(tr("Volume &Up"));
    this->action_Audio_DecreaseVolume->setText(tr("Volume &Down"));
    menu->addAction(this->action_Audio_IncreaseVolume);
    menu->addAction(this->action_Audio_DecreaseVolume);
    menu->addAction(this->action_System_Screenshot);
    menu->addSeparator();
    menu->addAction(this->action_StreamViewer_Close);

    // The toolbar has the same, and the catch-up progress the main window
    // shows in its title: a viewer's title stays put, since that's how the
    // taskbar and window capture tell the windows apart.
    this->toolBar->clear();
    this->toolBar->addAction(this->action_View_Fullscreen);
    this->toolBar->addAction(this->action_StreamViewer_Mute);
    this->toolBar->addAction(this->action_System_Screenshot);
    this->toolBar->setMovable(false);
    auto* spacer = new QWidget(this->toolBar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    this->toolBar->addWidget(spacer);
    this->ui_StreamViewerStatus = new QLabel(this->toolBar);
    this->ui_StreamViewerStatus->setContentsMargins(0, 0, 8, 0);
    this->toolBar->addWidget(this->ui_StreamViewerStatus);
    this->ui_ShowToolbar = true;
    this->ui_ShowStatusbar = false;
    this->toolBar->setVisible(true);
    this->statusBar()->setVisible(false);

    // Only these work from the keyboard, in fullscreen too (addActions).
    this->ui_Actions = {
        this->action_View_Fullscreen, this->action_System_Screenshot,
        this->action_StreamViewer_Mute, this->action_StreamViewer_Close,
    };

    connect(this->action_StreamViewer_Mute, &QAction::triggered, this, [this]() {
        this->setStreamViewerMuted(!this->ui_StreamViewerMuted);
    });
    connect(this->action_StreamViewer_Close, &QAction::triggered, this, [this]() { this->closeStreamViewer(); });
    // The mute hotkey works the same switch, so the lobby hears about it.
    disconnect(this->action_Audio_ToggleVolumeMute, &QAction::triggered, this, &MainWindow::on_Action_Audio_ToggleVolumeMute);
    connect(this->action_Audio_ToggleVolumeMute, &QAction::triggered, this->action_StreamViewer_Mute, &QAction::trigger);

    // What shows before the game starts and after it ends, instead of the
    // ROM browser.
    this->ui_StreamViewerPage = new QLabel(this);
    this->ui_StreamViewerPage->setAlignment(Qt::AlignCenter);
    this->ui_StreamViewerPage->setWordWrap(true);
    this->ui_StreamViewerPage->setMargin(24);
    this->ui_StreamViewerPage->setAutoFillBackground(true);
    QPalette palette = this->ui_StreamViewerPage->palette();
    palette.setColor(QPalette::Window, Qt::black);
    palette.setColor(QPalette::WindowText, QColor(220, 220, 220));
    this->ui_StreamViewerPage->setPalette(palette);
    QFont font = this->ui_StreamViewerPage->font();
    if (font.pointSizeF() > 0)
        font.setPointSizeF(font.pointSizeF() * 1.3);
    else if (font.pixelSize() > 0)
        font.setPixelSize(font.pixelSize() * 13 / 10);
    this->ui_StreamViewerPage->setFont(font);
    this->ui_Widgets->addWidget(this->ui_StreamViewerPage);
    this->showStreamViewerPage(tr("Connecting to the live replay…"));

    this->resize(800, 600);
    this->updateStreamViewerTitle();
}

void MainWindow::connectStreamViewer(void)
{
    this->ui_StreamViewer = new StreamViewerClient(this->ui_StreamViewerServer, this->ui_StreamViewerToken, this);
    StreamViewerClient* client = this->ui_StreamViewer;

    connect(client, &StreamViewerClient::init, this,
            [this](quint64 matchId, const QString& title, const QString& game, const QString& romPath, bool startMuted) {
        this->ui_StreamViewerTitle = title;
        if (startMuted)
        {
            this->ui_StreamViewerMuted = true;
            this->ui_StreamViewerMutePending = true;
        }
        this->updateStreamViewerTitle();
        this->showStreamViewerPage(tr("Connecting to %1…").arg(title));
        // The main window's own watch path, fed from the pipe.
        this->on_Lobby_SpectateLaunch(matchId, game, romPath);
        if (!this->ui_SpectateActive)
        {
            this->closeStreamViewer();
            return;
        }
        this->ui_StreamViewer->sendReady();
    });
    connect(client, &StreamViewerClient::manifest, this, [this](const QByteArray& manifest) {
        if (!this->ui_SpectateActive)
            return;
        // The same parser players and the main window's watch use, before any
        // replay input or keyframe can start the game.
        if (!Dialog::LobbyClient::applySpectateManifest(manifest))
        {
            this->stopLobbySpectate();
            this->reportSpectateError(tr("Live Replay Error"),
                tr("The live replay's cheat settings are missing or invalid."));
        }
    });
    connect(client, &StreamViewerClient::keyframe, this, &MainWindow::on_Lobby_SpectateKeyframe);
    connect(client, &StreamViewerClient::data, this, &MainWindow::on_Lobby_SpectateData);
    connect(client, &StreamViewerClient::viewers, this, [this](int viewerCount) {
        if (this->ui_SpectateActive)
            this->on_Lobby_LiveReplayViewerCountChanged(viewerCount, true);
    });
    connect(client, &StreamViewerClient::chat, this, &MainWindow::on_Lobby_SpectatorChatReceived);
    connect(client, &StreamViewerClient::ended, this, [this](const QString&) {
        this->closeStreamViewer(tr("The live replay ended."));
    });
    connect(client, &StreamViewerClient::raiseRequested, this, &MainWindow::raiseStreamViewer);
    connect(client, &StreamViewerClient::muteRequested, this, [this](bool muted) {
        if (muted != this->ui_StreamViewerMuted)
            this->setStreamViewerMuted(muted);
    });
    connect(client, &StreamViewerClient::closeRequested, this, [this]() { this->closeStreamViewer(); });
    // The main window quit or crashed: nothing feeds this window any more.
    connect(client, &StreamViewerClient::lost, this, [this]() { this->closeStreamViewer(); });

    client->start();
}

void MainWindow::showStreamViewerPage(const QString& text)
{
    if (this->ui_StreamViewerPage == nullptr)
        return;
    this->ui_StreamViewerPage->setText(text);
    // Called once before the emulation thread exists.
    if (this->emulationThread == nullptr || !this->emulationThread->isRunning())
        this->ui_Widgets->setCurrentWidget(this->ui_StreamViewerPage);
}

void MainWindow::updateStreamViewerTitle(void)
{
    // "Alice vs Bob — Live — RMG-K"
    const QString who = this->ui_StreamViewerTitle.isEmpty() ? tr("Live replay") : this->ui_StreamViewerTitle;
    this->setWindowTitle(tr("%1 — Live — %2").arg(who, QCoreApplication::applicationName()));
    if (this->action_StreamViewer_Mute != nullptr)
    {
        const bool muted = this->ui_StreamViewerMuted;
        this->action_StreamViewer_Mute->setIcon(QIcon::fromTheme(muted ? "volume-mute-line" : "volume-up-line"));
        this->action_StreamViewer_Mute->setText(muted ? tr("Un&mute") : tr("&Mute"));
    }
}

void MainWindow::applyStreamViewerMute(void)
{
    // A state asked for before the game ran. Without one the game starts the
    // way the user's sound settings say, and the switch follows it.
    if (this->ui_StreamViewer == nullptr || !this->ui_StreamViewerMutePending)
        return;
    this->ui_StreamViewerMutePending = false;
    if (CoreIsVolumeMuted() != this->ui_StreamViewerMuted)
        CoreToggleMuteVolume();
}

void MainWindow::setStreamViewerMuted(bool muted)
{
    this->ui_StreamViewerMuted = muted;
    this->updateStreamViewerTitle();
    if (CoreIsEmulationRunning())
    {
        if (CoreIsVolumeMuted() != muted)
            CoreToggleMuteVolume(); // its state callback tells the main window
        return;
    }
    // Before the game runs, the switch is what it starts with.
    this->ui_StreamViewerMutePending = true;
    if (this->ui_StreamViewer != nullptr)
        this->ui_StreamViewer->sendMuted(muted);
}

void MainWindow::raiseStreamViewer(void)
{
    CoreAddCallbackMessage(CoreDebugMessageType::Info, "Live replay viewer: brought to the front");
    if (this->isMinimized())
        this->setWindowState(this->windowState() & ~Qt::WindowMinimized);
    this->show();
    this->raise();
    this->activateWindow();
#ifdef _WIN32
    // The main window let this process take the foreground (it was the
    // foreground one when its lobby was clicked); Qt alone only flashes the
    // taskbar button.
    SetForegroundWindow(reinterpret_cast<HWND>(this->winId()));
#endif
}

void MainWindow::closeStreamViewer(const QString& message)
{
    if (this->ui_StreamViewerClosing)
    {
        // Already on its way out after a message; something that can't wait
        // (the main window going away) closes it now.
        if (message.isEmpty() && !this->ui_ForceClose)
        {
            this->ui_ForceClose = true;
            QTimer::singleShot(0, this, [this]() { this->close(); });
        }
        return;
    }
    this->ui_StreamViewerClosing = true;
    if (this->ui_SpectateActive)
        this->stopLobbySpectate();
    if (this->ui_StreamViewer != nullptr)
        this->ui_StreamViewer->sendState(StreamViewer::State::Ended);
    if (!message.isEmpty())
        this->showStreamViewerPage(message);
    // Not from inside whatever called this (it can be deep in the playback
    // code): the close waits for the game to stop.
    QTimer::singleShot(message.isEmpty() ? 0 : kEndedMessageMs, this, [this]() {
        this->ui_ForceClose = true;
        this->close();
    });
}

#endif // NETPLAY
