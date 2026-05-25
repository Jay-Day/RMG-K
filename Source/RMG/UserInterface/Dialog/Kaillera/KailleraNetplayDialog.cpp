/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "KailleraNetplayDialog.hpp"
#include "KailleraServerBrowserDialog.hpp"
#include "KailleraP2PDialog.hpp"
#include "KailleraTraversalConfig.hpp"
#include "KailleraTableStyle.hpp"

#ifdef NETPLAY

#include "../../KailleraUIBridge.hpp"

#include <RMG-Core/Settings.hpp>
#include <RMG-Core/Kaillera.hpp>
#include <RMG-Core/Emulation.hpp>

#include "n02_client.h"
#include "kailleraclient.h"
#include "kcore/kaillera_core.h"
#include "core/p2p_core.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QSizePolicy>
#include <QResizeEvent>
#include <QTabBar>
#include <QEventLoop>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QUrl>
#include <QApplication>
#include <QClipboard>
#include <QHostInfo>
#include <QMenu>
#include <QProcess>
#include <QSettings>
#include <QUdpSocket>
#include <QDesktopServices>
#include <QDir>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFrame>
#include <QEvent>
#include <QFormLayout>
#include <QListWidget>
#include <QListView>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStylePainter>
#include <QItemDelegate>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QPixmap>
#include <QStyledItemDelegate>
#include <QStringList>
#include <QMainWindow>
#include <QPlainTextEdit>

#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <algorithm>

static constexpr int kMaxP2PRecentEntries = 12;

namespace {

bool containsForbiddenNetplayListCharacter(const QString& text)
{
    return text.contains('{') || text.contains('}') || text.contains('|');
}

static constexpr int kMaxTraversalDigits = 3;
static constexpr int kConnectPollIntervalMs = 1;
static constexpr int kP2PWaitingGamesRefreshMs = 8000;
static constexpr int kP2PWaitingCodeRole = static_cast<int>(Qt::UserRole) + 1;
static constexpr int kP2PWaitingHostRole = static_cast<int>(Qt::UserRole) + 2;
static const char* kP2PWaitingGamesUrl = "http://kaillerareborn.2manygames.fr:27887/plist.txt";

enum class P2PWaitingStatus {
    Favorite = 0,
    Saved = 1,
    Available = 2,
    Warning = 3
};

int parseStoredPingValue(const QString& storedPingText, const QString& storedPingValueText)
{
    bool ok = false;
    const int storedPingValue = storedPingValueText.trimmed().toInt(&ok);
    if (ok)
    {
        return storedPingValue;
    }

    const QString pingText = storedPingText.trimmed();
    if (pingText == "...")
    {
        return 999998;
    }
    if (pingText.compare("timeout", Qt::CaseInsensitive) == 0)
    {
        return 999999;
    }
    if (pingText.endsWith("ms", Qt::CaseInsensitive))
    {
        QString msText = pingText;
        msText.chop(2);
        const int pingMs = msText.toInt(&ok);
        if (ok)
        {
            return pingMs;
        }
    }

    return 999999;
}

QString normalizedStoredPingText(const QString& storedPingText, int pingValue)
{
    const QString pingText = storedPingText.trimmed();
    if (!pingText.isEmpty())
    {
        return pingText;
    }
    if (pingValue == 999998)
    {
        return "...";
    }
    if (pingValue >= 0 && pingValue < 999998)
    {
        return QString::number(pingValue) + "ms";
    }

    return "-";
}

int compareNullableInts(int left, int right)
{
    if (left == right)
    {
        return 0;
    }
    if (left < 0)
    {
        return 1;
    }
    if (right < 0)
    {
        return -1;
    }

    return (left < right) ? -1 : 1;
}

int compareServerEntriesForColumn(const ServerEntry& left, const ServerEntry& right, int sortColumn)
{
    switch (sortColumn)
    {
    case 1:
        return QString::localeAwareCompare(left.name, right.name);
    case 2:
        return QString::localeAwareCompare(left.country, right.country);
    case 3:
        return compareNullableInts(left.playerCount, right.playerCount);
    case 4:
        return compareNullableInts(left.pingValue, right.pingValue);
    case 5:
        return QString::localeAwareCompare(left.host, right.host);
    default:
        return 0;
    }
}

class PatternedCodeLineEdit final : public QLineEdit
{
public:
    explicit PatternedCodeLineEdit(QWidget* parent = nullptr)
        : QLineEdit(parent)
    {
        setMaxLength(8);
        setPlaceholderText("ABC@123");

        QObject::connect(this, &QLineEdit::textEdited, this, [this](const QString& text) {
            applyNormalizedText(text);
        });
    }

private:
    static QString normalizeCodeText(const QString& rawText)
    {
        QString letters;
        QString digits;
        bool separatorRequested = false;

        for (const QChar& ch : rawText)
        {
            if (ch.isLetter())
            {
                if (!separatorRequested && digits.isEmpty() && letters.size() < 4)
                {
                    letters += ch.toUpper();
                }
                continue;
            }

            if (ch == '@' || ch == '#' || ch == '-' || ch == '_')
            {
                if (letters.size() >= 3)
                {
                    separatorRequested = true;
                }
                continue;
            }

            if (ch.isDigit())
            {
                if (letters.size() >= 3 && digits.size() < kMaxTraversalDigits)
                {
                    separatorRequested = true;
                    digits += ch;
                }
                continue;
            }
        }

        if (!separatorRequested)
        {
            return letters;
        }

        return letters + "@" + digits;
    }

    void applyNormalizedText(const QString& rawText)
    {
        const QString normalized = normalizeCodeText(rawText);
        if (normalized == text())
        {
            return;
        }

        const QSignalBlocker blocker(this);
        setText(normalized);
        setCursorPosition(normalized.size());
    }
};

} // namespace

static QString getKailleraRecordsDirectory()
{
    return QString::fromStdString(CoreGetKailleraRecordsDirectory());
}

static QIcon themedLineIcon(const QString& iconName)
{
    const bool darkTheme = QApplication::palette().window().color().value() < 128;
    const QString iconPath = QString(":/icons/%1/svg/%2.svg")
        .arg(darkTheme ? "white" : "black", iconName);
    return QIcon(iconPath);
}

static QIcon tintedLineIcon(const QString& iconName, const QColor& color)
{
    const QIcon baseIcon = themedLineIcon(iconName);
    QIcon tintedIcon;
    for (const QSize& size : {QSize(14, 14), QSize(16, 16), QSize(22, 22), QSize(32, 32)})
    {
        QPixmap pixmap = baseIcon.pixmap(size);
        if (pixmap.isNull())
        {
            continue;
        }

        QPainter painter(&pixmap);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), color);
        painter.end();
        tintedIcon.addPixmap(pixmap);
    }

    return tintedIcon.isNull() ? baseIcon : tintedIcon;
}

static QIcon themedFavoriteIcon(const QString& iconName)
{
    if (iconName == "star-fill")
    {
        return tintedLineIcon(iconName, QColor(245, 179, 1));
    }

    const QString theme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_Theme));
    if (theme == "Fusion Dark")
    {
        return QIcon(QString(":/icons/white/svg/%1.svg").arg(iconName));
    }

    return themedLineIcon(iconName);
}

static bool extractP2PAppCode(QString& emulator, QString& code)
{
    static const QString prefix = " {CC:";
    static const QString suffix = "}";

    const int prefixIdx = emulator.indexOf(prefix);
    if (prefixIdx < 0)
    {
        return false;
    }

    const int codeStart = prefixIdx + prefix.length();
    const int suffixIdx = emulator.indexOf(suffix, codeStart);
    if (suffixIdx < 0)
    {
        return false;
    }

    code = emulator.mid(codeStart, suffixIdx - codeStart);
    emulator.truncate(prefixIdx);
    return true;
}

static bool localKailleraGameListContains(const QString& gameName)
{
    if (gameName.startsWith("*") || infos.gameList == nullptr)
    {
        return true;
    }

    const char* p = infos.gameList;
    while (*p)
    {
        if (gameName == QString::fromUtf8(p))
        {
            return true;
        }
        p += strlen(p) + 1;
    }

    return false;
}

static QIcon drawnP2PWaitingStatusIcon(P2PWaitingStatus status)
{
    constexpr int iconExtent = 32;
    QPixmap pixmap(iconExtent, iconExtent);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (status == P2PWaitingStatus::Warning)
    {
        QPainterPath triangle;
        triangle.moveTo(iconExtent / 2.0, 4.0);
        triangle.lineTo(iconExtent - 4.0, iconExtent - 4.0);
        triangle.lineTo(4.0, iconExtent - 4.0);
        triangle.closeSubpath();

        painter.setPen(QPen(QColor(168, 92, 0), 2));
        painter.setBrush(QColor(245, 145, 0));
        painter.drawPath(triangle);

        painter.setPen(QPen(QColor(32, 32, 32), 3, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(iconExtent / 2, 12, iconExtent / 2, 21);
        painter.setBrush(QColor(32, 32, 32));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QPointF(iconExtent / 2.0, 25.0), 1.8, 1.8);
    }
    else if (status == P2PWaitingStatus::Available)
    {
        painter.setPen(QPen(QColor(24, 116, 50), 2));
        painter.setBrush(QColor(38, 166, 76));
        painter.drawEllipse(QRectF(7.0, 7.0, 18.0, 18.0));
    }

    painter.end();
    return QIcon(pixmap);
}

static QIcon p2pWaitingStatusIcon(P2PWaitingStatus status)
{
    switch (status)
    {
    case P2PWaitingStatus::Favorite:
        return themedFavoriteIcon("star-fill");
    case P2PWaitingStatus::Saved:
        return themedFavoriteIcon("star");
    case P2PWaitingStatus::Available:
    case P2PWaitingStatus::Warning:
        return drawnP2PWaitingStatusIcon(status);
    }

    return QIcon();
}

static bool isPrivateHostPort(const QString& hostPort)
{
    if (hostPort.startsWith("10.") || hostPort.startsWith("192.168.") || hostPort.startsWith("127."))
    {
        return true;
    }

    if (hostPort.startsWith("172."))
    {
        QStringList octets = hostPort.split('.');
        if (octets.size() >= 2)
        {
            bool ok = false;
            const int second = octets[1].toInt(&ok);
            if (ok && second >= 16 && second <= 31)
            {
                return true;
            }
        }
    }

    return false;
}

static bool resolveTraversalServerAddress(QHostAddress& address)
{
    const QString host = QString::fromUtf8(kN02TraversalHost);
    if (address.setAddress(host))
    {
        return true;
    }

    const QHostInfo hostInfo = QHostInfo::fromName(host);
    for (const QHostAddress& candidate : hostInfo.addresses())
    {
        if (candidate.protocol() == QAbstractSocket::IPv4Protocol)
        {
            address = candidate;
            return true;
        }
    }

    for (const QHostAddress& candidate : hostInfo.addresses())
    {
        if (candidate.protocol() == QAbstractSocket::IPv6Protocol)
        {
            address = candidate;
            return true;
        }
    }

    return false;
}

static bool sendTraversalRequest(const QByteArray& payload, QList<QByteArray>& parts, QString& error)
{
    QUdpSocket socket;
    if (!socket.bind(QHostAddress::AnyIPv4, 0, QUdpSocket::DefaultForPlatform))
    {
        error = "Failed to open a UDP socket for code configuration.";
        return false;
    }

    QHostAddress serverAddress;
    if (!resolveTraversalServerAddress(serverAddress))
    {
        error = "Failed to resolve the NAT server address.";
        return false;
    }

    if (socket.writeDatagram(payload, serverAddress, kN02TraversalPort) < 0)
    {
        error = "Failed to send the code request to the NAT server.";
        return false;
    }

    if (!socket.waitForReadyRead(2000))
    {
        error = "Timed out waiting for the NAT server.";
        return false;
    }

    QByteArray response;
    QHostAddress sender;
    quint16 senderPort = 0;
    while (socket.hasPendingDatagrams())
    {
        response.resize(static_cast<int>(socket.pendingDatagramSize()));
        socket.readDatagram(response.data(), response.size(), &sender, &senderPort);
    }
    Q_UNUSED(sender);
    Q_UNUSED(senderPort);

    if (!response.isEmpty() && response[0] == '\0')
    {
        response.remove(0, 1);
    }

    parts = response.split('|');
    if (parts.size() < 2 || parts[0] != kN02TraversalProtocol)
    {
        error = "Received an invalid response from the NAT server.";
        return false;
    }

    return true;
}

static bool looksLikeTraversalCode(const QString& s);
static QString normalizeTraversalCode(const QString& s);
static QString normalizeTraversalClaimTarget(const QString& s);

static bool confirmTraversalClaim(const QString& code, const QString& ownerToken, QString& error)
{
    if (code.isEmpty() || ownerToken.isEmpty())
    {
        error = "Missing claim confirmation data.";
        return false;
    }

    QList<QByteArray> parts;
    QByteArray payload = QByteArray(kN02TraversalProtocol) + "|CLAIMACK|" + ownerToken.toUtf8();
    if (sendTraversalRequest(payload, parts, error))
    {
        if (parts[1] == "OK")
        {
            return true;
        }

        if (parts[1] == "ERR" && parts.size() >= 3)
        {
            error = "NAT server error: " + QString::fromUtf8(parts[2]);
        }
        else
        {
            error = "Unexpected response while confirming the claim.";
        }
    }

    parts.clear();
    payload = QByteArray(kN02TraversalProtocol) + "|CHECK|" + code.toUtf8() + "|" + ownerToken.toUtf8();
    if (!sendTraversalRequest(payload, parts, error))
    {
        return false;
    }

    if (parts[1] == "CHECKOK" && parts.size() >= 3)
    {
        return normalizeTraversalCode(QString::fromUtf8(parts[2])) == normalizeTraversalCode(code);
    }

    if (parts[1] == "ERR" && parts.size() >= 3)
    {
        error = "NAT server error: " + QString::fromUtf8(parts[2]);
    }
    else
    {
        error = "Unexpected response while confirming the claim.";
    }
    return false;
}

class SearchableComboBox final : public QComboBox
{
public:
    explicit SearchableComboBox(QWidget* parent = nullptr)
        : QComboBox(parent)
    {
        setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
    }


    void setDisplayTextInset(int inset)
    {
        m_displayTextInset = inset;
        update();
    }

    void showPopup() override
    {
        if (count() == 0)
        {
            return;
        }

        if (m_popup == nullptr)
        {
            m_popup = new QFrame(this, Qt::Popup | Qt::FramelessWindowHint);
            m_popup->setObjectName("KailleraSearchPopup");
            m_popup->installEventFilter(this);
            m_popup->setStyleSheet(
                "QFrame#KailleraSearchPopup {"
                "  border: 1px solid palette(mid);"
                "  border-radius: 8px;"
                "  background-color: palette(base);"
                "}"
                "QLineEdit {"
                "  border: 1px solid palette(mid);"
                "  border-radius: 6px;"
                "  background-color: palette(base);"
                "  padding: 4px 8px;"
                "}"
                "QListWidget {"
                "  border: none;"
                "  background: transparent;"
                "  outline: none;"
                "}"
                "QListWidget::item {"
                "  padding: 2px 8px;"
                "  min-height: 18px;"
                "}"
                "QListWidget::item:selected {"
                "  background-color: palette(highlight);"
                "  color: palette(highlighted-text);"
                "}");
            auto* popupLayout = new QVBoxLayout(m_popup);
            popupLayout->setContentsMargins(8, 8, 8, 8);
            popupLayout->setSpacing(6);

            m_searchEdit = new QLineEdit(m_popup);
            m_searchEdit->setPlaceholderText("Search ROMs...");
            popupLayout->addWidget(m_searchEdit);

            m_listWidget = new QListWidget(m_popup);
            m_listWidget->setSelectionMode(QAbstractItemView::SingleSelection);
            m_listWidget->setUniformItemSizes(true);
            popupLayout->addWidget(m_listWidget);

            connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
                refreshPopupItems(text);
            });
            connect(m_searchEdit, &QLineEdit::returnPressed, this, [this]() {
                if (m_listWidget == nullptr || m_listWidget->count() == 0)
                {
                    return;
                }

                QListWidgetItem* item = m_listWidget->currentItem();
                if (item == nullptr)
                {
                    item = m_listWidget->item(0);
                }
                activatePopupItem(item);
            });
            connect(m_listWidget, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
                activatePopupItem(item);
            });
            connect(m_listWidget, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
                activatePopupItem(item);
            });
        }

        refreshPopupItems(QString());
        m_searchEdit->clear();
        const int popupWidth = qMax(width(), 340);
        const int visibleItems = qMin(10, qMax(1, m_listWidget->count()));
        const int rowHeight = m_listWidget->sizeHintForRow(0) > 0 ? m_listWidget->sizeHintForRow(0) : 22;
        const int popupHeight = 16 + m_searchEdit->sizeHint().height() + 6 + (visibleItems * rowHeight) + 16;

        const QPoint below = mapToGlobal(QPoint(0, height()));
        m_popup->resize(popupWidth, popupHeight);
        m_popup->move(below);
        m_popup->show();
        m_searchEdit->setFocus();
    }

    void hidePopup() override
    {
        if (m_popup != nullptr)
        {
            m_popup->hide();
        }
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == m_popup && event->type() == QEvent::Hide && QApplication::mouseButtons() != Qt::NoButton)
        {
            m_suppressPopupUntilMouseRelease = true;
        }

        return QComboBox::eventFilter(watched, event);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (m_suppressPopupUntilMouseRelease)
        {
            event->accept();
            return;
        }

        QComboBox::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (m_suppressPopupUntilMouseRelease)
        {
            m_suppressPopupUntilMouseRelease = false;
            event->accept();
            return;
        }

        QComboBox::mouseReleaseEvent(event);
    }

    void paintEvent(QPaintEvent* event) override
    {
        if (m_displayTextInset <= 0)
        {
            QComboBox::paintEvent(event);
            return;
        }

        Q_UNUSED(event);

        QStylePainter painter(this);
        QStyleOptionComboBox option;
        initStyleOption(&option);
        painter.drawComplexControl(QStyle::CC_ComboBox, option);

        QStyleOptionComboBox labelOption(option);
        labelOption.rect = style()->subControlRect(
            QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, this);
        labelOption.rect.adjust(m_displayTextInset, 0, 0, 0);
        painter.drawControl(QStyle::CE_ComboBoxLabel, labelOption);
    }

private:
    void refreshPopupItems(const QString& filter)
    {
        if (m_listWidget == nullptr)
        {
            return;
        }

        const QString normalizedFilter = filter.trimmed();
        const QString currentValue = currentText();

        m_listWidget->clear();
        int selectedRow = -1;
        for (int i = 0; i < count(); ++i)
        {
            const QString itemText = itemTextAt(i);
            if (!normalizedFilter.isEmpty()
                && !itemText.contains(normalizedFilter, Qt::CaseInsensitive))
            {
                continue;
            }

            auto* item = new QListWidgetItem(itemText, m_listWidget);
            item->setData(Qt::UserRole, i);
            if (selectedRow < 0 && itemText == currentValue)
            {
                selectedRow = m_listWidget->count() - 1;
            }
        }

        if (m_listWidget->count() > 0)
        {
            m_listWidget->setCurrentRow(selectedRow >= 0 ? selectedRow : 0);
        }
    }

    QString itemTextAt(int index) const
    {
        return QComboBox::itemText(index);
    }

    void activatePopupItem(QListWidgetItem* item)
    {
        if (item == nullptr)
        {
            return;
        }

        const int sourceIndex = item->data(Qt::UserRole).toInt();
        if (sourceIndex >= 0 && sourceIndex < count())
        {
            setCurrentIndex(sourceIndex);
        }
        hidePopup();
    }

    QFrame* m_popup = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QListWidget* m_listWidget = nullptr;
    bool m_suppressPopupUntilMouseRelease = false;
    int m_displayTextInset = 0;
};

class CenteredIconDelegate final : public QItemDelegate
{
public:
    explicit CenteredIconDelegate(QObject* parent = nullptr)
        : QItemDelegate(parent)
    {
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override
    {
        QStyleOptionViewItem viewOption(option);
        viewOption.state &= ~(QStyle::State_HasFocus | QStyle::State_Selected | QStyle::State_MouseOver);

        if (option.state & QStyle::State_Selected)
        {
            painter->fillRect(option.rect, QColor(100, 149, 237, 60));
        }
        else if (option.state & QStyle::State_MouseOver)
        {
            painter->fillRect(option.rect, QColor(100, 149, 237, 30));
        }

        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::UserRole));
        const QString text = index.data(Qt::DisplayRole).toString();
        if (icon.isNull() || !text.isEmpty())
        {
            QItemDelegate::paint(painter, viewOption, index);
            return;
        }

        // Clear decoration/text so base paint just draws background
        viewOption.decorationSize = QSize(0, 0);
        QItemDelegate::paint(painter, viewOption, index);

        // Draw icon centered manually
        const QSize iconSize(16, 16);
        const QRect iconRect(
            viewOption.rect.x() + (viewOption.rect.width() - iconSize.width()) / 2,
            viewOption.rect.y() + (viewOption.rect.height() - iconSize.height()) / 2,
            iconSize.width(),
            iconSize.height());
        icon.paint(painter, iconRect, Qt::AlignCenter,
            option.state & QStyle::State_Enabled ? QIcon::Normal : QIcon::Disabled);
    }
};

