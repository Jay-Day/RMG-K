#ifndef LOBBYROOMWINDOW_HPP
#define LOBBYROOMWINDOW_HPP

#ifdef NETPLAY

#include <QFrame>
#include <QPointer>
#include <QRect>

class QHBoxLayout;
class QVBoxLayout;

namespace UserInterface
{
namespace Dialog
{

class IconButton;

// Dims the lobby behind the room window and drops a shadow around it. A click
// on it minimizes the room: the room's ticket in the play bar brings it back.
class StageScrim : public QWidget
{
    Q_OBJECT

public:
    explicit StageScrim(QWidget* parent = nullptr);
    // Where the window sits, in this widget's coordinates, for the shadow.
    void setWindowRect(const QRect& rect);

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    QRect m_window;
};

// The room as a window inside the lobby. It floats over the browse view, moves
// by its title bar and resizes from its edges, and can fill the whole stage
// (maximize) or get out of the way (minimize) while you keep your seat. Its
// parent lays it out with placeIn whenever the stage changes size.
class RoomWindow : public QWidget
{
    Q_OBJECT

public:
    explicit RoomWindow(QWidget* parent = nullptr);

    // The title bar: the room's badge and name on the left; its state and
    // settings on the right, before the window buttons.
    QHBoxLayout* titleLeft() const { return m_titleLeft; }
    QHBoxLayout* titleRight() const { return m_titleRight; }
    void setBody(QWidget* body);

    bool isMaximized() const { return m_maximized; }
    void setMaximized(bool maximized);
    // Fits the window into bounds, in the parent's coordinates: all of it while
    // maximized, otherwise the normal geometry, kept inside it.
    void placeIn(const QRect& bounds);
    // Where the window goes when it isn't maximized. Invalid until it's moved,
    // resized or set; the window then sits centered at a comfortable size.
    QRect normalGeometry() const { return m_normal; }
    void setNormalGeometry(const QRect& geometry);

signals:
    void minimizeRequested();
    void maximizedChanged(bool maximized);
    // The user moved or resized the window.
    void normalGeometryEdited();
    // It moved or changed size, for whatever is drawn around it.
    void geometryChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    enum Edge { NoEdge = 0, Left = 1, Right = 2, Top = 4, Bottom = 8 };
    int edgesAt(const QPoint& pos) const;
    void updateCursor(int edges);
    QRect defaultGeometry() const;
    QRect keptInside(QRect rect) const;
    void refreshMaximizeButton();

    QWidget* m_titleBar = nullptr;
    QHBoxLayout* m_titleLeft = nullptr;
    QHBoxLayout* m_titleRight = nullptr;
    IconButton* m_minimize = nullptr;
    IconButton* m_maximize = nullptr;
    QVBoxLayout* m_root = nullptr;
    QWidget* m_body = nullptr;

    QRect m_bounds;
    QRect m_normal;
    bool m_maximized = false;
    int m_dragEdges = NoEdge;
    bool m_moving = false;
    QPoint m_pressGlobal;
    QRect m_pressGeometry;
};

// A panel that drops down from a button, inside the button's window. It closes
// on a click anywhere else (not counting the popups its own controls open), on
// Escape, or when the button is pressed again.
class DropPanel : public QFrame
{
    Q_OBJECT

public:
    explicit DropPanel(QWidget* parent = nullptr);
    // Opens right-aligned under anchor, or closes if it's open.
    void toggleBelow(QWidget* anchor);

signals:
    void opened();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QPointer<QWidget> m_anchor;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // LOBBYROOMWINDOW_HPP
