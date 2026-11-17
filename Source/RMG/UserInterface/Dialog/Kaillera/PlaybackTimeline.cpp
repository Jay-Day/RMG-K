#include "PlaybackTimeline.hpp"

#ifdef NETPLAY

#include <QAccessibleWidget>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>

#include <algorithm>
#include <cmath>

static constexpr int kHandleRadius = 6;
static constexpr int kTrackHeight = 6;
static constexpr int kMarkSnapPixels = 5;
static constexpr int kKeyStepRecords = 300; // 5 s

// a slider to screen readers and UI Automation: its value is the record
// played, and setting it jumps there
class PlaybackTimelineAccessible : public QAccessibleWidget, public QAccessibleValueInterface
{
public:
    explicit PlaybackTimelineAccessible(QWidget* widget) : QAccessibleWidget(widget, QAccessible::Slider)
    {
    }

    void* interface_cast(QAccessible::InterfaceType type) override
    {
        if (type == QAccessible::ValueInterface)
        {
            return static_cast<QAccessibleValueInterface*>(this);
        }
        return QAccessibleWidget::interface_cast(type);
    }

    QString text(QAccessible::Text type) const override
    {
        if (type == QAccessible::Value)
        {
            return PlaybackTimeText(timeline()->position());
        }
        return QAccessibleWidget::text(type);
    }

    QVariant currentValue() const override { return timeline()->position(); }
    void setCurrentValue(const QVariant& value) override { timeline()->requestSeek(value.toInt()); }
    QVariant maximumValue() const override { return timeline()->total(); }
    QVariant minimumValue() const override { return 0; }
    QVariant minimumStepSize() const override { return kKeyStepRecords; }

private:
    PlaybackTimeline* timeline() const { return static_cast<PlaybackTimeline*>(widget()); }
};

static QAccessibleInterface* playbackTimelineAccessibleFactory(const QString& className, QObject* object)
{
    if (className == QLatin1String("PlaybackTimeline") && object != nullptr && object->isWidgetType())
    {
        return new PlaybackTimelineAccessible(static_cast<QWidget*>(object));
    }
    return nullptr;
}

