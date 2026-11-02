/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifdef NETPLAY

#include "LobbyWidgets.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

using namespace UserInterface::Dialog;
using LobbyTheme::Role;
using LobbyTheme::Tokens;

namespace
{
    QPainterPath roundedPath(const QRectF& rect, qreal radius)
    {
        QPainterPath path;
        path.addRoundedRect(rect, radius, radius);
        return path;
    }

    // Top corners and bottom corners rounded differently, like a cartridge.
    QPainterPath cartridgePath(const QRectF& r, qreal top, qreal bottom)
    {
        QPainterPath path;
        path.moveTo(r.left() + top, r.top());
        path.lineTo(r.right() - top, r.top());
        path.arcTo(QRectF(r.right() - 2 * top, r.top(), 2 * top, 2 * top), 90, -90);
        path.lineTo(r.right(), r.bottom() - bottom);
        path.arcTo(QRectF(r.right() - 2 * bottom, r.bottom() - 2 * bottom, 2 * bottom, 2 * bottom), 0, -90);
        path.lineTo(r.left() + bottom, r.bottom());
        path.arcTo(QRectF(r.left(), r.bottom() - 2 * bottom, 2 * bottom, 2 * bottom), 270, -90);
        path.lineTo(r.left(), r.top() + top);
        path.arcTo(QRectF(r.left(), r.top(), 2 * top, 2 * top), 180, -90);
        path.closeSubpath();
        return path;
    }

    // Dark ink on the yellow and green ports in the dark theme, which are too
    // bright for white. The light theme's are deep enough to take it.
    QColor inkOnPort(int slot)
    {
        if (!LobbyTheme::tokens().dark || slot == 1 || slot == 2)
            return Qt::white;
        return slot == 3 ? QColor("#2a1a00") : QColor("#062a10");
    }
} // namespace

// ── ElidedLabel ──────────────────────────────────────────────────────

ElidedLabel::ElidedLabel(Role role, QWidget* parent)
    : QWidget(parent), m_role(role)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

void ElidedLabel::setText(const QString& text)
{
    if (m_text == text)
        return;
    m_text = text;
    updateGeometry();
    update();
}

void ElidedLabel::setLead(const QString& lead)
{
    if (m_lead == lead)
        return;
    m_lead = lead;
    updateGeometry();
    update();
}

void ElidedLabel::setRole(Role role)
{
    m_role = role;
    update();
}

void ElidedLabel::setAlignment(Qt::Alignment alignment)
{
    m_alignment = alignment;
    update();
}

QSize ElidedLabel::sizeHint() const
{
    const QFontMetrics fm(font());
    QFont leadFont = font();
    leadFont.setWeight(QFont::DemiBold);
    const int width = QFontMetrics(leadFont).horizontalAdvance(m_lead) + fm.horizontalAdvance(m_text);
    return QSize(width + 1, fm.height());
}

QSize ElidedLabel::minimumSizeHint() const
{
    return QSize(std::min(sizeHint().width(), 24), QFontMetrics(font()).height());
}

void ElidedLabel::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    QRect area = rect();

    if (!m_lead.isEmpty())
    {
        QFont leadFont = font();
        leadFont.setWeight(QFont::DemiBold);
        const QFontMetrics fm(leadFont);
        const QString lead = fm.elidedText(m_lead, Qt::ElideRight, area.width());
        painter.setFont(leadFont);
        painter.setPen(LobbyTheme::tokens().ink2);
        painter.drawText(area, Qt::AlignLeft | Qt::AlignVCenter, lead);
        area.setLeft(area.left() + fm.horizontalAdvance(lead));
        painter.setFont(font());
    }

    const QString text = QFontMetrics(font()).elidedText(m_text, Qt::ElideRight, area.width());
    painter.setPen(LobbyTheme::roleColor(m_role));
    painter.drawText(area, static_cast<int>(m_alignment), text);
}

// ── LobbyButton ──────────────────────────────────────────────────────

