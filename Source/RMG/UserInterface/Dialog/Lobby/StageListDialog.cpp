#ifdef NETPLAY

#include "StageListDialog.hpp"

#include "CreateRoomDialog.hpp"
#include "GameSettingsDialog.hpp"
#include "LobbyTheme.hpp"
#include "LobbyWidgets.hpp"

#include <RMG-Core/Callback.hpp>
#include <RMG-Core/Error.hpp>

#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStyle>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

#include <QKeySequence>

#include <algorithm>
#include <cmath>
#include <map>

using namespace UserInterface::Dialog;
using LobbyTheme::Tokens;

//
// Shared helpers
//

std::vector<CoreStageListEntry> UserInterface::Dialog::stageListEntries(const CoreStageListInfo& info, const QString& code)
{
    std::vector<CoreStageListEntry> entries;
    if (code.isEmpty() || CoreDecodeStageList(info, code.toStdString(), entries) != CoreStageListProblem::None)
        entries.clear();
    return entries;
}

const QHash<int, QImage>& UserInterface::Dialog::stageListIcons(const QString& romMd5, const QString& optionId,
                                                                 const QString& romPath)
{
    // Read once per ROM file and session; a failure is kept too, so a ROM
    // that can't be read isn't read again every time a view opens.
    static QHash<QString, QHash<int, QImage>> cache;
    const QString key = romMd5.toUpper() + QLatin1Char('/') + optionId + QLatin1Char('/') + romPath;
    auto it = cache.find(key);
    if (it != cache.end())
        return *it;

    QHash<int, QImage> icons;
    if (!romPath.isEmpty())
    {
        std::map<int, CoreStageIcon> raw;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const bool ok = CoreReadStageListIcons(romPath.toStdU32String(), romMd5.toStdString(), optionId.toStdString(), raw);
        QApplication::restoreOverrideCursor();
        for (const auto& [id, icon] : raw)
        {
            QImage image(icon.Rgba.data(), icon.Width, icon.Height, icon.Width * 4, QImage::Format_RGBA8888);
            icons.insert(id, image.copy());
        }
        CoreAddCallbackMessage(ok ? CoreDebugMessageType::Info : CoreDebugMessageType::Warning,
            ok ? QString("Stage list: read %1 stage icons from the ROM").arg(icons.size()).toStdString()
               : "Stage list: no stage icons, names only: " + CoreGetError());
    }
    return *cache.insert(key, icons);
}

QString UserInterface::Dialog::stageCountText(int stages)
{
    return stages == 1 ? StageListDialog::tr("1 stage") : StageListDialog::tr("%1 stages").arg(stages);
}

QString UserInterface::Dialog::stageListCountText(const CoreStageListInfo& info, int stages)
{
    if (stages <= 0)
        return StageListDialog::tr("No stages yet");
    const int pages = info.pagesFor(stages);
    return pages == 1 ? StageListDialog::tr("%1 on 1 page").arg(stageCountText(stages))
                      : StageListDialog::tr("%1 on %2 pages").arg(stageCountText(stages)).arg(pages);
}

namespace
{
constexpr int kIconW = 40;
constexpr int kIconH = 30;
constexpr int kCardWidth = 118;
constexpr int kCardIconScale = 2;
const char* const kRowMime = "application/x-rmgk-stage-row";

// A row of widgets that wraps, like words on a page.
class FlowLayout : public QLayout
{
  public:
    FlowLayout(QWidget* parent, int hSpacing, int vSpacing) : QLayout(parent), m_h(hSpacing), m_v(vSpacing)
    {
        setContentsMargins(0, 0, 0, 0);
    }
    explicit FlowLayout(int hSpacing, int vSpacing) : m_h(hSpacing), m_v(vSpacing)
    {
        setContentsMargins(0, 0, 0, 0);
    }
    ~FlowLayout() override
    {
        while (QLayoutItem* item = takeAt(0))
            delete item;
    }

    void addItem(QLayoutItem* item) override { m_items.append(item); }
    int count() const override { return int(m_items.size()); }
    QLayoutItem* itemAt(int index) const override { return m_items.value(index); }
    QLayoutItem* takeAt(int index) override
    {
        return index >= 0 && index < m_items.size() ? m_items.takeAt(index) : nullptr;
    }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), true); }
    void setGeometry(const QRect& rect) override
    {
        QLayout::setGeometry(rect);
        arrange(rect, false);
    }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override
    {
        QSize size;
        for (QLayoutItem* item : m_items)
        {
            if (!item->isEmpty())
                size = size.expandedTo(item->minimumSize());
        }
        const QMargins m = contentsMargins();
        return size + QSize(m.left() + m.right(), m.top() + m.bottom());
    }

  private:
    int arrange(const QRect& rect, bool testOnly) const
    {
        const QMargins m = contentsMargins();
        const QRect area = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
        int x = area.x();
        int y = area.y();
        int line = 0;
        for (QLayoutItem* item : m_items)
        {
            if (item->isEmpty())
                continue;
            const QSize hint = item->sizeHint();
            if (x > area.x() && x + hint.width() > area.right() + 1)
            {
                x = area.x();
                y += line + m_v;
                line = 0;
            }
            if (!testOnly)
                item->setGeometry(QRect(QPoint(x, y), hint));
            x += hint.width() + m_h;
            line = std::max(line, hint.height());
        }
        return y + line - rect.y() + m.bottom();
    }

    QList<QLayoutItem*> m_items;
    int m_h;
    int m_v;
};

QLabel* heading(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text.toUpper(), parent);
    QFont font = LobbyTheme::uiFont(10, QFont::DemiBold);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.9);
    label->setFont(font);
    label->setProperty("role", "muted");
    return label;
}

QLabel* small(const QString& text, const char* role, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setFont(LobbyTheme::uiFont(11));
    label->setProperty("role", role);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    return label;
}

QFrame* rule(QWidget* parent)
{
    auto* line = new QFrame(parent);
    line->setObjectName("FieldRule");
    return line;
}

void setRole(QLabel* label, const char* role)
{
    if (label->property("role").toString() == QLatin1String(role))
        return;
    label->setProperty("role", role);
    label->style()->unpolish(label);
    label->style()->polish(label);
}

LobbyButton* button(const QString& text, LobbyButton::Kind kind, LobbyButton::Size size, const char* name, QWidget* parent)
{
    auto* b = new LobbyButton(text, kind, parent);
    b->setObjectName(name);
    b->setButtonSize(size);
    return b;
}

// What a stage has, in words.
QString hazardWords(const CoreStageListStage& s)
{
    if (s.Hazards && s.Movement)
        return StageListDialog::tr("Hazards and movement", "what a stage has");
    if (s.Hazards)
        return StageListDialog::tr("Hazards", "what a stage has");
    if (s.Movement)
        return StageListDialog::tr("Moves", "what a stage has");
    return QString();
}

// An icon drawn into rect: the game's own, scaled without smoothing when it
// can be doubled cleanly, or the stage's initials on a plain tile when the
// ROM couldn't be read.
void paintStageIcon(QPainter& painter, const QRectF& rect, const QImage& image, const QString& name)
{
    const Tokens& t = LobbyTheme::tokens();
    painter.save();
    QPainterPath clip;
    clip.addRoundedRect(rect, 3, 3);
    painter.setClipPath(clip);
    if (!image.isNull())
    {
        const qreal scale = rect.width() / image.width() * painter.device()->devicePixelRatioF();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, scale < 1.9 || std::abs(scale - std::round(scale)) > 0.01);
        painter.drawImage(rect, image);
    }
    else
    {
        painter.fillRect(rect, t.well);
        QStringList words = name.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QString initials;
        for (const QString& word : words)
        {
            if (initials.size() < 2 && word.at(0).isLetterOrNumber())
                initials += word.at(0).toUpper();
        }
        painter.setPen(t.muted);
        painter.setFont(LobbyTheme::uiFont(int(std::max<qreal>(9, rect.height() / 3)), QFont::DemiBold));
        painter.drawText(rect, Qt::AlignCenter, initials);
    }
    painter.restore();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(t.line2, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    painter.restore();
}

// A stage's icon as a widget, for the list's rows.
class Thumb : public QWidget
{
  public:
    Thumb(const QImage& image, const QString& name, QWidget* parent) : QWidget(parent), m_image(image), m_name(name)
    {
        setFixedSize(kIconW, kIconH);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        paintStageIcon(painter, QRectF(rect()), m_image, m_name);
    }

  private:
    QImage m_image;
    QString m_name;
};

QString ordinalText(int position)
{
    return QString::number(position);
}
} // namespace

//
// StageStrip
//

StageStrip::StageStrip(QWidget* parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

void StageStrip::setStages(const QHash<int, QImage>& icons, const std::vector<CoreStageListEntry>& entries)
{
    constexpr int shown = 9;
    m_icons.clear();
    for (size_t i = 0; i < entries.size() && i < size_t(shown); i++)
        m_icons << icons.value(entries[i].Stage);
    m_more = std::max(0, int(entries.size()) - shown);
    updateGeometry();
    update();
}

QSize StageStrip::sizeHint() const
{
    return QSize(int(m_icons.size()) * (kIconW + 4) + (m_more > 0 ? 34 : 0), m_icons.isEmpty() ? 0 : kIconH);
}

void StageStrip::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    qreal x = 0;
    for (const QImage& image : std::as_const(m_icons))
    {
        paintStageIcon(painter, QRectF(x, 0, kIconW, kIconH), image, QStringLiteral("?"));
        x += kIconW + 4;
    }
    if (m_more > 0)
    {
        painter.setPen(LobbyTheme::tokens().muted);
        painter.setFont(LobbyTheme::uiFont(11, QFont::DemiBold));
        painter.drawText(QRectF(x, 0, 34, kIconH), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("+%1").arg(m_more));
    }
}

//
// The browse panel's cards: a base stage with its variants as chips.
//

class StageListDialog::Card : public QAbstractButton
{
  public:
    Card(const CoreStageListStage& base, const QImage& image, QWidget* parent)
        : QAbstractButton(parent), m_base(base), m_image(image)
    {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::StrongFocus);
        setObjectName("StageCard");
        setAccessibleName(QString::fromStdString(base.Name));
        m_words = hazardWords(base);

