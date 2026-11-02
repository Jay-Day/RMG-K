#ifndef LAGSTATSDIALOG_HPP
#define LAGSTATSDIALOG_HPP

#ifdef NETPLAY

#include "LagStats.hpp"

#include <QDialog>

#include <functional>

class QComboBox;
class QLabel;
class QPushButton;
class QTimer;
class QTreeWidget;

namespace UserInterface
{
namespace Dialog
{

// The Lag Stats window: who is holding the rollback match up, and every
// player's numbers. It refreshes once a second while open.
class LagStatsDialog : public QDialog
{
    Q_OBJECT

  public:
    using ReportSource = std::function<LagReport(bool wholeMatch)>;
    LagStatsDialog(ReportSource source, QWidget* parent = nullptr);

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    void refresh();

    ReportSource m_source;
    LagReport m_report;
    QLabel* m_dot = nullptr;
    QLabel* m_headline = nullptr;
    QLabel* m_details = nullptr;
    QLabel* m_notes = nullptr;
    QComboBox* m_windowCombo = nullptr;
    QTreeWidget* m_players = nullptr;
    QTreeWidget* m_links = nullptr;
    QPushButton* m_copyButton = nullptr;
    QTimer* m_timer = nullptr;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // LAGSTATSDIALOG_HPP