LobbyButton::LobbyButton(const QString& text, Kind kind, QWidget* parent)
    : QPushButton(text, parent), m_kind(kind)
{
    setAutoDefault(false);
    setDefault(false);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void LobbyButton::setKind(Kind kind)
{
    m_kind = kind;
    update();
}

void LobbyButton::setIconName(const QString& name)
{
    m_iconName = name;
    updateGeometry();
    update();
}

void LobbyButton::setSubText(const QString& text)
{
    m_subText = text;
    updateGeometry();
    update();
}

void LobbyButton::setPixelLabel(bool pixel)
{
    m_pixelLabel = pixel;
    updateGeometry();
    update();
}

void LobbyButton::setButtonSize(Size size)
{
    m_size = size;
    updateGeometry();
    update();
}

void LobbyButton::setDashed(bool dashed)
{
    m_dashed = dashed;
    update();
}

QFont LobbyButton::labelFont() const
{
    if (m_pixelLabel)
        return LobbyTheme::pixelFont(m_size == Size::Small ? 11 : 13, 0.5);
    return LobbyTheme::uiFont(m_size == Size::Small ? 11 : 12, QFont::Bold);
}

QString LobbyButton::labelText() const
{
    return m_pixelLabel ? text().toUpper() : text();
}

int LobbyButton::lipHeight() const
{
    if (!isEnabled() || m_kind == Kind::Ghost || m_kind == Kind::Danger)
        return 0;
    return m_size == Size::Small ? 2 : 3;
}

QSize LobbyButton::sizeHint() const
{
    const int height = m_size == Size::Small ? 28 : m_size == Size::Medium ? 34 : 44;
    const int padding = m_size == Size::Small ? 8 : m_size == Size::Medium ? 12 : 16;
    const int iconSide = m_size == Size::Small ? 14 : 16;

    int width = QFontMetrics(labelFont()).horizontalAdvance(labelText());
    if (!m_subText.isEmpty())
        width = std::max(width, QFontMetrics(LobbyTheme::uiFont(10, QFont::DemiBold)).horizontalAdvance(m_subText));
    if (!m_iconName.isEmpty())
        width += iconSide + (text().isEmpty() ? 0 : (m_size == Size::Small ? 5 : 8));
    return QSize(width + padding * 2 + 2, height);
}

QSize LobbyButton::minimumSizeHint() const
{
    return sizeHint();
}

void LobbyButton::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Tokens& t = LobbyTheme::tokens();
    const bool enabled = isEnabled();
    const bool hover = enabled && underMouse();
    const bool down = isDown() || isChecked();
    const int lip = lipHeight();
    const qreal radius = m_size == Size::Small ? 7.0 : m_size == Size::Medium ? 8.0 : 10.0;

    const QRectF outer = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QRectF face = outer;
    face.setBottom(outer.bottom() - lip);
    if (down && lip > 0)
        face.translate(0, lip - 1);

    QColor top, bottom, border, lipColor, ink;
    bool highlight = false;
    if (!enabled && m_dashed)
    {
        top = bottom = QColor(Qt::transparent);
        border = t.line2;
        ink = t.muted;
    }
    else if (!enabled)
    {
        top = bottom = t.panel2;
        border = t.line;
        ink = t.faint;
    }
    else
    {
        switch (m_kind)
        {
            case Kind::Primary:
                top = hover ? t.accentHi.lighter(108) : t.accentHi;
                bottom = hover ? t.accent.lighter(108) : t.accent;
                border = t.accentLo;
                lipColor = t.accentLip;
                ink = Qt::white;
                highlight = true;
                break;
            case Kind::Secondary:
                top = hover ? t.secondaryA.lighter(t.dark ? 118 : 102) : t.secondaryA;
                bottom = hover ? t.secondaryB.lighter(t.dark ? 118 : 102) : t.secondaryB;
                border = t.line2;
                lipColor = t.secondaryLip;
                ink = t.ink;
                highlight = true;
                break;
            case Kind::Ghost:
                top = bottom = hover ? t.tint : QColor(Qt::transparent);
                border = t.line2;
                ink = t.ink2;
                break;
            case Kind::Danger:
                top = bottom = hover ? LobbyTheme::mix(t.bad, QColor(Qt::transparent), 0.12) : QColor(Qt::transparent);
                border = LobbyTheme::mix(t.bad, QColor(Qt::transparent), 0.55);
                ink = t.bad;
                break;
        }
    }

    if (lip > 0)
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(lipColor);
        painter.drawRoundedRect(outer, radius, radius);
    }

    QLinearGradient fill(face.topLeft(), face.bottomLeft());
    fill.setColorAt(0, top);
    fill.setColorAt(1, bottom);
    painter.setBrush(fill);
    painter.setPen(QPen(border, 1.0, !enabled && m_dashed ? Qt::DashLine : Qt::SolidLine));
    painter.drawRoundedRect(face, radius, radius);

    if (highlight)
    {
        QColor shine(255, 255, 255, m_kind == Kind::Primary ? 66 : 14);
        painter.setPen(QPen(shine, 1.0));
        painter.drawLine(QPointF(face.left() + radius, face.top() + 1.0),
                         QPointF(face.right() - radius, face.top() + 1.0));
    }

    if (hasFocus() && window()->testAttribute(Qt::WA_KeyboardFocusChange))
    {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(t.ink2, 1.0, Qt::DotLine));
        painter.drawRoundedRect(face.adjusted(2, 2, -2, -2), radius - 2, radius - 2);
    }

    // Content is centred on the face, so it travels with a press.
    const int iconSide = m_size == Size::Small ? 14 : 16;
    const int gap = text().isEmpty() ? 0 : (m_size == Size::Small ? 5 : 8);
    const QFont font = labelFont();
    const QFont subFont = LobbyTheme::uiFont(10, QFont::DemiBold);
    const int padding = m_size == Size::Small ? 6 : 10;
    const int iconWidth = m_iconName.isEmpty() ? 0 : iconSide + gap;
    int textWidth = QFontMetrics(font).horizontalAdvance(labelText());
    if (!m_subText.isEmpty())
        textWidth = std::max(textWidth, QFontMetrics(subFont).horizontalAdvance(m_subText));
    textWidth = std::min(textWidth, std::max(0, static_cast<int>(face.width()) - padding * 2 - iconWidth));

    const qreal x = face.left() + (face.width() - iconWidth - textWidth) / 2.0;
    if (!m_iconName.isEmpty())
    {
        const QPixmap pixmap = LobbyTheme::icon(m_iconName, ink, iconSide, devicePixelRatioF());
        painter.drawPixmap(QPointF(x, face.center().y() - iconSide / 2.0), pixmap);
    }

    const QRectF textArea(x + iconWidth, face.top(), textWidth + 1, face.height());
    painter.setPen(ink);
    painter.setFont(font);
    if (m_subText.isEmpty())
    {
        const QString label = QFontMetrics(font).elidedText(labelText(), Qt::ElideRight, textWidth + 1);
        painter.drawText(textArea, Qt::AlignLeft | Qt::AlignVCenter, label);
    }
    else
    {
        const int labelHeight = QFontMetrics(font).height();
        const int subHeight = QFontMetrics(subFont).height();
        const qreal y = face.top() + (face.height() - labelHeight - subHeight) / 2.0;
        painter.drawText(QRectF(textArea.left(), y, textArea.width(), labelHeight),
                         Qt::AlignLeft | Qt::AlignVCenter, labelText());
        painter.setFont(subFont);
        QColor subInk = ink;
        subInk.setAlphaF(0.85);
        painter.setPen(subInk);
        painter.drawText(QRectF(textArea.left(), y + labelHeight, textArea.width(), subHeight),
                         Qt::AlignLeft | Qt::AlignVCenter, m_subText);
    }
}

QSize LayoutButton::sizeHint() const
{
    return layout() ? layout()->totalSizeHint() : QPushButton::sizeHint();
}

QSize LayoutButton::minimumSizeHint() const
{
    return sizeHint();
}

// ── IconButton ───────────────────────────────────────────────────────

IconButton::IconButton(const QString& iconName, int side, QWidget* parent)
    : QToolButton(parent), m_iconName(iconName)
{
    setFixedSize(side, side);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover);
}

void IconButton::setIconName(const QString& name)
{
    m_iconName = name;
    update();
}

void IconButton::setIconColorOverride(const QColor& color)
{
    m_override = color;
    update();
}

void IconButton::setBoxed(bool boxed)
{
    m_boxed = boxed;
    update();
}

void IconButton::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Tokens& t = LobbyTheme::tokens();
    const bool hover = isEnabled() && underMouse();

    if (m_boxed)
    {
        painter.setPen(QPen(t.line2, 1.0));
        painter.setBrush(hover ? t.raise : t.panel2);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 7, 7);
    }
    else if (isChecked() || hover || isDown())
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(isChecked() ? t.accentTint : t.raise);
        painter.drawRoundedRect(QRectF(rect()), 6, 6);
    }

    QColor color = m_override.isValid() ? m_override
        : !isEnabled() ? t.faint
        : isChecked() ? t.accentInk
        : hover ? t.ink : t.muted;
    const int side = m_boxed ? 15 : std::max(10, width() - 10);
    const QPixmap pixmap = LobbyTheme::icon(m_iconName, color, side, devicePixelRatioF());
    painter.drawPixmap(QPointF((width() - side) / 2.0, (height() - side) / 2.0), pixmap);
}

// ── IconLabel ────────────────────────────────────────────────────────

IconLabel::IconLabel(const QString& iconName, Role role, int side, QWidget* parent)
    : QWidget(parent), m_iconName(iconName), m_role(role)
{
    setFixedSize(side, side);
}

void IconLabel::setIconName(const QString& name)
{
    m_iconName = name;
    update();
}

void IconLabel::setRole(Role role)
{
    m_role = role;
    m_override = QColor();
    update();
}

void IconLabel::setColorOverride(const QColor& color)
{
    m_override = color;
    update();
}

void IconLabel::paintEvent(QPaintEvent*)
{
    if (m_iconName.isEmpty())
        return;
    QPainter painter(this);
    const QColor color = m_override.isValid() ? m_override : LobbyTheme::roleColor(m_role);
    painter.drawPixmap(0, 0, LobbyTheme::icon(m_iconName, color, width(), devicePixelRatioF()));
}

// ── Backgrounds ──────────────────────────────────────────────────────

void LobbyBackdrop::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    const Tokens& t = LobbyTheme::tokens();
    painter.fillRect(rect(), t.bg);

    QColor clear = t.glowA;
    clear.setAlpha(0);
    QRadialGradient red(QPointF(width() * 0.08, -40), 620);
    red.setColorAt(0, t.glowA);
    red.setColorAt(1, clear);
    painter.fillRect(rect(), red);

    clear = t.glowB;
    clear.setAlpha(0);
    QRadialGradient gold(QPointF(width(), height() + 40), 520);
    gold.setColorAt(0, t.glowB);
    gold.setColorAt(1, clear);
    painter.fillRect(rect(), gold);
}

LobbyCard::LobbyCard(QWidget* parent, bool scanlines)
    : QFrame(parent), m_scanlines(scanlines)
{
}

