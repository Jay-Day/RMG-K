#ifndef LAGSTATSDIALOG_HPP
#define LAGSTATSDIALOG_HPP

#ifdef NETPLAY

#include "LagStats.hpp"

#include <QDialog>

#include <functional>

class QGridLayout;
class QLabel;
class QPushButton;
class QTimer;
class QWidget;

namespace UserInterface
{
namespace Dialog
{

class IconBadge;
class LobbyButton;

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
    void changeEvent(QEvent* event) override;

  private:
    void refresh();
    void showWindow(bool wholeMatch);
    void fillPlayers();
    void fillLinks();

    ReportSource m_source;
    LagReport m_report;
    bool m_wholeMatch = false;
    QPushButton* m_recentChip = nullptr;
    QPushButton* m_matchChip = nullptr;
    IconBadge* m_badge = nullptr;
    QLabel* m_headline = nullptr;
    QLabel* m_details = nullptr;
    QWidget* m_playersBox = nullptr;
    QGridLayout* m_players = nullptr;
    QWidget* m_linksBox = nullptr;
    QGridLayout* m_links = nullptr;
    QLabel* m_notes = nullptr;
    LobbyButton* m_copyButton = nullptr;
    QTimer* m_timer = nullptr;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // LAGSTATSDIALOG_HPP
