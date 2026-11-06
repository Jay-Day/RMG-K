#ifdef NETPLAY

#include "LagStatsDialog.hpp"
#include "LobbyTheme.hpp"
#include "LobbyWidgets.hpp"

#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

using namespace UserInterface::Dialog;
using LobbyTheme::Role;
using LobbyTheme::Tokens;

namespace
{
QString number(double value, int decimals)
{
    return QString::number(value, 'f', decimals);
}

QLabel* fieldLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text.toUpper(), parent);
    QFont font = LobbyTheme::uiFont(10, QFont::DemiBold);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.9);
    label->setFont(font);
    label->setProperty("role", "muted");
    return label;
}

QFrame* rule(QWidget* parent)
{
    auto* line = new QFrame(parent);
    line->setObjectName("FieldRule");
    return line;
}

QLabel* cell(const QString& text, QWidget* parent, bool right = true, const QColor& color = QColor(), bool bold = false)
{
    auto* label = new QLabel(text, parent);
    label->setFont(LobbyTheme::uiFont(12, bold ? QFont::DemiBold : QFont::Normal));
    if (right)
        label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    if (color.isValid())
        label->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
    return label;
}

QString severityIcon(LagReport::Severity severity)
{
    switch (severity)
    {
    case LagReport::Severity::Lag:
        return QStringLiteral("error-warning-line");
    case LagReport::Severity::Minor:
        return QStringLiteral("information-line");
    case LagReport::Severity::Fine:
        return QStringLiteral("check-line");
    case LagReport::Severity::None:
        break;
    }
    return QStringLiteral("time-line");
}

void clearGrid(QGridLayout* grid)
{
    while (QLayoutItem* item = grid->takeAt(0))
    {
        if (QWidget* widget = item->widget())
            widget->deleteLater();
        delete item;
    }
}

// A table drawn the lobby's way: a header row of field labels and a hairline
// under every row.
QWidget* makeTable(QWidget* parent, QGridLayout** grid)
{
    auto* box = new QWidget(parent);
    *grid = new QGridLayout(box);
    (*grid)->setContentsMargins(0, 0, 0, 0);
    (*grid)->setHorizontalSpacing(18);
    (*grid)->setVerticalSpacing(0);
    return box;
}

// Each table row is a 30 px band; its rule spans every column.
void addRowRule(QGridLayout* grid, int row, int columns, QWidget* parent)
{
    grid->addWidget(rule(parent), row, 0, 1, columns);
}
} // namespace