void LobbyCard::setBorderColor(const QColor& color)
{
    m_borderColor = color;
    update();
}

void LobbyCard::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Tokens& t = LobbyTheme::tokens();
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const QPainterPath path = roundedPath(box, 12);

    QLinearGradient fill(box.topLeft(), box.bottomLeft());
    fill.setColorAt(0, t.panel);
    fill.setColorAt(1, t.panelB);
    painter.fillPath(path, fill);

    if (m_scanlines)
    {
        painter.save();
        painter.setClipPath(path);
        LobbyTheme::paintScanlines(painter, box, t.scan);
        painter.restore();
    }

    painter.setPen(QPen(m_borderColor.isValid() ? m_borderColor : t.line, 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
}

void LobbyBand::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    const Tokens& t = LobbyTheme::tokens();
    QLinearGradient fill(0, 0, 0, height());
    fill.setColorAt(0, t.bandA);
    fill.setColorAt(1, t.bandB);
    painter.fillRect(rect(), fill);
    LobbyTheme::paintScanlines(painter, QRectF(rect()), t.scan);
}

PortStripe::PortStripe(QWidget* parent)
    : QWidget(parent)
{
    setFixedHeight(2);
}

void PortStripe::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    const qreal w = width() / 4.0;
    for (int i = 0; i < 4; ++i)
        painter.fillRect(QRectF(i * w, 0, w + 1, height()), LobbyTheme::portColor(i + 1));
}

BrandMark::BrandMark(QWidget* parent)
    : QWidget(parent)
{
    setFixedSize(18, 18);
}

void BrandMark::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    for (int i = 0; i < 4; ++i)
    {
        painter.setBrush(LobbyTheme::portColor(i + 1));
        painter.drawRoundedRect(QRectF((i % 2) * 10, (i / 2) * 10, 8, 8), 2, 2);
    }
}

// ── Small status pieces ──────────────────────────────────────────────

LedDot::LedDot(QWidget* parent)
    : QWidget(parent)
{
    setFixedSize(16, 16);
}

void LedDot::setColor(const QColor& color)
{
    m_color = color;
    update();
}

void LedDot::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    const QPointF center(width() / 2.0, height() / 2.0);
    QColor halo = m_color.isValid() ? m_color : LobbyTheme::tokens().muted;
    halo.setAlphaF(0.2);
    painter.setBrush(halo);
    painter.drawEllipse(center, 7.0, 7.0);
    painter.setBrush(m_color.isValid() ? m_color : LobbyTheme::tokens().muted);
    painter.drawEllipse(center, 4.0, 4.0);
}

Spinner::Spinner(int side, QWidget* parent)
    : QWidget(parent), m_timer(new QTimer(this))
{
    setFixedSize(side, side);
    m_timer->setInterval(70);
    connect(m_timer, &QTimer::timeout, this, [this]() {
        m_angle = (m_angle + 40) % 360;
        update();
    });
}

void Spinner::setColors(const QColor& track, const QColor& head)
{
    m_track = track;
    m_head = head;
    update();
}

void Spinner::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    m_timer->start();
}

void Spinner::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    m_timer->stop();
}

void Spinner::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Tokens& t = LobbyTheme::tokens();
    const qreal stroke = std::max(2.5, width() / 8.0);
    const QRectF ring = QRectF(rect()).adjusted(stroke, stroke, -stroke, -stroke);
    painter.setPen(QPen(m_track.isValid() ? m_track : t.warnTint, stroke));
    painter.drawEllipse(ring);
    painter.setPen(QPen(m_head.isValid() ? m_head : t.warn, stroke, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(ring, -m_angle * 16, 90 * 16);
}

UserAvatar::UserAvatar(QWidget* parent)
    : QWidget(parent)
{
    setFixedSize(28, 28);
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

void UserAvatar::setInitial(const QString& initial)
{
    m_initial = initial;
    update();
}

void UserAvatar::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Tokens& t = LobbyTheme::tokens();
    QLinearGradient fill(0, 0, width(), height());
    fill.setColorAt(0, t.accentHi);
    fill.setColorAt(1, t.accentLo);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(QRectF(rect()), 7, 7);
    painter.setFont(LobbyTheme::pixelFont(13));
    painter.setPen(Qt::white);
    painter.drawText(rect(), Qt::AlignCenter, m_initial);
}

IconBadge::IconBadge(int side, qreal radius, int iconSide, QWidget* parent)
    : QWidget(parent), m_radius(radius), m_iconSide(iconSide)
{
    setFixedSize(side, side);
}

void IconBadge::setIconAndColors(const QString& icon, const QColor& ink, const QColor& fill)
{
    m_icon = icon;
    m_ink = ink;
    m_fill = fill;
    update();
}

void IconBadge::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(m_fill);
    painter.drawRoundedRect(QRectF(rect()), m_radius, m_radius);
    if (!m_icon.isEmpty())
        painter.drawPixmap(QPointF((width() - m_iconSide) / 2.0, (height() - m_iconSide) / 2.0),
                           LobbyTheme::icon(m_icon, m_ink, m_iconSide, devicePixelRatioF()));
}

SectionMarker::SectionMarker(Kind kind, QWidget* parent)
    : QWidget(parent), m_kind(kind)
{
    setFixedSize(10, 10);
}

void SectionMarker::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    const Tokens& t = LobbyTheme::tokens();
    if (m_kind == Kind::OnAir)
    {
        QColor halo = t.live;
        halo.setAlphaF(0.25);
        painter.setBrush(halo);
        painter.drawEllipse(QRectF(0, 0, 10, 10));
        painter.setBrush(t.live);
        painter.drawEllipse(QRectF(2, 2, 6, 6));
    }
    else
    {
        painter.setBrush(t.accent);
        painter.drawRoundedRect(QRectF(2, 2, 6, 6), 1, 1);
    }
}

QLabel* UserInterface::Dialog::makeSectionLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text.toUpper(), parent);
    label->setFont(LobbyTheme::pixelFont(11, 1.0));
    label->setProperty("role", "ink2");
    return label;
}

StatusTag::StatusTag(Kind kind, QWidget* parent)
    : QWidget(parent), m_kind(kind)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void StatusTag::setTag(Kind kind, const QString& text)
{
    m_kind = kind;
    m_text = text;
    updateGeometry();
    update();
}

QFont StatusTag::tagFont() const
{
    QFont font = LobbyTheme::uiFont(9, QFont::Bold);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
    return font;
}

QSize StatusTag::sizeHint() const
{
    const QFontMetrics fm(tagFont());
    const int dot = m_kind == Kind::Live ? 7 : 0;
    return QSize(fm.horizontalAdvance(m_text.toUpper()) + 10 + dot, fm.height() + 3);
}

void StatusTag::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Tokens& t = LobbyTheme::tokens();

    QColor fill, ink;
    switch (m_kind)
    {
        case Kind::Neutral: fill = t.raise;      ink = t.muted;      break;
        case Kind::Red:     fill = t.liveTint;   ink = t.live;       break;
        case Kind::Green:   fill = t.okTint;     ink = t.ok;         break;
        case Kind::Amber:   fill = t.warnTint;   ink = t.warn;       break;
        case Kind::Purple:  fill = t.watchTint;  ink = t.stateWatch; break;
        case Kind::Live:    fill = t.liveBg;     ink = Qt::white;    break;
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(QRectF(rect()), 4, 4);

    int x = 5;
    if (m_kind == Kind::Live)
    {
        painter.setBrush(Qt::white);
        painter.drawEllipse(QPointF(x + 2, height() / 2.0), 2, 2);
        x += 7;
    }
    painter.setFont(tagFont());
    painter.setPen(ink);
    painter.drawText(QRect(x, 0, width() - x, height()), Qt::AlignLeft | Qt::AlignVCenter, m_text.toUpper());
}

