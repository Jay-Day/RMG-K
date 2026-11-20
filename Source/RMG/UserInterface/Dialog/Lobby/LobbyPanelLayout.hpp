#ifndef LOBBYPANELLAYOUT_HPP
#define LOBBYPANELLAYOUT_HPP

#ifdef NETPLAY

#include <QMargins>
#include <QPointer>
#include <QRect>
#include <QWidget>

#include <memory>

class QLayout;
class QSplitter;
class QVBoxLayout;

namespace UserInterface
{
namespace Dialog
{

class LobbyMenu;
class PanelDropOverlay;

// The lobby's panels, which the player arranges by dragging them by their
// headers. Two groups of them:
//
//   - The stage: the play bar above Live and Open rooms. A room opens as a
//     window over it, so the stage moves as one piece and its two lists only
//     move within it: beside each other or one above the other, in either order.
//   - Players and Chat go anywhere around the stage, beside each other, or
//     along a whole edge of the window.
//
// The arrangement is a small tree of splitters, rebuilt whenever a panel is
// dropped somewhere new, so every divider stays draggable. Today's arrangement
// (players over chat to the right of the stage) builds the same widgets the
// lobby always had and keeps using its splitter settings; only another
// arrangement is saved in a setting of its own, and one this build can't use
// falls back to today's.
class PanelLayout : public QWidget
{
    Q_OBJECT

public:
    enum Panel
    {
        Stage,
        Players,
        Chat,
        Live,
        Rooms,
        PanelCount
    };

    explicit PanelLayout(QWidget* parent = nullptr);
    ~PanelLayout() override;

    // The stage, and the layout that pads it: the padding is set to fit where
    // the stage sits, so the gaps between panels stay even.
    void setStage(QWidget* stage, QLayout* padding);
    // Live and Open rooms, the two children of browse.
    void setBrowse(QSplitter* browse, QWidget* live, QWidget* rooms);
    void setSidePanel(Panel panel, QWidget* card);
    // Builds the saved arrangement, or today's when there's none or it can't be used.
    void restore();
    void save();
    // Today's arrangement and sizes; Live and Open rooms keep their direction.
    void reset();

    // The account menu's "Side by side" / "Vertical": Live and Open rooms
    // beside each other or one above the other. Each way keeps its divider
    // and the order the lists were dropped in.
    bool browseStacked() const;
    void setBrowseStacked(bool stacked);

    // Where the stage's cards sit inside it, for the room window.
    QMargins stageMargins() const { return m_stageMargins; }

    // Adds the layout items (how to move a panel, Reset layout) to a menu.
    void addMenuItems(LobbyMenu& menu);

    // For tests and the overlay.
    bool dragging() const { return m_dragActive; }
    QString arrangementText() const;

signals:
    // A panel moved or the layout was reset; it has its new place by now.
    void arrangementChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

public:
    struct Node;
    enum class Zone { Left, Top, Right, Bottom, Center };
    struct Target
    {
        enum class Kind { None, Panel, Edge, Browse } kind = Kind::None;
        int panel = -1; // the panel dropped on (Panel, Browse)
        Zone zone = Zone::Center;
        QRect indicator;
        QRect outline;
        bool operator==(const Target& other) const
        {
            return kind == other.kind && panel == other.panel && zone == other.zone;
        }
    };

private:
    void rebuild();
    QWidget* build(Node* node, const QMargins& edges);
    void refreshVisibility();
    void captureSizes(Node* node) const;
    void applyLegacySizes();
    void finishChange();

    void restoreBrowseSplit();
    void applyBrowseOrder();

    // Which movable panel's header is at pos in widget, or -1. A press on a
    // control there (a button, a tab, a search box) is the control's own.
    int headerPanelAt(QWidget* widget, const QPoint& pos) const;
    QRect headerRect(QWidget* card) const;
    QWidget* panelWidget(int panel) const;
    QRect panelRect(int panel) const;  // the panel's card, in this widget's coordinates
    QRect panelBox(int panel) const;   // the card with the room around it that's its own
    QString panelTitle(int panel) const;

    void beginDrag(const QPoint& globalPos);
    void moveDrag(const QPoint& globalPos);
    void endDrag(const QPoint& globalPos, bool drop);
    Target targetAt(const QPoint& pos) const;
    Target browseTargetAt(const QPoint& pos) const;
    // The arrangement a drop on target would give, or nullptr when it's the one shown.
    std::unique_ptr<Node> arrangementFor(const Target& target) const;
    bool browseChangeFor(const Target& target, bool* stacked, bool* roomsFirst) const;
    QSize stageNeed() const;
    void setHover(QWidget* widget);
    void showHeaderMenu(const QPoint& globalPos);

    QVBoxLayout* m_layout = nullptr;
    QWidget* m_rootBox = nullptr;
    std::unique_ptr<Node> m_root;
    PanelDropOverlay* m_overlay = nullptr;

    QPointer<QWidget> m_stage;
    QLayout* m_stagePadding = nullptr;
    QMargins m_stageMargins;
    QPointer<QSplitter> m_browse;
    QPointer<QWidget> m_live;
    QPointer<QWidget> m_rooms;
    QPointer<QWidget> m_players;
    QPointer<QWidget> m_chat;
    // Open rooms first (left of or above Live), for side by side and stacked.
    bool m_roomsFirst[2] = { false, false };

    bool m_rebuilding = false;
    bool m_filtering = false;
    int m_pressPanel = -1;
    QPoint m_pressGlobal;
    bool m_dragActive = false;
    int m_dragPanel = -1;
    Target m_target;
    QPointer<QWidget> m_hover; // a header widget showing the open hand
    bool m_hoverSetCursor = false;
    bool m_saveQueued = false;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // LOBBYPANELLAYOUT_HPP
