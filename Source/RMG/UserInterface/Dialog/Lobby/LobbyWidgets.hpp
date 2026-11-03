#ifndef LOBBYWIDGETS_HPP
#define LOBBYWIDGETS_HPP

#ifdef NETPLAY

#include "LobbyTheme.hpp"

#include <QCheckBox>
#include <QFrame>
#include <QPushButton>
#include <QToolButton>
#include <QWidget>

class QLabel;
class QTimer;

namespace UserInterface
{
namespace Dialog
{

// A single line of text that elides instead of pushing its layout wider, so a
// long name can never raise a column's minimum width. An optional lead is drawn
// first in a stronger colour ("<lead> · the rest").
class ElidedLabel : public QWidget
{
    Q_OBJECT

public:
    explicit ElidedLabel(LobbyTheme::Role role = LobbyTheme::Role::Ink, QWidget* parent = nullptr);

    void setText(const QString& text);
    void setLead(const QString& lead);
    QString text() const { return m_text; }
    void setRole(LobbyTheme::Role role);
    void setAlignment(Qt::Alignment alignment);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_text;
    QString m_lead;
    LobbyTheme::Role m_role;
    Qt::Alignment m_alignment = Qt::AlignLeft | Qt::AlignVCenter;
};

// The lobby's buttons, drawn with a console-style lip that the face presses into.
class LobbyButton : public QPushButton
{
    Q_OBJECT

public:
    enum class Kind { Primary, Secondary, Ghost, Danger };
    enum class Size { Small, Medium, Large }; // 28, 34 and 44 px tall

    explicit LobbyButton(const QString& text, Kind kind = Kind::Secondary, QWidget* parent = nullptr);

    void setKind(Kind kind);
    void setIconName(const QString& name);
    void setSubText(const QString& text);
    // Display-font caps, for the big calls to action.
    void setPixelLabel(bool pixel);
    void setButtonSize(Size size);
    // A dashed outline: there's a reason you can't, not just "not now".
    void setDashed(bool dashed);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QFont labelFont() const;
    QString labelText() const;
    int lipHeight() const;

    Kind m_kind;
    Size m_size = Size::Medium;
    QString m_iconName;
    QString m_subText;
    bool m_pixelLabel = false;
    bool m_dashed = false;
};

// A button whose face is a layout of other widgets, sized from that layout.
class LayoutButton : public QPushButton
{
    Q_OBJECT

public:
    using QPushButton::QPushButton;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
};

// A small flat button holding one icon: the search and filter toggles, the
// kick cross on a port card, the chat send button.
class IconButton : public QToolButton
{
    Q_OBJECT

public:
    explicit IconButton(const QString& iconName, int side = 26, QWidget* parent = nullptr);

    void setIconName(const QString& name);
    void setIconColorOverride(const QColor& color);
    // A visible outline, for the stepper's plus and minus.
    void setBoxed(bool boxed);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_iconName;
    QColor m_override;
    bool m_boxed = false;
};

// A tinted icon that follows the theme.
class IconLabel : public QWidget
{
    Q_OBJECT

public:
    explicit IconLabel(const QString& iconName, LobbyTheme::Role role, int side, QWidget* parent = nullptr);

    void setIconName(const QString& name);
    void setRole(LobbyTheme::Role role);
    void setColorOverride(const QColor& color);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_iconName;
    LobbyTheme::Role m_role;
    QColor m_override;
};

// The window-wide background: flat colour with two soft glows in the corners.
class LobbyBackdrop : public QWidget
{
    Q_OBJECT

public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent* event) override;
};

// A rounded panel. Scanlines make it read as hardware (the play bar); the
// border can take on a state's colour.
class LobbyCard : public QFrame
{
    Q_OBJECT

public:
    explicit LobbyCard(QWidget* parent = nullptr, bool scanlines = false);