        // Room for the icon, the name (up to 2 lines) and what it has; the
        // variant chips go under them.
        const QFontMetrics name(LobbyTheme::uiFont(11, QFont::DemiBold));
        const QRect bound = name.boundingRect(QRect(0, 0, kCardWidth - 12, 1000), Qt::TextWordWrap | Qt::AlignHCenter,
                                              QString::fromStdString(base.Name));
        m_nameHeight = std::min(bound.height(), name.lineSpacing() * 2);
        const int top = 8 + kIconH * kCardIconScale + 6 + m_nameHeight + (m_words.isEmpty() ? 0 : 15) + 6;
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(7, top, 7, 8);
        m_chips = new FlowLayout(4, 4);
        lay->addLayout(m_chips);
        setFixedWidth(kCardWidth);
    }

    // A variant's chip.
    QPushButton* addChip(const CoreStageListStage& variant, const QString& tip)
    {
        auto* chip = new QPushButton(this);
        chip->setProperty("chip", true);
        chip->setCheckable(true);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setFont(LobbyTheme::uiFont(10));
        const QString text = QString::fromStdString(variant.Short);
        chip->setText(chip->fontMetrics().elidedText(text, Qt::ElideRight, kCardWidth - 14 - 18));
        chip->setToolTip(tip);
        chip->setAccessibleName(QString::fromStdString(variant.Name));
        chip->setObjectName("StageVariantChip");
        chip->setProperty("stageId", int(variant.Id));
        m_chips->addWidget(chip);
        m_chipIds << variant.Id;
        m_chipList << chip;
        return chip;
    }

    void setPosition(int position)
    {
        const bool on = position > 0;
        if (on != isChecked())
        {
            const QSignalBlocker blocker(this);
            setChecked(on);
        }
        if (position != m_position)
        {
            m_position = position;
            update();
        }
        QString description = m_words.isEmpty() ? StageListDialog::tr("No hazards or movement") : m_words;
        if (on)
            description += QStringLiteral(". ") + StageListDialog::tr("Number %1 on your list").arg(position);
        setAccessibleDescription(description);
    }

    const CoreStageListStage& base() const { return m_base; }
    const QList<int>& chipIds() const { return m_chipIds; }
    const QList<QPushButton*>& chips() const { return m_chipList; }

    QSize sizeHint() const override { return QSize(kCardWidth, layout()->heightForWidth(kCardWidth)); }
    QSize minimumSizeHint() const override { return sizeHint(); }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        const Tokens& t = LobbyTheme::tokens();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF box = QRectF(rect()).adjusted(1, 1, -1, -1);
        const bool on = isChecked();
        const bool hover = underMouse();
        painter.setPen(QPen(on ? t.accent : hover ? t.line2 : t.line, on ? 2 : 1));
        painter.setBrush(on ? LobbyTheme::mix(t.accentTint, t.panel, 0.7) : hover ? t.raise : t.panel);
        painter.drawRoundedRect(box, 9, 9);
        if (hasFocus())
        {
            QPen focus(t.accent, 1, Qt::DashLine);
            painter.setPen(focus);
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(box.adjusted(3, 3, -3, -3), 7, 7);
        }

        const QRectF icon((width() - kIconW * kCardIconScale) / 2.0, 8, kIconW * kCardIconScale, kIconH * kCardIconScale);
        paintStageIcon(painter, icon, m_image, QString::fromStdString(m_base.Name));

        // Its place on the list, on the icon's corner.
        if (on && m_position > 0)
        {
            const QRectF badge(icon.right() - 13, icon.top() - 5, 20, 20);
            painter.setPen(Qt::NoPen);
            painter.setBrush(t.accent);
            painter.drawEllipse(badge);
            painter.setPen(Qt::white);
            painter.setFont(LobbyTheme::uiFont(10, QFont::Bold));
            painter.drawText(badge, Qt::AlignCenter, ordinalText(m_position));
        }

        painter.setPen(t.ink);
        painter.setFont(LobbyTheme::uiFont(11, QFont::DemiBold));
        const QRectF name(6, icon.bottom() + 6, width() - 12, m_nameHeight);
        const QFontMetrics fm(painter.font());
        QString text = QString::fromStdString(m_base.Name);
        if (fm.boundingRect(name.toRect(), Qt::TextWordWrap | Qt::AlignHCenter, text).height() > m_nameHeight)
            text = fm.elidedText(text, Qt::ElideRight, int(name.width()) * 2 - 8);
        painter.drawText(name, Qt::TextWordWrap | Qt::AlignHCenter | Qt::AlignTop, text);
        if (!m_words.isEmpty())
        {
            painter.setPen(t.muted);
            painter.setFont(LobbyTheme::uiFont(10));
            painter.drawText(QRectF(4, name.bottom() + 1, width() - 8, 14), Qt::AlignHCenter | Qt::AlignTop, m_words);
        }
    }

  private:
    CoreStageListStage m_base;
    QImage m_image;
    QString m_words;
    int m_nameHeight = 0;
    int m_position = 0;
    FlowLayout* m_chips = nullptr;
    QList<int> m_chipIds;
    QList<QPushButton*> m_chipList;
};

//
// The list's rows, the view that takes drops, and the stage select preview.
//

class StageListDialog::Row : public QWidget
{
  public:
    Row(StageListDialog* dialog, int index, const CoreStageListStage& stage, const CoreStageListStage* base, uint8_t mode,
        const QImage& image, bool editable, int count, QWidget* parent)
        : QWidget(parent), m_dialog(dialog), m_index(index)
    {
        setObjectName("StageListRow");
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(editable ? Qt::StrongFocus : Qt::NoFocus);
        setAccessibleName(StageListDialog::tr("%1, number %2").arg(QString::fromStdString(stage.Name)).arg(index + 1));
        auto* lay = new QHBoxLayout(this);
        lay->setContentsMargins(6, 5, 6, 5);
        lay->setSpacing(7);
        if (editable)
        {
            auto* grip = new IconLabel(QStringLiteral("draggable"), LobbyTheme::Role::Faint, 14, this);
            grip->setToolTip(StageListDialog::tr("Drag to move"));
            grip->setCursor(Qt::OpenHandCursor);
            lay->addWidget(grip, 0, Qt::AlignVCenter);
        }
        auto* number = new QLabel(QString::number(index + 1), this);
        number->setFont(LobbyTheme::uiFont(11, QFont::DemiBold));
        number->setProperty("role", "muted");
        number->setFixedWidth(20);
        number->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        lay->addWidget(number, 0, Qt::AlignVCenter);
        lay->addWidget(new Thumb(image, QString::fromStdString(stage.Name), this), 0, Qt::AlignVCenter);

        auto* column = new QVBoxLayout;
        column->setSpacing(2);
        auto* name = new ElidedLabel(LobbyTheme::Role::Muted, this);
        name->setFont(LobbyTheme::uiFont(12, QFont::DemiBold));
        name->setLead(QString::fromStdString(stage.Name));
        // A variant whose name doesn't say whose it is gets its base's.
        if (base && stage.Name.find(base->Name) == std::string::npos)
            name->setText(QStringLiteral("  ·  ") + QString::fromStdString(base->Name));
        column->addWidget(name);
        auto* switches = new QHBoxLayout;
        switches->setSpacing(12);
        const auto addSwitch = [&](const QString& title, uint8_t bit, const QString& tip) {
            auto* toggle = new ToggleSwitch(title, this);
            toggle->setObjectName(bit == CoreStageModeHazardsOff ? "StageHazardsSwitch" : "StageMovementSwitch");
            toggle->setChecked(!(mode & bit));
            toggle->setToolTip(tip);
            toggle->setAccessibleName(StageListDialog::tr("%1 on %2").arg(title, QString::fromStdString(stage.Name)));
            if (editable)
            {
                connect(toggle, &QCheckBox::toggled, this, [this, bit](bool on) {
                    m_mode = on ? uint8_t(m_mode & ~bit) : uint8_t(m_mode | bit);
                    m_dialog->setMode(m_index, m_mode);
                });
            }
            else
            {
                toggle->setAttribute(Qt::WA_TransparentForMouseEvents);
                toggle->setFocusPolicy(Qt::NoFocus);
                toggle->setCursor(Qt::ArrowCursor);
            }
            switches->addWidget(toggle);
        };
        m_mode = mode;
        if (stage.Hazards)
            addSwitch(StageListDialog::tr("Hazards"), CoreStageModeHazardsOff, StageListDialog::tr("Off: the stage's hazards stay away."));
        if (stage.Movement)
            addSwitch(StageListDialog::tr("Movement"), CoreStageModeMovementOff, StageListDialog::tr("Off: the stage stays still."));
        if (!stage.Hazards && !stage.Movement)
        {
            auto* none = new QLabel(StageListDialog::tr("No hazards or movement"), this);
            none->setFont(LobbyTheme::uiFont(11));
            none->setProperty("role", "faint");
            switches->addWidget(none);
        }
        switches->addStretch(1);
        column->addLayout(switches);
        lay->addLayout(column, 1);

        if (editable)
        {
            const QString stageName = QString::fromStdString(stage.Name);
            m_up = new IconButton(QStringLiteral("arrow-up-s-line"), 24, this);
            m_up->setObjectName("StageMoveUp");
            m_up->setToolTip(StageListDialog::tr("Move up"));
            m_up->setAccessibleName(StageListDialog::tr("Move %1 up").arg(stageName));
            m_up->setEnabled(index > 0);
            m_down = new IconButton(QStringLiteral("arrow-down-s-line"), 24, this);
            m_down->setObjectName("StageMoveDown");
            m_down->setToolTip(StageListDialog::tr("Move down"));
            m_down->setAccessibleName(StageListDialog::tr("Move %1 down").arg(stageName));
            m_down->setEnabled(index < count - 1);
            m_remove = new IconButton(QStringLiteral("close-line"), 24, this);
            m_remove->setObjectName("StageRemove");
            m_remove->setToolTip(StageListDialog::tr("Take off the list"));
            m_remove->setAccessibleName(StageListDialog::tr("Take %1 off the list").arg(stageName));
            lay->addWidget(m_up, 0, Qt::AlignVCenter);
            lay->addWidget(m_down, 0, Qt::AlignVCenter);
            lay->addWidget(m_remove, 0, Qt::AlignVCenter);
            connect(m_up, &QToolButton::clicked, this, [this]() { m_dialog->move(m_index, m_index - 1); m_dialog->focusRow(m_index - 1, 1); });
            connect(m_down, &QToolButton::clicked, this, [this]() { m_dialog->move(m_index, m_index + 1); m_dialog->focusRow(m_index + 1, 2); });
            connect(m_remove, &QToolButton::clicked, this, [this]() { m_dialog->removeAt(m_index); });
        }
        m_editable = editable;
    }