PortChip::PortChip(int slot, QWidget* parent)
    : QWidget(parent), m_slot(slot)
{
    setFixedSize(19, 15);
}

void PortChip::setSlot(int slot)
{
    m_slot = slot;
    update();
}

void PortChip::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(LobbyTheme::portColor(m_slot));
    painter.drawRoundedRect(QRectF(rect()), 3, 3);
    painter.setFont(LobbyTheme::uiFont(9, QFont::Black));
    painter.setPen(inkOnPort(m_slot));
    painter.drawText(rect(), Qt::AlignCenter, QString("P%1").arg(m_slot));
}

SignalBars::SignalBars(QWidget* parent)
    : QWidget(parent)
{
    setFixedSize(13, 12);
}

void SignalBars::setPing(int ms, bool estimate)
{
    m_ms = ms;
    m_estimate = estimate;
    update();
}

void SignalBars::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    LobbyTheme::paintPingBars(painter, QPointF(0, height()), m_ms, m_estimate);
}

SeatPips::SeatPips(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void SeatPips::setSeats(int filled, int total)
{
    m_filled = filled;
    m_total = total;
    setFixedSize(std::max(0, total * 12 - 3), 10);
    update();
}

void SeatPips::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    for (int i = 0; i < m_total; ++i)
    {
        const QRectF pip(i * 12 + 0.75, 0.75, 8.5, 8.5);
        if (i < m_filled)
        {
            painter.setPen(Qt::NoPen);
            painter.setBrush(LobbyTheme::portColor(i + 1));
        }
        else
        {
            painter.setPen(QPen(LobbyTheme::tokens().line2, 1.5));
            painter.setBrush(Qt::NoBrush);
        }
        painter.drawRoundedRect(pip, 2, 2);
    }
}

CountryFlag::CountryFlag(QWidget* parent)
    : QWidget(parent)
{
    setFixedSize(15, 10);
    setVisible(false);
}

void CountryFlag::setCountry(const QString& country)
{
    m_country = country;
    setVisible(!LobbyTheme::flag(country, devicePixelRatioF()).isNull());
    update();
}

void CountryFlag::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.drawPixmap(0, 0, LobbyTheme::flag(m_country, devicePixelRatioF()));
}

ConnectionGlyph::ConnectionGlyph(QWidget* parent)
    : QWidget(parent)
{
    setFixedSize(16, 14);
}

void ConnectionGlyph::setConnection(const QString& connection)
{
    m_connection = connection;
    update();
}

QString ConnectionGlyph::displayName(const QString& connection)
{
    if (connection == QLatin1String("wifi"))      return QCoreApplication::translate("RollbackLobbyDialog", "Wi-Fi");
    if (connection == QLatin1String("lan"))       return QCoreApplication::translate("RollbackLobbyDialog", "Ethernet");
    if (connection == QLatin1String("cellular"))  return QCoreApplication::translate("RollbackLobbyDialog", "Cellular");
    if (connection == QLatin1String("bluetooth")) return QCoreApplication::translate("RollbackLobbyDialog", "Bluetooth");
    return QString();
}

void ConnectionGlyph::paintSymbol(QPainter& painter, const QRectF& rect, const QString& connection, const QColor& color)
{
    const QPointF center = rect.center();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);

    if (connection == QLatin1String("wifi"))
    {
        // Two arcs and a dot.
        const qreal baseY = center.y() + 4.0;
        painter.drawArc(QRectF(center.x() - 7.5, baseY - 7.5, 15.0, 15.0), 45 * 16, 90 * 16);
        painter.drawArc(QRectF(center.x() - 4.5, baseY - 4.5, 9.0, 9.0), 45 * 16, 90 * 16);
        painter.setBrush(color);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QPointF(center.x(), baseY), 1.4, 1.4);
    }
    else if (connection == QLatin1String("lan"))
    {
        // An RJ45 plug: connector body, tab, and four pins.
        QPainterPath plug;
        plug.moveTo(center.x() - 5.5, center.y() - 5.0);
        plug.lineTo(center.x() + 5.5, center.y() - 5.0);
        plug.lineTo(center.x() + 5.5, center.y() + 2.0);
        plug.lineTo(center.x() + 2.5, center.y() + 2.0);
        plug.lineTo(center.x() + 2.5, center.y() + 5.5);
        plug.lineTo(center.x() - 2.5, center.y() + 5.5);
        plug.lineTo(center.x() - 2.5, center.y() + 2.0);
        plug.lineTo(center.x() - 5.5, center.y() + 2.0);
        plug.closeSubpath();
        painter.drawPath(plug);
        painter.setPen(QPen(color, 1.0));
        for (int i = -3; i <= 3; i += 2)
            painter.drawLine(QPointF(center.x() + i, center.y() - 3.5), QPointF(center.x() + i, center.y() - 1.0));
    }
    else if (!connection.isEmpty())
    {
        painter.setFont(LobbyTheme::uiFont(9, QFont::DemiBold));
        painter.setPen(color);
        painter.drawText(rect, Qt::AlignCenter,
            connection == QLatin1String("cellular") ? QStringLiteral("Cell")
            : connection == QLatin1String("bluetooth") ? QStringLiteral("BT")
            : QStringLiteral("?"));
    }
    painter.restore();
}

void ConnectionGlyph::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    paintSymbol(painter, QRectF(rect()), m_connection, LobbyTheme::tokens().muted);
}

// ── CartridgeTile ────────────────────────────────────────────────────

CartridgeTile::CartridgeTile(Size size, QWidget* parent)
    : QWidget(parent), m_size(Size::Small)
{
    setTileSize(size);
}

void CartridgeTile::setGame(const QString& gameName, const QString& coverPath)
{
    if (m_game == gameName && m_coverPath == coverPath)
        return;
    m_game = gameName;
    m_coverPath = coverPath;
    update();
}

void CartridgeTile::setTileSize(Size size)
{
    m_size = size;
    switch (size)
    {
        case Size::Small:  setFixedSize(30, 38); break;
        case Size::Normal: setFixedSize(46, 60); break;
        case Size::Large:  setFixedSize(58, 76); break;
    }
    update();
}