    void setBorderColor(const QColor& color);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    bool m_scanlines;
    QColor m_borderColor;
};

// The top bar's background.
class LobbyBand : public QFrame
{
    Q_OBJECT

public:
    using QFrame::QFrame;

protected:
    void paintEvent(QPaintEvent* event) override;
};

// The four port colours as a hairline under the top bar.
class PortStripe : public QWidget
{
    Q_OBJECT

public:
    explicit PortStripe(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
};

// The four port colours as the brand's square mark.
class BrandMark : public QWidget
{
    Q_OBJECT

public:
    explicit BrandMark(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
};

class LedDot : public QWidget
{
    Q_OBJECT

public:
    explicit LedDot(QWidget* parent = nullptr);
    void setColor(const QColor& color);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QColor m_color;
};

class Spinner : public QWidget
{
    Q_OBJECT

public:
    explicit Spinner(int side, QWidget* parent = nullptr);
    void setColors(const QColor& track, const QColor& head);

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QTimer* m_timer;
    int m_angle = 0;
    QColor m_track;
    QColor m_head;
};

// The small square in front of a section's heading; on air it's a red dot.
class SectionMarker : public QWidget
{
    Q_OBJECT

public:
    enum class Kind { Plain, OnAir };

    explicit SectionMarker(Kind kind, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    Kind m_kind;
};

// A section's heading text.
QLabel* makeSectionLabel(const QString& text, QWidget* parent);

// The player's initial on an accent tile, for the top bar's user chip.
class UserAvatar : public QWidget
{
    Q_OBJECT

public:
    explicit UserAvatar(QWidget* parent = nullptr);
    void setInitial(const QString& initial);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_initial;
};

// An icon on a tinted square: the play bar's state, or a smaller round one
// beside the room's status line.
class IconBadge : public QWidget
{
    Q_OBJECT

public:
    IconBadge(int side, qreal radius, int iconSide, QWidget* parent = nullptr);
    void setIconAndColors(const QString& icon, const QColor& ink, const QColor& fill);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    qreal m_radius;
    int m_iconSide;
    QString m_icon;
    QColor m_ink;
    QColor m_fill;
};

// A short status word in a filled box. Live is the red on-air mark.
class StatusTag : public QWidget
{
    Q_OBJECT

public:
    enum class Kind { Neutral, Red, Green, Amber, Purple, Live };

    explicit StatusTag(Kind kind = Kind::Neutral, QWidget* parent = nullptr);
    void setTag(Kind kind, const QString& text);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QFont tagFont() const;

    Kind m_kind;
    QString m_text;
};

// "P1" in the port's colour.
class PortChip : public QWidget
{
    Q_OBJECT

public:
    explicit PortChip(int slot, QWidget* parent = nullptr);
    void setSlot(int slot);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    int m_slot;
};

class SignalBars : public QWidget
{
    Q_OBJECT

public:
    explicit SignalBars(QWidget* parent = nullptr);
    void setPing(int ms, bool estimate);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    int m_ms = -1;
    bool m_estimate = false;
};

// One pip per seat, filled in port colours.
class SeatPips : public QWidget
{
    Q_OBJECT

public:
    explicit SeatPips(QWidget* parent = nullptr);
    void setSeats(int filled, int total);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    int m_filled = 0;
    int m_total = 0;
};

class CountryFlag : public QWidget
{
    Q_OBJECT

public:
    explicit CountryFlag(QWidget* parent = nullptr);
    // Hides itself when there's no flag to show.
    void setCountry(const QString& country);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_country;
};

// The connection type as a small symbol, drawn so it stays crisp and needs no
// platform font to have the glyph.
class ConnectionGlyph : public QWidget
{
    Q_OBJECT

public:
    explicit ConnectionGlyph(QWidget* parent = nullptr);
    void setConnection(const QString& connection);

    static QString displayName(const QString& connection);
    static void paintSymbol(QPainter& painter, const QRectF& rect, const QString& connection, const QColor& color);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_connection;
};

// A game's tile: an N64 Game Pak whose label is the game's cover art when the
// library has one, otherwise its initials.
class CartridgeTile : public QWidget
{
    Q_OBJECT

public:
    enum class Size { Small, Normal, Large }; // 42x29, 64x44, 84x56

    explicit CartridgeTile(Size size = Size::Normal, QWidget* parent = nullptr);

    void setGame(const QString& gameName, const QString& coverPath = QString());
    void setTileSize(Size size);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    Size m_size;
    QString m_game;
    QString m_coverPath;
};

// A game as a small rounded square, for lists: its cover art, or its initials
// on its colour.
class GameBadge : public QWidget
{
    Q_OBJECT

public:
    explicit GameBadge(int side, QWidget* parent = nullptr);

    void setGame(const QString& gameName, const QString& coverPath = QString());

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_game;
    QString m_coverPath;
};

// A stepper for a frame count, with a chip that hands the value back to Auto
// or Default. edited() fires only on the user's own changes.
class FrameStepper : public QWidget
{
    Q_OBJECT

public:
    FrameStepper(int minimum, int maximum, const QString& iconName, QWidget* parent = nullptr);

