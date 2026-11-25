#ifdef NETPLAY

#include "LobbyPanelLayout.hpp"
#include "LobbySocialWidgets.hpp"
#include "LobbyTheme.hpp"

#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>

#include <QFontMetrics>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

using namespace UserInterface::Dialog;
using LobbyTheme::Tokens;

// ── The tree ─────────────────────────────────────────────────────────

struct PanelLayout::Node
{
    int panel = -1; // Stage, Players or Chat; -1 for a divided area
    Qt::Orientation orientation = Qt::Horizontal;
    std::vector<std::unique_ptr<Node>> children;
    QList<int> sizes; // the splitter's, when known
    Node* parent = nullptr;
    // Built: what sits in the parent (the panel, its cell, the splitter or
    // the splitter's frame), and the splitter of a divided area.
    QWidget* box = nullptr;
    QSplitter* splitter = nullptr;
};

namespace
{
    using Node = PanelLayout::Node;
    using NodePtr = std::unique_ptr<Node>;
    using Zone = PanelLayout::Zone;

    // The lobby's spacing, as RollbackLobbyDialog lays it out.
    constexpr int kOuter        = 14; // inside the window's edge
    constexpr int kStageInner   = 8;  // the stage's side of a divider beside it
    constexpr int kStageHandle  = 6;  // a divider beside the stage: 14 px between cards with kStageInner
    constexpr int kSideGap      = 12; // a divider between two other panels
    constexpr int kSideShare    = 3;  // each side of such a gap, where the divider is kStageHandle wide
    constexpr int kSidebarWidth = 300;
    constexpr int kPlayersHeight = 256;
    constexpr int kChatHeight   = 400;
    constexpr int kSideMinWidth = 206; // the sidebar's 220 less the window's margin
    // A panel beside the stage or along the window: this wide, or this tall,
    // when the stage keeps room for the room window; never less than kLeast.
    constexpr int kPreferred[2] = { kSidebarWidth, kPlayersHeight };
    constexpr int kLeast[2] = { 220, 160 };
    // A panel split off another beside the stage: at least this wide, or tall.
    constexpr int kSplitOff[2] = { 260, 200 };
    constexpr int kEdgeBand = 22; // this close to the window's edge, a drop runs along all of it

    const char* const kLayoutSetting       = "RollbackLobby/PanelLayout";
    const char* const kMainSetting         = "RollbackLobby/MainSplitterState";
    const char* const kSidebarSetting      = "RollbackLobby/SidebarSplitterState";
    const char* const kBrowseLayoutSetting = "RollbackLobby/BrowseLayout";
    // Not "BrowseSplitterState": the classic lobby keeps its rooms / matches
    // splitter there, and both read the same settings.
    const char* const kSideBySideSetting   = "RollbackLobby/LiveRoomsSideBySide";
    const char* const kStackedSetting      = "RollbackLobby/LiveRoomsStacked";
    constexpr int kLayoutVersion = 1;

    const char* const kPanelNames[] = { "stage", "players", "chat", "live", "rooms" };
    const char* const kDefaultShape = "h(stage,v(players,chat))";
    // Whether Open rooms come first until the player moves them, side by side
    // and stacked: Live on the left, and Open rooms on top.
    constexpr bool kDefaultRoomsFirst[2] = { false, true };

    bool holds(const Node* node, int panel)
    {
        if (node->panel >= 0)
            return node->panel == panel;
        for (const auto& child : node->children)
        {
            if (holds(child.get(), panel))
                return true;
        }
        return false;
    }

    Node* findLeaf(Node* node, int panel)
    {
        if (node->panel >= 0)
            return node->panel == panel ? node : nullptr;
        for (const auto& child : node->children)
        {
            if (Node* found = findLeaf(child.get(), panel))
                return found;
        }
        return nullptr;
    }

    int indexIn(const Node* parent, const Node* child)
    {
        for (size_t i = 0; i < parent->children.size(); ++i)
        {
            if (parent->children[i].get() == child)
                return int(i);
        }
        return -1;
    }

    bool sized(const Node* node)
    {
        return node->sizes.size() == qsizetype(node->children.size());
    }

    int handleOf(const Node* split)
    {
        return holds(split, PanelLayout::Stage) ? kStageHandle : kSideGap;
    }

    void adopt(Node* parent)
    {
        for (const auto& child : parent->children)
            child->parent = parent;
    }

    NodePtr makeLeaf(int panel)
    {
        auto node = std::make_unique<Node>();
        node->panel = panel;
        return node;
    }

    NodePtr makeSplit(Qt::Orientation orientation, NodePtr a, NodePtr b, const QList<int>& sizes)
    {
        auto node = std::make_unique<Node>();
        node->orientation = orientation;
        node->children.push_back(std::move(a));
        node->children.push_back(std::move(b));
        node->sizes = sizes;
        adopt(node.get());
        return node;
    }

    // A copy that still knows where the original's widgets are, for their sizes.
    NodePtr clone(const Node* node)
    {
        auto copy = std::make_unique<Node>();
        copy->panel = node->panel;
        copy->orientation = node->orientation;
        copy->sizes = node->sizes;
        copy->box = node->box;
        for (const auto& child : node->children)
            copy->children.push_back(clone(child.get()));
        adopt(copy.get());
        return copy;
    }

    QString shape(const Node* node)
    {
        if (node->panel >= 0)
            return QString::fromLatin1(kPanelNames[node->panel]);
        QStringList parts;
        for (const auto& child : node->children)
            parts << shape(child.get());
        return QString(node->orientation == Qt::Horizontal ? QStringLiteral("h(%1)") : QStringLiteral("v(%1)"))
            .arg(parts.join(QLatin1Char(',')));
    }

    int axisOf(Qt::Orientation orientation)
    {
        return orientation == Qt::Horizontal ? 0 : 1;
    }

    int extent(const Node* node, Qt::Orientation orientation)
    {
        if (!node->box)
            return 0;
        return orientation == Qt::Horizontal ? node->box->width() : node->box->height();
    }

    bool across(Zone zone)
    {
        return zone == Zone::Left || zone == Zone::Right;
    }

    bool leading(Zone zone)
    {
        return zone == Zone::Left || zone == Zone::Top;
    }

    // What a panel gets of size pixels it shares with the stage: as much as a
    // sidebar, unless the stage would no longer hold the room window (stageNeed).
    int stageShare(int size, int axis, int handle, int stageNeed, int most)
    {
        return std::clamp(size - handle - stageNeed, kLeast[axis], std::max(kLeast[axis], most));
    }

    // Puts with where old is, and hands old back.
    NodePtr replaceNode(NodePtr& root, Node* old, NodePtr with)
    {
        Node* parent = old->parent;
        with->parent = parent;
        if (!parent)
        {
            NodePtr previous = std::move(root);
            root = std::move(with);
            return previous;
        }
        for (auto& child : parent->children)
        {
            if (child.get() == old)
            {
                NodePtr previous = std::move(child);
                child = std::move(with);
                return previous;
            }
        }
        return nullptr;
    }