void CartridgeTile::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const Tokens& t = LobbyTheme::tokens();

    const qreal topRadius = m_size == Size::Small ? 4 : 6;
    const qreal bottomRadius = m_size == Size::Small ? 2 : 3;
    const QRectF body = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -1.5);
    const QPainterPath shell = cartridgePath(body, topRadius, bottomRadius);

    const QPixmap cover = m_coverPath.isEmpty() ? QPixmap() : QPixmap(m_coverPath);
    if (!cover.isNull())
    {
        // The art fills the tile, cropped to its shape.
        painter.save();
        painter.setClipPath(shell);
        const QSizeF scaled = QSizeF(cover.size()).scaled(body.size(), Qt::KeepAspectRatioByExpanding);
        const QRectF target(body.center().x() - scaled.width() / 2, body.center().y() - scaled.height() / 2,
                            scaled.width(), scaled.height());
        painter.drawPixmap(target, cover, QRectF(cover.rect()));
        painter.restore();
        painter.setPen(QPen(QColor(0, 0, 0, 90), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(shell);
        return;
    }

    QLinearGradient plastic(body.topLeft(), body.bottomLeft());
    plastic.setColorAt(0, t.cartA);
    plastic.setColorAt(1, t.cartB);
    painter.fillPath(shell, plastic);

    painter.save();
    painter.setClipPath(shell);
    painter.setPen(QPen(QColor(255, 255, 255, 36), 1.0));
    painter.drawLine(QPointF(body.left() + topRadius, body.top() + 0.5), QPointF(body.right() - topRadius, body.top() + 0.5));
    painter.fillRect(QRectF(body.left(), body.bottom() - 3, body.width(), 3), QColor(0, 0, 0, 70));

    // The grip's ridges.
    const qreal inset = m_size == Size::Small ? 4 : m_size == Size::Normal ? 6 : 8;
    const qreal gripTop = m_size == Size::Small ? 3 : m_size == Size::Normal ? 4 : 6;
    const qreal gripHeight = m_size == Size::Small ? 4 : m_size == Size::Normal ? 6 : 8;
    for (qreal x = body.left() + inset; x < body.right() - inset; x += 5)
        painter.fillRect(QRectF(x, body.top() + gripTop, 2, gripHeight), QColor(0, 0, 0, 86));
    painter.restore();

    const QRectF label = m_size == Size::Small
        ? QRectF(body.left() + 3, body.top() + 9, body.width() - 6, body.height() - 14)
        : m_size == Size::Normal
            ? QRectF(body.left() + 5, body.top() + 13, body.width() - 10, body.height() - 20)
            : QRectF(body.left() + 6, body.top() + 17, body.width() - 12, body.height() - 26);
    const QColor hue = LobbyTheme::gameHue(m_game);
    QLinearGradient labelFill(label.topLeft(), label.bottomRight());
    labelFill.setColorAt(0, LobbyTheme::mix(hue, Qt::white, 0.88));
    labelFill.setColorAt(1, LobbyTheme::mix(hue, Qt::black, 0.72));
    painter.setBrush(labelFill);
    painter.setPen(QPen(QColor(0, 0, 0, 46), 1.0));
    painter.drawRoundedRect(label, 2, 2);

    const QString initials = LobbyTheme::gameInitials(m_game);
    int px = m_size == Size::Small ? 9 : m_size == Size::Normal ? 12 : 16;
    if (initials.size() > 2)
        px -= m_size == Size::Small ? 2 : 3;
    painter.setFont(LobbyTheme::pixelFont(px));
    painter.setPen(Qt::white);
    painter.drawText(label, Qt::AlignCenter, initials);
}

// ── FrameStepper ─────────────────────────────────────────────────────

FrameStepper::FrameStepper(int minimum, int maximum, const QString& iconName, QWidget* parent)
    : QWidget(parent), m_minimum(minimum), m_maximum(maximum), m_value(minimum)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(7);

    m_minus = new IconButton(QStringLiteral("subtract-line"), 28, this);
    m_minus->setObjectName("StepperMinus");
    m_plus = new IconButton(QStringLiteral("add-line"), 28, this);
    m_plus->setObjectName("StepperPlus");
    m_minus->setAccessibleName(QStringLiteral("-"));
    m_plus->setAccessibleName(QStringLiteral("+"));
    m_minus->setBoxed(true);
    m_plus->setBoxed(true);

    m_valueLabel = new QLabel(this);
    m_valueLabel->setFont(LobbyTheme::pixelFont(20));
    m_valueLabel->setAlignment(Qt::AlignCenter);
    m_valueLabel->setMinimumWidth(42);
    m_valueLabel->setTextFormat(Qt::RichText);

    m_chip = new QPushButton(this);
    m_chip->setObjectName("StepperChip");
    m_chip->setCursor(Qt::PointingHandCursor);
    m_chip->setFocusPolicy(Qt::NoFocus);
    m_chip->setAutoDefault(false);
    m_chip->setFont(LobbyTheme::uiFont(10, QFont::Bold));

    layout->addWidget(m_minus);
    layout->addWidget(m_valueLabel);
    layout->addWidget(m_plus);
    layout->addSpacing(2);
    layout->addWidget(m_chip);
    layout->addStretch(1);

    m_iconName = iconName;

    connect(m_minus, &QToolButton::clicked, this, [this]() { step(-1); });
    connect(m_plus, &QToolButton::clicked, this, [this]() { step(1); });
    connect(m_chip, &QPushButton::clicked, this, [this]() {
        // The chip always offers the automatic value, and Auto can also be
        // switched off to pin what it resolved to.
        if (m_automatic && !m_chipAccented)
            return;
        m_automatic = !m_automatic;
        refresh();
        emit edited();
    });
    refresh();
}

void FrameStepper::step(int delta)
{
    const int next = std::clamp(m_value + delta, m_minimum, m_maximum);
    if (next == m_value && !m_automatic)
        return;
    m_value = next;
    m_automatic = false;
    refresh();
    emit edited();
}

void FrameStepper::setValue(int value)
{
    m_value = std::clamp(value, m_minimum, m_maximum);
    refresh();
}

void FrameStepper::setAutomatic(bool automatic)
{
    m_automatic = automatic;
    refresh();
}

void FrameStepper::setChipLabels(const QString& automaticLabel, const QString& manualLabel)
{
    m_automaticLabel = automaticLabel;
    m_manualLabel = manualLabel;
    refresh();
}

void FrameStepper::setChipAccented(bool accented)
{
    m_chipAccented = accented;
    refresh();
}

void FrameStepper::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::EnabledChange || event->type() == QEvent::ApplicationPaletteChange)
        refresh();
}

void FrameStepper::refresh()
{
    const Tokens& t = LobbyTheme::tokens();
    const QColor ink = isEnabled() ? t.ink : t.faint;
    const QColor unit = isEnabled() ? t.muted : t.faint;
    m_valueLabel->setText(QString("<span style='color:%1;'>%2</span>"
                                  "<span style='font-size:12px; color:%3;'>F</span>")
                              .arg(ink.name()).arg(m_value).arg(unit.name()));
    m_chip->setText((m_automatic ? m_automaticLabel : m_manualLabel).toUpper());
    const bool lit = m_automatic && m_chipAccented && isEnabled();
    if (!m_iconName.isEmpty())
        m_chip->setIcon(QIcon(LobbyTheme::icon(m_iconName, lit ? t.accentInk : t.muted, 11, devicePixelRatioF())));
    m_chip->setStyleSheet(QString(
        "QPushButton#StepperChip { min-height: 20px; max-height: 20px; padding: 0 7px; border-radius: 5px;"
        " background-color: %1; color: %2; border: 1px solid %3; }")
        .arg(LobbyTheme::css(lit ? t.accentTint : QColor(Qt::transparent)),
             LobbyTheme::css(lit ? t.accentInk : (isEnabled() ? t.muted : t.faint)),
             LobbyTheme::css(lit ? LobbyTheme::mix(t.accent, QColor(Qt::transparent), 0.4) : t.line2)));
}

// ── ToggleSwitch ─────────────────────────────────────────────────────

