/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifdef NETPLAY

#include "LagStatsDialog.hpp"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

using namespace UserInterface::Dialog;

namespace
{
enum PlayerColumn
{
    ColumnPlayer,
    ColumnFps,
    ColumnFrame,
    ColumnLong,
    ColumnStutters,
    ColumnHeldUp,
    ColumnDropouts,
    ColumnWaited,
    ColumnPing,
    ColumnRollbacks,
    ColumnCount
};

bool isDark()
{
    return QApplication::palette().window().color().value() < 128;
}

QColor severityColor(LagReport::Severity severity)
{
    const bool dark = isDark();
    switch (severity)
    {
    case LagReport::Severity::Lag:
        return QColor(dark ? "#EF5350" : "#C62828");
    case LagReport::Severity::Minor:
        return QColor(dark ? "#FFCA28" : "#B26A00");
    case LagReport::Severity::Fine:
        return QColor(dark ? "#4CAF50" : "#2E7D32");
    case LagReport::Severity::None:
        break;
    }
    return QColor(dark ? "#BDBDBD" : "#9E9E9E");
}

QString number(double value, int decimals)
{
    return QString::number(value, 'f', decimals);
}

QTreeWidget* makeTree(QWidget* parent, const QStringList& headers)
{
    auto* tree = new QTreeWidget(parent);
    tree->setObjectName("LagTree");
    tree->setRootIsDecorated(false);
    tree->setSelectionMode(QAbstractItemView::NoSelection);
    tree->setFocusPolicy(Qt::NoFocus);
    tree->setUniformRowHeights(true);
    tree->setHeaderLabels(headers);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionsMovable(false);
    tree->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    return tree;
}

// Fit a tree to its rows, so the window shows every player without scrolling.
void fitHeight(QTreeWidget* tree)
{
    int height = tree->header()->sizeHint().height() + 2 * tree->frameWidth();
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        height += tree->sizeHintForRow(i) > 0 ? tree->sizeHintForRow(i) : tree->fontMetrics().height() + 8;
    tree->setFixedHeight(height + 2);
}

// How much wider a tree must be to show every column whole.
int missingWidth(QTreeWidget* tree)
{
    // QTreeView hides sizeHintForColumn; the base class's is public.
    const QAbstractItemView* view = tree;
    int needed = 2 * tree->frameWidth();
    for (int i = 0; i < tree->columnCount(); ++i)
        needed += std::max(view->sizeHintForColumn(i), tree->header()->sectionSizeHint(i));
    return std::max(0, needed - tree->width());
}
} // namespace

LagStatsDialog::LagStatsDialog(ReportSource source, QWidget* parent)
    : QDialog(parent), m_source(std::move(source))
{
    setWindowTitle(tr("Lag Stats"));
    setObjectName("LagStatsDialog");
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(960, 430);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    // Verdict: a status dot, the headline and what's behind it.
    auto* topRow = new QHBoxLayout;
    topRow->setSpacing(8);
    m_dot = new QLabel(this);
    m_dot->setFixedSize(12, 12);
    topRow->addWidget(m_dot, 0, Qt::AlignVCenter);
    m_headline = new QLabel(this);
    m_headline->setWordWrap(true);
    m_headline->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont headlineFont = m_headline->font();
    headlineFont.setPointSizeF(headlineFont.pointSizeF() * 1.3);
    headlineFont.setBold(true);
    m_headline->setFont(headlineFont);
    topRow->addWidget(m_headline, 1);
    m_windowCombo = new QComboBox(this);
    m_windowCombo->setObjectName("LobbyCombo");
    m_windowCombo->addItem(tr("Last %1 s").arg(RollbackLagStats::RecentSeconds), false);
    m_windowCombo->addItem(tr("Whole match"), true);
    m_windowCombo->setToolTip(tr("The stretch of the match the numbers cover."));
    connect(m_windowCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LagStatsDialog::refresh);
    topRow->addWidget(m_windowCombo, 0, Qt::AlignTop);
    layout->addLayout(topRow);

    m_details = new QLabel(this);
    m_details->setWordWrap(true);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_details);

    auto* playersHeader = new QLabel(tr("PLAYERS"), this);
    playersHeader->setProperty("class", "SectionHeader");
    layout->addWidget(playersHeader);

    m_players = makeTree(this, {tr("Player"), tr("FPS"), tr("Frame time"), tr("Long frames"), tr("Stutters"),
                                tr("Others waited"), tr("Dropouts"), tr("Waited on others"), tr("Ping"), tr("Rollbacks")});
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
    for (int i = 0; i < tips.size(); ++i)
        m_players->headerItem()->setToolTip(i, tips.at(i));
    layout->addWidget(m_players);

    auto* linksHeader = new QLabel(tr("CONNECTIONS"), this);
    linksHeader->setProperty("class", "SectionHeader");
    layout->addWidget(linksHeader);
    m_links = makeTree(this, {tr("Between"), tr("Ping"), tr("Jitter")});
    m_links->headerItem()->setToolTip(2, tr("How much the game packets' round trip varies. Each game reads packets once a frame, so up to about 15 ms is normal on any connection; much more points to Wi-Fi or a busy line."));
    layout->addWidget(m_links);

    m_notes = new QLabel(this);
    m_notes->setWordWrap(true);
    layout->addWidget(m_notes);
    layout->addStretch(1);

    auto* buttons = new QHBoxLayout;
    auto* footnote = new QLabel(tr("Each player's RMG-K measures its own frames and shares them once a second."), this);
    footnote->setWordWrap(true);
    footnote->setEnabled(false);
    buttons->addWidget(footnote, 1);
    m_copyButton = new QPushButton(tr("Copy Report"), this);
    m_copyButton->setObjectName("LagCopyBtn");
    m_copyButton->setAutoDefault(false);
    m_copyButton->setToolTip(tr("Copy the report as text, to paste into a chat or a bug report."));
    connect(m_copyButton, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(LagStats::plainText(m_report));
        m_copyButton->setText(tr("Copied"));
        QTimer::singleShot(1500, this, [this]() { m_copyButton->setText(tr("Copy Report")); });
    });
    buttons->addWidget(m_copyButton);
    auto* closeButton = new QPushButton(tr("Close"), this);
    closeButton->setAutoDefault(false);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    buttons->addWidget(closeButton);
    layout->addLayout(buttons);

    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &LagStatsDialog::refresh);
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