enum P2PStoredCardRoles {
    P2PStoredNameRole = Qt::UserRole,
    P2PStoredHostRole = Qt::UserRole + 1,
    P2PStoredFavoriteRole = Qt::UserRole + 2
};

static QColor blendColors(const QColor& from, const QColor& to, qreal amount);

class P2PStoredCardDelegate final : public QStyledItemDelegate
{
public:
    explicit P2PStoredCardDelegate(bool modern, std::function<void(int)> toggleFavorite, QObject* parent = nullptr)
        : QStyledItemDelegate(parent)
        , m_modern(modern)
        , m_toggleFavorite(std::move(toggleFavorite))
    {
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        Q_UNUSED(option);
        Q_UNUSED(index);
        return QSize(112, 60);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        if (painter == nullptr)
        {
            return;
        }

        const QString name = index.data(P2PStoredNameRole).toString().trimmed();
        const QString host = index.data(P2PStoredHostRole).toString().trimmed();
        const bool favorite = index.data(P2PStoredFavoriteRole).toBool();
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;

        const QPalette palette = QApplication::palette();
        const QColor highlightColor = palette.highlight().color().isValid()
            ? palette.highlight().color()
            : QColor(39, 128, 227);
        const QColor neutralFill = m_modern ? QColor(248, 250, 253) : QColor(246, 246, 246);
        const QColor fillColor = selected
            ? blendColors(neutralFill, highlightColor, m_modern ? 0.16 : 0.11)
            : hovered
                ? blendColors(neutralFill, highlightColor, m_modern ? 0.07 : 0.04)
                : neutralFill;
        const QColor borderColor = selected
            ? blendColors(highlightColor, QColor(255, 255, 255), 0.20)
            : (m_modern ? QColor(207, 215, 225) : QColor(186, 186, 186));
        const QColor nameColor = selected
            ? QColor(21, 52, 97)
            : QColor(28, 34, 43);
        const QColor hostColor = selected
            ? QColor(52, 86, 136)
            : QColor(88, 98, 112);

        const QRect cardRect = option.rect.adjusted(2, 2, -2, -2);
        const QRect starRect = favoriteIconRect(cardRect);
        const QRect nameRect(cardRect.left() + 12, cardRect.top() + 8, cardRect.width() - 36, 20);
        const QRect hostRect(cardRect.left() + 12, cardRect.top() + 29, cardRect.width() - 20, 18);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        QPainterPath cardPath;
        cardPath.addRoundedRect(cardRect, m_modern ? 12.0 : 8.0, m_modern ? 12.0 : 8.0);
        painter->fillPath(cardPath, fillColor);
        painter->setPen(QPen(borderColor, selected ? 1.4 : 1.0));
        painter->drawPath(cardPath);

        const QIcon favoriteIcon = themedFavoriteIcon(favorite ? "star-fill" : "star");
        favoriteIcon.paint(painter, starRect, Qt::AlignCenter,
            QIcon::Normal, favorite ? QIcon::On : QIcon::Off);

        QFont nameFont = option.font;
        nameFont.setWeight(QFont::DemiBold);
        painter->setFont(nameFont);
        painter->setPen(nameColor);
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
            QFontMetrics(nameFont).elidedText(name.isEmpty() ? "Saved Peer" : name, Qt::ElideRight, nameRect.width()));

        QFont hostFont = option.font;
        hostFont.setPointSizeF(qMax(7.5, hostFont.pointSizeF() - 0.4));
        painter->setFont(hostFont);
        painter->setPen(hostColor);
        painter->drawText(hostRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
            QFontMetrics(hostFont).elidedText(host, Qt::ElideRight, hostRect.width()));

        painter->restore();
    }

    bool editorEvent(QEvent* event, QAbstractItemModel* model,
        const QStyleOptionViewItem& option, const QModelIndex& index) override
    {
        Q_UNUSED(model);

        if (event != nullptr &&
            (event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick))
        {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            const QRect cardRect = option.rect.adjusted(2, 2, -2, -2);
            if (favoriteIconRect(cardRect).contains(mouseEvent->pos()))
            {
                if (m_toggleFavorite)
                {
                    m_toggleFavorite(index.row());
                }
                return true;
            }
        }

        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }

private:
    QRect favoriteIconRect(const QRect& cardRect) const
    {
        return QRect(cardRect.right() - 22, cardRect.top() + 8, 14, 14);
    }

    bool m_modern = false;
    std::function<void(int)> m_toggleFavorite;
};

class FloatingCornerButtonFilter final : public QObject
{
public:
    FloatingCornerButtonFilter(QWidget* container, QWidget* button, int rightMargin, int bottomMargin)
        : QObject(container)
        , m_container(container)
        , m_button(button)
        , m_rightMargin(rightMargin)
        , m_bottomMargin(bottomMargin)
    {
        reposition();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == m_container && (event->type() == QEvent::Resize || event->type() == QEvent::Show))
        {
            reposition();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void reposition()
    {
        if (m_container == nullptr || m_button == nullptr)
        {
            return;
        }

        const int x = qMax(0, m_container->width() - m_button->width() - m_rightMargin);
        const int y = qMax(0, m_container->height() - m_button->height() - m_bottomMargin);
        m_button->move(x, y);
        m_button->raise();
    }

    QWidget* m_container = nullptr;
    QWidget* m_button = nullptr;
    int m_rightMargin = 0;
    int m_bottomMargin = 0;
};

static void attachFloatingCornerButton(QWidget* container, QWidget* button, int rightMargin, int bottomMargin)
{
    if (container == nullptr || button == nullptr)
    {
        return;
    }

    container->installEventFilter(
        new FloatingCornerButtonFilter(container, button, rightMargin, bottomMargin));
}

static void configureLauncherButtonMetrics(QPushButton* button)
{
    if (button == nullptr)
    {
        return;
    }

    button->setMinimumHeight(32);
    button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
}

static void configureLauncherTallButtonMetrics(QPushButton* button)
{
    if (button == nullptr)
    {
        return;
    }

    button->setMinimumHeight(56);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

static void configureLauncherComboMetrics(QComboBox* combo)
{
    if (combo == nullptr)
    {
        return;
    }

    combo->setMinimumHeight(32);
    combo->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

static void configureLauncherComboPopup(QComboBox* combo, const QString& theme)
{
    if (combo == nullptr || theme != "Modern")
    {
        return;
    }

    auto* popupView = new QListView(combo);
    popupView->setUniformItemSizes(true);
    popupView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    popupView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    const QPalette appPalette = QApplication::palette();
    const QColor windowColor = appPalette.window().color();
    const QColor baseColor = appPalette.base().color();
    const bool darkTheme = windowColor.value() < 128;
    const QColor popupColor = darkTheme
        ? baseColor.darker(106)
        : baseColor.darker(108);
    const QColor borderColor = darkTheme
        ? windowColor.lighter(142)
        : windowColor.darker(132);

    popupView->setStyleSheet(QString(
        "QListView {"
        "  background-color: %1;"
        "  border: 1px solid %2;"
        "  outline: none;"
        "  padding: 2px 0px;"
        "}"
        "QListView::item {"
        "  padding: 2px 8px;"
        "  min-height: 18px;"
        "  margin: 0px;"
        "}"
        "QListView::item:selected {"
        "  background-color: palette(highlight);"
        "  color: palette(highlighted-text);"
        "}").arg(popupColor.name(QColor::HexRgb), borderColor.name(QColor::HexRgb)));

    combo->setView(popupView);
}

static bool isFusionFamilyTheme(const QString& theme)
{
    return theme == "Fusion" || theme == "Fusion Warm" || theme == "Fusion Dark";
}

static void configureLauncherLineEditMetrics(QLineEdit* edit, const QString& theme)
{
    if (edit == nullptr)
    {
        return;
    }

    if (isFusionFamilyTheme(theme))
    {
        edit->setFixedHeight(32);
    }
}

static QWidget* createLauncherSectionPane(
    const QString& theme, QWidget* parent, const QString& title, QVBoxLayout** outLayout)
{
    const bool useTitledGroup = (theme == "Modern");
    QWidget* pane = nullptr;
    QVBoxLayout* layout = nullptr;

    if (useTitledGroup)
    {
        auto* group = new QGroupBox(title, parent);
        group->setObjectName("KailleraPane");
        layout = new QVBoxLayout(group);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(0);
        pane = group;
    }
    else
    {
        auto* widget = new QWidget(parent);
        layout = new QVBoxLayout(widget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(10);
        auto* titleLabel = new QLabel(title, widget);
        titleLabel->setObjectName("KailleraSectionCaption");
        layout->addWidget(titleLabel, 0, Qt::AlignLeft);
        pane = widget;
    }

    if (outLayout != nullptr)
    {
        *outLayout = layout;
    }
    return pane;
}

static void setPaletteRoleForAllGroups(QPalette& palette, QPalette::ColorRole role, const QColor& color)
{
    palette.setColor(QPalette::Active, role, color);
    palette.setColor(QPalette::Inactive, role, color);
    palette.setColor(QPalette::Disabled, role, color);
}

static QColor blendColors(const QColor& from, const QColor& to, qreal amount)
{
    const qreal clamped = std::clamp(amount, 0.0, 1.0);
    return QColor(
        static_cast<int>(from.red() + ((to.red() - from.red()) * clamped)),
        static_cast<int>(from.green() + ((to.green() - from.green()) * clamped)),
        static_cast<int>(from.blue() + ((to.blue() - from.blue()) * clamped)));
}

static QString cssColor(const QColor& color)
{
    return color.name(QColor::HexRgb);
}

static void configureLauncherAccentPalette(QPushButton* button)
{
    if (button == nullptr)
    {
        return;
    }

    const QString theme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_Theme));
    if (!isFusionFamilyTheme(theme))
    {
        return;
    }

    const QPalette appPalette = QApplication::palette();
    const QColor accent = appPalette.color(QPalette::Highlight);
    const QColor accentText = appPalette.color(QPalette::HighlightedText);
    const QColor disabledText = appPalette.color(QPalette::Disabled, QPalette::ButtonText);

    QPalette palette = button->palette();
    setPaletteRoleForAllGroups(palette, QPalette::Button, accent);
    setPaletteRoleForAllGroups(palette, QPalette::Window, accent);
    setPaletteRoleForAllGroups(palette, QPalette::Base, accent);
    setPaletteRoleForAllGroups(palette, QPalette::Light, accent.lighter(130));
    setPaletteRoleForAllGroups(palette, QPalette::Midlight, accent.lighter(115));
    setPaletteRoleForAllGroups(palette, QPalette::Mid, accent.darker(115));
    setPaletteRoleForAllGroups(palette, QPalette::Dark, accent.darker(140));
    setPaletteRoleForAllGroups(palette, QPalette::Shadow, accent.darker(180));
    setPaletteRoleForAllGroups(palette, QPalette::ButtonText, accentText);
    setPaletteRoleForAllGroups(palette, QPalette::WindowText, accentText);
    setPaletteRoleForAllGroups(palette, QPalette::Text, accentText);
    setPaletteRoleForAllGroups(palette, QPalette::BrightText, accentText);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabledText);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabledText);
    palette.setColor(QPalette::Disabled, QPalette::Text, disabledText);
    button->setForegroundRole(QPalette::ButtonText);
    button->setPalette(palette);
}

class LauncherTabBar final : public QTabBar
{
public:
    explicit LauncherTabBar(QWidget* parent = nullptr)
        : QTabBar(parent)
    {
        setElideMode(Qt::ElideNone);
    }

    void setModernMode(bool modern)
    {
        setDrawBase(!modern);
    }

    QSize tabSizeHint(int index) const override
    {
        QSize hint = QTabBar::tabSizeHint(index);
        if (count() <= 0)
        {
            return hint;
        }

        const auto* tabWidget = qobject_cast<const QTabWidget*>(parentWidget());
        int availableWidth = tabWidget != nullptr ? tabWidget->width() : width();
        if (availableWidth <= 0)
        {
            return hint;
        }

        // Reserve a little room at the edges so the outer tab borders don't clip.
        availableWidth -= 24;
        hint.setWidth(qMax(hint.width(), availableWidth / count()));
        return hint;
    }
};

class LauncherTabWidget final : public QTabWidget
{
public:
    explicit LauncherTabWidget(QWidget* parent = nullptr)
        : QTabWidget(parent)
    {
        setTabBar(new LauncherTabBar(this));
        setUsesScrollButtons(false);
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QTabWidget::resizeEvent(event);
        if (tabBar() != nullptr)
        {
            const bool expanding = tabBar()->expanding();
            tabBar()->setExpanding(!expanding);
            tabBar()->setExpanding(expanding);
            tabBar()->updateGeometry();
            tabBar()->update();
        }
    }
};

static QString buildLauncherStyleSheet(const QString& theme)
{
    const bool modern = (theme == "Modern");
    const bool fusionLike = (theme == "Fusion" || theme == "Fusion Warm" || theme == "Fusion Dark");
    if (!modern && !fusionLike)
    {
        return QString();
    }

    const QString paneRadius = modern ? "10px" : "2px";
    const QString controlRadius = modern ? "7px" : "2px";
    const QString fabRadius = modern ? "23px" : "6px";
    const QString dividerColor = modern ? "palette(mid)" : "palette(midlight)";
    const QString comboDropWidth = modern ? "24px" : "32px";
    const QString comboDropRadius = modern ? controlRadius : "0px";
    const QString comboDropBackground = modern ? "transparent" : "palette(button)";
    const QString lineEditVerticalPadding = modern ? "5px" : "1px";
    const QPalette appPalette = QApplication::palette();
    const QColor windowColor = appPalette.window().color();
    const QColor shellColor = modern
        ? appPalette.base().color()
        : blendColors(windowColor, appPalette.base().color(), 0.75);
    const QString shellBackground = cssColor(shellColor);
    const bool darkTheme = windowColor.value() < 128;
    const QString borderColor = darkTheme
        ? cssColor(blendColors(windowColor, QColor(Qt::white), 0.20))
        : "palette(mid)";
    const QString comboArrowIcon = QString(":/icons/%1/svg/arrow-down-s-line.svg")
        .arg(darkTheme ? "white" : "black");

    QString style = QString(
        "QDialog#KailleraLauncherDialog {"
        "  background-color: palette(window);"
        "}"
        "QWidget#KailleraPane, QGroupBox#KailleraPane, QWidget#KailleraPaneGameList {"
        "  border: 1px solid palette(mid);"
        "  border-radius: %1;"
        "  background-color: %10;"
        "}"
        "QLabel#KailleraFieldLabel {"
        "  color: palette(text);"
        "  font-weight: 600;"
        "}"
        "QLabel#KailleraSectionCaption {"
        "  color: palette(mid);"
        "  font-weight: 600;"
        "  font-size: 15px;"
        "}"
        "QLabel#KailleraP2PPanelTitle {"
        "  color: palette(text);"
        "  font-weight: 600;"
        "  font-size: 15px;"
        "}"
        "QPushButton#KailleraP2PReloadButton {"
        "  color: palette(mid);"
        "  border: 1px solid transparent;"
        "  border-radius: %2;"
        "  background-color: transparent;"
        "  padding: 2px;"
        "  font-weight: 600;"
        "}"
        "QPushButton#KailleraP2PReloadButton:hover {"
        "  border-color: palette(mid);"
        "  background-color: palette(alternate-base);"
        "}"
        "QPushButton#KailleraP2PReloadButton:disabled {"
        "  color: palette(mid);"
        "  border-color: transparent;"
        "  background-color: transparent;"
        "}"
        "QLineEdit#KailleraInput {"
        "  border: 1px solid palette(mid);"
        "  border-radius: %2;"
        "  background-color: palette(base);"
        "  padding: %9 8px;"
        "}"
        "QLineEdit#KailleraInput:focus {"
        "  border-color: palette(highlight);"
        "}"
        "QFrame#KailleraSearchPopup {"
        "  border: 1px solid palette(mid);"
        "  border-radius: %1;"
        "  background-color: palette(base);"
        "}"
        "QTextBrowser#KailleraSurface, QTableWidget#KailleraSurface {"
        "  border: none;"
        "  background: transparent;"
        "}"
        "QTableWidget#KailleraSurface {"
        "  gridline-color: transparent;"
        "  alternate-background-color: palette(alternate-base);"
        "}"
        "QFrame#KailleraDivider {"
        "  background-color: %8;"
        "  max-height: 1px;"
        "}").arg(
            paneRadius,
            controlRadius,
            comboDropWidth,
            comboDropRadius,
            comboDropBackground,
            comboArrowIcon,
            fabRadius,
            dividerColor,
            lineEditVerticalPadding,
            shellBackground);

    style += QString(
        "QTableWidget#KailleraSurface[launcherServerTable=\"true\"] {"
        "  border-top: 1px solid palette(mid);"
        "  border-left: 1px solid palette(mid);"
        "}"
        "QTableWidget#KailleraSurface[launcherWaitingGamesTable=\"true\"] {"
        "  border: 1px solid palette(mid);"
        "  background-color: palette(base);"
        "}"
        "QListWidget#KailleraP2PCardList {"
        "  border: none;"
        "  background: transparent;"
        "  outline: none;"
        "  padding: 2px;"
        "}"
        "QListWidget#KailleraP2PCardList::item {"
        "  border: none;"
        "  margin: 0px;"
        "  padding: 0px;"
        "  background: transparent;"
        "}"
        "QListWidget#KailleraP2PCardList::item:selected {"
        "  background: transparent;"
        "}");

    if (modern)
    {
        style += QString(
            "QTabWidget#KailleraLauncherTabs::pane {"
            "  border-top: 1px solid palette(mid);"
            "  border-left: none;"
            "  border-right: none;"
            "  border-bottom: none;"
            "  border-radius: 0px;"
            "  background-color: %3;"
            "  top: -1px;"
            "}"
            "QTabWidget#KailleraLauncherTabs::tab-bar {"
            "  left: 8px;"
            "}"
            "QTabWidget#KailleraLauncherTabs QTabBar::tab {"
            "  border: 1px solid palette(mid);"
            "  border-top-left-radius: 8px;"
            "  border-top-right-radius: 8px;"
            "  padding: 7px 14px;"
            "  margin-right: 0px;"
            "}"
            "QTabWidget#KailleraLauncherTabs QTabBar::tab:selected {"
            "  background-color: palette(base);"
            "  border-bottom: none;"
            "  font-weight: 500;"
            "}"
            "QTabWidget#KailleraLauncherTabs QTabBar::tab:!selected {"
            "  background-color: %4;"
            "}"
            "QWidget#KailleraPaneGameList {"
            "  border-radius: 0px;"
            "}"
            "QHeaderView::section {"
            "  background-color: palette(window);"
            "  border: none;"
            "  border-bottom: 1px solid palette(mid);"
            "  border-left: 1px solid palette(mid);"
            "  padding: 6px 8px;"
            "  font-weight: 600;"
            "}"
            "QHeaderView::section:first {"
            "  border-left: none;"
            "}"
            "QGroupBox#KailleraPane {"
            "  margin-top: 10px;"
            "  padding-top: 6px;"
            "}"
            "QGroupBox#KailleraPane::title {"
            "  subcontrol-origin: margin;"
            "  left: 10px;"
            "  padding: 0 4px;"
            "  font-weight: 600;"
            "}"
            "QComboBox#KailleraInputCombo {"
            "  border: 1px solid palette(mid);"
            "  border-radius: %1;"
            "  background-color: palette(base);"
            "  padding: 5px 8px;"
            "  min-height: 24px;"
            "}"
            "QComboBox#KailleraInputCombo:focus {"
            "  border-color: palette(highlight);"
            "}"
            "QComboBox#KailleraInputCombo::drop-down {"
            "  subcontrol-origin: padding;"
            "  subcontrol-position: top right;"
            "  width: %2;"
            "  border: none;"
            "  border-left: 1px solid palette(mid);"
            "  border-top-right-radius: %1;"
            "  border-bottom-right-radius: %1;"
            "  background-color: transparent;"
            "  margin: 1px;"
            "}"
            "QComboBox#KailleraInputCombo::down-arrow {"
            "  image: url(%5);"
            "  width: 12px;"
            "  height: 12px;"
            "}"
            "QComboBox#KailleraInputCombo QAbstractItemView {"
            "  border: 1px solid palette(mid);"
            "  background-color: palette(base);"
            "  selection-background-color: palette(highlight);"
            "  selection-color: palette(highlighted-text);"
            "  outline: none;"
            "  padding: 0px;"
            "  margin: 0px;"
            "}"
            "QComboBox#KailleraInputCombo QAbstractItemView::item {"
            "  padding: 0px 8px;"
            "  min-height: 18px;"
            "  margin: 0px;"
            "}"
            "QPushButton#KailleraPrimaryButton {"
            "  border: 1px solid #0066b4;"
            "  border-radius: %1;"
            "  padding: 4px 12px;"
            "  font-weight: 700;"
            "  color: white;"
            "  background-color: #0078D7;"
            "}"
            "QPushButton#KailleraPrimaryButton:hover {"
            "  background-color: #1584dd;"
            "}"
            "QPushButton#KailleraPrimaryButton:pressed {"
            "  background-color: #0063b1;"
            "}"
            "QPushButton#KailleraSecondaryButton {"
            "  border: 1px solid palette(mid);"
            "  border-radius: %1;"
            "  padding: 4px 12px;"
            "  background-color: palette(window);"
            "  font-weight: 600;"
            "}"
            "QPushButton#KailleraSecondaryButton:hover {"
            "  border-color: palette(dark);"
            "  background-color: palette(light);"
            "}"
            "QPushButton#KailleraSecondaryButton:pressed {"
            "  border-color: palette(shadow);"
            "  background-color: palette(mid);"
            "  padding-top: 5px;"
            "  padding-bottom: 3px;"
            "}"
            "QPushButton#KailleraP2PIconButton {"
            "  border: 1px solid palette(mid);"
            "  border-radius: %1;"
            "  padding: 0px;"
            "  background-color: palette(button);"
            "}"
            "QPushButton#KailleraP2PIconButton:hover {"
            "  background-color: palette(light);"
            "}"
            "QPushButton#KailleraP2PIconButton:pressed {"
            "  background-color: palette(midlight);"
            "}"
            "QLineEdit#KailleraDisplayInput {"
            "  border: 1px solid palette(mid);"
            "  border-radius: %1;"
            "  background-color: palette(window);"
            "  color: palette(text);"
            "  padding: 5px 8px;"
            "  font-weight: 600;"
            "}"
            "QPushButton#KailleraFabButton {"
            "  border: 1px solid #005a9e;"
            "  border-radius: %2;"
            "  padding: 0px;"
            "  background-color: #0078D7;"
            "  color: white;"
            "}"
            "QPushButton#KailleraFabButton:hover {"
            "  background-color: #1c88dc;"
            "}"
            "QPushButton#KailleraFabButton:pressed {"
            "  border-color: #004f8b;"
            "  background-color: #005a9e;"
            "}").arg(
                controlRadius,
                fabRadius,
                shellBackground,
                cssColor(darkTheme
                    ? blendColors(windowColor, QColor(Qt::white), 0.18)
                    : blendColors(windowColor, QColor(Qt::white), 0.48)),
                comboArrowIcon);
    }

    if (darkTheme)
    {
        style.replace("palette(mid)", borderColor);
    }

    return style;
}

