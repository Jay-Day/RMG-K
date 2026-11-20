#ifdef NETPLAY

#include "LobbyPrivateChats.hpp"

#include "LobbySocialWidgets.hpp"
#include "LobbyTheme.hpp"
#include "LobbyWidgets.hpp"

#include <QEnterEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

using namespace UserInterface::Dialog;
using LobbyTheme::Role;

namespace
{
    // How wide a name gets on its tab before it's elided, and how far it gives
    // way when the bar is full (past that, the bar scrolls).
    constexpr int kMaxTabNameWidth = 112;
    constexpr int kMinTabNameWidth = 52;
} // namespace

// ── The tab's face ───────────────────────────────────────────────────

// What a conversation's tab shows: the other player's tile and name, the unread
// count, and a close button. The tab bar sizes the tab from it and lays it over
// the tab, so clicks off the close button fall through to the bar.
class PrivateChats::Face : public QWidget
{
public:
    Face()
    {
        auto* lay = new QHBoxLayout(this);
        lay->setContentsMargins(8, 0, 3, 0);
        lay->setSpacing(5);
        m_tile = new PlayerTile(16, this);
        lay->addWidget(m_tile);
        m_name = new ElidedLabel(Role::Muted, this);
        m_name->setFont(LobbyTheme::uiFont(12, QFont::DemiBold));
        m_name->setMaximumWidth(kMaxTabNameWidth);
        lay->addWidget(m_name, 1);
        m_badge = new QLabel(this);
        m_badge->setObjectName("ChatBadge");
        m_badge->setFont(LobbyTheme::uiFont(9, QFont::Bold));
        m_badge->setAlignment(Qt::AlignCenter);
        m_badge->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_badge->setVisible(false);
        lay->addWidget(m_badge);
        m_close = new IconButton(QStringLiteral("close-line"), 16, this);
        m_close->setObjectName("PmTabClose");
        m_close->setToolTip(PrivateChats::tr("Close"));
        m_close->setAccessibleName(PrivateChats::tr("Close"));
        lay->addWidget(m_close);
    }

    IconButton* closeButton() const { return m_close; }

    void setConversation(const PrivateConversation& c)
    {
        m_tile->setPlayer(c.remoteName);
        m_tile->setDimmed(c.gone);
        // Two of this PC's players can each be talking with the same person.
        m_name->setText(c.localIsOwner ? c.remoteName : PrivateChats::tr("%1 (to %2)").arg(c.remoteName, c.localName));
        m_name->setMinimumWidth(std::min(m_name->sizeHint().width(), kMinTabNameWidth));
        m_gone = c.gone;
        if (c.unread > 0)
            m_badge->setText(c.unread > 99 ? QStringLiteral("99+") : QString::number(c.unread));
        m_badge->setVisible(c.unread > 0);
        refreshName();
    }

    void setSelected(bool selected)
    {
        m_selected = selected;
        refreshName();
    }

protected:
    void enterEvent(QEnterEvent* event) override
    {
        QWidget::enterEvent(event);
        refreshName();
    }

    void leaveEvent(QEvent* event) override
    {
        QWidget::leaveEvent(event);
        refreshName();
    }

private:
    // The colors the stylesheet gives the fixed tabs' text.
    void refreshName()
    {
        m_name->setRole(m_gone ? Role::Faint : m_selected ? Role::Ink : underMouse() ? Role::Ink2 : Role::Muted);
    }

    PlayerTile* m_tile;
    ElidedLabel* m_name;
    QLabel* m_badge;
    IconButton* m_close;
    bool m_selected = false;
    bool m_gone = false;
};

// ── The page ─────────────────────────────────────────────────────────

// Who the conversation is with and what they're doing, over its lines.
class PrivateChats::Page : public QWidget
{
public:
    explicit Page(QWidget* parent)
        : QWidget(parent)
    {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);

