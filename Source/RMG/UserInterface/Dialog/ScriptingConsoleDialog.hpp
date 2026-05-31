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
#include <QSplitter>

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

    // Refresh the visual state of all script items (called when a script stops externally).
    void RefreshRunningList();

  private:
    QSplitter*      splitter         = nullptr;
    QListWidget*    scriptListWidget = nullptr;
    QPlainTextEdit* outputTextEdit   = nullptr;

    QPushButton* runButton      = nullptr;
    QPushButton* stopButton     = nullptr;
    QPushButton* refreshButton  = nullptr;
    QPushButton* stopAllButton  = nullptr;
    QPushButton* clearButton    = nullptr;

    static QIcon makePlayIcon();

    void refreshList();
    void updateItemState(QListWidgetItem* item);
    void appendOutputLine(const QString& line);

  private slots:
    void onRunSelected();
    void onStopSelected();
    void onRefresh();
    void onStopAll();
    void onClearOutput();
    void onItemDoubleClicked(QListWidgetItem* item);
};
} // namespace Dialog
} // namespace UserInterface

#endif // SCRIPTINGCONSOLEDIALOG_HPP
