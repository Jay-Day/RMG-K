/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef SCRIPTINGCONSOLEDIALOG_HPP
#define SCRIPTINGCONSOLEDIALOG_HPP

#include <QDialog>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>

namespace UserInterface
{
namespace Dialog
{
class ScriptingConsoleDialog : public QDialog
{
    Q_OBJECT

  public:
    explicit ScriptingConsoleDialog(QWidget* parent = nullptr);
    ~ScriptingConsoleDialog(void);

    // Call after a script is loaded or stopped externally to refresh the running list.
    void RefreshRunningList();

  private:
    // Available scripts panel
    QListWidget* availableListWidget = nullptr;
    QPushButton* refreshButton = nullptr;
    QPushButton* runSelectedButton = nullptr;

    // Running scripts panel
    QListWidget* runningListWidget = nullptr;
    QPushButton* stopSelectedButton = nullptr;
    QPushButton* stopAllButton = nullptr;

    QPushButton* clearOutputButton = nullptr;
    QPlainTextEdit* outputTextEdit = nullptr;

    void refreshAvailableList();
    void appendOutputLine(const QString& line);

  private slots:
    void onRefreshScripts();
    void onRunSelectedScript();
    void onStopSelectedScript();
    void onStopAllScripts();
    void onClearOutput();
};
} // namespace Dialog
} // namespace UserInterface

#endif // SCRIPTINGCONSOLEDIALOG_HPP
