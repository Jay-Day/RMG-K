/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "ScriptingConsoleDialog.hpp"

#include <RMG-Scripting/ScriptManager.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMetaObject>
#include <QPainter>
#include <QPixmap>
#include <QPolygon>
#include <QThread>
#include <QVBoxLayout>
#include <QWidget>

using namespace UserInterface::Dialog;

// Small green play-triangle icon for running scripts.
QIcon ScriptingConsoleDialog::makePlayIcon()
{
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor(0x43, 0xA0, 0x47));
    p.setPen(Qt::NoPen);
    QPolygon tri;
    tri << QPoint(3, 2) << QPoint(3, 12) << QPoint(12, 7);
    p.drawPolygon(tri);
    return QIcon(pm);
}

ScriptingConsoleDialog::ScriptingConsoleDialog(QWidget* parent) : QDialog(parent)
{
    this->setWindowTitle("Scripting Console");
    this->setWindowIcon(QIcon(":Resource/RMG.png"));
    this->setWindowFlags(this->windowFlags() | Qt::WindowMinimizeButtonHint);
    this->resize(960, 580);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // ── Splitter: script list | output ───────────────────────────────────────
    this->splitter = new QSplitter(Qt::Horizontal, this);

    this->scriptListWidget = new QListWidget(this->splitter);
    this->scriptListWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
    this->scriptListWidget->setIconSize(QSize(14, 14));
    this->scriptListWidget->setMinimumWidth(160);

    // Right panel: output + input row
    QWidget*     rightPanel  = new QWidget(this->splitter);
    QVBoxLayout* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(4);

    this->outputTextEdit = new QPlainTextEdit(rightPanel);
    this->outputTextEdit->setReadOnly(true);
    QFont mono("monospace");
#ifdef _WIN32
    mono.setStyleHint(QFont::TypeWriter);
#endif
    this->outputTextEdit->setFont(mono);
    rightLayout->addWidget(this->outputTextEdit, 1);

    QHBoxLayout* inputRow = new QHBoxLayout();
    inputRow->setSpacing(4);
    QLabel* promptLabel = new QLabel(">", rightPanel);
    promptLabel->setFont(mono);
    this->inputLineEdit = new QLineEdit(rightPanel);
    this->inputLineEdit->setFont(mono);
    this->inputLineEdit->setPlaceholderText("Input...");
    this->sendButton = new QPushButton("Send", rightPanel);
    inputRow->addWidget(promptLabel);
    inputRow->addWidget(this->inputLineEdit, 1);
    inputRow->addWidget(this->sendButton);
    rightLayout->addLayout(inputRow);

    this->splitter->addWidget(this->scriptListWidget);
    this->splitter->addWidget(rightPanel);
    this->splitter->setStretchFactor(0, 1);
    this->splitter->setStretchFactor(1, 3);
    root->addWidget(this->splitter, 1);

    // ── Bottom buttons ────────────────────────────────────────────────────────
    QHBoxLayout* btns = new QHBoxLayout();
    btns->setSpacing(6);

    this->runButton     = new QPushButton("▶  Run",     this);
    this->stopButton    = new QPushButton("■  Stop",    this);
    this->refreshButton = new QPushButton("⟳  Refresh", this);
    btns->addWidget(this->runButton);
    btns->addWidget(this->stopButton);
    btns->addWidget(this->refreshButton);
    btns->addStretch();
    this->stopAllButton = new QPushButton("Stop All",     this);
    this->clearButton   = new QPushButton("Clear Output", this);
    btns->addWidget(this->stopAllButton);
    btns->addWidget(this->clearButton);

    root->addLayout(btns);

    // ── Connections ───────────────────────────────────────────────────────────
    connect(this->runButton,     &QPushButton::clicked, this, &ScriptingConsoleDialog::onRunSelected);
    connect(this->stopButton,    &QPushButton::clicked, this, &ScriptingConsoleDialog::onStopSelected);
    connect(this->refreshButton, &QPushButton::clicked, this, &ScriptingConsoleDialog::onRefresh);
    connect(this->stopAllButton, &QPushButton::clicked, this, &ScriptingConsoleDialog::onStopAll);
    connect(this->clearButton,   &QPushButton::clicked, this, &ScriptingConsoleDialog::onClearOutput);

    connect(this->scriptListWidget, &QListWidget::itemDoubleClicked,
            this, &ScriptingConsoleDialog::onItemDoubleClicked);

    connect(this->sendButton,    &QPushButton::clicked, this, &ScriptingConsoleDialog::onSendInput);
    connect(this->inputLineEdit, &QLineEdit::returnPressed, this, &ScriptingConsoleDialog::onSendInput);

    ScriptManager::GetInstance().SetOutputCallback([this](const std::string& line) {
        const QString qline = QString::fromStdString(line);
        QMetaObject::invokeMethod(this, [this, qline]() {
            this->appendOutputLine(qline);
        }, Qt::QueuedConnection);
    });

    ScriptManager::GetInstance().SetInputPromptCallback([this](const std::string& message) -> std::string {
        const QString qmsg = QString::fromStdString(message);

        if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
            // UI thread: print the prompt, then spin a nested event loop so the
            // input bar stays responsive and we can capture the next submission.
            this->appendOutputLine(qmsg);
            QEventLoop loop;
            this->m_inputWaiting.store(true);
            this->m_inputWaitLoopPtr = &loop;
            this->m_inputWaitResult.clear();
            loop.exec();
            this->m_inputWaiting.store(false);
            this->m_inputWaitLoopPtr = nullptr;
            return this->m_inputWaitResult;
        } else {
            // Non-UI thread (e.g. emulation thread): print on UI thread async,
            // then block with a condition variable until onSendInput delivers.
            QMetaObject::invokeMethod(this, [this, qmsg]() {
                this->appendOutputLine(qmsg);
            }, Qt::QueuedConnection);
            std::unique_lock<std::mutex> lock(this->m_inputWaitMutex);
            this->m_inputWaiting.store(true);
            this->m_inputWaitLoopPtr = nullptr;
            this->m_inputWaitResult.clear();
            this->m_inputWaitCV.wait(lock, [this] { return !this->m_inputWaiting.load(); });
            return this->m_inputWaitResult;
        }
        return {};
    });

    this->refreshList();
}

