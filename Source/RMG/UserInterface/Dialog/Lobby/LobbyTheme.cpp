#ifdef NETPLAY

#include "LobbyTheme.hpp"

#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmapCache>
#include <QRegularExpression>
#include <QSvgRenderer>

#include <utility>

namespace UserInterface
{
namespace Dialog
{
namespace LobbyTheme
{

namespace
{
    QColor rgba(int r, int g, int b, qreal a)
    {
        QColor c(r, g, b);
        c.setAlphaF(a);
        return c;
    }

    Tokens makeDark()
    {
        Tokens t;
        t.dark = true;
        t.bg = QColor("#131214");
        t.glowA = rgba(229, 71, 58, 0.07);
        t.glowB = rgba(246, 194, 45, 0.035);
        t.panel = QColor("#1d1c1f");
        t.panelB = QColor("#19181b");
        t.panel2 = QColor("#252428");
        t.raise = QColor("#302e33");
        t.well = QColor("#100f11");
        t.tint = rgba(255, 240, 210, 0.05);
        t.line = QColor("#2c2a2f");
        t.line2 = QColor("#423f46");
        t.ink = QColor("#f4efe3");
        t.ink2 = QColor("#c9c2b4");
        t.muted = QColor("#958e80");
        t.faint = QColor("#6f6a5e");
        t.port1 = QColor("#f0544a");
        t.port2 = QColor("#4f7ff0");
        t.port3 = QColor("#f2c81e");
        t.port4 = QColor("#3ec45a");
        t.accent = QColor("#cf3a2d");
        t.accentHi = QColor("#dc4535");
        t.accentLo = QColor("#96261c");
        t.accentLip = QColor("#6e1a12");
        t.accentTint = rgba(229, 71, 58, 0.14);
        t.accentInk = QColor("#f4705f");
        t.live = QColor("#ff4136");
        t.liveBg = QColor("#d6281f");
        t.liveTint = rgba(255, 65, 54, 0.15);
        t.ok = QColor("#4ccf74");
        t.okTint = rgba(76, 207, 116, 0.13);
        t.warn = QColor("#f0a63a");
        t.warnTint = rgba(240, 166, 58, 0.13);
        t.bad = QColor("#ef5a4f");
        t.stateRoom = QColor("#a9a294");
        t.stateWatch = QColor("#c08bff");
        t.watchTint = rgba(192, 139, 255, 0.14);
        t.secondaryA = QColor("#322f35");
        t.secondaryB = QColor("#28262b");
        t.secondaryLip = QColor("#0e0d0f");
        t.bandA = QColor("#211f23");
        t.bandB = QColor("#171619");
        t.cartA = QColor("#57545b");
        t.cartB = QColor("#38363b");
        t.stat = rgba(0, 0, 0, 0.24);
        t.scan = rgba(255, 255, 255, 0.03);
        return t;
    }