    // A divided area inside one divided the same way becomes part of it.
    void flattenAt(Node* parent, int index)
    {
        Node* child = parent->children[size_t(index)].get();
        if (child->panel >= 0 || child->orientation != parent->orientation)
            return;
        NodePtr owned = std::move(parent->children[size_t(index)]);
        parent->children.erase(parent->children.begin() + index);
        const bool keepSizes = parent->sizes.size() == qsizetype(parent->children.size()) + 1 && sized(owned.get());
        if (keepSizes)
            parent->sizes.removeAt(index);
        else
            parent->sizes.clear();
        for (size_t k = 0; k < owned->children.size(); ++k)
        {
            parent->children.insert(parent->children.begin() + index + int(k), std::move(owned->children[k]));
            if (keepSizes)
                parent->sizes.insert(index + int(k), owned->sizes.at(qsizetype(k)));
        }
        adopt(parent);
    }

    void normalize(Node* node)
    {
        if (node->panel >= 0)
            return;
        for (int i = 0; i < int(node->children.size()); ++i)
        {
            normalize(node->children[size_t(i)].get());
            if (node->children[size_t(i)]->panel < 0 && node->children[size_t(i)]->orientation == node->orientation)
            {
                flattenAt(node, i);
                i = -1; // start over: the children moved
            }
        }
    }

    // Takes a panel out; the space it had goes to the stage's side, or a neighbor.
    NodePtr takeLeaf(NodePtr& root, int panel)
    {
        Node* leaf = findLeaf(root.get(), panel);
        if (!leaf || !leaf->parent)
            return nullptr;
        Node* parent = leaf->parent;
        const int index = indexIn(parent, leaf);
        const bool hadSizes = sized(parent);
        NodePtr taken = std::move(parent->children[size_t(index)]);
        parent->children.erase(parent->children.begin() + index);
        taken->parent = nullptr;
        if (hadSizes)
        {
            const int freed = parent->sizes.takeAt(index) + handleOf(parent);
            int to = -1;
            for (int k = 0; k < int(parent->children.size()); ++k)
            {
                if (holds(parent->children[size_t(k)].get(), PanelLayout::Stage))
                    to = k;
            }
            if (to < 0)
                to = std::min(index, int(parent->children.size()) - 1);
            parent->sizes[to] += freed;
        }
        if (parent->children.size() == 1)
        {
            NodePtr only = std::move(parent->children.front());
            parent->children.clear();
            Node* grandparent = parent->parent;
            Node* onlyNode = only.get();
            replaceNode(root, parent, std::move(only));
            if (grandparent)
                flattenAt(grandparent, indexIn(grandparent, onlyNode));
        }
        return taken;
    }

    // Puts moving beside the target panel, on the zone's side of it. need is
    // the room the stage keeps where it can: the room window, and its padding.
    void insertBeside(NodePtr& root, int targetPanel, NodePtr moving, Zone zone, const QSize& need)
    {
        Node* target = findLeaf(root.get(), targetPanel);
        if (!target)
            return;
        const Qt::Orientation orientation = across(zone) ? Qt::Horizontal : Qt::Vertical;
        const int axis = axisOf(orientation);
        const int stageNeed = axis == 0 ? need.width() : need.height();
        const bool before = leading(zone);
        const int size = extent(target, orientation);
        Node* parent = target->parent;

        if (parent && parent->orientation == orientation)
        {
            const int index = indexIn(parent, target);
            const int at = before ? index : index + 1;
            if (sized(parent) && size > 0)
            {
                const int handle = handleOf(parent);
                int share = 0;
                int rest = 0;
                if (targetPanel == PanelLayout::Stage)
                {
                    share = stageShare(size, axis, handle, stageNeed, std::min(kPreferred[axis], size / 2));
                    rest = size - share - handle;
                }
                else
                {
                    // Room for both, out of the stage's part when it takes
                    // more, as long as the stage keeps what the room window needs.
                    share = std::max((size - handle) / 2, kSplitOff[axis]);
                    rest = std::max(size - share - handle, kSplitOff[axis]);
                    const int deficit = share + rest + handle - size;
                    for (int k = 0; deficit > 0 && k < int(parent->children.size()); ++k)
                    {
                        if (holds(parent->children[size_t(k)].get(), PanelLayout::Stage))
                        {
                            const int stage = parent->sizes[k];
                            const int spare = std::max(0, stage - stageNeed);
                            parent->sizes[k] = stage - std::min(deficit, spare);
                        }
                    }
                }
                parent->sizes[index] = std::max(0, rest);
                parent->sizes.insert(at, share);
            }
            else
            {
                parent->sizes.clear();
            }
            moving->parent = parent;
            parent->children.insert(parent->children.begin() + at, std::move(moving));
            return;
        }

        // A new divider between the two, where the target was.
        auto split = std::make_unique<Node>();
        split->orientation = orientation;
        Node* splitNode = split.get();
        NodePtr targetNode = replaceNode(root, target, std::move(split));
        if (size > 0)
        {
            const int handle = targetPanel == PanelLayout::Stage ? kStageHandle : kSideGap;
            const int share = targetPanel == PanelLayout::Stage
                ? stageShare(size, axis, handle, stageNeed, std::min(kPreferred[axis], size / 2))
                : (size - handle) / 2;
            const int rest = std::max(0, size - share - handle);
            splitNode->sizes = before ? QList<int>{ share, rest } : QList<int>{ rest, share };
        }
        if (before)
        {
            splitNode->children.push_back(std::move(moving));
            splitNode->children.push_back(std::move(targetNode));
        }
        else
        {
            splitNode->children.push_back(std::move(targetNode));
            splitNode->children.push_back(std::move(moving));
        }
        adopt(splitNode);
    }

    // Puts moving along a whole edge of the window.
    void insertAtEdge(NodePtr& root, NodePtr moving, Zone zone, const QSize& host, const QSize& need)
    {
        const Qt::Orientation orientation = across(zone) ? Qt::Horizontal : Qt::Vertical;
        const int axis = axisOf(orientation);
        const int stageNeed = axis == 0 ? need.width() : need.height();
        const bool before = leading(zone);
        const int total = axis == 0 ? host.width() : host.height();
        const int most = std::min(kPreferred[axis], total * 2 / 5);

        if (root->panel < 0 && root->orientation == orientation)
        {
            if (sized(root.get()))
            {
                int share = kLeast[axis];
                for (int k = 0; k < int(root->children.size()); ++k)
                {
                    if (holds(root->children[size_t(k)].get(), PanelLayout::Stage))
                    {
                        share = stageShare(root->sizes[k], axis, kStageHandle, stageNeed, most);
                        root->sizes[k] = std::max(0, root->sizes[k] - share - kStageHandle);
                    }
                }
                root->sizes.insert(before ? 0 : root->sizes.size(), share);
            }
            moving->parent = root.get();
            root->children.insert(before ? root->children.begin() : root->children.end(), std::move(moving));
            return;
        }

        const int share = stageShare(total, axis, kStageHandle, stageNeed, most);
        const int rest = std::max(0, total - share - kStageHandle);
        NodePtr old = std::move(root);
        root = before ? makeSplit(orientation, std::move(moving), std::move(old), { share, rest })
                      : makeSplit(orientation, std::move(old), std::move(moving), { rest, share });
    }

