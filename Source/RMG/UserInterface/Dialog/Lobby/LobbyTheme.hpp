#ifndef LOBBYTHEME_HPP
#define LOBBYTHEME_HPP

#ifdef NETPLAY

#include <QColor>
#include <QFont>
#include <QPixmap>
#include <QString>

class QPainter;
class QPointF;
class QRectF;

namespace UserInterface
{
namespace Dialog
{
namespace LobbyTheme
{

// The lobby's colours: console charcoal when the application palette is dark,
// Famicom cream when it's light. Painted widgets read these on every paint and
// the stylesheet is generated from them, so nothing else hard-codes a colour.
// P2 keeps its port blue; nothing else in the theme is blue.
struct Tokens
{
    bool dark;

    QColor bg, glowA, glowB;
    QColor panel, panelB, panel2, raise, well, tint;
    QColor line, line2;
    QColor ink, ink2, muted, faint;
    QColor port1, port2, port3, port4;
    QColor accent, accentHi, accentLo, accentLip, accentTint, accentInk;
    QColor live, liveBg, liveTint;
    QColor ok, okTint, warn, warnTint, bad;
    QColor stateRoom, stateWatch, watchTint;
    QColor secondaryA, secondaryB, secondaryLip;
    QColor bandA, bandB;
    QColor cartA, cartB;
    QColor stat, scan;
};

// Text colours a widget can follow across a theme change.
enum class Role { Ink, Ink2, Muted, Faint, Accent, Ok, Warn, Bad, Live, Watch };

bool isDark();
const Tokens& tokens();
QColor roleColor(Role role);
QColor portColor(int slot);

// amountOfA is a's share of the result. Alpha is mixed too.
QColor mix(const QColor& a, const QColor& b, qreal amountOfA);
QString css(const QColor& color);

// Silkscreen for labels and big numbers. It has no kana, so Japanese falls back
// to the system UI font at the same size.
QFont pixelFont(int px, qreal letterSpacing = 0.0);
// The first character of a name, for avatars. Can be two code units.
QString nameInitial(const QString& name);
QFont uiFont(int px, QFont::Weight weight = QFont::Normal);

// A tinted icon from the lobby's resources, sized for the given pixel ratio.
QPixmap icon(const QString& name, const QColor& color, int px, qreal devicePixelRatio);
// The bundled country flag at 15x10, or a null pixmap when there isn't one.
QPixmap flag(const QString& country, qreal devicePixelRatio);

QColor pingColor(int ms);
// Three signal bars with their bottom-left corner at origin. ms < 0 lights none.
void paintPingBars(QPainter& painter, const QPointF& origin, int ms, bool estimate);

// Horizontal scanlines over rect, clipped by the painter's current clip.
void paintScanlines(QPainter& painter, const QRectF& rect, const QColor& color);

// Initials and label colour for a game with no cover art ("SR" for Smash Remix).
QString gameInitials(const QString& gameName);
QColor gameHue(const QString& gameName);

// The stylesheet covering everything that isn't custom painted.
QString styleSheet();

} // namespace LobbyTheme
} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // LOBBYTHEME_HPP