    int index() const { return m_index; }
    void setLifted(bool lifted)
    {
        m_lifted = lifted;
        update();
    }
    // part: 0 the row, 1 its up button, 2 its down button.
    void focusPart(int part)
    {
        QWidget* target = part == 1 && m_up && m_up->isEnabled() ? static_cast<QWidget*>(m_up)
                        : part == 2 && m_down && m_down->isEnabled() ? static_cast<QWidget*>(m_down)
                                                                     : this;
        target->setFocus();
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        const Tokens& t = LobbyTheme::tokens();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (m_lifted)
            painter.setOpacity(0.35);
        const bool hover = underMouse() && m_editable;
        painter.setPen(QPen(hasFocus() ? t.accent : t.line, 1));
        painter.setBrush(hover ? t.raise : t.panel);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 7, 7);
    }

    void focusInEvent(QFocusEvent* event) override
    {
        QWidget::focusInEvent(event);
        m_dialog->showPage(m_index / std::max(1, m_dialog->m_info.PageStages));
        update();
    }
    void focusOutEvent(QFocusEvent* event) override
    {
        QWidget::focusOutEvent(event);
        update();
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (!m_editable)
            return QWidget::keyPressEvent(event);
        const bool alt = event->modifiers() & Qt::AltModifier;
        if (alt && event->key() == Qt::Key_Up && m_index > 0)
        {
            m_dialog->move(m_index, m_index - 1);
            m_dialog->focusRow(m_index - 1, 0);
        }
        else if (alt && event->key() == Qt::Key_Down)
        {
            m_dialog->move(m_index, m_index + 1);
            m_dialog->focusRow(m_index + 1, 0);
        }
        else if (event->key() == Qt::Key_Up)
            m_dialog->focusRow(m_index - 1, 0);
        else if (event->key() == Qt::Key_Down)
            m_dialog->focusRow(m_index + 1, 0);
        else if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)
            m_dialog->removeAt(m_index);
        else
            return QWidget::keyPressEvent(event);
        event->accept();
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (m_editable && event->button() == Qt::LeftButton)
            m_pressAt = event->position().toPoint();
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!m_editable || !(event->buttons() & Qt::LeftButton) || m_pressAt.isNull() ||
            (event->position().toPoint() - m_pressAt).manhattanLength() < QApplication::startDragDistance())
            return QWidget::mouseMoveEvent(event);
        auto* mime = new QMimeData;
        mime->setData(kRowMime, QByteArray::number(m_index));
        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        QPixmap picture = grab();
        drag->setPixmap(picture);
        drag->setHotSpot(m_pressAt);
        m_pressAt = QPoint();
        setLifted(true);
        QPointer<Row> self(this);
        drag->exec(Qt::MoveAction);
        if (self)
            self->setLifted(false);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        m_pressAt = QPoint();
        QWidget::mouseReleaseEvent(event);
    }

  private:
    StageListDialog* m_dialog;
    int m_index;
    uint8_t m_mode = 0;
    bool m_editable = false;
    bool m_lifted = false;
    QPoint m_pressAt;
    IconButton* m_up = nullptr;
    IconButton* m_down = nullptr;
    IconButton* m_remove = nullptr;
};

class StageListDialog::ListView : public QWidget
{
  public:
    ListView(StageListDialog* dialog, QScrollArea* scroll, QWidget* parent)
        : QWidget(parent), m_dialog(dialog), m_scroll(scroll)
    {
        setAcceptDrops(true);
        m_lay = new QVBoxLayout(this);
        m_lay->setContentsMargins(0, 0, 6, 0);
        m_lay->setSpacing(5);
        m_indicator = new QWidget(this);
        m_indicator->setFixedHeight(3);
        m_indicator->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_indicator->setAutoFillBackground(true);
        m_indicator->hide();
        // Near an edge while dragging, the list scrolls.
        m_autoScroll = new QTimer(this);
        m_autoScroll->setInterval(30);
        connect(m_autoScroll, &QTimer::timeout, this, [this]() {
            const QPoint at = m_scroll->viewport()->mapFromGlobal(QCursor::pos());
            const int h = m_scroll->viewport()->height();
            QScrollBar* bar = m_scroll->verticalScrollBar();
            if (at.y() < 28)
                bar->setValue(bar->value() - 10);
            else if (at.y() > h - 28)
                bar->setValue(bar->value() + 10);
        });
    }

    QVBoxLayout* rows() const { return m_lay; }

  protected:
    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (event->mimeData()->hasFormat(kRowMime))
        {
            event->acceptProposedAction();
            m_autoScroll->start();
        }
    }
    void dragMoveEvent(QDragMoveEvent* event) override
    {
        if (!event->mimeData()->hasFormat(kRowMime))
            return;
        event->acceptProposedAction();
        const int y = event->position().toPoint().y();
        int at = 0;
        int lineY = 0;
        const QList<Row*>& rows = m_dialog->m_rows;
        for (at = 0; at < rows.size(); at++)
        {
            const QRect g = rows.at(at)->geometry();
            if (y < g.center().y())
            {
                lineY = g.top() - 4;
                break;
            }
            lineY = g.bottom() + 2;
        }
        m_target = at;
        QPalette palette = m_indicator->palette();
        palette.setColor(QPalette::Window, LobbyTheme::tokens().accent);
        m_indicator->setPalette(palette);
        m_indicator->setGeometry(0, std::max(0, lineY), width() - 6, 3);
        m_indicator->show();
        m_indicator->raise();
    }
    void dragLeaveEvent(QDragLeaveEvent*) override
    {
        m_indicator->hide();
        m_autoScroll->stop();
    }
    void dropEvent(QDropEvent* event) override
    {
        m_indicator->hide();
        m_autoScroll->stop();
        if (!event->mimeData()->hasFormat(kRowMime))
            return;
        bool ok = false;
        const int from = event->mimeData()->data(kRowMime).toInt(&ok);
        if (!ok)
            return;
        int to = m_target > from ? m_target - 1 : m_target;
        event->acceptProposedAction();
        // After the drag returns: the dragged row is deleted by the rebuild.
        QTimer::singleShot(0, m_dialog, [dialog = m_dialog, from, to]() {
            dialog->move(from, to);
            dialog->focusRow(to, 0);
        });
    }

  private:
    StageListDialog* m_dialog;
    QScrollArea* m_scroll;
    QVBoxLayout* m_lay;
    QWidget* m_indicator;
    QTimer* m_autoScroll;
    int m_target = 0;
};

class StageListDialog::Preview : public QWidget
{
  public:
    Preview(StageListDialog* dialog, QWidget* parent) : QWidget(parent), m_dialog(dialog)
    {
        setMouseTracking(true);
        const CoreStageListInfo& info = dialog->m_info;
        const int columns = std::max(1, info.PageColumns);
        const int rows = (info.PageIcons + columns - 1) / columns;
        setFixedSize(columns * kIconW + (columns - 1) * kGap, rows * kIconH + (rows - 1) * kGap);
        setObjectName("StageSelectPreview");
        setAccessibleName(StageListDialog::tr("How the stage select's page looks"));
    }

    void setPage(int page)
    {
        m_page = page;
        update();
    }
    void setHighlight(int index)
    {
        m_highlight = index;
        update();
    }

  protected:
    // The entry on a page's icon, or -1 for Random.
    int entryAt(int slot) const
    {
        const CoreStageListInfo& info = m_dialog->m_info;
        const int index = m_page * info.PageStages + slot;
        return slot < info.PageStages && index < int(m_dialog->m_entries.size()) ? index : -1;
    }
    QRect cell(int slot) const
    {
        const int columns = std::max(1, m_dialog->m_info.PageColumns);
        return QRect((slot % columns) * (kIconW + kGap), (slot / columns) * (kIconH + kGap), kIconW, kIconH);
    }

    void paintEvent(QPaintEvent*) override
    {
        const CoreStageListInfo& info = m_dialog->m_info;
        const Tokens& t = LobbyTheme::tokens();
        QPainter painter(this);
        const QImage random = m_dialog->m_icons.value(info.RandomStage);
        for (int slot = 0; slot < info.PageIcons; slot++)
        {
            const int index = entryAt(slot);
            const QRect r = cell(slot);
            if (index >= 0)
            {
                const int stage = m_dialog->m_entries[size_t(index)].Stage;
                const CoreStageListStage* s = info.stage(stage);
                paintStageIcon(painter, r, m_dialog->m_icons.value(stage), s ? QString::fromStdString(s->Name) : QString());
            }
            else
            {
                painter.save();
                painter.setOpacity(0.55);
                paintStageIcon(painter, r, random, QStringLiteral("?"));
                painter.restore();
            }
            if (index >= 0 && index == m_highlight)
            {
                painter.save();
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setPen(QPen(t.accent, 2));
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(QRectF(r).adjusted(1, 1, -1, -1), 3, 3);
                painter.restore();
            }
        }
    }

    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::ToolTip)
        {
            auto* help = static_cast<QHelpEvent*>(event);
            for (int slot = 0; slot < m_dialog->m_info.PageIcons; slot++)
            {
                if (!cell(slot).contains(help->pos()))
                    continue;
                const int index = entryAt(slot);
                const CoreStageListStage* s = index >= 0 ? m_dialog->m_info.stage(m_dialog->m_entries[size_t(index)].Stage) : nullptr;
                QToolTip::showText(help->globalPos(), s ? QStringLiteral("%1. %2").arg(index + 1).arg(QString::fromStdString(s->Name))
                                                        : StageListDialog::tr("Random"), this);
                return true;
            }
            QToolTip::hideText();
            return true;
        }
        return QWidget::event(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        for (int slot = 0; slot < m_dialog->m_info.PageIcons; slot++)
        {
            const int index = entryAt(slot);
            if (index >= 0 && cell(slot).contains(event->position().toPoint()))
                m_dialog->focusRow(index, 0);
        }
    }

  private:
    static constexpr int kGap = 4;
    StageListDialog* m_dialog;
    int m_page = 0;
    int m_highlight = -1;
};

//
// StageListDialog
//