    // How much of its divider a panel has in an arrangement, or 0 when unknown.
    int sizeAfter(Node* root, int panel)
    {
        const Node* leaf = findLeaf(root, panel);
        if (!leaf || !leaf->parent || !sized(leaf->parent))
            return 0;
        return leaf->parent->sizes.at(indexIn(leaf->parent, leaf));
    }

    void swapLeaves(Node* root, int a, int b)
    {
        Node* first = findLeaf(root, a);
        Node* second = findLeaf(root, b);
        if (first && second)
            std::swap(first->panel, second->panel);
    }

    // ── Saving ──

    QJsonValue toJson(const Node* node)
    {
        if (node->panel >= 0)
            return QString::fromLatin1(kPanelNames[node->panel]);
        QJsonObject object;
        object[QStringLiteral("split")] = node->orientation == Qt::Horizontal ? QStringLiteral("h") : QStringLiteral("v");
        QJsonArray children;
        for (const auto& child : node->children)
            children.append(toJson(child.get()));
        object[QStringLiteral("children")] = children;
        if (sized(node))
        {
            QJsonArray sizes;
            for (int size : node->sizes)
                sizes.append(size);
            object[QStringLiteral("sizes")] = sizes;
        }
        return object;
    }

    // nullptr for anything this build didn't write: an unknown or repeated
    // panel, a split of one, a nonsense size.
    NodePtr fromJson(const QJsonValue& value, int depth, QSet<int>& seen)
    {
        if (depth > 4)
            return nullptr;
        if (value.isString())
        {
            for (int panel : { PanelLayout::Stage, PanelLayout::Players, PanelLayout::Chat })
            {
                if (value.toString() == QLatin1String(kPanelNames[panel]))
                {
                    if (seen.contains(panel))
                        return nullptr;
                    seen.insert(panel);
                    return makeLeaf(panel);
                }
            }
            return nullptr;
        }
        if (!value.isObject())
            return nullptr;
        const QJsonObject object = value.toObject();
        const QString split = object.value(QStringLiteral("split")).toString();
        if (split != QLatin1String("h") && split != QLatin1String("v"))
            return nullptr;
        const QJsonArray children = object.value(QStringLiteral("children")).toArray();
        if (children.size() < 2 || children.size() > 3)
            return nullptr;
        auto node = std::make_unique<Node>();
        node->orientation = split == QLatin1String("h") ? Qt::Horizontal : Qt::Vertical;
        for (const QJsonValue& child : children)
        {
            NodePtr built = fromJson(child, depth + 1, seen);
            if (!built)
                return nullptr;
            node->children.push_back(std::move(built));
        }
        adopt(node.get());
        const QJsonArray sizes = object.value(QStringLiteral("sizes")).toArray();
        if (sizes.size() == children.size())
        {
            QList<int> list;
            for (const QJsonValue& size : sizes)
            {
                const double pixels = size.toDouble(-1);
                if (!size.isDouble() || pixels < 0 || pixels > 100000)
                {
                    list.clear();
                    break;
                }
                list.append(int(pixels));
            }
            node->sizes = list;
        }
        return node;
    }

    NodePtr parseLayout(const QString& text, bool* roomsFirst)
    {
        if (text.isEmpty())
            return nullptr;
        QJsonParseError error{};
        const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            return nullptr;
        const QJsonObject object = document.object();
        if (object.value(QStringLiteral("version")).toInt(-1) != kLayoutVersion)
            return nullptr;
        QSet<int> seen;
        NodePtr tree = fromJson(object.value(QStringLiteral("tree")), 0, seen);
        if (!tree || tree->panel >= 0 || seen.size() != 3)
            return nullptr;
        normalize(tree.get());
        const QJsonObject first = object.value(QStringLiteral("roomsFirst")).toObject();
        roomsFirst[0] = first.value(QStringLiteral("side")).toBool(kDefaultRoomsFirst[0]);
        roomsFirst[1] = first.value(QStringLiteral("stacked")).toBool(kDefaultRoomsFirst[1]);
        return tree;
    }

    // Players over chat, to the right of the stage: the lobby as it always was.
    NodePtr defaultTree(int width)
    {
        NodePtr side = makeSplit(Qt::Vertical, makeLeaf(PanelLayout::Players), makeLeaf(PanelLayout::Chat),
                                 { kPlayersHeight, kChatHeight });
        return makeSplit(Qt::Horizontal, makeLeaf(PanelLayout::Stage), std::move(side),
                         { width - kSidebarWidth - kStageHandle, kSidebarWidth });
    }

    // A splitter state the lobby saved before panels could move. One whose
    // direction doesn't match is someone else's: the defaults stay.
    void restoreSplitterState(QSplitter* splitter, const QByteArray& state, const QList<int>& defaults)
    {
        if (!splitter || state.isEmpty())
            return;
        const Qt::Orientation orientation = splitter->orientation();
        const int handleWidth = splitter->handleWidth();
        if (!splitter->restoreState(state) || splitter->orientation() != orientation)
        {
            splitter->setOrientation(orientation);
            splitter->setHandleWidth(handleWidth);
            splitter->setChildrenCollapsible(false);
            splitter->setSizes(defaults);
        }
    }

    // Where the pointer is in rect: nearest an edge, or in the middle.
    Zone zoneIn(const QRect& rect, const QPoint& pos, bool middle)
    {
        const double x = (pos.x() - rect.left()) / double(std::max(1, rect.width()));
        const double y = (pos.y() - rect.top()) / double(std::max(1, rect.height()));
        if (middle && std::abs(x - 0.5) < 0.2 && std::abs(y - 0.5) < 0.2)
            return Zone::Center;
        const double left = x, right = 1 - x, top = y, bottom = 1 - y;
        const double nearest = std::min({ left, right, top, bottom });
        return nearest == left ? Zone::Left : nearest == right ? Zone::Right : nearest == top ? Zone::Top : Zone::Bottom;
    }

    // The zone's side of rect, share pixels of it (half when share is 0).
    QRect sideOf(const QRect& rect, Zone zone, int share = 0)
    {
        const int width = share > 0 ? std::min(share, rect.width()) : (rect.width() - kSideGap) / 2;
        const int height = share > 0 ? std::min(share, rect.height()) : (rect.height() - kSideGap) / 2;
        switch (zone)
        {
        case Zone::Left:
            return QRect(rect.left(), rect.top(), width, rect.height());
        case Zone::Right:
            return QRect(rect.right() + 1 - width, rect.top(), width, rect.height());
        case Zone::Top:
            return QRect(rect.left(), rect.top(), rect.width(), height);
        case Zone::Bottom:
            return QRect(rect.left(), rect.bottom() + 1 - height, rect.width(), height);
        case Zone::Center:
            break;
        }
        return rect;
    }

