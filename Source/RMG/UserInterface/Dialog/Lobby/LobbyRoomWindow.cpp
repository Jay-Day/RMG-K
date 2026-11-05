#ifdef NETPLAY

#include "LobbyRoomWindow.hpp"
#include "LobbyTheme.hpp"
#include "LobbyWidgets.hpp"

#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

#include <algorithm>

using namespace UserInterface::Dialog;
using LobbyTheme::Tokens;

namespace
{
    constexpr int kRadius      = 12;
    constexpr int kGrip        = 5;   // the edge you resize from, inside the border
    constexpr int kTitleHeight = 46;
    constexpr int kStripe      = 3;   // the port colours under the title bar
    constexpr int kShadow      = 14;
    // A normal window opens at most this big, so the lobby still shows around it.
    constexpr int kNormalWidth  = 980;
    constexpr int kNormalHeight = 700;
} // namespace

// ── StageScrim ───────────────────────────────────────────────────────

StageScrim::StageScrim(QWidget* parent)
    : QWidget(parent)
{
    setCursor(Qt::PointingHandCursor);
    setToolTip(tr("Minimize the room. You keep your seat."));
}

void StageScrim::setWindowRect(const QRect& rect)
{
    if (rect == m_window)
        return;
    m_window = rect;
    update();
}

void StageScrim::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Tokens& t = LobbyTheme::tokens();
    painter.fillRect(rect(), t.dark ? QColor(8, 7, 9, 150) : QColor(60, 45, 20, 70));

    if (m_window.isEmpty())
        return;
    // A soft shadow: rings fading out from the window's edge.
    painter.setPen(Qt::NoPen);
    for (int i = kShadow; i >= 1; --i)
    {
        const qreal strength = 1.0 - qreal(i) / (kShadow + 1);
        painter.setBrush(QColor(0, 0, 0, int((t.dark ? 34 : 18) * strength * strength)));
        QPainterPath ring;
        ring.addRoundedRect(QRectF(m_window).adjusted(-i, -i + 3, i, i + 3), kRadius + i, kRadius + i);
        painter.drawPath(ring);
    }
}

void StageScrim::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        emit clicked();
}

// ── RoomWindow ───────────────────────────────────────────────────────

RoomWindow::RoomWindow(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("RoomWindow");
    setMouseTracking(true);
    setMinimumSize(620, 470);

    m_root = new QVBoxLayout(this);
    m_root->setContentsMargins(kGrip, 0, kGrip, kGrip);
    m_root->setSpacing(0);

    m_titleBar = new QWidget(this);
    m_titleBar->setObjectName("RoomWindowTitle");
    m_titleBar->setFixedHeight(kTitleHeight);
    m_titleBar->setMouseTracking(true);
    m_titleBar->installEventFilter(this);
    auto* title = new QHBoxLayout(m_titleBar);
    title->setContentsMargins(9, 0, 3, 0);
    title->setSpacing(10);
    m_titleLeft = new QHBoxLayout;
    m_titleLeft->setSpacing(10);
    title->addLayout(m_titleLeft, 1);
    m_titleRight = new QHBoxLayout;
    m_titleRight->setSpacing(6);
    title->addLayout(m_titleRight);

    auto* rule = new QFrame(m_titleBar);
    rule->setObjectName("BrandSeparator");
    rule->setFixedSize(1, 22);
    title->addWidget(rule);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(2);
    m_minimize = new IconButton(QStringLiteral("subtract-line"), 28, m_titleBar);
    m_minimize->setObjectName("RoomMinimizeButton");
    m_minimize->setToolTip(tr("Minimize. You keep your seat, and the room waits in the bar at the top."));
    m_minimize->setAccessibleName(tr("Minimize room"));
    connect(m_minimize, &QToolButton::clicked, this, &RoomWindow::minimizeRequested);
    buttons->addWidget(m_minimize);
    m_maximize = new IconButton(QStringLiteral("window-maximize"), 28, m_titleBar);
    m_maximize->setObjectName("RoomMaximizeButton");
    connect(m_maximize, &QToolButton::clicked, this, [this]() { setMaximized(!m_maximized); });
    buttons->addWidget(m_maximize);
    title->addLayout(buttons);
    m_root->addWidget(m_titleBar);
    m_root->addSpacing(kStripe);

    refreshMaximizeButton();
}

void RoomWindow::setBody(QWidget* body)
{
    if (m_body)
        m_root->removeWidget(m_body);
    m_body = body;
    if (m_body)
    {
        m_body->setParent(this);
        m_root->addWidget(m_body, 1);
    }
}

void RoomWindow::setMaximized(bool maximized)
{
    if (maximized == m_maximized)
        return;
    m_maximized = maximized;
    refreshMaximizeButton();
    if (m_bounds.isValid())
        placeIn(m_bounds);
    emit maximizedChanged(m_maximized);
}