KailleraNetplayDialog::KailleraNetplayDialog(QWidget* parent)
    : QDialog(parent, Qt::Window)
{
    setWindowIcon(QIcon(":Resource/Kaillera.svg"));
    m_netManager = new QNetworkAccessManager(this);
    m_serverPingPollTimer = new QTimer(this);
    m_serverPingPollTimer->setInterval(50);
    connect(m_serverPingPollTimer, &QTimer::timeout, this, &KailleraNetplayDialog::pollServerPing);

    setupUI();
    loadSettings();
    loadServerList();
    loadP2PStoredUsers();

    // Start the KSSDFA state machine timer
    // This replaces the blocking while-loop in n02::selectServerDialog()
    n02::setStateInput(0);
    m_stateMachineTimer = new QTimer(this);
    m_stateMachineTimer->setTimerType(Qt::PreciseTimer);
    connect(m_stateMachineTimer, &QTimer::timeout, this, &KailleraNetplayDialog::onStateMachineTimer);
    m_stateMachineTimer->start(1);

    schedulePingAllServers();
    fetchLiveServerList();

    // Restore saved geometry
    std::string geom = CoreSettingsGetStringValue(SettingsID::Kaillera_NetplayGeometry);
    if (!geom.empty())
    {
        restoreGeometry(QByteArray::fromBase64(QByteArray::fromStdString(geom)));
    }

    QTimer::singleShot(0, this, &KailleraNetplayDialog::maybeAutoClaimP2PStaticCode);
}

KailleraNetplayDialog::~KailleraNetplayDialog()
{
    m_p2pHostLaunchQueued = false;
    cancelPendingP2PAutoClaim();

    if (m_stateMachineTimer)
    {
        m_stateMachineTimer->stop();
    }
    if (m_serverPingPollTimer)
    {
        m_serverPingPollTimer->stop();
    }
    saveSettings();
    saveServerList();
    saveP2PStoredUsers();

    // Save geometry
    CoreSettingsSetValue(SettingsID::Kaillera_NetplayGeometry,
        saveGeometry().toBase64().toStdString());
    CoreSettingsSave();
}

void KailleraNetplayDialog::setupUI()
{
    setObjectName("KailleraLauncherDialog");
    setWindowTitle("RMG-K Netplay");
    setMinimumSize(520, 480);
    resize(580, 530);

    const QString theme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_Theme));
    setStyleSheet(buildLauncherStyleSheet(theme));

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 12, 0, 0);
    mainLayout->setSpacing(12);

    auto* profilePane = new QWidget(this);
    auto* profileWrapper = new QWidget(this);
    auto* profileWrapperLayout = new QVBoxLayout(profileWrapper);
    profileWrapperLayout->setContentsMargins(12, 0, 12, 0);
    profileWrapperLayout->setSpacing(0);
    auto* settingsLayout = new QHBoxLayout(profilePane);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    settingsLayout->setSpacing(10);
    auto* usernameLabel = new QLabel("Username:", profilePane);
    usernameLabel->setObjectName("KailleraFieldLabel");
    settingsLayout->addWidget(usernameLabel);
    m_usernameEdit = new QLineEdit(profilePane);
    m_usernameEdit->setObjectName("KailleraInput");
    m_usernameEdit->setMaxLength(31);
    configureLauncherLineEditMetrics(m_usernameEdit, theme);
    settingsLayout->addWidget(m_usernameEdit);
    profileWrapperLayout->addWidget(profilePane);
    mainLayout->addWidget(profileWrapper);

    // Mode tabs
    m_tabWidget = new LauncherTabWidget(this);
    m_tabWidget->setObjectName("KailleraLauncherTabs");
    if (auto* launcherTabBar = static_cast<LauncherTabBar*>(m_tabWidget->tabBar()))
    {
        launcherTabBar->setModernMode(theme == "Modern");
    }
    m_tabWidget->addTab(createServerTab(), "Server");
    m_tabWidget->addTab(createP2PTab(), "Peer to Peer (rollback)");
    connect(m_tabWidget, &QTabWidget::currentChanged, this, &KailleraNetplayDialog::onTabChanged);
    mainLayout->addWidget(m_tabWidget, 1);
}

QWidget* KailleraNetplayDialog::createServerTab()
{
    auto* tab = new QWidget();
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);
    const QString theme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_Theme));

    // Server list table
    auto* tablePane = new QWidget(tab);
    tablePane->setObjectName("KailleraPaneGameList");
    auto* tablePaneLayout = new QVBoxLayout(tablePane);
    tablePaneLayout->setContentsMargins(0, 0, 0, 0);
    tablePaneLayout->setSpacing(0);

    m_serverTable = new QTableWidget(0, 6, tablePane);
    m_serverTable->setObjectName("KailleraSurface");
    m_serverTable->setProperty("launcherServerTable", true);
    m_serverTable->setHorizontalHeaderLabels({"*", "Name", "Region", "Players", "Ping", "IP"});
    m_serverTable->verticalHeader()->setVisible(false);
    m_serverTable->setShowGrid(false);
    m_serverTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_serverTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_serverTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_serverTable->setSortingEnabled(false);
    m_serverTable->horizontalHeader()->setSortIndicatorShown(true);
    m_serverTable->horizontalHeader()->setSortIndicator(m_serverSortColumn, m_serverSortOrder);
    m_serverTable->horizontalHeader()->setMinimumSectionSize(16);
    // Stretch the IP column so IPs aren't truncated.
    m_serverTable->horizontalHeader()->setStretchLastSection(false);
    m_serverTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_serverTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_serverTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_serverTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
    m_serverTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Interactive);
    m_serverTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    QFont serverHeaderFont = m_serverTable->horizontalHeader()->font();
    serverHeaderFont.setBold(false);
    m_serverTable->horizontalHeader()->setFont(serverHeaderFont);
    m_serverTable->setColumnWidth(0, 28);
    m_serverTable->setColumnWidth(1, 170);
    m_serverTable->setColumnWidth(2, 114);
    m_serverTable->setColumnWidth(3, 70);
    m_serverTable->setColumnWidth(4, 60);
    m_serverTable->setContextMenuPolicy(Qt::CustomContextMenu);
    applyNoAccentStyle(m_serverTable);
    m_serverTable->setItemDelegateForColumn(0, new CenteredIconDelegate(m_serverTable));
    if (theme != "Modern" && !isFusionFamilyTheme(theme))
    {
        m_serverTable->setStyleSheet(
            "QTableWidget { border-top: 1px solid palette(mid); border-left: 1px solid palette(mid); }");
    }
    connect(m_serverTable, &QTableWidget::cellDoubleClicked, this, &KailleraNetplayDialog::onServerDoubleClicked);
    connect(m_serverTable, &QWidget::customContextMenuRequested, this, &KailleraNetplayDialog::onServerRightClicked);
    connect(m_serverTable, &QTableWidget::cellClicked, this, [this](int row, int column) {
        if (column != 0 || row < 0 || row >= m_displayServers.size())
        {
            updateServerButtons();
            return;
        }

        const ServerEntry& entry = m_displayServers[row];
        toggleFavoriteServer(entry.host, entry.name);
    });
    connect(m_serverTable, &QTableWidget::itemSelectionChanged, this, &KailleraNetplayDialog::updateServerButtons);
    connect(m_serverTable->horizontalHeader(), &QHeaderView::sectionClicked, this,
        [this](int section)
    {
        if (section == 0)
        {
            return;
        }

        Qt::SortOrder order = Qt::AscendingOrder;
        if (m_serverSortColumn == section)
        {
            order = (m_serverSortOrder == Qt::AscendingOrder)
                ? Qt::DescendingOrder
                : Qt::AscendingOrder;
        }

        m_serverSortColumn = section;
        m_serverSortOrder = order;
        m_serverTable->horizontalHeader()->setSortIndicator(section, order);

        refreshServerListDisplay(true);
        if (!m_pingAllInProgress)
        {
            cacheVisibleLiveServerOrder();
            saveServerList();
        }
    });
    tablePaneLayout->addWidget(m_serverTable);

    m_btnAdd = new QPushButton(tablePane);
    m_btnAdd->setObjectName("KailleraFabButton");
    m_btnAdd->setToolTip("Add a custom server");
    const bool useLauncherSkin =
        (theme == "Modern" || theme == "Fusion" || theme == "Fusion Warm" || theme == "Fusion Dark");
    m_btnAdd->setText("");
    m_btnAdd->setIcon(QIcon(":/icons/white/svg/add-line.svg"));
    m_btnAdd->setIconSize(QSize(22, 22));
    m_btnAdd->setCursor(Qt::PointingHandCursor);
    m_btnAdd->setFixedSize(46, 46);
    if (!useLauncherSkin)
    {
        m_btnAdd->setStyleSheet(
            "QPushButton {"
            "  border: 1px solid #005a9e;"
            "  border-radius: 6px;"
            "  padding: 0px;"
            "  background-color: #0078D7;"
            "  color: white;"
            "}"
            "QPushButton:hover {"
            "  background-color: #1c88dc;"
            "}"
            "QPushButton:pressed {"
            "  border-color: #004f8b;"
            "  background-color: #005a9e;"
            "}");
    }
    configureLauncherAccentPalette(m_btnAdd);
    connect(m_btnAdd, &QPushButton::clicked, this, &KailleraNetplayDialog::onAddServer);
    attachFloatingCornerButton(tablePane, m_btnAdd, 20, 14);

    layout->addWidget(tablePane, 1);


    // Bottom action row
    auto* btnLayout = new QHBoxLayout();
    btnLayout->setContentsMargins(0, 0, 0, 0);
    btnLayout->setSpacing(10);
    m_btnWaitingGames = new QPushButton("Waiting Games", tab);
    m_btnWaitingGames->setObjectName("KailleraSecondaryButton");
    configureLauncherButtonMetrics(m_btnWaitingGames);
    m_btnConnect = new QPushButton("Connect", tab);
    m_btnConnect->setObjectName("KailleraPrimaryButton");
    configureLauncherButtonMetrics(m_btnConnect);
    configureLauncherAccentPalette(m_btnConnect);
    auto* frameDelayLabel = new QLabel("Frame Delay:", tab);
    frameDelayLabel->setObjectName("KailleraFieldLabel");
    m_frameDelayCombo = new QComboBox(tab);
    m_frameDelayCombo->setObjectName("KailleraInputCombo");
    configureLauncherComboMetrics(m_frameDelayCombo);
    configureLauncherComboPopup(m_frameDelayCombo, theme);
    m_frameDelayCombo->addItem("Auto");
    m_frameDelayCombo->addItem("1 frame (8ms)");
    m_frameDelayCombo->addItem("2 frames (24ms)");
    m_frameDelayCombo->addItem("3 frames (40ms)");
    m_frameDelayCombo->addItem("4 frames (56ms)");
    m_frameDelayCombo->addItem("5 frames (72ms)");
    m_frameDelayCombo->addItem("6 frames (88ms)");
    m_frameDelayCombo->addItem("7 frames (104ms)");
    m_frameDelayCombo->addItem("8 frames (120ms)");
    m_frameDelayCombo->addItem("9 frames (136ms)");

    connect(m_btnWaitingGames, &QPushButton::clicked, this, &KailleraNetplayDialog::onWaitingGames);
    connect(m_btnConnect, &QPushButton::clicked, this, &KailleraNetplayDialog::onConnectServer);

    btnLayout->addWidget(m_btnWaitingGames);
    btnLayout->addStretch();
    btnLayout->addWidget(frameDelayLabel);
    btnLayout->addWidget(m_frameDelayCombo);
    btnLayout->addWidget(m_btnConnect);
    layout->addLayout(btnLayout);

    updateServerButtons();


    return tab;
}