LagStatsDialog::LagStatsDialog(ReportSource source, QWidget* parent)
    : QDialog(parent), m_source(std::move(source))
{
    setWindowTitle(tr("Lag Stats"));
    setWindowIcon(QIcon(":Resource/RMG.png"));
    setObjectName("LagStatsDialog");
    resize(980, 470);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // The band: what this is, and which stretch of the match it covers.
    auto* band = new LobbyBand(this);
    auto* bandLay = new QHBoxLayout(band);
    bandLay->setContentsMargins(18, 12, 18, 12);
    bandLay->setSpacing(12);
    bandLay->addWidget(new IconLabel(QStringLiteral("pulse-line"), Role::Accent, 22, band));
    auto* words = new QVBoxLayout;
    words->setSpacing(1);
    auto* title = new QLabel(tr("Lag Stats").toUpper(), band);
    title->setFont(LobbyTheme::pixelFont(15, 0.5));
    words->addWidget(title);
    auto* detail = new QLabel(tr("Each player's Dale64 measures its own frames and shares them once a second."), band);
    detail->setFont(LobbyTheme::uiFont(11));
    detail->setProperty("role", "muted");
    words->addWidget(detail);
    bandLay->addLayout(words, 1);
    const auto makeChip = [band](const QString& text, const QString& name) {
        auto* chip = new QPushButton(text, band);
        chip->setObjectName(name);
        chip->setProperty("chip", true);
        chip->setCheckable(true);
        chip->setAutoDefault(false);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setFont(LobbyTheme::uiFont(11, QFont::DemiBold));
        return chip;
    };
    m_recentChip = makeChip(tr("Last %1 s").arg(RollbackLagStats::RecentSeconds), QStringLiteral("LagRecent"));
    m_matchChip = makeChip(tr("Whole match"), QStringLiteral("LagMatch"));
    m_recentChip->setToolTip(tr("The stretch of the match the numbers cover."));
    m_matchChip->setToolTip(m_recentChip->toolTip());
    bandLay->addWidget(m_recentChip);
    bandLay->addWidget(m_matchChip);
    connect(m_recentChip, &QPushButton::clicked, this, [this]() { showWindow(false); });
    connect(m_matchChip, &QPushButton::clicked, this, [this]() { showWindow(true); });
    root->addWidget(band);
    root->addWidget(rule(this));

    auto* body = new QWidget(this);
    auto* bodyLay = new QVBoxLayout(body);
    bodyLay->setContentsMargins(18, 14, 18, 12);
    bodyLay->setSpacing(10);

    // The verdict: a badge in its colour, the one-line answer and why.
    auto* verdict = new QHBoxLayout;
    verdict->setSpacing(12);
    m_badge = new IconBadge(34, 9, 20, body);
    verdict->addWidget(m_badge, 0, Qt::AlignTop);
    auto* verdictWords = new QVBoxLayout;
    verdictWords->setSpacing(3);
    m_headline = new QLabel(body);
    m_headline->setFont(LobbyTheme::uiFont(16, QFont::DemiBold));
    m_headline->setWordWrap(true);
    m_headline->setTextInteractionFlags(Qt::TextSelectableByMouse);
    verdictWords->addWidget(m_headline);
    m_details = new QLabel(body);
    m_details->setFont(LobbyTheme::uiFont(12));
    m_details->setProperty("role", "ink2");
    m_details->setWordWrap(true);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    verdictWords->addWidget(m_details);
    verdict->addLayout(verdictWords, 1);
    bodyLay->addLayout(verdict);

    bodyLay->addSpacing(4);
    bodyLay->addWidget(fieldLabel(tr("Players"), body));
    m_playersBox = makeTable(body, &m_players);
    bodyLay->addWidget(m_playersBox);

    bodyLay->addSpacing(4);
    bodyLay->addWidget(fieldLabel(tr("Connections"), body));
    m_linksBox = makeTable(body, &m_links);
    bodyLay->addWidget(m_linksBox);

    m_notes = new QLabel(body);
    m_notes->setFont(LobbyTheme::uiFont(11));
    m_notes->setProperty("role", "muted");
    m_notes->setWordWrap(true);
    bodyLay->addWidget(m_notes);
    bodyLay->addStretch(1);
    root->addWidget(body, 1);

    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(18, 0, 18, 14);
    footer->setSpacing(8);
    footer->addStretch(1);
    m_copyButton = new LobbyButton(tr("Copy Report"), LobbyButton::Kind::Secondary, this);
    m_copyButton->setObjectName("LagCopyBtn");
    m_copyButton->setIconName(QStringLiteral("file-copy-line"));
    m_copyButton->setAutoDefault(false);
    m_copyButton->setToolTip(tr("Copy the report as text, to paste into a chat or a bug report."));
    connect(m_copyButton, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(LagStats::plainText(m_report));
        m_copyButton->setText(tr("Copied"));
        QTimer::singleShot(1500, this, [this]() { m_copyButton->setText(tr("Copy Report")); });
    });
    footer->addWidget(m_copyButton);
    auto* closeButton = new LobbyButton(tr("Close"), LobbyButton::Kind::Ghost, this);
    closeButton->setAutoDefault(false);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    footer->addWidget(closeButton);
    root->addLayout(footer);

    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &LagStatsDialog::refresh);
    showWindow(false);
}

void LagStatsDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    refresh();
    m_timer->start();
}

void LagStatsDialog::hideEvent(QHideEvent* event)
{
    m_timer->stop();
    QDialog::hideEvent(event);
}

void LagStatsDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::ApplicationPaletteChange && isVisible())
        refresh();
}

void LagStatsDialog::showWindow(bool wholeMatch)
{
    m_wholeMatch = wholeMatch;
    m_recentChip->setChecked(!wholeMatch);
    m_matchChip->setChecked(wholeMatch);
    if (isVisible())
        refresh();
}

