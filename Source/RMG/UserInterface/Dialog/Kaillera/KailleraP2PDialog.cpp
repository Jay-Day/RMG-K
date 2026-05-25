/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "KailleraP2PDialog.hpp"
#include "KailleraTraversalConfig.hpp"

#ifdef NETPLAY

#include "../../KailleraUIBridge.hpp"

#include <RMG-Core/Emulation.hpp>
#include <RMG-Core/Kaillera.hpp>
#include <RMG-Core/Settings.hpp>

#include "core/p2p_core.h"
#include "kailleraclient.h"
#include "n02_client.h"

#include <QVBoxLayout>
#include <QAction>
#include <QHBoxLayout>
#include <QDateTime>
#include <QClipboard>
#include <QApplication>
#include <QIcon>
#include <QFrame>
#include <QFont>
#include <QListView>
#include <QMessageBox>
#include <QMenu>
#include <QPainter>
#include <QPoint>
#include <QResizeEvent>
#include <QSettings>
#include <QSizePolicy>
#include <QWidgetAction>

#include <algorithm>
#include <cstring>

#include <chrono>
static inline unsigned long monotonicTickCount() {
    using namespace std::chrono;
    return (unsigned long)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static const char* kSsrvHost = "kaillerareborn.2manygames.fr";
static const int kSsrvPort = 27887;
static const char* kGameLayerMessagePrefix = "RMGK:MODE:";
static const char* kGameLayerStandard = "STD";
static const char* kGameLayerRollback = "RB";
static const char* kRollbackDelayMessagePrefix = "RMGK:RBDELAY:";
static const char* kRollbackPredictionMessagePrefix = "RMGK:RBPRED:";
static constexpr int kRollbackDelayModeDefault = 0;
static constexpr int kRollbackDelayModeLower = 1;
static constexpr int kRollbackDelayModeHigher = 2;
static constexpr int kRollbackDelayModeCustom = 3;
static constexpr int kRollbackMinFrameDelay = 1;
static constexpr int kRollbackMaxFrameDelay = 9;
static constexpr int kDefaultRollbackFrameDelay = 2;
static constexpr int kDefaultRollbackPredictionWindow = 7;
static constexpr int kRollbackDelayInitialSampleCount = 5;
static constexpr unsigned long kRollbackDelayInitialUpdateMs = 5000;
static constexpr unsigned long kRollbackDelayUpdateIntervalMs = 10000;
static constexpr unsigned long kRollbackDelaySampleWindowMs = 20000;
static constexpr int kP2PNormalFontPointDelta = 2;
static constexpr int kP2PPlayerKickButtonSize = 20;
static const char* kRollbackDelayHelpText =
    "Lower input delay reduces latency but <b>can increase rollbacks.</b> Higher input delay can reduce rollbacks.";

class KailleraSwitch : public QCheckBox
{
public:
    explicit KailleraSwitch(QWidget* parent = nullptr)
        : QCheckBox(parent)
    {
        setText(QString());
        setFixedSize(sizeHint());
        setFocusPolicy(Qt::TabFocus);
    }

    QSize sizeHint() const override
    {
        return QSize(46, 24);
    }

    QSize minimumSizeHint() const override
    {
        return sizeHint();
    }

protected:
    bool hitButton(const QPoint& pos) const override
    {
        return rect().contains(pos);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QPalette pal = palette();
        const bool darkTheme = pal.window().color().value() < 128;
        const bool checked = isChecked();
        const bool active = isEnabled();

        QColor trackColor;
        QColor borderColor;
        QColor knobColor;
        if (!active)
        {
            trackColor = checked ? QColor("#8cbce4") : pal.button().color();
            borderColor = pal.mid().color();
            knobColor = pal.mid().color();
        }
        else if (checked)
        {
            trackColor = QColor("#0078D7");
            borderColor = QColor("#0066b4");
            knobColor = Qt::white;
        }
        else
        {
            trackColor = darkTheme ? pal.window().color().lighter(135) : QColor("#ffffff");
            borderColor = pal.mid().color();
            knobColor = darkTheme ? pal.midlight().color() : QColor("#aeb6c2");
        }

        const QRectF trackRect(1.0, 1.0, width() - 2.0, height() - 2.0);
        painter.setPen(QPen(borderColor, 1.0));
        painter.setBrush(trackColor);
        painter.drawRoundedRect(trackRect, trackRect.height() / 2.0, trackRect.height() / 2.0);

        const qreal knobMargin = 4.0;
        const qreal knobSize = height() - (knobMargin * 2.0);
        const qreal knobX = checked ? width() - knobMargin - knobSize : knobMargin;
        const QRectF shadowRect(knobX, knobMargin + 1.0, knobSize, knobSize);
        const QRectF knobRect(knobX, knobMargin, knobSize, knobSize);

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, active ? 36 : 18));
        painter.drawEllipse(shadowRect);
        painter.setBrush(knobColor);
        painter.drawEllipse(knobRect);

        if (hasFocus())
        {
            QPen focusPen(pal.highlight().color(), 1.0, Qt::DashLine);
            painter.setPen(focusPen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(trackRect.adjusted(2.0, 2.0, -2.0, -2.0),
                                    (trackRect.height() - 4.0) / 2.0,
                                    (trackRect.height() - 4.0) / 2.0);
        }
    }
};

static int clampRollbackFrameDelay(int delay)
{
    return std::clamp(delay, kRollbackMinFrameDelay, kRollbackMaxFrameDelay);
}

static int normalizeRollbackDelayMode(int mode)
{
    if (mode < kRollbackDelayModeDefault || mode > kRollbackDelayModeCustom)
    {
        return kRollbackDelayModeDefault;
    }
    return mode;
}

static int normalizeRollbackPredictionWindow(int predictionWindow)
{
    if (predictionWindow < 0 || predictionWindow > 10)
    {
        return 0;
    }
    return predictionWindow;
}

static int automaticRollbackFrameDelayForPing(int ping)
{
    if (ping < 0)
    {
        return kDefaultRollbackFrameDelay;
    }
    if (ping <= 50)
    {
        return 1;
    }
    if (ping <= 100)
    {
        return 2;
    }
    if (ping <= 140)
    {
        return 3;
    }
    if (ping <= 200)
    {
        return 4;
    }
    return 5;
}

static bool isImportantP2PDebugMessage(const QString& text)
{
    const QString lower = text.toLower();
    return lower.contains("error") ||
        lower.contains("timed out") ||
        lower.contains("timeout") ||
        lower.contains("failed") ||
        lower.contains("dropped connection") ||
        lower.contains("different emu") ||
        lower.contains("different version") ||
        lower.contains("desync") ||
        lower.contains("cant quit");
}

static void increaseP2PNormalFontSizes(QWidget* root, int pointDelta)
{
    if (root == nullptr || pointDelta == 0)
    {
        return;
    }

    QList<QWidget*> widgets = root->findChildren<QWidget*>();
    QList<QFont> originalFonts;
    originalFonts.reserve(widgets.size());
    for (QWidget* widget : widgets)
    {
        originalFonts.append(widget != nullptr ? widget->font() : QFont());
    }

    const auto adjustFont = [pointDelta](QWidget* widget, QFont font) {
        if (widget == nullptr || widget->objectName() == "KailleraP2PSurface")
        {
            return;
        }

        if (font.pointSize() > 0)
        {
            font.setPointSize(std::max(1, font.pointSize() + pointDelta));
        }
        else if (font.pointSizeF() > 0.0)
        {
            font.setPointSizeF(std::max(1.0, font.pointSizeF() + pointDelta));
        }
        else if (font.pixelSize() > 0)
        {
            font.setPixelSize(std::max(1, font.pixelSize() + pointDelta));
        }
        widget->setFont(font);
    };

    for (int i = 0; i < widgets.size(); i++)
    {
        adjustFont(widgets.at(i), originalFonts.at(i));
    }
}

static void disableDefaultButtonBehavior(QWidget* root)
{
    if (root == nullptr)
    {
        return;
    }

    const QList<QPushButton*> buttons = root->findChildren<QPushButton*>();
    for (QPushButton* button : buttons)
    {
        button->setAutoDefault(false);
        button->setDefault(false);
    }
}

static void updateReadyBadge(QLabel* badge, bool ready)
{
    if (badge == nullptr)
    {
        return;
    }

    badge->setText(ready ? QString::fromUtf8("\xe2\x9c\x93 Ready") : QString("Not Ready"));
    badge->setStyleSheet(ready ?
        "QLabel {"
        "  color: #087a2f;"
        "  font-weight: 700;"
        "}" :
        "QLabel {"
        "  color: #a66a00;"
        "  font-weight: 700;"
        "}");
}

static QString timestamp()
{
    return QDateTime::currentDateTime().toString("[h:mm AP] ");
}

static QString formatTraversalCode(const QString& prefix, int number)
{
    return prefix + "@" + QString::number(number);
}

static QIcon themedP2PIcon(const QString& iconName)
{
    const bool darkTheme = QApplication::palette().window().color().value() < 128;
    return QIcon(QString(":/icons/%1/svg/%2.svg")
        .arg(darkTheme ? "white" : "black", iconName));
}

static bool localGameListContains(const QString& gameName)
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

static QString buildP2PStyleSheet(const QString& theme)
{
    if (theme != "Modern")
    {
        return QString();
    }

    const bool darkTheme = QApplication::palette().window().color().value() < 128;
    const QString comboArrowIcon = QString(":/icons/%1/svg/arrow-down-s-line.svg")
        .arg(darkTheme ? "white" : "black");
    const QString footerStatusColor = darkTheme ? "#b8c0cc" : "#555f6a";
    const QString mutedTextColor = darkTheme ? "#c8ced6" : "#6a737d";
    const QString hostCodeColor = darkTheme ? "#4ab7ed" : "#0096d3";
    const QString controlBorderColor = darkTheme ? "#697380" : "#b8bec6";

    return QString(
        "QDialog#KailleraP2PDialog {"
        "  background-color: palette(window);"
        "}"
        "QFrame#KailleraP2PTopBar {"
        "  border: 1px solid palette(mid);"
        "  border-radius: 10px;"
        "  background-color: palette(base);"
        "}"
        "QLabel#KailleraP2PGameModeLabel {"
        "  color: #3f45c9;"
        "  font-weight: 700;"
        "}"
        "QLabel#KailleraP2PGameBanner {"
        "  padding: 0px;"
        "  font-size: 18px;"
        "  font-weight: 800;"
        "}"
        "QLabel#KailleraP2PSubtleLabel {"
        "  color: palette(mid);"
        "  font-weight: 500;"
        "}"
        "QLabel#KailleraP2PPeerStatus {"
        "  color: palette(text);"
        "  padding: 0px 2px;"
        "  font-weight: 600;"
        "}"
        "QFrame#KailleraP2PPlayerCard {"
        "  border: none;"
        "  border-radius: 8px;"
        "  background-color: palette(window);"
        "}"
        "QLabel#KailleraP2PPlayerName {"
        "  font-weight: 700;"
        "}"
        "QFrame#KailleraP2PCodeBadge {"
        "  border: none;"
        "  background-color: transparent;"
        "}"
        "QLabel#KailleraP2PCodeTitle {"
        "  color: %4;"
        "  font-size: 10px;"
        "  font-weight: 700;"
        "}"
        "QLabel#KailleraP2PCodeText {"
        "  color: %5;"
        "  font-size: 22px;"
        "  font-weight: 800;"
        "}"
        "QPushButton#KailleraP2PCodeCopyButton {"
        "  border: 1px solid palette(mid);"
        "  border-radius: 7px;"
        "  padding: 0px;"
        "  background-color: palette(button);"
        "}"
        "QPushButton#KailleraP2PCodeCopyButton:hover {"
        "  border-color: palette(dark);"
        "  background-color: palette(light);"
        "}"
        "QPushButton#KailleraP2PCodeCopyButton:pressed {"
        "  background-color: palette(midlight);"
        "}"
        "QLabel#KailleraP2PStatusLabel {"
        "  color: palette(text);"
        "  padding: 0 2px;"
        "  font-weight: 600;"
        "}"
        "QLabel#KailleraP2PFooterStatusLabel {"
        "  color: %3;"
        "  padding: 0 2px;"
        "  font-weight: 600;"
        "}"
        "QLabel#KailleraP2PHelpText {"
        "  color: %4;"
        "  font-size: 11px;"
        "  padding: 0 2px;"
        "}"
        "QLabel#KailleraP2PSectionLabel {"
        "  color: palette(text);"
        "  padding: 2px 0px 0px 0px;"
        "  font-weight: 700;"
        "}"
        "QTextBrowser#KailleraP2PSurface {"
        "  border: none;"
        "  background-color: transparent;"
        "  padding: 6px;"
        "}"
        "QLineEdit#KailleraP2PInput {"
        "  border: 1px solid %6;"
        "  border-radius: 7px;"
        "  background-color: palette(base);"
        "  padding: 5px 8px;"
        "  min-height: 24px;"
        "}"
        "QLineEdit#KailleraP2PInput:focus {"
        "  border-color: palette(highlight);"
        "}"
        "QComboBox#KailleraP2PCombo {"
        "  border: 1px solid %6;"
        "  border-radius: 7px;"
        "  background-color: palette(base);"
        "  padding: 4px 8px;"
        "  min-height: 24px;"
        "}"
        "QComboBox#KailleraP2PCombo:focus {"
        "  border-color: palette(highlight);"
        "}"
        "QComboBox#KailleraP2PCombo::drop-down {"
        "  subcontrol-origin: padding;"
        "  subcontrol-position: top right;"
        "  width: 24px;"
        "  border: none;"
        "  border-left: 1px solid %6;"
        "  border-top-right-radius: 7px;"
        "  border-bottom-right-radius: 7px;"
        "  background-color: transparent;"
        "  margin: 1px;"
        "}"
        "QComboBox#KailleraP2PCombo::down-arrow {"
        "  image: url(%1);"
        "  width: 12px;"
        "  height: 12px;"
        "}"
        "QComboBox#KailleraP2PCombo QAbstractItemView {"
        "  border: 1px solid %6;"
        "  background-color: palette(base);"
        "  selection-background-color: palette(highlight);"
        "  selection-color: palette(highlighted-text);"
        "  outline: none;"
        "  padding: 0px;"
        "  margin: 0px;"
        "}"
        "QComboBox#KailleraP2PCombo QAbstractItemView::item {"
        "  padding: 0px 8px;"
        "  min-height: 18px;"
        "  margin: 0px;"
        "}"
        "QGroupBox#KailleraP2PGroup {"
        "  border: 1px solid palette(mid);"
        "  border-radius: 10px;"
        "  background-color: palette(base);"
        "  margin-top: 10px;"
        "  padding-top: 6px;"
        "}"
        "QGroupBox#KailleraP2PGroup::title {"
        "  subcontrol-origin: margin;"
        "  left: 10px;"
        "  padding: 0 4px;"
        "  font-weight: 600;"
        "}"
        "QWidget#KailleraChatComposer {"
        "  border: 1px solid %6;"
        "  border-radius: 7px;"
        "  background-color: palette(base);"
        "}"
        "QLineEdit#KailleraChatComposerInput {"
        "  border: none;"
        "  background: transparent;"
        "  padding: 2px 0px;"
        "  min-height: 22px;"
        "}"
        "QLineEdit#KailleraChatComposerInput:focus {"
        "  border: none;"
        "}"
        "QPushButton#KailleraChatComposerSendButton {"
        "  border: none;"
        "  border-radius: 6px;"
        "  min-width: 30px;"
        "  min-height: 30px;"
        "  max-width: 30px;"
        "  max-height: 30px;"
        "  padding: 0px;"
        "  background-color: transparent;"
        "  color: palette(text);"
        "  font-size: 15px;"
        "  font-weight: 900;"
        "}"
        "QPushButton#KailleraChatComposerSendButton:hover {"
        "  border: 1px solid palette(mid);"
        "  background-color: palette(light);"
        "}"
        "QPushButton#KailleraChatComposerSendButton:pressed {"
        "  border: 1px solid palette(mid);"
        "  background-color: palette(midlight);"
        "}"
        "QPushButton#KailleraPlayerKickButton {"
        "  border: 1px solid transparent;"
        "  border-radius: 6px;"
        "  min-width: %2px;"
        "  max-width: %2px;"
        "  min-height: %2px;"
        "  max-height: %2px;"
        "  padding: 0px;"
        "  color: #c03a3a;"
        "  background-color: transparent;"
        "  font-weight: 800;"
        "}"
        "QPushButton#KailleraPlayerKickButton:hover {"
        "  border-color: #d77a7a;"
        "  background-color: rgba(208, 72, 72, 0.10);"
        "}"
        "QPushButton#KailleraPlayerKickButton:pressed {"
        "  border-color: #b55a5a;"
        "  background-color: rgba(208, 72, 72, 0.18);"
        "}"
        "QPushButton#KailleraP2PPrimaryButton {"
        "  border: 1px solid #0066b4;"
        "  border-radius: 7px;"
        "  min-height: 24px;"
        "  padding: 4px 12px;"
        "  font-weight: 700;"
        "  color: white;"
        "  background-color: #0078D7;"
        "}"
        "QPushButton#KailleraP2PPrimaryButton:hover {"
        "  background-color: #1584dd;"
        "}"
        "QPushButton#KailleraP2PPrimaryButton:pressed,"
        "QPushButton#KailleraP2PPrimaryButton:checked {"
        "  background-color: #0063b1;"
        "}"
        "QPushButton#KailleraP2PSecondaryButton {"
        "  border: 1px solid palette(mid);"
        "  border-radius: 7px;"
        "  min-height: 24px;"
        "  padding: 4px 12px;"
        "  background-color: palette(window);"
        "  font-weight: 600;"
        "}"
        "QPushButton#KailleraP2PSecondaryButton:hover {"
        "  border-color: palette(dark);"
        "  background-color: palette(light);"
        "}"
        "QPushButton#KailleraP2PSecondaryButton:pressed {"
        "  border-color: palette(shadow);"
        "  background-color: palette(mid);"
        "  padding-top: 5px;"
        "  padding-bottom: 3px;"
        "}"
        "QMenu#KailleraP2PNetcodeMenu {"
        "  border: 1px solid palette(mid);"
        "  border-radius: 7px;"
        "  background-color: palette(base);"
        "  padding: 6px;"
        "}"
        "QMenu#KailleraP2PNetcodeMenu::item {"
        "  color: palette(text);"
        "  padding: 4px 18px 4px 6px;"
        "  border-radius: 4px;"
        "}"
        "QMenu#KailleraP2PNetcodeMenu::item:selected {"
        "  color: white;"
        "  background-color: #0078D7;"
        "}"
        "QMenu#KailleraP2PNetcodeMenu::indicator {"
        "  width: 14px;"
        "  height: 14px;"
        "}"
        "QWidget#KailleraP2PLayerToggle {"
        "  border: 1px solid palette(mid);"
        "  border-radius: 14px;"
        "  background-color: palette(window);"
        "}"
        "QPushButton#KailleraP2PLayerLeftButton,"
        "QPushButton#KailleraP2PLayerRightButton {"
        "  border: none;"
        "  min-height: 24px;"
        "  padding: 3px 14px;"
        "  background-color: transparent;"
        "  font-weight: 600;"
        "}"
        "QPushButton#KailleraP2PLayerLeftButton {"
        "  border-top-left-radius: 13px;"
        "  border-bottom-left-radius: 13px;"
        "}"
        "QPushButton#KailleraP2PLayerRightButton {"
        "  border-top-right-radius: 13px;"
        "  border-bottom-right-radius: 13px;"
        "}"
        "QPushButton#KailleraP2PLayerLeftButton:hover,"
        "QPushButton#KailleraP2PLayerRightButton:hover {"
        "  background-color: palette(light);"
        "}"
        "QPushButton#KailleraP2PLayerLeftButton:checked,"
        "QPushButton#KailleraP2PLayerRightButton:checked {"
        "  background-color: palette(midlight);"
        "  color: palette(text);"
        "  font-weight: 700;"
        "}"
        "QPushButton#KailleraP2PIconButton {"
        "  border: 1px solid transparent;"
        "  border-radius: 7px;"
        "  padding: 0px;"
        "  background-color: transparent;"
        "}"
        "QPushButton#KailleraP2PIconButton:hover {"
        "  border-color: palette(mid);"
        "  background-color: palette(light);"
        "}"
        "QPushButton#KailleraP2PIconButton:pressed {"
        "  background-color: palette(midlight);"
        "}"
        "QPushButton#KailleraP2PIconButton::menu-indicator {"
        "  image: none;"
        "  width: 0px;"
        "}"
    ).arg(comboArrowIcon)
        .arg(kP2PPlayerKickButtonSize)
        .arg(footerStatusColor)
        .arg(mutedTextColor)
        .arg(hostCodeColor)
        .arg(controlBorderColor);
}