StageListDialog::StageListDialog(const QString& gameName, CoreStageListInfo info, const QString& code, bool editable,
                                 const QString& romMd5, const QString& romPath, QWidget* parent)
    : QDialog(parent), m_info(std::move(info)), m_editable(editable), m_romMd5(romMd5)
{
    setWindowTitle(editable ? tr("Stage List") : tr("The Host's Stage List"));
    setWindowIcon(QIcon(":Resource/RMG.png"));
    m_icons = stageListIcons(romMd5, QString::fromStdString(m_info.OptionId), romPath);
    m_entries = stageListEntries(m_info, code);
    m_saved = savedLists(romMd5, m_info);
    if (editable)
    {
        setMinimumSize(900, 560);
        resize(1140, 720);
    }
    else
    {
        setMinimumSize(440, 480);
        resize(500, 660);
    }

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* header = new LobbyBand(this);
    auto* headerLay = new QHBoxLayout(header);
    headerLay->setContentsMargins(18, 12, 18, 12);
    headerLay->setSpacing(12);
    auto* titles = new QVBoxLayout;
    titles->setSpacing(1);
    auto* title = new QLabel((editable ? tr("Stage List") : tr("The Host's Stage List")).toUpper(), header);
    title->setFont(LobbyTheme::pixelFont(15, 0.5));
    titles->addWidget(title);
    auto* gameLabel = new QLabel(gameName.isEmpty() ? QStringLiteral("—") : gameName, header);
    gameLabel->setFont(LobbyTheme::uiFont(11));
    gameLabel->setProperty("role", "muted");
    titles->addWidget(gameLabel);
    headerLay->addLayout(titles, 1);
    root->addWidget(header);
    root->addWidget(rule(this));

    auto* body = new QVBoxLayout;
    body->setContentsMargins(18, 12, 18, 10);
    body->setSpacing(10);
    auto* intro = small(editable
        ? tr("Click stages to put them on the list, in the order the stage select will show them: %1 to a page, "
             "with Random on the last icon. Hazards and movement are set per stage on the list.").arg(m_info.PageStages)
        : tr("The stages the host picked, in the order the stage select shows them, each with its hazards "
             "and movement as the host set them."), "muted", this);
    body->addWidget(intro);
    if (m_icons.isEmpty())
    {
        auto* noIcons = small(tr("The stage icons come from your copy of the game, which couldn't be read here, "
                                 "so the stages show by name only."), "faint", this);
        noIcons->setObjectName("StageListNoIcons");
        body->addWidget(noIcons);
    }
    buildTopBar(body);

    auto* panels = new QHBoxLayout;
    panels->setSpacing(16);
    if (editable)
        panels->addWidget(buildBrowsePanel(), 1);
    QWidget* listPanel = buildListPanel();
    if (editable)
        listPanel->setFixedWidth(436);
    panels->addWidget(listPanel, editable ? 0 : 1);
    body->addLayout(panels, 1);

    // Save as: a name field above the footer, as in the Custom view.
    m_saveBar = new QWidget(this);
    m_saveBar->setObjectName("StageListSaveBar");
    auto* barLay = new QVBoxLayout(m_saveBar);
    barLay->setContentsMargins(0, 2, 0, 0);
    barLay->setSpacing(5);
    auto* fieldRow = new QHBoxLayout;
    fieldRow->setSpacing(8);
    m_saveName = new QLineEdit(m_saveBar);
    m_saveName->setObjectName("StageListSaveName");
    m_saveName->setMaxLength(MaxSavedNameLength);
    m_saveName->setMinimumHeight(30);
    m_saveName->setPlaceholderText(tr("A name, like Weekly bracket"));
    m_saveName->setAccessibleName(tr("Name for this stage list"));
    fieldRow->addWidget(m_saveName, 1);
    m_saveConfirm = button(tr("Save"), LobbyButton::Kind::Primary, LobbyButton::Size::Small, "StageListSaveConfirm", m_saveBar);
    fieldRow->addWidget(m_saveConfirm);
    auto* saveCancel = button(tr("Cancel"), LobbyButton::Kind::Ghost, LobbyButton::Size::Small, "StageListSaveCancel", m_saveBar);
    fieldRow->addWidget(saveCancel);
    barLay->addLayout(fieldRow);
    m_saveMessage = small(QString(), "ink2", m_saveBar);
    m_saveMessage->setObjectName("StageListSaveMessage");
    m_saveMessage->setVisible(false);
    barLay->addWidget(m_saveMessage);
    m_saveBar->setVisible(false);
    body->addWidget(m_saveBar);
    connect(m_saveName, &QLineEdit::textChanged, this, &StageListDialog::checkSaveName);
    connect(m_saveConfirm, &QPushButton::clicked, this, &StageListDialog::saveAs);
    connect(saveCancel, &QPushButton::clicked, this, &StageListDialog::closeSaveBar);

    root->addLayout(body, 1);
    root->addWidget(rule(this));

    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(18, 11, 18, 15);
    footer->setSpacing(10);
    m_saveAsButton = button(tr("Save as..."), LobbyButton::Kind::Secondary, LobbyButton::Size::Large, "StageListSaveAs", this);
    m_saveAsButton->setIconName(QStringLiteral("add-line"));
    footer->addWidget(m_saveAsButton);
    connect(m_saveAsButton, &QPushButton::clicked, this, &StageListDialog::openSaveBar);
    footer->addStretch(1);
    if (editable)
    {
        auto* cancel = button(tr("Cancel"), LobbyButton::Kind::Ghost, LobbyButton::Size::Large, "StageListCancel", this);
        m_doneButton = button(tr("Done"), LobbyButton::Kind::Primary, LobbyButton::Size::Large, "StageListDone", this);
        m_doneButton->setIconName(QStringLiteral("check-line"));
        m_doneButton->setDefault(true);
        footer->addWidget(cancel);
        footer->addWidget(m_doneButton);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(m_doneButton, &QPushButton::clicked, this, &QDialog::accept);
        m_mainButton = m_doneButton;
    }
    else
    {
        auto* close = button(tr("Close"), LobbyButton::Kind::Primary, LobbyButton::Size::Large, "StageListClose", this);
        close->setDefault(true);
        footer->addWidget(close);
        connect(close, &QPushButton::clicked, this, &QDialog::accept);
        m_mainButton = close;
    }
    root->addLayout(footer);

    changed();
    if (m_search)
        QTimer::singleShot(0, m_search, [search = m_search]() { search->setFocus(); });
}

StageListDialog::~StageListDialog() = default;

void StageListDialog::buildTopBar(QVBoxLayout* body)
{
    auto* bar = new QHBoxLayout;
    bar->setSpacing(8);
    if (m_editable)
    {
        m_startCombo = new QComboBox(this);
        m_startCombo->setObjectName("StageListStartCombo");
        m_startCombo->setMinimumHeight(30);
        m_startCombo->setMinimumWidth(240);
        m_startCombo->setAccessibleName(tr("Start from a list"));
        m_startCombo->setToolTip(tr("The game's preset lists and your saved lists. Picking one replaces the list (you can undo it)."));
        bar->addWidget(m_startCombo);
        connect(m_startCombo, QOverload<int>::of(&QComboBox::activated), this, &StageListDialog::startFrom);
    }
    m_savedAs = new QLabel(this);
    m_savedAs->setObjectName("StageListSavedAs");
    m_savedAs->setFont(LobbyTheme::uiFont(11));
    m_savedAs->setProperty("role", "muted");
    bar->addWidget(m_savedAs);
    if (m_editable)
    {
        m_renameButton = button(tr("Rename"), LobbyButton::Kind::Ghost, LobbyButton::Size::Compact, "StageListRename", this);
        m_deleteButton = button(tr("Delete"), LobbyButton::Kind::Ghost, LobbyButton::Size::Compact, "StageListDelete", this);
        bar->addWidget(m_renameButton);
        bar->addWidget(m_deleteButton);
        connect(m_renameButton, &QPushButton::clicked, this, &StageListDialog::renameSaved);
        connect(m_deleteButton, &QPushButton::clicked, this, &StageListDialog::deleteSaved);
    }
    bar->addStretch(1);
    if (m_editable)
    {
        m_pasteButton = button(tr("Paste code..."), LobbyButton::Kind::Secondary, LobbyButton::Size::Small, "StageListPaste", this);
        m_pasteButton->setToolTip(tr("Use a stage list someone shared as a code (STG-...)."));
        bar->addWidget(m_pasteButton);
        connect(m_pasteButton, &QPushButton::clicked, this, &StageListDialog::openPasteBar);
    }
    m_copyButton = button(tr("Copy code"), LobbyButton::Kind::Secondary, LobbyButton::Size::Small, "StageListCopy", this);
    m_copyButton->setIconName(QStringLiteral("file-copy-line"));
    m_copyButton->setToolTip(tr("Copy a short code for this list, to share it: whoever pastes it gets the same "
                                "stages, order and settings."));
    bar->addWidget(m_copyButton);
    connect(m_copyButton, &QPushButton::clicked, this, &StageListDialog::copyCode);
    body->addLayout(bar);

    // Paste: a field under the bar.
    m_pasteBar = new QWidget(this);
    m_pasteBar->setObjectName("StageListPasteBar");
    auto* pasteLay = new QVBoxLayout(m_pasteBar);
    pasteLay->setContentsMargins(0, 0, 0, 0);
    pasteLay->setSpacing(4);
    auto* pasteRow = new QHBoxLayout;
    pasteRow->setSpacing(8);
    m_pasteEdit = new QLineEdit(m_pasteBar);
    m_pasteEdit->setObjectName("StageListPasteEdit");
    m_pasteEdit->setMinimumHeight(30);
    m_pasteEdit->setPlaceholderText(tr("Paste a stage list code, like %1-220C-6YA8-2T5V-W0")
                                        .arg(QString::fromStdString(m_info.CodePrefix)));
    m_pasteEdit->setAccessibleName(tr("Stage list code"));
    pasteRow->addWidget(m_pasteEdit, 1);
    auto* use = button(tr("Use code"), LobbyButton::Kind::Primary, LobbyButton::Size::Small, "StageListPasteUse", m_pasteBar);
    auto* pasteCancel = button(tr("Cancel"), LobbyButton::Kind::Ghost, LobbyButton::Size::Small, "StageListPasteCancel", m_pasteBar);
    pasteRow->addWidget(use);
    pasteRow->addWidget(pasteCancel);
    pasteLay->addLayout(pasteRow);
    m_pasteError = small(QString(), "bad", m_pasteBar);
    m_pasteError->setObjectName("StageListPasteError");
    m_pasteError->setVisible(false);
    pasteLay->addWidget(m_pasteError);
    m_pasteBar->setVisible(false);
    body->addWidget(m_pasteBar);
    connect(use, &QPushButton::clicked, this, &StageListDialog::usePastedCode);
    connect(pasteCancel, &QPushButton::clicked, this, &StageListDialog::closePasteBar);
    connect(m_pasteEdit, &QLineEdit::textChanged, m_pasteError, &QWidget::hide);
    connect(m_pasteEdit, &QLineEdit::returnPressed, this, &StageListDialog::usePastedCode);

    // What just happened, with an undo where it can be undone.
    m_noteBar = new QWidget(this);
    m_noteBar->setObjectName("StageListNoteBar");
    auto* noteLay = new QHBoxLayout(m_noteBar);
    noteLay->setContentsMargins(0, 0, 0, 0);
    noteLay->setSpacing(8);
    m_note = small(QString(), "ink2", m_noteBar);
    m_note->setObjectName("StageListNote");
    m_note->setTextInteractionFlags(Qt::TextSelectableByMouse);
    noteLay->addWidget(m_note, 1);
    m_undoButton = button(tr("Undo"), LobbyButton::Kind::Ghost, LobbyButton::Size::Compact, "StageListUndo", m_noteBar);
    m_undoButton->setIconName(QStringLiteral("arrow-go-back-line"));
    noteLay->addWidget(m_undoButton, 0, Qt::AlignTop);
    connect(m_undoButton, &QPushButton::clicked, this, &StageListDialog::undo);
    m_noteBar->setVisible(false);
    body->addWidget(m_noteBar);
}