void LagStatsDialog::refresh()
{
    m_report = m_source(m_wholeMatch);
    const Tokens& t = LobbyTheme::tokens();
    const QColor color = LagStats::severityColor(m_report.severity);
    m_badge->setIconAndColors(severityIcon(m_report.severity), color, LobbyTheme::mix(color, t.panel, 0.16));
    m_copyButton->setEnabled(m_report.severity != LagReport::Severity::None);

    setUpdatesEnabled(false);
    if (m_report.severity == LagReport::Severity::None)
    {
        m_headline->setText(tr("No rollback match to check yet."));
        m_headline->setStyleSheet(QString());
        m_details->setText(tr("Lag stats appear once a rollback match is running. You can also type /lagstat in the room chat or the in-game chat."));
        clearGrid(m_players);
        clearGrid(m_links);
        m_playersBox->setVisible(false);
        m_linksBox->setVisible(false);
        m_notes->clear();
        setUpdatesEnabled(true);
        return;
    }

    m_headline->setText(m_report.headline);
    m_headline->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
    m_details->setText(m_report.details.join(QStringLiteral("\n")));
    m_playersBox->setVisible(true);
    fillPlayers();
    m_linksBox->setVisible(!m_report.links.isEmpty());
    fillLinks();
    m_notes->setText(m_report.notes.join(QStringLiteral("\n")));
    m_notes->setVisible(!m_report.notes.isEmpty());
    setUpdatesEnabled(true);
}

