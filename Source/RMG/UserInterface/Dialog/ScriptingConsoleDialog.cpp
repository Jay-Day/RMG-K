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

#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMetaObject>
#include <QVBoxLayout>
#include <QDir>
#include <QFileInfo>

using namespace UserInterface::Dialog;

ScriptingConsoleDialog::ScriptingConsoleDialog(QWidget* parent) : QDialog(parent)
{
    this->setWindowTitle("Scripting Console");
    this->setWindowIcon(QIcon(":Resource/RMG.png"));
    this->setWindowFlags(this->windowFlags() | Qt::WindowMinimizeButtonHint);
    this->resize(960, 620);

    QVBoxLayout* rootLayout = new QVBoxLayout(this);

    // ── Available scripts ────────────────────────────────────────────────────
    rootLayout->addWidget(new QLabel("Available scripts (Data/Scripts/*.lua)", this));

    this->availableListWidget = new QListWidget(this);
    this->availableListWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
    rootLayout->addWidget(this->availableListWidget, 2);

    QHBoxLayout* availBtns = new QHBoxLayout();
    this->refreshButton     = new QPushButton("Refresh", this);
    this->runSelectedButton = new QPushButton("Run Selected", this);
    availBtns->addWidget(this->refreshButton);
    availBtns->addWidget(this->runSelectedButton);
    availBtns->addStretch();
    rootLayout->addLayout(availBtns);

    // ── Running scripts ──────────────────────────────────────────────────────
    rootLayout->addWidget(new QLabel("Running scripts", this));

    this->runningListWidget = new QListWidget(this);
    this->runningListWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
    rootLayout->addWidget(this->runningListWidget, 1);

    QHBoxLayout* runBtns = new QHBoxLayout();
    this->stopSelectedButton = new QPushButton("Stop Selected", this);
    this->stopAllButton      = new QPushButton("Stop All", this);
    runBtns->addWidget(this->stopSelectedButton);
    runBtns->addWidget(this->stopAllButton);
    runBtns->addStretch();
    rootLayout->addLayout(runBtns);

    // ── Output ───────────────────────────────────────────────────────────────
    rootLayout->addWidget(new QLabel("Script output", this));

    this->outputTextEdit = new QPlainTextEdit(this);
    this->outputTextEdit->setReadOnly(true);
    QFont mono("monospace");
#ifdef _WIN32
    mono.setStyleHint(QFont::TypeWriter);
#endif
    this->outputTextEdit->setFont(mono);
    rootLayout->addWidget(this->outputTextEdit, 3);

    QHBoxLayout* outBtns = new QHBoxLayout();
    this->clearOutputButton = new QPushButton("Clear Output", this);
    outBtns->addStretch();
    outBtns->addWidget(this->clearOutputButton);
    rootLayout->addLayout(outBtns);

    // ── Connections ──────────────────────────────────────────────────────────
    connect(this->refreshButton,      &QPushButton::clicked, this, &ScriptingConsoleDialog::onRefreshScripts);
    connect(this->runSelectedButton,  &QPushButton::clicked, this, &ScriptingConsoleDialog::onRunSelectedScript);
    connect(this->stopSelectedButton, &QPushButton::clicked, this, &ScriptingConsoleDialog::onStopSelectedScript);
    connect(this->stopAllButton,      &QPushButton::clicked, this, &ScriptingConsoleDialog::onStopAllScripts);
    connect(this->clearOutputButton,  &QPushButton::clicked, this, &ScriptingConsoleDialog::onClearOutput);

    connect(this->availableListWidget, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) { this->onRunSelectedScript(); });

    ScriptManager::GetInstance().SetOutputCallback([this](const std::string& line) {
        const QString qline = QString::fromStdString(line);
        QMetaObject::invokeMethod(this, [this, qline]() {
            this->appendOutputLine(qline);
        }, Qt::QueuedConnection);
    });

    this->refreshAvailableList();
}

ScriptingConsoleDialog::~ScriptingConsoleDialog(void)
{
    ScriptManager::GetInstance().SetOutputCallback(nullptr);
}

void ScriptingConsoleDialog::RefreshRunningList()
{
    this->runningListWidget->clear();
    for (const std::string& fp : ScriptManager::GetInstance().GetRunningScripts())
    {
        QFileInfo fi(QString::fromStdString(fp));
        QListWidgetItem* item = new QListWidgetItem(fi.fileName(), this->runningListWidget);
        item->setData(Qt::UserRole, QString::fromStdString(fp));
        this->runningListWidget->addItem(item);
    }
}

void ScriptingConsoleDialog::refreshAvailableList()
{
    this->availableListWidget->clear();

    QDir scriptsDir(QDir::currentPath() + "/Data/Scripts");
    if (!scriptsDir.exists())
    {
        this->appendOutputLine("Data/Scripts directory not found.");
        return;
    }

    QFileInfoList files = scriptsDir.entryInfoList(QStringList() << "*.lua", QDir::Files, QDir::Name);
    for (const QFileInfo& fileInfo : files)
    {
        QListWidgetItem* item = new QListWidgetItem(fileInfo.fileName(), this->availableListWidget);
        item->setData(Qt::UserRole, fileInfo.absoluteFilePath());
        this->availableListWidget->addItem(item);
    }

    this->appendOutputLine(QString("Discovered %1 script(s).").arg(files.size()));
}

void ScriptingConsoleDialog::appendOutputLine(const QString& line)
{
    this->outputTextEdit->appendPlainText(line);
}

void ScriptingConsoleDialog::onRefreshScripts()
{
    this->refreshAvailableList();
    this->RefreshRunningList();
}

void ScriptingConsoleDialog::onRunSelectedScript()
{
    QList<QListWidgetItem*> items = this->availableListWidget->selectedItems();
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
            this->appendOutputLine(QString("[already running] %1").arg(path));
            continue;
        }
        const bool ok = ScriptManager::GetInstance().LoadScript(path.toStdString());
        this->appendOutputLine(QString("[%1] %2").arg(ok ? "OK" : "FAIL", path));
    }

    this->RefreshRunningList();
}

void ScriptingConsoleDialog::onStopSelectedScript()
{
    QList<QListWidgetItem*> items = this->runningListWidget->selectedItems();
    if (items.isEmpty())
    {
        this->appendOutputLine("No running script selected.");
        return;
    }

    for (QListWidgetItem* item : items)
    {
        const QString path = item->data(Qt::UserRole).toString();
        ScriptManager::GetInstance().StopScript(path.toStdString());
        this->appendOutputLine(QString("[stopped] %1").arg(path));
    }

    this->RefreshRunningList();
}

void ScriptingConsoleDialog::onStopAllScripts()
{
    ScriptManager::GetInstance().StopAll();
    this->appendOutputLine("[stopped all scripts]");
    this->RefreshRunningList();
}

void ScriptingConsoleDialog::onClearOutput()
{
    this->outputTextEdit->clear();
}
