#ifdef NETPLAY

#include "LobbyWatchTracker.hpp"

#include "LobbyTheme.hpp"
#include "LobbyWidgets.hpp"

#include <QAbstractButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QSet>

using namespace UserInterface::Dialog;
using LobbyTheme::Role;
using LobbyTheme::Tokens;
using StreamViewer::State;

namespace UserInterface
{
namespace Dialog
{

// One stream: a dot for how it's doing, who's playing, its state, its sound
// and a close button. The whole chip is a button for its window, so the
// keyboard and screen readers can use it too.
class WatchEntry : public QAbstractButton
{
    Q_OBJECT

public:
    explicit WatchEntry(QWidget* parent)
        : QAbstractButton(parent)
    {
        setObjectName("WatchEntry");
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
        connect(this, &QAbstractButton::clicked, this, [this]() { emit entryClicked(m_view.matchId); });
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setFixedHeight(30);

        auto* lay = new QHBoxLayout(this);
        lay->setContentsMargins(9, 0, 3, 0);
        lay->setSpacing(6);
        m_dot = new LedDot(this);
        m_dot->setFixedSize(8, 8);
        lay->addWidget(m_dot);
        m_place = new IconLabel(QStringLiteral("eye-line"), Role::Muted, 13, this);
        lay->addWidget(m_place);
        m_title = new ElidedLabel(Role::Ink, this);
        m_title->setFont(LobbyTheme::uiFont(12, QFont::DemiBold));
        m_title->setMinimumWidth(60);
        m_title->setMaximumWidth(220);
        lay->addWidget(m_title, 1);
        m_state = new StatusTag(StatusTag::Kind::Neutral, this);
        lay->addWidget(m_state);
        m_sound = new IconButton(QStringLiteral("volume-up-line"), 22, this);
        m_sound->setObjectName("WatchEntryMute");
        lay->addWidget(m_sound);
        m_close = new IconButton(QStringLiteral("close-line"), 22, this);
        m_close->setObjectName("WatchEntryClose");
        lay->addWidget(m_close);

        connect(m_sound, &QToolButton::clicked, this, [this]() { emit muteToggled(m_view.matchId, !m_view.muted); });
        connect(m_close, &QToolButton::clicked, this, [this]() { emit closeClicked(m_view.matchId); });
    }

    void apply(const WatchEntryView& view)
    {
        m_view = view;
        const Tokens& t = LobbyTheme::tokens();
        m_title->setText(view.title);
        QString stateText;
        switch (view.state)
        {
        case State::Connecting:
            stateText = tr("Connecting");
            m_state->setTag(StatusTag::Kind::Neutral, stateText);
            m_dot->setColor(t.muted);
            m_dot->setPulsing(false);
            break;
        case State::CatchingUp:
            stateText = tr("Catching up");
            m_state->setTag(StatusTag::Kind::Amber, stateText);
            m_dot->setColor(t.warn);
            m_dot->setPulsing(false);
            break;
        case State::Live:
            stateText = tr("Live");
            m_state->setTag(StatusTag::Kind::Live, stateText);
            m_dot->setColor(t.live);
            m_dot->setPulsing(true);
            break;
        case State::Ended:
            stateText = tr("Ended");
            m_state->setTag(StatusTag::Kind::Neutral, stateText);
            m_dot->setColor(t.faint);
            m_dot->setPulsing(false);
            break;
        }
        // Which window it's in: the main one, or its own.
        m_place->setIconName(view.mainWindow ? QStringLiteral("home-4-line") : QStringLiteral("external-link-line"));
        m_sound->setIconName(view.muted ? QStringLiteral("volume-mute-line") : QStringLiteral("volume-up-line"));
        m_sound->setToolTip(view.muted ? tr("Turn its sound on") : tr("Mute it"));
        m_close->setToolTip(view.mainWindow ? tr("Stop watching") : tr("Close its window"));
        // Icons alone say nothing to a screen reader.
        m_sound->setAccessibleName(m_sound->toolTip());
        m_close->setAccessibleName(m_close->toolTip());
        const QString where = view.mainWindow ? tr("In the main window.") : tr("In a window of its own.");
        QString tip = view.game.isEmpty() ? view.title : tr("%1 · %2").arg(view.title, view.game);
        tip += QLatin1Char('\n') + where + QLatin1Char('\n') +
               tr("Click to bring it to the front and chat with its viewers.");
        setToolTip(tip);
        // The painted tag isn't text a screen reader can see, so the name says it.
        setAccessibleName(tr("%1, %2").arg(view.title, stateText));
        update();
    }