QWidget* KailleraNetplayDialog::createP2PTab()
{
    auto* tab = new QWidget();
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    const QString theme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_Theme));

    // Host area
    QVBoxLayout* hostLayout = nullptr;
    auto* hostPane = createLauncherSectionPane(theme, tab, "Host", &hostLayout);

    auto* hostBody = new QWidget(hostPane);
    auto* hostBodyLayout = new QVBoxLayout(hostBody);
    hostBodyLayout->setContentsMargins(0, 0, 0, 0);
    hostBodyLayout->setSpacing(10);

    // Game picker
    auto* gameLayout = new QHBoxLayout();
    auto* gameLabel = new QLabel("ROM:", hostBody);
    gameLabel->setObjectName("KailleraFieldLabel");
    gameLayout->addWidget(gameLabel);
    m_p2pGameCombo = new SearchableComboBox(hostBody);
    m_p2pGameCombo->setObjectName("KailleraInputCombo");
    configureLauncherComboMetrics(m_p2pGameCombo);
    configureLauncherComboPopup(m_p2pGameCombo, theme);
    if (theme != "Modern")
    {
        static_cast<SearchableComboBox*>(m_p2pGameCombo)->setDisplayTextInset(4);
    }
    m_p2pGameCombo->setToolTip("Choose the ROM to host");
    gameLayout->addWidget(m_p2pGameCombo, 1);
    hostBodyLayout->addLayout(gameLayout);

    QStringList gameNames;
    if (infos.gameList)
    {
        const char* p = infos.gameList;
        while (*p)
        {
            gameNames.append(QString::fromUtf8(p));
            p += strlen(p) + 1;
        }
    }
    std::sort(gameNames.begin(), gameNames.end(), [](const QString& a, const QString& b) {
        return QString::localeAwareCompare(a, b) < 0;
    });
    m_p2pGameCombo->addItems(gameNames);

    if (m_p2pGameCombo->count() > 0)
    {
        std::string lastGame = CoreSettingsGetStringValue(SettingsID::Kaillera_P2PLastGame);
        int idx = -1;
        if (!lastGame.empty())
        {
            idx = m_p2pGameCombo->findText(QString::fromStdString(lastGame));
        }
        m_p2pGameCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    }

    auto* codeLayout = new QHBoxLayout();
    auto* codeLabel = new QLabel("Your code:", hostBody);
    codeLabel->setObjectName("KailleraFieldLabel");
    codeLayout->addWidget(codeLabel);
    m_p2pCurrentCodeEdit = new QLineEdit(hostBody);
    m_p2pCurrentCodeEdit->setObjectName("KailleraDisplayInput");
    m_p2pCurrentCodeEdit->setReadOnly(true);
    m_p2pCurrentCodeEdit->setFocusPolicy(Qt::NoFocus);
    m_p2pCurrentCodeEdit->setCursor(Qt::ArrowCursor);
    m_p2pCurrentCodeEdit->setAlignment(Qt::AlignCenter);
    configureLauncherLineEditMetrics(m_p2pCurrentCodeEdit, theme);
    m_p2pCurrentCodeEdit->setFixedWidth(
        m_p2pCurrentCodeEdit->fontMetrics().horizontalAdvance("WXYZ@12345") + 40);
    m_p2pCopyAction = m_p2pCurrentCodeEdit->addAction(themedLineIcon("copy-line"), QLineEdit::TrailingPosition);
    m_p2pCopyAction->setToolTip("Copy connect code");
    connect(m_p2pCopyAction, &QAction::triggered, this, &KailleraNetplayDialog::onCopyP2PCode);
    codeLayout->addWidget(m_p2pCurrentCodeEdit);
    m_btnP2PConfigureCode = new QPushButton("Customize", hostBody);
    m_btnP2PConfigureCode->setObjectName("KailleraSecondaryButton");
    configureLauncherButtonMetrics(m_btnP2PConfigureCode);
    connect(m_btnP2PConfigureCode, &QPushButton::clicked, this, &KailleraNetplayDialog::onConfigureP2PCode);
    codeLayout->addWidget(m_btnP2PConfigureCode);
    codeLayout->addStretch();
    hostBodyLayout->addLayout(codeLayout);

    m_p2pCopyFeedbackTimer = new QTimer(this);
    m_p2pCopyFeedbackTimer->setSingleShot(true);
    connect(m_p2pCopyFeedbackTimer, &QTimer::timeout, this, [this]() {
        if (m_p2pCopyAction == nullptr)
        {
            return;
        }

        m_p2pCopyAction->setIcon(themedLineIcon("copy-line"));
        m_p2pCopyAction->setToolTip("Copy connect code");
    });

    // Host button
    const int hostStatusIndent = codeLabel->sizeHint().width() + codeLayout->spacing();
    auto* hostBtnLayout = new QHBoxLayout();
    hostBtnLayout->addSpacing(hostStatusIndent);
    m_p2pCodeStatusLabel = new QLabel(hostBody);
    m_p2pCodeStatusLabel->hide();
    m_p2pCodeStatusLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    hostBtnLayout->addWidget(m_p2pCodeStatusLabel, 0, Qt::AlignLeft | Qt::AlignVCenter);
    hostBtnLayout->addStretch();
    hostBtnLayout->addSpacing(10);
    m_btnP2PHost = new QPushButton("Host", hostBody);
    m_btnP2PHost->setObjectName("KailleraPrimaryButton");
    configureLauncherButtonMetrics(m_btnP2PHost);
    configureLauncherAccentPalette(m_btnP2PHost);
    connect(m_btnP2PHost, &QPushButton::clicked, this, &KailleraNetplayDialog::onP2PHost);
    m_btnP2PHostPrivate = new QPushButton("Host (private)", hostBody);
    m_btnP2PHostPrivate->setObjectName("KailleraSecondaryButton");
    configureLauncherButtonMetrics(m_btnP2PHostPrivate);
    connect(m_btnP2PHostPrivate, &QPushButton::clicked, this, &KailleraNetplayDialog::onP2PHostPrivate);
    hostBtnLayout->addWidget(m_btnP2PHostPrivate);
    hostBtnLayout->addWidget(m_btnP2PHost);
    hostBodyLayout->addLayout(hostBtnLayout);
    hostLayout->addWidget(hostBody);

    layout->addWidget(hostPane);

    if (theme != "Modern")
    {
        auto* dividerRow = new QWidget(tab);
        auto* dividerRowLayout = new QVBoxLayout(dividerRow);
        dividerRowLayout->setContentsMargins(0, 6, 0, 6);
        dividerRowLayout->setSpacing(0);

        auto* divider = new QFrame(dividerRow);
        divider->setObjectName("KailleraDivider");
        divider->setFrameShape(QFrame::HLine);
        divider->setFrameShadow(QFrame::Plain);
        dividerRowLayout->addWidget(divider);
        layout->addWidget(dividerRow);
    }

    // Connect area
    QVBoxLayout* connectLayout = nullptr;
    auto* connectPane = createLauncherSectionPane(theme, tab, "Connect", &connectLayout);

    auto* connectBody = new QWidget(connectPane);
    auto* connectBodyLayout = new QVBoxLayout(connectBody);
    connectBodyLayout->setContentsMargins(0, 0, 0, 0);
    connectBodyLayout->setSpacing(10);

    // Top row: IP/Code field + Connect
    auto* addrLayout = new QHBoxLayout();
    auto* addrLabel = new QLabel("IP/Code:", connectBody);
    addrLabel->setObjectName("KailleraFieldLabel");
    addrLayout->addWidget(addrLabel);
    const int p2pLabelWidth = qMax(codeLabel->sizeHint().width(),
        qMax(gameLabel->sizeHint().width(), addrLabel->sizeHint().width()));
    codeLabel->setFixedWidth(p2pLabelWidth);
    gameLabel->setFixedWidth(p2pLabelWidth);
    addrLabel->setFixedWidth(p2pLabelWidth);
    m_p2pHostEdit = new QLineEdit(connectBody);
    m_p2pHostEdit->setObjectName("KailleraInput");
    m_p2pHostEdit->setPlaceholderText("Connect code or ip:port");
    configureLauncherLineEditMetrics(m_p2pHostEdit, theme);
    addrLayout->addWidget(m_p2pHostEdit, 1);
    m_btnP2PJoin = new QPushButton("Connect", connectBody);
    m_btnP2PJoin->setObjectName("KailleraPrimaryButton");
    configureLauncherButtonMetrics(m_btnP2PJoin);
    configureLauncherAccentPalette(m_btnP2PJoin);
    connect(m_btnP2PJoin, &QPushButton::clicked, this, &KailleraNetplayDialog::onP2PJoin);
    addrLayout->addWidget(m_btnP2PJoin);
    connectBodyLayout->addLayout(addrLayout);

    auto* p2pListsLayout = new QHBoxLayout();
    p2pListsLayout->setContentsMargins(0, 0, 0, 0);
    p2pListsLayout->setSpacing(10);

    auto* storedPanel = new QWidget(connectBody);
    auto* storedPanelLayout = new QVBoxLayout(storedPanel);
    storedPanelLayout->setContentsMargins(0, 0, 0, 0);
    storedPanelLayout->setSpacing(4);

    auto* storedTitle = new QLabel("Saved Opponents", storedPanel);
    storedTitle->setObjectName("KailleraP2PPanelTitle");
    storedPanelLayout->addWidget(storedTitle);

    m_p2pStoredList = new QListWidget(storedPanel);
    m_p2pStoredList->setObjectName("KailleraP2PCardList");
    m_p2pStoredList->setViewMode(QListView::IconMode);
    m_p2pStoredList->setFlow(QListView::LeftToRight);
    m_p2pStoredList->setWrapping(true);
    m_p2pStoredList->setResizeMode(QListView::Adjust);
    m_p2pStoredList->setMovement(QListView::Static);
    m_p2pStoredList->setUniformItemSizes(true);
    m_p2pStoredList->setSpacing(1);
    m_p2pStoredList->setGridSize(QSize(116, 64));
    const int storedTileColumns = 2;
    const int storedPanelWidth =
        (m_p2pStoredList->gridSize().width() * storedTileColumns) +
        (m_p2pStoredList->spacing() * (storedTileColumns + 1)) +
        style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, m_p2pStoredList) +
        12;
    storedPanel->setFixedWidth(storedPanelWidth);
    storedPanel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    m_p2pStoredList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_p2pStoredList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_p2pStoredList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_p2pStoredList->setMouseTracking(true);
    m_p2pStoredList->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_p2pStoredList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_p2pStoredList->setItemDelegate(new P2PStoredCardDelegate(
        theme == "Modern",
        [this](int row) { toggleP2PStoredFavorite(row); },
        m_p2pStoredList));
    auto useP2PStoredItem = [this](QListWidgetItem* item, bool connectNow) {
        if (item == nullptr || m_p2pStoredList == nullptr || m_p2pHostEdit == nullptr)
        {
            return;
        }

        const int row = m_p2pStoredList->row(item);
        if (row < 0 || row >= m_p2pStoredUsers.size())
        {
            return;
        }

        m_p2pHostEdit->setText(m_p2pStoredUsers[row].host);
        if (connectNow)
        {
            onP2PJoin();
        }
    };
    connect(m_p2pStoredList, &QListWidget::currentItemChanged, this,
        [useP2PStoredItem](QListWidgetItem* current, QListWidgetItem*) {
            useP2PStoredItem(current, false);
        });
    connect(m_p2pStoredList, &QListWidget::itemClicked, this,
        [useP2PStoredItem](QListWidgetItem* item) {
            useP2PStoredItem(item, false);
        });
    connect(m_p2pStoredList, &QListWidget::itemDoubleClicked, this,
        [useP2PStoredItem](QListWidgetItem* item) {
            useP2PStoredItem(item, true);
        });
    connect(m_p2pStoredList, &QListWidget::customContextMenuRequested,
        this, &KailleraNetplayDialog::onP2PStoredRightClicked);
    storedPanelLayout->addWidget(m_p2pStoredList, 1);

    auto* waitingPanel = new QWidget(connectBody);
    auto* waitingPanelLayout = new QVBoxLayout(waitingPanel);
    waitingPanelLayout->setContentsMargins(0, 0, 0, 0);
    waitingPanelLayout->setSpacing(4);

    auto* waitingHeaderLayout = new QHBoxLayout();
    waitingHeaderLayout->setContentsMargins(0, 0, 0, 0);
    waitingHeaderLayout->setSpacing(8);
    auto* waitingTitle = new QLabel("Waiting Games", waitingPanel);
    waitingTitle->setObjectName("KailleraP2PPanelTitle");
    waitingHeaderLayout->addWidget(waitingTitle);
    waitingHeaderLayout->addStretch();
    m_btnP2PWaitingGamesReload = new QPushButton(waitingPanel);
    m_btnP2PWaitingGamesReload->setObjectName("KailleraP2PReloadButton");
    m_btnP2PWaitingGamesReload->setFlat(true);
    m_btnP2PWaitingGamesReload->setFocusPolicy(Qt::NoFocus);
    m_btnP2PWaitingGamesReload->setCursor(Qt::PointingHandCursor);
    m_btnP2PWaitingGamesReload->setIcon(tintedLineIcon("refresh-line", palette().mid().color()));
    m_btnP2PWaitingGamesReload->setIconSize(QSize(14, 14));
    m_btnP2PWaitingGamesReload->setFixedSize(24, 24);
    m_btnP2PWaitingGamesReload->setToolTip("Reload waiting games");
    connect(m_btnP2PWaitingGamesReload, &QPushButton::clicked, this,
            [this]() { fetchP2PWaitingGames(true); });
    waitingHeaderLayout->addWidget(m_btnP2PWaitingGamesReload);
    waitingPanelLayout->addLayout(waitingHeaderLayout);

    m_p2pWaitingGamesTable = new QTableWidget(0, 3, waitingPanel);
    m_p2pWaitingGamesTable->setObjectName("KailleraSurface");
    m_p2pWaitingGamesTable->setProperty("launcherWaitingGamesTable", true);
    m_p2pWaitingGamesTable->setHorizontalHeaderLabels({"", "Host", "Game"});
    m_p2pWaitingGamesTable->horizontalHeader()->setMinimumSectionSize(40);
    m_p2pWaitingGamesTable->horizontalHeader()->setStretchLastSection(false);
    m_p2pWaitingGamesTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_p2pWaitingGamesTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_p2pWaitingGamesTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_p2pWaitingGamesTable->setColumnWidth(0, 40);
    m_p2pWaitingGamesTable->setColumnWidth(1, 140);
    m_p2pWaitingGamesTable->verticalHeader()->setVisible(false);
    m_p2pWaitingGamesTable->setShowGrid(false);
    m_p2pWaitingGamesTable->setAlternatingRowColors(true);
    m_p2pWaitingGamesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_p2pWaitingGamesTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_p2pWaitingGamesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_p2pWaitingGamesTable->setFocusPolicy(Qt::NoFocus);
    applyNoAccentStyle(m_p2pWaitingGamesTable);
    m_p2pWaitingGamesTable->setItemDelegateForColumn(0, new CenteredIconDelegate(m_p2pWaitingGamesTable));
    connect(m_p2pWaitingGamesTable, &QTableWidget::cellClicked, this,
        [this](int row, int) {
            useP2PWaitingGameRow(row, false);
        });
    connect(m_p2pWaitingGamesTable, &QTableWidget::cellDoubleClicked, this,
        [this](int row, int) {
            useP2PWaitingGameRow(row, true);
        });
    waitingPanelLayout->addWidget(m_p2pWaitingGamesTable, 1);
    p2pListsLayout->addWidget(waitingPanel, 1);
    p2pListsLayout->addWidget(storedPanel);

    connectBodyLayout->addLayout(p2pListsLayout, 1);
    connectLayout->addWidget(connectBody);
    layout->addWidget(connectPane, 1);

    m_p2pWaitingGamesRefreshTimer = new QTimer(this);
    m_p2pWaitingGamesRefreshTimer->setInterval(kP2PWaitingGamesRefreshMs);
    connect(m_p2pWaitingGamesRefreshTimer, &QTimer::timeout, this,
            [this]() { fetchP2PWaitingGames(false); });
    m_p2pWaitingGamesRefreshTimer->start();
    QTimer::singleShot(0, this, [this]() { fetchP2PWaitingGames(false); });

    refreshP2PStaticCodeDisplay();

    return tab;
}

void KailleraNetplayDialog::loadSettings()
{
    // Load username
    std::string username = CoreSettingsGetStringValue(SettingsID::Kaillera_Username);
    if (username.empty())
    {
        // Fallback to OS username
        QByteArray envUser = qgetenv("USER");
        if (envUser.isEmpty())
            envUser = qgetenv("USERNAME");
        if (!envUser.isEmpty())
        {
            username = envUser.toStdString();
        }
        else
        {
            username = "Player";
        }
    }
    m_usernameEdit->setText(QString::fromStdString(username));

    // Load the server-mode frame-delay selection.
    // This is a dropdown index (Auto, 1-9 frames), not a raw spoof-ping value.
    int frameDelay = CoreSettingsGetIntValue(SettingsID::Kaillera_FrameDelay);
    if (frameDelay < 0 || frameDelay > 9) frameDelay = 0;
    m_frameDelayCombo->setCurrentIndex(frameDelay);

    // Load active mode and select the corresponding tab
    int mode = CoreSettingsGetIntValue(SettingsID::Kaillera_ActiveMode);
    if (mode < 0 || mode > 1) mode = 0;
    // Tab order: 0=Server, 1=P2P
    // Mode order: 0=P2P, 1=Server
    int tabIndex = (mode == 0) ? 1 : 0;
    m_tabWidget->setCurrentIndex(tabIndex);
    // Always explicitly activate the mode. setCurrentIndex does not emit
    // currentChanged when the index is unchanged (e.g., default 0 → 0),
    // so onTabChanged never fires and the n02 module stays on whatever
    // CoreInitKaillera loaded (which could be playback mode 2).
    n02::activateMode(mode);
}

void KailleraNetplayDialog::saveSettings()
{
    CoreSettingsSetValue(SettingsID::Kaillera_Username,
                         m_usernameEdit->text().toStdString());
    CoreSettingsSetValue(SettingsID::Kaillera_FrameDelay,
                         m_frameDelayCombo->currentIndex());

    // Tab order: 0=Server, 1=P2P
    // Mode order: 0=P2P, 1=Server
    int mode = (m_tabWidget->currentIndex() == 1) ? 0 : 1;
    CoreSettingsSetValue(SettingsID::Kaillera_ActiveMode, mode);

    if (m_p2pGameCombo && m_p2pGameCombo->currentIndex() >= 0)
    {
        CoreSettingsSetValue(SettingsID::Kaillera_P2PLastGame,
                             m_p2pGameCombo->currentText().toStdString());
    }
}

void KailleraNetplayDialog::loadServerList()
{
    m_favoriteServers.clear();
    m_cachedLiveServers.clear();
    m_displayServers.clear();

    const std::vector<std::string> favoriteNames =
        CoreSettingsGetStringListValue(SettingsID::Kaillera_ServerListNames);
    const std::vector<std::string> favoriteHosts =
        CoreSettingsGetStringListValue(SettingsID::Kaillera_ServerListHosts);
    const std::vector<std::string> favoriteCountries =
        CoreSettingsGetStringListValue(SettingsID::Kaillera_ServerListCountries);
    const std::vector<std::string> favoritePings =
        CoreSettingsGetStringListValue(SettingsID::Kaillera_ServerListPings);
    const std::vector<std::string> favoritePingValues =
        CoreSettingsGetStringListValue(SettingsID::Kaillera_ServerListPingValues);
    for (size_t i = 0; i < favoriteNames.size() && i < favoriteHosts.size(); ++i)
    {
        const QString storedPingText =
            (i < favoritePings.size()) ? QString::fromStdString(favoritePings[i]) : QString();
        const QString storedPingValueText =
            (i < favoritePingValues.size()) ? QString::fromStdString(favoritePingValues[i]) : QString();
        const int storedPingValue = parseStoredPingValue(storedPingText, storedPingValueText);
        m_favoriteServers.append({
            QString::fromStdString(favoriteNames[i]),
            QString::fromStdString(favoriteHosts[i]),
            (i < favoriteCountries.size()) ? QString::fromStdString(favoriteCountries[i]) : QString(),
            "-",
            -1,
            normalizedStoredPingText(storedPingText, storedPingValue),
            storedPingValue
        });
    }

    const std::vector<std::string> cachedNames =
        CoreSettingsGetStringListValue(SettingsID::Kaillera_LiveServerCacheNames);
    const std::vector<std::string> cachedHosts =
        CoreSettingsGetStringListValue(SettingsID::Kaillera_LiveServerCacheHosts);
    const std::vector<std::string> cachedCountries =
        CoreSettingsGetStringListValue(SettingsID::Kaillera_LiveServerCacheCountries);
    const std::vector<std::string> cachedPings =
        CoreSettingsGetStringListValue(SettingsID::Kaillera_LiveServerCachePings);
    const std::vector<std::string> cachedPingValues =
        CoreSettingsGetStringListValue(SettingsID::Kaillera_LiveServerCachePingValues);
    for (size_t i = 0; i < cachedNames.size() && i < cachedHosts.size(); ++i)
    {
        const QString host = QString::fromStdString(cachedHosts[i]);
        if (host.isEmpty())
        {
            continue;
        }

        const QString storedPingText =
            (i < cachedPings.size()) ? QString::fromStdString(cachedPings[i]) : QString();
        const QString storedPingValueText =
            (i < cachedPingValues.size()) ? QString::fromStdString(cachedPingValues[i]) : QString();
        const int storedPingValue = parseStoredPingValue(storedPingText, storedPingValueText);
        m_cachedLiveServers.append({
            QString::fromStdString(cachedNames[i]),
            host,
            (i < cachedCountries.size()) ? QString::fromStdString(cachedCountries[i]) : QString(),
            "-",
            -1,
            normalizedStoredPingText(storedPingText, storedPingValue),
            storedPingValue
        });
    }

    for (auto& favoriteServer : m_favoriteServers)
    {
        const int cachedIndex = cachedServerIndexByHost(favoriteServer.host);
        if (cachedIndex < 0)
        {
            continue;
        }

        if (favoriteServer.country.isEmpty())
        {
            favoriteServer.country = m_cachedLiveServers[cachedIndex].country;
        }
        if (favoriteServer.ping == "-")
        {
            favoriteServer.ping = m_cachedLiveServers[cachedIndex].ping;
            favoriteServer.pingValue = m_cachedLiveServers[cachedIndex].pingValue;
        }
    }

    refreshServerListDisplay();
}

void KailleraNetplayDialog::saveServerList()
{
    std::vector<std::string> favoriteNames;
    std::vector<std::string> favoriteHosts;
    std::vector<std::string> favoriteCountries;
    std::vector<std::string> favoritePings;
    std::vector<std::string> favoritePingValues;
    favoriteNames.reserve(m_favoriteServers.size());
    favoriteHosts.reserve(m_favoriteServers.size());
    favoriteCountries.reserve(m_favoriteServers.size());
    favoritePings.reserve(m_favoriteServers.size());
    favoritePingValues.reserve(m_favoriteServers.size());
    for (const auto& server : m_favoriteServers)
    {
        favoriteNames.push_back(server.name.toStdString());
        favoriteHosts.push_back(server.host.toStdString());
        favoriteCountries.push_back(server.country.toStdString());
        favoritePings.push_back(server.ping.toStdString());
        favoritePingValues.push_back(QString::number(server.pingValue).toStdString());
    }

    std::vector<std::string> cachedNames;
    std::vector<std::string> cachedHosts;
    std::vector<std::string> cachedCountries;
    std::vector<std::string> cachedPings;
    std::vector<std::string> cachedPingValues;
    cachedNames.reserve(m_cachedLiveServers.size());
    cachedHosts.reserve(m_cachedLiveServers.size());
    cachedCountries.reserve(m_cachedLiveServers.size());
    cachedPings.reserve(m_cachedLiveServers.size());
    cachedPingValues.reserve(m_cachedLiveServers.size());
    for (const auto& server : m_cachedLiveServers)
    {
        cachedNames.push_back(server.name.toStdString());
        cachedHosts.push_back(server.host.toStdString());
        cachedCountries.push_back(server.country.toStdString());
        cachedPings.push_back(server.ping.toStdString());
        cachedPingValues.push_back(QString::number(server.pingValue).toStdString());
    }

    CoreSettingsSetValue(SettingsID::Kaillera_ServerListNames, favoriteNames);
    CoreSettingsSetValue(SettingsID::Kaillera_ServerListHosts, favoriteHosts);
    CoreSettingsSetValue(SettingsID::Kaillera_ServerListCountries, favoriteCountries);
    CoreSettingsSetValue(SettingsID::Kaillera_ServerListPings, favoritePings);
    CoreSettingsSetValue(SettingsID::Kaillera_ServerListPingValues, favoritePingValues);
    CoreSettingsSetValue(SettingsID::Kaillera_LiveServerCacheNames, cachedNames);
    CoreSettingsSetValue(SettingsID::Kaillera_LiveServerCacheHosts, cachedHosts);
    CoreSettingsSetValue(SettingsID::Kaillera_LiveServerCacheCountries, cachedCountries);
    CoreSettingsSetValue(SettingsID::Kaillera_LiveServerCachePings, cachedPings);
    CoreSettingsSetValue(SettingsID::Kaillera_LiveServerCachePingValues, cachedPingValues);
    CoreSettingsSave();
}

void KailleraNetplayDialog::refreshServerListDisplay(bool forcePingResort)
{
    QString selectedHost;
    const int currentRow = m_serverTable->currentRow();
    if (currentRow >= 0 && currentRow < m_displayServers.size())
    {
        selectedHost = m_displayServers[currentRow].host;
    }

    const int verticalScroll = m_serverTable->verticalScrollBar() != nullptr
        ? m_serverTable->verticalScrollBar()->value()
        : 0;
    const int horizontalScroll = m_serverTable->horizontalScrollBar() != nullptr
        ? m_serverTable->horizontalScrollBar()->value()
        : 0;

    m_displayServers = m_favoriteServers;

    QVector<ServerEntry> nonFavorites;
    nonFavorites.reserve(m_cachedLiveServers.size());
    for (const auto& server : m_cachedLiveServers)
    {
        if (favoriteServerIndexByHost(server.host) < 0)
        {
            nonFavorites.append(server);
        }
    }

    const bool deferPingResort =
        (m_pingAllInProgress && m_serverSortColumn == 4 && !forcePingResort);
    if (!deferPingResort && m_serverSortColumn != 0)
    {
        std::stable_sort(nonFavorites.begin(), nonFavorites.end(),
            [this](const ServerEntry& a, const ServerEntry& b)
        {
            int compare = compareServerEntriesForColumn(a, b, m_serverSortColumn);
            if (compare == 0)
            {
                compare = QString::localeAwareCompare(a.name, b.name);
            }
            if (compare == 0)
            {
                compare = QString::localeAwareCompare(a.host, b.host);
            }

            return (m_serverSortOrder == Qt::AscendingOrder) ? (compare < 0) : (compare > 0);
        });
    }

    for (const auto& server : nonFavorites)
    {
        m_displayServers.append(server);
    }

    QSignalBlocker blocker(m_serverTable);
    m_serverTable->setUpdatesEnabled(false);
    m_serverTable->setRowCount(m_displayServers.size());
    for (int i = 0; i < m_displayServers.size(); ++i)
    {
        const ServerEntry& server = m_displayServers[i];
        const bool favorite = (favoriteServerIndexByHost(server.host) >= 0);

        auto* favoriteItem = new QTableWidgetItem();
        favoriteItem->setData(Qt::UserRole, themedFavoriteIcon(favorite ? "star-fill" : "star"));
        favoriteItem->setTextAlignment(Qt::AlignCenter);
        favoriteItem->setFlags((favoriteItem->flags() | Qt::ItemIsEnabled | Qt::ItemIsSelectable)
            & ~Qt::ItemIsEditable);
        favoriteItem->setToolTip(favorite ? "Favorited server" : "Favorite server");
        m_serverTable->setItem(i, 0, favoriteItem);

        auto* nameItem = new QTableWidgetItem(server.name);
        m_serverTable->setItem(i, 1, nameItem);

        auto* countryItem = new QTableWidgetItem(server.country);
        m_serverTable->setItem(i, 2, countryItem);

        auto* playersItem = new QTableWidgetItem(server.players);
        playersItem->setTextAlignment(Qt::AlignCenter);
        m_serverTable->setItem(i, 3, playersItem);

        auto* pingItem = new QTableWidgetItem(server.ping);
        pingItem->setTextAlignment(Qt::AlignCenter);
        m_serverTable->setItem(i, 4, pingItem);

        m_serverTable->setItem(i, 5, new QTableWidgetItem(server.host));
    }

    bool restoredSelection = false;
    if (!selectedHost.isEmpty())
    {
        for (int i = 0; i < m_displayServers.size(); ++i)
        {
            if (m_displayServers[i].host == selectedHost)
            {
                m_serverTable->selectRow(i);
                restoredSelection = true;
                break;
            }
        }
    }
    if (!restoredSelection)
    {
        m_serverTable->clearSelection();
    }

    m_serverTable->setUpdatesEnabled(true);
    if (m_serverTable->verticalScrollBar() != nullptr)
    {
        m_serverTable->verticalScrollBar()->setValue(verticalScroll);
    }
    if (m_serverTable->horizontalScrollBar() != nullptr)
    {
        m_serverTable->horizontalScrollBar()->setValue(horizontalScroll);
    }

    updateServerButtons();
}