static void configureP2PComboPopup(QComboBox* combo, const QString& theme)
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

// Check if a string looks like a NAT traversal code rather than an IP address.
static QString normalizeTraversalCode(const QString& input)
{
    QString s = input.trimmed().toUpper();
    s.remove(' ');
    if (s.isEmpty()) return QString();

    for (const QChar& ch : s)
    {
        if (ch == '.' || ch == ':' || ch == '/') return QString();
    }

    if (s.size() < 4) return QString();

    int prefixLength = 0;
    while (prefixLength < s.size() && prefixLength < 4 && s[prefixLength].isLetter())
    {
        ++prefixLength;
    }
    if (prefixLength < 3) return QString();
    if (prefixLength < s.size() && s[prefixLength].isLetter()) return QString();

    const QString prefix = s.left(prefixLength);
    QString digits = s.mid(prefixLength);
    if (digits.startsWith('@') || digits.startsWith('#') || digits.startsWith('-') || digits.startsWith('_'))
        digits.remove(0, 1);
    if (digits.isEmpty()) return QString();
    if (digits.size() > 3) return QString();

    for (const QChar& ch : digits)
    {
        if (!ch.isDigit()) return QString();
    }

    bool ok = false;
    const int number = digits.toInt(&ok);
    if (!ok || number < 0) return QString();

    return formatTraversalCode(prefix, number);
}

// Matches n02-rmg's LooksLikeTraversalCode() intent, but accepts legacy separators and leading zeroes.
static bool looksLikeTraversalCode(const QString& s)
{
    return !normalizeTraversalCode(s).isEmpty();
}

// Try to extract an IPv4 address and optional port from a string.
// Returns true if an IP was found.
static bool tryExtractIPv4AndPort(const QByteArray& s, QString& outIp, int& outPort)
{
    outIp.clear();
    outPort = 0;

    const char* data = s.constData();
    int len = s.size();

    for (int i = 0; i < len; i++)
    {
        if (data[i] < '0' || data[i] > '9') continue;

        unsigned int octets[4];
        const char* cur = data + i;
        bool ok = true;
        for (int o = 0; o < 4; o++)
        {
            unsigned int val = 0;
            int digits = 0;
            while (*cur >= '0' && *cur <= '9' && digits < 3)
            {
                val = val * 10 + (unsigned int)(*cur - '0');
                cur++;
                digits++;
            }
            if (digits == 0 || val > 255) { ok = false; break; }
            octets[o] = val;
            if (o < 3)
            {
                if (*cur != '.') { ok = false; break; }
                cur++;
            }
        }
        if (!ok) continue;

        outIp = QString("%1.%2.%3.%4").arg(octets[0]).arg(octets[1]).arg(octets[2]).arg(octets[3]);

        // Optional ":port"
        while (*cur == ' ' || *cur == '\t') cur++;
        if (*cur == ':') cur++;
        else if (*cur != 0 && (*cur < '0' || *cur > '9')) return true;

        unsigned int port = 0;
        int digits = 0;
        while (*cur >= '0' && *cur <= '9' && digits < 5)
        {
            port = port * 10 + (unsigned int)(*cur - '0');
            cur++;
            digits++;
        }
        if (digits > 0 && port > 0 && port <= 65535)
            outPort = (int)port;
        return true;
    }

    return false;
}

KailleraP2PDialog::KailleraP2PDialog(bool isHost, const QString& gameName,
                                     const QString& username,
                                     const QString& joinCode, QWidget* parent,
                                     bool showOnPublicList)
    : QDialog(parent, Qt::Window), m_isHost(isHost), m_initialShowOnPublicList(showOnPublicList),
      m_gameName(gameName), m_username(username)
{
    m_lobbyOpening = m_isHost || (!joinCode.isEmpty() && looksLikeTraversalCode(joinCode));
    {
        QSettings settings("RMG-K", "n02");
        m_showDebugMessages = settings.value("P2P_ShowDebugMessages", false).toBool();
        if (m_isHost)
        {
            m_gameLayer = settings.value("P2P_GameLayer", QString(kGameLayerStandard)).toString() == kGameLayerRollback ?
                GameLayer::Rollback : GameLayer::Standard;
        }
    }
    loadLocalRollbackSettings();

    setWindowIcon(QIcon(":Resource/Kaillera.svg"));
    setupUI();
    connectSignals();

    // P2P step timer — drives the p2p state machine (1ms)
    m_stepTimer = new QTimer(this);
    connect(m_stepTimer, &QTimer::timeout, this, &KailleraP2PDialog::onStepTimer);
    m_stepTimer->start(1);

    // NAT traversal housekeeping timer (1s)
    m_travTimer = new QTimer(this);
    connect(m_travTimer, &QTimer::timeout, this, &KailleraP2PDialog::onTravTimer);
    m_travTimer->start(1000);

    travResetState();
    travLoadIdentity();

    if (m_isHost)
    {
        // Host always uses NAT traversal (host by code)
        m_travHostEnabled = true;
        if (!m_travToken.isEmpty() && !m_travCode.isEmpty())
        {
            travSendHostOpen();
        }
        else
        {
            travSendClaimAuto();
        }
        m_travNextRegMs = QDateTime::currentMSecsSinceEpoch() + 2000;
        updateHostCodeUI();

        appendChatStatus("Hosting " + m_gameName + " on port " +
            QString::number(p2p_core_get_port()), "green", true);
    }
    else if (!joinCode.isEmpty() && looksLikeTraversalCode(joinCode))
    {
        // Join by traversal code
        m_travJoinEnabled = true;
        m_travJoinCode = normalizeTraversalCode(joinCode);
        updatePeerConnectionUI();
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        travSendJoin();
        m_travNextJoinMs = now + 3000;
        m_travJoinDeadlineMs = now + 15000;

        appendChatStatus("Looking up host for code " + joinCode, "green", true);
    }
}

KailleraP2PDialog::~KailleraP2PDialog()
{
    cleanupSessionForClose();
}