    void setValue(int value);
    int value() const { return m_value; }
    void setAutomatic(bool automatic);
    bool isAutomatic() const { return m_automatic; }
    // Text for the chip when the value is automatic, and when it's manual.
    void setChipLabels(const QString& automaticLabel, const QString& manualLabel);
    // Whether the chip lights up while automatic. Default doesn't need to shout.
    void setChipAccented(bool accented);

signals:
    void edited();

protected:
    void changeEvent(QEvent* event) override;

private:
    void step(int delta);
    void refresh();

    int m_minimum;
    int m_maximum;
    int m_value;
    QString m_iconName;
    bool m_automatic = true;
    bool m_chipAccented = true;
    QString m_automaticLabel;
    QString m_manualLabel;
    IconButton* m_minus;
    IconButton* m_plus;
    QLabel* m_valueLabel;
    QPushButton* m_chip;
};

// A checkbox drawn as a switch, with a title and a line of explanation.
class ToggleSwitch : public QCheckBox
{
    Q_OBJECT

public:
    explicit ToggleSwitch(const QString& title, QWidget* parent = nullptr);

    void setSubText(const QString& text);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    bool hitButton(const QPoint& pos) const override;

private:
    QString m_subText;
};

// The room's state as an LED and a word in a soft pill.
class StatePill : public QWidget
{
    Q_OBJECT

public:
    explicit StatePill(QWidget* parent = nullptr);
    void setState(const QString& text, const QColor& color);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_text;
    QColor m_color;
};

// One player's seat in a room. The dialog owns the seat logic; this is the
// card's look and its three moods: open, playing, and waiting to join.
class PortCard : public QFrame
{
    Q_OBJECT

public:
    explicit PortCard(int slot, QWidget* parent = nullptr);

    void showOpen(const QString& hint);
    // isSelf: played on this PC. selfTag names how, in place of "You".
    void showPlayer(const QString& name, bool isSelf, bool isHost, const QString& selfTag = QString());
    void setCountry(const QString& country, const QString& tooltip);
    void setConnection(const QString& connection);
    void setDelay(int frames);

    // What the Link stat shows: this client, a measured ping, or the state of
    // a connection that isn't up yet.
    void setLinkLocal();
    void setLinkPing(int ms);
    void setLinkConnecting(int attempt, int maxAttempts);
    void setLinkStatus(const QString& text, LobbyTheme::Role role);

    // A member seated beside a running match they aren't in yet.
    void setWaitingToJoin(bool waiting);
    void setAdmitVisible(bool visible);

    void setHostTools(bool dragHandle, bool kick);

    QWidget* dragHandle() const { return m_dragHandle; }
    IconButton* kickButton() const { return m_kick; }
    LobbyButton* admitButton() const { return m_admit; }
    int slotNumber() const { return m_slot; }
    bool isOpen() const { return m_open; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    class Portrait;
    class StatBox;

    void refreshBottom();
    // What the connection state shows above the stats. A member waiting to join
    // isn't in the match yet, so there's no connection to report on.
    void refreshConnecting();

    int m_slot;
    bool m_open = true;
    bool m_self = false;
    bool m_connecting = false;
    bool m_waiting = false;
    bool m_admitVisible = false;

    QWidget* m_cap = nullptr;
    QLabel* m_capLabel = nullptr;
    QWidget* m_hostBadge = nullptr;
    IconLabel* m_dragHandle = nullptr;
    IconButton* m_kick = nullptr;

    QWidget* m_openPage = nullptr;
    QLabel* m_openSlot = nullptr;
    QLabel* m_openHint = nullptr;

    QWidget* m_playerPage = nullptr;
    Portrait* m_portrait = nullptr;
    ElidedLabel* m_name = nullptr;
    StatusTag* m_youTag = nullptr;
    CountryFlag* m_flag = nullptr;
    ConnectionGlyph* m_connectionIcon = nullptr;
    QLabel* m_connectionName = nullptr;
    QWidget* m_progress = nullptr;
    QLabel* m_progressHint = nullptr;
    StatusTag* m_waitingTag = nullptr;
    QWidget* m_stats = nullptr;
    StatBox* m_linkStat = nullptr;
    StatBox* m_delayStat = nullptr;
    StatBox* m_waitingStat = nullptr;
    LobbyButton* m_admit = nullptr;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // LOBBYWIDGETS_HPP