QString PlaybackTimeText(int records)
{
    const int seconds = std::max(0, records) / 60;
    if (seconds >= 3600)
    {
        return QString("%1:%2:%3").arg(seconds / 3600).arg(seconds / 60 % 60, 2, 10, QChar('0'))
            .arg(seconds % 60, 2, 10, QChar('0'));
    }
    return QString("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0'));
}

PlaybackTimeline::PlaybackTimeline(QWidget* parent) : QWidget(parent)
{
    static const bool accessibleInstalled = []() {
        QAccessible::installFactory(playbackTimelineAccessibleFactory);
        return true;
    }();
    (void)accessibleInstalled;
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAccessibleName(tr("Timeline"));
}

void PlaybackTimeline::setTotal(int records)
{
    records = std::max(0, records);
    if (records != m_total)
    {
        m_total = records;
        update();
    }
}

void PlaybackTimeline::setPosition(int record)
{
    if (record != m_position)
    {
        m_position = record;
        update();
    }
}

void PlaybackTimeline::setSeekTarget(int record)
{
    if (record != m_seekTarget)
    {
        m_seekTarget = record;
        update();
    }
}

void PlaybackTimeline::requestSeek(int record)
{
    if (isEnabled() && m_total > 0 && !m_dragging)
    {
        emit seekRequested(std::clamp(record, 0, m_total));
    }
}

void PlaybackTimeline::setMarks(const QVector<Mark>& marks)
{
    m_marks = marks;
    update();
}

QSize PlaybackTimeline::sizeHint() const
{
    return QSize(400, 26);
}

QSize PlaybackTimeline::minimumSizeHint() const
{
    return QSize(100, 26);
}

QRectF PlaybackTimeline::trackRect() const
{
    const double margin = kHandleRadius + 1;
    // the lower part; bookmark ticks sit above the track
    const double y = height() - margin - kTrackHeight / 2.0 - 1;
    return QRectF(margin, y - kTrackHeight / 2.0, std::max(1.0, width() - margin * 2), kTrackHeight);
}

double PlaybackTimeline::xForRecord(int record) const
{
    const QRectF track = trackRect();
    if (m_total <= 0)
    {
        return track.left();
    }
    return track.left() + track.width() * std::clamp(record, 0, m_total) / m_total;
}

int PlaybackTimeline::recordForX(double x) const
{
    const QRectF track = trackRect();
    if (m_total <= 0)
    {
        return 0;
    }
    const double fraction = std::clamp((x - track.left()) / track.width(), 0.0, 1.0);
    return static_cast<int>(std::lround(fraction * m_total));
}

int PlaybackTimeline::markNear(double x) const
{
    int best = -1;
    double bestDistance = kMarkSnapPixels + 1;
    for (int i = 0; i < m_marks.size(); i++)
    {
        const double distance = std::abs(xForRecord(m_marks[i].record) - x);
        if (distance < bestDistance)
        {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
}

void PlaybackTimeline::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QPalette pal = palette();
    const bool enabled = isEnabled() && m_total > 0;
    const QColor accent = enabled ? pal.color(QPalette::Highlight) : pal.color(QPalette::Mid);
    QColor groove = pal.color(QPalette::Mid);
    groove.setAlpha(110);

    const QRectF track = trackRect();
    painter.setPen(Qt::NoPen);
    painter.setBrush(groove);
    painter.drawRoundedRect(track, kTrackHeight / 2.0, kTrackHeight / 2.0);

    const int shown = m_dragging ? m_dragRecord : m_position;
    const double x = xForRecord(shown);
    QRectF played = track;
    played.setRight(x);
    painter.setBrush(accent);
    painter.drawRoundedRect(played, kTrackHeight / 2.0, kTrackHeight / 2.0);

    // bookmarks: small downward ticks above the track
    QColor tick = pal.color(QPalette::WindowText);
    tick.setAlpha(enabled ? 200 : 90);
    painter.setBrush(tick);
    for (const Mark& mark : m_marks)
    {
        const double mx = xForRecord(mark.record);
        QPainterPath path;
        path.moveTo(mx - 4, track.top() - 9);
        path.lineTo(mx + 4, track.top() - 9);
        path.lineTo(mx, track.top() - 3);
        path.closeSubpath();
        painter.drawPath(path);
    }

    // where a jump is heading
    if (m_seekTarget >= 0 && !m_dragging)
    {
        QColor ghost = accent;
        ghost.setAlpha(120);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(ghost, 2));
        painter.drawEllipse(QPointF(xForRecord(m_seekTarget), track.center().y()), kHandleRadius - 1, kHandleRadius - 1);
    }

    if (enabled)
    {
        painter.setPen(QPen(pal.color(QPalette::Base), 1.5));
        painter.setBrush(accent);
        painter.drawEllipse(QPointF(x, track.center().y()), kHandleRadius, kHandleRadius);
        if (hasFocus())
        {
            painter.setPen(QPen(accent, 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(QPointF(x, track.center().y()), kHandleRadius + 2.5, kHandleRadius + 2.5);
        }
    }

    // hover guide
    if (enabled && m_hoverX >= 0 && !m_dragging)
    {
        QColor guide = pal.color(QPalette::WindowText);
        guide.setAlpha(80);
        painter.setPen(QPen(guide, 1));
        const double hx = std::clamp<double>(m_hoverX, track.left(), track.right());
        painter.drawLine(QPointF(hx, track.top() - 2), QPointF(hx, track.bottom() + 2));
    }
}

void PlaybackTimeline::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !isEnabled() || m_total <= 0)
    {
        QWidget::mousePressEvent(event);
        return;
    }
    m_dragging = true;
    const int mark = markNear(event->position().x());
    m_dragRecord = mark >= 0 ? m_marks[mark].record : recordForX(event->position().x());
    emit dragPreview(m_dragRecord);
    update();
}

void PlaybackTimeline::mouseMoveEvent(QMouseEvent* event)
{
    m_hoverX = static_cast<int>(event->position().x());
    if (m_dragging)
    {
        m_dragRecord = recordForX(event->position().x());
        emit dragPreview(m_dragRecord);
    }
    update();
}

void PlaybackTimeline::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !m_dragging)
    {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_dragging = false;
    update();
    emit seekRequested(m_dragRecord);
}

void PlaybackTimeline::keyPressEvent(QKeyEvent* event)
{
    if (!isEnabled() || m_total <= 0 || m_dragging)
    {
        QWidget::keyPressEvent(event);
        return;
    }
    // from where a jump is heading, so repeated presses add up
    const int from = m_seekTarget >= 0 ? m_seekTarget : m_position;
    switch (event->key())
    {
    case Qt::Key_Left:
        emit seekRequested(std::max(0, from - kKeyStepRecords));
        break;
    case Qt::Key_Right:
        emit seekRequested(std::min(m_total, from + kKeyStepRecords));
        break;
    case Qt::Key_Home:
        emit seekRequested(0);
        break;
    case Qt::Key_End:
        emit seekRequested(m_total);
        break;
    default:
        QWidget::keyPressEvent(event);
        break;
    }
}

void PlaybackTimeline::leaveEvent(QEvent* event)
{
    m_hoverX = -1;
    update();
    QWidget::leaveEvent(event);
}

bool PlaybackTimeline::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTip)
    {
        auto* help = static_cast<QHelpEvent*>(event);
        if (m_total <= 0)
        {
            QToolTip::hideText();
            event->ignore();
            return true;
        }
        const int mark = markNear(help->pos().x());
        const QString text = mark >= 0
            ? QString("%1  %2").arg(m_marks[mark].label, PlaybackTimeText(m_marks[mark].record))
            : PlaybackTimeText(recordForX(help->pos().x()));
        QToolTip::showText(help->globalPos(), text, this);
        return true;
    }
    return QWidget::event(event);
}

#endif // NETPLAY