void KailleraP2PDialog::setupUI()
{
    setObjectName("KailleraP2PDialog");
    setWindowTitle(m_isHost ? "Hosting P2P" : "P2P Game");
    setMinimumSize(820, 540);
    resize(minimumSize());

    const QString theme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_Theme));
    setStyleSheet(buildP2PStyleSheet(theme));

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);

    auto* topBar = new QFrame(this);
    topBar->setObjectName("KailleraP2PTopBar");
    topBar->setFrameStyle(QFrame::Box | QFrame::Sunken);
    topBar->setMinimumHeight(68);
    auto* topBarLayout = new QHBoxLayout(topBar);
    topBarLayout->setContentsMargins(16, 8, 16, 8);
    topBarLayout->setSpacing(14);

    m_lobbyStatusLabel = new QLabel(topBar);
    m_lobbyStatusLabel->setObjectName("KailleraP2PGameModeLabel");
    topBarLayout->addWidget(m_lobbyStatusLabel, 1, Qt::AlignLeft | Qt::AlignVCenter);

    m_hostConnectCodeBadge = new QFrame(topBar);
    m_hostConnectCodeBadge->setObjectName("KailleraP2PCodeBadge");
    auto* hostConnectCodeLayout = new QVBoxLayout(m_hostConnectCodeBadge);
    hostConnectCodeLayout->setContentsMargins(0, 0, 0, 0);
    hostConnectCodeLayout->setSpacing(1);

    auto* hostConnectCodeTitle = new QLabel("HOST CODE", m_hostConnectCodeBadge);
    hostConnectCodeTitle->setObjectName("KailleraP2PCodeTitle");
    hostConnectCodeTitle->setAlignment(Qt::AlignCenter);
    hostConnectCodeLayout->addWidget(hostConnectCodeTitle, 0, Qt::AlignCenter);

    auto* hostConnectCodeRow = new QWidget(m_hostConnectCodeBadge);
    auto* hostConnectCodeRowLayout = new QHBoxLayout(hostConnectCodeRow);
    hostConnectCodeRowLayout->setContentsMargins(0, 0, 0, 0);
    hostConnectCodeRowLayout->setSpacing(8);

    m_hostConnectCodeLabel = new QLabel("--", m_hostConnectCodeBadge);
    m_hostConnectCodeLabel->setObjectName("KailleraP2PCodeText");
    m_hostConnectCodeLabel->setMinimumHeight(30);
    m_hostConnectCodeLabel->setAlignment(Qt::AlignCenter);
    hostConnectCodeRowLayout->addWidget(m_hostConnectCodeLabel, 0, Qt::AlignVCenter);

    m_btnCopyConnectCode = new QPushButton(m_hostConnectCodeBadge);
    m_btnCopyConnectCode->setObjectName("KailleraP2PCodeCopyButton");
    m_btnCopyConnectCode->setIcon(themedP2PIcon("copy-line"));
    m_btnCopyConnectCode->setIconSize(QSize(16, 16));
    m_btnCopyConnectCode->setFixedSize(30, 30);
    m_btnCopyConnectCode->setCursor(Qt::PointingHandCursor);
    m_btnCopyConnectCode->setToolTip("Copy connect code");
    m_btnCopyConnectCode->setVisible(m_isHost);
    m_btnCopyConnectCode->setEnabled(false);
    connect(m_btnCopyConnectCode, &QPushButton::clicked, this, &KailleraP2PDialog::onCopyConnectCode);
    hostConnectCodeRowLayout->addWidget(m_btnCopyConnectCode, 0, Qt::AlignVCenter);
    hostConnectCodeLayout->addWidget(hostConnectCodeRow, 0, Qt::AlignCenter);
    topBarLayout->addWidget(m_hostConnectCodeBadge, 0, Qt::AlignCenter);

    m_gameLabel = new QLabel(topBar);
    m_gameLabel->setObjectName("KailleraP2PGameBanner");
    m_gameLabel->setText(m_gameName.isEmpty() ? "Waiting for game" : m_gameName);
    m_gameLabel->setMinimumWidth(0);
    m_gameLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    topBarLayout->addWidget(m_gameLabel, 1);

    mainLayout->addWidget(topBar);

    auto* contentLayout = new QHBoxLayout();
    contentLayout->setSpacing(10);

    auto* chatColumn = new QWidget(this);
    auto* chatColumnLayout = new QVBoxLayout(chatColumn);
    chatColumnLayout->setContentsMargins(0, 0, 0, 0);
    chatColumnLayout->setSpacing(10);

    m_chatGroup = new QGroupBox("Chat", chatColumn);
    m_chatGroup->setObjectName("KailleraP2PGroup");
    auto* chatLayout = new QVBoxLayout(m_chatGroup);
    chatLayout->setContentsMargins(9, 10, 9, 9);
    chatLayout->setSpacing(8);

    m_chatSettingsButton = new QPushButton(m_chatGroup);
    m_chatSettingsButton->setObjectName("KailleraP2PIconButton");
    m_chatSettingsButton->setIcon(themedP2PIcon("settings-3-line"));
    m_chatSettingsButton->setIconSize(QSize(17, 17));
    m_chatSettingsButton->setFixedSize(24, 24);
    m_chatSettingsButton->setCursor(Qt::PointingHandCursor);
    m_chatSettingsButton->setToolTip("Chat settings");
    auto* chatSettingsMenu = new QMenu(m_chatSettingsButton);
    chatSettingsMenu->setObjectName("KailleraP2PNetcodeMenu");
    m_showDebugMessagesAction = chatSettingsMenu->addAction("Show debug messages");
    m_showDebugMessagesAction->setCheckable(true);
    m_showDebugMessagesAction->setChecked(m_showDebugMessages);
    m_chatSettingsButton->setMenu(chatSettingsMenu);
    connect(m_showDebugMessagesAction, &QAction::toggled, this, &KailleraP2PDialog::setShowDebugMessages);

    m_chat = new QTextBrowser(m_chatGroup);
    m_chat->setObjectName("KailleraP2PSurface");
    m_chat->setFrameStyle(QFrame::NoFrame);
    m_chat->setStyleSheet("QTextBrowser { border: none; background: transparent; }");
    m_chat->setOpenExternalLinks(true);
    m_chat->document()->setMaximumBlockCount(1000);
    QFont chatFont = m_chat->font();
    if (chatFont.pointSize() > 0)
    {
        chatFont.setPointSize(chatFont.pointSize() + 2);
    }
    else if (chatFont.pointSizeF() > 0.0)
    {
        chatFont.setPointSizeF(chatFont.pointSizeF() + 2.0);
    }
    m_chat->setFont(chatFont);
    m_chat->document()->setDefaultFont(chatFont);
    chatLayout->addWidget(m_chat, 1);

    auto* chatInputLayout = new QHBoxLayout();
    chatInputLayout->setContentsMargins(0, 0, 0, 0);

    auto* chatComposer = new QWidget(m_chatGroup);
    chatComposer->setObjectName("KailleraChatComposer");
    chatComposer->setFixedHeight(36);
    auto* chatComposerLayout = new QHBoxLayout(chatComposer);
    chatComposerLayout->setContentsMargins(10, 3, 5, 3);
    chatComposerLayout->setSpacing(6);

    m_chatInput = new QLineEdit(chatComposer);
    m_chatInput->setPlaceholderText("Type a message...");
    m_chatInput->setClearButtonEnabled(true);
    m_chatInput->setObjectName("KailleraChatComposerInput");
    m_chatInput->setFrame(false);

    m_btnChat = new QPushButton(chatComposer);
    m_btnChat->setObjectName("KailleraChatComposerSendButton");
    m_btnChat->setToolTip("Send message");
    m_btnChat->setText("");
    m_btnChat->setIcon(themedP2PIcon("play-line"));
    m_btnChat->setIconSize(QSize(24, 24));

    chatComposerLayout->addWidget(m_chatInput);
    chatComposerLayout->addWidget(m_btnChat, 0, Qt::AlignVCenter);
    chatInputLayout->addWidget(chatComposer, 1);
    chatLayout->addLayout(chatInputLayout);
    chatColumnLayout->addWidget(m_chatGroup, 1);

    auto* chatActionLayout = new QHBoxLayout();
    chatActionLayout->setContentsMargins(0, 0, 0, 0);
    chatActionLayout->setSpacing(8);
    m_btnReady = new QPushButton("Ready", chatColumn);
    m_btnReady->setCheckable(true);
    m_btnReady->setObjectName("KailleraP2PPrimaryButton");
    m_btnReady->setIcon(QIcon(":/icons/white/svg/check-line.svg"));
    m_btnReady->setIconSize(QSize(16, 16));
    if (!m_isHost)
    {
        m_btnReady->setEnabled(false);
    }
    m_btnDrop = new QPushButton("Close Game", chatColumn);
    m_btnDrop->setObjectName("KailleraP2PSecondaryButton");
    m_btnDrop->setIcon(themedP2PIcon("close-line"));
    m_btnDrop->setIconSize(QSize(16, 16));
    m_btnLeave = new QPushButton("Leave", chatColumn);
    m_btnLeave->setObjectName("KailleraP2PSecondaryButton");
    m_btnLeave->setIcon(themedP2PIcon("arrow-bar-right-line"));
    m_btnLeave->setIconSize(QSize(16, 16));
    chatActionLayout->addWidget(m_btnReady, 1);
    chatActionLayout->addWidget(m_btnDrop, 1);
    chatActionLayout->addWidget(m_btnLeave, 1);
    chatColumnLayout->addLayout(chatActionLayout);

    auto* rightWidget = new QWidget(this);
    rightWidget->setMinimumWidth(310);
    auto* rightLayout = new QVBoxLayout(rightWidget);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(10);

    auto* playersGroup = new QGroupBox("Players", rightWidget);
    playersGroup->setObjectName("KailleraP2PGroup");
    playersGroup->setMinimumHeight(150);
    auto* playersLayout = new QVBoxLayout(playersGroup);
    playersLayout->setContentsMargins(9, 10, 9, 9);
    playersLayout->setSpacing(8);

    m_hostPlayerCard = new QFrame(playersGroup);
    m_hostPlayerCard->setObjectName("KailleraP2PPlayerCard");
    auto* hostPlayerCardLayout = new QHBoxLayout(m_hostPlayerCard);
    hostPlayerCardLayout->setContentsMargins(10, 8, 8, 8);
    hostPlayerCardLayout->setSpacing(8);

    m_hostPlayerNameLabel = new QLabel(m_hostPlayerCard);
    m_hostPlayerNameLabel->setObjectName("KailleraP2PPlayerName");
    hostPlayerCardLayout->addWidget(m_hostPlayerNameLabel, 1);

    m_hostReadyLabel = new QLabel(m_hostPlayerCard);
    updateReadyBadge(m_hostReadyLabel, false);
    hostPlayerCardLayout->addWidget(m_hostReadyLabel, 0, Qt::AlignVCenter);
    auto* hostKickPlaceholder = new QWidget(m_hostPlayerCard);
    hostKickPlaceholder->setFixedSize(kP2PPlayerKickButtonSize, kP2PPlayerKickButtonSize);
    hostPlayerCardLayout->addWidget(hostKickPlaceholder, 0, Qt::AlignVCenter);

    playersLayout->addWidget(m_hostPlayerCard);

    m_playersEmptyLabel = new QLabel(playersGroup);
    m_playersEmptyLabel->setObjectName("KailleraP2PStatusLabel");
    m_playersEmptyLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    playersLayout->addWidget(m_playersEmptyLabel);

    m_playerCard = new QFrame(playersGroup);
    m_playerCard->setObjectName("KailleraP2PPlayerCard");
    auto* playerCardLayout = new QHBoxLayout(m_playerCard);
    playerCardLayout->setContentsMargins(10, 8, 8, 8);
    playerCardLayout->setSpacing(8);

    m_playerNameLabel = new QLabel(m_playerCard);
    m_playerNameLabel->setObjectName("KailleraP2PPlayerName");
    playerCardLayout->addWidget(m_playerNameLabel, 1);

    m_playerReadyLabel = new QLabel(m_playerCard);
    updateReadyBadge(m_playerReadyLabel, false);
    playerCardLayout->addWidget(m_playerReadyLabel, 0, Qt::AlignVCenter);

    m_btnKickPeer = new QPushButton("X", m_playerCard);
    m_btnKickPeer->setObjectName("KailleraPlayerKickButton");
    m_btnKickPeer->setCursor(Qt::PointingHandCursor);
    m_btnKickPeer->setToolTip("Kick opponent from the lobby");
    QSizePolicy kickButtonPolicy = m_btnKickPeer->sizePolicy();
    kickButtonPolicy.setRetainSizeWhenHidden(true);
    m_btnKickPeer->setSizePolicy(kickButtonPolicy);
    m_btnKickPeer->setVisible(false);
    connect(m_btnKickPeer, &QPushButton::clicked, this, &KailleraP2PDialog::onKickPeer);
    playerCardLayout->addWidget(m_btnKickPeer, 0, Qt::AlignVCenter);
    playersLayout->addWidget(m_playerCard);
    rightLayout->addWidget(playersGroup, 0);

    m_hostGroup = new QGroupBox("Session Settings", rightWidget);
    m_hostGroup->setObjectName("KailleraP2PGroup");
    auto* hostLayout = new QVBoxLayout(m_hostGroup);
    hostLayout->setContentsMargins(9, 10, 9, 9);
    hostLayout->setSpacing(8);

    if (m_isHost)
    {
        m_netcodeSettingsButton = new QPushButton(m_hostGroup);
        m_netcodeSettingsButton->setObjectName("KailleraP2PIconButton");
        m_netcodeSettingsButton->setIcon(themedP2PIcon("settings-3-line"));
        m_netcodeSettingsButton->setIconSize(QSize(20, 20));
        m_netcodeSettingsButton->setFixedSize(26, 26);
        m_netcodeSettingsButton->setCursor(Qt::PointingHandCursor);
        m_netcodeSettingsButton->setToolTip("Netcode settings");

        auto* netcodeMenu = new QMenu(m_netcodeSettingsButton);
        netcodeMenu->setObjectName("KailleraP2PNetcodeMenu");
        auto* netcodeWidget = new QWidget(netcodeMenu);
        auto* netcodeLayout = new QVBoxLayout(netcodeWidget);
        netcodeLayout->setContentsMargins(0, 0, 0, 0);
        netcodeLayout->setSpacing(6);

        auto* layerRow = new QHBoxLayout();
        layerRow->setContentsMargins(0, 0, 0, 0);
        layerRow->setSpacing(8);
        auto* layerLabel = new QLabel("Netcode:", netcodeWidget);
        layerRow->addWidget(layerLabel);

        auto* layerToggle = new QWidget(netcodeWidget);
        layerToggle->setObjectName("KailleraP2PLayerToggle");
        auto* layerToggleLayout = new QHBoxLayout(layerToggle);
        layerToggleLayout->setContentsMargins(2, 2, 2, 2);
        layerToggleLayout->setSpacing(0);

        m_standardLayerButton = new QPushButton("Standard", layerToggle);
        m_standardLayerButton->setObjectName("KailleraP2PLayerLeftButton");
        m_standardLayerButton->setCheckable(true);
        m_standardLayerButton->setAutoExclusive(true);
        m_standardLayerButton->setToolTip("Use the standard Kaillera game layer");
        m_rollbackLayerButton = new QPushButton("Rollback", layerToggle);
        m_rollbackLayerButton->setObjectName("KailleraP2PLayerRightButton");
        m_rollbackLayerButton->setCheckable(true);
        m_rollbackLayerButton->setAutoExclusive(true);
        m_rollbackLayerButton->setToolTip("Use the rollback game layer");
        layerToggleLayout->addWidget(m_standardLayerButton);
        layerToggleLayout->addWidget(m_rollbackLayerButton);

        layerRow->addWidget(layerToggle);
        netcodeLayout->addLayout(layerRow);

        auto* netcodeMenuAction = new QWidgetAction(netcodeMenu);
        netcodeMenuAction->setDefaultWidget(netcodeWidget);
        netcodeMenu->addAction(netcodeMenuAction);
        m_netcodeSettingsButton->setMenu(netcodeMenu);

        connect(m_standardLayerButton, &QPushButton::clicked, this, [this, netcodeMenu]() {
            setGameLayer(GameLayer::Standard, true, true);
            netcodeMenu->hide();
        });
        connect(m_rollbackLayerButton, &QPushButton::clicked, this, [this, netcodeMenu]() {
            setGameLayer(GameLayer::Rollback, true, true);
            netcodeMenu->hide();
        });
    }

    m_frameDelayRow = new QWidget(m_hostGroup);
    auto* fdlyLayout = new QHBoxLayout(m_frameDelayRow);
    fdlyLayout->setContentsMargins(0, 0, 0, 0);
    fdlyLayout->setSpacing(6);
    m_frameDelayLabel = new QLabel(m_frameDelayRow);
    m_frameDelayLabel->setText(isRollbackMode() ? "Input Delay:" : "Frame Delay:");
    fdlyLayout->addWidget(m_frameDelayLabel);
    m_frameDelayCombo = new QComboBox(m_frameDelayRow);
    m_frameDelayCombo->setObjectName("KailleraP2PCombo");
    m_frameDelayCombo->setMinimumWidth(126);
    m_frameDelayCombo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
    configureP2PComboPopup(m_frameDelayCombo, theme);
    connect(m_frameDelayCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (isRollbackMode())
        {
            if (canEditRollbackDelaySettings() && index >= 0)
            {
                setRollbackDelayMode(m_frameDelayCombo->itemData(index).toInt(), m_isHost, true);
            }
        }
        else if (m_isHost)
        {
            if (index < 0)
            {
                return;
            }
            m_standardFrameDelay = index;
            KailleraUIBridge::instance().setSelectedDelay(m_standardFrameDelay);
        }
    });
    fdlyLayout->addWidget(m_frameDelayCombo);

    m_customFrameDelayCombo = new QComboBox(m_frameDelayRow);
    m_customFrameDelayCombo->setObjectName("KailleraP2PCombo");
    m_customFrameDelayCombo->setMinimumWidth(66);
    m_customFrameDelayCombo->setMaximumWidth(72);
    m_customFrameDelayCombo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
    configureP2PComboPopup(m_customFrameDelayCombo, theme);
    for (int frames = kRollbackMinFrameDelay; frames <= kRollbackMaxFrameDelay; frames++)
    {
        m_customFrameDelayCombo->addItem(QString("%1f").arg(frames), frames);
    }
    const int customDelayIndex = m_customFrameDelayCombo->findData(m_customRollbackFrameDelay);
    m_customFrameDelayCombo->setCurrentIndex(customDelayIndex >= 0 ? customDelayIndex : 0);
    connect(m_customFrameDelayCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (isRollbackMode() && canEditRollbackDelaySettings() &&
            m_rollbackDelayMode == kRollbackDelayModeCustom && index >= 0)
        {
            setRollbackCustomFrameDelay(m_customFrameDelayCombo->itemData(index).toInt(), m_isHost, true);
        }
    });
    fdlyLayout->addWidget(m_customFrameDelayCombo);
    fdlyLayout->addStretch();
    hostLayout->addWidget(m_frameDelayRow);

    m_frameDelayHelpLabel = new QLabel(kRollbackDelayHelpText, m_hostGroup);
    m_frameDelayHelpLabel->setObjectName("KailleraP2PHelpText");
    m_frameDelayHelpLabel->setTextFormat(Qt::RichText);
    m_frameDelayHelpLabel->setWordWrap(true);
    hostLayout->addWidget(m_frameDelayHelpLabel);

    auto* predictionLayout = new QHBoxLayout();
    predictionLayout->setContentsMargins(0, 0, 0, 0);
    predictionLayout->setSpacing(6);
    auto* predictionWindowLabel = new QLabel("Prediction:", m_hostGroup);
    predictionLayout->addWidget(predictionWindowLabel);
    m_predictionWindowCombo = new QComboBox(m_hostGroup);
    m_predictionWindowCombo->setObjectName("KailleraP2PCombo");
    m_predictionWindowCombo->setMinimumWidth(120);
    m_predictionWindowCombo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
    configureP2PComboPopup(m_predictionWindowCombo, theme);
    m_predictionWindowCombo->addItem("Default", 0);
    for (int frames = 1; frames <= 10; frames++)
    {
        m_predictionWindowCombo->addItem(frames == 1 ? "1 frame" : QString("%1 frames").arg(frames), frames);
    }
    const int predictionWindowIndex = m_predictionWindowCombo->findData(m_rollbackPredictionWindow);
    m_predictionWindowCombo->setCurrentIndex(predictionWindowIndex >= 0 ? predictionWindowIndex : 0);
    connect(m_predictionWindowCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (canEditRollbackPredictionSettings() && m_predictionWindowCombo != nullptr && index >= 0)
        {
            setRollbackPredictionWindow(m_predictionWindowCombo->itemData(index).toInt(), m_isHost, true);
        }
    });
    predictionLayout->addWidget(m_predictionWindowCombo);
    predictionLayout->addStretch();
    hostLayout->addLayout(predictionLayout);

    if (m_isHost)
    {
        auto* publicLobbyRow = new QHBoxLayout();
        publicLobbyRow->setContentsMargins(0, 0, 0, 0);
        publicLobbyRow->setSpacing(8);
        auto* publicLobbyLabel = new QLabel("Show on public lobby", m_hostGroup);
        publicLobbyRow->addWidget(publicLobbyLabel);
        publicLobbyRow->addStretch();
        m_enlistCheck = new KailleraSwitch(m_hostGroup);
        m_enlistCheck->setObjectName("KailleraP2PSwitch");
        m_enlistCheck->setAccessibleName("Show on public lobby");
        m_enlistCheck->setCursor(Qt::PointingHandCursor);
        m_enlistCheck->setChecked(m_initialShowOnPublicList);
        publicLobbyRow->addWidget(m_enlistCheck);
        hostLayout->addLayout(publicLobbyRow);
        connect(m_enlistCheck, &QCheckBox::toggled, this, [this](bool checked) {
            if (checked)
                enlistGame();
            else
                unenlistGame();
        });
    }

    auto* localSettingsLabel = new QLabel("Local Settings", m_hostGroup);
    localSettingsLabel->setObjectName("KailleraP2PSectionLabel");
    hostLayout->addWidget(localSettingsLabel);

    m_recordCheck = new QCheckBox("Record game", m_hostGroup);
    const bool recordingEnabledByDefault = CoreGetKailleraEffectiveRecordingDefault();
    extern bool n02_kaillera_recording_enabled;
    n02_kaillera_recording_enabled = recordingEnabledByDefault;
    m_recordCheck->setChecked(recordingEnabledByDefault);
    connect(m_recordCheck, &QCheckBox::toggled, this, [](bool checked) {
        extern bool n02_kaillera_recording_enabled;
        n02_kaillera_recording_enabled = checked;
    });
    hostLayout->addWidget(m_recordCheck);

    const int formLabelWidth = std::max(
        m_frameDelayLabel->sizeHint().width(),
        predictionWindowLabel->sizeHint().width());
    m_frameDelayLabel->setMinimumWidth(formLabelWidth);
    predictionWindowLabel->setMinimumWidth(formLabelWidth);

    const QMargins hostMargins = hostLayout->contentsMargins();
    m_frameDelayLabel->setText(isRollbackMode() ? "Input Delay:" : "Frame Delay:");
    const int labelWidth = m_frameDelayLabel->sizeHint().width();
    const int hostMinWidth =
        hostMargins.left() +
        labelWidth +
        fdlyLayout->spacing() +
        m_frameDelayCombo->minimumWidth() +
        fdlyLayout->spacing() +
        m_customFrameDelayCombo->minimumWidth() +
        hostMargins.right();
    m_hostGroup->setMinimumWidth(hostMinWidth);
    rightLayout->addWidget(m_hostGroup, 0);
    rightLayout->addStretch();

    contentLayout->addWidget(rightWidget, 2);
    contentLayout->addWidget(chatColumn, 3);
    mainLayout->addLayout(contentLayout, 1);

    auto* statusLayout = new QHBoxLayout();
    statusLayout->setContentsMargins(0, 0, 0, 0);
    statusLayout->setSpacing(12);
    m_netcodeModeLabel = new QLabel(this);
    m_netcodeModeLabel->setObjectName("KailleraP2PFooterStatusLabel");
    statusLayout->addWidget(m_netcodeModeLabel, 0, Qt::AlignLeft | Qt::AlignVCenter);
    m_pingLabel = new QLabel("Ping: --", this);
    m_pingLabel->setObjectName("KailleraP2PFooterStatusLabel");
    statusLayout->addWidget(m_pingLabel, 0, Qt::AlignLeft | Qt::AlignVCenter);
    m_delayLabel = new QLabel("Delay: --", this);
    m_delayLabel->setObjectName("KailleraP2PFooterStatusLabel");
    statusLayout->addWidget(m_delayLabel, 0, Qt::AlignLeft | Qt::AlignVCenter);
    statusLayout->addStretch();
    mainLayout->addLayout(statusLayout);

    increaseP2PNormalFontSizes(this, kP2PNormalFontPointDelta);
    disableDefaultButtonBehavior(this);

    applyGameLayerUI();
    updatePeerConnectionUI();
    updateNetcodeModeStatus();
    if (isRollbackMode())
    {
        updateRollbackDelayControls();
    }
    positionCornerButtons();
    QTimer::singleShot(0, this, [this]() {
        positionCornerButtons();
    });

    // Connect button actions
    connect(m_btnChat, &QPushButton::clicked, this, &KailleraP2PDialog::onSendChat);
    connect(m_chatInput, &QLineEdit::returnPressed, this, &KailleraP2PDialog::onSendChat);
    connect(m_btnReady, &QPushButton::clicked, this, &KailleraP2PDialog::onReady);
    connect(m_btnDrop, &QPushButton::clicked, this, &KailleraP2PDialog::onDrop);
    connect(m_btnLeave, &QPushButton::clicked, this, &KailleraP2PDialog::reject);

    m_copyFeedbackTimer = new QTimer(this);
    m_copyFeedbackTimer->setSingleShot(true);
    connect(m_copyFeedbackTimer, &QTimer::timeout, this, [this]() {
        if (m_btnCopyConnectCode == nullptr)
        {
            return;
        }

        m_btnCopyConnectCode->setIcon(themedP2PIcon("copy-line"));
        m_btnCopyConnectCode->setToolTip("Copy connect code");
    });
}