QWidget* StageListDialog::buildBrowsePanel()
{
    auto* panel = new QWidget(this);
    auto* lay = new QVBoxLayout(panel);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);

    auto* tools = new QHBoxLayout;
    tools->setSpacing(10);
    m_search = new QLineEdit(panel);
    m_search->setObjectName("StageListSearch");
    m_search->setMinimumHeight(30);
    m_search->setClearButtonEnabled(true);
    m_search->setPlaceholderText(tr("Search stages"));
    m_search->setAccessibleName(tr("Search stages"));
    m_search->addAction(QIcon(LobbyTheme::icon(QStringLiteral("search-line"), LobbyTheme::tokens().muted, 16, devicePixelRatioF())),
                        QLineEdit::LeadingPosition);
    tools->addWidget(m_search, 1);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_filter = text.trimmed();
        applyFilter();
    });

    auto* seg = new QFrame(panel);
    seg->setObjectName("Seg");
    auto* segLay = new QHBoxLayout(seg);
    segLay->setContentsMargins(2, 2, 2, 2);
    segLay->setSpacing(2);
    const QList<std::pair<QString, QString>> kinds = {
        { tr("All", "filter: every stage"), tr("Every stage") },
        { tr("Static", "filter: stages with no hazards and no movement"), tr("Stages with no hazards and no movement") },
        { tr("Hazards", "filter: stages with hazards"), tr("Stages with hazards") },
        { tr("Moving", "filter: stages that move"), tr("Stages that move") },
    };
    for (int i = 0; i < kinds.size(); i++)
    {
        auto* b = new QPushButton(kinds.at(i).first, seg);
        b->setProperty("seg", true);
        b->setCheckable(true);
        b->setChecked(i == 0);
        b->setToolTip(kinds.at(i).second);
        b->setObjectName("StageListFilter");
        b->setAccessibleName(kinds.at(i).second);
        b->setCursor(Qt::PointingHandCursor);
        segLay->addWidget(b);
        m_filterButtons << b;
        connect(b, &QPushButton::clicked, this, [this, i]() {
            m_filterKind = i;
            for (int k = 0; k < m_filterButtons.size(); k++)
                m_filterButtons.at(k)->setChecked(k == i);
            applyFilter();
        });
    }
    tools->addWidget(seg);
    lay->addLayout(tools);

    auto* scroll = new QScrollArea(panel);
    scroll->setObjectName("StageListBrowse");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->viewport()->setAutoFillBackground(false);
    auto* content = new QWidget(scroll);
    auto* sections = new QVBoxLayout(content);
    sections->setContentsMargins(0, 2, 8, 8);
    sections->setSpacing(14);

    // The game's own pages, in its order; each base stage carries its variants.
    std::vector<std::string> groups;
    for (const CoreStageListText& group : m_info.Groups)
        groups.push_back(group.Id);
    groups.push_back(std::string());
    for (size_t g = 0; g < groups.size(); g++)
    {
        const std::string& groupId = groups[g];
        const bool any = std::any_of(m_info.Stages.begin(), m_info.Stages.end(), [&](const CoreStageListStage& s) {
            return s.VariantOf < 0 && s.Group == groupId;
        });
        if (!any)
            continue;
        auto* section = new QWidget(content);
        auto* sectionLay = new QVBoxLayout(section);
        sectionLay->setContentsMargins(0, 0, 0, 0);
        sectionLay->setSpacing(7);
        auto* head = new QHBoxLayout;
        head->setSpacing(8);
        if (g < m_info.Groups.size())
        {
            auto* page = new QLabel(tr("Page %1").arg(g + 1).toUpper(), section);
            page->setFont(LobbyTheme::pixelFont(11, 0.6));
            page->setProperty("role", "heading");
            head->addWidget(page);
            auto* label = new QLabel(QString::fromStdString(m_info.Groups[g].Label), section);
            label->setFont(LobbyTheme::uiFont(11));
            label->setProperty("role", "muted");
            head->addWidget(label);
            section->setToolTip(tr("Page %1 of the game's own stage select").arg(g + 1));
        }
        else
        {
            head->addWidget(heading(tr("Other stages"), section));
        }
        head->addStretch(1);
        sectionLay->addLayout(head);
        auto* gridHolder = new QWidget(section);
        auto* grid = new FlowLayout(gridHolder, 8, 8);
        for (const CoreStageListStage& s : m_info.Stages)
        {
            if (s.VariantOf >= 0 || s.Group != groupId)
                continue;
            auto* card = new Card(s, m_icons.value(s.Id), gridHolder);
            const QString what = hazardWords(s);
            card->setToolTip(what.isEmpty() ? QString::fromStdString(s.Name)
                                            : QStringLiteral("%1\n%2").arg(QString::fromStdString(s.Name), what));
            for (const CoreStageListStage& v : m_info.Stages)
            {
                if (v.VariantOf != s.Id)
                    continue;
                QString kind;
                for (const CoreStageListText& k : m_info.Kinds)
                {
                    if (k.Id == v.Kind)
                        kind = QString::fromStdString(k.Label);
                }
                QStringList tip = { QString::fromStdString(v.Name) };
                if (!kind.isEmpty())
                    tip << kind;
                const QString vwhat = hazardWords(v);
                tip << (vwhat.isEmpty() ? tr("No hazards or movement") : vwhat);
                QPushButton* chip = card->addChip(v, tip.join(QLatin1Char('\n')));
                const int id = v.Id;
                // toggled, not clicked: accessibility tools toggle a checkable button
                // without clicking it. The dialog's own updates are signal-blocked.
                connect(chip, &QPushButton::toggled, this, [this, id]() { toggleStage(id); });
            }
            const int id = s.Id;
            connect(card, &QAbstractButton::toggled, this, [this, id]() { toggleStage(id); });
            grid->addWidget(card);
            m_cards << card;
        }
        sectionLay->addWidget(gridHolder);
        sections->addWidget(section);
        m_sections << section;
    }
    m_noMatch = small(QString(), "muted", content);
    m_noMatch->setObjectName("StageListNoMatch");
    m_noMatch->setAlignment(Qt::AlignCenter);
    m_noMatch->setVisible(false);
    sections->addWidget(m_noMatch);
    sections->addStretch(1);
    scroll->setWidget(content);
    content->setAutoFillBackground(false);
    lay->addWidget(scroll, 1);
    return panel;
}