    // A press on one of these is the control's. Anything else in a header that
    // takes clicks can say so with the "lobbyNoPanelDrag" property.
    bool isControl(QWidget* widget, const QPoint& pos)
    {
        if (qobject_cast<QAbstractButton*>(widget) || qobject_cast<QComboBox*>(widget) ||
            qobject_cast<QLineEdit*>(widget) || qobject_cast<QAbstractSpinBox*>(widget) ||
            qobject_cast<QAbstractSlider*>(widget) || qobject_cast<QAbstractScrollArea*>(widget) ||
            widget->property("lobbyNoPanelDrag").toBool())
            return true;
        if (auto* tabs = qobject_cast<QTabBar*>(widget))
            return tabs->tabAt(pos) >= 0;
        return false;
    }
} // namespace

// ── The drop indicator ───────────────────────────────────────────────

namespace UserInterface
{
namespace Dialog
{

// Drawn over the panels while one is dragged: the panel fading where it is,
// where it would land, and its name at the pointer. It takes no clicks.
class PanelDropOverlay : public QWidget
{
public:
    struct Scene
    {
        QRect source;        // the panel being moved
        QString sourceLabel; // for a swap: the panel that would go there instead
        QRect indicator;     // where it would land
        QRect outline;       // the panel it would split or swap with
        QString label;       // the panel's name
        QPoint cursor;
        bool allowed = false;
    };

    explicit PanelDropOverlay(QWidget* parent)
        : QWidget(parent)
    {
        setObjectName("PanelDropOverlay");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
    }

    void setScene(const Scene& scene)
    {
        m_scene = scene;
        update();
    }