void KailleraP2PDialog::connectSignals()
{
    auto& bridge = KailleraUIBridge::instance();
    constexpr Qt::ConnectionType kUiCallbackConnection = Qt::QueuedConnection;

    connect(&bridge, &KailleraUIBridge::p2pChatReceived, this, &KailleraP2PDialog::onChatReceived,
            kUiCallbackConnection);
    connect(&bridge, &KailleraUIBridge::p2pGameStarted, this, &KailleraP2PDialog::onGameStarted,
            kUiCallbackConnection);
    connect(&bridge, &KailleraUIBridge::p2pGameEnded, this, &KailleraP2PDialog::onGameEnded,
            kUiCallbackConnection);
    connect(&bridge, &KailleraUIBridge::p2pClientDropped, this, &KailleraP2PDialog::onClientDropped,
            kUiCallbackConnection);
    connect(&bridge, &KailleraUIBridge::p2pDebugMessage, this, &KailleraP2PDialog::onDebug,
            kUiCallbackConnection);
    connect(&bridge, &KailleraUIBridge::p2pHostedGame, this, &KailleraP2PDialog::onHostedGame,
            kUiCallbackConnection);
    connect(&bridge, &KailleraUIBridge::p2pPingUpdated, this, &KailleraP2PDialog::onPingUpdated,
            kUiCallbackConnection);
    connect(&bridge, &KailleraUIBridge::p2pPeerJoined, this, &KailleraP2PDialog::onPeerJoined,
            kUiCallbackConnection);
    connect(&bridge, &KailleraUIBridge::p2pPeerLeft, this, &KailleraP2PDialog::onPeerLeft,
            kUiCallbackConnection);
    connect(&bridge, &KailleraUIBridge::p2pPeerInfo, this, &KailleraP2PDialog::onPeerInfo,
            kUiCallbackConnection);
    connect(&bridge, &KailleraUIBridge::p2pFodippResult, this, &KailleraP2PDialog::onFodippResult,
            kUiCallbackConnection);
    connect(&bridge, &KailleraUIBridge::p2pSsrvPacketReceived, this, &KailleraP2PDialog::onSsrvPacketReceived,
            kUiCallbackConnection);
}

void KailleraP2PDialog::reject()
{
    cleanupSessionForClose();
    QDialog::reject();
}

void KailleraP2PDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    positionCornerButtons();
}

void KailleraP2PDialog::cleanupSessionForClose()
{
    if (m_closeCleanupDone)
        return;

    m_closeCleanupDone = true;

    if (m_stepTimer) m_stepTimer->stop();
    if (m_travTimer) m_travTimer->stop();
    m_ready = false;
    m_peerReady = false;
    if (m_btnReady) m_btnReady->setChecked(false);

    if (isRollbackMode() && m_rollbackGameActive)
    {
        m_rollbackGameActive = false;
        m_gameActive = false;
        emit rollbackSessionEnded();
        CoreStopEmulation();
    }

    // Remove from public game list
    if (m_enlistCheck && m_enlistCheck->isChecked())
        unenlistGame();

    if (m_isHost && m_travHostEnabled)
        travSendHostClose();

    p2p_disconnect();
    p2p_core_cleanup();

    travResetState();
}

bool KailleraP2PDialog::isRollbackMode() const
{
    return m_gameLayer == GameLayer::Rollback;
}

bool KailleraP2PDialog::canEditRollbackDelaySettings() const
{
    return m_isHost || !m_hasRemoteRollbackDelaySettings;
}

bool KailleraP2PDialog::canEditRollbackPredictionSettings() const
{
    return m_isHost || !m_hasRemoteRollbackPredictionSettings;
}

void KailleraP2PDialog::loadLocalRollbackSettings()
{
    QSettings settings("RMG-K", "n02");
    m_rollbackDelayMode = normalizeRollbackDelayMode(
        settings.value("Rollback_FrameDelayMode", kRollbackDelayModeDefault).toInt());
    m_customRollbackFrameDelay = clampRollbackFrameDelay(
        settings.value("Rollback_FrameDelay", kDefaultRollbackFrameDelay).toInt());
    m_rollbackFrameDelay = m_customRollbackFrameDelay;
    m_rollbackPredictionWindow = normalizeRollbackPredictionWindow(
        settings.value("Rollback_PredictionWindow", 0).toInt());
}

void KailleraP2PDialog::appendChatHtml(const QString& html, bool debug)
{
    m_chatHistory.append({ html, debug });
    if (m_chatHistory.size() > 1000)
    {
        m_chatHistory.erase(m_chatHistory.begin(), m_chatHistory.begin() + (m_chatHistory.size() - 1000));
    }

    if (m_chat != nullptr && (!debug || m_showDebugMessages))
    {
        m_chat->append(html);
    }
}

void KailleraP2PDialog::appendChatStatus(const QString& message, const QString& color, bool debug)
{
    appendChatHtml("<span style='color:" + color + ";'>" + timestamp() +
        message.toHtmlEscaped() + "</span>", debug);
}

void KailleraP2PDialog::appendChatError(const QString& message)
{
    appendChatStatus(message, "red", false);
}

void KailleraP2PDialog::rebuildChat()
{
    if (m_chat == nullptr)
    {
        return;
    }

    m_chat->clear();
    for (const ChatEntry& entry : m_chatHistory)
    {
        if (!entry.debug || m_showDebugMessages)
        {
            m_chat->append(entry.html);
        }
    }
}

void KailleraP2PDialog::setShowDebugMessages(bool show)
{
    if (m_showDebugMessages == show)
    {
        return;
    }

    m_showDebugMessages = show;
    QSettings settings("RMG-K", "n02");
    settings.setValue("P2P_ShowDebugMessages", m_showDebugMessages);
    rebuildChat();
}

void KailleraP2PDialog::updateLobbyStatusLabel()
{
    if (m_lobbyStatusLabel == nullptr)
    {
        return;
    }

    if (m_gameActive || (isRollbackMode() && m_rollbackGameActive) || n02::isGameRunning())
    {
        m_lobbyStatusLabel->setText("Playing");
        m_lobbyStatusLabel->setStyleSheet("color: #3f45c9; font-weight: 700;");
    }
    else if (m_peerConnected && m_ready && m_peerReady)
    {
        m_lobbyStatusLabel->setText("Ready");
        m_lobbyStatusLabel->setStyleSheet("color: #087a2f; font-weight: 700;");
    }
    else if (m_peerConnected)
    {
        m_lobbyStatusLabel->setText("Waiting for Ready");
        m_lobbyStatusLabel->setStyleSheet("color: #a66a00; font-weight: 700;");
    }
    else if (m_lobbyOpening)
    {
        m_lobbyStatusLabel->setText("Opening Lobby");
        m_lobbyStatusLabel->setStyleSheet("color: #8a8a8a; font-weight: 700;");
    }
    else
    {
        m_lobbyStatusLabel->setText(m_isHost ? "Hosting" : "P2P Game");
        m_lobbyStatusLabel->setStyleSheet("color: #a66a00; font-weight: 700;");
    }
}