void RoomWindow::refreshMaximizeButton()
{
    m_maximize->setIconName(m_maximized ? QStringLiteral("window-restore") : QStringLiteral("window-maximize"));
    const QString tip = m_maximized ? tr("Restore down") : tr("Maximize");
    m_maximize->setToolTip(tip + tr(" (or double-click the title)"));
    m_maximize->setAccessibleName(tip);
    unsetCursor();
}

void RoomWindow::setNormalGeometry(const QRect& geometry)
{
    m_normal = geometry;
    if (!m_maximized && m_bounds.isValid())
        placeIn(m_bounds);
}

QRect RoomWindow::defaultGeometry() const
{
    const int width = std::min(kNormalWidth, m_bounds.width() - 48);
    const int height = std::min(kNormalHeight, m_bounds.height() - 40);
    QRect rect(0, 0, std::max(width, minimumWidth()), std::max(height, minimumHeight()));
    rect.moveCenter(m_bounds.center());
    return rect;
}

QRect RoomWindow::keptInside(QRect rect) const
{
    if (!m_bounds.isValid())
        return rect;
    rect.setWidth(std::clamp(rect.width(), std::min(minimumWidth(), m_bounds.width()), m_bounds.width()));
    rect.setHeight(std::clamp(rect.height(), std::min(minimumHeight(), m_bounds.height()), m_bounds.height()));
    rect.moveLeft(std::clamp(rect.left(), m_bounds.left(), m_bounds.right() - rect.width() + 1));
    rect.moveTop(std::clamp(rect.top(), m_bounds.top(), m_bounds.bottom() - rect.height() + 1));
    return rect;
}

void RoomWindow::placeIn(const QRect& bounds)
{
    m_bounds = bounds;
    if (m_maximized)
        setGeometry(bounds);
    else
        setGeometry(keptInside(m_normal.isValid() ? m_normal : defaultGeometry()));
}

int RoomWindow::edgesAt(const QPoint& pos) const
{
    if (m_maximized)
        return NoEdge;
    int edges = NoEdge;
    if (pos.x() < kGrip + 2)
        edges |= Left;
    else if (pos.x() >= width() - kGrip - 2)
        edges |= Right;
    if (pos.y() < 4)
        edges |= Top;
    else if (pos.y() >= height() - kGrip - 2)
        edges |= Bottom;
    return edges;
}

void RoomWindow::updateCursor(int edges)
{
    const bool horizontal = edges & (Left | Right);
    const bool vertical = edges & (Top | Bottom);
    if (horizontal && vertical)
        setCursor(((edges & Left) != 0) == ((edges & Top) != 0) ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
    else if (horizontal)
        setCursor(Qt::SizeHorCursor);
    else if (vertical)
        setCursor(Qt::SizeVerCursor);
    else
        unsetCursor();
}

bool RoomWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != m_titleBar)
        return QWidget::eventFilter(watched, event);

    // The title bar moves the window, and its top few pixels resize it.
    switch (event->type())
    {
    case QEvent::MouseButtonDblClick:
        if (static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton)
        {
            setMaximized(!m_maximized);
            return true;
        }
        break;
    case QEvent::MouseButtonPress:
    case QEvent::MouseMove:
    case QEvent::MouseButtonRelease:
    {
        auto* mouse = static_cast<QMouseEvent*>(event);
        const QPoint pos = m_titleBar->mapTo(this, mouse->position().toPoint());
        if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton)
        {
            m_dragEdges = edgesAt(pos);
            m_moving = m_dragEdges == NoEdge && !m_maximized;
            m_pressGlobal = mouse->globalPosition().toPoint();
            m_pressGeometry = geometry();
            return true;
        }
        if (event->type() == QEvent::MouseMove)
        {
            if (m_moving && (mouse->buttons() & Qt::LeftButton))
            {
                QRect moved = m_pressGeometry;
                moved.moveTopLeft(m_pressGeometry.topLeft() + mouse->globalPosition().toPoint() - m_pressGlobal);
                setGeometry(keptInside(moved));
                m_normal = geometry();
                return true;
            }
            if (m_dragEdges != NoEdge)
            {
                mouseMoveEvent(mouse);
                return true;
            }
            m_titleBar->setCursor(edgesAt(pos) & Top ? Qt::SizeVerCursor : Qt::ArrowCursor);
            return false;
        }
        if (mouse->button() == Qt::LeftButton && (m_moving || m_dragEdges != NoEdge))
        {
            m_moving = false;
            m_dragEdges = NoEdge;
            emit normalGeometryEdited();
            return true;
        }
        break;
    }
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void RoomWindow::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_dragEdges = edgesAt(event->position().toPoint());
    m_pressGlobal = event->globalPosition().toPoint();
    m_pressGeometry = geometry();
}