    Tokens makeLight()
    {
        Tokens t;
        t.dark = false;
        t.bg = QColor("#ebe6d9");
        t.glowA = rgba(200, 40, 30, 0.05);
        t.glowB = rgba(242, 185, 30, 0.06);
        t.panel = QColor("#fbf8f0");
        t.panelB = QColor("#f6f2e7");
        t.panel2 = QColor("#f1ece0");
        t.raise = QColor("#e7e1d2");
        t.well = QColor("#fffdf7");
        t.tint = rgba(90, 60, 20, 0.05);
        t.line = QColor("#dcd5c4");
        t.line2 = QColor("#c7beaa");
        t.ink = QColor("#231f18");
        t.ink2 = QColor("#4b453a");
        t.muted = QColor("#6f6858");
        t.faint = QColor("#8f8674");
        t.port1 = QColor("#d63a2f");
        t.port2 = QColor("#2e62d4");
        t.port3 = QColor("#b98c00");
        t.port4 = QColor("#1e9444");
        t.accent = QColor("#c8281e");
        t.accentHi = QColor("#d23226");
        t.accentLo = QColor("#96180f");
        t.accentLip = QColor("#7c120b");
        t.accentTint = rgba(200, 40, 30, 0.09);
        t.accentInk = QColor("#c8281e");
        t.live = QColor("#d81f2d");
        t.liveBg = QColor("#d81f2d");
        t.liveTint = rgba(216, 31, 45, 0.1);
        t.ok = QColor("#14763a");
        t.okTint = rgba(27, 143, 69, 0.11);
        t.warn = QColor("#8f5a00");
        t.warnTint = rgba(178, 111, 0, 0.11);
        t.bad = QColor("#c5362c");
        t.stateRoom = QColor("#7b7364");
        t.stateWatch = QColor("#7f4fc4");
        t.watchTint = rgba(127, 79, 196, 0.1);
        t.secondaryA = QColor("#fffdf7");
        t.secondaryB = QColor("#efeadc");
        t.secondaryLip = QColor("#c7beaa");
        t.bandA = QColor("#f9f5eb");
        t.bandB = QColor("#eee9dc");
        t.cartA = QColor("#cfcabe");
        t.cartB = QColor("#aaa597");
        t.stat = rgba(90, 60, 20, 0.06);
        t.scan = rgba(70, 50, 20, 0.035);
        return t;
    }

    QString pixelFamily()
    {
        static const QString family = []() {
            const int id = QFontDatabase::addApplicationFont(
                QStringLiteral(":/lobby/fonts/Audiowide-Regular.ttf"));
            const QStringList families = QFontDatabase::applicationFontFamilies(id);
            return families.isEmpty() ? QString() : families.first();
        }();
        return family;
    }