ScriptingConsoleDialog::~ScriptingConsoleDialog(void)
{
    ScriptManager::GetInstance().SetOutputCallback(nullptr);
    ScriptManager::GetInstance().SetInputPromptCallback(nullptr);
}

void ScriptingConsoleDialog::RefreshRunningList()
{
    for (int i = 0; i < this->scriptListWidget->count(); ++i)
        this->updateItemState(this->scriptListWidget->item(i));
}

void ScriptingConsoleDialog::refreshList()
{
    // Preserve the set of running paths so we can restore their state after re-scanning.
    QDir scriptsDir(QDir::currentPath() + "/Data/Scripts");

    this->scriptListWidget->clear();

    if (!scriptsDir.exists())
    {
        this->appendOutputLine("Data/Scripts directory not found.");
        return;
    }

    const QFileInfoList files = scriptsDir.entryInfoList(
        QStringList() << "*.js", QDir::Files, QDir::Name);

    for (const QFileInfo& fi : files)
    {
        QListWidgetItem* item = new QListWidgetItem(fi.fileName(), this->scriptListWidget);
        item->setData(Qt::UserRole, fi.absoluteFilePath());
        this->updateItemState(item);
        this->scriptListWidget->addItem(item);
    }
}

void ScriptingConsoleDialog::updateItemState(QListWidgetItem* item)
{
    const QString path = item->data(Qt::UserRole).toString();
    const bool running = ScriptManager::GetInstance().IsScriptRunning(path.toStdString());

    if (running)
    {
        item->setIcon(makePlayIcon());
        item->setBackground(QColor(0x43, 0xA0, 0x47, 55));
    }
    else
    {
        item->setIcon(QIcon());
        item->setBackground(QBrush());
    }
}

