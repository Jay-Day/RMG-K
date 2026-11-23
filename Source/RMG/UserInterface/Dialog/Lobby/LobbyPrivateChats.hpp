#ifndef LOBBYPRIVATECHATS_HPP
#define LOBBYPRIVATECHATS_HPP

#ifdef NETPLAY

#include <QHash>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QString>

class QStackedWidget;
class QWidget;

namespace UserInterface
{
namespace Dialog
{

class ChatTabBar;
class ChatView;

// One private conversation: a player on this PC and someone else. It goes by user
// id, never by name, because anyone can pick up a guest's name after they leave.
struct PrivateConversation
{
    quint64 localId = 0;
    QString localName;
    bool localIsOwner = true;
    quint64 remoteId = 0;
    QString remoteName;
    bool gone = false;      // one end left, so nobody can write in it any more
    bool localGone = false; // ...and it was this PC's end
    int unread = 0;
    qint64 lastMs = 0;
};

// The private conversations, a chat tab each after the bar's fixed tabs (Lobby,
// Room, Spectators). Each one's page goes at the same index of the chat stack, so
// a tab's index picks its page there as it does for the fixed tabs. Closing a tab
// only hides it: a conversation lasts as long as the connection, and its next line
// brings the tab back. The server keeps nothing.
class PrivateChats : public QObject
{
    Q_OBJECT

public:
    // The bar's fixed tabs and their pages must be in place already.
    PrivateChats(ChatTabBar* tabs, QStackedWidget* stack, QObject* parent);

    // Finds or starts the conversation between this PC's localId and remoteId. A
    // new one's tab stays hidden until it's opened or a line goes into it.
    ChatView* conversation(quint64 localId, const QString& localName,
                           quint64 remoteId, const QString& remoteName, bool localIsOwner);
    // Shows its tab and selects it. False when there's no such conversation.
    bool open(quint64 localId, quint64 remoteId);
    // A line went into it: its tab shows again, and an incoming line is unread
    // unless its tab is the selected one.
    void noteMessage(quint64 localId, quint64 remoteId, qint64 timeMs, bool incoming);
    // Hides the tab; the conversation stays.
    void close(int tab);
    // All of them, tabs and pages: the connection is over.
    void clear();

    // The conversation a tab holds, or null for the fixed tabs.
    const PrivateConversation* conversationAt(int tab) const;
    bool isCurrent(quint64 localId, quint64 remoteId) const;
    // Which of this PC's players last talked with remoteId, or 0.
    quint64 localFor(quint64 remoteId) const;
    ChatView* existingView(quint64 remoteId) const;
    QList<ChatView*> views() const;

    // How the remote player reads in the headers of their conversations.
    void setRemoteStatus(quint64 remoteId, const QString& state, const QString& statusText,
                         const QString& country, bool canChallenge);
    // A player left: it ends every conversation they're at either end of.
    void markGone(quint64 userId);

signals:
    // The selected conversation changed under the message box.
    void currentChanged();
    // A conversation's tab showed or hid.
    void tabsChanged();
    void userActivated(quint64 userId, const QPoint& globalPos);
    void challengeRequested(quint64 remoteId);
    void menuRequested(quint64 remoteId, const QPoint& globalPos);

private:
    class Face;
    class Page;
    struct Entry
    {
        PrivateConversation conversation;
        Page* page = nullptr;
        QPointer<Face> face; // the tab bar deletes it with its tab
    };

    static quint64 keyOf(quint64 localId, quint64 remoteId) { return (localId << 32) ^ remoteId; }
    Entry* entryAt(int tab);
    const Entry* entryAt(int tab) const;
    int tabOf(const Entry& entry) const;
    void refreshTab(const Entry& entry);
    void setUnread(Entry& entry, int unread);
    void onCurrentChanged(int tab);

    ChatTabBar* m_tabs;
    QStackedWidget* m_stack;
    QHash<quint64, Entry> m_entries;
    QHash<const QWidget*, quint64> m_keyOfPage;
    // The page shown before the current one, where closing the current tab goes back to.
    QPointer<QWidget> m_currentPage;
    QPointer<QWidget> m_previousPage;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // LOBBYPRIVATECHATS_HPP