void KailleraNetplayDialog::fetchLiveServerList()
{
    QNetworkRequest request(QUrl("http://kaillerareborn.2manygames.fr/server_list.php"));
    QNetworkReply* reply = m_netManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError)
        {
            return;
        }

        const QVector<ServerEntry> liveServers = parseLiveServerList(reply->readAll());
        if (liveServers.isEmpty())
        {
            return;
        }

        QVector<ServerEntry> mergedLiveServers;
        mergedLiveServers.reserve(liveServers.size());

        for (const auto& cachedServer : m_cachedLiveServers)
        {
            for (const auto& liveServer : liveServers)
            {
                if (liveServer.host == cachedServer.host)
                {
                    ServerEntry merged = liveServer;
                    merged.ping = cachedServer.ping;
                    merged.pingValue = cachedServer.pingValue;
                    mergedLiveServers.append(merged);
                    break;
                }
            }
        }

        for (const auto& liveServer : liveServers)
        {
            if (cachedServerIndexByHost(liveServer.host) < 0)
            {
                mergedLiveServers.append(liveServer);
            }
        }

        m_cachedLiveServers = mergedLiveServers;
        for (auto& favoriteServer : m_favoriteServers)
        {
            for (const auto& liveServer : liveServers)
            {
                if (liveServer.host == favoriteServer.host)
                {
                    favoriteServer.country = liveServer.country;
                    favoriteServer.players = liveServer.players;
                    favoriteServer.playerCount = liveServer.playerCount;
                    break;
                }
            }
        }
        refreshServerListDisplay();
        saveServerList();
        schedulePingAllServers();
    });
}

void KailleraNetplayDialog::schedulePingAllServers()
{
    if (m_serverPingsSuspended)
    {
        m_pingAllQueued = false;
        return;
    }

    if (m_pingAllInProgress)
    {
        m_pingAllQueued = true;
        return;
    }

    if (m_pingAllQueued)
    {
        return;
    }

    m_pingAllQueued = true;
    QTimer::singleShot(100, this, [this]() {
        if (!m_pingAllQueued || m_pingAllInProgress)
        {
            return;
        }

        m_pingAllQueued = false;
        pingAllServers();
    });
}

void KailleraNetplayDialog::pingAllServers()
{
    if (m_serverPingsSuspended)
    {
        m_pingAllInProgress = false;
        m_pingAllQueued = false;
        m_pendingPingHosts.clear();
        return;
    }

    m_pingAllInProgress = true;
    m_serverListNeedsRefresh = false;
    m_pendingPingHosts.clear();
    m_pendingPingHosts.reserve(m_displayServers.size());
    for (const auto& server : m_displayServers)
    {
        m_pendingPingHosts.append(server.host);
    }

    startNextServerPing();
}

void KailleraNetplayDialog::startNextServerPing()
{
    if (m_serverPingsSuspended)
    {
        m_pingAllInProgress = false;
        m_pingAllQueued = false;
        m_pendingPingHosts.clear();
        return;
    }

    if (m_pendingPingHosts.isEmpty())
    {
        m_pingAllInProgress = false;
        if (m_serverListNeedsRefresh)
        {
            m_serverListNeedsRefresh = false;
            refreshServerListDisplay();
            cacheVisibleLiveServerOrder();
            saveServerList();
        }
        if (m_pingAllQueued)
        {
            m_pingAllQueued = false;
            schedulePingAllServers();
        }
        return;
    }

    m_activePingHost = m_pendingPingHosts.takeFirst();
    QByteArray ipBytes;
    int port = 27888;
    const int colonIdx = m_activePingHost.lastIndexOf(':');
    if (colonIdx >= 0)
    {
        ipBytes = m_activePingHost.left(colonIdx).toUtf8();
        port = m_activePingHost.mid(colonIdx + 1).toInt();
        if (port == 0)
        {
            port = 27888;
        }
    }
    else
    {
        ipBytes = m_activePingHost.toUtf8();
    }

    m_activePingFuture = std::async(std::launch::async, [ipBytes, port]() {
        return kaillera_ping_server(const_cast<char*>(ipBytes.constData()), port, 2000);
    });

    if (m_serverPingPollTimer != nullptr)
    {
        m_serverPingPollTimer->start();
    }
}

void KailleraNetplayDialog::stopServerPingQueue()
{
    m_pingAllQueued = false;
    m_pingAllInProgress = false;
    m_pendingPingHosts.clear();
    m_activePingHost.clear();
    if (m_serverPingPollTimer != nullptr)
    {
        m_serverPingPollTimer->stop();
    }
}

void KailleraNetplayDialog::waitForActiveServerPing(bool applyResult)
{
    if (!m_activePingFuture.valid())
    {
        return;
    }

    while (m_activePingFuture.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready)
    {
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    const int pingResult = m_activePingFuture.get();
    if (applyResult && !m_activePingHost.isEmpty())
    {
        updateServerPing(m_activePingHost, pingResult);
    }
    m_activePingHost.clear();
}

void KailleraNetplayDialog::pollServerPing()
{
    if (!m_activePingFuture.valid())
    {
        if (m_serverPingPollTimer != nullptr)
        {
            m_serverPingPollTimer->stop();
        }
        return;
    }

    if (m_activePingFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
    {
        return;
    }

    if (m_serverPingPollTimer != nullptr)
    {
        m_serverPingPollTimer->stop();
    }

    updateServerPing(m_activePingHost, m_activePingFuture.get());
    m_activePingHost.clear();
    startNextServerPing();
}

QVector<ServerEntry> KailleraNetplayDialog::parseLiveServerList(const QByteArray& data) const
{
    QVector<ServerEntry> parsedServers;
    if (data.size() < 32)
    {
        return parsedServers;
    }

    const char* ptr = data.constData();
    const char* end = ptr + data.size();

    while (ptr < end - 10)
    {
        const char* nameStart = ptr;
        while (ptr < end && *ptr != '\n') ++ptr;
        if (ptr >= end)
        {
            break;
        }
        const QString name = QString::fromUtf8(nameStart, ptr - nameStart).trimmed();
        ++ptr;

        const char* lineStart = ptr;
        while (ptr < end && *ptr != '\n') ++ptr;
        const QString line = QString::fromUtf8(lineStart, ptr - lineStart).trimmed();
        if (ptr < end)
        {
            ++ptr;
        }

        if (name.isEmpty() || line.isEmpty())
        {
            continue;
        }

        const QStringList parts = line.split(';');
        if (parts.isEmpty())
        {
            continue;
        }

        const QString hostPort = parts[0].trimmed();
        if (hostPort.isEmpty() || isPrivateHostPort(hostPort))
        {
            continue;
        }

        bool duplicate = false;
        for (const auto& server : parsedServers)
        {
            if (server.host == hostPort)
            {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
        {
            continue;
        }

        QString players = "-";
        int playerCount = -1;
        if (parts.size() > 1)
        {
            players = parts[1].trimmed();
            if (players.isEmpty())
            {
                players = "-";
            }
            else
            {
                bool ok = false;
                const QString currentPlayers = players.section('/', 0, 0).trimmed();
                const int parsedCount = currentPlayers.toInt(&ok);
                if (ok)
                {
                    playerCount = parsedCount;
                }
            }
        }

        QString country;
        if (parts.size() > 4)
        {
            country = parts[4].trimmed();
        }

        parsedServers.append({name, hostPort, country, players, playerCount, "-", 999999});
    }

    return parsedServers;
}

int KailleraNetplayDialog::favoriteServerIndexByHost(const QString& host) const
{
    for (int i = 0; i < m_favoriteServers.size(); ++i)
    {
        if (m_favoriteServers[i].host == host)
        {
            return i;
        }
    }
    return -1;
}

int KailleraNetplayDialog::cachedServerIndexByHost(const QString& host) const
{
    for (int i = 0; i < m_cachedLiveServers.size(); ++i)
    {
        if (m_cachedLiveServers[i].host == host)
        {
            return i;
        }
    }
    return -1;
}

void KailleraNetplayDialog::toggleFavoriteServer(const QString& host, const QString& name)
{
    const int favoriteIndex = favoriteServerIndexByHost(host);
    if (favoriteIndex >= 0)
    {
        m_favoriteServers.removeAt(favoriteIndex);
    }
    else
    {
        ServerEntry entry{name, host, QString(), "-", -1, "-", 999999};
        const int cachedIndex = cachedServerIndexByHost(host);
        if (cachedIndex >= 0)
        {
            entry = m_cachedLiveServers[cachedIndex];
        }
        m_favoriteServers.append(entry);
    }

    refreshServerListDisplay();
    saveServerList();
}

void KailleraNetplayDialog::moveFavoriteServer(int favoriteIndex, int delta)
{
    const int targetIndex = favoriteIndex + delta;
    if (favoriteIndex < 0 || favoriteIndex >= m_favoriteServers.size()
        || targetIndex < 0 || targetIndex >= m_favoriteServers.size())
    {
        return;
    }

    m_favoriteServers.move(favoriteIndex, targetIndex);
    refreshServerListDisplay();
    if (targetIndex >= 0 && targetIndex < m_displayServers.size())
    {
        m_serverTable->selectRow(targetIndex);
    }
    saveServerList();
}

void KailleraNetplayDialog::updateServerPing(const QString& host, int pingMs)
{
    QString pingText;
    auto applyPing = [pingMs](ServerEntry& server) {
        if (pingMs == -2)
        {
            server.ping = "...";
            server.pingValue = 999998;
        }
        else if (pingMs >= 0)
        {
            server.ping = QString::number(pingMs) + "ms";
            server.pingValue = pingMs;
        }
        else
        {
            server.ping = "timeout";
            server.pingValue = 999999;
        }
    };

    if (pingMs == -2)
    {
        pingText = "...";
    }
    else if (pingMs >= 0)
    {
        pingText = QString::number(pingMs) + "ms";
    }
    else
    {
        pingText = "timeout";
    }

    const int favoriteIndex = favoriteServerIndexByHost(host);
    if (favoriteIndex >= 0)
    {
        applyPing(m_favoriteServers[favoriteIndex]);
    }

    const int cachedIndex = cachedServerIndexByHost(host);
    if (cachedIndex >= 0)
    {
        applyPing(m_cachedLiveServers[cachedIndex]);
    }

    for (auto& server : m_displayServers)
    {
        if (server.host == host)
        {
            applyPing(server);
            break;
        }
    }

    if (m_pingAllInProgress)
    {
        updateVisibleServerPing(host, pingText);
        if (pingMs != -2)
        {
            m_serverListNeedsRefresh = true;
        }
        return;
    }

    refreshServerListDisplay();
}

void KailleraNetplayDialog::updateVisibleServerPing(const QString& host, const QString& pingText)
{
    for (int row = 0; row < m_displayServers.size(); ++row)
    {
        if (m_displayServers[row].host != host)
        {
            continue;
        }

        QTableWidgetItem* pingItem = m_serverTable->item(row, 4);
        if (pingItem != nullptr)
        {
            pingItem->setText(pingText);
        }
        break;
    }
}

void KailleraNetplayDialog::cacheVisibleLiveServerOrder()
{
    QVector<ServerEntry> sortedNonFavorites;
    sortedNonFavorites.reserve(m_displayServers.size());
    for (const auto& server : m_displayServers)
    {
        if (favoriteServerIndexByHost(server.host) < 0)
        {
            sortedNonFavorites.append(server);
        }
    }

    int nextNonFavorite = 0;
    for (int i = 0; i < m_cachedLiveServers.size() && nextNonFavorite < sortedNonFavorites.size(); ++i)
    {
        if (favoriteServerIndexByHost(m_cachedLiveServers[i].host) >= 0)
        {
            continue;
        }

        m_cachedLiveServers[i] = sortedNonFavorites[nextNonFavorite];
        ++nextNonFavorite;
    }
}

void KailleraNetplayDialog::updateServerButtons()
{
    const int row = m_serverTable ? m_serverTable->currentRow() : -1;
    const bool hasSelection = (row >= 0 && row < m_displayServers.size());
    if (m_btnConnect != nullptr)
    {
        m_btnConnect->setEnabled(hasSelection);
    }
}

QString KailleraNetplayDialog::currentP2PStaticCode() const
{
    return normalizeTraversalCode(
        QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Kaillera_P2PStaticCode)));
}

QString KailleraNetplayDialog::currentP2PStaticCodeOwnerToken() const
{
    return QString::fromStdString(
        CoreSettingsGetStringValue(SettingsID::Kaillera_P2PStaticCodeOwnerToken)).trimmed();
}

void KailleraNetplayDialog::connectRollbackSessionLaunch(KailleraP2PDialog& p2pDialog, bool& rollbackSessionActive)
{
    // Track whether rollback is active at dialog close, not whether it was ever launched.
    connect(&p2pDialog, &KailleraP2PDialog::rollbackSessionReady, this,
        [this, &rollbackSessionActive](QString game, QString remoteAddress, int localPort, int remotePort, int localPlayer, int frameDelay, int predictionWindow) {
            rollbackSessionActive = true;
            emit rollbackSessionPreparing();
            emit rollbackSessionRequested(game, remoteAddress, localPort, remotePort, localPlayer, frameDelay, predictionWindow);
        },
        Qt::DirectConnection);
    connect(&p2pDialog, &KailleraP2PDialog::rollbackSessionEnded, this,
        [&rollbackSessionActive]() {
            rollbackSessionActive = false;
        },
        Qt::DirectConnection);
}

static void releaseOldTraversalReservation(QWidget* parent, const QString& ownerToken)
{
    if (ownerToken.isEmpty())
    {
        return;
    }

    QList<QByteArray> parts;
    QString error;
    const QByteArray payload = QByteArray(kN02TraversalProtocol) + "|RELEASE|" + ownerToken.toUtf8();
    if (!sendTraversalRequest(payload, parts, error))
    {
        QMessageBox::warning(parent, "Configure P2P Code",
            "Your new code was saved, but the old code could not be released.\n" + error);
        return;
    }

    if (parts[1] == "OK")
    {
        return;
    }

    if (parts[1] == "ERR" && parts.size() >= 3)
    {
        const QString reason = QString::fromUtf8(parts[2]);
        if (reason == "NOAUTH")
        {
            return;
        }

        QMessageBox::warning(parent, "Configure P2P Code",
            "Your new code was saved, but the old code could not be released.\nNAT server error: " + reason);
        return;
    }

    QMessageBox::warning(parent, "Configure P2P Code",
        "Your new code was saved, but the old code could not be released.");
}

void KailleraNetplayDialog::refreshP2PStaticCodeDisplay()
{
    if (!m_p2pCurrentCodeEdit)
    {
        return;
    }

    const QString code = currentP2PStaticCode();
    const bool hasIdentity = !code.isEmpty() && !currentP2PStaticCodeOwnerToken().isEmpty();
    if (hasIdentity)
    {
        m_p2pCurrentCodeEdit->setText(code);
    }
    else
    {
        m_p2pCurrentCodeEdit->clear();
    }

    if (m_p2pCopyAction)
    {
        m_p2pCopyAction->setEnabled(hasIdentity);
    }
}

void KailleraNetplayDialog::showP2PCodeStatusMessage(const QString& message, const QColor& color)
{
    if (m_p2pCodeStatusLabel == nullptr)
    {
        return;
    }

    if (m_p2pCodeStatusTimer == nullptr)
    {
        m_p2pCodeStatusTimer = new QTimer(this);
        m_p2pCodeStatusTimer->setSingleShot(true);
        connect(m_p2pCodeStatusTimer, &QTimer::timeout, this, [this]() {
            if (m_p2pCodeStatusLabel == nullptr)
            {
                return;
            }

            m_p2pCodeStatusLabel->clear();
            m_p2pCodeStatusLabel->hide();
        });
    }

    if (message.isEmpty())
    {
        m_p2pCodeStatusTimer->stop();
        m_p2pCodeStatusLabel->clear();
        m_p2pCodeStatusLabel->hide();
        return;
    }

    m_p2pCodeStatusLabel->setStyleSheet(QString("color: %1; font-weight: 600;").arg(color.name()));
    m_p2pCodeStatusLabel->setText(message);
    m_p2pCodeStatusLabel->show();
    m_p2pCodeStatusTimer->start(4000);
}

void KailleraNetplayDialog::cancelPendingP2PAutoClaim()
{
    m_p2pAutoClaimAwaitingAck = false;
    m_p2pAutoClaimPendingCode.clear();
    m_p2pAutoClaimPendingToken.clear();

    if (m_p2pAutoClaimTimeoutTimer != nullptr)
    {
        m_p2pAutoClaimTimeoutTimer->stop();
        m_p2pAutoClaimTimeoutTimer->deleteLater();
        m_p2pAutoClaimTimeoutTimer = nullptr;
    }

    if (m_p2pAutoClaimSocket != nullptr)
    {
        m_p2pAutoClaimSocket->close();
        m_p2pAutoClaimSocket->deleteLater();
        m_p2pAutoClaimSocket = nullptr;
    }

    if (m_p2pHostLaunchQueued)
    {
        const bool queuedPublicHost = m_p2pHostLaunchQueuedPublic;
        m_p2pHostLaunchQueued = false;
        if (m_btnP2PHost != nullptr)
        {
            m_btnP2PHost->setEnabled(true);
        }
        if (m_btnP2PHostPrivate != nullptr)
        {
            m_btnP2PHostPrivate->setEnabled(true);
        }
        QTimer::singleShot(0, this, [this, queuedPublicHost]() {
            hostP2P(queuedPublicHost);
        });
    }
}

void KailleraNetplayDialog::maybeAutoClaimP2PStaticCode()
{
    if (m_p2pAutoClaimAttempted)
    {
        return;
    }

    if (!currentP2PStaticCode().isEmpty() && !currentP2PStaticCodeOwnerToken().isEmpty())
    {
        return;
    }

    m_p2pAutoClaimAttempted = true;

    auto* socket = new QUdpSocket(this);
    if (!socket->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::DefaultForPlatform))
    {
        socket->deleteLater();
        return;
    }

    QHostAddress serverAddress;
    if (!resolveTraversalServerAddress(serverAddress))
    {
        socket->deleteLater();
        return;
    }

    auto* timeoutTimer = new QTimer(this);
    timeoutTimer->setSingleShot(true);

    m_p2pAutoClaimSocket = socket;
    m_p2pAutoClaimTimeoutTimer = timeoutTimer;
    m_p2pAutoClaimAwaitingAck = false;
    m_p2pAutoClaimPendingCode.clear();
    m_p2pAutoClaimPendingToken.clear();

    connect(timeoutTimer, &QTimer::timeout, this, [this]() {
        cancelPendingP2PAutoClaim();
    });

    connect(socket, &QUdpSocket::readyRead, this, [this, socket, serverAddress]() {
        QByteArray response;
        QHostAddress sender;
        quint16 senderPort = 0;

        while (socket->hasPendingDatagrams())
        {
            response.resize(static_cast<int>(socket->pendingDatagramSize()));
            socket->readDatagram(response.data(), response.size(), &sender, &senderPort);
        }
        Q_UNUSED(sender);
        Q_UNUSED(senderPort);

        if (!response.isEmpty() && response[0] == '\0')
        {
            response.remove(0, 1);
        }

        const QList<QByteArray> parts = response.split('|');
        if (parts.size() < 2 || parts[0] != kN02TraversalProtocol)
        {
            cancelPendingP2PAutoClaim();
            return;
        }

        if (!m_p2pAutoClaimAwaitingAck)
        {
            if (parts[1] != "CLAIMOK" || parts.size() < 4)
            {
                cancelPendingP2PAutoClaim();
                return;
            }

            const QString code = normalizeTraversalCode(QString::fromUtf8(parts[2]));
            const QString token = QString::fromUtf8(parts[3]).trimmed();
            if (code.isEmpty() || token.isEmpty())
            {
                cancelPendingP2PAutoClaim();
                return;
            }

            if (!currentP2PStaticCode().isEmpty() || !currentP2PStaticCodeOwnerToken().isEmpty())
            {
                cancelPendingP2PAutoClaim();
                return;
            }

            m_p2pAutoClaimPendingCode = code;
            m_p2pAutoClaimPendingToken = token;
            CoreSettingsSetValue(SettingsID::Kaillera_P2PStaticCode, code.toStdString());
            CoreSettingsSetValue(SettingsID::Kaillera_P2PStaticCodeOwnerToken, token.toStdString());
            CoreSettingsSave();
            refreshP2PStaticCodeDisplay();

            const QByteArray ack = QByteArray(kN02TraversalProtocol) + "|CLAIMACK|" + token.toUtf8();
            if (socket->writeDatagram(ack, serverAddress, kN02TraversalPort) < 0)
            {
                cancelPendingP2PAutoClaim();
                return;
            }

            m_p2pAutoClaimAwaitingAck = true;
            if (m_p2pAutoClaimTimeoutTimer != nullptr)
            {
                m_p2pAutoClaimTimeoutTimer->start(2000);
            }
            return;
        }

        if (parts[1] == "OK")
        {
            cancelPendingP2PAutoClaim();
            return;
        }

        cancelPendingP2PAutoClaim();
    });

    const QByteArray payload = QByteArray(kN02TraversalProtocol) + "|CLAIM|AUTO";
    if (socket->writeDatagram(payload, serverAddress, kN02TraversalPort) < 0)
    {
        cancelPendingP2PAutoClaim();
        return;
    }

    timeoutTimer->start(2000);
}

void KailleraNetplayDialog::onCopyP2PCode()
{
    const QString code = currentP2PStaticCode();
    if (code.isEmpty())
    {
        return;
    }

    QApplication::clipboard()->setText(code);
    if (m_p2pCopyAction != nullptr)
    {
        m_p2pCopyAction->setIcon(themedLineIcon("copy-check-line"));
        m_p2pCopyAction->setToolTip("Copied");
    }
    if (m_p2pCopyFeedbackTimer != nullptr)
    {
        m_p2pCopyFeedbackTimer->start(1200);
    }
}

void KailleraNetplayDialog::onStateMachineTimer()
{
    // Drive the KSSDFA state machine one step (non-blocking)
    bool active = n02::processStateMachineStep();
    if (!active)
    {
        // State 3 = shutdown, close dialog
        m_stateMachineTimer->stop();
        accept();
    }
}

void KailleraNetplayDialog::onTabChanged(int index)
{
    // Tab order: 0=Server, 1=P2P
    // n02 mode: 0=P2P, 1=Server
    int mode = (index == 1) ? 0 : 1;
    n02::activateMode(mode);
}

void KailleraNetplayDialog::onConfigureP2PCode()
{
    cancelPendingP2PAutoClaim();
    showP2PCodeStatusMessage(QString(), QColor());

    const QString theme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_Theme));
    QString requested = currentP2PStaticCode();
    const QString oldCode = currentP2PStaticCode();
    const QString oldOwnerToken = currentP2PStaticCodeOwnerToken();

    while (true)
    {
        QDialog dlg(this);
        dlg.setObjectName("KailleraLauncherDialog");
        dlg.setWindowTitle("Configure P2P Code");
        dlg.setWindowIcon(windowIcon());
        dlg.setStyleSheet(buildLauncherStyleSheet(theme));
        auto* layout = new QVBoxLayout(&dlg);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(10);

        auto* infoLabel = new QLabel(
            "Connect codes are 3 or 4 letters, followed by 1-3 numbers.\n"
            "Examples: ABC@1, PIKA@555, NAT@20.",
            &dlg);
        infoLabel->setWordWrap(true);

        auto* codeEdit = new PatternedCodeLineEdit(&dlg);
        codeEdit->setObjectName("KailleraInput");
        configureLauncherLineEditMetrics(codeEdit, theme);
        codeEdit->setFixedWidth(codeEdit->fontMetrics().horizontalAdvance("WXYZ@12345") + 24);
        if (!requested.isEmpty())
        {
            codeEdit->setText(requested);
            codeEdit->selectAll();
        }

        auto* statusLabel = new QLabel(&dlg);
        statusLabel->setWordWrap(true);
        statusLabel->setFixedWidth(320);
        statusLabel->setFixedHeight(statusLabel->fontMetrics().lineSpacing() + 4);
        statusLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        statusLabel->setText(QString());

        auto* btnCheck = new QPushButton("Check Availability", &dlg);
        btnCheck->setObjectName("KailleraSecondaryButton");
        configureLauncherButtonMetrics(btnCheck);
        auto* inputLayout = new QHBoxLayout();
        inputLayout->setContentsMargins(0, 0, 0, 0);
        inputLayout->setSpacing(8);
        inputLayout->addWidget(codeEdit);
        inputLayout->addWidget(btnCheck);
        inputLayout->addStretch();

        auto* btnLayout = new QHBoxLayout();
        btnLayout->setContentsMargins(0, 0, 0, 0);
        btnLayout->setSpacing(8);
        auto* btnAccept = new QPushButton("Accept", &dlg);
        auto* btnCancel = new QPushButton("Cancel", &dlg);
        btnAccept->setObjectName("KailleraPrimaryButton");
        btnCancel->setObjectName("KailleraSecondaryButton");
        configureLauncherButtonMetrics(btnAccept);
        configureLauncherButtonMetrics(btnCancel);
        configureLauncherAccentPalette(btnAccept);
        btnAccept->setEnabled(false);
        btnLayout->addWidget(btnAccept);
        btnLayout->addWidget(btnCancel);
        btnLayout->addStretch();

        layout->addWidget(infoLabel);
        layout->addLayout(inputLayout);
        layout->addWidget(statusLabel);
        layout->addLayout(btnLayout);

        QString approvedTarget;
        const QString ownerToken = currentP2PStaticCodeOwnerToken();

        auto setStatus = [&statusLabel](const QString& text, const QString& color) {
            statusLabel->setStyleSheet("color: " + color + ";");
            statusLabel->setText(text);
        };

        auto resetAvailability = [&]() {
            approvedTarget.clear();
            btnAccept->setEnabled(false);
            statusLabel->setStyleSheet(QString());
            statusLabel->clear();
        };

        auto runCheck = [&]() {
            const QString normalizedRequest = normalizeTraversalClaimTarget(codeEdit->text());
            if (normalizedRequest.isEmpty())
            {
                resetAvailability();
                setStatus("Enter 3 or 4 letters or a full code like CATS@123.", "#b00020");
                return;
            }

            QList<QByteArray> parts;
            QString error;
            QByteArray payload = QByteArray(kN02TraversalProtocol) + "|CHECK|" + normalizedRequest.toUtf8();
            if (!ownerToken.isEmpty())
            {
                payload += "|" + ownerToken.toUtf8();
            }

            if (!sendTraversalRequest(payload, parts, error))
            {
                QMessageBox::warning(this, "Configure P2P Code", error);
                return;
            }

            resetAvailability();

            if (parts[1] == "CHECKOK" && parts.size() >= 3)
            {
                const QString code = normalizeTraversalCode(QString::fromUtf8(parts[2]));
                if (code.isEmpty())
                {
                    QMessageBox::warning(this, "Configure P2P Code",
                        "The NAT server returned an invalid availability response.");
                    return;
                }

                approvedTarget = code;
                {
                    const QSignalBlocker blocker(codeEdit);
                    codeEdit->setText(code);
                }
                btnAccept->setEnabled(true);
                setStatus(code + " is available.", "#0b6e2e");
                return;
            }

            if (parts[1] == "CHECKSUGGEST" && parts.size() >= 4)
            {
                const QString requestedCode = normalizeTraversalCode(QString::fromUtf8(parts[2]));
                const QString suggestedCode = normalizeTraversalCode(QString::fromUtf8(parts[3]));
                if (!suggestedCode.isEmpty())
                {
                    const QSignalBlocker blocker(codeEdit);
                    codeEdit->setText(suggestedCode);
                    codeEdit->selectAll();
                }

                if (!requestedCode.isEmpty() && !suggestedCode.isEmpty())
                {
                    setStatus(requestedCode + " is unavailable. Suggested code: " + suggestedCode + ".", "#b00020");
                }
                else
                {
                    setStatus("That code is unavailable. Try another one.", "#b00020");
                }
                return;
            }

            if (parts[1] == "ERR" && parts.size() >= 3)
            {
                setStatus("NAT server error: " + QString::fromUtf8(parts[2]), "#b00020");
                return;
            }

            QMessageBox::warning(this, "Configure P2P Code",
                "Unexpected response from the NAT server.");
        };

        connect(btnCheck, &QPushButton::clicked, &dlg, runCheck);
        connect(btnAccept, &QPushButton::clicked, &dlg, &QDialog::accept);
        connect(btnCancel, &QPushButton::clicked, &dlg, &QDialog::reject);
        connect(codeEdit, &QLineEdit::textChanged, &dlg, [&]() { resetAvailability(); });
        connect(codeEdit, &QLineEdit::returnPressed, &dlg, runCheck);
        layout->activate();
        dlg.resize(std::max(dlg.sizeHint().width(), 380), dlg.sizeHint().height());
        dlg.setFixedSize(dlg.size());
        codeEdit->setFocus();

        if (dlg.exec() != QDialog::Accepted)
        {
            return;
        }

        requested = approvedTarget;
        if (requested.isEmpty())
        {
            continue;
        }

        QByteArray payload = QByteArray(kN02TraversalProtocol) + "|CLAIM|" + requested.toUtf8();
        if (!ownerToken.isEmpty())
        {
            payload += "|" + ownerToken.toUtf8();
        }

        QList<QByteArray> parts;
        QString error;
        if (!sendTraversalRequest(payload, parts, error))
        {
            QMessageBox::warning(this, "Configure P2P Code", error);
            return;
        }

        if (parts[1] == "CLAIMOK" && parts.size() >= 4)
        {
            const QString code = normalizeTraversalCode(QString::fromUtf8(parts[2]));
            const QString token = QString::fromUtf8(parts[3]).trimmed();
            if (code.isEmpty() || token.isEmpty())
            {
                QMessageBox::warning(this, "Configure P2P Code",
                    "The NAT server returned an invalid code response.");
                return;
            }

            CoreSettingsSetValue(SettingsID::Kaillera_P2PStaticCode, code.toStdString());
            CoreSettingsSetValue(SettingsID::Kaillera_P2PStaticCodeOwnerToken, token.toStdString());
            CoreSettingsSave();
            refreshP2PStaticCodeDisplay();

            QString confirmError;
            const bool confirmed = confirmTraversalClaim(code, token, confirmError);

            if (confirmed && !oldOwnerToken.isEmpty() && oldOwnerToken != token && (!oldCode.isEmpty() && oldCode != code))
            {
                releaseOldTraversalReservation(this, oldOwnerToken);
            }

            if (confirmed)
            {
                showP2PCodeStatusMessage("Connect code updated to " + code + ".", QColor("#2E7D32"));
            }
            else
            {
                QMessageBox::warning(this, "Configure P2P Code",
                    "Your connect code was saved locally as " + code +
                    ", but the NAT server did not confirm it yet.\n" + confirmError +
                    "\n\nThe code will be retried automatically the next time you host.");
            }
            return;
        }

        if (parts[1] == "CLAIMSUGGEST" && parts.size() >= 4)
        {
            requested = normalizeTraversalCode(QString::fromUtf8(parts[3]));
            QMessageBox::information(this, "Configure P2P Code",
                "That code was taken before it could be claimed.\n"
                "Suggested code: " + requested);
            continue;
        }

        if (parts[1] == "ERR" && parts.size() >= 3)
        {
            QMessageBox::warning(this, "Configure P2P Code",
                "NAT server error: " + QString::fromUtf8(parts[2]));
            return;
        }

        QMessageBox::warning(this, "Configure P2P Code",
            "Unexpected response from the NAT server.");
        return;
    }
}