ToggleSwitch::ToggleSwitch(const QString& title, QWidget* parent)
    : QCheckBox(title, parent)
{
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

void ToggleSwitch::setSubText(const QString& text)
{
    if (m_subText == text)
        return;
    m_subText = text;
    updateGeometry();
    update();
}

QSize ToggleSwitch::sizeHint() const
{
    const QFontMetrics title(LobbyTheme::uiFont(12));
    const QFontMetrics sub(LobbyTheme::uiFont(11));
    const int width = std::max(title.horizontalAdvance(text()), sub.horizontalAdvance(m_subText));
    return QSize(32 + 9 + width + 1, title.height() + (m_subText.isEmpty() ? 0 : sub.height()));
}

QSize ToggleSwitch::minimumSizeHint() const
{
    return QSize(100, sizeHint().height());
}

bool ToggleSwitch::hitButton(const QPoint& pos) const
{
    return rect().contains(pos);
}

void ToggleSwitch::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Tokens& t = LobbyTheme::tokens();

    const bool on = isChecked();
    const qreal opacity = isEnabled() ? 1.0 : 0.45;
    const QRectF track(0, (height() - 18) / 2.0, 32, 18);
    painter.save();
    painter.setOpacity(opacity);
    painter.setPen(Qt::NoPen);
    painter.setBrush(on ? t.ok : t.line2);
    painter.drawRoundedRect(track, 9, 9);
    painter.setBrush(on ? QColor(Qt::white) : t.panel);
    painter.drawEllipse(QPointF(track.left() + (on ? 23 : 9), track.center().y()), 6, 6);
    painter.restore();

    const int x = 41;
    const QFont titleFont = LobbyTheme::uiFont(12);
    const QFont subFont = LobbyTheme::uiFont(11);
    const QFontMetrics title(titleFont);
    const QFontMetrics sub(subFont);
    const int blockHeight = title.height() + (m_subText.isEmpty() ? 0 : sub.height());
    int y = (height() - blockHeight) / 2;
    painter.setFont(titleFont);
    painter.setPen(isEnabled() ? t.ink2 : t.muted);
    painter.drawText(QRect(x, y, width() - x, title.height()), Qt::AlignLeft | Qt::AlignVCenter,
                     title.elidedText(text(), Qt::ElideRight, width() - x));
    if (!m_subText.isEmpty())
    {
        y += title.height();
        painter.setFont(subFont);
        painter.setPen(t.muted);
        painter.drawText(QRect(x, y, width() - x, sub.height()), Qt::AlignLeft | Qt::AlignVCenter,
                         sub.elidedText(m_subText, Qt::ElideRight, width() - x));
    }
}

// ── StatePill ────────────────────────────────────────────────────────

StatePill::StatePill(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void StatePill::setState(const QString& text, const QColor& color)
{
    m_text = text;
    m_color = color;
    updateGeometry();
    update();
}

QSize StatePill::sizeHint() const
{
    const QFontMetrics fm(LobbyTheme::pixelFont(10, 0.7));
    return QSize(fm.horizontalAdvance(m_text.toUpper()) + 11 + 8 + 7 + 11, 28);
}

void StatePill::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor color = m_color.isValid() ? m_color : LobbyTheme::tokens().muted;
    QColor fill = color;
    fill.setAlphaF(LobbyTheme::tokens().dark ? 0.15 : 0.11);
    QColor border = color;
    border.setAlphaF(0.36);
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(border, 1.0));
    painter.setBrush(fill);
    painter.drawRoundedRect(box, box.height() / 2, box.height() / 2);

    painter.setPen(Qt::NoPen);
    QColor halo = color;
    halo.setAlphaF(0.25);
    painter.setBrush(halo);
    painter.drawEllipse(QPointF(15, height() / 2.0), 6, 6);
    painter.setBrush(color);
    painter.drawEllipse(QPointF(15, height() / 2.0), 3.5, 3.5);

    painter.setFont(LobbyTheme::pixelFont(10, 0.7));
    painter.setPen(color);
    painter.drawText(QRect(26, 0, width() - 26, height()), Qt::AlignLeft | Qt::AlignVCenter, m_text.toUpper());
}

// ── PortCard ─────────────────────────────────────────────────────────

// The player's initial on a tile in the port's colour.
class PortCard::Portrait : public QWidget
{
public:
    explicit Portrait(int slot, QWidget* parent)
        : QWidget(parent), m_slot(slot)
    {
        setFixedSize(56, 56);
        m_spinner = new Spinner(26, this);
        m_spinner->move(15, 15);
        m_spinner->setColors(QColor(0, 0, 0, 62), Qt::white);
        m_spinner->setVisible(false);
    }

    void setInitial(const QString& initial)
    {
        m_initial = initial;
        update();
    }

    void setBusy(bool busy, bool muted)
    {
        m_spinner->setVisible(busy);
        m_muted = muted;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QColor base = LobbyTheme::portColor(m_slot);
        if (m_muted)
            base = LobbyTheme::mix(base, LobbyTheme::tokens().stateRoom, 0.3);
        const QRectF box = QRectF(rect());
        const QPainterPath path = roundedPath(box, 12);
        QLinearGradient fill(box.topLeft(), box.bottomRight());
        fill.setColorAt(0, LobbyTheme::mix(base, Qt::white, 0.85));
        fill.setColorAt(1, LobbyTheme::mix(base, Qt::black, 0.55));
        painter.fillPath(path, fill);

        painter.save();
        painter.setClipPath(path);
        LobbyTheme::paintScanlines(painter, box, QColor(0, 0, 0, 28));
        painter.fillRect(QRectF(box.left(), box.bottom() - 4, box.width(), 4), QColor(0, 0, 0, 56));
        painter.restore();

        if (m_spinner->isVisible())
            return;
        painter.setFont(LobbyTheme::pixelFont(26));
        painter.setPen(m_muted ? QColor(Qt::white) : inkOnPort(m_slot));
        painter.drawText(rect(), Qt::AlignCenter, m_initial);
    }

private:
    int m_slot;
    bool m_muted = false;
    QString m_initial;
    Spinner* m_spinner;
};

// A small labelled number at the bottom of a port card.
class PortCard::StatBox : public QWidget
{
public:
    explicit StatBox(const QString& title, QWidget* parent)
        : QWidget(parent), m_title(title)
    {
        setFixedHeight(44);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }

    // Numbers and short codes use the pixel font; words don't, so they fit.
    void setValue(const QString& value, const QString& unit, Role role, bool pixel = true)
    {
        m_value = value;
        m_unit = unit;
        m_role = role;
        m_pixel = pixel;
        m_ping = -2;
        update();
    }

    void setPing(int ms)
    {
        m_value = QString::number(ms);
        m_unit = QStringLiteral("ms");
        m_role = ms <= 60 ? Role::Ok : ms <= 120 ? Role::Warn : Role::Bad;
        m_pixel = true;
        m_ping = ms;
        update();
    }

    void setCentered(bool centered)
    {
        m_centered = centered;
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const Tokens& t = LobbyTheme::tokens();
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(QPen(t.tint, 1.0));
        painter.setBrush(t.stat);
        painter.drawRoundedRect(box, 7, 7);

        const Qt::Alignment horizontal = m_centered ? Qt::AlignHCenter : Qt::AlignLeft;
        QFont titleFont = LobbyTheme::uiFont(9, QFont::DemiBold);
        titleFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
        painter.setFont(titleFont);
        painter.setPen(t.muted);
        painter.drawText(QRect(7, 5, width() - 14, 13), horizontal | Qt::AlignVCenter, m_title.toUpper());
        if (m_ping >= -1)
            LobbyTheme::paintPingBars(painter, QPointF(width() - 20, 17), m_ping, false);

        const QFont valueFont = m_pixel ? LobbyTheme::pixelFont(16) : LobbyTheme::uiFont(11, QFont::Bold);
        painter.setFont(valueFont);
        painter.setPen(LobbyTheme::roleColor(m_role));
        const QFontMetrics fm(valueFont);
        const QRect valueArea(7, 19, width() - 14, 21);
        if (m_centered || m_unit.isEmpty())
        {
            painter.drawText(valueArea, horizontal | Qt::AlignVCenter,
                             fm.elidedText(m_value, Qt::ElideRight, valueArea.width()));
            return;
        }
        painter.drawText(valueArea, Qt::AlignLeft | Qt::AlignVCenter, m_value);
        painter.setFont(LobbyTheme::uiFont(10, QFont::DemiBold));
        painter.setPen(t.muted);
        painter.drawText(valueArea.adjusted(fm.horizontalAdvance(m_value) + 2, 0, 0, 0),
                         Qt::AlignLeft | Qt::AlignVCenter, m_unit);
    }

private:
    QString m_title;
    QString m_value;
    QString m_unit;
    Role m_role = Role::Ink;
    bool m_pixel = true;
    bool m_centered = false;
    int m_ping = -2; // -2: no bars
};