void RoomWindow::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragEdges == NoEdge || !(event->buttons() & Qt::LeftButton))
    {
        updateCursor(edgesAt(event->position().toPoint()));
        return;
    }
    const QPoint delta = event->globalPosition().toPoint() - m_pressGlobal;
    QRect rect = m_pressGeometry;
    const int minWidth = std::min(minimumWidth(), m_bounds.width());
    const int minHeight = std::min(minimumHeight(), m_bounds.height());
    if (m_dragEdges & Left)
        rect.setLeft(std::clamp(rect.left() + delta.x(), m_bounds.left(), rect.right() - minWidth + 1));
    if (m_dragEdges & Right)
        rect.setRight(std::clamp(rect.right() + delta.x(), rect.left() + minWidth - 1, m_bounds.right()));
    if (m_dragEdges & Top)
        rect.setTop(std::clamp(rect.top() + delta.y(), m_bounds.top(), rect.bottom() - minHeight + 1));
    if (m_dragEdges & Bottom)
        rect.setBottom(std::clamp(rect.bottom() + delta.y(), rect.top() + minHeight - 1, m_bounds.bottom()));
    setGeometry(rect);
    m_normal = rect;
}

void RoomWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_dragEdges != NoEdge)
    {
        m_dragEdges = NoEdge;
        emit normalGeometryEdited();
    }
}

void RoomWindow::leaveEvent(QEvent* event)
{
    if (m_dragEdges == NoEdge)
        unsetCursor();
    QWidget::leaveEvent(event);
}

void RoomWindow::moveEvent(QMoveEvent* event)
{
    QWidget::moveEvent(event);
    emit geometryChanged();
}

void RoomWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    emit geometryChanged();
}

void RoomWindow::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Tokens& t = LobbyTheme::tokens();
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath shape;
    shape.addRoundedRect(box, kRadius, kRadius);

    painter.fillPath(shape, t.bg);
    painter.save();
    painter.setClipPath(shape);
    QLinearGradient band(0, 0, 0, kTitleHeight);
    band.setColorAt(0, t.bandA);
    band.setColorAt(1, t.bandB);
    painter.fillRect(QRectF(0, 0, width(), kTitleHeight), band);
    const qreal quarter = width() / 4.0;
    for (int i = 0; i < 4; ++i)
        painter.fillRect(QRectF(i * quarter, kTitleHeight, quarter + 1, kStripe), LobbyTheme::portColor(i + 1));
    painter.restore();

    painter.setPen(QPen(t.line2, 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(shape);
}

// ── DropPanel ────────────────────────────────────────────────────────

DropPanel::DropPanel(QWidget* parent)
    : QFrame(parent)
{
    setObjectName("DropPanel");
    setVisible(false);
}

void DropPanel::toggleBelow(QWidget* anchor)
{
    if (isVisible())
    {
        hide();
        return;
    }
    m_anchor = anchor;
    adjustSize();
    QWidget* host = parentWidget();
    const QPoint corner = anchor->mapTo(host, QPoint(anchor->width(), anchor->height() + 4));
    const int x = std::clamp(corner.x() - width(), 4, std::max(4, host->width() - width() - 4));
    const int y = std::clamp(corner.y(), 4, std::max(4, host->height() - height() - 4));
    move(x, y);
    raise();
    show();
    emit opened();
}

void DropPanel::showEvent(QShowEvent* event)
{
    QFrame::showEvent(event);
    qApp->installEventFilter(this);
}

void DropPanel::hideEvent(QHideEvent* event)
{
    qApp->removeEventFilter(this);
    QFrame::hideEvent(event);
}

bool DropPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape)
    {
        hide();
        return true;
    }
    if (event->type() != QEvent::MouseButtonPress)
        return false;
    auto* target = qobject_cast<QWidget*>(watched);
    if (target == nullptr)
        return false;
    // The panel's own popups (a combo box's list) count as inside it.
    QWidget* top = target->window();
    if (top != window() && (top->windowFlags() & Qt::Popup) == Qt::Popup)
        return false;
    if (target == this || isAncestorOf(target))
        return false;
    // The button that opened it closes it itself.
    if (m_anchor && (target == m_anchor || m_anchor->isAncestorOf(target)))
        return false;
    hide();
    return false;
}

void DropPanel::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Tokens& t = LobbyTheme::tokens();
    QPainterPath shape;
    shape.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 9, 9);
    painter.fillPath(shape, t.panel2);
    painter.setPen(QPen(t.line2, 1.0));
    painter.drawPath(shape);
}

#endif // NETPLAY