void KailleraNetplayDialog::onAddServer()
{
    QDialog dlg(this);
    dlg.setWindowTitle("Add Server");
    auto* layout = new QVBoxLayout(&dlg);
    auto* nameEdit = new QLineEdit(&dlg);
    nameEdit->setPlaceholderText("Server Name");
    auto* hostEdit = new QLineEdit(&dlg);
    hostEdit->setPlaceholderText("Host (ip:port)");
    hostEdit->setText("127.0.0.1:27888");
    auto* btnLayout = new QHBoxLayout();
    auto* btnOk = new QPushButton("OK", &dlg);
    auto* btnCancel = new QPushButton("Cancel", &dlg);
    btnLayout->addStretch();
    btnLayout->addWidget(btnOk);
    btnLayout->addWidget(btnCancel);
    layout->addWidget(nameEdit);
    layout->addWidget(hostEdit);
    layout->addLayout(btnLayout);
    connect(btnOk, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(btnCancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    nameEdit->setFocus();

    if (dlg.exec() != QDialog::Accepted) return;
    QString name = nameEdit->text().trimmed();
    QString host = hostEdit->text().trimmed();
    if (name.isEmpty() || host.isEmpty()) return;

    const int existingFavorite = favoriteServerIndexByHost(host);
    if (existingFavorite >= 0)
    {
        m_favoriteServers[existingFavorite].name = name;
    }
    else
    {
        ServerEntry entry{name, host, QString(), "-", -1, "-", 999999};
        const int cachedIndex = cachedServerIndexByHost(host);
        if (cachedIndex >= 0)
        {
            entry.country = m_cachedLiveServers[cachedIndex].country;
            entry.ping = m_cachedLiveServers[cachedIndex].ping;
            entry.pingValue = m_cachedLiveServers[cachedIndex].pingValue;
        }
        m_favoriteServers.append(entry);
    }
    refreshServerListDisplay();
    saveServerList();
    schedulePingAllServers();
}

void KailleraNetplayDialog::onEditServer()
{
    int row = m_serverTable->currentRow();
    if (row < 0 || row >= m_displayServers.size()) return;

    const QString selectedHost = m_displayServers[row].host;
    const int favoriteIndex = favoriteServerIndexByHost(selectedHost);
    if (favoriteIndex < 0 || favoriteIndex >= m_favoriteServers.size()) return;

    QDialog dlg(this);
    dlg.setWindowTitle("Edit Server");
    auto* layout = new QVBoxLayout(&dlg);
    auto* nameEdit = new QLineEdit(&dlg);
    nameEdit->setPlaceholderText("Server Name");
    nameEdit->setText(m_favoriteServers[favoriteIndex].name);
    auto* hostEdit = new QLineEdit(&dlg);
    hostEdit->setPlaceholderText("Host (ip:port)");
    hostEdit->setText(m_favoriteServers[favoriteIndex].host);
    auto* btnLayout = new QHBoxLayout();
    auto* btnOk = new QPushButton("OK", &dlg);
    auto* btnCancel = new QPushButton("Cancel", &dlg);
    btnLayout->addStretch();
    btnLayout->addWidget(btnOk);
    btnLayout->addWidget(btnCancel);
    layout->addWidget(nameEdit);
    layout->addWidget(hostEdit);
    layout->addLayout(btnLayout);
    connect(btnOk, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(btnCancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    nameEdit->setFocus();

    if (dlg.exec() != QDialog::Accepted) return;
    QString name = nameEdit->text().trimmed();
    QString host = hostEdit->text().trimmed();
    if (name.isEmpty() || host.isEmpty()) return;
    const int duplicateFavorite = favoriteServerIndexByHost(host);
    if (duplicateFavorite >= 0 && duplicateFavorite != favoriteIndex)
    {
        QMessageBox::information(this, "Edit Server",
            "That host is already in your favorites.");
        return;
    }

    const QString previousHost = m_favoriteServers[favoriteIndex].host;
    const QString previousCountry = m_favoriteServers[favoriteIndex].country;

    m_favoriteServers[favoriteIndex].name = name;
    m_favoriteServers[favoriteIndex].host = host;
    m_favoriteServers[favoriteIndex].country = (host == previousHost) ? previousCountry : QString();
    m_favoriteServers[favoriteIndex].ping = "-";
    m_favoriteServers[favoriteIndex].pingValue = 999999;
    const int cachedIndex = cachedServerIndexByHost(host);
    if (cachedIndex >= 0)
    {
        m_favoriteServers[favoriteIndex].country = m_cachedLiveServers[cachedIndex].country;
    }
    refreshServerListDisplay();
    saveServerList();
    schedulePingAllServers();
}

void KailleraNetplayDialog::onServerRightClicked(QPoint pos)
{
    int row = m_serverTable->rowAt(pos.y());
    if (row < 0 || row >= m_displayServers.size()) return;

    m_serverTable->selectRow(row);
    const ServerEntry& entry = m_displayServers[row];
    const int favoriteIndex = favoriteServerIndexByHost(entry.host);
    const bool favorite = favoriteIndex >= 0;

    QMenu menu(this);
    QAction* actFavorite = menu.addAction(favorite ? "Unfavorite" : "Favorite");
    QAction* actEdit = nullptr;
    QAction* actMoveUp = nullptr;
    QAction* actMoveDown = nullptr;
    if (favorite)
    {
        actEdit = menu.addAction("Edit");
        actMoveUp = menu.addAction("Move Favorite Up");
        actMoveDown = menu.addAction("Move Favorite Down");
        actMoveUp->setEnabled(favoriteIndex > 0);
        actMoveDown->setEnabled(favoriteIndex + 1 < m_favoriteServers.size());
        menu.addSeparator();
    }
    QAction* actCopyIp = menu.addAction("Copy IP");
    QAction* actPing = menu.addAction("Ping");
    QAction* actTraceroute = menu.addAction("Traceroute");

    QAction* chosen = menu.exec(m_serverTable->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == actFavorite)
    {
        toggleFavoriteServer(entry.host, entry.name);
    }
    else if (chosen == actEdit)
    {
        onEditServer();
    }
    else if (chosen == actMoveUp)
    {
        moveFavoriteServer(favoriteIndex, -1);
    }
    else if (chosen == actMoveDown)
    {
        moveFavoriteServer(favoriteIndex, 1);
    }
    else if (chosen == actCopyIp)
    {
        QApplication::clipboard()->setText(entry.host.section(':', 0, 0));
    }
    else if (chosen == actPing)
    {
        const QString hostStr = entry.host;
        QByteArray ipBytes;
        int port = 27888;
        const int colonIdx = hostStr.lastIndexOf(':');
        if (colonIdx >= 0)
        {
            ipBytes = hostStr.left(colonIdx).toUtf8();
            port = hostStr.mid(colonIdx + 1).toInt();
            if (port == 0)
            {
                port = 27888;
            }
        }
        else
        {
            ipBytes = hostStr.toUtf8();
        }

        updateServerPing(hostStr, -2);
        QApplication::processEvents();
        updateServerPing(hostStr, kaillera_ping_server(ipBytes.data(), port, 2000));
    }
    else if (chosen == actTraceroute)
    {
        const QString ip = entry.host.split(':').first();
#ifdef Q_OS_WIN
        const QString traceCmd = QStringLiteral("tracert");
#else
        const QString traceCmd = QStringLiteral("traceroute");
#endif

        auto* dlg = new QDialog(this);
        dlg->setWindowTitle("Traceroute - " + ip);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->resize(600, 400);

        auto* layout = new QVBoxLayout(dlg);
        auto* output = new QPlainTextEdit(dlg);
        output->setReadOnly(true);
        QFont monoFont("monospace");
        monoFont.setStyleHint(QFont::Monospace);
        output->setFont(monoFont);
        layout->addWidget(output);

        auto* proc = new QProcess(dlg);
        proc->setProcessChannelMode(QProcess::MergedChannels);
        connect(proc, &QProcess::readyReadStandardOutput, dlg, [proc, output]() {
            output->moveCursor(QTextCursor::End);
            output->insertPlainText(QString::fromLocal8Bit(proc->readAllStandardOutput()));
            output->ensureCursorVisible();
        });
        connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                dlg, [output](int exitCode, QProcess::ExitStatus) {
            output->moveCursor(QTextCursor::End);
            output->insertPlainText(QString("\n--- Finished (exit code %1) ---\n").arg(exitCode));
            output->ensureCursorVisible();
        });
        connect(proc, &QProcess::errorOccurred, dlg, [output](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                output->moveCursor(QTextCursor::End);
                output->insertPlainText("Error: Could not start traceroute. Is it installed?\n");
                output->ensureCursorVisible();
            }
        });

        output->insertPlainText("$ " + traceCmd + " " + ip + "\n\n");
        proc->start(traceCmd, QStringList() << ip);
        dlg->show();
    }
}


void KailleraNetplayDialog::onWaitingGames()
{
    m_btnWaitingGames->setEnabled(false);

    QNetworkRequest request(QUrl("http://kaillerareborn.2manygames.fr/game_list.php"));
    QNetworkReply* reply = m_netManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onWaitingGamesReply(reply);
    });
}