PortCard::PortCard(int slot, QWidget* parent)
    : QFrame(parent), m_slot(slot)
{
    setObjectName("SeatCard");
    setMinimumWidth(116);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // The coloured cap: port number, host badge, and the host's tools.
    m_cap = new QWidget(this);
    m_cap->setFixedHeight(30);
    auto* capLay = new QHBoxLayout(m_cap);
    capLay->setContentsMargins(10, 0, 6, 0);
    capLay->setSpacing(6);
    m_capLabel = new QLabel(QString("P%1").arg(slot), m_cap);
    m_capLabel->setFont(LobbyTheme::pixelFont(13, 0.4));
    capLay->addWidget(m_capLabel);

    m_hostBadge = new QWidget(m_cap);
    auto* badgeLay = new QHBoxLayout(m_hostBadge);
    badgeLay->setContentsMargins(5, 2, 5, 2);
    badgeLay->setSpacing(3);
    auto* crown = new IconLabel(QStringLiteral("vip-crown-fill"), Role::Ink, 11, m_hostBadge);
    crown->setColorOverride(inkOnPort(slot));
    auto* hostText = new QLabel(tr("HOST"), m_hostBadge);
    QFont hostFont = LobbyTheme::uiFont(9, QFont::Bold);
    hostFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.7);
    hostText->setFont(hostFont);
    badgeLay->addWidget(crown);
    badgeLay->addWidget(hostText);
    m_hostBadge->setObjectName("HostBadge");
    m_hostBadge->setAttribute(Qt::WA_StyledBackground, true);
    m_hostBadge->setVisible(false);
    capLay->addWidget(m_hostBadge, 0, Qt::AlignVCenter);
    capLay->addStretch(1);

    m_dragHandle = new IconLabel(QStringLiteral("draggable"), Role::Ink, 14, m_cap);
    m_dragHandle->setColorOverride(inkOnPort(slot));
    m_dragHandle->setCursor(Qt::OpenHandCursor);
    m_dragHandle->setToolTip(tr("Drag onto another seat to swap players"));
    m_dragHandle->setVisible(false);
    capLay->addWidget(m_dragHandle);

    m_kick = new IconButton(QStringLiteral("close-line"), 22, m_cap);
    m_kick->setObjectName("SeatKickButton");
    m_kick->setIconColorOverride(inkOnPort(slot));
    m_kick->setToolTip(tr("Remove this player from the match"));
    m_kick->setAccessibleName(tr("Remove player"));
    m_kick->setVisible(false);
    capLay->addWidget(m_kick);
    root->addWidget(m_cap);

    // Open seat.
    m_openPage = new QWidget(this);
    auto* openLay = new QVBoxLayout(m_openPage);
    openLay->setContentsMargins(10, 10, 10, 10);
    openLay->setSpacing(6);
    openLay->addStretch(1);
    m_openSlot = new QLabel(QString("P%1").arg(slot), m_openPage);
    m_openSlot->setFont(LobbyTheme::pixelFont(18));
    m_openSlot->setAlignment(Qt::AlignCenter);
    m_openSlot->setProperty("role", "faint");
    openLay->addWidget(m_openSlot);
    auto* openWord = new QLabel(tr("OPEN"), m_openPage);
    openWord->setFont(LobbyTheme::pixelFont(10, 0.8));
    openWord->setAlignment(Qt::AlignCenter);
    openWord->setProperty("role", "faint");
    openLay->addWidget(openWord);
    m_openHint = new QLabel(m_openPage);
    m_openHint->setFont(LobbyTheme::uiFont(11));
    m_openHint->setAlignment(Qt::AlignCenter);
    m_openHint->setWordWrap(true);
    m_openHint->setProperty("role", "muted");
    openLay->addWidget(m_openHint);
    openLay->addStretch(1);
    root->addWidget(m_openPage, 1);

    // Filled seat.
    m_playerPage = new QWidget(this);
    auto* playerLay = new QVBoxLayout(m_playerPage);
    playerLay->setContentsMargins(10, 10, 10, 10);
    playerLay->setSpacing(3);
    playerLay->addStretch(1);
    m_portrait = new Portrait(slot, m_playerPage);
    playerLay->addWidget(m_portrait, 0, Qt::AlignHCenter);
    playerLay->addSpacing(5);

    auto* nameRow = new QHBoxLayout;
    nameRow->setSpacing(6);
    nameRow->addStretch(1);
    m_name = new ElidedLabel(Role::Ink, m_playerPage);
    m_name->setFont(LobbyTheme::uiFont(14, QFont::Bold));
    nameRow->addWidget(m_name);
    m_youTag = new StatusTag(StatusTag::Kind::Neutral, m_playerPage);
    m_youTag->setTag(StatusTag::Kind::Neutral, tr("You"));
    nameRow->addWidget(m_youTag);
    m_flag = new CountryFlag(m_playerPage);
    nameRow->addWidget(m_flag);
    nameRow->addStretch(1);
    playerLay->addLayout(nameRow);

    auto* connectionRow = new QHBoxLayout;
    connectionRow->setSpacing(4);
    connectionRow->addStretch(1);
    m_connectionIcon = new ConnectionGlyph(m_playerPage);
    connectionRow->addWidget(m_connectionIcon);
    m_connectionName = new QLabel(m_playerPage);
    m_connectionName->setFont(LobbyTheme::uiFont(11));
    m_connectionName->setProperty("role", "muted");
    connectionRow->addWidget(m_connectionName);
    connectionRow->addStretch(1);
    playerLay->addLayout(connectionRow);

    // How far a connection has got, while it isn't up yet.
    m_progress = new QWidget(m_playerPage);
    m_progress->setFixedHeight(4);
    m_progress->setObjectName("SeatProgress");
    m_progress->setAttribute(Qt::WA_StyledBackground, true);
    m_progress->setVisible(false);
    playerLay->addSpacing(2);
    playerLay->addWidget(m_progress);
    m_progressHint = new QLabel(m_playerPage);
    m_progressHint->setFont(LobbyTheme::uiFont(11));
    m_progressHint->setAlignment(Qt::AlignCenter);
    m_progressHint->setProperty("role", "muted");
    m_progressHint->setVisible(false);
    playerLay->addWidget(m_progressHint);

    m_waitingTag = new StatusTag(StatusTag::Kind::Amber, m_playerPage);
    m_waitingTag->setTag(StatusTag::Kind::Amber, tr("Waiting to join"));
    m_waitingTag->setVisible(false);
    playerLay->addWidget(m_waitingTag, 0, Qt::AlignHCenter);
    playerLay->addStretch(1);

    m_stats = new QWidget(m_playerPage);
    auto* statsLay = new QHBoxLayout(m_stats);
    statsLay->setContentsMargins(0, 0, 0, 0);
    statsLay->setSpacing(6);
    m_linkStat = new StatBox(tr("Link"), m_stats);
    m_delayStat = new StatBox(tr("Delay"), m_stats);
    m_waitingStat = new StatBox(tr("Status"), m_stats);
    m_waitingStat->setCentered(true);
    m_waitingStat->setValue(tr("WAITING"), QString(), Role::Warn, false);
    m_waitingStat->setVisible(false);
    statsLay->addWidget(m_linkStat, 1);
    statsLay->addWidget(m_delayStat, 1);
    statsLay->addWidget(m_waitingStat, 1);
    playerLay->addWidget(m_stats);

    m_admit = new LobbyButton(tr("Add to Game"), LobbyButton::Kind::Primary, m_playerPage);
    m_admit->setObjectName("SeatAdmitButton");
    m_admit->setIconName(QStringLiteral("add-line"));
    m_admit->setButtonSize(LobbyButton::Size::Medium);
    m_admit->setFocusPolicy(Qt::NoFocus);
    m_admit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_admit->setVisible(false);
    playerLay->addWidget(m_admit);
    root->addWidget(m_playerPage, 1);

    showOpen(QString());
}