    QSize sizeHint() const override { return layout()->sizeHint(); }
    QSize minimumSizeHint() const override { return layout()->minimumSize(); }

signals:
    void entryClicked(quint64 matchId);
    void muteToggled(quint64 matchId, bool muted);
    void closeClicked(quint64 matchId);

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const Tokens& t = LobbyTheme::tokens();
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath path;
        path.addRoundedRect(box, 7, 7);
        painter.fillPath(path, m_view.selected ? t.watchTint : (underMouse() ? t.tint : t.panel2));
        painter.strokePath(path, QPen(m_view.selected || hasFocus() ? t.stateWatch : t.line, 1.0));
    }

private:
    WatchEntryView m_view;
    LedDot* m_dot = nullptr;
    IconLabel* m_place = nullptr;
    ElidedLabel* m_title = nullptr;
    StatusTag* m_state = nullptr;
    IconButton* m_sound = nullptr;
    IconButton* m_close = nullptr;
};

} // namespace Dialog
} // namespace UserInterface

WatchTracker::WatchTracker(QWidget* parent)
    : QFrame(parent)
{
    setObjectName("WatchTracker");
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(14, 6, 14, 6);
    lay->setSpacing(8);
    lay->addWidget(new IconLabel(QStringLiteral("eye-line"), Role::Watch, 14, this));
    auto* heading = new QLabel(tr("WATCHING"), this);
    heading->setFont(LobbyTheme::pixelFont(10, 1.0));
    heading->setProperty("role", "heading");
    centerDisplayCaps(heading);
    lay->addWidget(heading);
    m_entries = new QHBoxLayout;
    m_entries->setSpacing(6);
    lay->addLayout(m_entries);
    lay->addStretch(1);
    m_closeAll = new LobbyButton(tr("Close all"), LobbyButton::Kind::Ghost, this);
    m_closeAll->setObjectName("WatchCloseAll");
    m_closeAll->setButtonSize(LobbyButton::Size::Compact);
    m_closeAll->setIconName(QStringLiteral("close-line"));
    m_closeAll->setToolTip(tr("Stop watching all of them."));
    lay->addWidget(m_closeAll);
    connect(m_closeAll, &QPushButton::clicked, this, &WatchTracker::closeAllClicked);
    setVisible(false);
}

void WatchTracker::setEntries(const QList<WatchEntryView>& entries)
{
    QSet<quint64> keep;
    for (const WatchEntryView& view : entries)
        keep.insert(view.matchId);
    for (auto it = m_entryWidgets.begin(); it != m_entryWidgets.end();)
    {
        if (!keep.contains(it.key()))
        {
            it.value()->deleteLater();
            it = m_entryWidgets.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (int i = 0; i < entries.size(); ++i)
    {
        const WatchEntryView& view = entries.at(i);
        WatchEntry* entry = m_entryWidgets.value(view.matchId);
        if (entry == nullptr)
        {
            entry = new WatchEntry(this);
            connect(entry, &WatchEntry::entryClicked, this, &WatchTracker::entryClicked);
            connect(entry, &WatchEntry::muteToggled, this, &WatchTracker::muteToggled);
            connect(entry, &WatchEntry::closeClicked, this, &WatchTracker::closeClicked);
            m_entryWidgets.insert(view.matchId, entry);
        }
        entry->apply(view);
        // In the order given: the main window's first, then by opening.
        m_entries->removeWidget(entry);
        m_entries->insertWidget(i, entry);
    }
    m_closeAll->setVisible(entries.size() >= 2);
    setVisible(!entries.isEmpty());
}

void WatchTracker::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    const Tokens& t = LobbyTheme::tokens();
    painter.fillRect(rect(), LobbyTheme::mix(t.watchTint, t.bg, 0.5));
    painter.fillRect(QRectF(0, height() - 1, width(), 1), t.line);
}

#include "LobbyWatchTracker.moc"

#endif // NETPLAY