void KailleraNetplayDialog::onWaitingGamesReply(QNetworkReply* reply)
{
    m_btnWaitingGames->setEnabled(true);
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError)
    {
        QMessageBox::warning(this, "Waiting Games", "Error: " + reply->errorString());
        return;
    }

    QByteArray data = reply->readAll();
    if (data.size() < 50)
    {
        QMessageBox::information(this, "Waiting Games", "No waiting games found.");
        return;
    }

    // Build popup dialog
    QDialog* wgDialog = new QDialog(this);
    wgDialog->setWindowTitle("Waiting Games");
    wgDialog->setMinimumSize(600, 400);
    wgDialog->resize(700, 450);
    const QString theme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_Theme));
    if (theme == "Modern")
    {
        wgDialog->setObjectName("KailleraLauncherDialog");
        wgDialog->setStyleSheet(buildLauncherStyleSheet(theme));
    }

    auto* wgLayout = new QVBoxLayout(wgDialog);

    auto* wgTable = new QTableWidget(0, 5, wgDialog);
    wgTable->setObjectName("KailleraSurface");
    wgTable->setProperty("launcherWaitingGamesTable", true);
    wgTable->setHorizontalHeaderLabels({"Game", "Emulator", "User", "Server", "IP"});
    wgTable->horizontalHeader()->setStretchLastSection(true);
    wgTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    wgTable->setSelectionMode(QAbstractItemView::SingleSelection);
    wgTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    wgTable->setSortingEnabled(true);
    wgTable->horizontalHeader()->setMinimumSectionSize(16);
    wgTable->verticalHeader()->setVisible(false);
    wgTable->setShowGrid(false);
    applyNoAccentStyle(wgTable);
    installHeaderDoubleClickSortToggle(wgTable);
    wgLayout->addWidget(wgTable);

    auto* wgBtnLayout = new QHBoxLayout();
    auto* btnAddToList = new QPushButton("Favorite Server", wgDialog);
    auto* btnWgClose = new QPushButton("Close", wgDialog);
    if (theme == "Modern")
    {
        btnAddToList->setObjectName("KailleraSecondaryButton");
        btnWgClose->setObjectName("KailleraSecondaryButton");
        configureLauncherButtonMetrics(btnAddToList);
        configureLauncherButtonMetrics(btnWgClose);
    }
    wgBtnLayout->addWidget(btnAddToList);
    wgBtnLayout->addStretch();
    wgBtnLayout->addWidget(btnWgClose);
    wgLayout->addLayout(wgBtnLayout);

    QString connectWaitingGameServerHost;
    QString connectWaitingGameServerName;

    auto queueWaitingGameServerConnect = [wgTable, wgDialog, &connectWaitingGameServerHost,
                                          &connectWaitingGameServerName](int row) {
        if (row < 0)
        {
            return;
        }

        QTableWidgetItem* serverNameItem = wgTable->item(row, 3);
        QTableWidgetItem* hostPortItem = wgTable->item(row, 4);
        if (serverNameItem == nullptr || hostPortItem == nullptr)
        {
            return;
        }

        connectWaitingGameServerName = serverNameItem->text().trimmed();
        connectWaitingGameServerHost = hostPortItem->text().trimmed();
        if (connectWaitingGameServerHost.isEmpty())
        {
            return;
        }

        wgDialog->accept();
    };

    connect(btnWgClose, &QPushButton::clicked, wgDialog, &QDialog::accept);
    connect(wgTable, &QTableWidget::cellDoubleClicked, this,
        [queueWaitingGameServerConnect](int row, int) {
            queueWaitingGameServerConnect(row);
        });

    // Parse: pipe-delimited fields, 7 per entry
    // GameName|IP:Port|Username|EmuName|WaitingPlayers|ServerName|ServerLocation|...
    QStringList fields = QString::fromUtf8(data).split('|', Qt::SkipEmptyParts);

    int total = 0;
    wgTable->setSortingEnabled(false);
    for (int i = 0; i + 6 < fields.size(); i += 7)
    {
        QString gameName = fields[i].trimmed();
        QString hostPort = fields[i + 1].trimmed();
        QString username = fields[i + 2].trimmed();
        QString emulator = fields[i + 3].trimmed();
        QString serverName = fields[i + 5].trimmed();

        if (isPrivateHostPort(hostPort)) continue;

        int row = wgTable->rowCount();
        wgTable->insertRow(row);
        wgTable->setItem(row, 0, new QTableWidgetItem(gameName));
        wgTable->setItem(row, 1, new QTableWidgetItem(emulator));
        wgTable->setItem(row, 2, new QTableWidgetItem(username));
        wgTable->setItem(row, 3, new QTableWidgetItem(serverName));
        wgTable->setItem(row, 4, new QTableWidgetItem(hostPort));
        total++;
    }

    wgTable->setSortingEnabled(true);

    connect(btnAddToList, &QPushButton::clicked, this, [this, wgTable, wgDialog]() {
        int row = wgTable->currentRow();
        if (row < 0) return;

        QString serverName = wgTable->item(row, 3)->text();
        QString hostPort = wgTable->item(row, 4)->text();

        if (favoriteServerIndexByHost(hostPort) >= 0)
        {
            QMessageBox::information(wgDialog, "Already Favorited",
                "This server is already in your favorites.");
            return;
        }

        toggleFavoriteServer(hostPort, serverName);
    });

    wgDialog->exec();
    delete wgDialog;

    if (connectWaitingGameServerHost.isEmpty())
    {
        return;
    }

    int displayIndex = -1;
    for (int i = 0; i < m_displayServers.size(); ++i)
    {
        if (m_displayServers[i].host == connectWaitingGameServerHost)
        {
            displayIndex = i;
            break;
        }
    }

    if (displayIndex < 0)
    {
        if (favoriteServerIndexByHost(connectWaitingGameServerHost) < 0)
        {
            const int cachedIndex = cachedServerIndexByHost(connectWaitingGameServerHost);
            if (cachedIndex < 0)
            {
                m_cachedLiveServers.append({
                    connectWaitingGameServerName.isEmpty()
                        ? connectWaitingGameServerHost
                        : connectWaitingGameServerName,
                    connectWaitingGameServerHost,
                    QString(),
                    "-",
                    -1,
                    "-",
                    999999
                });
            }
            else if (!connectWaitingGameServerName.isEmpty())
            {
                m_cachedLiveServers[cachedIndex].name = connectWaitingGameServerName;
            }

            saveServerList();
        }

        refreshServerListDisplay();

        for (int i = 0; i < m_displayServers.size(); ++i)
        {
            if (m_displayServers[i].host == connectWaitingGameServerHost)
            {
                displayIndex = i;
                break;
            }
        }
    }

    if (displayIndex >= 0)
    {
        m_serverTable->selectRow(displayIndex);
        onConnectServer();
    }
}

void KailleraNetplayDialog::onConnectServer()
{
    int row = m_serverTable->currentRow();
    if (row < 0 || row >= m_displayServers.size()) return;
    const ServerEntry& entry = m_displayServers[row];

    // Browser pings use the same legacy socket registry as connect/login.
    // Drain them before starting a server session to avoid cross-thread
    // overlap in the n02 networking layer.
    m_serverPingsSuspended = true;
    stopServerPingQueue();
    waitForActiveServerPing(false);

    // Parse host:port
    QString hostStr = entry.host;
    QByteArray ipBytes;
    int port = 27888;
    int colonIdx = hostStr.lastIndexOf(':');
    if (colonIdx >= 0)
    {
        ipBytes = hostStr.left(colonIdx).toUtf8();
        port = hostStr.mid(colonIdx + 1).toInt();
        if (port == 0) port = 27888;
    }
    else
    {
        ipBytes = hostStr.toUtf8();
    }

    // Set spoof ping from frame delay combo
    int fdlyIndex = m_frameDelayCombo->currentIndex();
    if (fdlyIndex > 0 && fdlyIndex <= 9)
    {
        kaillera_set_spoof_ping(fdlyIndex * 16 - 8);
    }
    else
    {
        kaillera_set_spoof_ping(0);
    }

    // Get username
    QByteArray usernameBytes = m_usernameEdit->text().toUtf8();
    if (usernameBytes.isEmpty()) usernameBytes = "Player";
    CoreSettingsSetValue(SettingsID::Kaillera_Username,
                         QString::fromUtf8(usernameBytes).toStdString());

    // Initialize kaillera core for server mode
    if (kaillera_core_initialize(0, APP, usernameBytes.data(), 1))
    {
        const bool stateTimerWasRunning =
            (m_stateMachineTimer != nullptr && m_stateMachineTimer->isActive());

        // IMPORTANT: pause the n02 state-machine timer while connect runs on a
        // worker thread. Both paths touch shared socket globals inside n02, and
        // running them concurrently can corrupt state and crash on teardown.
        if (stateTimerWasRunning)
        {
            m_stateMachineTimer->stop();
        }

        // Run connect on a background thread so the UI stays responsive
        // (kaillera_core_connect blocks for up to 15 seconds on timeout)
        auto connectFuture = std::async(std::launch::async, [&]() {
            return kaillera_core_connect(ipBytes.data(), port);
        });

        // Show a progress dialog while connecting
        QProgressDialog progress("Connecting to " + entry.name + "...",
                                 "Cancel", 0, 0, this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(0);
        progress.show();

        // Poll until the future completes or user cancels
        while (connectFuture.wait_for(std::chrono::milliseconds(kConnectPollIntervalMs)) != std::future_status::ready)
        {
            QApplication::processEvents();
            if (progress.wasCanceled())
            {
                // Can't cancel the blocking socket wait, so keep processing events until it finishes
                while (connectFuture.wait_for(std::chrono::milliseconds(kConnectPollIntervalMs)) != std::future_status::ready)
                    QApplication::processEvents();
                kaillera_core_cleanup();
                if (stateTimerWasRunning && m_stateMachineTimer != nullptr)
                {
                    m_stateMachineTimer->start(1);
                }
                m_serverPingsSuspended = false;
                schedulePingAllServers();
                return;
            }
        }
        const bool connected = connectFuture.get();
        std::unique_ptr<KailleraServerBrowserDialog> browser;
        if (connected)
        {
            // Construct the browser before finishing login so it receives the
            // initial LONGSUCC lobby population callbacks.
            browser = std::make_unique<KailleraServerBrowserDialog>(entry.name, nullptr);

            // Finish the login speed test before handing control back to the
            // Qt timer. The old Win32 client effectively kept polling here,
            // which let the server measure the true join RTT instead of a
            // timer-paced ACK cadence.
            kaillera_core_finish_login(1000);
        }
        progress.close();
        if (stateTimerWasRunning && m_stateMachineTimer != nullptr)
        {
            m_stateMachineTimer->start(1);
        }

        if (connected)
        {
            // Hide the netplay dialog while the server browser is open
            hide();

            // Open the server browser dialog as a standalone top-level window
            // so it doesn't stay on top of the emulator frame
            browser->show();

            QEventLoop loop;
            connect(browser.get(), &QDialog::finished, &loop, &QEventLoop::quit);
            loop.exec();
            // Browser dialog handles disconnect/cleanup on close

            // Re-show the netplay dialog, unless the main window
            // is closing (user clicked X on the emulator window).
            // The dialog has no Qt parent (nullptr) to avoid Linux
            // window-stacking issues, so check the active windows instead.
            bool mainWindowAlive = false;
            for (QWidget* w : QApplication::topLevelWidgets())
            {
                if (qobject_cast<QMainWindow*>(w) && w->isVisible())
                {
                    mainWindowAlive = true;
                    break;
                }
            }
            if (mainWindowAlive)
            {
                show();
                m_serverPingsSuspended = false;
                schedulePingAllServers();
            }
            else
            {
                accept();
            }
        }
        else
        {
            QString errorMsg = QString::fromUtf8(kaillera_core_get_last_error());
            kaillera_core_cleanup();
            if (errorMsg.isEmpty())
            {
                errorMsg = "Failed to connect to server";
            }
            QMessageBox::warning(this, "Connection Error",
                                 errorMsg + "\n\nServer: " + entry.name);
            m_serverPingsSuspended = false;
            schedulePingAllServers();
        }
    }
    else
    {
        QMessageBox::warning(this, "Connection Error", "Failed to initialize Kaillera core.");
        m_serverPingsSuspended = false;
        schedulePingAllServers();
    }
}

void KailleraNetplayDialog::onServerDoubleClicked(int row, int column)
{
    (void)column;
    if (row >= 0 && row < m_displayServers.size())
    {
        m_serverTable->selectRow(row);
        onConnectServer();
    }
}

void KailleraNetplayDialog::onP2PHost()
{
    hostP2P(true);
}

void KailleraNetplayDialog::onP2PHostPrivate()
{
    hostP2P(false);
}

void KailleraNetplayDialog::hostP2P(bool showOnPublicList)
{
    // Use selected game from the host picker
    QString gameName = (m_p2pGameCombo != nullptr) ? m_p2pGameCombo->currentText().trimmed() : QString();
    if (gameName.isEmpty())
    {
        QMessageBox::warning(this, "P2P Host", "No game selected. Choose a ROM to host.");
        return;
    }
    if (containsForbiddenNetplayListCharacter(gameName))
    {
        QMessageBox::warning(this, "P2P Host", "Game names containing {, }, or | are not permitted.");
        return;
    }

    QString username = (m_usernameEdit != nullptr) ? m_usernameEdit->text() : QString();
    if (username.isEmpty()) username = "Player";
    if (containsForbiddenNetplayListCharacter(username))
    {
        QMessageBox::warning(this, "P2P Host", "Usernames containing {, }, or | are not permitted.");
        return;
    }

    if (m_p2pAutoClaimSocket != nullptr &&
        currentP2PStaticCode().isEmpty() &&
        currentP2PStaticCodeOwnerToken().isEmpty())
    {
        m_p2pHostLaunchQueued = true;
        m_p2pHostLaunchQueuedPublic = showOnPublicList;
        if (m_btnP2PHost != nullptr)
        {
            m_btnP2PHost->setEnabled(false);
        }
        if (m_btnP2PHostPrivate != nullptr)
        {
            m_btnP2PHostPrivate->setEnabled(false);
        }
        return;
    }

    QByteArray usernameBytes = username.toUtf8();
    CoreSettingsSetValue(SettingsID::Kaillera_Username,
                         username.toStdString());

    QByteArray gameBytes = gameName.toUtf8();

    int port = CoreSettingsGetIntValue(SettingsID::Kaillera_Port);
    if (port <= 0 || port > 65535) port = 27886;

    if (p2p_core_initialize(true, port, APP, gameBytes.data(), usernameBytes.data()))
    {
        const bool stateTimerWasRunning =
            (m_stateMachineTimer != nullptr && m_stateMachineTimer->isActive());
        if (stateTimerWasRunning)
        {
            m_stateMachineTimer->stop();
        }

        hide();

        bool rollbackSessionActive = false;
        QString username = QString::fromUtf8(usernameBytes);
        KailleraP2PDialog p2pDialog(true, gameName, username, QString(), nullptr, showOnPublicList);
        connectRollbackSessionLaunch(p2pDialog, rollbackSessionActive);
        p2pDialog.show();

        QEventLoop loop;
        connect(&p2pDialog, &QDialog::finished, &loop, &QEventLoop::quit);
        loop.exec();

        if (stateTimerWasRunning && m_stateMachineTimer != nullptr)
        {
            m_stateMachineTimer->start(1);
        }

        if (rollbackSessionActive)
        {
            accept();
            return;
        }

        show();
        refreshP2PStaticCodeDisplay();
    }
    else
    {
        QMessageBox::warning(this, "P2P Host", "Failed to initialize P2P core.");
    }
}

// Check if a string looks like a NAT traversal code rather than an IP address.
static bool looksLikeTraversalCode(const QString& s)
{
    QString text = s.trimmed().toUpper();
    text.remove(' ');
    if (text.isEmpty()) return false;
    for (const QChar& ch : text)
    {
        if (ch == '.' || ch == ':' || ch == '/') return false;
    }
    if (text.size() < 4) return false;

    int prefixLength = 0;
    while (prefixLength < text.size() && prefixLength < 4 && text[prefixLength].isLetter())
    {
        ++prefixLength;
    }
    if (prefixLength < 3) return false;
    if (prefixLength < text.size() && text[prefixLength].isLetter()) return false;

    QString digits = text.mid(prefixLength);
    if (digits.startsWith('@') || digits.startsWith('#') || digits.startsWith('-') || digits.startsWith('_'))
        digits.remove(0, 1);
    if (digits.isEmpty()) return false;
    if (digits.size() > kMaxTraversalDigits) return false;
    for (const QChar& ch : digits)
    {
        if (!ch.isDigit()) return false;
    }
    return true;
}

static QString stripTraversalLeadingZeros(QString digits)
{
    int firstNonZero = 0;
    while (firstNonZero < digits.size() && digits[firstNonZero] == '0')
    {
        ++firstNonZero;
    }
    if (firstNonZero >= digits.size())
    {
        return "0";
    }
    return digits.mid(firstNonZero);
}

static QString normalizeTraversalCode(const QString& s)
{
    QString text = s.trimmed().toUpper();
    text.remove(' ');
    if (!looksLikeTraversalCode(text)) return QString();

    int prefixLength = 0;
    while (prefixLength < text.size() && prefixLength < 4 && text[prefixLength].isLetter())
    {
        ++prefixLength;
    }

    QString digits = text.mid(prefixLength);
    if (digits.startsWith('@') || digits.startsWith('#') || digits.startsWith('-') || digits.startsWith('_'))
        digits.remove(0, 1);
    if (digits.size() > kMaxTraversalDigits) return QString();
    digits = stripTraversalLeadingZeros(digits);

    return text.left(prefixLength) + "@" + digits;
}

static QString normalizeTraversalClaimTarget(const QString& s)
{
    QString text = s.trimmed().toUpper();
    text.remove(' ');
    if (text.isEmpty()) return QString();
    if (text == "AUTO") return text;

    for (const QChar& ch : text)
    {
        if (ch == '.' || ch == ':' || ch == '/') return QString();
    }

    int prefixLength = 0;
    while (prefixLength < text.size() && prefixLength < 4 && text[prefixLength].isLetter())
    {
        ++prefixLength;
    }
    if (prefixLength < 3) return QString();
    if (prefixLength < text.size() && text[prefixLength].isLetter()) return QString();

    const QString prefix = text.left(prefixLength);
    QString suffix = text.mid(prefixLength);
    if (suffix.isEmpty()) return prefix;
    if (suffix.startsWith('@') || suffix.startsWith('#') || suffix.startsWith('-') || suffix.startsWith('_'))
        suffix.remove(0, 1);
    if (suffix.isEmpty()) return prefix;
    if (suffix.size() > kMaxTraversalDigits) return QString();

    for (const QChar& ch : suffix)
    {
        if (!ch.isDigit()) return QString();
    }
    suffix = stripTraversalLeadingZeros(suffix);

    return prefix + "@" + suffix;
}

void KailleraNetplayDialog::onP2PJoin()
{
    QString addrText = m_p2pHostEdit->text().trimmed();
    addrText.remove(' ');
    if (addrText.isEmpty())
    {
        QMessageBox::warning(this, "P2P Join", "Please enter a connect code or host address (ip:port).");
        return;
    }

    QByteArray usernameBytes = m_usernameEdit->text().toUtf8();
    if (usernameBytes.isEmpty()) usernameBytes = "Player";
    CoreSettingsSetValue(SettingsID::Kaillera_Username,
                         QString::fromUtf8(usernameBytes).toStdString());

    bool isCode = looksLikeTraversalCode(addrText);
    const QString normalizedCode = isCode ? normalizeTraversalCode(addrText) : QString();

    if (p2p_core_initialize(false, 0, APP, (char*)"", usernameBytes.data()))
    {
        const bool stateTimerWasRunning =
            (m_stateMachineTimer != nullptr && m_stateMachineTimer->isActive());
        if (stateTimerWasRunning)
        {
            m_stateMachineTimer->stop();
        }

        if (isCode)
        {
            // Join by traversal code — the dialog handles connecting via NAT traversal
            hide();

            QString username = QString::fromUtf8(usernameBytes);
            KailleraP2PDialog p2pDialog(false, QString(), username, normalizedCode, nullptr);
            connect(&p2pDialog, &KailleraP2PDialog::peerNicknameResolved, this,
                    [this, normalizedCode](const QString& nickname) {
                        updateP2PStoredNickname(normalizedCode, nickname);
                    },
                    Qt::QueuedConnection);
            bool rollbackSessionActive = false;
            connectRollbackSessionLaunch(p2pDialog, rollbackSessionActive);
            p2pDialog.show();

            QEventLoop loop;
            connect(&p2pDialog, &QDialog::finished, &loop, &QEventLoop::quit);
            loop.exec();

            if (stateTimerWasRunning && m_stateMachineTimer != nullptr)
            {
                m_stateMachineTimer->start(1);
            }

            if (rollbackSessionActive)
            {
                accept();
                return;
            }

            show();
        }
        else
        {
            // Join by direct IP:port
            QByteArray ipBytes;
            int port = 27886;
            int colonIdx = addrText.lastIndexOf(':');
            if (colonIdx >= 0)
            {
                ipBytes = addrText.left(colonIdx).toUtf8();
                port = addrText.mid(colonIdx + 1).toInt();
                if (port == 0) port = 27886;
            }
            else
            {
                ipBytes = addrText.toUtf8();
            }

            if (p2p_core_connect(ipBytes.data(), port))
            {
                hide();

                QString username = QString::fromUtf8(usernameBytes);
                KailleraP2PDialog p2pDialog(false, QString(), username, QString(), nullptr);
                connect(&p2pDialog, &KailleraP2PDialog::peerNicknameResolved, this,
                        [this, addrText](const QString& nickname) {
                            updateP2PStoredNickname(addrText, nickname);
                        },
                        Qt::QueuedConnection);
                bool rollbackSessionActive = false;
                connectRollbackSessionLaunch(p2pDialog, rollbackSessionActive);
                p2pDialog.show();

                QEventLoop loop;
                connect(&p2pDialog, &QDialog::finished, &loop, &QEventLoop::quit);
                loop.exec();

                if (stateTimerWasRunning && m_stateMachineTimer != nullptr)
                {
                    m_stateMachineTimer->start(1);
                }

                if (rollbackSessionActive)
                {
                    accept();
                    return;
                }

                show();
            }
            else
            {
                p2p_core_cleanup();
                if (stateTimerWasRunning && m_stateMachineTimer != nullptr)
                {
                    m_stateMachineTimer->start(1);
                }
                QMessageBox::warning(this, "P2P Join", "Failed to connect to host: " + addrText);
            }
        }
    }
    else
    {
        QMessageBox::warning(this, "P2P Join", "Failed to initialize P2P core.");
    }
}

// ---- P2P recent/favorite peers ----

void KailleraNetplayDialog::loadP2PStoredUsers()
{
    QSettings settings("RMG-K", "n02");
    m_p2pStoredUsers.clear();

    int count = settings.value("P2P_HistoryCount", -1).toInt();
    if (count >= 0)
    {
        for (int i = 0; i < count; i++)
        {
            P2PStoredEntry entry;
            entry.name = settings.value(QString("P2P_HistoryName_%1").arg(i)).toString().trimmed();
            entry.host = settings.value(QString("P2P_HistoryHost_%1").arg(i)).toString().trimmed();
            entry.favorite = settings.value(QString("P2P_HistoryFavorite_%1").arg(i), false).toBool();
            if (!entry.host.isEmpty())
            {
                m_p2pStoredUsers.append(entry);
            }
        }
    }
    else
    {
        count = settings.value("P2P_StoredCount", 0).toInt();
        for (int i = 0; i < count; i++)
        {
            P2PStoredEntry entry;
            entry.name = settings.value(QString("P2P_StoredName_%1").arg(i)).toString().trimmed();
            entry.host = settings.value(QString("P2P_StoredHost_%1").arg(i)).toString().trimmed();
            if (!entry.host.isEmpty())
            {
                m_p2pStoredUsers.append(entry);
            }
        }
    }

    QVector<P2PStoredEntry> favorites;
    QVector<P2PStoredEntry> recents;
    favorites.reserve(m_p2pStoredUsers.size());
    recents.reserve(m_p2pStoredUsers.size());
    for (const auto& entry : m_p2pStoredUsers)
    {
        if (entry.favorite)
        {
            favorites.append(entry);
        }
        else
        {
            recents.append(entry);
        }
    }
    m_p2pStoredUsers = favorites;
    m_p2pStoredUsers += recents;

    while (m_p2pStoredUsers.size() - p2pFavoriteCount() > kMaxP2PRecentEntries)
    {
        m_p2pStoredUsers.removeLast();
    }

    refreshP2PStoredDisplay();
}

void KailleraNetplayDialog::saveP2PStoredUsers()
{
    QSettings settings("RMG-K", "n02");
    const int cleanupCount = std::max(
        settings.value("P2P_HistoryCount", 0).toInt(),
        settings.value("P2P_StoredCount", 0).toInt());
    settings.setValue("P2P_HistoryCount", m_p2pStoredUsers.size());
    for (int i = 0; i < cleanupCount; i++)
    {
        settings.remove(QString("P2P_HistoryName_%1").arg(i));
        settings.remove(QString("P2P_HistoryHost_%1").arg(i));
        settings.remove(QString("P2P_HistoryFavorite_%1").arg(i));
        settings.remove(QString("P2P_StoredName_%1").arg(i));
        settings.remove(QString("P2P_StoredHost_%1").arg(i));
    }
    for (int i = 0; i < m_p2pStoredUsers.size(); i++)
    {
        settings.setValue(QString("P2P_HistoryName_%1").arg(i), m_p2pStoredUsers[i].name);
        settings.setValue(QString("P2P_HistoryHost_%1").arg(i), m_p2pStoredUsers[i].host);
        settings.setValue(QString("P2P_HistoryFavorite_%1").arg(i), m_p2pStoredUsers[i].favorite);
    }
    settings.setValue("P2P_StoredCount", 0);
}

void KailleraNetplayDialog::refreshP2PStoredDisplay()
{
    if (!m_p2pStoredList) return;

    QString selectedHost;
    if (QListWidgetItem* currentItem = m_p2pStoredList->currentItem(); currentItem != nullptr)
    {
        selectedHost = currentItem->data(P2PStoredHostRole).toString();
    }
    if (selectedHost.isEmpty() && m_p2pHostEdit != nullptr)
    {
        selectedHost = m_p2pHostEdit->text().trimmed();
    }

    QSignalBlocker blocker(m_p2pStoredList);
    m_p2pStoredList->clear();

    int selectedRow = -1;
    for (int i = 0; i < m_p2pStoredUsers.size(); i++)
    {
        auto* item = new QListWidgetItem();
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        item->setData(P2PStoredNameRole, m_p2pStoredUsers[i].name);
        item->setData(P2PStoredHostRole, m_p2pStoredUsers[i].host);
        item->setData(P2PStoredFavoriteRole, m_p2pStoredUsers[i].favorite);
        item->setToolTip(m_p2pStoredUsers[i].host);
        item->setSizeHint(QSize(112, 60));
        m_p2pStoredList->addItem(item);

        if (m_p2pStoredUsers[i].host == selectedHost)
        {
            selectedRow = i;
        }
    }

    if (selectedRow >= 0)
    {
        m_p2pStoredList->setCurrentRow(selectedRow);
        m_p2pStoredList->scrollToItem(m_p2pStoredList->item(selectedRow));
    }
    else
    {
        m_p2pStoredList->setCurrentItem(nullptr);
    }
}

int KailleraNetplayDialog::p2pStoredIndexByHost(const QString& host) const
{
    for (int i = 0; i < m_p2pStoredUsers.size(); ++i)
    {
        if (m_p2pStoredUsers[i].host == host)
        {
            return i;
        }
    }
    return -1;
}

int KailleraNetplayDialog::p2pFavoriteCount() const
{
    int count = 0;
    while (count < m_p2pStoredUsers.size() && m_p2pStoredUsers[count].favorite)
    {
        ++count;
    }
    return count;
}

void KailleraNetplayDialog::toggleP2PStoredFavorite(int row)
{
    if (row < 0 || row >= m_p2pStoredUsers.size())
    {
        return;
    }

    P2PStoredEntry entry = m_p2pStoredUsers[row];
    m_p2pStoredUsers.removeAt(row);
    entry.favorite = !entry.favorite;

    const int insertIndex = p2pFavoriteCount();
    m_p2pStoredUsers.insert(insertIndex, entry);
    refreshP2PStoredDisplay();
    if (m_p2pStoredList != nullptr)
    {
        m_p2pStoredList->setCurrentRow(insertIndex);
    }
    saveP2PStoredUsers();
}

void KailleraNetplayDialog::editP2PStoredEntry(int row)
{
    if (row < 0 || row >= m_p2pStoredUsers.size())
    {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle("Edit P2P History Entry");

    auto* layout = new QVBoxLayout(&dialog);
    auto* formLayout = new QFormLayout();
    auto* nameEdit = new QLineEdit(m_p2pStoredUsers[row].name, &dialog);
    auto* hostEdit = new QLineEdit(m_p2pStoredUsers[row].host, &dialog);
    hostEdit->setPlaceholderText("Connect code or ip:port");

    formLayout->addRow("Name:", nameEdit);
    formLayout->addRow("IP/Code:", hostEdit);
    layout->addLayout(formLayout);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    while (dialog.exec() == QDialog::Accepted)
    {
        QString normalizedHost = hostEdit->text().trimmed();
        normalizedHost.remove(' ');

        if (normalizedHost.isEmpty())
        {
            QMessageBox::warning(&dialog, "Edit P2P History Entry", "IP/Code cannot be empty.");
            continue;
        }

        const int existingIndex = p2pStoredIndexByHost(normalizedHost);
        if (existingIndex >= 0 && existingIndex != row)
        {
            QMessageBox::warning(&dialog, "Edit P2P History Entry",
                "That IP/Code is already saved in your P2P history.");
            continue;
        }

        m_p2pStoredUsers[row].name = nameEdit->text().trimmed();
        m_p2pStoredUsers[row].host = normalizedHost;
        refreshP2PStoredDisplay();
        if (m_p2pStoredList != nullptr)
        {
            m_p2pStoredList->setCurrentRow(row);
        }
        saveP2PStoredUsers();
        return;
    }
}

void KailleraNetplayDialog::deleteP2PStoredEntry(int row)
{
    if (row < 0 || row >= m_p2pStoredUsers.size())
    {
        return;
    }

    m_p2pStoredUsers.removeAt(row);
    refreshP2PStoredDisplay();
    saveP2PStoredUsers();
}

void KailleraNetplayDialog::onP2PStoredRightClicked(const QPoint& pos)
{
    if (m_p2pStoredList == nullptr)
    {
        return;
    }

    QListWidgetItem* item = m_p2pStoredList->itemAt(pos);
    if (item == nullptr)
    {
        return;
    }

    const int row = m_p2pStoredList->row(item);
    if (row < 0 || row >= m_p2pStoredUsers.size())
    {
        return;
    }

    m_p2pStoredList->setCurrentRow(row);

    QMenu menu(this);
    QAction* favoriteAction = menu.addAction(
        m_p2pStoredUsers[row].favorite ? "Unfavorite" : "Favorite");
    QAction* editAction = menu.addAction("Edit");
    QAction* deleteAction = menu.addAction("Delete");

    QAction* selectedAction = menu.exec(m_p2pStoredList->viewport()->mapToGlobal(pos));
    if (selectedAction == favoriteAction)
    {
        toggleP2PStoredFavorite(row);
    }
    else if (selectedAction == editAction)
    {
        editP2PStoredEntry(row);
    }
    else if (selectedAction == deleteAction)
    {
        deleteP2PStoredEntry(row);
    }
}

void KailleraNetplayDialog::rememberP2PStoredEntry(const QString& host, const QString& nickname)
{
    QString normalizedHost = host.trimmed();
    normalizedHost.remove(' ');
    if (normalizedHost.isEmpty())
    {
        return;
    }

    const int existingIndex = p2pStoredIndexByHost(normalizedHost);
    if (existingIndex >= 0)
    {
        P2PStoredEntry entry = m_p2pStoredUsers[existingIndex];
        if (!nickname.trimmed().isEmpty())
        {
            entry.name = nickname.trimmed();
        }
        if (entry.favorite)
        {
            m_p2pStoredUsers[existingIndex] = entry;
            refreshP2PStoredDisplay();
            saveP2PStoredUsers();
            return;
        }

        m_p2pStoredUsers.removeAt(existingIndex);
        m_p2pStoredUsers.insert(p2pFavoriteCount(), entry);
        while (m_p2pStoredUsers.size() - p2pFavoriteCount() > kMaxP2PRecentEntries)
        {
            m_p2pStoredUsers.removeLast();
        }
        refreshP2PStoredDisplay();
        saveP2PStoredUsers();
        return;
    }

    m_p2pStoredUsers.insert(p2pFavoriteCount(), {nickname.trimmed(), normalizedHost, false});
    while (m_p2pStoredUsers.size() - p2pFavoriteCount() > kMaxP2PRecentEntries)
    {
        m_p2pStoredUsers.removeLast();
    }

    refreshP2PStoredDisplay();
    saveP2PStoredUsers();
}

void KailleraNetplayDialog::updateP2PStoredNickname(const QString& host, const QString& nickname)
{
    const QString trimmedNickname = nickname.trimmed();
    if (trimmedNickname.isEmpty())
    {
        return;
    }

    rememberP2PStoredEntry(host, trimmedNickname);
}

void KailleraNetplayDialog::onP2PPasteAndGo()
{
    QString clip = QApplication::clipboard()->text().trimmed();
    if (clip.isEmpty()) return;

    // Remove spaces (connect codes shouldn't have spaces)
    clip.remove(' ');

    m_p2pHostEdit->setText(clip);
    onP2PJoin();
}

void KailleraNetplayDialog::fetchP2PWaitingGames(bool manualRefresh)
{
    if (m_p2pWaitingGamesFetchInFlight || m_netManager == nullptr)
    {
        return;
    }

    m_p2pWaitingGamesFetchInFlight = true;
    if (m_btnP2PWaitingGamesReload != nullptr)
    {
        m_btnP2PWaitingGamesReload->setEnabled(false);
        m_btnP2PWaitingGamesReload->setToolTip(manualRefresh ? "Reloading waiting games..." : "Loading waiting games...");
    }

    QNetworkRequest request{QUrl(kP2PWaitingGamesUrl)};
    QNetworkReply* reply = m_netManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        handleP2PWaitingGamesReply(reply);
    });
}