QWidget* StageListDialog::buildListPanel()
{
    auto* panel = new LobbyCard(this);
    panel->setObjectName("StageListPanel");
    auto* lay = new QVBoxLayout(panel);
    lay->setContentsMargins(14, 12, 10, 12);
    lay->setSpacing(9);

    auto* head = new QHBoxLayout;
    head->addWidget(heading(m_editable ? tr("Your list") : tr("The list"), panel));
    head->addStretch(1);
    m_count = new QLabel(panel);
    m_count->setObjectName("StageListCount");
    m_count->setFont(LobbyTheme::uiFont(12, QFont::DemiBold));
    head->addWidget(m_count);
    lay->addLayout(head);

    // The stage select as it will look, a page at a time.
    auto* previewBox = new QWidget(panel);
    auto* previewLay = new QVBoxLayout(previewBox);
    previewLay->setContentsMargins(0, 0, 0, 0);
    previewLay->setSpacing(5);
    auto* previewHead = new QHBoxLayout;
    previewHead->addWidget(heading(tr("On the stage select"), previewBox));
    previewHead->addStretch(1);
    m_pager = new QWidget(previewBox);
    auto* pagerLay = new QHBoxLayout(m_pager);
    pagerLay->setContentsMargins(0, 0, 0, 0);
    pagerLay->setSpacing(4);
    auto* prev = new IconButton(QStringLiteral("arrow-left-s-line"), 22, m_pager);
    prev->setObjectName("StagePreviewPrev");
    prev->setAccessibleName(tr("Previous page"));
    auto* next = new IconButton(QStringLiteral("arrow-right-s-line"), 22, m_pager);
    next->setObjectName("StagePreviewNext");
    next->setAccessibleName(tr("Next page"));
    m_pageLabel = new QLabel(m_pager);
    m_pageLabel->setObjectName("StagePreviewPage");
    m_pageLabel->setFont(LobbyTheme::uiFont(11));
    m_pageLabel->setProperty("role", "muted");
    pagerLay->addWidget(prev);
    pagerLay->addWidget(m_pageLabel);
    pagerLay->addWidget(next);
    m_prevPage = prev;
    m_nextPage = next;
    connect(prev, &QToolButton::clicked, this, [this]() { showPage(m_page - 1); });
    connect(next, &QToolButton::clicked, this, [this]() { showPage(m_page + 1); });
    previewHead->addWidget(m_pager);
    previewLay->addLayout(previewHead);
    m_preview = new Preview(this, previewBox);
    previewLay->addWidget(m_preview, 0, Qt::AlignHCenter);
    lay->addWidget(previewBox);
    lay->addWidget(rule(panel));

    m_listScroll = new QScrollArea(panel);
    m_listScroll->setObjectName("StageListRows");
    m_listScroll->setFrameShape(QFrame::NoFrame);
    m_listScroll->setWidgetResizable(true);
    m_listScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_listScroll->viewport()->setAutoFillBackground(false);
    m_listView = new ListView(this, m_listScroll, m_listScroll);
    m_listScroll->setWidget(m_listView);
    m_listView->setAutoFillBackground(false);
    lay->addWidget(m_listScroll, 1);

    m_empty = new QWidget(panel);
    auto* emptyLay = new QVBoxLayout(m_empty);
    emptyLay->setContentsMargins(10, 20, 10, 20);
    emptyLay->setSpacing(6);
    emptyLay->addWidget(new IconLabel(QStringLiteral("add-line"), LobbyTheme::Role::Faint, 22, m_empty), 0, Qt::AlignHCenter);
    auto* emptyText = small(m_editable ? tr("No stages yet. Click a stage on the left to put it on the list, or start "
                                            "from a ready-made list above.")
                                       : tr("The host hasn't picked any stages."), "muted", m_empty);
    emptyText->setAlignment(Qt::AlignCenter);
    emptyLay->addWidget(emptyText);
    emptyLay->addStretch(1);
    lay->addWidget(m_empty, 1);

    if (m_editable)
    {
        auto* bulk = new QHBoxLayout;
        bulk->setSpacing(6);
        const auto add = [&](const QString& text, const char* name, const QString& tip, std::function<void()> action) {
            auto* b = button(text, LobbyButton::Kind::Ghost, LobbyButton::Size::Compact, name, panel);
            b->setToolTip(tip);
            bulk->addWidget(b);
            m_bulkButtons << b;
            connect(b, &QPushButton::clicked, this, std::move(action));
            return b;
        };
        add(tr("All hazards off"), "StageListHazardsOff", tr("Turn hazards off on every stage that has them."),
            [this]() { setAll(0, CoreStageModeHazardsOff, tr("Turned hazards off on every stage.")); });
        add(tr("All movement off"), "StageListMovementOff", tr("Stop every stage that moves."),
            [this]() { setAll(0, CoreStageModeMovementOff, tr("Turned movement off on every stage.")); });
        add(tr("All on"), "StageListAllOn", tr("Hazards and movement on, on every stage."),
            [this]() { setAll(CoreStageModeHazardsOff | CoreStageModeMovementOff, 0, tr("Turned hazards and movement on everywhere.")); });
        bulk->addStretch(1);
        add(tr("Clear"), "StageListClear", tr("Take every stage off the list."),
            [this]() { replaceList({}, tr("Cleared the list.")); });
        lay->addLayout(bulk);
    }
    return panel;
}

bool StageListDialog::inList(int stage) const
{
    return indexOf(stage) >= 0;
}

int StageListDialog::indexOf(int stage) const
{
    for (size_t i = 0; i < m_entries.size(); i++)
    {
        if (m_entries[i].Stage == stage)
            return int(i);
    }
    return -1;
}

void StageListDialog::toggleStage(int stage)
{
    const int at = indexOf(stage);
    if (at >= 0)
    {
        m_entries.erase(m_entries.begin() + at);
        m_canUndo = false;
        m_noteBar->setVisible(false);
        changed();
        return;
    }
    if (int(m_entries.size()) >= m_info.MaxStages)
    {
        showNote(tr("A list holds at most %1 stages: %2 pages of %3 on the stage select, each with Random on "
                    "its last icon. Take one off to add another.")
                     .arg(m_info.MaxStages).arg(m_info.MaxPages).arg(m_info.PageStages), false, true);
        refreshCards();
        return;
    }
    // A new stage plays as it normally does.
    m_entries.push_back({ uint8_t(stage), 0 });
    m_canUndo = false;
    m_noteBar->setVisible(false);
    changed();
    // Where it went, so a long list's new stage isn't out of sight.
    const int index = int(m_entries.size()) - 1;
    showPage(index / std::max(1, m_info.PageStages));
    QTimer::singleShot(0, this, [this, index]() {
        if (index < m_rows.size())
            m_listScroll->ensureWidgetVisible(m_rows.at(index), 0, 8);
    });
}

void StageListDialog::setMode(int index, uint8_t mode)
{
    if (index < 0 || index >= int(m_entries.size()))
        return;
    std::vector<CoreStageListEntry> one = { { m_entries[size_t(index)].Stage, mode } };
    CoreNormalizeStageList(m_info, one);
    m_entries[size_t(index)].Mode = one[0].Mode;
    // Only the status changes: the row already shows the switch where it is.
    refreshStatus();
    refreshSavedActions();
}

void StageListDialog::move(int from, int to)
{
    const int n = int(m_entries.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to)
        return;
    const CoreStageListEntry entry = m_entries[size_t(from)];
    m_entries.erase(m_entries.begin() + from);
    m_entries.insert(m_entries.begin() + to, entry);
    m_noteBar->setVisible(false);
    changed();
}

void StageListDialog::removeAt(int index)
{
    if (index < 0 || index >= int(m_entries.size()))
        return;
    m_entries.erase(m_entries.begin() + index);
    m_noteBar->setVisible(false);
    changed();
    focusRow(std::min(index, int(m_entries.size()) - 1), 0);
}

void StageListDialog::replaceList(const std::vector<CoreStageListEntry>& entries, const QString& note)
{
    m_undo = m_entries;
    m_entries = entries;
    changed();
    showPage(0);
    showNote(note, true);
}

void StageListDialog::setAll(uint8_t clear, uint8_t set, const QString& note)
{
    if (m_entries.empty())
        return;
    std::vector<CoreStageListEntry> next = m_entries;
    for (CoreStageListEntry& entry : next)
        entry.Mode = uint8_t((entry.Mode & ~clear) | set);
    CoreNormalizeStageList(m_info, next);
    if (next == m_entries)
    {
        showNote(note, false);
        return;
    }
    m_undo = m_entries;
    m_entries = next;
    changed();
    showNote(note, true);
}

void StageListDialog::undo()
{
    if (!m_canUndo)
        return;
    std::swap(m_entries, m_undo);
    m_canUndo = false;
    m_noteBar->setVisible(false);
    changed();
}

void StageListDialog::showNote(const QString& text, bool undoable, bool bad)
{
    m_canUndo = undoable;
    m_note->setText(text);
    setRole(m_note, bad ? "bad" : "ink2");
    m_undoButton->setVisible(undoable && m_editable);
    m_noteBar->setVisible(!text.isEmpty());
}

void StageListDialog::changed()
{
    refreshCards();
    rebuildRows();
    refreshStatus();
    refreshSavedActions();
    if (m_saveBar && !m_saveBar->isHidden())
        checkSaveName();
}

void StageListDialog::refreshCards()
{
    for (Card* card : std::as_const(m_cards))
    {
        card->setPosition(indexOf(card->base().Id) + 1);
        const QList<QPushButton*>& chips = card->chips();
        for (int i = 0; i < chips.size(); i++)
        {
            const int at = indexOf(card->chipIds().at(i));
            const QSignalBlocker blocker(chips.at(i));
            chips.at(i)->setChecked(at >= 0);
            chips.at(i)->setAccessibleDescription(at >= 0 ? tr("Number %1 on your list").arg(at + 1) : QString());
        }
    }
}