    // Renders an SVG into a square or rectangular transparent image at the
    // given pixel ratio. Going through QSvgRenderer rather than QIcon keeps
    // this independent of which icon engine plugins got deployed.
    QImage renderSvg(const QString& path, const QSize& logicalSize, qreal devicePixelRatio)
    {
        QImage image(logicalSize * devicePixelRatio, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QSvgRenderer renderer(path);
        QPainter painter(&image);
        renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(image.size())));
        painter.end();
        image.setDevicePixelRatio(devicePixelRatio);
        return image;
    }
} // namespace

bool isDark()
{
    return QApplication::palette().window().color().value() < 128;
}

const Tokens& tokens()
{
    static const Tokens dark = makeDark();
    static const Tokens light = makeLight();
    return isDark() ? dark : light;
}

QColor roleColor(Role role)
{
    const Tokens& t = tokens();
    switch (role)
    {
        case Role::Ink:    return t.ink;
        case Role::Ink2:   return t.ink2;
        case Role::Muted:  return t.muted;
        case Role::Faint:  return t.faint;
        case Role::Accent: return t.accentInk;
        case Role::Ok:     return t.ok;
        case Role::Warn:   return t.warn;
        case Role::Bad:    return t.bad;
        case Role::Live:   return t.live;
        case Role::Watch:  return t.stateWatch;
    }
    return t.ink;
}

QColor portColor(int slot)
{
    const Tokens& t = tokens();
    switch (slot)
    {
        case 1: return t.port1;
        case 2: return t.port2;
        case 3: return t.port3;
        case 4: return t.port4;
    }
    return t.muted;
}

QColor mix(const QColor& a, const QColor& b, qreal amountOfA)
{
    const qreal x = amountOfA;
    const qreal y = 1.0 - amountOfA;
    QColor c;
    c.setRgbF(a.redF() * x + b.redF() * y, a.greenF() * x + b.greenF() * y,
              a.blueF() * x + b.blueF() * y, a.alphaF() * x + b.alphaF() * y);
    return c;
}

QString css(const QColor& color)
{
    return QString("rgba(%1,%2,%3,%4)")
        .arg(color.red()).arg(color.green()).arg(color.blue())
        .arg(color.alphaF(), 0, 'f', 3);
}

QFont pixelFont(int px, qreal letterSpacing)
{
    QFont font = QApplication::font();
    QStringList families;
    const QString family = pixelFamily();
    if (!family.isEmpty())
        families << family;
    families << QStringLiteral("Yu Gothic UI") << font.family();
    font.setFamilies(families);
    font.setPixelSize(px);
    font.setWeight(QFont::Normal);
    if (letterSpacing != 0.0)
        font.setLetterSpacing(QFont::AbsoluteSpacing, letterSpacing);
    return font;
}

QString nameInitial(const QString& name)
{
    if (name.isEmpty())
        return QString();
    return name.left(name.front().isHighSurrogate() ? 2 : 1).toUpper();
}

QFont uiFont(int px, QFont::Weight weight)
{
    QFont font = QApplication::font();
    font.setPixelSize(px);
    font.setWeight(weight);
    return font;
}

QPixmap icon(const QString& name, const QColor& color, int px, qreal devicePixelRatio)
{
    const QString key = QString("lobby-icon:%1:%2:%3:%4")
        .arg(name).arg(color.rgba(), 8, 16).arg(px).arg(devicePixelRatio);
    QPixmap cached;
    if (QPixmapCache::find(key, &cached))
        return cached;

    QImage image = renderSvg(QString(":/lobby/icons/%1.svg").arg(name), QSize(px, px), devicePixelRatio);
    QPainter painter(&image);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(QRect(0, 0, px, px), color);
    painter.end();

    cached = QPixmap::fromImage(image);
    QPixmapCache::insert(key, cached);
    return cached;
}

QPixmap flag(const QString& country, qreal devicePixelRatio)
{
    if (country.isEmpty())
        return QPixmap();
    const QString path = QString(":/flags/%1.svg").arg(country.toLower());
    if (!QFile::exists(path))
        return QPixmap();

    const QString key = QString("lobby-flag:%1:%2").arg(country.toLower()).arg(devicePixelRatio);
    QPixmap cached;
    if (QPixmapCache::find(key, &cached))
        return cached;

    const QSize size(15, 10);
    const QImage art = renderSvg(path, size, devicePixelRatio);
    QImage image(size * devicePixelRatio, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    image.setDevicePixelRatio(devicePixelRatio);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, size.width(), size.height()), 1.5, 1.5);
    painter.setClipPath(clip);
    painter.drawImage(QPointF(0, 0), art);
    painter.end();