void KailleraP2PDialog::positionCornerButtons()
{
    const auto placeButton = [](QGroupBox* group, QPushButton* button, int yOffset = 0) {
        if (group == nullptr || button == nullptr)
        {
            return;
        }

        const int rightMargin = 2;
        const int topMargin = std::max(0, static_cast<int>(button->height() * 0.7) + yOffset);
        const int x = std::max(0, group->width() - button->width() - rightMargin);
        button->move(x, topMargin);
        button->raise();
    };

    placeButton(m_chatGroup, m_chatSettingsButton, -2);
    placeButton(m_hostGroup, m_netcodeSettingsButton, -5);
}

void KailleraP2PDialog::appendPeerJoinedNotice()
{
    if (m_peerJoinNoticeShown)
    {
        return;
    }

    const QString name = m_peerName.trimmed().isEmpty() ? QString("Opponent") : m_peerName.trimmed();
    appendChatStatus(name + " joined the lobby.", "green", false);
    m_peerJoinNoticeShown = true;
    m_peerLeaveNoticeShown = false;
}

void KailleraP2PDialog::appendPeerLeftNotice(const QString& name)
{
    if (m_peerLeaveNoticeShown)
    {
        return;
    }

    const QString displayName = name.trimmed().isEmpty() ? QString("Opponent") : name.trimmed();
    appendChatStatus(displayName + " left the lobby.", "red", false);
    m_peerLeaveNoticeShown = true;
    m_peerJoinNoticeShown = false;
}

int KailleraP2PDialog::automaticRollbackFrameDelay() const
{
    return clampRollbackFrameDelay(m_autoRollbackFrameDelay);
}

void KailleraP2PDialog::resetAutomaticRollbackDelay()
{
    m_autoRollbackFrameDelay = kDefaultRollbackFrameDelay;
    resetAutomaticRollbackDelaySamples();
}

void KailleraP2PDialog::resetAutomaticRollbackDelaySamples()
{
    m_rollbackDelayPingSamples.clear();
    m_rollbackDelayFirstSampleMs = 0;
    m_rollbackDelayLastUpdateMs = 0;
}

void KailleraP2PDialog::recordRollbackDelayPingSample(int ping)
{
    if (ping < 0)
    {
        return;
    }

    const unsigned long now = monotonicTickCount();
    m_rollbackDelayPingSamples.append({now, ping});
    while (!m_rollbackDelayPingSamples.isEmpty() &&
           now - m_rollbackDelayPingSamples.first().timestampMs > kRollbackDelaySampleWindowMs)
    {
        m_rollbackDelayPingSamples.removeFirst();
    }

    m_rollbackDelayFirstSampleMs = m_rollbackDelayPingSamples.isEmpty()
        ? 0
        : m_rollbackDelayPingSamples.first().timestampMs;
}

bool KailleraP2PDialog::maybeUpdateAutomaticRollbackDelay(bool force)
{
    const unsigned long now = monotonicTickCount();
    while (!m_rollbackDelayPingSamples.isEmpty() &&
           now - m_rollbackDelayPingSamples.first().timestampMs > kRollbackDelaySampleWindowMs)
    {
        m_rollbackDelayPingSamples.removeFirst();
    }
    m_rollbackDelayFirstSampleMs = m_rollbackDelayPingSamples.isEmpty()
        ? 0
        : m_rollbackDelayPingSamples.first().timestampMs;

    if (m_rollbackDelayPingSamples.isEmpty())
    {
        return false;
    }

    if (!force)
    {
        if (m_rollbackDelayLastUpdateMs == 0)
        {
            const bool enoughStartupSamples =
                m_rollbackDelayPingSamples.size() >= kRollbackDelayInitialSampleCount;
            const bool waitedStartupWindow =
                m_rollbackDelayFirstSampleMs != 0 &&
                now - m_rollbackDelayFirstSampleMs >= kRollbackDelayInitialUpdateMs;
            if (!enoughStartupSamples && !waitedStartupWindow)
            {
                return false;
            }
        }
        else if (now - m_rollbackDelayLastUpdateMs < kRollbackDelayUpdateIntervalMs)
        {
            return false;
        }
    }

    // Use a high-but-not-max sample so brief ping spikes do not immediately
    // push everyone into a higher input delay bucket.
    QVector<int> samples;
    samples.reserve(m_rollbackDelayPingSamples.size());
    for (const RollbackDelayPingSample& sample : m_rollbackDelayPingSamples)
    {
        samples.append(sample.ping);
    }
    std::sort(samples.begin(), samples.end());
    const int sampleIndex = (samples.size() - 1) * 3 / 4;
    const int estimatedPing = samples.at(sampleIndex);
    const int previousDelay = m_autoRollbackFrameDelay;
    m_autoRollbackFrameDelay = automaticRollbackFrameDelayForPing(estimatedPing);
    m_rollbackDelayLastUpdateMs = now;

    return previousDelay != m_autoRollbackFrameDelay;
}

int KailleraP2PDialog::calculatedRollbackFrameDelay() const
{
    int delay = automaticRollbackFrameDelay();

    if (m_rollbackDelayMode == kRollbackDelayModeLower)
    {
        delay--;
    }
    else if (m_rollbackDelayMode == kRollbackDelayModeHigher)
    {
        delay++;
    }
    else if (m_rollbackDelayMode == kRollbackDelayModeCustom)
    {
        delay = m_customRollbackFrameDelay;
    }

    return clampRollbackFrameDelay(delay);
}

int KailleraP2PDialog::effectiveRollbackFrameDelay() const
{
    if (!m_isHost && m_hasRemoteRollbackDelaySettings)
    {
        return clampRollbackFrameDelay(m_rollbackFrameDelay);
    }

    return calculatedRollbackFrameDelay();
}

int KailleraP2PDialog::effectiveRollbackPredictionWindow() const
{
    const int predictionWindow = normalizeRollbackPredictionWindow(m_rollbackPredictionWindow);
    return predictionWindow == 0 ? kDefaultRollbackPredictionWindow : predictionWindow;
}

bool KailleraP2PDialog::parseRollbackDelayMessage(const QString& message, int& mode, int& delay) const
{
    QString text = message.trimmed().toUpper();
    if (!text.startsWith(kRollbackDelayMessagePrefix))
    {
        return false;
    }

    text = text.mid(QString(kRollbackDelayMessagePrefix).size());
    const int separator = text.indexOf(':');
    bool modeOk = false;
    bool delayOk = false;
    const int parsedMode = (separator >= 0 ? text.left(separator) : text).toInt(&modeOk);
    const int parsedDelay = (separator >= 0) ? text.mid(separator + 1).toInt(&delayOk) : kDefaultRollbackFrameDelay;

    mode = modeOk ? normalizeRollbackDelayMode(parsedMode) : kRollbackDelayModeDefault;
    delay = delayOk ? clampRollbackFrameDelay(parsedDelay) : kDefaultRollbackFrameDelay;
    return true;
}

bool KailleraP2PDialog::parseRollbackPredictionMessage(const QString& message, int& predictionWindow) const
{
    QString text = message.trimmed().toUpper();
    if (!text.startsWith(kRollbackPredictionMessagePrefix))
    {
        return false;
    }

    text = text.mid(QString(kRollbackPredictionMessagePrefix).size());
    bool ok = false;
    const int parsedPredictionWindow = text.toInt(&ok);
    predictionWindow = ok ? normalizeRollbackPredictionWindow(parsedPredictionWindow) : 0;
    return true;
}

void KailleraP2PDialog::sendRollbackDelaySettings(bool force)
{
    if (!m_isHost || !isRollbackMode() || !p2p_is_connected() ||
        m_rollbackGameActive || n02::isGameRunning())
    {
        return;
    }

    const int delay = effectiveRollbackFrameDelay();
    if (!force && m_hasSentRollbackDelaySettings &&
        m_lastSentRollbackDelayMode == m_rollbackDelayMode &&
        m_lastSentRollbackFrameDelay == delay)
    {
        return;
    }

    QByteArray message = QByteArray(kRollbackDelayMessagePrefix) +
        QByteArray::number(m_rollbackDelayMode) + ":" +
        QByteArray::number(delay);
    p2p_send_chat(message.data());

    m_hasSentRollbackDelaySettings = true;
    m_lastSentRollbackDelayMode = m_rollbackDelayMode;
    m_lastSentRollbackFrameDelay = delay;
}

void KailleraP2PDialog::sendRollbackPredictionSettings(bool force)
{
    if (!m_isHost || !isRollbackMode() || !p2p_is_connected() ||
        m_rollbackGameActive || n02::isGameRunning())
    {
        return;
    }

    const int predictionWindow = normalizeRollbackPredictionWindow(m_rollbackPredictionWindow);
    if (!force && m_hasSentRollbackPredictionSettings &&
        m_lastSentRollbackPredictionWindow == predictionWindow)
    {
        return;
    }

    QByteArray message = QByteArray(kRollbackPredictionMessagePrefix) +
        QByteArray::number(predictionWindow);
    p2p_send_chat(message.data());

    m_hasSentRollbackPredictionSettings = true;
    m_lastSentRollbackPredictionWindow = predictionWindow;
}

void KailleraP2PDialog::setRollbackDelayMode(int mode, bool announceToPeer, bool resetReady)
{
    mode = normalizeRollbackDelayMode(mode);
    const int previousDelay = effectiveRollbackFrameDelay();
    const bool changed = (m_rollbackDelayMode != mode);
    m_rollbackDelayMode = mode;

    if (changed && mode == kRollbackDelayModeCustom)
    {
        m_customRollbackFrameDelay = previousDelay;
    }

    if (canEditRollbackDelaySettings())
    {
        QSettings settings("RMG-K", "n02");
        settings.setValue("Rollback_FrameDelayMode", m_rollbackDelayMode);
        if (mode == kRollbackDelayModeCustom)
        {
            settings.setValue("Rollback_FrameDelay", m_customRollbackFrameDelay);
        }
    }

    updateRollbackDelayControls();

    if (changed && resetReady)
    {
        resetReadyState();
    }
    if (announceToPeer)
    {
        sendRollbackDelaySettings(true);
    }
}

void KailleraP2PDialog::setRollbackCustomFrameDelay(int delay, bool announceToPeer, bool resetReady)
{
    delay = clampRollbackFrameDelay(delay);
    const bool changed = (m_customRollbackFrameDelay != delay);
    m_customRollbackFrameDelay = delay;

    if (canEditRollbackDelaySettings())
    {
        QSettings settings("RMG-K", "n02");
        settings.setValue("Rollback_FrameDelay", m_customRollbackFrameDelay);
    }

    updateRollbackDelayControls();

    if (changed && resetReady)
    {
        resetReadyState();
    }
    if (changed && announceToPeer)
    {
        sendRollbackDelaySettings(true);
    }
}

void KailleraP2PDialog::setRollbackPredictionWindow(int predictionWindow, bool announceToPeer, bool resetReady)
{
    predictionWindow = normalizeRollbackPredictionWindow(predictionWindow);
    const bool changed = (m_rollbackPredictionWindow != predictionWindow);
    m_rollbackPredictionWindow = predictionWindow;

    if (canEditRollbackPredictionSettings())
    {
        QSettings settings("RMG-K", "n02");
        settings.setValue("Rollback_PredictionWindow", m_rollbackPredictionWindow);
    }

    updateRollbackPredictionControls();

    if (changed && resetReady)
    {
        resetReadyState();
    }
    if (changed && announceToPeer)
    {
        sendRollbackPredictionSettings(true);
    }
}

void KailleraP2PDialog::updateRollbackPredictionControls()
{
    if (m_predictionWindowCombo == nullptr)
    {
        return;
    }

    const bool inGame = (isRollbackMode() && m_rollbackGameActive) || n02::isGameRunning();
    const bool blocked = m_predictionWindowCombo->blockSignals(true);
    const int index = m_predictionWindowCombo->findData(normalizeRollbackPredictionWindow(m_rollbackPredictionWindow));
    if (index >= 0)
    {
        m_predictionWindowCombo->setCurrentIndex(index);
    }
    m_predictionWindowCombo->setEnabled(isRollbackMode() && canEditRollbackPredictionSettings() && !inGame);
    m_predictionWindowCombo->blockSignals(blocked);
}

void KailleraP2PDialog::updateRollbackDelayControls()
{
    if (!isRollbackMode())
    {
        return;
    }

    const bool inGame = m_rollbackGameActive;
    const int delay = effectiveRollbackFrameDelay();
    const bool canEditDelay = canEditRollbackDelaySettings();
    const bool editableDelayInput = canEditDelay && m_rollbackDelayMode == kRollbackDelayModeCustom;
    m_rollbackFrameDelay = delay;

    if (m_frameDelayCombo != nullptr)
    {
        const bool blocked = m_frameDelayCombo->blockSignals(true);
        const int index = m_frameDelayCombo->findData(m_rollbackDelayMode);
        if (index >= 0)
        {
            m_frameDelayCombo->setCurrentIndex(index);
        }
        m_frameDelayCombo->setEnabled(canEditDelay && !inGame);
        m_frameDelayCombo->blockSignals(blocked);
    }

    if (m_customFrameDelayCombo != nullptr)
    {
        const bool blocked = m_customFrameDelayCombo->blockSignals(true);
        const int index = m_customFrameDelayCombo->findData(delay);
        if (index >= 0)
        {
            m_customFrameDelayCombo->setCurrentIndex(index);
        }
        m_customFrameDelayCombo->setVisible(m_rollbackDelayMode == kRollbackDelayModeCustom);
        m_customFrameDelayCombo->setEnabled(editableDelayInput && !inGame);
        m_customFrameDelayCombo->blockSignals(blocked);
    }

    if (m_frameDelayHelpLabel != nullptr)
    {
        m_frameDelayHelpLabel->setVisible(true);
    }

    if (m_delayLabel != nullptr)
    {
        m_delayLabel->setText(QString("Delay: %1f").arg(delay));
    }
}

void KailleraP2PDialog::updateNetcodeModeStatus()
{
    if (m_netcodeModeLabel == nullptr)
    {
        return;
    }

    if (isRollbackMode())
    {
        m_netcodeModeLabel->setText("Rollback");
        m_netcodeModeLabel->setStyleSheet(QString());
    }
    else
    {
        m_netcodeModeLabel->setText("Delay-only");
        m_netcodeModeLabel->setStyleSheet("color: #c03a3a; font-weight: 700;");
    }
}

void KailleraP2PDialog::setPingLabelText(const QString& text)
{
    if (m_pingLabel == nullptr)
    {
        return;
    }

    m_pingLabel->setStyleSheet(QString());
    m_pingLabel->setText(text);
}

void KailleraP2PDialog::setPingLabelFromValue(int ping)
{
    if (m_pingLabel == nullptr)
    {
        return;
    }

    if (ping < 0)
    {
        setPingLabelText("Ping: --");
        return;
    }

    QString color = "#c03a3a";
    if (ping <= 64)
    {
        color = "#087a2f";
    }
    else if (ping <= 128)
    {
        color = "#a66a00";
    }

    m_pingLabel->setStyleSheet(QString());
    m_pingLabel->setText(QString("Ping: <span style='color:%1; font-weight:700;'>%2ms</span>")
        .arg(color, QString::number(ping)));
}