void StageListDialog::rebuildRows()
{
    QVBoxLayout* lay = m_listView->rows();
    while (QLayoutItem* item = lay->takeAt(0))
    {
        if (QWidget* widget = item->widget())
        {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    m_rows.clear();
    const int pageStages = std::max(1, m_info.PageStages);
    const int pages = m_info.pagesFor(int(m_entries.size()));
    for (size_t i = 0; i < m_entries.size(); i++)
    {
        // Each page of the stage select starts a section.
        if (int(i) % pageStages == 0)
        {
            const int page = int(i) / pageStages + 1;
            auto* header = new QWidget(m_listView);
            auto* headerLay = new QHBoxLayout(header);
            headerLay->setContentsMargins(2, i == 0 ? 0 : 6, 0, 0);
            headerLay->setSpacing(8);
            auto* label = new QLabel(tr("Page %1").arg(page).toUpper(), header);
            label->setFont(LobbyTheme::pixelFont(11, 0.6));
            label->setProperty("role", "heading");
            headerLay->addWidget(label);
            if (pages > 1 || int(m_entries.size()) < pageStages)
            {
                const int onPage = std::min(pageStages, int(m_entries.size()) - (page - 1) * pageStages);
                auto* sub = new QLabel(onPage == pageStages ? tr("%1 + Random").arg(stageCountText(onPage))
                                                            : tr("%1, Random on the other icons").arg(stageCountText(onPage)),
                                       header);
                sub->setFont(LobbyTheme::uiFont(10));
                sub->setProperty("role", "faint");
                headerLay->addWidget(sub);
            }
            headerLay->addStretch(1);
            lay->addWidget(header);
        }
        const CoreStageListEntry& entry = m_entries[i];
        const CoreStageListStage* stage = m_info.stage(entry.Stage);
        if (!stage)
            continue;
        const CoreStageListStage* base = stage->VariantOf >= 0 ? m_info.stage(stage->VariantOf) : nullptr;
        auto* row = new Row(this, int(i), *stage, base, entry.Mode, m_icons.value(entry.Stage), m_editable,
                            int(m_entries.size()), m_listView);
        lay->addWidget(row);
        m_rows << row;
    }
    lay->addStretch(1);
    m_empty->setVisible(m_entries.empty());
    m_listScroll->setVisible(!m_entries.empty());
}

void StageListDialog::refreshStatus()
{
    const int n = int(m_entries.size());
    m_count->setText(stageListCountText(m_info, n));
    m_count->setToolTip(tr("Up to %1 stages, each once.").arg(m_info.MaxStages));
    for (LobbyButton* b : std::as_const(m_bulkButtons))
        b->setEnabled(n > 0);
    m_copyButton->setEnabled(n > 0);
    m_copyButton->setToolTip(n > 0 ? tr("Copy a short code for this list, to share it: whoever pastes it gets the "
                                        "same stages, order and settings.")
                                   : tr("Put some stages on the list first."));
    const int pages = std::max(1, m_info.pagesFor(n));
    if (m_page >= pages)
        m_page = pages - 1;
    showPage(m_page);
}

void StageListDialog::showPage(int page)
{
    const int pages = std::max(1, m_info.pagesFor(int(m_entries.size())));
    m_page = std::clamp(page, 0, pages - 1);
    m_preview->setPage(m_page);
    m_pageLabel->setText(tr("Page %1 of %2").arg(m_page + 1).arg(pages));
    m_prevPage->setEnabled(m_page > 0);
    m_nextPage->setEnabled(m_page < pages - 1);
    m_pager->setVisible(pages > 1);
    int highlight = -1;
    for (Row* row : std::as_const(m_rows))
    {
        if (row->hasFocus())
            highlight = row->index();
    }
    m_preview->setHighlight(highlight);
}

void StageListDialog::focusRow(int index, int part)
{
    if (index < 0)
        return;
    QTimer::singleShot(0, this, [this, index, part]() {
        if (index >= m_rows.size())
            return;
        Row* row = m_rows.at(index);
        row->focusPart(part);
        m_listScroll->ensureWidgetVisible(row, 0, 8);
        m_preview->setHighlight(index);
    });
}

void StageListDialog::applyFilter()
{
    const QString needle = m_filter.toCaseFolded();
    const auto passes = [this](const CoreStageListStage& s) {
        switch (m_filterKind)
        {
        case 1: return !s.Hazards && !s.Movement;
        case 2: return s.Hazards;
        case 3: return s.Movement;
        default: return true;
        }
    };
    const auto matches = [&](const CoreStageListStage& s) {
        if (needle.isEmpty())
            return true;
        // A variant's kind by its id ("dl", "omega"), not its description: "dream"
        // shouldn't find every Dream Land layout.
        QString text = QString::fromStdString(s.Name) + QLatin1Char(' ') + QString::fromStdString(s.Short) +
                       QLatin1Char(' ') + QString::fromStdString(s.Kind);
        // Ω is "omega" too.
        if (text.contains(QChar(0x03A9)))
            text += QStringLiteral(" omega");
        return text.toCaseFolded().contains(needle);
    };
    int shown = 0;
    for (Card* card : std::as_const(m_cards))
    {
        bool any = passes(card->base()) && matches(card->base());
        const bool baseMatches = matches(card->base());
        const QList<QPushButton*>& chips = card->chips();
        for (int i = 0; i < chips.size(); i++)
        {
            const CoreStageListStage* v = m_info.stage(card->chipIds().at(i));
            // A card found by its own name keeps every variant that passes.
            const bool chipShown = v && passes(*v) && (baseMatches || matches(*v));
            chips.at(i)->setVisible(chipShown);
            any = any || chipShown;
        }
        card->setVisible(any);
        shown += any ? 1 : 0;
        card->updateGeometry();
    }
    for (QWidget* section : std::as_const(m_sections))
    {
        bool any = false;
        for (Card* card : std::as_const(m_cards))
            any = any || (card->isVisibleTo(section) && section->isAncestorOf(card));
        section->setVisible(any);
        if (QLayout* l = section->layout())
            l->invalidate();
    }
    m_noMatch->setText(needle.isEmpty() ? tr("No stages of this kind.") : tr("No stages match \"%1\".").arg(m_filter));
    m_noMatch->setVisible(shown == 0);
}

//
// Starting points, codes and saved lists
//

void StageListDialog::fillStartCombo()
{
    if (!m_startCombo)
        return;
    const QSignalBlocker blocker(m_startCombo);
    m_startCombo->clear();
    m_startCombo->addItem(QIcon(LobbyTheme::icon(QStringLiteral("magic-line"), LobbyTheme::tokens().muted, 16, devicePixelRatioF())),
                          tr("Start from a list..."), QString());
    for (const CoreStageListPreset& preset : m_info.Presets)
    {
        m_startCombo->addItem(tr("%1 (%2)").arg(QString::fromStdString(preset.Label)).arg(preset.Stages.size()),
                              QStringLiteral("preset:") + QString::fromStdString(preset.Id));
        m_startCombo->setItemData(m_startCombo->count() - 1, QString::fromStdString(preset.Description), Qt::ToolTipRole);
    }
    if (!m_saved.isEmpty())
    {
        m_startCombo->insertSeparator(m_startCombo->count());
        for (const SavedStageList& list : std::as_const(m_saved))
        {
            const int n = int(stageListEntries(m_info, list.Code).size());
            m_startCombo->addItem(tr("%1 (%2)").arg(list.Name).arg(n), QStringLiteral("saved:") + list.Id);
            m_startCombo->setItemData(m_startCombo->count() - 1, tr("Your saved list"), Qt::ToolTipRole);
        }
    }
    m_startCombo->setCurrentIndex(0);
}

void StageListDialog::startFrom(int index)
{
    const QString data = m_startCombo->itemData(index).toString();
    m_startCombo->setCurrentIndex(0);
    if (data.startsWith(QLatin1String("preset:")))
    {
        if (const CoreStageListPreset* preset = m_info.preset(data.mid(7).toStdString()))
            replaceList(preset->Stages, tr("Started from \"%1\".").arg(QString::fromStdString(preset->Label)));
    }
    else if (data.startsWith(QLatin1String("saved:")))
    {
        for (const SavedStageList& list : std::as_const(m_saved))
        {
            if (list.Id == data.mid(6))
                replaceList(stageListEntries(m_info, list.Code), tr("Loaded \"%1\".").arg(list.Name));
        }
    }
}

QString StageListDialog::code() const
{
    return QString::fromStdString(CoreEncodeStageList(m_info, m_entries));
}

void StageListDialog::showCode(const QString& code)
{
    const std::vector<CoreStageListEntry> entries = stageListEntries(m_info, code);
    if (entries == m_entries)
        return;
    m_entries = entries;
    m_noteBar->setVisible(false);
    changed();
}

void StageListDialog::copyCode()
{
    const QString text = code();
    if (text.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(text);
    showNote(tr("Copied the list's code: %1").arg(text), false);
}

void StageListDialog::openPasteBar()
{
    closeSaveBar();
    m_pasteBar->setVisible(true);
    m_pasteError->setVisible(false);
    // What's on the clipboard, when it's a code.
    const QString clip = QGuiApplication::clipboard()->text().trimmed();
    std::vector<CoreStageListEntry> entries;
    if (clip.size() < 400 && CoreDecodeStageList(m_info, clip.toStdString(), entries) == CoreStageListProblem::None)
        m_pasteEdit->setText(clip);
    m_pasteEdit->setFocus();
    m_pasteEdit->selectAll();
}

void StageListDialog::closePasteBar()
{
    m_pasteBar->setVisible(false);
    m_pasteEdit->clear();
}

QString StageListDialog::problemText(CoreStageListProblem problem) const
{
    switch (problem)
    {
    case CoreStageListProblem::None:
        return QString();
    case CoreStageListProblem::Empty:
    case CoreStageListProblem::BadLength:
        return tr("That code is incomplete or has extra characters. Copy the whole code, from %1- to its end.")
            .arg(QString::fromStdString(m_info.CodePrefix));
    case CoreStageListProblem::NotACode:
        return tr("That isn't a stage list code: codes are letters and digits in groups, like %1-220C-6YA8-2T5V-W0.")
            .arg(QString::fromStdString(m_info.CodePrefix));
    case CoreStageListProblem::BadChecksum:
        return tr("That code doesn't check out: a character may be mistyped, or it's a code for another game or version.");
    case CoreStageListProblem::BadVersion:
        return tr("That code was made by a newer version of RMG-K.");
    case CoreStageListProblem::TooMany:
        return tr("That code has more than %1 stages.").arg(m_info.MaxStages);
    case CoreStageListProblem::UnknownStage:
        return tr("That code names a stage this game doesn't have.");
    case CoreStageListProblem::Repeated:
        return tr("That code has a stage on it twice.");
    }
    return QString();
}

void StageListDialog::usePastedCode()
{
    const QString text = m_pasteEdit->text().trimmed();
    std::vector<CoreStageListEntry> entries;
    const CoreStageListProblem problem = text.isEmpty() ? CoreStageListProblem::Empty
                                                        : CoreDecodeStageList(m_info, text.toStdString(), entries);
    if (problem != CoreStageListProblem::None)
    {
        m_pasteError->setText(text.isEmpty() ? tr("Paste a code first.") : problemText(problem));
        m_pasteError->setVisible(true);
        return;
    }
    closePasteBar();
    replaceList(entries, tr("Used the pasted code: %1.").arg(stageListCountText(m_info, int(entries.size()))));
}

QList<SavedStageList> StageListDialog::savedLists(const QString& romMd5, const CoreStageListInfo& info)
{
    QList<SavedStageList> lists;
    if (romMd5.isEmpty())
        return lists;
    QSettings s("RMG-K", "n02");
    s.beginGroup(CreateRoomDialog::romSettingsGroup(romMd5));
    const QJsonArray array = QJsonDocument::fromJson(s.value("SavedStageLists").toString().toUtf8()).array();
    s.endGroup();
    static const QRegularExpression idPattern(QStringLiteral("^[0-9a-f]{8}$"));
    for (const QJsonValue& value : array)
    {
        const QJsonObject o = value.toObject();
        QString error;
        // A room carries the name to the lobby, so it follows the lobby's rules.
        SavedStageList list{ o.value("id").toString(), GameSettingsCatalog::cleanSavedName(o.value("name").toString(), &error),
                             o.value("code").toString() };
        const bool taken = std::any_of(lists.begin(), lists.end(), [&list](const SavedStageList& other) { return other.Id == list.Id; });
        // Hand-edited or unreadable entries are left out.
        if (!idPattern.match(list.Id).hasMatch() || list.Name.isEmpty() || taken || stageListEntries(info, list.Code).empty())
            continue;
        lists << list;
    }
    std::sort(lists.begin(), lists.end(), [](const SavedStageList& a, const SavedStageList& b) {
        const int byName = QString::localeAwareCompare(a.Name.toCaseFolded(), b.Name.toCaseFolded());
        return byName != 0 ? byName < 0 : a.Id < b.Id;
    });
    return lists;
}

void StageListDialog::storeSavedLists(const QString& romMd5, const QList<SavedStageList>& lists)
{
    QJsonArray array;
    for (const SavedStageList& list : lists)
        array.append(QJsonObject{ { "id", list.Id }, { "name", list.Name }, { "code", list.Code } });
    QSettings s("RMG-K", "n02");
    s.beginGroup(CreateRoomDialog::romSettingsGroup(romMd5));
    if (array.isEmpty())
        s.remove("SavedStageLists");
    else
        s.setValue("SavedStageLists", QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
    s.endGroup();
}

const SavedStageList* StageListDialog::savedMatching() const
{
    if (m_entries.empty())
        return nullptr;
    for (const SavedStageList& list : m_saved)
    {
        if (stageListEntries(m_info, list.Code) == m_entries)
            return &list;
    }
    return nullptr;
}

const CoreStageListPreset* StageListDialog::presetMatching() const
{
    if (m_entries.empty())
        return nullptr;
    for (const CoreStageListPreset& preset : m_info.Presets)
    {
        if (preset.Stages == m_entries)
            return &preset;
    }
    return nullptr;
}

void StageListDialog::setRoomListName(const QString& name, bool savedByHost)
{
    if (name == m_roomListName && savedByHost == m_roomSavedByHost)
        return;
    m_roomListName = name;
    m_roomSavedByHost = savedByHost;
    // A name being typed for the old one isn't one for the new.
    if (!m_editable)
        closeSaveBar();
    refreshSavedActions();
}

QString StageListDialog::saveBlocker() const
{
    if (m_entries.empty())
        return tr("Put some stages on the list first.");
    // Like Game settings: a list that's in the list of lists already isn't
    // saved twice.
    if (const CoreStageListPreset* preset = presetMatching())
        return tr("This is the \"%1\" list already.").arg(QString::fromStdString(preset->Label));
    if (const SavedStageList* saved = savedMatching())
        return m_editable ? tr("This list is saved as \"%1\".").arg(saved->Name)
                          : tr("You already have this list saved as \"%1\".").arg(saved->Name);
    return QString();
}

void StageListDialog::refreshSavedActions()
{
    fillStartCombo();
    // What the list is: one of the preset lists, a saved one, or the host's.
    const CoreStageListPreset* preset = presetMatching();
    const SavedStageList* saved = preset ? nullptr : savedMatching();
    QString text;
    if (preset)
        text = tr("Same as \"%1\"").arg(QString::fromStdString(preset->Label));
    else if (saved)
        text = tr("Saved as \"%1\"").arg(saved->Name);
    else if (!m_editable && !m_roomListName.isEmpty())
        text = m_roomSavedByHost ? tr("The host's own list, saved as \"%1\"").arg(m_roomListName)
                                 : tr("Same as \"%1\"").arg(m_roomListName);
    m_savedAs->setText(text);
    m_savedAs->setVisible(!text.isEmpty());
    if (m_renameButton)
    {
        m_renameButton->setVisible(saved != nullptr);
        m_deleteButton->setVisible(saved != nullptr);
    }
    const QString why = saveBlocker();
    m_saveAsButton->setEnabled(why.isEmpty());
    m_saveAsButton->setToolTip(!why.isEmpty() ? why
                               : m_editable ? tr("Save this list under a name of your own, to pick it from the stage lists next time.")
                                            : tr("Save the host's list under a name of your own, to pick it when you host."));
}

void StageListDialog::openSaveBar()
{
    if (m_entries.empty() || (m_renamingId.isEmpty() && !saveBlocker().isEmpty()))
        return;
    closePasteBar();
    m_saveBar->setVisible(true);
    const SavedStageList* saved = m_renamingId.isEmpty() ? nullptr : savedMatching();
    // Renaming keeps the old name to edit; a guest gets the host's name for
    // a list the host saved, to keep or type over.
    m_saveName->setText(saved ? saved->Name : !m_editable && m_roomSavedByHost ? m_roomListName : QString());
    m_saveConfirm->setText(m_renamingId.isEmpty() ? tr("Save") : tr("Rename"));
    checkSaveName();
    m_saveConfirm->setDefault(true);
    m_saveName->setFocus();
    m_saveName->selectAll();
}

void StageListDialog::closeSaveBar()
{
    if (!m_saveBar || m_saveBar->isHidden())
        return;
    m_saveBar->setVisible(false);
    m_saveName->clear();
    m_renamingId.clear();
    m_mainButton->setDefault(true);
}

void StageListDialog::checkSaveName()
{
    QString error;
    const QString name = GameSettingsCatalog::cleanSavedName(m_saveName->text(), &error);
    // Not one of the stage lists' own entries, which it would pass for.
    QStringList taken = { GameSettingsCatalog::tr("Off", "a stage list option that isn't on"), QStringLiteral("Off"),
                          GameSettingsCatalog::tr("Custom"), QStringLiteral("Custom") };
    for (const CoreStageListPreset& preset : m_info.Presets)
        taken << QString::fromStdString(preset.Label) << QString::fromStdString(preset.EnglishLabel);
    QString reserved;
    for (const QString& other : std::as_const(taken))
    {
        if (!name.isEmpty() && name.compare(other, Qt::CaseInsensitive) == 0)
            reserved = other;
    }
    const SavedStageList* existing = nullptr;
    for (const SavedStageList& list : std::as_const(m_saved))
    {
        if (!name.isEmpty() && list.Name.compare(name, Qt::CaseInsensitive) == 0 && list.Id != m_renamingId)
            existing = &list;
    }
    QString message;
    bool bad = false;
    bool ok = false;
    const QString blocker = m_renamingId.isEmpty() ? saveBlocker() : QString();
    if (!blocker.isEmpty())
        message = blocker;
    else if (m_saveName->text().trimmed().isEmpty())
        ; // nothing typed yet
    else if (name.isEmpty())
    {
        message = error;
        bad = true;
    }
    else if (!reserved.isEmpty())
    {
        message = tr("\"%1\" is already one of the stage lists' entries.").arg(reserved);
        bad = true;
    }
    else if (existing && !m_renamingId.isEmpty())
    {
        message = tr("You already have a list saved as \"%1\".").arg(existing->Name);
        bad = true;
    }
    else if (existing)
    {
        message = tr("You already have a list saved as \"%1\". Replace it with this one?").arg(existing->Name);
        ok = true;
    }
    else if (m_renamingId.isEmpty() && m_saved.size() >= MaxSavedLists)
    {
        message = tr("You can keep up to %1 stage lists for a game. Delete one to save another.").arg(MaxSavedLists);
        bad = true;
    }
    else
        ok = true;
    const bool replace = ok && existing;
    m_saveConfirm->setText(!m_renamingId.isEmpty() ? tr("Rename") : replace ? tr("Replace") : tr("Save"));
    m_saveConfirm->setKind(replace ? LobbyButton::Kind::Danger : LobbyButton::Kind::Primary);
    m_saveConfirm->setEnabled(ok);
    setRole(m_saveMessage, bad ? "bad" : "ink2");
    m_saveMessage->setText(message);
    m_saveMessage->setVisible(!message.isEmpty());
}

void StageListDialog::saveAs()
{
    checkSaveName();
    if (!m_saveConfirm->isEnabled())
        return;
    QString error;
    const QString name = GameSettingsCatalog::cleanSavedName(m_saveName->text(), &error);
    QString note;
    if (!m_renamingId.isEmpty())
    {
        for (SavedStageList& list : m_saved)
        {
            if (list.Id == m_renamingId)
            {
                note = tr("Renamed \"%1\" to \"%2\".").arg(list.Name, name);
                list.Name = name;
            }
        }
    }
    else
    {
        int existing = -1;
        for (int i = 0; i < m_saved.size(); i++)
        {
            if (m_saved.at(i).Name.compare(name, Qt::CaseInsensitive) == 0)
                existing = i;
        }
        if (existing >= 0)
        {
            // Same id, so a room using it follows.
            m_saved[existing].Name = name;
            m_saved[existing].Code = code();
            note = tr("Replaced \"%1\" with this list.").arg(name);
        }
        else
        {
            // A new id of this client's own, also for a copy of the host's list.
            QString id;
            do
                id = QStringLiteral("%1").arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0'));
            while (std::any_of(m_saved.begin(), m_saved.end(), [&id](const SavedStageList& l) { return l.Id == id; }));
            m_saved.append({ id, name, code() });
            note = m_editable ? tr("Saved as \"%1\". You can pick it from the Stage list now.").arg(name)
                              : tr("Saved as \"%1\". It's in your stage lists when you host.").arg(name);
        }
    }
    storeSavedLists(m_romMd5, m_saved);
    m_saved = savedLists(m_romMd5, m_info);
    closeSaveBar();
    refreshSavedActions();
    showNote(note, false);
    emit savedListsChanged();
}


void StageListDialog::renameSaved()
{
    const SavedStageList* saved = savedMatching();
    if (!saved)
        return;
    m_renamingId = saved->Id;
    openSaveBar();
}

void StageListDialog::deleteSaved()
{
    const SavedStageList* saved = savedMatching();
    if (!saved)
        return;
    const QString name = saved->Name;
    const QString id = saved->Id;
    m_saved.erase(std::remove_if(m_saved.begin(), m_saved.end(), [&id](const SavedStageList& l) { return l.Id == id; }),
                  m_saved.end());
    storeSavedLists(m_romMd5, m_saved);
    refreshSavedActions();
    // The list itself stays; only its saved copy is gone.
    showNote(tr("Deleted the saved list \"%1\". The list is still here.").arg(name), false);
    emit savedListsChanged();
}

void StageListDialog::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && ((m_saveBar && !m_saveBar->isHidden()) || (m_pasteBar && !m_pasteBar->isHidden())))
    {
        closeSaveBar();
        closePasteBar();
        event->accept();
        return;
    }
    if (m_editable && event->matches(QKeySequence::Find) && m_search)
    {
        m_search->setFocus();
        m_search->selectAll();
        event->accept();
        return;
    }
    // Enter in the search or paste field shouldn't close the editor.
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
        (focusWidget() == m_search || focusWidget() == m_pasteEdit))
    {
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

#endif // NETPLAY
