#ifndef PLAYBACKTIMELINE_HPP
#define PLAYBACKTIMELINE_HPP

#ifdef NETPLAY

#include <QString>
#include <QVector>
#include <QWidget>

// m:ss (h:mm:ss past an hour) for a count of .krec records, one per 60 Hz frame
QString PlaybackTimeText(int records);

// A recording's timeline in .krec input records: how far playback is, the
// record a jump is heading to, and the bookmarks. Clicking or dragging asks
// for a jump; a press near a bookmark snaps to it. With focus, Left/Right
// jump 5 seconds and Home/End to either end.
class PlaybackTimeline : public QWidget
{
    Q_OBJECT

public:
    struct Mark
    {
        int record = 0;
        QString label;
    };

    explicit PlaybackTimeline(QWidget* parent = nullptr);

    void setTotal(int records);
    void setPosition(int record);
    // -1 = not jumping
    void setSeekTarget(int record);
    void setMarks(const QVector<Mark>& marks);

    bool isDragging() const { return m_dragging; }
    int total() const { return m_total; }
    int position() const { return m_position; }
    // asks for a jump, as a click there would (accessibility tools use it)
    void requestSeek(int record);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    // the user picked a record (mouse released)
    void seekRequested(int record);
    // the record under the mouse while dragging
    void dragPreview(int record);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;
    bool event(QEvent* event) override;

private:
    QRectF trackRect() const;
    double xForRecord(int record) const;
    int recordForX(double x) const;
    // the bookmark within a few pixels of x, or -1
    int markNear(double x) const;

    int m_total = 0;
    int m_position = 0;
    int m_seekTarget = -1;
    int m_dragRecord = 0;
    int m_hoverX = -1;
    bool m_dragging = false;
    QVector<Mark> m_marks;
};

#endif // NETPLAY
#endif // PLAYBACKTIMELINE_HPP