void KailleraP2PDialog::updatePeerConnectionUI()
{
    const QString localName = m_username.trimmed().isEmpty() ? "You" : m_username.trimmed();
    const QString remoteName = m_peerName.trimmed().isEmpty() ? "Opponent" : m_peerName.trimmed();
    const QString hostName = m_isHost ? localName : (m_peerName.trimmed().isEmpty() ? "Host" : m_peerName.trimmed());
    const QString playerName = m_isHost ? remoteName : localName;

    QString hostCode;
    if (!m_travCode.isEmpty())
    {
        hostCode = m_travCode;
    }
    else if (!m_travJoinCode.isEmpty())
    {
        hostCode = m_travJoinCode;
    }
    else if (!m_travHostIpPort.isEmpty())
    {
        hostCode = m_travHostIpPort;
    }
    else if (!m_travJoinFallbackIpPort.isEmpty())
    {
        hostCode = m_travJoinFallbackIpPort;
    }

    if (m_hostPlayerCard != nullptr)
    {
        m_hostPlayerCard->setVisible(true);
    }
    if (m_hostPlayerNameLabel != nullptr)
    {
        m_hostPlayerNameLabel->setText(hostName + " (Host)");
    }
    updateReadyBadge(m_hostReadyLabel, m_isHost ? m_ready : m_peerReady);
    if (m_hostConnectCodeLabel != nullptr)
    {
        QString hostCodeText = "--";
        if (!hostCode.isEmpty())
        {
            hostCodeText = hostCode;
        }
        else if (m_isHost && m_travHostIpPending)
        {
            hostCodeText = "checking IP";
        }
        else if (m_isHost)
        {
            hostCodeText = "waiting";
        }
        m_hostConnectCodeLabel->setText(hostCodeText);
    }
    if (m_playerCard != nullptr)
    {
        m_playerCard->setVisible(m_peerConnected);
    }
    if (m_playersEmptyLabel != nullptr)
    {
        m_playersEmptyLabel->setVisible(!m_peerConnected);
        if (!m_peerName.trimmed().isEmpty())
        {
            m_playersEmptyLabel->setText("Opponent left");
            m_playersEmptyLabel->setStyleSheet("color: #c03a3a; font-weight: 700;");
        }
        else
        {
            m_playersEmptyLabel->setText(m_isHost ? "Waiting for opponent" : "Opponent not connected");
            m_playersEmptyLabel->setStyleSheet("color: #a66a00; font-weight: 700;");
        }
    }
    if (m_playerNameLabel != nullptr)
    {
        m_playerNameLabel->setText(m_isHost ? playerName : playerName + " (You)");
    }
    updateReadyBadge(m_playerReadyLabel, m_isHost ? m_peerReady : m_ready);
    if (m_btnKickPeer != nullptr)
    {
        const bool inGame = (isRollbackMode() && m_rollbackGameActive) || n02::isGameRunning();
        const bool showKick = m_isHost && m_peerConnected && !inGame;
        m_btnKickPeer->setVisible(showKick);
        m_btnKickPeer->setEnabled(showKick);
    }
    updateLobbyStatusLabel();
}

void KailleraP2PDialog::resetReadyState()
{
    if (!m_ready && (m_btnReady == nullptr || !m_btnReady->isChecked()))
    {
        return;
    }

    m_ready = false;
    if (m_btnReady != nullptr)
    {
        m_btnReady->setChecked(false);
    }
    p2p_set_ready(false);
    updatePeerConnectionUI();
}

bool KailleraP2PDialog::parseGameLayerMessage(const QString& message, GameLayer& layer) const
{
    QString text = message.trimmed().toUpper();
    if (!text.startsWith(kGameLayerMessagePrefix))
    {
        return false;
    }

    text = text.mid(QString(kGameLayerMessagePrefix).size());
    if (text == kGameLayerRollback)
    {
        layer = GameLayer::Rollback;
        return true;
    }
    if (text == kGameLayerStandard)
    {
        layer = GameLayer::Standard;
        return true;
    }

    return false;
}

void KailleraP2PDialog::sendGameLayer()
{
    if (!m_isHost)
    {
        return;
    }

    QByteArray message = QByteArray(kGameLayerMessagePrefix) +
        (isRollbackMode() ? kGameLayerRollback : kGameLayerStandard);
    p2p_send_chat(message.data());
}

void KailleraP2PDialog::setGameLayer(GameLayer layer, bool announceToPeer, bool resetReady)
{
    const bool changed = (m_gameLayer != layer);
    m_gameLayer = layer;

    if (changed)
    {
        applyGameLayerUI();
        if (resetReady)
        {
            resetReadyState();
        }
    }

    if (m_isHost)
    {
        QSettings settings("RMG-K", "n02");
        settings.setValue("P2P_GameLayer", isRollbackMode() ? kGameLayerRollback : kGameLayerStandard);
    }

    if (announceToPeer)
    {
        sendGameLayer();
        sendRollbackDelaySettings(true);
        sendRollbackPredictionSettings(true);
    }
}

void KailleraP2PDialog::applyGameLayerUI()
{
    const bool rollback = isRollbackMode();

    if (m_standardLayerButton != nullptr)
    {
        const bool blocked = m_standardLayerButton->blockSignals(true);
        m_standardLayerButton->setChecked(!rollback);
        m_standardLayerButton->blockSignals(blocked);
    }
    if (m_rollbackLayerButton != nullptr)
    {
        const bool blocked = m_rollbackLayerButton->blockSignals(true);
        m_rollbackLayerButton->setChecked(rollback);
        m_rollbackLayerButton->blockSignals(blocked);
    }

    if (m_frameDelayCombo != nullptr)
    {
        const bool blocked = m_frameDelayCombo->blockSignals(true);
        m_frameDelayCombo->clear();
        if (rollback)
        {
            m_frameDelayCombo->setMinimumWidth(126);
            m_frameDelayCombo->setMaximumWidth(150);
            m_frameDelayCombo->addItem("Default", kRollbackDelayModeDefault);
            m_frameDelayCombo->addItem("Lower delay", kRollbackDelayModeLower);
            m_frameDelayCombo->addItem("Higher delay", kRollbackDelayModeHigher);
            m_frameDelayCombo->addItem("Custom", kRollbackDelayModeCustom);
            const int modeIndex = m_frameDelayCombo->findData(m_rollbackDelayMode);
            m_frameDelayCombo->setCurrentIndex(modeIndex >= 0 ? modeIndex : 0);
        }
        else
        {
            m_frameDelayCombo->setMinimumWidth(175);
            m_frameDelayCombo->setMaximumWidth(220);
            m_frameDelayCombo->addItem("Auto");
            m_frameDelayCombo->addItem("1 frame (0-33ms)");
            m_frameDelayCombo->addItem("2 frames (34-67ms)");
            m_frameDelayCombo->addItem("3 frames (68-99ms)");
            m_frameDelayCombo->addItem("4 frames (100-133ms)");
            m_frameDelayCombo->addItem("5 frames (134-167ms)");
            m_frameDelayCombo->addItem("6 frames (168-199ms)");
            m_frameDelayCombo->addItem("7 frames (200-233ms)");
            m_frameDelayCombo->addItem("8 frames (234-267ms)");
            m_frameDelayCombo->addItem("9 frames (268+ms)");

            if (m_standardFrameDelay < 0 || m_standardFrameDelay > 9)
            {
                m_standardFrameDelay = 0;
            }
            m_frameDelayCombo->setCurrentIndex(m_standardFrameDelay);
            KailleraUIBridge::instance().setSelectedDelay(m_standardFrameDelay);
        }
        m_frameDelayCombo->blockSignals(blocked);
    }

    if (m_frameDelayLabel != nullptr)
    {
        m_frameDelayLabel->setText(rollback ? "Input Delay:" : "Frame Delay:");
    }
    if (!rollback && m_delayLabel != nullptr)
    {
        m_delayLabel->setText("Delay: --");
    }
    if (m_frameDelayHelpLabel != nullptr)
    {
        m_frameDelayHelpLabel->setVisible(rollback);
    }

    // Frame delay is baked into the GekkoNet (or Kaillera) session at start —
    // changing it mid-game has no effect until the next session, so lock the
    // row down for the duration of an active game.
    const bool inGame = (rollback && m_rollbackGameActive) || n02::isGameRunning();

    if (m_frameDelayRow != nullptr)
    {
        m_frameDelayRow->setVisible(true);
        m_frameDelayRow->setEnabled(true);
    }
    if (!rollback && m_frameDelayCombo != nullptr)
    {
        m_frameDelayCombo->setEnabled(m_isHost && !inGame);
    }
    if (rollback)
    {
        updateRollbackDelayControls();
    }
    else if (m_customFrameDelayCombo != nullptr)
    {
        m_customFrameDelayCombo->setVisible(false);
    }
    if (m_netcodeSettingsButton != nullptr)
    {
        m_netcodeSettingsButton->setVisible(m_isHost);
        m_netcodeSettingsButton->setEnabled(m_isHost && !inGame);
    }
    if (m_standardLayerButton != nullptr)
    {
        m_standardLayerButton->setEnabled(m_isHost && !inGame);
    }
    if (m_rollbackLayerButton != nullptr)
    {
        m_rollbackLayerButton->setEnabled(m_isHost && !inGame);
    }
    updateRollbackPredictionControls();
    updateNetcodeModeStatus();
    updatePeerConnectionUI();
}

// ---- NAT traversal helpers ----

void KailleraP2PDialog::travResetState()
{
    m_travHostEnabled = false;
    m_travJoinEnabled = false;
    m_travLiveToken.clear();
    m_travRegAttempts = 0;
    m_travHostSessionSuspended = false;
    m_travHostFallbackActive = false;
    m_travHostIpPending = false;
    m_travHostIpPort.clear();
    m_travNextRegMs = 0;
    m_travNextKeepMs = 0;
    m_travNextJoinMs = 0;
    m_travJoinDeadlineMs = 0;
    m_travJoinCode.clear();
    m_travJoinGotHost = false;
    m_travJoinToken.clear();
    m_travJoinHostIp.clear();
    m_travJoinHostPort = 0;
    m_travNextConnectMs = 0;
    m_travJoinFallbackIpPort.clear();
    m_travJoinFallbackTried = false;
    m_travJoinBusy = false;
    m_travJoinPunchAttempts = 0;
    m_travHostPeerIp.clear();
    m_travHostPeerPort = 0;
    m_travHostPeerDeadlineMs = 0;
    m_travNextHostPunchMs = 0;
    m_ssrvCopyMyIpPending = false;
}

void KailleraP2PDialog::travLoadIdentity()
{
    const QString storedCode =
        normalizeTraversalCode(QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Kaillera_P2PStaticCode)));
    const QString storedToken =
        QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Kaillera_P2PStaticCodeOwnerToken)).trimmed();

    if (!storedCode.isEmpty() && !storedToken.isEmpty())
    {
        m_travCode = storedCode;
        m_travToken = storedToken;
    }
    else
    {
        m_travCode.clear();
        m_travToken.clear();
    }
}

void KailleraP2PDialog::travSaveIdentity() const
{
    CoreSettingsSetValue(SettingsID::Kaillera_P2PStaticCode, m_travCode.toStdString());
    CoreSettingsSetValue(SettingsID::Kaillera_P2PStaticCodeOwnerToken, m_travToken.toStdString());
    CoreSettingsSave();
}

void KailleraP2PDialog::travClearIdentity()
{
    m_travCode.clear();
    m_travToken.clear();
    m_travLiveToken.clear();

    CoreSettingsSetValue(SettingsID::Kaillera_P2PStaticCode, std::string(""));
    CoreSettingsSetValue(SettingsID::Kaillera_P2PStaticCodeOwnerToken, std::string(""));
    CoreSettingsSave();
}

void KailleraP2PDialog::travSendToServer(const QByteArray& msg)
{
    if (msg.isEmpty()) return;
    // NAT traversal messages must NOT include the trailing NUL byte.
    p2p_send_ssrv_packet(const_cast<char*>(msg.constData()), msg.size(),
                         const_cast<char*>(kN02TraversalHost), kN02TraversalPort);
}

void KailleraP2PDialog::travSendClaimAuto()
{
    QByteArray msg = QByteArray(kN02TraversalProtocol) + "|CLAIM|AUTO";
    travSendToServer(msg);
    if (m_travHostEnabled)
        m_travRegAttempts++;
}

void KailleraP2PDialog::travSendClaimAck()
{
    if (m_travToken.isEmpty()) return;
    QByteArray msg = QByteArray(kN02TraversalProtocol) + "|CLAIMACK|" + m_travToken.toUtf8();
    travSendToServer(msg);
}

void KailleraP2PDialog::travSendHostOpen()
{
    if (m_travToken.isEmpty()) return;
    QByteArray msg = QByteArray(kN02TraversalProtocol) + "|HOSTOPEN|" + m_travToken.toUtf8();
    travSendToServer(msg);
}

void KailleraP2PDialog::travSendHostKeep()
{
    if (m_travToken.isEmpty()) return;
    QByteArray msg = QByteArray(kN02TraversalProtocol) + "|HOSTKEEP|" + m_travToken.toUtf8();
    travSendToServer(msg);
}

void KailleraP2PDialog::travSendHostClose()
{
    if (m_travToken.isEmpty()) return;
    QByteArray msg = QByteArray(kN02TraversalProtocol) + "|HOSTCLOSE|" + m_travToken.toUtf8();
    travSendToServer(msg);
}

void KailleraP2PDialog::travSendJoin()
{
    if (m_travJoinCode.isEmpty()) return;
    QByteArray msg = QByteArray(kN02TraversalProtocol) + "|JOIN|" +
                     m_travJoinCode.toUtf8() + "|" +
                     QByteArray::number((quint32)monotonicTickCount());
    travSendToServer(msg);
}

void KailleraP2PDialog::travPunchEndpoint(const QString& hostIp, int hostPort, const QString& token)
{
    if (hostIp.isEmpty() || hostPort <= 0) return;

    QByteArray msg = QByteArray(kN02TraversalProtocol) + "|PUNCH|" + token.toUtf8();
    QByteArray ipBytes = hostIp.toUtf8();

    // Send 10 punch packets to open NAT mappings
    for (int i = 0; i < 10; i++)
    {
        p2p_send_ssrv_packet(const_cast<char*>(msg.constData()), msg.size(),
                             const_cast<char*>(ipBytes.constData()), hostPort);
    }
}

bool KailleraP2PDialog::travTryFallbackConnect(const QString& reason)
{
    if (m_travJoinFallbackTried) return false;

    QString fallback;
    if (!m_travJoinFallbackIpPort.isEmpty())
        fallback = m_travJoinFallbackIpPort;
    else if (!m_travJoinHostIp.isEmpty())
        fallback = m_travJoinHostIp;
    if (fallback.isEmpty()) return false;

    m_travJoinFallbackTried = true;

    QString ip;
    int port = 0;
    QByteArray fallbackBytes = fallback.toUtf8();
    if (!tryExtractIPv4AndPort(fallbackBytes, ip, port))
        return false;
    if (port <= 0) port = 27886;

    appendChatStatus("NAT traversal: " + reason + ". Falling back to direct connect", "green", true);

    QByteArray ipBytes = ip.toUtf8();
    if (!p2p_core_connect(ipBytes.data(), port))
    {
        appendChatError("NAT traversal: fallback connect failed");
        return false;
    }
    return true;
}