void KailleraNetplayDialog::handleP2PWaitingGamesReply(QNetworkReply* reply)
{
    m_p2pWaitingGamesFetchInFlight = false;

    if (reply == nullptr)
    {
        if (m_btnP2PWaitingGamesReload != nullptr)
        {
            m_btnP2PWaitingGamesReload->setEnabled(true);
            m_btnP2PWaitingGamesReload->setToolTip("Reload waiting games");
        }
        return;
    }

    if (reply->error() != QNetworkReply::NoError)
    {
        if (m_btnP2PWaitingGamesReload != nullptr)
        {
            m_btnP2PWaitingGamesReload->setEnabled(true);
            m_btnP2PWaitingGamesReload->setToolTip("Reload waiting games. Last refresh failed.");
        }
        reply->deleteLater();
        return;
    }

    const QByteArray data = reply->readAll();
    reply->deleteLater();
    populateP2PWaitingGames(data);
}

void KailleraNetplayDialog::populateP2PWaitingGames(const QByteArray& data)
{
    if (m_p2pWaitingGamesTable == nullptr)
    {
        if (m_btnP2PWaitingGamesReload != nullptr)
        {
            m_btnP2PWaitingGamesReload->setEnabled(true);
            m_btnP2PWaitingGamesReload->setToolTip("Reload waiting games");
        }
        return;
    }

    QString selectedCode;
    QString selectedHost;
    const int selectedRow = m_p2pWaitingGamesTable->currentRow();
    if (selectedRow >= 0)
    {
        if (QTableWidgetItem* selectedItem = m_p2pWaitingGamesTable->item(selectedRow, 0); selectedItem != nullptr)
        {
            selectedCode = selectedItem->data(kP2PWaitingCodeRole).toString();
            selectedHost = selectedItem->data(kP2PWaitingHostRole).toString();
        }
    }

    QByteArray body = data;
    const int headerEnd = body.indexOf("\r\n\r\n");
    if (headerEnd >= 0)
    {
        body = body.mid(headerEnd + 4);
    }

    m_p2pWaitingGamesTable->setRowCount(0);

    if (body.trimmed().isEmpty())
    {
        if (m_btnP2PWaitingGamesReload != nullptr)
        {
            m_btnP2PWaitingGamesReload->setEnabled(true);
            m_btnP2PWaitingGamesReload->setToolTip("Reload waiting games");
        }
        return;
    }

    struct WaitingGameEntry {
        QString gameName;
        QString emulator;
        QString userName;
        QString hostPort;
        QString code;
        QString tooltip;
        P2PWaitingStatus status = P2PWaitingStatus::Available;
    };

    QVector<WaitingGameEntry> entries;
    const QList<QByteArray> lines = body.split('\n');
    for (const QByteArray& line : lines)
    {
        if (line.trimmed().isEmpty())
        {
            continue;
        }

        // plist.txt may return either newline-separated rows or one flat pipe stream.
        const QList<QByteArray> fields = line.split('|');
        if (fields.size() < 4)
        {
            continue;
        }

        for (qsizetype fieldIndex = 0; fieldIndex + 3 < fields.size(); fieldIndex += 4)
        {
            const QString gameName = QString::fromUtf8(fields[fieldIndex].trimmed());
            QString emulator = QString::fromUtf8(fields[fieldIndex + 1].trimmed());
            const QString userName = QString::fromUtf8(fields[fieldIndex + 2].trimmed());
            const QString hostPort = QString::fromUtf8(fields[fieldIndex + 3].trimmed());

            QString code;
            extractP2PAppCode(emulator, code);
            if (code.isEmpty())
            {
                continue;
            }

            const bool gameAvailable = localKailleraGameListContains(gameName);
            const QString localEmulator = QString::fromUtf8(APP).trimmed();
            const bool emulatorMismatch = emulator.trimmed() != localEmulator;

            const QString normalizedCode = normalizeTraversalCode(code);
            int storedIndex = normalizedCode.isEmpty() ? -1 : p2pStoredIndexByHost(normalizedCode);
            if (storedIndex < 0)
            {
                storedIndex = p2pStoredIndexByHost(code);
            }
            if (storedIndex < 0)
            {
                storedIndex = p2pStoredIndexByHost(hostPort);
            }

            P2PWaitingStatus status = P2PWaitingStatus::Available;
            QStringList tooltipLines;
            tooltipLines << gameName << emulator << code;
            if (!gameAvailable)
            {
                tooltipLines << "Warning: this ROM is not in your local list.";
            }
            if (emulatorMismatch)
            {
                tooltipLines << QString("Warning: emulator/version differs. Host: %1. You: %2.")
                    .arg(emulator, localEmulator);
            }

            if (!gameAvailable || emulatorMismatch)
            {
                status = P2PWaitingStatus::Warning;
            }
            else if (storedIndex >= 0 && m_p2pStoredUsers[storedIndex].favorite)
            {
                status = P2PWaitingStatus::Favorite;
                tooltipLines << "Favorite saved opponent.";
            }
            else if (storedIndex >= 0)
            {
                status = P2PWaitingStatus::Saved;
                tooltipLines << "Saved opponent.";
            }
            else
            {
                tooltipLines << "Ready to connect.";
            }

            entries.append({gameName, emulator, userName, hostPort, code, tooltipLines.join('\n'), status});
        }
    }

    std::stable_sort(entries.begin(), entries.end(), [](const WaitingGameEntry& a, const WaitingGameEntry& b) {
        return static_cast<int>(a.status) < static_cast<int>(b.status);
    });

    bool restoredSelection = false;
    for (const WaitingGameEntry& entry : entries)
    {
        const int row = m_p2pWaitingGamesTable->rowCount();
        m_p2pWaitingGamesTable->insertRow(row);

        auto* statusItem = new QTableWidgetItem();
        auto* userItem = new QTableWidgetItem(entry.userName);
        auto* gameItem = new QTableWidgetItem(entry.gameName);

        for (QTableWidgetItem* item : {statusItem, userItem, gameItem})
        {
            item->setData(kP2PWaitingCodeRole, entry.code);
            item->setData(kP2PWaitingHostRole, entry.hostPort);
            item->setToolTip(entry.tooltip);
        }

        statusItem->setData(Qt::UserRole, p2pWaitingStatusIcon(entry.status));
        statusItem->setTextAlignment(Qt::AlignCenter);
        statusItem->setFlags((statusItem->flags() | Qt::ItemIsEnabled | Qt::ItemIsSelectable)
            & ~Qt::ItemIsEditable);

        m_p2pWaitingGamesTable->setItem(row, 0, statusItem);
        m_p2pWaitingGamesTable->setItem(row, 1, userItem);
        m_p2pWaitingGamesTable->setItem(row, 2, gameItem);

        if (!restoredSelection &&
            ((!selectedCode.isEmpty() && selectedCode == entry.code) ||
             (!selectedHost.isEmpty() && selectedHost == entry.hostPort)))
        {
            m_p2pWaitingGamesTable->setCurrentCell(
                row,
                0,
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            restoredSelection = true;
        }
    }

    if (m_btnP2PWaitingGamesReload != nullptr)
    {
        m_btnP2PWaitingGamesReload->setEnabled(true);
        m_btnP2PWaitingGamesReload->setToolTip("Reload waiting games");
    }
}

void KailleraNetplayDialog::useP2PWaitingGameRow(int row, bool connectNow)
{
    if (m_p2pWaitingGamesTable == nullptr || m_p2pHostEdit == nullptr || row < 0)
    {
        return;
    }

    QTableWidgetItem* item = m_p2pWaitingGamesTable->item(row, 0);
    if (item == nullptr)
    {
        return;
    }

    QString code = item->data(kP2PWaitingCodeRole).toString().trimmed();
    QString host = item->data(kP2PWaitingHostRole).toString().trimmed();
    if (code.isEmpty() && host.isEmpty())
    {
        return;
    }

    m_p2pHostEdit->setText(!code.isEmpty() ? code : host);
    if (connectNow)
    {
        onP2PJoin();
    }
}

#endif // NETPLAY