        auto* header = new QFrame(this);
        header->setObjectName("PmThreadHeader");
        auto* headerLay = new QHBoxLayout(header);
        headerLay->setContentsMargins(6, 3, 6, 3);
        headerLay->setSpacing(5);
        m_dot = new LedDot(header);
        headerLay->addWidget(m_dot);
        m_name = new ElidedLabel(Role::Ink, header);
        m_name->setFont(LobbyTheme::uiFont(12, QFont::Bold));
        headerLay->addWidget(m_name);
        m_flag = new CountryFlag(header);
        headerLay->addWidget(m_flag);
        m_to = new StatusTag(StatusTag::Kind::Neutral, header);
        m_to->setVisible(false);
        headerLay->addWidget(m_to);
        m_status = new ElidedLabel(Role::Muted, header);
        m_status->setFont(LobbyTheme::uiFont(11));
        m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        headerLay->addWidget(m_status, 1);
        m_challenge = new IconButton(QStringLiteral("sword-fill"), 24, header);
        m_challenge->setObjectName("PmChallenge");
        m_challenge->setToolTip(PrivateChats::tr("Challenge"));
        m_challenge->setAccessibleName(PrivateChats::tr("Challenge"));
        headerLay->addWidget(m_challenge);
        m_more = new IconButton(QStringLiteral("more-2-fill"), 24, header);
        m_more->setObjectName("PmMore");
        m_more->setToolTip(PrivateChats::tr("More"));
        m_more->setAccessibleName(PrivateChats::tr("More"));
        headerLay->addWidget(m_more);
        root->addWidget(header);

        // The same margins as the other tabs' chat.
        m_view = new ChatView(this);
        auto* viewWrap = new QVBoxLayout;
        viewWrap->setContentsMargins(10, 6, 4, 2);
        viewWrap->addWidget(m_view);
        root->addLayout(viewWrap, 1);
        m_view->addLine(QString("<i style='color:%1;'>%2</i>")
            .arg(LobbyTheme::tokens().muted.name(),
                 PrivateChats::tr("Only the two of you see this. It's gone when either of you leaves the lobby.")));
    }

    ChatView* view() const { return m_view; }
    IconButton* challengeButton() const { return m_challenge; }
    IconButton* moreButton() const { return m_more; }

    void setStatus(const QString& state, const QString& statusText, const QString& country, bool canChallenge)
    {
        m_state = state;
        m_statusText = statusText;
        m_country = country;
        m_canChallenge = canChallenge;
    }

    void refresh(const PrivateConversation& c)
    {
        const LobbyTheme::Tokens& t = LobbyTheme::tokens();
        m_dot->setColor(c.gone ? t.faint : presenceColor(m_state));
        m_name->setText(c.remoteName);
        m_name->setRole(c.gone ? Role::Faint : Role::Ink);
        m_flag->setCountry(m_country);
        m_to->setVisible(!c.localIsOwner);
        if (!c.localIsOwner)
            m_to->setTag(StatusTag::Kind::Neutral, PrivateChats::tr("To %1").arg(c.localName));
        m_status->setText(c.localGone ? PrivateChats::tr("%1 left this PC").arg(c.localName)
                          : c.gone ? PrivateChats::tr("Left the lobby") : m_statusText);
        // A challenge goes out as this PC's owner, so answering as someone else it
        // would come from a name the other side wasn't talking to.
        m_challenge->setVisible(!c.gone && m_canChallenge && c.localIsOwner);
        m_more->setVisible(!c.gone);
    }

private:
    LedDot* m_dot;
    ElidedLabel* m_name;
    CountryFlag* m_flag;
    StatusTag* m_to;
    ElidedLabel* m_status;
    IconButton* m_challenge;
    IconButton* m_more;
    ChatView* m_view;
    QString m_state;
    QString m_statusText;
    QString m_country;
    bool m_canChallenge = true;
};

// ── PrivateChats ─────────────────────────────────────────────────────

PrivateChats::PrivateChats(ChatTabBar* tabs, QStackedWidget* stack, QObject* parent)
    : QObject(parent), m_tabs(tabs), m_stack(stack)
{
    m_currentPage = m_stack->widget(m_tabs->currentIndex());
    connect(m_tabs, &QTabBar::currentChanged, this, &PrivateChats::onCurrentChanged);
    connect(m_tabs, &QTabBar::tabCloseRequested, this, &PrivateChats::close);
    connect(m_tabs, &ChatTabBar::tabMenuRequested, this, [this](int tab, const QPoint& globalPos) {
        if (const PrivateConversation* c = conversationAt(tab); c && !c->gone)
            emit menuRequested(c->remoteId, globalPos);
    });
}