void KailleraP2PDialog::updateHostCodeUI()
{
    if (!m_isHost) return;

    if (m_hostGroup) m_hostGroup->setVisible(true);
    const bool codeActive = !m_travHostSessionSuspended;
    if (m_enlistCheck) m_enlistCheck->setEnabled(codeActive);

    const bool canCopyCode = !m_travCode.isEmpty() || !m_travHostIpPort.isEmpty();
    if (m_btnCopyConnectCode != nullptr)
    {
        m_btnCopyConnectCode->setEnabled(codeActive && canCopyCode);
    }

    updatePeerConnectionUI();
}

void KailleraP2PDialog::ssrvSend(const QByteArray& cmd)
{
    // Send to super-server (SSRV). Includes trailing NUL byte per ssrv protocol.
    int sendLen = cmd.size() + 1;
    p2p_send_ssrv_packet(const_cast<char*>(cmd.constData()), sendLen,
                         const_cast<char*>(kSsrvHost), kSsrvPort);
}

void KailleraP2PDialog::ssrvWhatIsMyIp()
{
    m_ssrvCopyMyIpPending = true;
    ssrvSend("WHATISMYIP");
}

QString KailleraP2PDialog::buildEnlistAppName()
{
    extern char APP[128];
    QString app = QString::fromUtf8(APP);

    // Append traversal code if hosting by code
    if (m_isHost && m_travHostEnabled && !m_travCode.isEmpty())
    {
        app += " {CC:" + m_travCode + "}";
    }
    return app;
}

void KailleraP2PDialog::enlistGame()
{
    QString app = buildEnlistAppName();
    int port = p2p_core_get_port();

    // Use ENLISP (with port) when hosting by code or on non-default port
    if (m_travHostEnabled || port != 27886)
    {
        QByteArray msg = QString("ENLISP %1|%2|%3|%4")
            .arg(m_gameName, app, m_username)
            .arg(port).toUtf8();
        ssrvSend(msg);
    }
    else
    {
        QByteArray msg = QString("ENLIST %1|%2|%3")
            .arg(m_gameName, app, m_username).toUtf8();
        ssrvSend(msg);
    }
}

void KailleraP2PDialog::unenlistGame()
{
    ssrvSend("UNENLIST");
}

// ---- SSRV packet handler (NAT traversal + super-server responses) ----

void KailleraP2PDialog::onSsrvPacketReceived(QByteArray cmd, QByteArray saddr)
{
    if (cmd.isEmpty()) return;

    // Null-terminate for safe string operations
    QByteArray cmdBuf = cmd;
    if (cmdBuf.size() > 0 && cmdBuf[cmdBuf.size() - 1] != '\0')
        cmdBuf.append('\0');

    const char* cmdStr = cmdBuf.constData();

    // ---- NAT traversal ----
    const QByteArray traversalPrefix = QByteArray(kN02TraversalProtocol) + "|";
    if (strncmp(cmdStr, traversalPrefix.constData(), traversalPrefix.size()) == 0)
    {
        // Split the message by '|'
        QByteArray parseBuf = cmdBuf;
        char* parts[8] = { nullptr };
        int partCount = 0;
        parts[partCount++] = parseBuf.data();
        for (char* p = parseBuf.data(); *p && partCount < 8; p++)
        {
            if (*p == '|')
            {
                *p = 0;
                parts[partCount++] = p + 1;
            }
        }

        if (partCount < 2) return;
        const char* type = parts[1];

        if (strcmp(type, "CLAIMOK") == 0 && partCount >= 4)
        {
            if (!m_travHostEnabled) return;

            m_travCode = normalizeTraversalCode(QString::fromUtf8(parts[2]));
            m_travToken = QString::fromUtf8(parts[3]);
            m_travLiveToken.clear();
            m_travRegAttempts = 0;
            m_travHostIpPending = false;
            m_travHostIpPort.clear();
            travSaveIdentity();

            appendChatStatus("Claimed connect code: " + m_travCode, "green", true);

            // Auto-copy to clipboard
            QApplication::clipboard()->setText(m_travCode);

            updateHostCodeUI();
            travSendClaimAck();
            travSendHostOpen();

            return;
        }

        if (strcmp(type, "CLAIMSUGGEST") == 0 && partCount >= 4)
        {
            appendChatError("Requested code " + QString::fromUtf8(parts[2]) +
                " is unavailable. Suggested: " + QString::fromUtf8(parts[3]));
            return;
        }

        if (strcmp(type, "HOSTOK") == 0 && partCount >= 6)
        {
            if (!m_travHostEnabled) return;

            m_travCode = normalizeTraversalCode(QString::fromUtf8(parts[2]));
            m_travLiveToken = QString::fromUtf8(parts[3]);
            m_travRegAttempts = 0;
            m_travHostIpPending = false;
            m_travHostIpPort.clear();
            m_travNextRegMs = 0;
            m_travNextKeepMs = 0;
            travSaveIdentity();
            updateHostCodeUI();
            m_lobbyOpening = false;
            updateLobbyStatusLabel();

            appendChatStatus("Host session opened for " + m_travCode, "green", true);

            if (m_enlistCheck && m_enlistCheck->isChecked())
                enlistGame();

            return;
        }

        // HOST: Joiner received the host's endpoint from the traversal server.
        if (strcmp(type, "HOST") == 0 && partCount >= 5)
        {
            if (!m_travJoinEnabled) return;

            QString token = QString::fromUtf8(parts[2]);
            QString hostIp = QString::fromUtf8(parts[3]);
            int hostPort = atoi(parts[4]);

            appendChatStatus("NAT traversal: got host endpoint", "green", true);

            m_travJoinToken = token;
            m_travJoinHostIp = hostIp;
            m_travJoinHostPort = hostPort;
            m_travJoinGotHost = true;

            if (m_travJoinFallbackIpPort.isEmpty())
            {
                m_travJoinFallbackIpPort = hostIp;
                m_travJoinFallbackTried = false;
            }

            // Stop asking the server
            m_travNextJoinMs = 0;

            // Try connecting immediately
            m_travJoinPunchAttempts++;
            travPunchEndpoint(m_travJoinHostIp, m_travJoinHostPort, m_travJoinToken);

            QByteArray ipBytes = m_travJoinHostIp.toUtf8();
            if (!p2p_core_connect(ipBytes.data(), m_travJoinHostPort))
            {
                travTryFallbackConnect("connect failed");
            }
            if (m_travJoinPunchAttempts >= 3)
            {
                travTryFallbackConnect("trying direct IP/port");
            }
            return;
        }

        // PEER: Host received a peer's endpoint — need to punch their NAT.
        if (strcmp(type, "PEER") == 0 && partCount >= 5)
        {
            if (!m_travHostEnabled) return;

            QString token = QString::fromUtf8(parts[2]);
            QString peerIp = QString::fromUtf8(parts[3]);
            int peerPort = atoi(parts[4]);

            if (!m_travLiveToken.isEmpty() && token != m_travLiveToken) return;

            appendChatStatus("NAT traversal: got peer endpoint", "green", true);

            m_travHostPeerIp = peerIp;
            m_travHostPeerPort = peerPort;
            m_travHostPeerDeadlineMs = QDateTime::currentMSecsSinceEpoch() + 15000;
            m_travNextHostPunchMs = 0;

            travPunchEndpoint(peerIp, peerPort, token);
            return;
        }

        // ERR: Server error
        if (strcmp(type, "ERR") == 0 && partCount >= 3)
        {
            const QString reason = QString::fromUtf8(parts[2]);

            if (m_travJoinEnabled && strcmp(parts[2], "BUSY") == 0)
            {
                m_travJoinBusy = true;
                m_travJoinDeadlineMs = QDateTime::currentMSecsSinceEpoch();
                m_travNextJoinMs = 0;
                return;
            }

            if (m_travJoinEnabled && (strcmp(parts[2], "OFFLINE") == 0 || strcmp(parts[2], "UNKNOWNCODE") == 0))
            {
                m_travJoinEnabled = false;
                m_travJoinDeadlineMs = 0;
                m_travNextJoinMs = 0;
                m_lobbyOpening = false;
                updateLobbyStatusLabel();
            }

            if (m_travHostEnabled && strcmp(parts[2], "NOAUTH") == 0)
            {
                appendChatStatus("Saved connect code identity was rejected. Claiming a new code.", "green", true);
                travClearIdentity();
                m_travRegAttempts = 0;
                m_travNextRegMs = 0;
                m_travNextKeepMs = 0;
                m_travHostIpPending = false;
                m_lobbyOpening = true;
                updateHostCodeUI();
                return;
            }

            if (m_travHostEnabled && strcmp(parts[2], "NOSESSION") == 0)
            {
                m_travLiveToken.clear();
                m_travNextRegMs = 0;
                m_travNextKeepMs = 0;
                m_lobbyOpening = true;
                updateLobbyStatusLabel();
            }

            appendChatError("NAT traversal error: " + reason);
            return;
        }

        // OK: Acknowledgement
        if (strcmp(type, "OK") == 0) return;

        // PUNCH: No-op payload for NAT hole punching
        if (strcmp(type, "PUNCH") == 0) return;

        return;
    }

    // ---- PINGRQ: Super-server ping request ----
    if (strncmp(cmdStr, "PINGRQ", 6) == 0)
    {
        if (!saddr.isEmpty())
        {
            p2p_send_ssrv_packet(const_cast<char*>("xxxxxxxxxx"), 11, saddr.data());
        }
        return;
    }

    // ---- MSG: Ignore ----
    if (strncmp(cmdStr, "MSG", 3) == 0) return;

    // ---- WHATISMYIP response handling ----
    if (m_ssrvCopyMyIpPending)
    {
        QString ip;
        int port = 0;
        if (tryExtractIPv4AndPort(cmd, ip, port))
        {
            if (port <= 0) port = p2p_core_get_port();
            if (port > 0)
            {
                m_travHostIpPort = ip + ":" + QString::number(port);
                m_travHostIpPending = false;
                updateHostCodeUI();

                if (m_travHostFallbackActive)
                {
                    appendChatStatus("Your IP address is: " + m_travHostIpPort, "green", true);
                }
            }
        }
        m_ssrvCopyMyIpPending = false;
    }
}

// ---- Signal handlers ----

void KailleraP2PDialog::onChatReceived(QString nick, QString message)
{
    if (message.startsWith("Using version:"))
    {
        return;
    }
    if (message.contains("Emulator/version difference alert", Qt::CaseInsensitive))
    {
        appendChatError("Emulator/version difference alert! Game may desync!");
        return;
    }

    GameLayer layer = GameLayer::Standard;
    if (parseGameLayerMessage(message, layer))
    {
        if (!m_isHost)
        {
            setGameLayer(layer, false, true);
        }
        return;
    }

    int rollbackDelayMode = kRollbackDelayModeDefault;
    int rollbackFrameDelay = kDefaultRollbackFrameDelay;
    if (parseRollbackDelayMessage(message, rollbackDelayMode, rollbackFrameDelay))
    {
        if (!m_isHost)
        {
            m_rollbackDelayMode = rollbackDelayMode;
            m_rollbackFrameDelay = rollbackFrameDelay;
            m_hasRemoteRollbackDelaySettings = true;
            updateRollbackDelayControls();
        }
        return;
    }

    int rollbackPredictionWindow = 0;
    if (parseRollbackPredictionMessage(message, rollbackPredictionWindow))
    {
        if (!m_isHost)
        {
            m_rollbackPredictionWindow = rollbackPredictionWindow;
            m_hasRemoteRollbackPredictionSettings = true;
            updateRollbackPredictionControls();
        }
        return;
    }

    appendChatHtml("<b>" + nick.toHtmlEscaped() + ":</b> " + message.toHtmlEscaped(), false);
}

void KailleraP2PDialog::onGameStarted(QString game, int player, int maxPlayers)
{
    if (isRollbackMode())
    {
        if (m_rollbackGameActive)
        {
            return;
        }

        char peerIp[128] = {};
        int peerP2PPort = 0;
        const int localP2PPort = p2p_core_get_port();
        const int frameDelay = effectiveRollbackFrameDelay();
        const int predictionWindow = effectiveRollbackPredictionWindow();
        if (!p2p_core_get_peer_endpoint(peerIp, sizeof(peerIp), &peerP2PPort))
        {
            appendChatError("Could not get rollback peer endpoint.");
            return;
        }
        if (localP2PPort <= 0 || peerP2PPort <= 0)
        {
            appendChatError("Could not get rollback game ports.");
            return;
        }

        m_rollbackGameActive = true;
        resetAutomaticRollbackDelaySamples();
        m_gameActive = true;
        applyGameLayerUI();
        appendChatStatus("Rollback game started: " + game, "green", true);
        emit rollbackSessionReady(game, QString::fromUtf8(peerIp), localP2PPort, peerP2PPort, player, frameDelay, predictionWindow);
        return;
    }

    (void)player;
    (void)maxPlayers;
    m_gameActive = true;
    appendChatStatus("Game started: " + game, "green", true);
    applyGameLayerUI();
}

void KailleraP2PDialog::onGameEnded()
{
    if (isRollbackMode())
    {
        const bool wasActive = m_rollbackGameActive;
        const bool wasReady = m_ready || (m_btnReady != nullptr && m_btnReady->isChecked());
        m_rollbackGameActive = false;
        resetAutomaticRollbackDelaySamples();
        m_gameActive = false;
        if (wasActive)
        {
            emit rollbackSessionEnded();
        }
        m_ready = false;
        m_peerReady = false;
        if (m_btnReady) m_btnReady->setChecked(false);
        applyGameLayerUI();
        updatePeerConnectionUI();
        if (wasActive || wasReady)
        {
            appendChatStatus("Game ended.",
                QApplication::palette().window().color().value() < 128 ? "cornflowerblue" : "darkblue", true);
        }
        if (wasActive)
        {
            QTimer::singleShot(0, this, []() {
                CoreStopEmulation();
            });
        }
        return;
    }

    CoreMarkKailleraGameInactive();
    CoreStopEmulation();
    m_gameActive = false;
    m_ready = false;
    m_peerReady = false;
    if (m_btnReady) m_btnReady->setChecked(false);
    applyGameLayerUI();
    updatePeerConnectionUI();
    appendChatStatus("Game ended.",
        QApplication::palette().window().color().value() < 128 ? "cornflowerblue" : "darkblue", true);
}

void KailleraP2PDialog::onClientDropped(QString nick, int player)
{
    (void)player;
    setPingLabelFromValue(-1);
    m_lastPing = -1;
    resetAutomaticRollbackDelay();
    m_gameActive = false;
    m_peerReady = false;
    updatePeerConnectionUI();
    appendChatStatus(nick + " dropped.", "red", false);
}