void ScriptingConsoleDialog::appendOutputLine(const QString& line)
{
    this->outputTextEdit->appendPlainText(line);
}

void ScriptingConsoleDialog::onRunSelected()
{
    const QList<QListWidgetItem*> items = this->scriptListWidget->selectedItems();
    if (items.isEmpty())
    {
        this->appendOutputLine("No script selected.");
        return;
    }
    for (QListWidgetItem* item : items)
    {
        const QString path = item->data(Qt::UserRole).toString();
        if (ScriptManager::GetInstance().IsScriptRunning(path.toStdString()))
        {
            this->appendOutputLine(QString("[already running] %1").arg(item->text()));
            continue;
        }
        const bool ok = ScriptManager::GetInstance().LoadScript(path.toStdString());
        if (!ok)
            this->appendOutputLine(QString("[failed] %1").arg(item->text()));
        else if (ScriptManager::GetInstance().IsScriptRunning(path.toStdString()))
            this->appendOutputLine(QString("[started] %1").arg(item->text()));
        // else: script ran to completion with no callbacks — [done] already printed by ScriptManager
        this->updateItemState(item);
    }
}

void ScriptingConsoleDialog::onStopSelected()
{
    const QList<QListWidgetItem*> items = this->scriptListWidget->selectedItems();
    if (items.isEmpty())
    {
        this->appendOutputLine("No script selected.");
        return;
    }
    for (QListWidgetItem* item : items)
    {
        const QString path = item->data(Qt::UserRole).toString();
        if (!ScriptManager::GetInstance().IsScriptRunning(path.toStdString()))
            continue;
        ScriptManager::GetInstance().StopScript(path.toStdString());
        this->appendOutputLine(QString("[stopped] %1").arg(item->text()));
        this->updateItemState(item);
    }
}

void ScriptingConsoleDialog::onRefresh()
{
    this->refreshList();
}

void ScriptingConsoleDialog::onStopAll()
{
    ScriptManager::GetInstance().StopAll();
    this->appendOutputLine("[stopped all scripts]");
    this->RefreshRunningList();
}

void ScriptingConsoleDialog::onClearOutput()
{
    this->outputTextEdit->clear();
}

void ScriptingConsoleDialog::onSendInput()
{
    const QString text = this->inputLineEdit->text().trimmed();
    if (text.isEmpty())
        return;
    this->inputLineEdit->clear();
    this->appendOutputLine("> " + text);

    if (this->m_inputWaiting.load()) {
        // A script is blocking on emu.input() — deliver to it, don't broadcast.
        if (this->m_inputWaitLoopPtr) {
            // UI thread nested event loop path
            this->m_inputWaitResult = text.toStdString();
            this->m_inputWaitLoopPtr->quit();
        } else {
            // Non-UI thread condition variable path
            std::lock_guard<std::mutex> lock(this->m_inputWaitMutex);
            this->m_inputWaitResult = text.toStdString();
            this->m_inputWaiting.store(false);
            this->m_inputWaitCV.notify_one();
        }
        return;
    }

    ScriptManager::GetInstance().SubmitInput(text.toStdString());
}

void ScriptingConsoleDialog::onItemDoubleClicked(QListWidgetItem* item)
{
    const QString path = item->data(Qt::UserRole).toString();
    if (ScriptManager::GetInstance().IsScriptRunning(path.toStdString()))
    {
        ScriptManager::GetInstance().StopScript(path.toStdString());
        this->appendOutputLine(QString("[stopped] %1").arg(item->text()));
    }
    else
    {
        const bool ok = ScriptManager::GetInstance().LoadScript(path.toStdString());
        if (!ok)
            this->appendOutputLine(QString("[failed] %1").arg(item->text()));
        else if (ScriptManager::GetInstance().IsScriptRunning(path.toStdString()))
            this->appendOutputLine(QString("[started] %1").arg(item->text()));
    }
    this->updateItemState(item);
}