ChatView* PrivateChats::conversation(quint64 localId, const QString& localName,
                                     quint64 remoteId, const QString& remoteName, bool localIsOwner)
{
    const quint64 key = keyOf(localId, remoteId);
    if (const auto it = m_entries.constFind(key); it != m_entries.constEnd())
        return it->page->view();

    Entry& entry = m_entries[key];
    entry.conversation.localId = localId;
    entry.conversation.localName = localName;
    entry.conversation.localIsOwner = localIsOwner;
    entry.conversation.remoteId = remoteId;
    entry.conversation.remoteName = remoteName;
    entry.page = new Page(m_stack);
    entry.face = new Face;
    m_keyOfPage.insert(entry.page, key);

    // Tab and page both go last, so they keep sharing an index.
    m_stack->addWidget(entry.page);
    const int tab = m_tabs->addTab(QString());
    m_tabs->setTabFace(tab, entry.face);
    m_tabs->showTab(tab, false);

    connect(entry.page->view(), &ChatView::userActivated, this, &PrivateChats::userActivated);
    connect(entry.page->challengeButton(), &QToolButton::clicked, this, [this, remoteId]() {
        emit challengeRequested(remoteId);
    });
    IconButton* more = entry.page->moreButton();
    connect(more, &QToolButton::clicked, this, [this, remoteId, more]() {
        emit menuRequested(remoteId, more->mapToGlobal(QPoint(0, more->height())));
    });
    Page* page = entry.page;
    connect(entry.face->closeButton(), &QToolButton::clicked, this, [this, page]() {
        close(m_stack->indexOf(page));
    });

    refreshTab(entry);
    return entry.page->view();
}

bool PrivateChats::open(quint64 localId, quint64 remoteId)
{
    const auto it = m_entries.constFind(keyOf(localId, remoteId));
    if (it == m_entries.constEnd())
        return false;
    const int tab = tabOf(*it);
    m_tabs->showTab(tab, true);
    m_tabs->setCurrentIndex(tab);
    return true;
}

void PrivateChats::noteMessage(quint64 localId, quint64 remoteId, qint64 timeMs, bool incoming)
{
    const auto it = m_entries.find(keyOf(localId, remoteId));
    if (it == m_entries.end())
        return;
    it->conversation.lastMs = timeMs;
    const int tab = tabOf(*it);
    m_tabs->showTab(tab, true);
    if (incoming && m_tabs->currentIndex() != tab)
        setUnread(*it, it->conversation.unread + 1);
}

void PrivateChats::close(int tab)
{
    Entry* entry = entryAt(tab);
    if (!entry)
        return;
    if (m_tabs->currentIndex() == tab)
    {
        // Back to where they were, else the nearest tab to the left. Before
        // hiding: Qt would otherwise move on to the next visible tab.
        int next = m_previousPage ? m_stack->indexOf(m_previousPage) : -1;
        if (next < 0 || next == tab || !m_tabs->isTabVisible(next))
        {
            next = tab - 1;
            while (next > 0 && !m_tabs->isTabVisible(next))
                --next;
        }
        m_tabs->setCurrentIndex(std::max(next, 0));
    }
    m_tabs->showTab(tab, false);
    setUnread(*entry, 0);
}

void PrivateChats::clear()
{
    if (conversationAt(m_tabs->currentIndex()))
        m_tabs->setCurrentIndex(0);
    // From the end, so the tabs before keep their indexes.
    for (int tab = m_tabs->count() - 1; tab >= 0; --tab)
    {
        const Entry* entry = entryAt(tab);
        if (!entry)
            continue;
        QPointer<Face> face = entry->face;
        Page* page = entry->page;
        m_tabs->removeTab(tab);
        if (face)
            face->deleteLater();
        m_stack->removeWidget(page);
        page->deleteLater();
    }
    m_entries.clear();
    m_keyOfPage.clear();
    emit currentChanged();
}

const PrivateConversation* PrivateChats::conversationAt(int tab) const
{
    const Entry* entry = entryAt(tab);
    return entry ? &entry->conversation : nullptr;
}

bool PrivateChats::isCurrent(quint64 localId, quint64 remoteId) const
{
    const PrivateConversation* c = conversationAt(m_tabs->currentIndex());
    return c && c->localId == localId && c->remoteId == remoteId;
}