void PortCard::showOpen(const QString& hint)
{
    m_open = true;
    m_self = false;
    m_connecting = false;
    m_waiting = false;
    m_admitVisible = false;
    m_cap->setVisible(false);
    m_playerPage->setVisible(false);
    m_openPage->setVisible(true);
    m_openHint->setText(hint);
    m_hostBadge->setVisible(false);
    m_dragHandle->setVisible(false);
    m_kick->setVisible(false);
    m_admit->setVisible(false);
    update();
}

void PortCard::showPlayer(const QString& name, bool isSelf, bool isHost, const QString& selfTag)
{
    m_open = false;
    m_self = isSelf;
    m_cap->setVisible(true);
    m_openPage->setVisible(false);
    m_playerPage->setVisible(true);
    m_name->setText(name);
    m_youTag->setTag(StatusTag::Kind::Neutral, selfTag.isEmpty() ? tr("You") : selfTag);
    m_youTag->setVisible(isSelf);
    m_hostBadge->setVisible(isHost);
    m_portrait->setInitial(LobbyTheme::nameInitial(name));

    const QColor capInk = inkOnPort(m_slot);
    m_capLabel->setStyleSheet(QString("color: %1;").arg(capInk.name()));
    m_hostBadge->setStyleSheet(QString(
        "QWidget#HostBadge { background-color: rgba(0,0,0,0.24); border-radius: 4px; }"
        " QLabel { color: %1; }").arg(capInk.name()));
    update();
}

void PortCard::setCountry(const QString& country, const QString& tooltip)
{
    m_flag->setCountry(country);
    m_flag->setToolTip(tooltip);
}

void PortCard::setConnection(const QString& connection)
{
    const QString name = ConnectionGlyph::displayName(connection);
    m_connectionIcon->setConnection(connection);
    m_connectionIcon->setVisible(!connection.isEmpty());
    m_connectionName->setText(name);
    m_connectionName->setVisible(!name.isEmpty());
}

void PortCard::setDelay(int frames)
{
    if (frames >= 0)
        m_delayStat->setValue(QString::number(frames), QStringLiteral("F"), Role::Ink);
    else
        m_delayStat->setValue(QStringLiteral("—"), QString(), Role::Faint);
}

void PortCard::refreshConnecting()
{
    const bool connecting = m_connecting && !m_waiting;
    m_progress->setVisible(connecting);
    m_progressHint->setVisible(connecting);
    m_portrait->setBusy(connecting, connecting || m_waiting);
}

void PortCard::setLinkLocal()
{
    m_connecting = false;
    m_linkStat->setValue(tr("LOCAL"), QString(), Role::Faint);
    refreshConnecting();
}

void PortCard::setLinkPing(int ms)
{
    m_connecting = false;
    if (ms >= 0)
        m_linkStat->setPing(ms);
    else
        m_linkStat->setValue(QStringLiteral("—"), QString(), Role::Faint);
    refreshConnecting();
}

void PortCard::setLinkConnecting(int attempt, int maxAttempts)
{
    m_connecting = true;
    const qreal share = maxAttempts > 0 ? std::clamp(attempt / qreal(maxAttempts), 0.05, 1.0) : 0.05;
    const Tokens& t = LobbyTheme::tokens();
    m_progress->setStyleSheet(QString(
        "QWidget#SeatProgress { border-radius: 2px; background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        " stop:0 %1, stop:%2 %1, stop:%3 %4, stop:1 %4); }")
        .arg(t.warn.name()).arg(share, 0, 'f', 3).arg(std::min(share + 0.001, 1.0), 0, 'f', 3)
        .arg(LobbyTheme::css(t.tint)));
    m_progressHint->setText(attempt > 1 ? tr("Retrying…") : tr("Connecting…"));
    m_linkStat->setValue(QString("%1/%2").arg(attempt).arg(maxAttempts), QString(), Role::Warn);
    refreshConnecting();
}

void PortCard::setLinkStatus(const QString& text, Role role)
{
    m_connecting = false;
    m_linkStat->setValue(text, QString(), role, false);
    m_linkStat->setToolTip(text);
    refreshConnecting();
}

void PortCard::setWaitingToJoin(bool waiting)
{
    m_waiting = waiting;
    m_waitingTag->setVisible(waiting);
    refreshConnecting();
    refreshBottom();
}

void PortCard::setAdmitVisible(bool visible)
{
    m_admitVisible = visible;
    refreshBottom();
}

void PortCard::refreshBottom()
{
    const bool add = m_waiting && m_admitVisible;
    m_admit->setVisible(add);
    m_stats->setVisible(!add);
    m_linkStat->setVisible(!m_waiting);
    m_delayStat->setVisible(!m_waiting);
    m_waitingStat->setVisible(m_waiting);
}

void PortCard::setHostTools(bool dragHandle, bool kick)
{
    m_dragHandle->setVisible(dragHandle);
    m_kick->setVisible(kick);
}

void PortCard::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Tokens& t = LobbyTheme::tokens();
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    if (m_open)
    {
        painter.setPen(QPen(t.line2, 1.5, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(box, 12, 12);
        return;
    }

    const QColor port = LobbyTheme::portColor(m_slot);
    const QPainterPath path = roundedPath(box, 12);
    QLinearGradient fill(box.topLeft(), box.bottomLeft());
    fill.setColorAt(0, LobbyTheme::mix(port, t.panel, 0.09));
    fill.setColorAt(1, t.panel);
    painter.fillPath(path, fill);

    // The cap, inside the rounded outline.
    painter.save();
    painter.setClipPath(path);
    const QRectF cap(box.left(), box.top(), box.width(), m_cap->height());
    QLinearGradient capFill(cap.topLeft(), cap.bottomLeft());
    capFill.setColorAt(0, LobbyTheme::mix(port, Qt::white, 0.92));
    capFill.setColorAt(1, LobbyTheme::mix(port, Qt::black, 0.78));
    painter.fillRect(cap, capFill);
    LobbyTheme::paintScanlines(painter, cap, QColor(0, 0, 0, 18));
    painter.fillRect(QRectF(cap.left(), cap.bottom() - 2, cap.width(), 2), QColor(0, 0, 0, 56));
    painter.restore();

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(m_self ? port : LobbyTheme::mix(port, t.line, 0.35), 1.0));
    painter.drawPath(path);
}

#endif // NETPLAY