    const Scene& scene() const { return m_scene; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const Tokens& t = LobbyTheme::tokens();
        const auto rounded = [](const QRectF& rect, qreal radius) {
            QPainterPath path;
            path.addRoundedRect(rect, radius, radius);
            return path;
        };

        if (!m_scene.source.isEmpty())
        {
            const QRectF rect = QRectF(m_scene.source).adjusted(0.5, 0.5, -0.5, -0.5);
            QColor wash = t.bg;
            wash.setAlpha(t.dark ? 160 : 140);
            painter.fillPath(rounded(rect, 12), wash);
            painter.setPen(QPen(t.line2, 1.5, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(rounded(rect.adjusted(1, 1, -1, -1), 11));
            if (!m_scene.sourceLabel.isEmpty())
                drawTag(painter, rect.center(), m_scene.sourceLabel);
        }

        if (!m_scene.outline.isEmpty() && m_scene.outline != m_scene.indicator)
        {
            QColor ring = t.accent;
            ring.setAlpha(120);
            painter.setPen(QPen(ring, 1.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(rounded(QRectF(m_scene.outline).adjusted(0.5, 0.5, -0.5, -0.5), 12));
        }

        if (!m_scene.indicator.isEmpty())
        {
            const QRectF rect = QRectF(m_scene.indicator).adjusted(1, 1, -1, -1);
            QColor fill = t.accent;
            fill.setAlphaF(t.dark ? 0.24 : 0.17);
            painter.fillPath(rounded(rect, 11), fill);
            painter.setPen(QPen(t.accent, 2.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(rounded(rect, 11));
        }

        // The panel's name at the pointer, off to the side so the drop shows.
        if (!m_scene.label.isEmpty())
        {
            const QSize size = tagSize(m_scene.label);
            QRect chip(m_scene.cursor + QPoint(16, 18), size);
            if (chip.right() > width() - 4)
                chip.moveRight(m_scene.cursor.x() - 12);
            if (chip.bottom() > height() - 4)
                chip.moveBottom(m_scene.cursor.y() - 12);
            painter.setPen(Qt::NoPen);
            for (int i = 6; i >= 1; --i)
            {
                const qreal strength = 1.0 - qreal(i) / 7;
                painter.setBrush(QColor(0, 0, 0, int((t.dark ? 60 : 28) * strength * strength)));
                painter.drawPath(rounded(QRectF(chip).adjusted(-i, -i + 2, i, i + 2), 8 + i));
            }
            drawChip(painter, chip, m_scene.label, m_scene.allowed ? t.ink : t.muted, m_scene.allowed ? t.line2 : t.line,
                     t.raise, m_scene.allowed ? t.muted : t.faint);
        }
    }

private:
    QFont tagFont() const { return LobbyTheme::pixelFont(11, 1.0); }

    QSize tagSize(const QString& text) const
    {
        const int textWidth = QFontMetrics(tagFont()).horizontalAdvance(text.toUpper());
        return QSize(10 + 14 + 6 + textWidth + 12, 28);
    }

    // A name in the middle of a panel: the one that would take its place.
    void drawTag(QPainter& painter, const QPointF& center, const QString& text)
    {
        if (text.isEmpty())
            return;
        const Tokens& t = LobbyTheme::tokens();
        QRect chip(QPoint(), tagSize(text));
        chip.moveCenter(center.toPoint());
        drawChip(painter, chip, text, t.ink2, t.line2, t.panel2, t.muted);
    }

    void drawChip(QPainter& painter, const QRect& chip, const QString& text, const QColor& ink, const QColor& border,
                  const QColor& fill, const QColor& iconColor)
    {
        QPainterPath path;
        path.addRoundedRect(QRectF(chip).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
        painter.fillPath(path, fill);
        painter.setPen(QPen(border, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
        painter.drawPixmap(chip.left() + 10, chip.center().y() - 7,
                           LobbyTheme::icon(QStringLiteral("draggable"), iconColor, 14, devicePixelRatioF()));
        painter.setFont(tagFont());
        painter.setPen(ink);
        painter.drawText(QRectF(chip.adjusted(30, 0, -8, 0)).translated(0, LobbyTheme::pixelTextDrop(tagFont())),
                         Qt::AlignLeft | Qt::AlignVCenter, text.toUpper());
    }

    Scene m_scene;
};

} // namespace Dialog
} // namespace UserInterface

// ── PanelLayout ──────────────────────────────────────────────────────

PanelLayout::PanelLayout(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("LobbyPanels");
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
    m_overlay = new PanelDropOverlay(this);
}

PanelLayout::~PanelLayout()
{
    if (m_dragActive)
        QApplication::restoreOverrideCursor();
    if (m_filtering)
        qApp->removeEventFilter(this);
}

void PanelLayout::setStage(QWidget* stage, QLayout* padding)
{
    m_stage = stage;
    m_stagePadding = padding;
    if (padding)
        m_stageMargins = padding->contentsMargins();
}

void PanelLayout::setBrowse(QSplitter* browse, QWidget* live, QWidget* rooms)
{
    m_browse = browse;
    m_live = live;
    m_rooms = rooms;
}

void PanelLayout::setSidePanel(Panel panel, QWidget* card)
{
    if (panel == Players)
        m_players = card;
    else if (panel == Chat)
        m_chat = card;
    else
        return;
    // Beside another panel, never narrower than the sidebar could get.
    card->setMinimumWidth(std::max(card->minimumWidth(), kSideMinWidth));
}

QWidget* PanelLayout::panelWidget(int panel) const
{
    switch (panel)
    {
    case Stage:
        return m_stage;
    case Players:
        return m_players;
    case Chat:
        return m_chat;
    case Live:
        return m_live;
    case Rooms:
        return m_rooms;
    default:
        return nullptr;
    }
}

QString PanelLayout::panelTitle(int panel) const
{
    switch (panel)
    {
    case Players:
        return tr("Players");
    case Chat:
        return tr("Chat");
    case Live:
        return tr("Live now");
    case Rooms:
        return tr("Open rooms");
    default:
        return QString();
    }
}

// ── Building ─────────────────────────────────────────────────────────

void PanelLayout::restore()
{
    QSettings settings("RMG-K", "n02");
    bool roomsFirst[2] = { kDefaultRoomsFirst[0], kDefaultRoomsFirst[1] };
    NodePtr tree = parseLayout(settings.value(kLayoutSetting).toString(), roomsFirst);
    const bool saved = tree != nullptr;
    if (!saved)
    {
        tree = defaultTree(window()->width());
        roomsFirst[0] = kDefaultRoomsFirst[0];
        roomsFirst[1] = kDefaultRoomsFirst[1];
    }
    m_root = std::move(tree);
    m_roomsFirst[0] = roomsFirst[0];
    m_roomsFirst[1] = roomsFirst[1];
    rebuild();
    if (!saved)
        applyLegacySizes();
    restoreBrowseSplit();
}

// Today's arrangement keeps the splitter settings it always had, so a lobby
// that never moved a panel opens exactly as before.
void PanelLayout::applyLegacySizes()
{
    if (!m_root || shape(m_root.get()) != QLatin1String(kDefaultShape))
        return;
    QSettings settings("RMG-K", "n02");
    Node* side = m_root->children[1].get();
    restoreSplitterState(m_root->splitter, settings.value(kMainSetting).toByteArray(), m_root->sizes);
    restoreSplitterState(side->splitter, settings.value(kSidebarSetting).toByteArray(), side->sizes);
}

void PanelLayout::rebuild()
{
    if (!m_root || !m_stage || !m_players || !m_chat)
        return;

    // Reparenting a widget drops its focus: put it back afterwards, so a
    // half-typed chat line keeps its caret. The window's own focus child, as
    // the lobby needn't be the active window.
    QPointer<QWidget> focus = window()->focusWidget();
    if (focus && !isAncestorOf(focus))
        focus = nullptr;

    // Out of the old dividers before they go, keeping whether each is hidden.
    // Until the new ones are up, the tree points at widgets on their way out.
    m_rebuilding = true;
    QWidget* panels[] = { m_stage, m_players, m_chat };
    bool hidden[3] = {};
    for (int i = 0; i < 3; ++i)
    {
        hidden[i] = panels[i]->isHidden() && panels[i]->testAttribute(Qt::WA_WState_ExplicitShowHide);
        panels[i]->setParent(this);
    }
    if (m_rootBox)
    {
        m_layout->removeWidget(m_rootBox);
        m_rootBox->hide();
        m_rootBox->deleteLater();
        m_rootBox = nullptr;
    }

    m_rootBox = build(m_root.get(), QMargins(kOuter, kOuter, kOuter, kOuter));
    m_layout->addWidget(m_rootBox);
    for (int i = 0; i < 3; ++i)
        panels[i]->setVisible(!hidden[i]);
    m_rootBox->show();
    m_rebuilding = false;
    refreshVisibility();
    m_overlay->raise();

    if (focus)
        focus->setFocus(Qt::OtherFocusReason);
}

// edges: the space to leave on each side, the window's margin on an outer edge
// or this panel's part of the gap at a divider.
QWidget* PanelLayout::build(Node* node, const QMargins& edges)
{
    node->splitter = nullptr;
    if (node->panel == Stage)
    {
        m_stageMargins = edges;
        if (m_stagePadding)
            m_stagePadding->setContentsMargins(edges);
        node->box = m_stage;
        return m_stage;
    }
    if (node->panel >= 0)
    {
        QWidget* card = panelWidget(node->panel);
        if (edges.isNull())
        {
            node->box = card;
            return card;
        }
        auto* cell = new QWidget(this);
        cell->setObjectName("PanelCell");
        auto* cellLayout = new QVBoxLayout(cell);
        cellLayout->setContentsMargins(edges);
        cellLayout->setSpacing(0);
        cellLayout->addWidget(card);
        node->box = cell;
        return cell;
    }

    // Beside the stage a divider is narrow, the stage keeps its side of the
    // gap and grows with the window. Elsewhere it's the whole gap, and chat
    // takes what's left.
    const bool stageHere = holds(node, Stage);
    auto* splitter = new QSplitter(node->orientation, this);
    splitter->setObjectName("PanelSplitter");
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(stageHere ? kStageHandle : kSideGap);
    node->splitter = splitter;
    const bool horizontal = node->orientation == Qt::Horizontal;
    const int count = int(node->children.size());
    for (int i = 0; i < count; ++i)
    {
        Node* child = node->children[size_t(i)].get();
        QMargins childEdges = stageHere ? edges : QMargins();
        if (stageHere)
        {
            const auto gapShare = [&](int a, int b, bool firstSide) {
                const bool stageA = holds(node->children[size_t(a)].get(), Stage);
                const bool stageB = holds(node->children[size_t(b)].get(), Stage);
                if (stageA)
                    return firstSide ? kStageInner : 0;
                if (stageB)
                    return firstSide ? 0 : kStageInner;
                return kSideShare;
            };
            if (i > 0)
            {
                const int share = gapShare(i - 1, i, false);
                horizontal ? childEdges.setLeft(share) : childEdges.setTop(share);
            }
            if (i + 1 < count)
            {
                const int share = gapShare(i, i + 1, true);
                horizontal ? childEdges.setRight(share) : childEdges.setBottom(share);
            }
        }
        splitter->addWidget(build(child, childEdges));
        const bool grows = stageHere ? holds(child, Stage) : holds(child, Chat);
        splitter->setStretchFactor(i, grows ? 1 : 0);
    }
    if (sized(node))
        splitter->setSizes(node->sizes);

    if (!stageHere && !edges.isNull())
    {
        auto* frame = new QWidget(this);
        frame->setObjectName("PanelCell");
        auto* frameLayout = new QVBoxLayout(frame);
        frameLayout->setContentsMargins(edges);
        frameLayout->setSpacing(0);
        frameLayout->addWidget(splitter);
        node->box = frame;
        return frame;
    }
    node->box = splitter;
    return splitter;
}

// A divided area whose panels are all hidden takes no room.
void PanelLayout::refreshVisibility()
{
    const std::function<bool(Node*)> visit = [&](Node* node) -> bool {
        bool shown = false;
        if (node->panel >= 0)
        {
            QWidget* panel = panelWidget(node->panel);
            shown = panel && !(panel->isHidden() && panel->testAttribute(Qt::WA_WState_ExplicitShowHide));
        }
        else
        {
            for (const auto& child : node->children)
                shown = visit(child.get()) || shown;
        }
        if (node->box && node->box != panelWidget(node->panel) && node->box->isHidden() == shown)
            node->box->setVisible(shown);
        return shown;
    };
    if (m_root && !m_rebuilding)
        visit(m_root.get());
}

void PanelLayout::captureSizes(Node* node) const
{
    if (!node || node->panel >= 0)
        return;
    // A splitter that's never been laid out, or holds a hidden panel, reports
    // nothing worth keeping: the last sizes stay.
    if (node->splitter && node->splitter->count() == int(node->children.size()))
    {
        const QList<int> sizes = node->splitter->sizes();
        if (std::all_of(sizes.begin(), sizes.end(), [](int size) { return size > 0; }))
            node->sizes = sizes;
    }
    for (const auto& child : node->children)
        captureSizes(child.get());
}

void PanelLayout::save()
{
    if (!m_root)
        return;
    captureSizes(m_root.get());
    QSettings settings("RMG-K", "n02");
    const bool defaultShape = shape(m_root.get()) == QLatin1String(kDefaultShape);
    if (defaultShape)
    {
        // What the lobby saved before panels could move, so older builds and
        // this one agree about it.
        if (m_root->splitter)
            settings.setValue(kMainSetting, m_root->splitter->saveState());
        if (QSplitter* side = m_root->children[1]->splitter)
            settings.setValue(kSidebarSetting, side->saveState());
    }
    if (defaultShape && m_roomsFirst[0] == kDefaultRoomsFirst[0] && m_roomsFirst[1] == kDefaultRoomsFirst[1])
    {
        settings.remove(kLayoutSetting);
    }
    else
    {
        QJsonObject object;
        object[QStringLiteral("version")] = kLayoutVersion;
        object[QStringLiteral("tree")] = toJson(m_root.get());
        QJsonObject first;
        first[QStringLiteral("side")] = m_roomsFirst[0];
        first[QStringLiteral("stacked")] = m_roomsFirst[1];
        object[QStringLiteral("roomsFirst")] = first;
        settings.setValue(kLayoutSetting, QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
    }
    if (m_browse)
        settings.setValue(m_browse->orientation() == Qt::Vertical ? kStackedSetting : kSideBySideSetting,
                          m_browse->saveState());
}

void PanelLayout::reset()
{
    if (m_dragActive)
        endDrag(QPoint(), false);
    QSettings settings("RMG-K", "n02");
    for (const char* key : { kLayoutSetting, kMainSetting, kSidebarSetting, kSideBySideSetting, kStackedSetting })
        settings.remove(key);
    m_roomsFirst[0] = kDefaultRoomsFirst[0];
    m_roomsFirst[1] = kDefaultRoomsFirst[1];
    m_root = defaultTree(width() > 0 && isVisible() ? width() : window()->width());
    rebuild();
    restoreBrowseSplit();
    finishChange();
    emit layoutReset();
}

// Once the new arrangement is laid out: keep it, and let the room window refit.
void PanelLayout::finishChange()
{
    if (m_saveQueued)
        return;
    m_saveQueued = true;
    QTimer::singleShot(0, this, [this]() {
        m_saveQueued = false;
        if (QLayout* top = window()->layout())
            top->activate();
        save();
        emit arrangementChanged();
    });
}

QString PanelLayout::arrangementText() const
{
    if (!m_root)
        return QString();
    const bool stacked = browseStacked();
    return QStringLiteral("%1 %2 %3").arg(shape(m_root.get()), stacked ? QStringLiteral("stacked") : QStringLiteral("side"),
                                          m_roomsFirst[stacked ? 1 : 0] ? QStringLiteral("rooms-first") : QStringLiteral("live-first"));
}

// ── Live and Open rooms ──────────────────────────────────────────────

bool PanelLayout::browseStacked() const
{
    return m_browse && m_browse->orientation() == Qt::Vertical;
}

void PanelLayout::applyBrowseOrder()
{
    if (!m_browse || !m_live || !m_rooms)
        return;
    QWidget* first = m_roomsFirst[browseStacked() ? 1 : 0] ? m_rooms.data() : m_live.data();
    if (m_browse->indexOf(first) != 0)
        m_browse->insertWidget(0, first);
}

// The divider where the player left it in this direction. A saved state
// carries its own orientation, and the direction wins: one that disagrees is
// dropped.
void PanelLayout::restoreBrowseSplit()
{
    if (!m_browse)
        return;
    applyBrowseOrder();
    const Qt::Orientation orientation = m_browse->orientation();
    const int handleWidth = m_browse->handleWidth();
    const QByteArray state = QSettings("RMG-K", "n02")
        .value(orientation == Qt::Vertical ? kStackedSetting : kSideBySideSetting).toByteArray();
    if (state.isEmpty() || !m_browse->restoreState(state) || m_browse->orientation() != orientation)
    {
        // A rejected state has already set its own handle width and collapsing.
        m_browse->setOrientation(orientation);
        m_browse->setHandleWidth(handleWidth);
        m_browse->setChildrenCollapsible(false);
        const bool liveFirst = m_browse->indexOf(m_live) == 0;
        if (orientation == Qt::Horizontal)
            m_browse->setSizes(liveFirst ? QList<int>{ 340, 500 } : QList<int>{ 500, 340 });
        else
            m_browse->setSizes({ 1, 1 });
    }
}

// Live beside the open rooms, or one above the other. Each way keeps its own
// divider.
void PanelLayout::setBrowseStacked(bool stacked)
{
    const Qt::Orientation orientation = stacked ? Qt::Vertical : Qt::Horizontal;
    if (!m_browse || m_browse->orientation() == orientation)
        return;
    QSettings settings("RMG-K", "n02");
    settings.setValue(browseStacked() ? kStackedSetting : kSideBySideSetting, m_browse->saveState());
    m_browse->setOrientation(orientation);
    restoreBrowseSplit();
    settings.setValue(kBrowseLayoutSetting, stacked ? QStringLiteral("stacked") : QStringLiteral("side"));
}

// ── Menus ────────────────────────────────────────────────────────────

void PanelLayout::addMenuItems(LobbyMenu& menu)
{
    menu.addDisabledItem(QStringLiteral("draggable"), tr("Move a panel"), tr("Drag its header"));
    QAction* reset = menu.addItem(QStringLiteral("arrow-go-back-line"), tr("Reset layout"));
    reset->setObjectName("ResetLayoutAction");
    // Queued: the panels are rebuilt once the menu has closed.
    connect(reset, &QAction::triggered, this, &PanelLayout::reset, Qt::QueuedConnection);
}

void PanelLayout::showHeaderMenu(const QPoint& globalPos)
{
    LobbyMenu menu(this);
    addMenuItems(menu);
    menu.exec(globalPos);
}

// ── Dragging ─────────────────────────────────────────────────────────

QRect PanelLayout::headerRect(QWidget* card) const
{
    // A panel's header is the first thing in its layout: the title row, or
    // the chat's tabs.
    QLayout* layout = card->layout();
    if (!layout || layout->count() == 0)
        return QRect();
    return QRect(0, 0, card->width(), layout->itemAt(0)->geometry().bottom() + 1);
}

int PanelLayout::headerPanelAt(QWidget* widget, const QPoint& pos) const
{
    for (QWidget* ancestor = widget; ancestor && ancestor != this && !ancestor->isWindow();
         ancestor = ancestor->parentWidget())
    {
        int panel = -1;
        for (int candidate : { Players, Chat, Live, Rooms })
        {
            if (ancestor == panelWidget(candidate))
                panel = candidate;
        }
        if (panel < 0)
            continue;
        if ((panel == Live || panel == Rooms) && !m_browse)
            return -1;
        if (!ancestor->isEnabled() || !headerRect(ancestor).contains(widget->mapTo(ancestor, pos)))
            return -1;
        for (QWidget* inner = widget; inner != ancestor; inner = inner->parentWidget())
        {
            if (isControl(inner, widget->mapTo(inner, pos)))
                return -1;
        }
        return panel;
    }
    return -1;
}

QRect PanelLayout::panelRect(int panel) const
{
    QWidget* widget = panelWidget(panel);
    if (!widget || !widget->isVisible() || !isAncestorOf(widget))
        return QRect();
    const QRect rect(widget->mapTo(this, QPoint(0, 0)), widget->size());
    return panel == Stage ? rect.marginsRemoved(m_stageMargins) : rect;
}

QRect PanelLayout::panelBox(int panel) const
{
    if (!m_root)
        return QRect();
    const Node* leaf = findLeaf(m_root.get(), panel);
    if (!leaf || !leaf->box || !leaf->box->isVisible())
        return QRect();
    return QRect(leaf->box->mapTo(this, QPoint(0, 0)), leaf->box->size());
}

bool PanelLayout::eventFilter(QObject* watched, QEvent* event)
{
    const QEvent::Type type = event->type();
    switch (type)
    {
    case QEvent::ShowToParent:
    case QEvent::HideToParent:
        if (watched == m_players || watched == m_chat || watched == m_stage)
            refreshVisibility();
        return false;
    case QEvent::ApplicationDeactivate:
        m_pressPanel = -1;
        if (m_dragActive)
            endDrag(QPoint(), false);
        return false;
    case QEvent::KeyPress:
        if (m_dragActive && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape)
        {
            endDrag(QPoint(), false);
            return true;
        }
        return false;
    case QEvent::ContextMenu:
    {
        if (m_dragActive || !watched->isWidgetType())
            return false;
        auto* widget = static_cast<QWidget*>(watched);
        auto* menuEvent = static_cast<QContextMenuEvent*>(event);
        if (menuEvent->reason() != QContextMenuEvent::Mouse || widget->window() != window() ||
            headerPanelAt(widget, menuEvent->pos()) < 0)
            return false;
        showHeaderMenu(menuEvent->globalPos());
        return true;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseMove:
        break;
    default:
        return false;
    }

    if (!watched->isWidgetType())
        return false;
    auto* widget = static_cast<QWidget*>(watched);
    auto* mouse = static_cast<QMouseEvent*>(event);
    const QPoint global = mouse->globalPosition().toPoint();

    // While a panel is in the air, nothing under it takes the pointer.
    if (m_dragActive)
    {
        if (type == QEvent::MouseMove)
            moveDrag(global);
        else if (type == QEvent::MouseButtonRelease && mouse->button() == Qt::LeftButton)
            endDrag(global, true);
        return true;
    }
    if (widget->window() != window())
        return false;

    switch (type)
    {
    case QEvent::MouseButtonPress:
        // The press goes on to whatever is under it; only moving far enough
        // makes it a drag. Called again for each parent it propagates to.
        if (mouse->button() == Qt::LeftButton)
        {
            const int panel = headerPanelAt(widget, mouse->position().toPoint());
            if (panel >= 0)
            {
                m_pressPanel = panel;
                m_pressGlobal = global;
            }
        }
        return false;
    case QEvent::MouseButtonRelease:
        m_pressPanel = -1;
        return false;
    case QEvent::MouseMove:
        if (m_pressPanel >= 0)
        {
            if (!(mouse->buttons() & Qt::LeftButton))
            {
                m_pressPanel = -1;
                return false;
            }
            if ((global - m_pressGlobal).manhattanLength() < QApplication::startDragDistance())
                return false;
            m_dragPanel = m_pressPanel;
            m_pressPanel = -1;
            beginDrag(global);
            return true;
        }
        // An open hand over the parts of a header that drag.
        if (mouse->buttons() == Qt::NoButton && widget == QApplication::widgetAt(global))
            setHover(headerPanelAt(widget, mouse->position().toPoint()) >= 0 ? widget : nullptr);
        return false;
    default:
        return false;
    }
}

void PanelLayout::setHover(QWidget* widget)
{
    if (widget == m_hover)
        return;
    if (m_hover && m_hoverSetCursor)
        m_hover->unsetCursor();
    m_hover = widget;
    m_hoverSetCursor = false;
    if (widget && !widget->testAttribute(Qt::WA_SetCursor))
    {
        widget->setCursor(Qt::OpenHandCursor);
        m_hoverSetCursor = true;
    }
}

void PanelLayout::beginDrag(const QPoint& globalPos)
{
    captureSizes(m_root.get());
    m_dragActive = true;
    m_target = Target();
    setHover(nullptr);
    QApplication::setOverrideCursor(Qt::ClosedHandCursor);
    m_overlay->setGeometry(rect());
    m_overlay->raise();
    m_overlay->show();
    moveDrag(globalPos);
}

void PanelLayout::moveDrag(const QPoint& globalPos)
{
    if (!m_dragActive)
        return;
    const QPoint pos = mapFromGlobal(globalPos);
    const bool browse = m_dragPanel == Live || m_dragPanel == Rooms;
    m_target = browse ? browseTargetAt(pos) : targetAt(pos);
    const bool allowed = m_target.kind != Target::Kind::None;
    QApplication::changeOverrideCursor(allowed ? Qt::ClosedHandCursor : Qt::ForbiddenCursor);

    PanelDropOverlay::Scene scene;
    scene.source = panelRect(m_dragPanel);
    scene.label = panelTitle(m_dragPanel);
    scene.indicator = m_target.indicator;
    scene.outline = m_target.outline;
    scene.cursor = pos;
    scene.allowed = allowed;
    if (allowed && m_target.zone == Zone::Center)
        scene.sourceLabel = panelTitle(m_target.panel);
    m_overlay->setScene(scene);
}

void PanelLayout::endDrag(const QPoint& globalPos, bool drop)
{
    if (!m_dragActive)
        return;
    if (drop)
        moveDrag(globalPos);
    const Target target = m_target;
    const int panel = m_dragPanel;
    m_dragActive = false;
    m_dragPanel = -1;
    m_target = Target();
    QApplication::restoreOverrideCursor();
    m_overlay->hide();
    m_overlay->setScene(PanelDropOverlay::Scene());
    if (!drop || target.kind == Target::Kind::None)
        return;

    // After the release has been delivered: the widget that took it moves.
    QTimer::singleShot(0, this, [this, target, panel]() {
        if (target.kind == Target::Kind::Browse)
        {
            bool stacked = false;
            bool roomsFirst = false;
            const int saved = m_dragPanel;
            m_dragPanel = panel;
            const bool changes = browseChangeFor(target, &stacked, &roomsFirst);
            m_dragPanel = saved;
            if (!changes)
                return;
            m_roomsFirst[stacked ? 1 : 0] = roomsFirst;
            if (stacked != browseStacked())
                setBrowseStacked(stacked);
            else
                applyBrowseOrder();
        }
        else
        {
            const int saved = m_dragPanel;
            m_dragPanel = panel;
            NodePtr next = arrangementFor(target);
            m_dragPanel = saved;
            if (!next)
                return;
            m_root = std::move(next);
            rebuild();
        }
        finishChange();
    });
}

std::unique_ptr<PanelLayout::Node> PanelLayout::arrangementFor(const Target& target) const
{
    if (!m_root || m_dragPanel < 0 || target.kind == Target::Kind::None || target.kind == Target::Kind::Browse)
        return nullptr;
    NodePtr tree = clone(m_root.get());
    if (target.kind == Target::Kind::Panel && target.zone == Zone::Center)
    {
        swapLeaves(tree.get(), m_dragPanel, target.panel);
    }
    else
    {
        NodePtr moving = takeLeaf(tree, m_dragPanel);
        if (!moving)
            return nullptr;
        if (target.kind == Target::Kind::Edge)
            insertAtEdge(tree, std::move(moving), target.zone, size(), stageNeed());
        else
            insertBeside(tree, target.panel, std::move(moving), target.zone, stageNeed());
    }
    if (shape(tree.get()) == shape(m_root.get()))
        return nullptr;
    return tree;
}

// The room window opens over the stage, so a drop leaves the stage room for
// all of it and the stage's padding wherever there's space.
QSize PanelLayout::stageNeed() const
{
    QSize window(620, 470);
    if (m_stage)
    {
        if (auto* room = m_stage->findChild<QWidget*>("RoomWindow", Qt::FindDirectChildrenOnly))
            window = room->minimumSize();
    }
    return window + QSize(kOuter + kStageInner, kOuter + kStageInner);
}

bool PanelLayout::browseChangeFor(const Target& target, bool* stacked, bool* roomsFirst) const
{
    const bool nowStacked = browseStacked();
    if (target.zone == Zone::Center)
    {
        *stacked = nowStacked;
        *roomsFirst = !m_roomsFirst[nowStacked ? 1 : 0];
    }
    else
    {
        *stacked = !across(target.zone);
        *roomsFirst = (m_dragPanel == Rooms) == leading(target.zone);
    }
    return *stacked != nowStacked || *roomsFirst != m_roomsFirst[nowStacked ? 1 : 0];
}

PanelLayout::Target PanelLayout::targetAt(const QPoint& pos) const
{
    const QRect host = rect();
    if (!host.contains(pos))
        return Target();

    // Along a whole edge of the window, when that's somewhere new.
    const int left = pos.x() - host.left(), right = host.right() - pos.x();
    const int top = pos.y() - host.top(), bottom = host.bottom() - pos.y();
    const int nearest = std::min({ left, right, top, bottom });
    if (nearest < kEdgeBand)
    {
        Target edge;
        edge.kind = Target::Kind::Edge;
        edge.zone = nearest == left ? Zone::Left : nearest == right ? Zone::Right : nearest == top ? Zone::Top : Zone::Bottom;
        if (NodePtr next = arrangementFor(edge))
        {
            const QRect inner = host.adjusted(kOuter, kOuter, -kOuter, -kOuter);
            edge.indicator = sideOf(inner, edge.zone, std::max(1, sizeAfter(next.get(), m_dragPanel) - kOuter));
            return edge;
        }
    }

    for (int panel : { Stage, Players, Chat })
    {
        if (!panelBox(panel).contains(pos))
            continue;
        if (panel == m_dragPanel)
            return Target();
        Target target;
        target.kind = Target::Kind::Panel;
        target.panel = panel;
        target.outline = panelRect(panel);
        // The stage can't trade places with a panel: it only makes room.
        target.zone = zoneIn(target.outline, pos, panel != Stage);
        const NodePtr next = arrangementFor(target);
        if (!next)
            return Target();
        if (target.zone == Zone::Center)
            target.indicator = target.outline;
        else if (panel == Stage)
            target.indicator = sideOf(target.outline, target.zone, std::max(1, sizeAfter(next.get(), m_dragPanel) - kStageInner));
        else
            target.indicator = sideOf(target.outline, target.zone);
        return target;
    }
    // On a divider: keep showing what was there.
    return m_target;
}

PanelLayout::Target PanelLayout::browseTargetAt(const QPoint& pos) const
{
    const int other = m_dragPanel == Live ? Rooms : Live;
    const QRect otherRect = panelRect(other);
    const QRect both = panelRect(Live) | panelRect(Rooms);
    constexpr int slack = 6; // half the divider between them
    if (otherRect.isEmpty() || !both.adjusted(-slack, -slack, slack, slack).contains(pos))
        return Target();
    if (!otherRect.adjusted(-slack, -slack, slack, slack).contains(pos))
        return panelRect(m_dragPanel).contains(pos) ? Target() : m_target;

    Target target;
    target.kind = Target::Kind::Browse;
    target.panel = other;
    target.zone = zoneIn(otherRect, pos, true);
    target.outline = otherRect;
    bool stacked = false;
    bool roomsFirst = false;
    if (!browseChangeFor(target, &stacked, &roomsFirst))
        return Target();
    target.indicator = target.zone == Zone::Center ? otherRect : sideOf(both, target.zone);
    return target;
}

// ── Events ───────────────────────────────────────────────────────────

void PanelLayout::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    m_overlay->setGeometry(rect());
}

void PanelLayout::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    // Headers are found from every press in the window, so the filter only
    // runs while the lobby is on screen.
    if (!m_filtering)
    {
        qApp->installEventFilter(this);
        m_filtering = true;
    }
    refreshVisibility();
}

void PanelLayout::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    if (m_dragActive)
        endDrag(QPoint(), false);
    m_pressPanel = -1;
    setHover(nullptr);
    if (m_filtering)
    {
        qApp->removeEventFilter(this);
        m_filtering = false;
    }
}

#endif // NETPLAY