quint64 PrivateChats::localFor(quint64 remoteId) const
{
    const PrivateConversation* latest = nullptr;
    for (const Entry& entry : m_entries)
    {
        const PrivateConversation& c = entry.conversation;
        if (c.remoteId == remoteId && !c.gone && (!latest || c.lastMs > latest->lastMs))
            latest = &c;
    }
    return latest ? latest->localId : 0;
}

ChatView* PrivateChats::existingView(quint64 remoteId) const
{
    const quint64 local = localFor(remoteId);
    const auto it = m_entries.constFind(keyOf(local, remoteId));
    return local != 0 && it != m_entries.constEnd() ? it->page->view() : nullptr;
}

QList<ChatView*> PrivateChats::views() const
{
    QList<ChatView*> out;
    for (const Entry& entry : m_entries)
        out << entry.page->view();
    return out;
}

void PrivateChats::setRemoteStatus(quint64 remoteId, const QString& state, const QString& statusText,
                                   const QString& country, bool canChallenge)
{
    for (Entry& entry : m_entries)
    {
        if (entry.conversation.remoteId != remoteId || entry.conversation.gone)
            continue;
        entry.page->setStatus(state, statusText, country, canChallenge);
        entry.page->refresh(entry.conversation);
    }
}

void PrivateChats::markGone(quint64 userId)
{
    bool current = false;
    for (Entry& entry : m_entries)
    {
        PrivateConversation& c = entry.conversation;
        if ((c.remoteId != userId && c.localId != userId) || c.gone)
            continue;
        c.gone = true;
        c.localGone = c.localId == userId;
        entry.page->view()->addLine(QString("<i style='color:%1;'>%2</i>")
            .arg(LobbyTheme::tokens().muted.name(),
                 c.localGone ? tr("%1 left this PC. Nobody can answer as them any more.").arg(c.localName.toHtmlEscaped())
                             : tr("%1 left the lobby. Messages can't reach them.").arg(c.remoteName.toHtmlEscaped())));
        refreshTab(entry);
        current = current || tabOf(entry) == m_tabs->currentIndex();
    }
    if (current)
        emit currentChanged();
}

PrivateChats::Entry* PrivateChats::entryAt(int tab)
{
    const auto key = m_keyOfPage.constFind(m_stack->widget(tab));
    if (key == m_keyOfPage.constEnd())
        return nullptr;
    const auto it = m_entries.find(*key);
    return it != m_entries.end() ? &it.value() : nullptr;
}

const PrivateChats::Entry* PrivateChats::entryAt(int tab) const
{
    const auto key = m_keyOfPage.constFind(m_stack->widget(tab));
    if (key == m_keyOfPage.constEnd())
        return nullptr;
    const auto it = m_entries.constFind(*key);
    return it != m_entries.constEnd() ? &it.value() : nullptr;
}

int PrivateChats::tabOf(const Entry& entry) const
{
    return m_stack->indexOf(entry.page);
}

void PrivateChats::refreshTab(const Entry& entry)
{
    const PrivateConversation& c = entry.conversation;
    const int tab = tabOf(entry);
    if (entry.face)
    {
        entry.face->setConversation(c);
        entry.face->setSelected(tab == m_tabs->currentIndex());
    }
    entry.page->refresh(c);
    QString tip = c.remoteName;
    if (!c.localIsOwner)
        tip += QLatin1Char('\n') + tr("To %1").arg(c.localName);
    if (c.localGone)
        tip += QLatin1Char('\n') + tr("%1 left this PC").arg(c.localName);
    else if (c.gone)
        tip += QLatin1Char('\n') + tr("Left the lobby");
    m_tabs->setTabToolTip(tab, tip);
    m_tabs->setAccessibleTabName(tab, c.remoteName);
    m_tabs->setTabAttention(tab, c.unread > 0);
    m_tabs->refreshTab(tab);
}

void PrivateChats::setUnread(Entry& entry, int unread)
{
    if (entry.conversation.unread == unread)
        return;
    entry.conversation.unread = unread;
    refreshTab(entry);
}

void PrivateChats::onCurrentChanged(int tab)
{
    if (QWidget* page = m_stack->widget(tab); page != m_currentPage)
    {
        m_previousPage = m_currentPage;
        m_currentPage = page;
    }
    for (Entry& entry : m_entries)
    {
        const bool selected = tabOf(entry) == tab;
        if (entry.face)
            entry.face->setSelected(selected);
        if (selected)
            setUnread(entry, 0);
    }
}

#endif // NETPLAY