void LagStatsDialog::fillPlayers()
{
    enum Column { Player, Fps, Frame, Long, Stutters, OthersWaited, Dropouts, Waited, Ping, Rollbacks, Count };
    clearGrid(m_players);
    const Tokens& t = LobbyTheme::tokens();
    const QColor alarm = LagStats::severityColor(m_report.severity);

    const QStringList headers = {tr("Player"), tr("FPS"), tr("Frame time"), tr("Long frames"), tr("Stutters"),
                                 tr("Others waited"), tr("Dropouts"), tr("Waited on others"), tr("Ping"), tr("Rollbacks")};
    const QStringList tips = {
        QString(),
        tr("Frames the game ran per second. Full speed is about 60."),
        tr("How long this player's PC took to run a frame (emulation, rollbacks and drawing), not counting waits. It has to stay under the frame time, about 16.7 ms."),
        tr("Share of frames this player's PC took longer to run than a frame allows."),
        tr("Times the picture froze for 2.5 frames or more. In brackets: how many came from this player's own PC rather than waiting on someone."),
        tr("The longest any other player's game stood still waiting for this player's inputs. A player held up by someone else falls behind and shows up here too, so read it with the verdict above."),
        tr("Of that wait, the time nothing at all arrived from this player: lost packets, a connection dropping out, or a frozen PC. A player who is only behind keeps sending."),
        tr("How long this player's game stood still waiting for the others' inputs."),
        tr("Worst ping between this player and anyone else."),
        tr("Rollbacks per second, and the most frames one rollback replayed."),
    };
    for (int column = 0; column < Count; ++column)
    {
        QLabel* label = fieldLabel(headers.at(column), m_playersBox);
        label->setToolTip(tips.at(column));
        label->setMinimumHeight(24);
        label->setAlignment((column == Player ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter);
        m_players->addWidget(label, 0, column);
    }
    m_players->setColumnStretch(Player, 1);

    int gridRow = 1;
    for (const LagReport::Row& row : m_report.rows)
    {
        addRowRule(m_players, gridRow++, Count, m_playersBox);

        auto* who = new QWidget(m_playersBox);
        who->setMinimumHeight(32);
        auto* whoLay = new QHBoxLayout(who);
        whoLay->setContentsMargins(0, 0, 0, 0);
        whoLay->setSpacing(8);
        whoLay->addWidget(new PortChip(row.slot, who));
        auto* name = new QLabel(row.name, who);
        name->setFont(LobbyTheme::uiFont(12, QFont::DemiBold));
        if (row.culprit)
            name->setStyleSheet(QStringLiteral("color: %1;").arg(alarm.name()));
        whoLay->addWidget(name);
        if (row.self)
        {
            auto* you = new QLabel(tr("you"), who);
            you->setFont(LobbyTheme::uiFont(11));
            you->setProperty("role", "muted");
            whoLay->addWidget(you);
        }
        whoLay->addStretch(1);
        m_players->addWidget(who, gridRow, Player);

        if (!row.reported)
        {
            QLabel* none = cell(tr("no stats"), m_playersBox, false, t.muted);
            none->setToolTip(tr("This player hasn't sent lag stats: they may be on an older Dale64, or their connection to you is down."));
            m_players->addWidget(none, gridRow, Fps, 1, Stutters - Fps + 1);
        }
        else
        {
            const bool slowPc = row.frameMs >= m_report.budgetMs * 0.9 || row.slowPercent >= 10;
            const QColor frameColor = slowPc ? alarm : QColor();
            m_players->addWidget(cell(number(row.fps, 1), m_playersBox), gridRow, Fps);
            QLabel* frame = cell(row.frameMs >= 0 ? tr("%1 ms").arg(number(row.frameMs, 1)) : QStringLiteral("—"),
                                 m_playersBox, true, frameColor, slowPc);
            frame->setToolTip(tr("Longest: %1 ms").arg(number(row.frameMaxMs, 1)));
            m_players->addWidget(frame, gridRow, Frame);
            m_players->addWidget(cell(QStringLiteral("%1%").arg(row.slowPercent), m_playersBox, true, frameColor, slowPc),
                                 gridRow, Long);
            m_players->addWidget(cell(row.stutters > 0 ? tr("%1 (%2 own)").arg(row.stutters).arg(row.ownStutters)
                                                       : QStringLiteral("0"), m_playersBox),
                                 gridRow, Stutters);
            m_players->addWidget(cell(LagStats::formatDuration(row.waitedMs), m_playersBox), gridRow, Waited);
            m_players->addWidget(cell(tr("%1/s · max %2 f").arg(number(row.rollbacksPerSecond, 1)).arg(row.maxRollback),
                                      m_playersBox, true, t.ink2),
                                 gridRow, Rollbacks);
        }
        // The number the verdict rests on, in its colour.
        const bool dropEvidence = row.culprit && row.dropoutMs > 0;
        const bool waitEvidence = row.culprit && !dropEvidence;
        m_players->addWidget(cell(LagStats::formatDuration(row.heldUpMs), m_playersBox, true,
                                  waitEvidence ? alarm : QColor(), waitEvidence),
                             gridRow, OthersWaited);
        QLabel* dropout = cell(LagStats::formatDuration(row.dropoutMs), m_playersBox, true,
                               dropEvidence ? alarm : QColor(), dropEvidence);
        if (row.longestDropoutMs > 0)
            dropout->setToolTip(tr("Longest: %1").arg(LagStats::formatDuration(row.longestDropoutMs)));
        m_players->addWidget(dropout, gridRow, Dropouts);
        auto* ping = new QWidget(m_playersBox);
        auto* pingLay = new QHBoxLayout(ping);
        pingLay->setContentsMargins(0, 0, 0, 0);
        pingLay->setSpacing(5);
        pingLay->addStretch(1);
        auto* bars = new SignalBars(ping);
        bars->setPing(row.pingMs, false);
        pingLay->addWidget(bars);
        pingLay->addWidget(cell(row.pingMs >= 0 ? tr("%1 ms").arg(row.pingMs) : QStringLiteral("—"), ping));
        m_players->addWidget(ping, gridRow, Ping);
        ++gridRow;
    }
}

void LagStatsDialog::fillLinks()
{
    clearGrid(m_links);
    const QStringList headers = {tr("Between"), tr("Ping"), tr("Jitter")};
    for (int column = 0; column < headers.size(); ++column)
    {
        QLabel* label = fieldLabel(headers.at(column), m_linksBox);
        label->setMinimumHeight(24);
        label->setAlignment((column == 0 ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter);
        m_links->addWidget(label, 0, column);
    }
    m_links->itemAtPosition(0, 2)->widget()->setToolTip(
        tr("How much the game packets' round trip varies. Each game reads packets once a frame, so up to about 15 ms is normal on any connection; much more points to Wi-Fi or a busy line."));
    m_links->setColumnStretch(0, 1);

    int gridRow = 1;
    for (const LagReport::Link& link : m_report.links)
    {
        addRowRule(m_links, gridRow++, headers.size(), m_linksBox);
        QLabel* between = cell(link.between, m_linksBox, false);
        between->setMinimumHeight(28);
        m_links->addWidget(between, gridRow, 0);
        m_links->addWidget(cell(link.pingMs >= 0 ? tr("%1 ms").arg(link.pingMs) : QStringLiteral("—"), m_linksBox), gridRow, 1);
        m_links->addWidget(cell(link.jitterMs >= 0 ? tr("%1 ms").arg(link.jitterMs) : QStringLiteral("—"), m_linksBox), gridRow, 2);
        ++gridRow;
    }
}

#endif // NETPLAY