void LagStatsDialog::refresh()
{
    m_report = m_source(m_windowCombo->currentData().toBool());
    const QColor color = severityColor(m_report.severity);
    m_dot->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 6px;").arg(color.name()));
    m_copyButton->setEnabled(m_report.severity != LagReport::Severity::None);

    if (m_report.severity == LagReport::Severity::None)
    {
        m_headline->setText(tr("No rollback match to check yet."));
        m_headline->setStyleSheet(QString());
        m_details->setText(tr("Lag stats appear once a rollback match is running. You can also type /lagstat in the room chat or the in-game chat."));
        m_players->clear();
        m_links->clear();
        m_notes->clear();
        fitHeight(m_players);
        fitHeight(m_links);
        return;
    }

    m_headline->setText(m_report.headline);
    m_headline->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
    QStringList details = m_report.details;
    m_details->setText(details.join(QStringLiteral("\n")));

    const QColor muted = QApplication::palette().color(QPalette::Disabled, QPalette::Text);
    m_players->clear();
    for (const LagReport::Row& row : m_report.rows)
    {
        auto* item = new QTreeWidgetItem(m_players);
        QString name = QStringLiteral("P%1  %2").arg(row.slot).arg(row.name);
        if (row.self)
            name += tr(" (you)");
        item->setText(ColumnPlayer, name);
        if (!row.reported)
        {
            item->setText(ColumnFps, tr("no stats"));
            item->setToolTip(ColumnFps, tr("This player hasn't sent lag stats: they may be on an older RMG-K, or their connection to you is down."));
            for (int column = ColumnFps; column < ColumnCount; ++column)
                item->setForeground(column, muted);
        }
        else
        {
            item->setText(ColumnFps, number(row.fps, 1));
            item->setText(ColumnFrame, row.frameMs >= 0 ? tr("%1 ms").arg(number(row.frameMs, 1)) : QStringLiteral("—"));
            item->setToolTip(ColumnFrame, tr("Longest: %1 ms").arg(number(row.frameMaxMs, 1)));
            item->setText(ColumnLong, QStringLiteral("%1%").arg(row.slowPercent));
            item->setText(ColumnStutters, row.stutters > 0 ? tr("%1 (%2 own)").arg(row.stutters).arg(row.ownStutters)
                                                           : QStringLiteral("0"));
            item->setText(ColumnWaited, LagStats::formatDuration(row.waitedMs));
            item->setText(ColumnRollbacks, tr("%1/s · max %2 f").arg(number(row.rollbacksPerSecond, 1)).arg(row.maxRollback));
            if (row.frameMs >= m_report.budgetMs * 0.9 || row.slowPercent >= 10)
            {
                item->setForeground(ColumnFrame, color);
                item->setForeground(ColumnLong, color);
            }
        }
        item->setText(ColumnHeldUp, LagStats::formatDuration(row.heldUpMs));
        item->setText(ColumnDropouts, LagStats::formatDuration(row.dropoutMs));
        if (row.longestDropoutMs > 0)
            item->setToolTip(ColumnDropouts, tr("Longest: %1").arg(LagStats::formatDuration(row.longestDropoutMs)));
        item->setText(ColumnPing, row.pingMs >= 0 ? tr("%1 ms").arg(row.pingMs) : QStringLiteral("—"));
        for (int column = ColumnFps; column < ColumnCount; ++column)
            item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
        if (row.culprit)
        {
            // The name, and the number the verdict rests on.
            QFont font = item->font(ColumnPlayer);
            font.setBold(true);
            const int evidence = row.dropoutMs > 0 ? ColumnDropouts : ColumnHeldUp;
            for (int column : {int(ColumnPlayer), evidence})
            {
                item->setFont(column, font);
                item->setForeground(column, color);
            }
        }
    }
    fitHeight(m_players);
    // Grow rather than cut names short; never shrink under the user's hand.
    if (const int missing = missingWidth(m_players); missing > 0)
        resize(width() + missing, height());

    m_links->clear();
    for (const LagReport::Link& link : m_report.links)
    {
        auto* item = new QTreeWidgetItem(m_links);
        item->setText(0, link.between);
        item->setText(1, link.pingMs >= 0 ? tr("%1 ms").arg(link.pingMs) : QStringLiteral("—"));
        item->setText(2, link.jitterMs >= 0 ? tr("%1 ms").arg(link.jitterMs) : QStringLiteral("—"));
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
    }
    fitHeight(m_links);

    QPalette notesPalette = m_notes->palette();
    notesPalette.setColor(QPalette::WindowText, muted);
    m_notes->setPalette(notesPalette);
    m_notes->setText(m_report.notes.join(QStringLiteral("\n")));
    m_notes->setVisible(!m_report.notes.isEmpty());
}

#endif // NETPLAY