void KailleraP2PDialog::onDebug(QString message)
{
    const QString text = message.trimmed();
    if (text == "You are marked as ready")
    {
        m_ready = true;
        if (m_btnReady != nullptr)
        {
            const bool blocked = m_btnReady->blockSignals(true);
            m_btnReady->setChecked(true);
            m_btnReady->blockSignals(blocked);
        }
        updatePeerConnectionUI();
        return;
    }
    if (text == "You are marked as not ready")
    {
        m_ready = false;
        if (m_btnReady != nullptr)
        {
            const bool blocked = m_btnReady->blockSignals(true);
            m_btnReady->setChecked(false);
            m_btnReady->blockSignals(blocked);
        }
        updatePeerConnectionUI();
        return;
    }
    if (text.endsWith(" is ready"))
    {
        m_peerReady = true;
        updatePeerConnectionUI();
        return;
    }
    if (text.endsWith(" is not ready"))
    {
        m_peerReady = false;
        updatePeerConnectionUI();
        return;
    }

    if (text == "reinitializing server...")
    {
        m_lobbyOpening = true;
        updateLobbyStatusLabel();
    }
    else if (text == "Done" && m_isHost && (!m_travHostEnabled || m_travHostFallbackActive))
    {
        m_lobbyOpening = false;
        updateLobbyStatusLabel();
    }

    if (isImportantP2PDebugMessage(text))
    {
        appendChatError(text);
    }
    else
    {
        appendChatHtml("<span style='color:green;'>" + message.toHtmlEscaped() + "</span>", true);
    }
}

void KailleraP2PDialog::onHostedGame(QString game)
{
    if (m_isHost)
    {
        return;
    }

    m_gameName = game;
    if (m_gameLabel != nullptr)
    {
        m_gameLabel->setText(m_gameName);
    }

    if (localGameListContains(m_gameName))
    {
        if (m_btnReady != nullptr)
        {
            m_btnReady->setEnabled(true);
        }
        return;
    }

    m_ready = false;
    if (m_btnReady != nullptr)
    {
        m_btnReady->setChecked(false);
        m_btnReady->setEnabled(false);
    }
    updatePeerConnectionUI();

    const QString message = "The ROM '" + m_gameName + "' is not in your list.";
    appendChatError(message);
    QMessageBox::warning(this, "P2P Join", message);
    reject();
}

void KailleraP2PDialog::onPingUpdated(int ping)
{
    m_lastPing = ping;
    setPingLabelFromValue(ping);

    bool autoDelayUpdated = false;
    const bool canUpdateAutomaticDelay =
        isRollbackMode() && canEditRollbackDelaySettings() &&
        !m_rollbackGameActive && !n02::isGameRunning();
    if (canUpdateAutomaticDelay)
    {
        recordRollbackDelayPingSample(ping);
        autoDelayUpdated = maybeUpdateAutomaticRollbackDelay(false);
    }

    if (isRollbackMode())
    {
        if (autoDelayUpdated)
        {
            updateRollbackDelayControls();
            sendRollbackDelaySettings(false);
        }
    }
    updatePeerConnectionUI();
}

void KailleraP2PDialog::onPeerJoined()
{
    setPingLabelText("Ping: measuring...");
    resetAutomaticRollbackDelay();
    m_peerConnected = true;
    m_peerReady = false;
    m_lobbyOpening = false;
    m_peerJoinNoticeShown = false;
    m_peerLeaveNoticeShown = false;
    updatePeerConnectionUI();
    if (isRollbackMode())
    {
        updateRollbackDelayControls();
    }
    appendChatStatus("Peer connected.", "green", true);
    QTimer::singleShot(250, this, [this]() {
        if (m_peerConnected)
        {
            appendPeerJoinedNotice();
        }
    });
    if (m_isHost && CoreSettingsGetBoolValue(SettingsID::Kaillera_BeepOnJoin))
    {
        QApplication::beep();
    }
    if (m_isHost && CoreSettingsGetBoolValue(SettingsID::Kaillera_FlashOnJoin) && !isActiveWindow())
    {
        QApplication::alert(this);
    }
    sendGameLayer();
    sendRollbackDelaySettings(true);
    sendRollbackPredictionSettings(true);
    m_travHostPeerIp.clear();
    m_travHostPeerPort = 0;
    m_travHostPeerDeadlineMs = 0;
    m_travNextHostPunchMs = 0;

    if (m_isHost && m_travHostEnabled)
    {
        m_travHostSessionSuspended = true;
        m_travNextRegMs = 0;
        m_travNextKeepMs = 0;

        if (!m_travLiveToken.isEmpty())
        {
            travSendHostClose();
            m_travLiveToken.clear();
        }
    }

    // Remove from public game list while peer is connected
    if (m_isHost && m_enlistCheck && m_enlistCheck->isChecked())
        unenlistGame();
}

void KailleraP2PDialog::onPeerLeft()
{
    const bool kickedPeer = m_peerKickPending;
    const QString departedName = m_peerName;
    m_peerKickPending = false;
    setPingLabelFromValue(-1);
    resetAutomaticRollbackDelay();
    m_peerConnected = false;
    m_peerName.clear();
    m_peerReady = false;
    m_gameActive = false;
    if (m_isHost && m_travHostEnabled)
    {
        m_lobbyOpening = true;
    }
    m_lastPing = -1;
    m_hasRemoteRollbackDelaySettings = false;
    m_hasRemoteRollbackPredictionSettings = false;
    if (!m_isHost)
    {
        loadLocalRollbackSettings();
    }
    m_hasSentRollbackDelaySettings = false;
    m_hasSentRollbackPredictionSettings = false;
    m_lastSentRollbackDelayMode = -1;
    m_lastSentRollbackFrameDelay = -1;
    m_lastSentRollbackPredictionWindow = -1;
    if (isRollbackMode())
    {
        updateRollbackDelayControls();
    }
    updateRollbackPredictionControls();
    if (!kickedPeer)
    {
        appendPeerLeftNotice(departedName);
        appendChatStatus("Peer disconnected.", "red", true);
    }
    m_ready = false;
    if (m_btnReady) m_btnReady->setChecked(false);
    updatePeerConnectionUI();
    if (!m_isHost && m_btnReady != nullptr)
    {
        m_btnReady->setEnabled(false);
    }

    // Clear peer punching state (always, regardless of trav mode)
    m_travHostPeerIp.clear();
    m_travHostPeerPort = 0;
    m_travHostPeerDeadlineMs = 0;
    m_travNextHostPunchMs = 0;

    if (m_isHost && m_travHostEnabled)
    {
        m_travHostSessionSuspended = false;
        m_travNextRegMs = 0;
        m_travNextKeepMs = 0;
    }

    // Re-enlist on public game list now that we're waiting for a new peer
    if (m_isHost && m_enlistCheck && m_enlistCheck->isChecked())
        enlistGame();
}

void KailleraP2PDialog::onPeerInfo(QString name, QString app)
{
    m_peerName = name.trimmed();
    updatePeerConnectionUI();
    emit peerNicknameResolved(name);
    if (m_peerConnected)
    {
        appendPeerJoinedNotice();
    }
    appendChatStatus("Peer: " + name + " (" + app + ")", "green", true);
}

void KailleraP2PDialog::onFodippResult(QString host)
{
    // FODIPP result serves as an additional fallback for IP display
    if (m_isHost && m_travHostIpPort.isEmpty())
    {
        int port = p2p_core_get_port();
        m_travHostIpPort = host + ":" + QString::number(port);
        m_travHostIpPending = false;
        updateHostCodeUI();
    }
    appendChatStatus("External IP: " + host, "green", true);
}

// ---- Button actions ----

void KailleraP2PDialog::onSendChat()
{
    QString text = m_chatInput->text().trimmed();
    if (text.isEmpty()) return;

    QByteArray utf8 = text.toUtf8();
    p2p_send_chat(utf8.data());
    m_chatInput->clear();
}

void KailleraP2PDialog::onReady()
{
    if (!m_isHost && m_gameName.isEmpty())
    {
        m_ready = false;
        if (m_btnReady != nullptr)
        {
            m_btnReady->setChecked(false);
        }
        updatePeerConnectionUI();
        return;
    }

    if (m_isHost)
    {
        sendGameLayer();
        sendRollbackDelaySettings(true);
        sendRollbackPredictionSettings(true);
    }

    m_ready = (m_btnReady != nullptr) ? m_btnReady->isChecked() : !m_ready;

    p2p_set_ready(m_ready);
    updatePeerConnectionUI();
}

void KailleraP2PDialog::onDrop()
{
    const bool rollbackWasActive = isRollbackMode() && m_rollbackGameActive;
    p2p_drop_game();
    if (rollbackWasActive && m_rollbackGameActive)
    {
        onGameEnded();
    }
}

void KailleraP2PDialog::onKickPeer()
{
    if (!m_isHost || !m_peerConnected || (isRollbackMode() && m_rollbackGameActive) || n02::isGameRunning())
    {
        updatePeerConnectionUI();
        return;
    }

    const QString kickedName = m_peerName.trimmed().isEmpty() ? "opponent" : m_peerName.trimmed();
    m_peerKickPending = true;
    if (p2p_kick_peer())
    {
        m_peerConnected = false;
        m_peerName.clear();
        m_peerReady = false;
        updatePeerConnectionUI();
        appendChatError("Kicked " + kickedName + " from the lobby.");
    }
    else
    {
        m_peerKickPending = false;
        updatePeerConnectionUI();
        appendChatError("Could not kick " + kickedName + ".");
    }
}

void KailleraP2PDialog::onCopyConnectCode()
{
    // Copy the best available code/address
    if (!m_travCode.isEmpty())
    {
        QApplication::clipboard()->setText(m_travCode);
    }
    else if (!m_travHostIpPort.isEmpty())
    {
        QApplication::clipboard()->setText(m_travHostIpPort);
    }
    else
    {
        return;
    }

    if (m_btnCopyConnectCode != nullptr)
    {
        m_btnCopyConnectCode->setIcon(themedP2PIcon("check-line"));
        m_btnCopyConnectCode->setToolTip("Copied");
    }

    if (m_copyFeedbackTimer != nullptr)
    {
        m_copyFeedbackTimer->start(1200);
    }
}

void KailleraP2PDialog::onStepTimer()
{
    if (isRollbackMode() && m_rollbackGameActive)
    {
        p2p_rollback_process_control();
        return;
    }

    // Standard P2P gameplay polls from the emulation thread.
    if (!n02::isGameRunning())
    {
        p2p_step();
    }
    n02::processStateMachineStep();
}

// ---- NAT traversal housekeeping (1-second timer) ----

void KailleraP2PDialog::onTravTimer()
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_travTimerStep++;

    // Match the old p2p lobby behavior: keep refreshing ping once per second
    // while a peer is in the lobby, but stop once the game has actually started.
    if (m_peerConnected && !m_gameActive)
    {
        p2p_ping();
    }

    // ---- HOST: NAT traversal registration & keepalive ----
    if (m_isHost && m_travHostEnabled)
    {
        if (m_travHostSessionSuspended)
        {
            // Keep the static code identity locally, but do not advertise or keep
            // a live host session open while a peer is connected.
        }
        else if (m_travToken.isEmpty() || m_travCode.isEmpty())
        {
            if (m_travNextRegMs == 0 || now >= m_travNextRegMs)
            {
                if (m_travRegAttempts >= 4)
                {
                    // Fallback to IP-based hosting
                    m_travHostEnabled = false;
                    m_travHostFallbackActive = true;
                    m_lobbyOpening = false;
                    m_travNextRegMs = 0;
                    m_travNextKeepMs = 0;
                    m_travHostIpPending = true;
                    m_travHostIpPort.clear();
                    updateHostCodeUI();
                    updateLobbyStatusLabel();

                    appendChatError("Failed to get a connect code from the NAT server. "
                        "Hosting by IP instead. You may need to manually port forward.");
                    ssrvWhatIsMyIp();
                }
                else
                {
                    travSendClaimAuto();
                    m_travNextRegMs = now + 2000;
                }
            }
        }
        else if (m_travLiveToken.isEmpty())
        {
            if (m_travNextRegMs == 0 || now >= m_travNextRegMs)
            {
                travSendHostOpen();
                m_travNextRegMs = now + 2000;
            }
        }
        else if (m_travNextKeepMs == 0 || now >= m_travNextKeepMs)
        {
            travSendHostKeep();
            m_travNextKeepMs = now + 10000;
        }

        // While waiting for the peer's LOGN_REQ, keep punching their endpoint
        if (!p2p_is_connected() &&
            !m_travHostPeerIp.isEmpty() &&
            m_travHostPeerPort > 0 &&
            m_travHostPeerDeadlineMs != 0 &&
            now < m_travHostPeerDeadlineMs)
        {
            if (m_travNextHostPunchMs == 0 || now >= m_travNextHostPunchMs)
            {
                travPunchEndpoint(m_travHostPeerIp, m_travHostPeerPort, m_travLiveToken);
                m_travNextHostPunchMs = now + 1000;
            }
        }
    }

    // ---- JOIN: traversal code lookup & connection retries ----
    if (!m_isHost && m_travJoinEnabled && !p2p_is_connected())
    {
        if (m_travJoinDeadlineMs != 0 && now >= m_travJoinDeadlineMs)
        {
            // Timed out
            if (m_travJoinBusy)
            {
                appendChatError("NAT traversal: host is busy. Please wait and try again.");
            }
            else
            {
                appendChatError("NAT traversal: timed out (try direct IP/port-forwarding or server mode)");
            }
            m_travJoinEnabled = false;
            m_lobbyOpening = false;
            updateLobbyStatusLabel();
            if (!m_travJoinBusy)
            {
                travTryFallbackConnect("timed out");
            }
            m_travJoinBusy = false;
        }
        else if (!m_travJoinGotHost)
        {
            // Keep asking for the host's address
            if (m_travNextJoinMs == 0 || now >= m_travNextJoinMs)
            {
                travSendJoin();
                m_travNextJoinMs = now + 3000;
            }
        }
        else
        {
            // Got host endpoint — retry punch + connect
            if (m_travNextConnectMs == 0 || now >= m_travNextConnectMs)
            {
                m_travJoinPunchAttempts++;
                travPunchEndpoint(m_travJoinHostIp, m_travJoinHostPort, m_travJoinToken);

                QByteArray ipBytes = m_travJoinHostIp.toUtf8();
                p2p_core_connect(ipBytes.data(), m_travJoinHostPort);
                m_travNextConnectMs = now + 1000;

                if (m_travJoinPunchAttempts >= 3)
                {
                    travTryFallbackConnect("trying direct IP/port");
                }
            }
        }
    }
    else if (!m_isHost && m_travJoinEnabled && p2p_is_connected())
    {
        // Connected — stop traversal retries
        m_travJoinEnabled = false;
        m_travJoinDeadlineMs = 0;
    }

    // ---- Periodic re-enlist on public game list (every 30s while waiting) ----
    if (m_travTimerStep % 30 == 0 && !p2p_is_connected() &&
        m_isHost && m_enlistCheck && m_enlistCheck->isChecked())
    {
        enlistGame();
    }
}

#endif // NETPLAY