    cached = QPixmap::fromImage(image);
    QPixmapCache::insert(key, cached);
    return cached;
}

QColor pingColor(int ms)
{
    const Tokens& t = tokens();
    if (ms < 0)    return t.muted;
    if (ms <= 60)  return t.ok;
    if (ms <= 120) return t.warn;
    return t.bad;
}

void paintPingBars(QPainter& painter, const QPointF& origin, int ms, bool estimate)
{
    const int level = ms < 0 ? 0 : ms <= 60 ? 3 : ms <= 120 ? 2 : 1;
    QColor lit = pingColor(ms);
    QColor unlit = tokens().line2;
    if (estimate)
    {
        lit.setAlphaF(0.6);
        unlit.setAlphaF(0.6);
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    for (int i = 0; i < 3; ++i)
    {
        const qreal height = 4.0 * (i + 1);
        painter.setBrush(i < level ? lit : unlit);
        painter.drawRoundedRect(QRectF(origin.x() + i * 5.0, origin.y() - height, 3.0, height), 1.0, 1.0);
    }
    painter.restore();
}

void paintScanlines(QPainter& painter, const QRectF& rect, const QColor& color)
{
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.setRenderHint(QPainter::Antialiasing, false);
    for (qreal y = rect.top(); y < rect.bottom(); y += 3.0)
        painter.drawRect(QRectF(rect.left(), y, rect.width(), 1.0));
    painter.restore();
}

QString gameInitials(const QString& gameName)
{
    // Tags and version numbers aren't part of the name: "Mario Kart 64 (U) [!]"
    // is MK. Words split on case changes too, so GoldenEye gives GE.
    QString name = gameName;
    name.remove(QRegularExpression(QStringLiteral("[\\(\\[].*$")));
    QString initials;
    static const QRegularExpression word(QStringLiteral("[A-Z]?[a-z0-9']+|[A-Z]+"));
    auto it = word.globalMatch(name);
    QString first;
    while (it.hasNext() && initials.size() < 3)
    {
        const QString w = it.next().captured();
        if (w.front().isDigit() || w.compare(QLatin1String("the"), Qt::CaseInsensitive) == 0 ||
            w.compare(QLatin1String("of"), Qt::CaseInsensitive) == 0)
            continue;
        if (first.isEmpty())
            first = w;
        initials += w.front().toUpper();
    }
    if (initials.size() == 1 && first.size() > 1)
        initials += first.at(1).toUpper();
    if (initials.isEmpty())
        initials = name.trimmed().left(2).toUpper();
    return initials;
}

QColor gameHue(const QString& gameName)
{
    static const QColor hues[] = {
        QColor("#d9482f"), QColor("#e0862a"), QColor("#2f9e53"), QColor("#6f7480"), QColor("#8a5bd1"),
        QColor("#d9559a"), QColor("#2a9d8f"), QColor("#c4a020"), QColor("#a8443a"),
    };
    // Spelled out rather than qHash so a game keeps its colour across runs.
    quint32 hash = 2166136261u;
    for (const QChar c : gameName)
    {
        hash ^= c.unicode();
        hash *= 16777619u;
    }
    return hues[hash % (sizeof(hues) / sizeof(hues[0]))];
}

QString styleSheet()
{
    const Tokens& t = tokens();
    const QString chevron = t.dark ? QStringLiteral(":/icons/white/svg/arrow-down-s-line.svg")
                                   : QStringLiteral(":/icons/black/svg/arrow-down-s-line.svg");

    // QString::arg tops out at 9 arguments per call, so the sheet is assembled
    // from named replacements instead.
    QString qss = QStringLiteral(R"(
        QWidget { color: @ink; }
        QDialog { background-color: @bg; }
        QToolTip { background-color: @panel2; color: @ink; border: 1px solid @line2; padding: 3px 6px; }

        QMenu { background-color: @panel2; border: 1px solid @line2; padding: 4px 0; }
        QMenu::item { padding: 6px 22px 6px 22px; color: @ink2; background: transparent; }
        QMenu::item:selected { background-color: @raise; color: @ink; }
        QMenu::item:disabled { color: @faint; }
        QMenu::separator { height: 1px; background: @line; margin: 3px 0; }
        QMenu::indicator { width: 12px; height: 12px; left: 6px; }

        QMenu#LobbyMenu { background: transparent; border: none; padding: 4px 0 5px 0; }
        QMenu#LobbyMenu::item { padding: 6px 30px 6px 36px; background: transparent; }
        QMenu#LobbyMenu::separator { height: 9px; background: transparent; margin: 0; }

        QLabel[role="ink2"] { color: @ink2; }
        QLabel[role="muted"] { color: @muted; }
        QLabel[role="faint"] { color: @faint; }
        QLabel[role="warn"] { color: @warn; }
        QLabel[role="bad"] { color: @bad; }

        QSplitter::handle { background: transparent; }
        QSplitter::handle:hover { background: @tint; border-radius: 3px; }

        QScrollArea { background: transparent; border: none; }
        QScrollBar:vertical { background: transparent; width: 8px; margin: 2px 1px; }
        QScrollBar::handle:vertical { background: @line2; border-radius: 3px; min-height: 24px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
        QScrollBar:horizontal { height: 0; }

        QLineEdit {
            background-color: @well; color: @ink; border: 1px solid @line2; border-radius: 8px;
            padding: 5px 9px; selection-background-color: @accent; selection-color: #ffffff;
        }
        QLineEdit:focus { border-color: @accent; }
        QLineEdit:disabled { color: @faint; }

        QLineEdit#ComposeInput { background: transparent; border: none; padding: 0; }
        QFrame#Compose { background-color: @well; border: 1px solid @line2; border-radius: 8px; }

        QTextEdit { background: transparent; border: none; color: @ink2;
            selection-background-color: @accent; selection-color: #ffffff; }

        QTreeWidget#PlayersTree { background: transparent; border: none; outline: 0; }
        QTreeWidget#PlayersTree::item, QTreeWidget#PlayersTree::branch,
        QTreeWidget#PlayersTree::item:selected, QTreeWidget#PlayersTree::item:hover {
            background: transparent; border: none;
        }

        QLabel#NoRomLabel { color: @muted; border: 1px dashed @line2; border-radius: 6px; padding: 3px 8px; }

        QFrame#ChatTabRow { background: transparent; border: none; border-bottom: 1px solid @line; }
        QTabBar#ChatTabs { background: transparent; qproperty-drawBase: 0; }
        QTabBar#ChatTabs::tab {
            height: 26px; padding: 0 10px; margin-right: 2px; color: @muted; background: transparent;
            border: 1px solid transparent; border-top-left-radius: 7px; border-top-right-radius: 7px;
        }
        QTabBar#ChatTabs::tab:hover { color: @ink2; }
        QTabBar#ChatTabs::tab:selected {
            color: @ink; background: @panel; border-color: @line; border-bottom-color: @panel;
            border-top: 2px solid @accent;
        }
        QLabel#ChatBadge {
            color: #ffffff; background-color: @liveBg; border-radius: 7px; padding: 0 4px;
            min-width: 6px; min-height: 14px; max-height: 14px;
        }
        QLabel#ChatBadge[mention="true"] { color: @onWarn; background-color: @warn; }

        QPushButton#MentionJump {
            min-height: 22px; max-height: 22px; padding: 0 10px; border-radius: 12px;
            background-color: @raise; color: @ink; border: 1px solid @line2;
        }
        QPushButton#MentionJump:hover { border-color: @warn; }

        QFrame#PmThreadHeader { background: transparent; border: none; border-bottom: 1px solid @line; }
        QFrame#ChallengeVersus { background-color: @tint; border: 1px solid @line; border-radius: 10px; }
        QFrame#SanctionNote { background-color: @warnTint; border: 1px solid @warnEdge; border-radius: 8px; }
        QFrame#ModerationRow { background: transparent; border: none; border-bottom: 1px solid @line; }

        QComboBox#ChallengeRomCombo { background: transparent; border: none; padding: 0; color: @ink; }
        QComboBox#ChallengeRomCombo::drop-down { width: 0; border: none; }
        QComboBox#ChallengeRomCombo::down-arrow { image: none; width: 0; height: 0; }

        QFrame#RomPicker { background-color: @well; border: 1px solid @line2; border-radius: 9px; }
        QFrame#RomPicker:focus { border-color: @accent; }
        QComboBox#BrowseRomCombo { background: transparent; border: none; padding: 0; color: @ink; }
        QComboBox#BrowseRomCombo QLineEdit {
            background: transparent; border: none; padding: 0; color: @ink;
            selection-background-color: @accent; selection-color: #ffffff;
        }
        QComboBox#BrowseRomCombo:disabled, QComboBox#BrowseRomCombo QLineEdit:disabled { color: @faint; }
        QComboBox#BrowseRomCombo::drop-down { width: 0; border: none; }
        QComboBox#BrowseRomCombo::down-arrow { image: none; width: 0; height: 0; }

        QComboBox QAbstractItemView, QListView#qt_completer_popup, QCompleter QAbstractItemView {
            background-color: @panel2; color: @ink; border: 1px solid @line2; outline: 0;
            selection-background-color: @accentTint; selection-color: @ink;
        }

        QComboBox#LiveSort {
            background: transparent; border: 1px solid @line2; border-radius: 6px;
            padding: 0 6px 0 9px; min-height: 22px; max-height: 22px; color: @muted;
        }
        QComboBox#LiveSort:hover { color: @ink; }
        QComboBox#LiveSort::drop-down { border: none; width: 15px; }
        QComboBox#LiveSort::down-arrow { image: url(@chevron); width: 12px; height: 12px; }

        QPushButton[chip="true"] {
            min-height: 22px; max-height: 22px; padding: 0 9px; border-radius: 6px;
            border: 1px solid @line2; background: transparent; color: @muted;
        }
        QPushButton[chip="true"]:hover { color: @ink; }
        QPushButton[chip="true"]:checked { background-color: @raise; color: @ink; }
        QPushButton[chip="true"]:disabled { color: @faint; }
        QFrame#ChipSeg { background-color: @well; border: 1px solid @line2; border-radius: 8px; }
        QFrame#ChipSeg QPushButton[chip="true"] {
            min-height: 20px; max-height: 20px; padding: 0 9px; border: none; border-radius: 6px;
        }

        QFrame#Seg { background-color: @well; border: 1px solid @line2; border-radius: 9px; }
        QPushButton[seg="true"] {
            min-height: 28px; padding: 0 12px; border: none; border-radius: 6px;
            background: transparent; color: @muted;
        }
        QPushButton[seg="true"]:hover { color: @ink; }
        QPushButton[seg="true"]:checked { background-color: @raise; color: @ink; }
        QPushButton[seg="true"]:disabled { color: @faint; }

        QFrame#FieldSeparator { background-color: @line; max-width: 1px; min-width: 1px; border: none; }
        QFrame#FieldRule { background-color: @line; max-height: 1px; min-height: 1px; border: none; }
        QFrame#BrandSeparator { background-color: @line2; max-width: 1px; min-width: 1px; border: none; }

        QLabel#RegionChip {
            color: @ink2; background-color: @raise; border: 1px solid @line2; border-radius: 5px;
            padding: 1px 6px;
        }

        QPushButton#UserChip {
            background-color: @tint; border: 1px solid @line2; border-radius: 9px;
        }
        QPushButton#UserChip:hover { background-color: @raise; }
        QPushButton#UserChip::menu-indicator { image: none; width: 0; }

        QMessageBox QPushButton, QInputDialog QPushButton {
            min-height: 24px; min-width: 64px; padding: 2px 12px; border-radius: 7px;
            border: 1px solid @line2; background-color: @secondaryB; color: @ink;
        }
        QMessageBox QPushButton:hover, QInputDialog QPushButton:hover { background-color: @raise; }
        QMessageBox QPushButton:default, QInputDialog QPushButton:default { border-color: @accent; }
    )");

    const std::pair<const char*, QColor> colors[] = {
        // Longest names first, so @panel2 isn't half-replaced by @panel.
        {"@secondaryB", t.secondaryB}, {"@accentTint", t.accentTint}, {"@liveBg", t.liveBg},
        {"@panel2", t.panel2}, {"@accent", t.accent}, {"@panel", t.panel}, {"@raise", t.raise},
        {"@line2", t.line2}, {"@line", t.line}, {"@well", t.well}, {"@tint", t.tint},
        {"@ink2", t.ink2}, {"@ink", t.ink}, {"@muted", t.muted}, {"@faint", t.faint},
        {"@warnEdge", mix(t.warn, t.line, 0.35)}, {"@warnTint", t.warnTint},
        {"@onWarn", t.dark ? QColor("#1f1400") : QColor("#ffffff")},
        {"@warn", t.warn}, {"@bad", t.bad}, {"@bg", t.bg},
    };
    qss.replace(QStringLiteral("@chevron"), chevron);
    for (const auto& [name, color] : colors)
        qss.replace(QLatin1String(name), css(color));
    return qss;
}

} // namespace LobbyTheme
} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
