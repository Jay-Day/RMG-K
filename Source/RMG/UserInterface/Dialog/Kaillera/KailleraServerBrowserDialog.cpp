/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "KailleraServerBrowserDialog.hpp"
#include "KailleraTableStyle.hpp"
#include "OnScreenDisplay.hpp"

#ifdef NETPLAY

#include "../../KailleraUIBridge.hpp"
#include "KailleraOptionsDialog.hpp"

#include <RMG-Core/Kaillera.hpp>
#include <RMG-Core/Emulation.hpp>
#include <RMG-Core/Settings.hpp>

#include "n02_client.h"
#include "kailleraclient.h"
#include "kcore/kaillera_core.h"
#include "stats.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QSplitterHandle>
#include <QHeaderView>
#include <QMessageBox>
#include <QScrollBar>
#include <QTimer>
#include <QTime>
#include <QRegularExpression>
#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QIcon>
#include <QToolButton>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QProxyStyle>
#include <QStyle>

namespace
{
QString infoMessageColor()
{
    return (QApplication::palette().window().color().value() < 128) ? "cornflowerblue" : "darkblue";
}

QIcon themedLineIcon(const QString& iconName)
{
    const bool darkTheme = QApplication::palette().window().color().value() < 128;
    const QString iconPath = QString(":/icons/%1/svg/%2.svg")
        .arg(darkTheme ? "white" : "black", iconName);
    return QIcon(iconPath);
}

QIcon whiteLineIcon(const QString& iconName)
{
    return QIcon(QString(":/icons/white/svg/%1.svg").arg(iconName));
}

QColor blendColors(const QColor& fg, const QColor& bg, int fgWeight)
{
    const int clampedWeight = (fgWeight < 0) ? 0 : ((fgWeight > 100) ? 100 : fgWeight);
    const int bgWeight = 100 - clampedWeight;
    return QColor(
        (fg.red() * clampedWeight + bg.red() * bgWeight) / 100,
        (fg.green() * clampedWeight + bg.green() * bgWeight) / 100,
        (fg.blue() * clampedWeight + bg.blue() * bgWeight) / 100);
}

bool isFusionFamilyTheme(const QString& theme)
{
    return theme == "Fusion" || theme == "Fusion Warm" || theme == "Fusion Dark";
}

void setPaletteRoleForAllGroups(QPalette& palette, QPalette::ColorRole role, const QColor& color)
{
    palette.setColor(QPalette::Active, role, color);
    palette.setColor(QPalette::Inactive, role, color);
    palette.setColor(QPalette::Disabled, role, color);
}

void configureServerAccentPalette(QPushButton* button, bool enabled = true)
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
    button->setEnabled(enabled);
}

void configureServerFabMetrics(QPushButton* button)
{
    if (button == nullptr)
    {
        return;
    }
    button->setFixedSize(46, 46);
}

void configureServerButtonMetrics(QWidget* button)
{
    if (button == nullptr)
    {
        return;
    }
    button->setMinimumHeight(31);
}

void configureServerTinyButtonMetrics(QWidget* button)
{
    if (button == nullptr)
    {
        return;
    }
    button->setMinimumHeight(20);
    button->setMaximumHeight(20);
    button->setMinimumWidth(102);
}

void configureServerHeaderActionMetrics(QWidget* button)
{
    if (button == nullptr)
    {
        return;
    }
    button->setMinimumHeight(22);
}

void configureServerStartButtonMetrics(QWidget* button)
{
    if (button == nullptr)
    {
        return;
    }
    button->setMinimumHeight(41);
}

QColor playerPortColor(int portIndex)
{
    switch (portIndex)
    {
    case 0: return QColor("#D55C5C"); // P1 red
    case 1: return QColor("#4C86E8"); // P2 blue
    case 2: return QColor("#E0B24D"); // P3 yellow
    case 3: return QColor("#5AAE63"); // P4 green
    default: return QColor("#8A8F98"); // Extra players
    }
}

constexpr int PlayerIdRole = Qt::UserRole;
constexpr int PlayerNameRole = Qt::UserRole + 1;
constexpr int PlayerPingRole = Qt::UserRole + 2;
constexpr int PlayerDelayRole = Qt::UserRole + 3;

class ThinSplitterHandle final : public QSplitterHandle
{
public:
    ThinSplitterHandle(Qt::Orientation orientation, QSplitter* parent)
        : QSplitterHandle(orientation, parent)
    {
    }

    QSize sizeHint() const override
    {
        QSize hint = QSplitterHandle::sizeHint();
        if (orientation() == Qt::Horizontal)
        {
            hint.setWidth(1);
        }
        else
        {
            hint.setHeight(1);
        }
        return hint;
    }
};

class ThinSplitter final : public QSplitter
{
public:
    ThinSplitter(Qt::Orientation orientation, QWidget* parent = nullptr)
        : QSplitter(orientation, parent)
    {
    }

protected:
    QSplitterHandle* createHandle() override
    {
        return new ThinSplitterHandle(orientation(), this);
    }
};

class TableBlankClickClearFilter final : public QObject
{
public:
    explicit TableBlankClickClearFilter(QTableWidget* table)
        : QObject(table)
        , m_table(table)
    {
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (m_table == nullptr || watched != m_table->viewport() || event->type() != QEvent::MouseButtonPress)
        {
            return QObject::eventFilter(watched, event);
        }

        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (!m_table->indexAt(mouseEvent->pos()).isValid())
        {
            // Clicking empty area should clear the current row highlight.
            m_table->clearSelection();
            m_table->setCurrentCell(-1, -1);
        }

        return QObject::eventFilter(watched, event);
    }

private:
    QTableWidget* m_table = nullptr;
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

class TrailingTableColumnWidthFilter final : public QObject
{
public:
    TrailingTableColumnWidthFilter(QTableWidget* table, int trailingColumn, int minWidth, int rightInset)
        : QObject(table)
        , m_table(table)
        , m_trailingColumn(trailingColumn)
        , m_minWidth(minWidth)
        , m_rightInset(rightInset)
    {
        if (m_table != nullptr)
        {
            if (m_table->viewport() != nullptr)
            {
                m_table->viewport()->installEventFilter(this);
            }
            if (m_table->horizontalHeader() != nullptr)
            {
                m_table->horizontalHeader()->installEventFilter(this);
                connect(m_table->horizontalHeader(), &QHeaderView::sectionResized,
                        this, [this](int logicalIndex, int, int) {
                    if (!m_adjusting && logicalIndex != m_trailingColumn)
                    {
                        apply();
                    }
                });
                connect(m_table->horizontalHeader(), &QHeaderView::geometriesChanged,
                        this, [this]() {
                    if (!m_adjusting)
                    {
                        apply();
                    }
                });
            }
        }

        apply();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (m_table == nullptr)
        {
            return QObject::eventFilter(watched, event);
        }

        const bool watchedTableGeometry =
            watched == m_table->viewport() || watched == m_table->horizontalHeader();
        if (watchedTableGeometry
            && (event->type() == QEvent::Resize || event->type() == QEvent::Show || event->type() == QEvent::LayoutRequest))
        {
            apply();
        }

        return QObject::eventFilter(watched, event);
    }

private:
    void apply()
    {
        if (m_table == nullptr || m_table->horizontalHeader() == nullptr
            || m_trailingColumn < 0 || m_trailingColumn >= m_table->columnCount()
            || m_table->isColumnHidden(m_trailingColumn))
        {
            return;
        }

        int availableWidth = m_table->viewport()->width() - m_rightInset;
        if (availableWidth <= 0)
        {
            return;
        }

        int occupiedWidth = 0;
        for (int col = 0; col < m_table->columnCount(); ++col)
        {
            if (col == m_trailingColumn || m_table->isColumnHidden(col))
            {
                continue;
            }
            occupiedWidth += m_table->columnWidth(col);
        }

        const int targetWidth = qMax(m_minWidth, availableWidth - occupiedWidth);
        if (targetWidth == m_table->columnWidth(m_trailingColumn))
        {
            return;
        }

        m_adjusting = true;
        m_table->setColumnWidth(m_trailingColumn, targetWidth);
        m_adjusting = false;
    }

    QTableWidget* m_table = nullptr;
    int m_trailingColumn = -1;
    int m_minWidth = 0;
    int m_rightInset = 0;
    bool m_adjusting = false;
};

void enableBlankAreaDeselect(QTableWidget* table)
{
    if (table == nullptr)
    {
        return;
    }
    table->viewport()->installEventFilter(new TableBlankClickClearFilter(table));
}

void attachFloatingCornerButton(QWidget* container, QWidget* button, int rightMargin, int bottomMargin)
{
    if (container == nullptr || button == nullptr)
    {
        return;
    }

    container->installEventFilter(
        new FloatingCornerButtonFilter(container, button, rightMargin, bottomMargin));
}

void anchorTrailingTableColumn(QTableWidget* table, int trailingColumn, int minWidth, int rightInset)
{
    if (table == nullptr)
    {
        return;
    }

    new TrailingTableColumnWidthFilter(table, trailingColumn, minWidth, rightInset);
}
}

// QTableWidgetItem subclass that sorts numerically instead of alphabetically
class NumericTableWidgetItem : public QTableWidgetItem
{
public:
    explicit NumericTableWidgetItem(int value)
        : QTableWidgetItem(QString::number(value))
    {
        setData(Qt::UserRole, value);
    }

    bool operator<(const QTableWidgetItem& other) const override
    {
        return data(Qt::UserRole).toInt() < other.data(Qt::UserRole).toInt();
    }
};

// QTableWidgetItem subclass for game status that sorts Waiting < Playing < Idle
class StatusTableWidgetItem : public QTableWidgetItem
{
public:
    explicit StatusTableWidgetItem(const QString& text, int sortKey)
        : QTableWidgetItem(text)
    {
        setData(Qt::UserRole, sortKey);
    }

    bool operator<(const QTableWidgetItem& other) const override
    {
        return data(Qt::UserRole).toInt() < other.data(Qt::UserRole).toInt();
    }
};

KailleraServerBrowserDialog::KailleraServerBrowserDialog(const QString& serverName, QWidget* parent)
    : QDialog(parent, Qt::Window)
    , m_serverName(serverName)
{
    setWindowIcon(QIcon(":Resource/Kaillera.svg"));
    setupUI();
    connectSignals();

    // Restore saved geometry and splitter state
    std::string geom = CoreSettingsGetStringValue(SettingsID::Kaillera_BrowserGeometry);
    if (!geom.empty())
    {
        restoreGeometry(QByteArray::fromBase64(QByteArray::fromStdString(geom)));
    }
    std::string topSplit = CoreSettingsGetStringValue(SettingsID::Kaillera_BrowserTopSplitter);
    if (!topSplit.empty())
    {
        m_topSplitter->restoreState(QByteArray::fromBase64(QByteArray::fromStdString(topSplit)));
    }
    std::string botSplit = CoreSettingsGetStringValue(SettingsID::Kaillera_BrowserBottomSplitter);
    if (!botSplit.empty() && m_roomSplitter)
    {
        m_roomSplitter->restoreState(QByteArray::fromBase64(QByteArray::fromStdString(botSplit)));
    }

    // Restore saved column widths
    restoreColumnWidths();

    // Start stats timer (1-second interval for FPS/delay updates in game room)
    m_statsTimer = new QTimer(this);
    connect(m_statsTimer, &QTimer::timeout, this, &KailleraServerBrowserDialog::onStatsTimer);
}

KailleraServerBrowserDialog::~KailleraServerBrowserDialog()
{
    if (m_statsTimer)
    {
        m_statsTimer->stop();
    }
}

void KailleraServerBrowserDialog::reject()
{
    if (m_isClosing)
    {
        return;
    }
    m_isClosing = true;

    if (m_statsTimer)
    {
        m_statsTimer->stop();
    }

    // Prevent any further lobby/game callbacks from targeting this dialog
    // while the network core is being torn down.
    QObject::disconnect(&KailleraUIBridge::instance(), nullptr, this, nullptr);

    // Save geometry and splitter state
    CoreSettingsSetValue(SettingsID::Kaillera_BrowserGeometry,
        saveGeometry().toBase64().toStdString());
    CoreSettingsSetValue(SettingsID::Kaillera_BrowserTopSplitter,
        m_topSplitter->saveState().toBase64().toStdString());
    if (m_roomSplitter)
    {
        CoreSettingsSetValue(SettingsID::Kaillera_BrowserBottomSplitter,
            m_roomSplitter->saveState().toBase64().toStdString());
    }
    saveColumnWidths();
    CoreSettingsSave();

    if (m_inGameRoom)
    {
        kaillera_leave_game();
    }

    kaillera_disconnect(nullptr);
    kaillera_core_cleanup();

    QDialog::reject();
}

void KailleraServerBrowserDialog::setupUI()
{
    setWindowTitle("Connected to " + m_serverName);
    setMinimumSize(700, 500);
    resize(800, 600);

    const QString theme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_Theme));
    const bool modern = (theme == "Modern");
    const QString paneRadius = modern ? "10px" : "2px";
    const QString splitOuterRadius = modern ? "10px" : "2px";
    const QString playerCardRadius = modern ? "8px" : "2px";
    const QString kickButtonRadius = modern ? "6px" : "2px";
    const QString tableShellRadius = modern ? "9px" : "2px";
    const QString composerRadius = modern ? "7px" : "2px";
    const QString sidebarRadius = modern ? "10px" : "2px";
    const QString headerIconRadius = modern ? "5px" : "2px";
    const QColor windowColor = QApplication::palette().window().color();
    const bool darkTheme = windowColor.value() < 128;

    // Style the joined-server UI surfaces and keep list/table selection visually neutral.
    QString style = QString(
        "QWidget#KailleraPane {"
        "  border: 1px solid palette(mid);"
        "  border-radius: %1;"
        "  background-color: palette(base);"
        "}"
        "QWidget#KailleraPaneGameList {"
        "  border: 1px solid palette(mid);"
        "  border-radius: %1;"
        "  background-color: palette(base);"
        "}"
        "QWidget#KailleraPaneLeftJoined {"
        "  border: 1px solid palette(mid);"
        "  border-right: none;"
        "  border-top-left-radius: %2;"
        "  border-bottom-left-radius: %2;"
        "  border-top-right-radius: 0px;"
        "  border-bottom-right-radius: 0px;"
        "  background-color: palette(base);"
        "}"
        "QWidget#KailleraPaneRightJoined {"
        "  border: 1px solid palette(mid);"
        "  border-left: none;"
        "  border-top-left-radius: 0px;"
        "  border-bottom-left-radius: 0px;"
        "  border-top-right-radius: %2;"
        "  border-bottom-right-radius: %2;"
        "  background-color: palette(base);"
        "}"
        "QWidget#KailleraPaneHeader {"
        "  border: none;"
        "  border-bottom: 1px solid palette(mid);"
        "  min-height: 30px;"
        "  background-color: transparent;"
        "}"
        "QSplitter#KailleraJoinedSplitter::handle {"
        "  background-color: palette(mid);"
        "  margin: 0px;"
        "  padding: 0px;"
        "}"
        "QSplitter#KailleraJoinedSplitter::handle:horizontal {"
        "  width: 1px;"
        "  border: none;"
        "}"
        "QLabel#KailleraPaneTitle {"
        "  font-weight: 400;"
        "  font-size: 13px;"
        "  letter-spacing: 0.6px;"
        "}"
        "QLabel#KailleraPaneMeta {"
        "  color: palette(mid);"
        "  font-weight: 600;"
        "}"
        "QTextBrowser#KailleraSurface, QTableWidget#KailleraSurface {"
        "  border: none;"
        "  background: transparent;"
        "}"
        "QTableWidget#KailleraSurface {"
        "  gridline-color: transparent;"
        "  alternate-background-color: palette(alternate-base);"
        "  selection-background-color: transparent;"
        "  selection-color: palette(text);"
        "}"
        "QTableWidget#KailleraSurface::item:selected {"
        "  background: transparent;"
        "  color: palette(text);"
        "}"
        "QListWidget#KailleraPlayerList {"
        "  border: none;"
        "  background: transparent;"
        "  outline: none;"
        "}"
        "QListWidget#KailleraPlayerList::item {"
        "  border: none;"
        "  padding: 0px;"
        "  margin: 0px 0px 6px 0px;"
        "}"
        "QListWidget#KailleraPlayerList::item:selected {"
        "  background: transparent;"
        "}"
        "QWidget#KailleraPlayerCard {"
        "  border: none;"
        "  border-radius: %3;"
        "}"
        "QPushButton#KailleraPlayerKickButton {"
        "  border: 1px solid transparent;"
        "  border-radius: %4;"
        "  min-width: 20px;"
        "  max-width: 20px;"
        "  min-height: 20px;"
        "  max-height: 20px;"
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
        "QLabel#KailleraPlayerName {"
        "  font-weight: 600;"
        "  font-size: 14px;"
        "}"
        "QLabel#KailleraPlayerStats {"
        "  color: palette(mid);"
        "}"
        "QTableWidget#KailleraSurface[tableShell=\"true\"] {"
        "  border-top-left-radius: %5;"
        "  border-top-right-radius: %5;"
        "}"
        "QWidget#KailleraChatComposer {"
        "  border: 1px solid palette(mid);"
        "  border-radius: %6;"
        "  background-color: palette(base);"
        "}"
        "QLineEdit#KailleraChatComposerInput {"
        "  border: none;"
        "  background: transparent;"
        "  padding: 1px 0px;"
        "  min-height: 16px;"
        "}"
        "QLineEdit#KailleraChatComposerInput:focus {"
        "  border: none;"
        "}"
        "QPushButton#KailleraChatComposerSendButton {"
        "  border: none;"
        "  border-radius: 6px;"
        "  min-width: 20px;"
        "  min-height: 20px;"
        "  max-width: 20px;"
        "  max-height: 20px;"
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
        "QWidget#KailleraSidebar {"
        "  border: 1px solid palette(mid);"
        "  border-radius: %7;"
        "  background-color: palette(base);"
        "}"
        "QPushButton#KailleraHeaderIconButton {"
        "  border: 1px solid transparent;"
        "  border-radius: %8;"
        "  min-width: 20px;"
        "  max-width: 20px;"
        "  min-height: 20px;"
        "  max-height: 20px;"
        "  padding: 0px;"
        "  background-color: transparent;"
        "}"
        "QPushButton#KailleraHeaderIconButton:hover {"
        "  border: 1px solid palette(mid);"
        "  background-color: palette(light);"
        "}"
        "QPushButton#KailleraHeaderIconButton:pressed {"
        "  border: 1px solid palette(shadow);"
        "  background-color: palette(mid);"
        "  padding-top: 1px;"
        "}"
        "QLabel#KailleraStatValue {"
        "  font-weight: 600;"
        "}").arg(
            paneRadius,
            splitOuterRadius,
            playerCardRadius,
            kickButtonRadius,
            tableShellRadius,
            composerRadius,
            sidebarRadius,
            headerIconRadius);

    if (modern)
    {
        style +=
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
            "QTableWidget#KailleraSurface[roomLobbySnapshot=\"true\"] QHeaderView::section:first {"
            "  border-left: 1px solid palette(mid);"
            "}"
            "QTableWidget#KailleraSurface[mainGameList=\"true\"] QHeaderView::section:first {"
            "  border-top-left-radius: 9px;"
            "}"
            "QTableWidget#KailleraSurface[mainGameList=\"true\"] QHeaderView::section:last {"
            "  border-top-right-radius: 9px;"
            "}"
            "QPushButton#KailleraPrimaryButton {"
            "  border: 1px solid #0066b4;"
            "  border-radius: 7px;"
            "  padding: 4px 12px;"
            "  font-weight: 700;"
            "  color: white;"
            "  background-color: #0078D7;"
            "}"
            "QPushButton#KailleraPrimaryButton:hover {"
            "  background-color: #1c88dc;"
            "}"
            "QPushButton#KailleraPrimaryButton:pressed {"
            "  border-color: #004f8b;"
            "  background-color: #005a9e;"
            "  padding-top: 5px;"
            "  padding-bottom: 3px;"
            "}"
            "QPushButton#KailleraFabButton {"
            "  border: 1px solid #005a9e;"
            "  border-radius: 23px;"
            "  padding: 0px;"
            "  background-color: #0078D7;"
            "}"
            "QPushButton#KailleraFabButton:hover {"
            "  background-color: #1c88dc;"
            "}"
            "QPushButton#KailleraFabButton:pressed {"
            "  border-color: #004f8b;"
            "  background-color: #005a9e;"
            "}"
            "QPushButton#KailleraSecondaryButton {"
            "  border: 1px solid palette(mid);"
            "  border-radius: 7px;"
            "  padding: 4px 10px;"
            "  background-color: palette(window);"
            "}"
            "QPushButton#KailleraSecondaryButton:hover {"
            "  background-color: palette(light);"
            "}"
            "QPushButton#KailleraSecondaryButton:pressed {"
            "  border-color: palette(shadow);"
            "  background-color: palette(mid);"
            "  padding-top: 5px;"
            "  padding-bottom: 3px;"
            "}"
            "QPushButton#KailleraSecondarySplitMain {"
            "  border: 1px solid palette(mid);"
            "  border-right: none;"
            "  border-top-left-radius: 7px;"
            "  border-bottom-left-radius: 7px;"
            "  border-top-right-radius: 0px;"
            "  border-bottom-right-radius: 0px;"
            "  padding: 4px 10px;"
            "  background-color: palette(window);"
            "}"
            "QPushButton#KailleraSecondarySplitMain:hover {"
            "  border-right: none;"
            "  background-color: palette(light);"
            "}"
            "QPushButton#KailleraSecondarySplitMain:pressed {"
            "  border-color: palette(shadow);"
            "  border-right: none;"
            "  background-color: palette(mid);"
            "  padding-top: 5px;"
            "  padding-bottom: 3px;"
            "}"
            "QPushButton#KailleraSecondarySplitMenu,"
            "QToolButton#KailleraSecondarySplitMenu {"
            "  border: 1px solid palette(mid);"
            "  border-top-left-radius: 0px;"
            "  border-bottom-left-radius: 0px;"
            "  border-top-right-radius: 7px;"
            "  border-bottom-right-radius: 7px;"
            "  min-width: 24px;"
            "  max-width: 24px;"
            "  padding: 0px;"
            "  background-color: palette(window);"
            "}"
            "QPushButton#KailleraSecondarySplitMenu:hover,"
            "QToolButton#KailleraSecondarySplitMenu:hover {"
            "  background-color: palette(light);"
            "}"
            "QPushButton#KailleraSecondarySplitMenu:pressed,"
            "QPushButton#KailleraSecondarySplitMenu:checked,"
            "QToolButton#KailleraSecondarySplitMenu:pressed,"
            "QToolButton#KailleraSecondarySplitMenu:checked,"
            "QToolButton#KailleraSecondarySplitMenu:open {"
            "  border-color: palette(shadow);"
            "  background-color: palette(mid);"
            "  padding-top: 1px;"
            "}"
            "QToolButton#KailleraSecondarySplitMenu::menu-indicator {"
            "  image: none;"
            "  width: 0px;"
            "}"
            "QPushButton#KailleraTinyButton {"
            "  border: 1px solid palette(mid);"
            "  border-radius: 7px;"
            "  max-height: 20px;"
            "  max-width: 102px;"
            "  padding: 0px 6px;"
            "  background-color: palette(window);"
            "  font-weight: 600;"
            "}"
            "QPushButton#KailleraTinyButton:hover {"
            "  background-color: palette(light);"
            "}"
            "QPushButton#KailleraTinyButton:pressed {"
            "  border-color: palette(shadow);"
            "  background-color: palette(mid);"
            "  padding-top: 1px;"
            "}"
            "QPushButton#KailleraHeaderActionButton {"
            "  border: 1px solid palette(mid);"
            "  border-radius: 7px;"
            "  padding: 0px 8px;"
            "  background-color: palette(window);"
            "  font-weight: 600;"
            "}"
            "QPushButton#KailleraHeaderActionButton:hover {"
            "  background-color: palette(light);"
            "}"
            "QPushButton#KailleraHeaderActionButton:pressed {"
            "  border-color: palette(shadow);"
            "  background-color: palette(mid);"
            "  padding-top: 1px;"
            "}"
            "QPushButton#KailleraStartButton {"
            "  border: 1px solid #005a9e;"
            "  border-radius: 7px;"
            "  padding: 6px 14px;"
            "  color: white;"
            "  background-color: #0078D7;"
            "  font-size: 15px;"
            "  font-weight: 700;"
            "}"
            "QPushButton#KailleraStartButton:hover {"
            "  background-color: #1c88dc;"
            "}"
            "QPushButton#KailleraStartButton:pressed {"
            "  border-color: #004f8b;"
            "  background-color: #005a9e;"
            "  padding-top: 7px;"
            "  padding-bottom: 5px;"
            "}"
            "QPushButton#KailleraStartButton:disabled {"
            "  border: 1px solid palette(mid);"
            "  color: palette(mid);"
            "  background-color: palette(window);"
            "}";
    }

    if (darkTheme)
    {
        const QString borderColor = blendColors(QColor(Qt::white), windowColor, 20).name(QColor::HexRgb);
        style.replace("palette(mid)", borderColor);
    }

    setStyleSheet(style);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(10);

    // === TOP SECTION (always visible) ===

    // Lobby chat (left) + User table (right)
    m_topSplitter = new ThinSplitter(Qt::Horizontal, this);
    m_topSplitter->setObjectName("KailleraJoinedSplitter");
    m_topSplitter->setHandleWidth(1);
    auto* lobbyPane = new QWidget(this);
    lobbyPane->setObjectName("KailleraPaneLeftJoined");
    auto* lobbyPaneLayout = new QVBoxLayout(lobbyPane);
    lobbyPaneLayout->setContentsMargins(0, 0, 0, 0);
    lobbyPaneLayout->setSpacing(0);

    auto* lobbyHeader = new QWidget(lobbyPane);
    lobbyHeader->setObjectName("KailleraPaneHeader");
    auto* lobbyHeaderLayout = new QHBoxLayout(lobbyHeader);
    lobbyHeaderLayout->setContentsMargins(10, 4, 10, 4);
    lobbyHeaderLayout->setSpacing(6);
    auto* lobbyHeaderIcon = new QLabel(lobbyHeader);
    lobbyHeaderIcon->setPixmap(themedLineIcon("server-line").pixmap(14, 14));
    auto* lobbyHeaderTitle = new QLabel("SERVER CHAT", lobbyHeader);
    lobbyHeaderTitle->setObjectName("KailleraPaneTitle");
    lobbyHeaderLayout->addWidget(lobbyHeaderIcon);
    lobbyHeaderLayout->addWidget(lobbyHeaderTitle);
    lobbyHeaderLayout->addStretch();
    auto* leaveServerButton = new QPushButton("Leave Server", lobbyHeader);
    leaveServerButton->setObjectName("KailleraHeaderActionButton");
    leaveServerButton->setIcon(themedLineIcon("door-open-line"));
    leaveServerButton->setIconSize(QSize(14, 14));
    configureServerHeaderActionMetrics(leaveServerButton);
    leaveServerButton->setToolTip("Disconnect from the server and return to the server list");
    leaveServerButton->setAutoDefault(false);
    leaveServerButton->setDefault(false);
    connect(leaveServerButton, &QPushButton::clicked, this, [this]() { reject(); });
    lobbyHeaderLayout->addWidget(leaveServerButton);

    auto* lobbyBody = new QWidget(lobbyPane);
    auto* lobbyBodyLayout = new QVBoxLayout(lobbyBody);
    lobbyBodyLayout->setContentsMargins(8, 8, 8, 8);
    lobbyBodyLayout->setSpacing(8);

    m_lobbyChat = new QTextBrowser(lobbyBody);
    m_lobbyChat->setObjectName("KailleraSurface");
    m_lobbyChat->setReadOnly(true);
    m_lobbyChat->setOpenExternalLinks(true);
    m_lobbyChat->document()->setMaximumBlockCount(2000);
    lobbyBodyLayout->addWidget(m_lobbyChat);

    auto* lobbyComposer = new QWidget(lobbyBody);
    lobbyComposer->setObjectName("KailleraChatComposer");
    lobbyComposer->setFixedHeight(24);
    auto* lobbyComposerLayout = new QHBoxLayout(lobbyComposer);
    lobbyComposerLayout->setContentsMargins(10, 2, 4, 2);
    lobbyComposerLayout->setSpacing(4);

    m_lobbyChatInput = new QLineEdit(lobbyComposer);
    m_lobbyChatInput->setPlaceholderText("Type a message...");
    m_lobbyChatInput->setClearButtonEnabled(true);
    m_lobbyChatInput->setObjectName("KailleraChatComposerInput");
    m_lobbyChatInput->setFrame(false);

    m_btnSendLobby = new QPushButton(lobbyComposer);
    m_btnSendLobby->setObjectName("KailleraChatComposerSendButton");
    m_btnSendLobby->setToolTip("Send lobby chat message");
    m_btnSendLobby->setText("");
    m_btnSendLobby->setIcon(themedLineIcon("play-line"));
    m_btnSendLobby->setIconSize(QSize(13, 13));
    m_btnSendLobby->setAutoDefault(false);
    m_btnSendLobby->setDefault(false);
    connect(m_btnSendLobby, &QPushButton::clicked, this, &KailleraServerBrowserDialog::onSendLobbyChat);
    connect(m_lobbyChatInput, &QLineEdit::returnPressed, this, &KailleraServerBrowserDialog::onSendLobbyChat);

    lobbyComposerLayout->addWidget(m_lobbyChatInput);
    lobbyComposerLayout->addWidget(m_btnSendLobby, 0, Qt::AlignVCenter);

    lobbyBodyLayout->addWidget(lobbyComposer);

    lobbyPaneLayout->addWidget(lobbyHeader);
    lobbyPaneLayout->addWidget(lobbyBody, 1);
    m_topSplitter->addWidget(lobbyPane);

    // User list — styled as a selectable list
    auto* usersPane = new QWidget(this);
    usersPane->setObjectName("KailleraPaneRightJoined");
    auto* usersPaneLayout = new QVBoxLayout(usersPane);
    usersPaneLayout->setContentsMargins(0, 0, 0, 0);
    usersPaneLayout->setSpacing(0);

    auto* usersHeader = new QWidget(usersPane);
    usersHeader->setObjectName("KailleraPaneHeader");
    if (!modern)
    {
        usersHeader->setFixedHeight(32);
    }
    else
    {
        usersHeader->setMinimumHeight(31);
    }
    auto* usersHeaderLayout = new QHBoxLayout(usersHeader);
    usersHeaderLayout->setContentsMargins(10, 4, 10, 4);
    usersHeaderLayout->setSpacing(6);
    auto* usersHeaderIcon = new QLabel(usersHeader);
    usersHeaderIcon->setPixmap(themedLineIcon("list-check").pixmap(14, 14));
    auto* usersHeaderTitle = new QLabel("PLAYERS", usersHeader);
    usersHeaderTitle->setObjectName("KailleraPaneTitle");
    usersHeaderLayout->addWidget(usersHeaderIcon);
    usersHeaderLayout->addWidget(usersHeaderTitle);
    usersHeaderLayout->addStretch();
    m_connectedPlayersCountLabel = new QLabel("(0)", usersHeader);
    m_connectedPlayersCountLabel->setObjectName("KailleraPaneMeta");
    usersHeaderLayout->addWidget(m_connectedPlayersCountLabel);
    if (!modern)
    {
        QTimer::singleShot(0, this, [lobbyHeader, usersHeader]() {
            if (lobbyHeader != nullptr && usersHeader != nullptr)
            {
                usersHeader->setFixedHeight(lobbyHeader->height());
            }
        });
    }

    auto* usersBody = new QWidget(usersPane);
    auto* usersBodyLayout = new QVBoxLayout(usersBody);
    // Keep the right/bottom inset but let the header sit flush against the title row and left border.
    usersBodyLayout->setContentsMargins(0, 0, 1, 1);
    usersBodyLayout->setSpacing(0);

    m_userTable = new QTableWidget(0, 4, usersBody);
    m_userTable->setObjectName("KailleraSurface");
    m_userTable->setProperty("tableShell", true);
    m_userTable->setHorizontalHeaderLabels({"Name", "Ping", "UID", "Status"});
    m_userTable->horizontalHeader()->setStretchLastSection(false);
    m_userTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_userTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_userTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_userTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_userTable->verticalHeader()->setDefaultSectionSize(24);
    m_userTable->verticalHeader()->setVisible(false);
    m_userTable->setShowGrid(false);
    m_userTable->setAlternatingRowColors(true);
    m_userTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_userTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_userTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_userTable->setSortingEnabled(true);
    m_userTable->horizontalHeader()->setMinimumSectionSize(16);
    m_userTable->setColumnWidth(0, 130);
    m_userTable->setColumnWidth(1, 64);
    m_userTable->setColumnWidth(2, 64);
    applyNoAccentStyle(m_userTable);
    installHeaderDoubleClickSortToggle(m_userTable);
    m_userTable->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_userTable->horizontalHeader(), &QWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        QMenu menu(this);
        for (int i = 0; i < m_userTable->columnCount(); ++i) {
            QAction* act = menu.addAction(m_userTable->horizontalHeaderItem(i)->text());
            act->setCheckable(true);
            act->setChecked(!m_userTable->isColumnHidden(i));
            connect(act, &QAction::toggled, this, [this, i](bool visible) {
                m_userTable->setColumnHidden(i, !visible);
            });
        }
        menu.exec(m_userTable->horizontalHeader()->mapToGlobal(pos));
    });
    m_userTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_userTable, &QWidget::customContextMenuRequested,
            this, &KailleraServerBrowserDialog::onUserListContextMenu);
    enableBlankAreaDeselect(m_userTable);
    usersBodyLayout->addWidget(m_userTable);
    usersPaneLayout->addWidget(usersHeader);
    usersPaneLayout->addWidget(usersBody, 1);
    m_topSplitter->addWidget(usersPane);
    m_userTable->sortByColumn(1, Qt::AscendingOrder);

    m_topSplitter->setStretchFactor(0, 3);
    m_topSplitter->setStretchFactor(1, 2);
    mainLayout->addWidget(m_topSplitter, 3);

    // === BOTTOM SECTION (swappable between game list and game room) ===

    m_bottomStack = new QStackedWidget(this);
    m_bottomStack->addWidget(createGameListWidget());   // index 0 = game table
    m_bottomStack->addWidget(createGameRoomWidget());   // index 1 = game room
    m_bottomStack->setCurrentIndex(0);
    mainLayout->addWidget(m_bottomStack, 2);

    if (!modern && m_userTable != nullptr)
    {
        const int normalizedHeaderHeight = qMax(
            m_userTable->horizontalHeader()->sizeHint().height(),
            qMax(
                m_gameTable != nullptr ? m_gameTable->horizontalHeader()->sizeHint().height() : 0,
                m_roomLobbyTable != nullptr ? m_roomLobbyTable->horizontalHeader()->sizeHint().height() : 0));
        m_userTable->horizontalHeader()->setFixedHeight(normalizedHeaderHeight);
        if (m_gameTable != nullptr)
        {
            m_gameTable->horizontalHeader()->setFixedHeight(normalizedHeaderHeight);
        }
        if (m_roomLobbyTable != nullptr)
        {
            m_roomLobbyTable->horizontalHeader()->setFixedHeight(normalizedHeaderHeight);
        }
    }

    // Build game list context menu for Create Game
    m_gameListMenu = new QMenu(this);
    buildGameListMenu();
    updateHeaderCounts();
}

QWidget* KailleraServerBrowserDialog::createGameListWidget()
{
    const QString theme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_Theme));
    const bool modern = (theme == "Modern");

    auto* widget = new QWidget();
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* pane = new QWidget(widget);
    pane->setObjectName("KailleraPaneGameList");
    auto* paneLayout = new QVBoxLayout(pane);
    // Keep a 1px inset so table contents don't paint over card borders.
    paneLayout->setContentsMargins(1, 1, 1, 1);
    paneLayout->setSpacing(0);

    m_gameTable = new QTableWidget(0, 6, pane);
    m_gameTable->setObjectName("KailleraSurface");
    m_gameTable->setProperty("mainGameList", true);
    m_gameTable->setProperty("tableShell", true);
    m_gameTable->setHorizontalHeaderLabels({"Game", "GameID", "Emulator", "User", "Status", "Users"});
    m_gameTable->horizontalHeader()->setStretchLastSection(false);
    m_gameTable->verticalHeader()->setVisible(false);
    m_gameTable->setShowGrid(false);
    m_gameTable->setAlternatingRowColors(true);
    m_gameTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_gameTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_gameTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_gameTable->setSortingEnabled(true);
    m_gameTable->horizontalHeader()->setMinimumSectionSize(16);
    applyNoAccentStyle(m_gameTable);
    installHeaderDoubleClickSortToggle(m_gameTable);
    m_gameTable->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_gameTable->horizontalHeader(), &QWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        QMenu menu(this);
        for (int i = 0; i < m_gameTable->columnCount(); ++i) {
            QAction* act = menu.addAction(m_gameTable->horizontalHeaderItem(i)->text());
            act->setCheckable(true);
            act->setChecked(!m_gameTable->isColumnHidden(i));
            connect(act, &QAction::toggled, this, [this, i](bool visible) {
                m_gameTable->setColumnHidden(i, !visible);
            });
        }
        menu.exec(m_gameTable->horizontalHeader()->mapToGlobal(pos));
    });
    m_gameTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_gameTable, &QWidget::customContextMenuRequested,
            this, &KailleraServerBrowserDialog::onGameListContextMenu);
    enableBlankAreaDeselect(m_gameTable);
    // Double-click a game to join it
    connect(m_gameTable, &QTableWidget::cellDoubleClicked, this, [this]() { onJoinGame(); });
    paneLayout->addWidget(m_gameTable, 1);
    anchorTrailingTableColumn(m_gameTable, 5, 76, 2);

    m_btnCreateSwap = new QPushButton(pane);
    m_btnCreateSwap->setObjectName("KailleraFabButton");
    m_btnCreateSwap->setToolTip("Create a game");
    m_btnCreateSwap->setText("");
    configureServerFabMetrics(m_btnCreateSwap);
    if (modern)
    {
        m_btnCreateSwap->setIcon(whiteLineIcon("add-line"));
    }
    else
    {
        m_btnCreateSwap->setIcon(themedLineIcon("add-line"));
        configureServerAccentPalette(m_btnCreateSwap);
    }
    m_btnCreateSwap->setIconSize(QSize(22, 22));
    m_btnCreateSwap->setCursor(Qt::PointingHandCursor);
    m_btnCreateSwap->setAutoDefault(false);
    m_btnCreateSwap->setDefault(false);
    connect(m_btnCreateSwap, &QPushButton::clicked, this, &KailleraServerBrowserDialog::onCreateOrSwap);
    attachFloatingCornerButton(pane, m_btnCreateSwap, 14, 14);

    layout->addWidget(pane, 1);
    m_gameTable->sortByColumn(4, Qt::AscendingOrder);

    return widget;
}

QWidget* KailleraServerBrowserDialog::createGameRoomWidget()
{
    const QString theme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_Theme));
    const bool modern = (theme == "Modern");

    auto* widget = new QWidget();
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    // Main game room area: Game chat (left) + Player list (center) + Buttons (right)
    m_roomSplitter = new ThinSplitter(Qt::Horizontal, widget);
    m_roomSplitter->setObjectName("KailleraJoinedSplitter");
    m_roomSplitter->setHandleWidth(1);
    auto* roomSplitter = m_roomSplitter;

    // Game chat (left)
    auto* chatPane = new QWidget(widget);
    chatPane->setObjectName("KailleraPaneLeftJoined");
    auto* chatPaneLayout = new QVBoxLayout(chatPane);
    chatPaneLayout->setContentsMargins(0, 0, 0, 0);
    chatPaneLayout->setSpacing(0);

    auto* chatHeader = new QWidget(chatPane);
    chatHeader->setObjectName("KailleraPaneHeader");
    auto* chatHeaderLayout = new QHBoxLayout(chatHeader);
    chatHeaderLayout->setContentsMargins(10, 4, 10, 4);
    chatHeaderLayout->setSpacing(6);
    auto* chatHeaderIcon = new QLabel(chatHeader);
    chatHeaderIcon->setPixmap(themedLineIcon("file-list-line").pixmap(14, 14));
    auto* chatHeaderTitle = new QLabel("LOBBY CHAT", chatHeader);
    chatHeaderTitle->setObjectName("KailleraPaneTitle");
    chatHeaderLayout->addWidget(chatHeaderIcon);
    chatHeaderLayout->addWidget(chatHeaderTitle);
    chatHeaderLayout->addStretch();
    m_btnSwapChat = new QPushButton("Show lobbies", chatHeader);
    m_btnSwapChat->setObjectName("KailleraTinyButton");
    m_btnSwapChat->setIcon(themedLineIcon("swap-line"));
    configureServerTinyButtonMetrics(m_btnSwapChat);
    m_btnSwapChat->setToolTip("Show open lobbies");
    m_btnSwapChat->setAutoDefault(false);
    m_btnSwapChat->setDefault(false);
    connect(m_btnSwapChat, &QPushButton::clicked, this, &KailleraServerBrowserDialog::onCreateOrSwap);
    chatHeaderLayout->addWidget(m_btnSwapChat);

    m_roomChatStack = new QStackedWidget(chatPane);

    auto* gameChatPage = new QWidget(chatPane);
    auto* chatVBox = new QVBoxLayout(gameChatPage);
    chatVBox->setContentsMargins(8, 8, 8, 8);
    chatVBox->setSpacing(8);

    m_gameChat = new QTextBrowser(gameChatPage);
    m_gameChat->setObjectName("KailleraSurface");
    m_gameChat->setReadOnly(true);
    m_gameChat->setOpenExternalLinks(true);
    m_gameChat->document()->setMaximumBlockCount(1000);
    chatVBox->addWidget(m_gameChat);

    // Game chat input + send
    auto* gameChatRow = new QHBoxLayout();
    gameChatRow->setContentsMargins(0, 0, 0, 0);

    auto* gameComposer = new QWidget(gameChatPage);
    gameComposer->setObjectName("KailleraChatComposer");
    gameComposer->setFixedHeight(24);
    auto* gameComposerLayout = new QHBoxLayout(gameComposer);
    gameComposerLayout->setContentsMargins(10, 2, 4, 2);
    gameComposerLayout->setSpacing(4);

    m_gameChatInput = new QLineEdit(gameComposer);
    m_gameChatInput->setPlaceholderText("Type a message...");
    m_gameChatInput->setClearButtonEnabled(true);
    m_gameChatInput->setObjectName("KailleraChatComposerInput");
    m_gameChatInput->setFrame(false);

    m_btnSendGame = new QPushButton(gameComposer);
    m_btnSendGame->setObjectName("KailleraChatComposerSendButton");
    m_btnSendGame->setToolTip("Send game chat message");
    m_btnSendGame->setText("");
    m_btnSendGame->setIcon(themedLineIcon("play-line"));
    m_btnSendGame->setIconSize(QSize(13, 13));
    m_btnSendGame->setAutoDefault(false);
    m_btnSendGame->setDefault(false);
    connect(m_btnSendGame, &QPushButton::clicked, this, &KailleraServerBrowserDialog::onSendGameChat);
    connect(m_gameChatInput, &QLineEdit::returnPressed, this, &KailleraServerBrowserDialog::onSendGameChat);

    gameComposerLayout->addWidget(m_gameChatInput);
    gameComposerLayout->addWidget(m_btnSendGame, 0, Qt::AlignVCenter);
    gameChatRow->addWidget(gameComposer);
    chatVBox->addLayout(gameChatRow);

    auto* roomLobbiesPage = new QWidget(chatPane);
    auto* roomLobbiesLayout = new QVBoxLayout(roomLobbiesPage);
    roomLobbiesLayout->setContentsMargins(0, 0, 0, 0);
    roomLobbiesLayout->setSpacing(0);
    m_roomLobbyTable = new QTableWidget(0, 6, roomLobbiesPage);
    m_roomLobbyTable->setObjectName("KailleraSurface");
    m_roomLobbyTable->setProperty("roomLobbySnapshot", true);
    m_roomLobbyTable->setHorizontalHeaderLabels({"Game", "GameID", "Emulator", "User", "Status", "Users"});
    m_roomLobbyTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_roomLobbyTable->horizontalHeader()->setStretchLastSection(false);
    m_roomLobbyTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_roomLobbyTable->verticalHeader()->setVisible(false);
    m_roomLobbyTable->setShowGrid(false);
    m_roomLobbyTable->setAlternatingRowColors(true);
    m_roomLobbyTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_roomLobbyTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_roomLobbyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_roomLobbyTable->setSortingEnabled(true);
    m_roomLobbyTable->horizontalHeader()->setMinimumSectionSize(16);
    applyNoAccentStyle(m_roomLobbyTable);
    installHeaderDoubleClickSortToggle(m_roomLobbyTable);
    m_roomLobbyTable->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_roomLobbyTable->horizontalHeader(), &QWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        QMenu menu(this);
        for (int i = 0; i < m_roomLobbyTable->columnCount(); ++i) {
            QAction* act = menu.addAction(m_roomLobbyTable->horizontalHeaderItem(i)->text());
            act->setCheckable(true);
            act->setChecked(!m_roomLobbyTable->isColumnHidden(i));
            connect(act, &QAction::toggled, this, [this, i](bool visible) {
                m_roomLobbyTable->setColumnHidden(i, !visible);
                if (m_gameTable != nullptr && i < m_gameTable->columnCount())
                {
                    m_gameTable->setColumnHidden(i, !visible);
                }
            });
        }
        menu.exec(m_roomLobbyTable->horizontalHeader()->mapToGlobal(pos));
    });
    connect(m_roomLobbyTable->horizontalHeader(), &QHeaderView::sectionResized,
            this, [this](int logicalIndex, int, int newSize) {
        const int lastColumn = (m_roomLobbyTable != nullptr) ? (m_roomLobbyTable->columnCount() - 1) : -1;
        if (logicalIndex == lastColumn)
        {
            return;
        }
        if (m_gameTable != nullptr && logicalIndex >= 0 && logicalIndex < m_gameTable->columnCount())
        {
            m_gameTable->setColumnWidth(logicalIndex, newSize);
        }
    });
    connect(m_roomLobbyTable, &QTableWidget::cellDoubleClicked,
            this, [this](int row, int) { tryJoinGameFromTable(m_roomLobbyTable, row, true); });
    enableBlankAreaDeselect(m_roomLobbyTable);
    roomLobbiesLayout->addWidget(m_roomLobbyTable);

    m_roomChatStack->addWidget(gameChatPage);
    m_roomChatStack->addWidget(roomLobbiesPage);
    m_roomChatStack->setCurrentIndex(0);
    chatPaneLayout->addWidget(chatHeader);
    chatPaneLayout->addWidget(m_roomChatStack, 1);

    roomSplitter->addWidget(chatPane);

    // Player list (center)
    auto* playersPane = new QWidget(widget);
    playersPane->setObjectName("KailleraPaneRightJoined");
    auto* playersPaneLayout = new QVBoxLayout(playersPane);
    playersPaneLayout->setContentsMargins(0, 0, 0, 0);
    playersPaneLayout->setSpacing(0);

    auto* playersHeader = new QWidget(playersPane);
    playersHeader->setObjectName("KailleraPaneHeader");
    auto* playersHeaderLayout = new QHBoxLayout(playersHeader);
    playersHeaderLayout->setContentsMargins(10, 4, 10, 4);
    playersHeaderLayout->setSpacing(6);
    auto* playersHeaderIcon = new QLabel(playersHeader);
    playersHeaderIcon->setPixmap(themedLineIcon("gamepad-line").pixmap(14, 14));
    auto* playersHeaderTitle = new QLabel("LOBBY", playersHeader);
    playersHeaderTitle->setObjectName("KailleraPaneTitle");
    playersHeaderLayout->addWidget(playersHeaderIcon);
    playersHeaderLayout->addWidget(playersHeaderTitle);
    playersHeaderLayout->addStretch();
    m_playersInGameCountLabel = new QLabel("(0/?)", playersHeader);
    m_playersInGameCountLabel->setObjectName("KailleraPaneMeta");
    m_playersInGameCountLabel->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    playersHeaderLayout->addWidget(m_playersInGameCountLabel, 0, Qt::AlignVCenter);
    m_btnOptions = new QPushButton(playersHeader);
    m_btnOptions->setObjectName("KailleraHeaderIconButton");
    m_btnOptions->setToolTip("Lobby options");
    m_btnOptions->setText("");
    m_btnOptions->setIcon(themedLineIcon("settings-3-line"));
    m_btnOptions->setIconSize(QSize(14, 14));
    m_btnOptions->setAutoDefault(false);
    m_btnOptions->setDefault(false);
    playersHeaderLayout->addWidget(m_btnOptions, 0, Qt::AlignVCenter);

    auto* playersBody = new QWidget(playersPane);
    auto* playersBodyLayout = new QVBoxLayout(playersBody);
    playersBodyLayout->setContentsMargins(8, 8, 8, 8);
    playersBodyLayout->setSpacing(0);

    m_playerList = new QListWidget(playersBody);
    m_playerList->setObjectName("KailleraPlayerList");
    m_playerList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_playerList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_playerList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_playerList->viewport()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_playerList->viewport(), &QWidget::customContextMenuRequested,
            this, &KailleraServerBrowserDialog::onPlayerListContextMenu);
    playersBodyLayout->addWidget(m_playerList);
    playersPaneLayout->addWidget(playersHeader);
    playersPaneLayout->addWidget(playersBody, 1);
    roomSplitter->addWidget(playersPane);

    // Buttons + stats (right)
    auto* rightWidget = new QWidget();
    rightWidget->setObjectName("KailleraSidebar");
    auto* rightVBox = new QVBoxLayout(rightWidget);
    rightVBox->setContentsMargins(10, 10, 10, 10);
    rightVBox->setSpacing(8);

    m_btnStart = new QPushButton("Start", rightWidget);
    m_btnStart->setObjectName("KailleraStartButton");
    configureServerStartButtonMetrics(m_btnStart);
    if (modern)
    {
        m_btnStart->setIcon(whiteLineIcon("play-line"));
    }
    else
    {
        m_btnStart->setIcon(themedLineIcon("play-line"));
        configureServerAccentPalette(m_btnStart);
    }
    m_btnStart->setIconSize(QSize(16, 16));
    m_btnStart->setAutoDefault(false);
    m_btnStart->setDefault(false);
    rightVBox->addWidget(m_btnStart);

    auto* utilityRow = new QHBoxLayout();
    utilityRow->setSpacing(6);
    auto* lagControl = new QWidget(rightWidget);
    auto* lagControlLayout = new QHBoxLayout(lagControl);
    lagControlLayout->setContentsMargins(0, 0, 0, 0);
    lagControlLayout->setSpacing(0);
    lagControl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_btnLagStat = new QPushButton("Lagstat", lagControl);
    m_btnLagStat->setObjectName(modern ? "KailleraSecondarySplitMain" : "KailleraSecondaryButton");
    configureServerButtonMetrics(m_btnLagStat);
    m_btnLagStat->setIcon(themedLineIcon("search-line"));
    m_btnLagStat->setIconSize(QSize(16, 16));
    m_btnLagStat->setAutoDefault(false);
    m_btnLagStat->setDefault(false);
    m_btnLagStat->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* btnLagMenu = new QToolButton(lagControl);
    btnLagMenu->setObjectName(modern ? "KailleraSecondarySplitMenu" : "KailleraSecondaryButton");
    configureServerButtonMetrics(btnLagMenu);
    btnLagMenu->setIcon(themedLineIcon("arrow-down-s-line"));
    btnLagMenu->setIconSize(QSize(14, 14));
    btnLagMenu->setToolTip("More lag tools");
    btnLagMenu->setFixedWidth(24);
    btnLagMenu->setStyleSheet(
        "QToolButton::menu-indicator {"
        "  image: none;"
        "  width: 0px;"
        "}");

    auto* lagMenu = new QMenu(btnLagMenu);
    QAction* lagResetAction = lagMenu->addAction("Lagreset");
    btnLagMenu->setMenu(lagMenu);
    btnLagMenu->setPopupMode(QToolButton::InstantPopup);

    lagControlLayout->addWidget(m_btnLagStat, 1);
    lagControlLayout->addWidget(btnLagMenu);

    m_btnAdvertise = new QPushButton("Advertise", rightWidget);
    m_btnAdvertise->setObjectName("KailleraSecondaryButton");
    configureServerButtonMetrics(m_btnAdvertise);
    m_btnAdvertise->setIcon(themedLineIcon("volume-up-line"));
    m_btnAdvertise->setIconSize(QSize(16, 16));
    m_btnAdvertise->setAutoDefault(false);
    m_btnAdvertise->setDefault(false);
    m_btnAdvertise->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    utilityRow->addWidget(lagControl, 1);
    utilityRow->addWidget(m_btnAdvertise, 1);
    rightVBox->addLayout(utilityRow);

    m_btnDrop = new QPushButton("Close Game", rightWidget);
    m_btnDrop->setObjectName("KailleraSecondaryButton");
    configureServerButtonMetrics(m_btnDrop);
    m_btnDrop->setIcon(themedLineIcon("shut-down-line"));
    m_btnDrop->setIconSize(QSize(16, 16));
    m_btnDrop->setAutoDefault(false);
    m_btnDrop->setDefault(false);
    rightVBox->addWidget(m_btnDrop);

    m_btnLeave = new QPushButton("Leave Lobby", rightWidget);
    m_btnLeave->setObjectName("KailleraSecondaryButton");
    configureServerButtonMetrics(m_btnLeave);
    m_btnLeave->setIcon(themedLineIcon("door-open-line"));
    m_btnLeave->setIconSize(QSize(16, 16));
    m_btnLeave->setAutoDefault(false);
    m_btnLeave->setDefault(false);
    rightVBox->addWidget(m_btnLeave);

    m_recordCheck = new QCheckBox("Record game", rightWidget);
    rightVBox->addWidget(m_recordCheck);

#ifdef RMGK_GAME_STATS
    // Independent .rmgr toggle - separate file, separate checkbox. Hidden
    // until m_currentGameName is known to be Smash Remix 2.0.1.
    m_replayRecordCheck = new QCheckBox("Record replay (.rmgr)", rightWidget);
    m_replayRecordCheck->setToolTip(
        "Record this match's inputs and game state to a .rmgr file, "
        "separate from the .krec above.\nSmash Remix 2.0.1 only.");
    m_replayRecordCheck->setVisible(false);
    rightVBox->addWidget(m_replayRecordCheck);
#endif

    m_fpsLabel = new QLabel("<span style='color:#8b8b8b;'>Rate</span> <span style='font-weight:600;'>0 fps / 0 pps</span>", rightWidget);
    m_fpsLabel->setObjectName("KailleraStatValue");
    m_fpsLabel->setTextFormat(Qt::RichText);
    rightVBox->addWidget(m_fpsLabel);

    rightVBox->addStretch();

    connect(m_btnStart, &QPushButton::clicked, this, &KailleraServerBrowserDialog::onStartGame);
    connect(m_btnDrop, &QPushButton::clicked, this, &KailleraServerBrowserDialog::onDropGame);
    connect(m_btnLeave, &QPushButton::clicked, this, &KailleraServerBrowserDialog::onLeaveGame);
    connect(m_btnLagStat, &QPushButton::clicked, this, &KailleraServerBrowserDialog::onLagStat);
    connect(lagResetAction, &QAction::triggered, this, [this]() {
        QByteArray cmd("/lagreset");
        kaillera_game_chat_send(cmd.data());
    });
    connect(m_btnOptions, &QPushButton::clicked, this, &KailleraServerBrowserDialog::onOptions);
    connect(m_btnAdvertise, &QPushButton::clicked, this, &KailleraServerBrowserDialog::onAdvertise);

    roomSplitter->setStretchFactor(0, 3);  // lobby chat
    roomSplitter->setStretchFactor(1, 2);  // lobby

    auto* roomRow = new QHBoxLayout();
    roomRow->setContentsMargins(0, 0, 0, 0);
    roomRow->setSpacing(8);
    roomRow->addWidget(roomSplitter, 1);
    roomRow->addWidget(rightWidget);
    layout->addLayout(roomRow);

    return widget;
}

void KailleraServerBrowserDialog::connectSignals()
{
    auto& bridge = KailleraUIBridge::instance();

    // Lobby signals
    connect(&bridge, &KailleraUIBridge::kailleraUserAdded,
            this, &KailleraServerBrowserDialog::onUserAdded);
    connect(&bridge, &KailleraUIBridge::kailleraUserJoined,
            this, &KailleraServerBrowserDialog::onUserJoined);
    connect(&bridge, &KailleraUIBridge::kailleraUserLeft,
            this, &KailleraServerBrowserDialog::onUserLeft);
    connect(&bridge, &KailleraUIBridge::kailleraGameAdded,
            this, &KailleraServerBrowserDialog::onGameAdded);
    connect(&bridge, &KailleraUIBridge::kailleraGameCreated,
            this, &KailleraServerBrowserDialog::onGameCreated);
    connect(&bridge, &KailleraUIBridge::kailleraGameClosed,
            this, &KailleraServerBrowserDialog::onGameClosed);
    connect(&bridge, &KailleraUIBridge::kailleraGameStatusChanged,
            this, &KailleraServerBrowserDialog::onGameStatusChanged);
    connect(&bridge, &KailleraUIBridge::kailleraChatReceived,
            this, &KailleraServerBrowserDialog::onChatReceived);
    connect(&bridge, &KailleraUIBridge::kailleraMotdReceived,
            this, &KailleraServerBrowserDialog::onMotdReceived);
    connect(&bridge, &KailleraUIBridge::kailleraLoginStatus,
            this, &KailleraServerBrowserDialog::onLoginStatus);
    connect(&bridge, &KailleraUIBridge::kailleraErrorMessage,
            this, &KailleraServerBrowserDialog::onError);

    // Debug messages shown in lobby chat
    connect(&bridge, &KailleraUIBridge::kailleraDebugMessage,
            this, [this](QString message) {
        m_lobbyChat->append(timestamp() + message.toHtmlEscaped());
    });

    // Game room signals
    connect(&bridge, &KailleraUIBridge::kailleraUserGameCreated,
            this, &KailleraServerBrowserDialog::onUserGameCreated);
    connect(&bridge, &KailleraUIBridge::kailleraUserGameJoined,
            this, &KailleraServerBrowserDialog::onUserGameJoined);
    connect(&bridge, &KailleraUIBridge::kailleraUserGameClosed,
            this, &KailleraServerBrowserDialog::onUserGameClosed);
    connect(&bridge, &KailleraUIBridge::kailleraPlayerAdded,
            this, &KailleraServerBrowserDialog::onPlayerAdded);
    connect(&bridge, &KailleraUIBridge::kailleraPlayerJoined,
            this, &KailleraServerBrowserDialog::onPlayerJoined);
    connect(&bridge, &KailleraUIBridge::kailleraPlayerLeft,
            this, &KailleraServerBrowserDialog::onPlayerLeft);
    connect(&bridge, &KailleraUIBridge::kailleraGameChatReceived,
            this, &KailleraServerBrowserDialog::onGameChatReceived);
    connect(&bridge, &KailleraUIBridge::kailleraUserKicked,
            this, &KailleraServerBrowserDialog::onUserKicked);
    connect(&bridge, &KailleraUIBridge::kailleraGameStarted,
            this, &KailleraServerBrowserDialog::onGameStarted);
    connect(&bridge, &KailleraUIBridge::kailleraPlayerDropped,
            this, &KailleraServerBrowserDialog::onPlayerDropped);
    connect(&bridge, &KailleraUIBridge::kailleraGameEnded,
            this, &KailleraServerBrowserDialog::onGameEnded);
    connect(&bridge, &KailleraUIBridge::kailleraNetsyncWait,
            this, &KailleraServerBrowserDialog::onNetsyncWait);

    // Wire Record checkbox to recording flag
    n02_kaillera_recording_enabled = CoreGetKailleraEffectiveRecordingDefault();
    m_recordCheck->setChecked(n02_kaillera_recording_enabled);
    connect(m_recordCheck, &QCheckBox::toggled, this, [](bool checked) {
        n02_kaillera_recording_enabled = checked;
    });

#ifdef RMGK_GAME_STATS
    // Wire Record replay checkbox to the .rmgr override, independent of the
    // krec flag above.
    m_replayRecordCheck->setChecked(CoreSettingsGetBoolValue(SettingsID::GameStats_ReplayEnabled));
    connect(m_replayRecordCheck, &QCheckBox::toggled, this, [](bool checked) {
        Replay::SetEnabledOverride(checked);
    });
#endif
}

void KailleraServerBrowserDialog::switchToLobby()
{
    m_inGameRoom = false;
    m_isHost = false;
    m_currentGameName.clear();
#ifdef RMGK_GAME_STATS
    if (m_replayRecordCheck)
        m_replayRecordCheck->setVisible(false);
#endif
    m_currentGameId = 0;
    m_roomMaxPlayers = 0;
    m_statsTimer->stop();

    // Clear player list
    m_playerList->clear();
    m_gameChat->clear();
    setRoomChatSwapView(false);
    updateHeaderCounts();

    // Switch bottom to game list and re-enable lobby create action
    showBottomGameList();
    if (m_btnCreateSwap != nullptr)
    {
        m_btnCreateSwap->setVisible(true);
        m_btnCreateSwap->setEnabled(true);
    }

    updateTitle();
}

void KailleraServerBrowserDialog::switchToGameRoom()
{
    m_inGameRoom = true;
    m_roomMaxPlayers = m_isHost ? CoreSettingsGetIntValue(SettingsID::Kaillera_MaxPlayers)
                                : detectCurrentRoomMaxPlayers();

    // Clear game room state
    m_playerList->clear();
    m_gameChat->clear();
    updateHeaderCounts();
    m_lastFrameCount = kaillera_get_frames_count();
    m_lastPPS = SOCK_SEND_PACKETS;

    // Start stats updates
    m_statsTimer->start(1000);

    // Detect non-game lobbies — Start should be disabled
    bool isNonGameLobby = (m_currentGameName == "*Away (leave messages)" ||
                           m_currentGameName == "*Chat (not game)");

    // Update button visibility based on host status
    m_btnStart->setEnabled(m_isHost && !isNonGameLobby);

    // Switch bottom to game room and reset left panel swap to chat view
    showBottomGameRoom();
    setRoomChatSwapView(false);
    if (m_btnCreateSwap != nullptr)
    {
        m_btnCreateSwap->setVisible(false);
    }

    setWindowTitle("Game Room - " + m_serverName);
}

void KailleraServerBrowserDialog::showBottomGameList()
{
    m_bottomStack->setCurrentIndex(0);
}

void KailleraServerBrowserDialog::showBottomGameRoom()
{
    m_bottomStack->setCurrentIndex(1);
}

void KailleraServerBrowserDialog::setRoomChatSwapView(bool showLobbies)
{
    m_roomShowingLobbies = showLobbies;
    if (m_roomChatStack != nullptr)
    {
        m_roomChatStack->setCurrentIndex(showLobbies ? 1 : 0);
    }
    if (m_btnSwapChat != nullptr)
    {
        m_btnSwapChat->setText(showLobbies ? "Show chat" : "Show lobbies");
        m_btnSwapChat->setToolTip(showLobbies ? "Show lobby chat" : "Show open lobbies");
    }
    if (showLobbies)
    {
        refreshRoomLobbyTable();
    }
}

void KailleraServerBrowserDialog::refreshRoomLobbyTable()
{
    if (m_roomLobbyTable == nullptr || m_gameTable == nullptr)
    {
        return;
    }

    const int previousSortColumn = m_roomLobbyTable->horizontalHeader()->sortIndicatorSection();
    const Qt::SortOrder previousSortOrder = m_roomLobbyTable->horizontalHeader()->sortIndicatorOrder();

    m_roomLobbyTable->setSortingEnabled(false);
    m_roomLobbyTable->setRowCount(0);

    const int targetColumns = qMin(m_roomLobbyTable->columnCount(), m_gameTable->columnCount());
    for (int row = 0; row < m_gameTable->rowCount(); ++row)
    {
        const int outRow = m_roomLobbyTable->rowCount();
        m_roomLobbyTable->insertRow(outRow);
        for (int col = 0; col < targetColumns; ++col)
        {
            QTableWidgetItem* sourceItem = m_gameTable->item(row, col);
            m_roomLobbyTable->setItem(outRow, col, sourceItem ? sourceItem->clone()
                                                               : new QTableWidgetItem(""));
        }
    }

    for (int col = 0; col < targetColumns; ++col)
    {
        m_roomLobbyTable->setColumnHidden(col, m_gameTable->isColumnHidden(col));
        if (col == targetColumns - 1)
        {
            continue;
        }

        const int width = m_gameTable->columnWidth(col);
        if (width > 0)
        {
            m_roomLobbyTable->setColumnWidth(col, width);
        }
    }

    m_roomLobbyTable->setHorizontalHeaderLabels({"Game", "GameID", "Emulator", "User", "Status", "Users"});

    m_roomLobbyTable->setSortingEnabled(true);
    if (previousSortColumn >= 0 && previousSortColumn < targetColumns)
    {
        m_roomLobbyTable->sortByColumn(previousSortColumn, previousSortOrder);
    }
    else
    {
        m_roomLobbyTable->sortByColumn(4, Qt::AscendingOrder);
    }
}

void KailleraServerBrowserDialog::executeOptions()
{
    if (!m_isHost)
        return;

    int maxPlayers = CoreSettingsGetIntValue(SettingsID::Kaillera_MaxPlayers);
    int maxPing = CoreSettingsGetIntValue(SettingsID::Kaillera_MaxPing);

    if (maxPlayers > 0)
    {
        QByteArray cmd = QString("/maxusers %1").arg(maxPlayers).toUtf8();
        kaillera_game_chat_send(cmd.data());
    }
    if (maxPing > 0)
    {
        QByteArray cmd = QString("/maxping %1").arg(maxPing).toUtf8();
        kaillera_game_chat_send(cmd.data());
    }
}

void KailleraServerBrowserDialog::buildGameListMenu()
{
    if (m_gameListMenu == nullptr)
    {
        return;
    }

    m_gameListMenu->clear();
    addCreateMenuEntries(m_gameListMenu);
}

void KailleraServerBrowserDialog::requestCreateGame(const QString& gameName)
{
    if (gameName.isEmpty())
    {
        return;
    }

    m_currentGameName = gameName;
    m_currentGameId = 0;
#ifdef RMGK_GAME_STATS
    if (m_replayRecordCheck)
        m_replayRecordCheck->setVisible(m_currentGameName == "SmashRemix2.0.1");
#endif
    QByteArray nameBytes = gameName.toUtf8();
    kaillera_create_game(nameBytes.data());
}

void KailleraServerBrowserDialog::addCreateMenuEntries(QMenu* parentMenu)
{
    if (parentMenu == nullptr)
    {
        return;
    }

    auto addCreateAction = [parentMenu](const QString& gameName) {
        QAction* action = parentMenu->addAction(gameName);
        action->setData(gameName);
    };

    // Add special entries (match original n02 names).
    addCreateAction("*Chat (not game)");
    addCreateAction("*Away (leave messages)");
    parentMenu->addSeparator();

    populateGameSubmenus(parentMenu);
}

void KailleraServerBrowserDialog::populateGameSubmenus(QMenu* parentMenu)
{
    if (!infos.gameList)
        return;

    // Collect all game names
    std::vector<QString> gameNames;
    const char* p = infos.gameList;
    while (*p)
    {
        gameNames.push_back(QString::fromUtf8(p));
        p += strlen(p) + 1;
    }

    // Group by first character: # for digits/symbols, A-Z for letters
    QMap<QChar, QStringList> groups;
    for (const auto& name : gameNames)
    {
        if (name.isEmpty())
            continue;
        QChar first = name.at(0).toUpper();
        QChar key = first.isLetter() ? first : QChar('#');
        groups[key].append(name);
    }

    // Add # submenu first if it exists
    if (groups.contains(QChar('#')))
    {
        QMenu* sub = parentMenu->addMenu("#");
        for (const auto& gameName : groups[QChar('#')])
        {
            QAction* action = sub->addAction(gameName);
            action->setData(gameName);
        }
    }

    // Add A-Z submenus
    for (char c = 'A'; c <= 'Z'; ++c)
    {
        QChar key(c);
        if (!groups.contains(key))
            continue;
        QMenu* sub = parentMenu->addMenu(QString(key));
        for (const auto& gameName : groups[key])
        {
            QAction* action = sub->addAction(gameName);
            action->setData(gameName);
        }
    }
}

void KailleraServerBrowserDialog::updateTitle()
{
    int users = m_userTable->rowCount();
    int games = m_gameTable->rowCount();
    setWindowTitle(QString("Connected to %1 (%2 users & %3 games)")
        .arg(m_serverName)
        .arg(users)
        .arg(games));
    updateHeaderCounts();
    if (m_roomShowingLobbies)
    {
        refreshRoomLobbyTable();
    }
}

int KailleraServerBrowserDialog::detectCurrentRoomMaxPlayers()
{
    if (m_roomMaxPlayers > 0)
    {
        return m_roomMaxPlayers;
    }

    if (m_gameTable == nullptr)
    {
        return 0;
    }

    int gameRow = -1;
    if (m_currentGameId > 0)
    {
        gameRow = findRowByValue(m_gameTable, 1, m_currentGameId);
    }
    if (gameRow < 0 && !m_currentGameName.isEmpty())
    {
        gameRow = findRowByText(m_gameTable, 0, m_currentGameName);
    }
    if (gameRow < 0)
    {
        return 0;
    }

    QTableWidgetItem* usersItem = m_gameTable->item(gameRow, 5);
    if (usersItem == nullptr)
    {
        return 0;
    }

    const QString usersText = usersItem->text().trimmed();
    const QStringList parts = usersText.split('/');
    if (parts.size() != 2)
    {
        return 0;
    }

    bool ok = false;
    int parsedMax = parts[1].trimmed().toInt(&ok);
    return (ok && parsedMax > 0) ? parsedMax : 0;
}

void KailleraServerBrowserDialog::updateHeaderCounts()
{
    if (m_connectedPlayersCountLabel != nullptr && m_userTable != nullptr)
    {
        m_connectedPlayersCountLabel->setText(QString("(%1)").arg(m_userTable->rowCount()));
    }

    if (m_playersInGameCountLabel == nullptr || m_playerList == nullptr)
    {
        return;
    }

    int currentPlayers = m_playerList->count();
    int maxPlayers = detectCurrentRoomMaxPlayers();
    if (maxPlayers > 0)
    {
        m_playersInGameCountLabel->setText(QString("(%1/%2)").arg(currentPlayers).arg(maxPlayers));
    }
    else
    {
        m_playersInGameCountLabel->setText(QString("(%1/?)").arg(currentPlayers));
    }
}

int KailleraServerBrowserDialog::findPlayerIndexById(unsigned short id) const
{
    if (m_playerList == nullptr)
    {
        return -1;
    }

    for (int row = 0; row < m_playerList->count(); ++row)
    {
        QListWidgetItem* item = m_playerList->item(row);
        if (item != nullptr && item->data(PlayerIdRole).toUInt() == static_cast<unsigned int>(id))
        {
            return row;
        }
    }

    return -1;
}

void KailleraServerBrowserDialog::refreshPlayerCards()
{
    if (m_playerList == nullptr)
    {
        return;
    }

    const QString theme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_Theme));
    const bool modern = (theme == "Modern");
    const QColor frameColor = palette().text().color();
    const bool darkTheme = palette().base().color().value() < 128;
    const QColor msColor = darkTheme ? QColor(255, 255, 255, 180) : QColor(0, 0, 0, 140);
    const QString msColorCss = QString("rgba(%1, %2, %3, %4)")
        .arg(msColor.red())
        .arg(msColor.green())
        .arg(msColor.blue())
        .arg(msColor.alpha());

    for (int row = 0; row < m_playerList->count(); ++row)
    {
        QListWidgetItem* item = m_playerList->item(row);
        if (item == nullptr)
        {
            continue;
        }

        const QString name = item->data(PlayerNameRole).toString();
        const int ping = item->data(PlayerPingRole).toInt();
        const int delay = item->data(PlayerDelayRole).toInt();
        const unsigned short playerId = static_cast<unsigned short>(item->data(PlayerIdRole).toUInt());

        auto* card = new QWidget(m_playerList);
        card->setObjectName("KailleraPlayerCard");
        const QColor portColor = playerPortColor(row);
        if (modern)
        {
            const QColor cardTint = blendColors(portColor, palette().base().color(), 16);
            card->setStyleSheet(QString(
                "QWidget#KailleraPlayerCard { border: none; border-radius: 8px; background-color: %1; }")
                .arg(cardTint.name()));
        }
        else
        {
            card->setStyleSheet(
                "QWidget#KailleraPlayerCard {"
                "  border: 1px solid palette(mid);"
                "  border-radius: 2px;"
                "  background-color: palette(base);"
                "}");
        }
        auto* cardLayout = new QHBoxLayout(card);
        cardLayout->setContentsMargins(8, 6, 8, 6);
        cardLayout->setSpacing(8);

        auto* portBadge = new QLabel(QString::number(row + 1), card);
        portBadge->setFixedSize(22, 22);
        portBadge->setAlignment(Qt::AlignCenter);
        portBadge->setStyleSheet(QString(
            "QLabel { border-radius: %2px; background-color: %1; color: white; font-weight: 700; }")
            .arg(portColor.name())
            .arg(modern ? 11 : 4));

        auto* textColumn = new QVBoxLayout();
        textColumn->setContentsMargins(0, 0, 0, 0);
        textColumn->setSpacing(1);

        auto* nameLabel = new QLabel(name, card);
        nameLabel->setObjectName("KailleraPlayerName");

        auto* statsLabel = new QLabel(card);
        statsLabel->setObjectName("KailleraPlayerStats");
        statsLabel->setTextFormat(Qt::RichText);
        statsLabel->setText(QString(
            "<span style='color:%1;'>%2 frames</span>"
            "<span style='color:%3;'> / %4 ms</span>")
            .arg(frameColor.name())
            .arg(delay)
            .arg(msColorCss)
            .arg(ping));

        textColumn->addWidget(nameLabel);
        textColumn->addWidget(statsLabel);

        cardLayout->addWidget(portBadge, 0, Qt::AlignVCenter);
        cardLayout->addLayout(textColumn, 1);
        cardLayout->addStretch();

        if (m_isHost && row > 0 && playerId > 0)
        {
            auto* kickButton = new QPushButton("X", card);
            kickButton->setObjectName("KailleraPlayerKickButton");
            kickButton->setCursor(Qt::PointingHandCursor);
            kickButton->setToolTip("Kick player");
            connect(kickButton, &QPushButton::clicked, this, [this, playerId]() {
                if (m_isHost && playerId > 0)
                {
                    kaillera_kick_user(playerId);
                }
            });
            cardLayout->addWidget(kickButton, 0, Qt::AlignVCenter);
        }

        if (QWidget* oldCard = m_playerList->itemWidget(item))
        {
            oldCard->deleteLater();
        }
        item->setSizeHint(QSize(0, 50));
        m_playerList->setItemWidget(item, card);
    }
}

QString KailleraServerBrowserDialog::timestamp(const QString& baseColor)
{
    QColor messageColor;
    if (!baseColor.isEmpty())
    {
        messageColor = QColor(baseColor);
    }
    if (!messageColor.isValid())
    {
        messageColor = palette().text().color();
    }

    const QColor backgroundColor = palette().base().color();
    const bool darkTheme = backgroundColor.value() < 128;
    QColor tsColor = darkTheme
        ? blendColors(messageColor, QColor(255, 255, 255), 72)
        : blendColors(messageColor, backgroundColor, 42);

    return QString("<span style='color:%1;'>[%2] </span>")
        .arg(tsColor.name(QColor::HexRgb), QTime::currentTime().toString("h:mm AP"));
}

QString KailleraServerBrowserDialog::linkify(const QString& text)
{
    QString html = text.toHtmlEscaped();
    // Turn URLs into clickable links
    static QRegularExpression urlRe("(https?://\\S+)");
    html.replace(urlRe, "<a href='\\1'>\\1</a>");
    return html;
}

QString KailleraServerBrowserDialog::connString(char conn)
{
    switch (conn)
    {
        case 1: return "LAN";
        case 2: return "Excellent";
        case 3: return "Good";
        case 4: return "Average";
        case 5: return "Low";
        case 6: return "Bad";
        default: return "Unknown";
    }
}

QString KailleraServerBrowserDialog::userStatusString(char status)
{
    switch (status)
    {
        case 0: return "Playing";
        case 1: return "Idle";
        case 2: return "Playing";
        default: return "Unknown";
    }
}

QString KailleraServerBrowserDialog::gameStatusString(char status)
{
    switch (status)
    {
        case 0: return "Waiting";
        case 1: return "Playing";
        case 2: return "Playing";
        default: return "Unknown";
    }
}

bool KailleraServerBrowserDialog::hasOpenSlot(QTableWidget* table, int row) const
{
    if (table == nullptr || row < 0)
    {
        return false;
    }

    QTableWidgetItem* usersItem = table->item(row, 5);
    if (usersItem == nullptr)
    {
        return false;
    }

    const QStringList parts = usersItem->text().trimmed().split('/');
    if (parts.size() != 2)
    {
        return false;
    }

    bool currentOk = false;
    bool maxOk = false;
    const int currentPlayers = parts[0].trimmed().toInt(&currentOk);
    const int maxPlayers = parts[1].trimmed().toInt(&maxOk);

    if (!currentOk || currentPlayers < 0)
    {
        return false;
    }
    if (!maxOk || maxPlayers <= 0)
    {
        return true;
    }

    return currentPlayers < maxPlayers;
}

bool KailleraServerBrowserDialog::tryJoinGameFromTable(QTableWidget* table, int row, bool leaveCurrentGame)
{
    if (table == nullptr || row < 0)
    {
        if (!leaveCurrentGame)
        {
            QMessageBox::information(this, "Join Game", "Select a game from the list to join.");
        }
        return false;
    }

    QTableWidgetItem* gameNameItem = table->item(row, 0);
    QTableWidgetItem* statusItem = table->item(row, 4);
    QTableWidgetItem* gameIdItem = table->item(row, 1);
    const QString gameName = gameNameItem ? gameNameItem->text() : "";
    const unsigned int gameId = gameIdItem ? gameIdItem->data(Qt::UserRole).toUInt() : 0;

    if (leaveCurrentGame && m_inGameRoom)
    {
        const bool isCurrentLobby = (m_currentGameId > 0 && gameId == m_currentGameId)
            || (m_currentGameId == 0 && !m_currentGameName.isEmpty() && gameName == m_currentGameName);
        if (isCurrentLobby)
        {
            return false;
        }
    }

    if (statusItem && statusItem->text() != "Waiting")
    {
        QMessageBox::warning(this, "Join Game", "Joining a running game is not allowed.");
        return false;
    }

    if (leaveCurrentGame && !hasOpenSlot(table, row))
    {
        QMessageBox::warning(this, "Join Game", "That lobby is full.");
        return false;
    }

    if (!gameName.startsWith("*") && infos.gameList)
    {
        bool found = false;
        const char* p = infos.gameList;
        while (*p)
        {
            if (gameName == QString::fromUtf8(p))
            {
                found = true;
                break;
            }
            p += strlen(p) + 1;
        }
        if (!found)
        {
            QMessageBox::warning(this, "Join Game",
                QString("The ROM '%1' is not in your list.").arg(gameName));
            return false;
        }
    }

    QTableWidgetItem* emuItem = table->item(row, 2);
    if (emuItem)
    {
        const QString remoteEmu = emuItem->text();
        const QString localEmu = QString::fromUtf8(APP);
        if (remoteEmu != localEmu)
        {
            int ret = QMessageBox::warning(this, "Version Mismatch",
                "Emulator/version mismatch and the game may desync.\nDo you want to continue?",
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (ret != QMessageBox::Yes)
            {
                return false;
            }
        }
    }

    if (gameId == 0)
    {
        return false;
    }

    if (leaveCurrentGame && m_inGameRoom)
    {
        m_pendingJoinGameName = gameName;
        m_pendingJoinGameId = gameId;
        kaillera_leave_game();
        return true;
    }

    m_currentGameName = gameName;
    m_currentGameId = gameId;
#ifdef RMGK_GAME_STATS
    if (m_replayRecordCheck)
        m_replayRecordCheck->setVisible(m_currentGameName == "SmashRemix2.0.1");
#endif
    QByteArray gameBytes = gameName.toUtf8();
    kaillera_join_game(gameId, gameBytes.constData());
    return true;
}

void KailleraServerBrowserDialog::syncCurrentGameUsersCount()
{
    if (!m_inGameRoom || m_gameTable == nullptr || m_playerList == nullptr)
    {
        return;
    }

    int row = -1;
    if (m_currentGameId > 0)
    {
        row = findRowByValue(m_gameTable, 1, m_currentGameId);
    }
    if (row < 0 && !m_currentGameName.isEmpty())
    {
        row = findRowByText(m_gameTable, 0, m_currentGameName);
    }
    if (row < 0)
    {
        return;
    }

    const int currentPlayers = m_playerList->count();
    const int maxPlayers = detectCurrentRoomMaxPlayers();
    const QString usersText = (maxPlayers > 0)
        ? QString("%1/%2").arg(currentPlayers).arg(maxPlayers)
        : QString("%1/?").arg(currentPlayers);

    m_gameTable->setItem(row, 5, new QTableWidgetItem(usersText));

    if (m_roomShowingLobbies)
    {
        refreshRoomLobbyTable();
    }
}

int KailleraServerBrowserDialog::findRowByValue(QTableWidget* table, int column, unsigned int value)
{
    for (int row = 0; row < table->rowCount(); ++row)
    {
        QTableWidgetItem* item = table->item(row, column);
        if (item && item->data(Qt::UserRole).toUInt() == value)
        {
            return row;
        }
    }
    return -1;
}

int KailleraServerBrowserDialog::findRowByText(QTableWidget* table, int column, const QString& text)
{
    for (int row = 0; row < table->rowCount(); ++row)
    {
        QTableWidgetItem* item = table->item(row, column);
        if (item && item->text() == text)
        {
            return row;
        }
    }
    return -1;
}

void KailleraServerBrowserDialog::updateUserStatus(const QString& username, const QString& status, int sortKey)
{
    int row = findRowByText(m_userTable, 0, username);
    if (row >= 0)
    {
        m_userTable->setItem(row, 3, new StatusTableWidgetItem(status, sortKey));
    }
}

void KailleraServerBrowserDialog::saveColumnWidths()
{
    // Save user table column widths as comma-separated string
    QStringList userWidths;
    for (int i = 0; i < m_userTable->columnCount(); ++i)
        userWidths << QString::number(m_userTable->columnWidth(i));
    CoreSettingsSetValue(SettingsID::Kaillera_UserColumnWidths, userWidths.join(",").toStdString());

    // Save game table column widths
    QStringList gameWidths;
    for (int i = 0; i < m_gameTable->columnCount() - 1; ++i)
        gameWidths << QString::number(m_gameTable->columnWidth(i));
    CoreSettingsSetValue(SettingsID::Kaillera_GameColumnWidths, gameWidths.join(",").toStdString());

}

void KailleraServerBrowserDialog::restoreColumnWidths()
{
    auto restoreTable = [](QTableWidget* table, const std::string& saved, bool skipLastColumn) {
        if (saved.empty()) return;
        QStringList widths = QString::fromStdString(saved).split(",");
        const int lastColumn = table->columnCount() - 1;
        for (int i = 0; i < widths.size() && i < table->columnCount(); ++i)
        {
            if (skipLastColumn && i == lastColumn)
                continue;
            int w = widths[i].toInt();
            if (w > 0)
                table->setColumnWidth(i, w);
        }
    };

    restoreTable(m_userTable, CoreSettingsGetStringValue(SettingsID::Kaillera_UserColumnWidths), false);
    restoreTable(m_gameTable, CoreSettingsGetStringValue(SettingsID::Kaillera_GameColumnWidths), true);
}

// ---- Lobby mode handlers ----

void KailleraServerBrowserDialog::onUserAdded(QString name, int ping, int status, unsigned short id, char conn)
{
    (void)conn;
    m_userTable->setSortingEnabled(false);
    int row = m_userTable->rowCount();
    m_userTable->insertRow(row);
    m_userTable->setItem(row, 0, new QTableWidgetItem(name));
    m_userTable->setItem(row, 1, new NumericTableWidgetItem(ping));
    m_userTable->setItem(row, 2, new NumericTableWidgetItem(id));
    m_userTable->setItem(row, 3, new StatusTableWidgetItem(userStatusString(static_cast<char>(status)), status));
    m_userTable->setSortingEnabled(true);
    updateTitle();
}

void KailleraServerBrowserDialog::onUserJoined(QString name, int ping, unsigned short id, char conn)
{
    (void)conn;
    m_userTable->setSortingEnabled(false);
    int row = m_userTable->rowCount();
    m_userTable->insertRow(row);
    m_userTable->setItem(row, 0, new QTableWidgetItem(name));
    m_userTable->setItem(row, 1, new NumericTableWidgetItem(ping));
    m_userTable->setItem(row, 2, new NumericTableWidgetItem(id));
    m_userTable->setItem(row, 3, new StatusTableWidgetItem("Idle", 2));
    m_userTable->setSortingEnabled(true);

    const QString color = infoMessageColor();
    m_lobbyChat->append("<span style='color:" + color + ";'>" + timestamp(color) + "* Joins: " + name.toHtmlEscaped()
        + " (Ping: " + QString::number(ping) + ")</span>");
    updateTitle();
}

void KailleraServerBrowserDialog::onUserLeft(QString name, QString quitMsg, unsigned short id)
{
    int row = findRowByValue(m_userTable, 2, id);
    if (row >= 0)
    {
        m_userTable->removeRow(row);
    }

    QString msg = "* Parts: " + name.toHtmlEscaped();
    if (!quitMsg.isEmpty())
    {
        msg += " (" + quitMsg.toHtmlEscaped() + ")";
    }
    const QString color = infoMessageColor();
    m_lobbyChat->append("<span style='color:" + color + ";'>" + timestamp(color) + msg + "</span>");
    updateTitle();
}

void KailleraServerBrowserDialog::onGameAdded(QString gameName, unsigned int id, QString emulator, QString owner, QString users, char status)
{
    m_gameTable->setSortingEnabled(false);
    int row = m_gameTable->rowCount();
    m_gameTable->insertRow(row);
    m_gameTable->setItem(row, 0, new QTableWidgetItem(gameName));
    m_gameTable->setItem(row, 1, new NumericTableWidgetItem(static_cast<int>(id)));
    m_gameTable->setItem(row, 2, new QTableWidgetItem(emulator));
    m_gameTable->setItem(row, 3, new QTableWidgetItem(owner));
    m_gameTable->setItem(row, 4, new StatusTableWidgetItem(gameStatusString(status), status));
    m_gameTable->setItem(row, 5, new QTableWidgetItem(users));
    m_gameTable->setSortingEnabled(true);
    if (m_roomShowingLobbies)
    {
        refreshRoomLobbyTable();
    }
    updateTitle();
}

void KailleraServerBrowserDialog::onGameCreated(QString gameName, unsigned int id, QString emulator, QString owner)
{
    // Deduplicate — Kaillera UDP retransmissions can deliver GAMEMAKE twice
    if (findRowByValue(m_gameTable, 1, id) >= 0)
        return;

    m_gameTable->setSortingEnabled(false);
    int row = m_gameTable->rowCount();
    m_gameTable->insertRow(row);
    m_gameTable->setItem(row, 0, new QTableWidgetItem(gameName));
    m_gameTable->setItem(row, 1, new NumericTableWidgetItem(static_cast<int>(id)));
    m_gameTable->setItem(row, 2, new QTableWidgetItem(emulator));
    m_gameTable->setItem(row, 3, new QTableWidgetItem(owner));
    m_gameTable->setItem(row, 4, new StatusTableWidgetItem("Waiting", 0));
    m_gameTable->setItem(row, 5, new QTableWidgetItem("1/?"));
    m_gameTable->setSortingEnabled(true);
    if (m_roomShowingLobbies)
    {
        refreshRoomLobbyTable();
    }

    if (m_currentGameId == 0 && gameName == m_currentGameName)
    {
        m_currentGameId = id;
    }

    // Update owner's user status to Waiting (game hasn't started yet)
    updateUserStatus(owner, "Waiting", 0);
    updateTitle();
}

void KailleraServerBrowserDialog::onGameClosed(unsigned int id)
{
    int row = findRowByValue(m_gameTable, 1, id);
    if (row >= 0)
    {
        // Set the game owner's user status back to Idle before removing the row
        QTableWidgetItem* ownerItem = m_gameTable->item(row, 3);
        if (ownerItem)
            updateUserStatus(ownerItem->text(), "Idle", 1);

        m_gameTable->removeRow(row);
    }

    // Set all game room players back to Idle
    for (int i = 0; i < m_playerList->count(); ++i)
    {
        QListWidgetItem* playerItem = m_playerList->item(i);
        if (playerItem != nullptr)
        {
            updateUserStatus(playerItem->data(PlayerNameRole).toString(), "Idle", 1);
        }
    }

    if (m_roomShowingLobbies)
    {
        refreshRoomLobbyTable();
    }
    updateTitle();
}

void KailleraServerBrowserDialog::onGameStatusChanged(unsigned int id, char status, int players, int maxPlayers)
{
    int row = findRowByValue(m_gameTable, 1, id);
    if (row >= 0)
    {
        m_gameTable->setItem(row, 4, new StatusTableWidgetItem(gameStatusString(status), status));
        m_gameTable->setItem(row, 5, new QTableWidgetItem(
            QString::number(players) + "/" + QString::number(maxPlayers)));

        QString gameStatus = gameStatusString(status);

        // Update the game owner's user status to match
        QTableWidgetItem* ownerItem = m_gameTable->item(row, 3);
        if (ownerItem)
            updateUserStatus(ownerItem->text(), gameStatus, status);

        // Update all players in the game room (if we're in this game)
        for (int i = 0; i < m_playerList->count(); ++i)
        {
            QListWidgetItem* playerItem = m_playerList->item(i);
            if (playerItem != nullptr)
            {
                updateUserStatus(playerItem->data(PlayerNameRole).toString(), gameStatus, status);
            }
        }

        QTableWidgetItem* gameNameItem = m_gameTable->item(row, 0);
        const bool isCurrentGame = (m_currentGameId > 0 && id == m_currentGameId)
            || (m_currentGameId == 0 && gameNameItem && gameNameItem->text() == m_currentGameName);
        if (m_inGameRoom && isCurrentGame && maxPlayers > 0)
        {
            m_roomMaxPlayers = maxPlayers;
            updateHeaderCounts();
            syncCurrentGameUsersCount();
        }

        if (m_roomShowingLobbies)
        {
            refreshRoomLobbyTable();
        }
    }
}

void KailleraServerBrowserDialog::onChatReceived(QString name, QString message)
{
    // Detect private messages: "TO: <user>(id): msg" or "<user>(id): msg"
    bool isPM = message.startsWith("TO: <") ||
                (message.startsWith("<") && message.contains("): "));
    if (isPM)
    {
        constexpr const char* pmColor = "lime";
        m_lobbyChat->append("<span style='color:lime;'>" + timestamp(pmColor)
            + "&lt;" + name.toHtmlEscaped() + "&gt; " + linkify(message) + "</span>");
    }
    else
    {
        m_lobbyChat->append(timestamp() + "&lt;" + name.toHtmlEscaped() + "&gt; " + linkify(message));
    }
}

void KailleraServerBrowserDialog::onMotdReceived(QString name, QString message)
{
    (void)name;
    constexpr const char* motdColor = "green";
    m_lobbyChat->append("<span style='color:green;'>" + timestamp(motdColor) + "- " + linkify(message) + "</span>");
}

void KailleraServerBrowserDialog::onLoginStatus(QString message)
{
    constexpr const char* errorColor = "red";
    m_lobbyChat->append("<span style='color:red;'>" + timestamp(errorColor) + message.toHtmlEscaped() + "</span>");
}

void KailleraServerBrowserDialog::onError(QString message)
{
    constexpr const char* errorColor = "red";
    m_lobbyChat->append("<span style='color:red;'>" + timestamp(errorColor) + "Error: " + message.toHtmlEscaped() + "</span>");
}

// ---- Game room handlers ----

void KailleraServerBrowserDialog::onUserGameCreated()
{
    m_isHost = true;
    switchToGameRoom();
    const QString color = infoMessageColor();
    m_gameChat->append("<span style='color:" + color + ";'>" + timestamp(color) + "You created a game. Waiting for players...</span>");
    executeOptions();
}

void KailleraServerBrowserDialog::onUserGameJoined()
{
    m_isHost = false;
    switchToGameRoom();
    const QString color = infoMessageColor();
    m_gameChat->append("<span style='color:" + color + ";'>" + timestamp(color) + "You joined the game. Waiting for host to start...</span>");

    QString joinMsg = QString::fromStdString(
        CoreSettingsGetStringValue(SettingsID::Kaillera_JoinMessageJoin)).trimmed();
    if (!joinMsg.isEmpty())
    {
        QByteArray msgBytes = joinMsg.toUtf8();
        kaillera_game_chat_send(msgBytes.data());
    }
}

void KailleraServerBrowserDialog::onUserGameClosed()
{
    if (m_pendingJoinGameId > 0)
    {
        switchToLobby();
        return;
    }

    constexpr const char* errorColor = "red";
    m_gameChat->append("<span style='color:red;'>" + timestamp(errorColor) + "Game has been closed.</span>");
    switchToLobby();
}

void KailleraServerBrowserDialog::onPlayerAdded(QString name, int ping, unsigned short id, char conn)
{
    // Delay calculation: (ping * 60 / 1000 / conn) + 2, then thrp * conn - 1 frames
    int c = (conn > 0) ? conn : 1;
    int thrp = (ping * 60 / 1000 / c) + 2;
    int delay = thrp * c - 1;
    int row = findPlayerIndexById(id);
    QListWidgetItem* playerItem = (row >= 0) ? m_playerList->item(row) : new QListWidgetItem();
    playerItem->setData(PlayerIdRole, static_cast<unsigned int>(id));
    playerItem->setData(PlayerNameRole, name);
    playerItem->setData(PlayerPingRole, ping);
    playerItem->setData(PlayerDelayRole, delay);
    if (row < 0)
    {
        m_playerList->addItem(playerItem);
    }
    refreshPlayerCards();
    updateHeaderCounts();
    syncCurrentGameUsersCount();
}

void KailleraServerBrowserDialog::onPlayerJoined(QString name, int ping, unsigned short uid, char conn)
{
    int c = (conn > 0) ? conn : 1;
    int thrp = (ping * 60 / 1000 / c) + 2;
    int delay = thrp * c - 1;
    int row = findPlayerIndexById(uid);
    QListWidgetItem* playerItem = (row >= 0) ? m_playerList->item(row) : new QListWidgetItem();
    playerItem->setData(PlayerIdRole, static_cast<unsigned int>(uid));
    playerItem->setData(PlayerNameRole, name);
    playerItem->setData(PlayerPingRole, ping);
    playerItem->setData(PlayerDelayRole, delay);
    if (row < 0)
    {
        m_playerList->addItem(playerItem);
    }
    refreshPlayerCards();
    updateHeaderCounts();
    syncCurrentGameUsersCount();

    const QString color = infoMessageColor();
    m_gameChat->append("<span style='color:" + color + ";'>" + timestamp(color) + "* " + name.toHtmlEscaped() + " joined the game</span>");

    // Update this user's status in the lobby user table
    updateUserStatus(name, "Waiting", 0);

    // Auto-kick players who join while game is already running (host only)
    if (m_isHost && kaillera_is_game_running())
    {
        kaillera_kick_user(uid);
        return;
    }

    // Auto-send configured host join message
    if (m_isHost)
    {
        QString joinMsg = QString::fromStdString(
            CoreSettingsGetStringValue(SettingsID::Kaillera_JoinMessageHost)).trimmed();
        if (!joinMsg.isEmpty())
        {
            QByteArray msgBytes = joinMsg.toUtf8();
            kaillera_game_chat_send(msgBytes.data());
        }
    }

    // Beep on player join
    if (CoreSettingsGetBoolValue(SettingsID::Kaillera_BeepOnJoin))
    {
        QApplication::beep();
    }

    // Flash taskbar if dialog not focused
    if (CoreSettingsGetBoolValue(SettingsID::Kaillera_FlashOnJoin) && !isActiveWindow())
    {
        QApplication::alert(this);
    }
}

void KailleraServerBrowserDialog::onPlayerLeft(QString name, unsigned short id)
{
    int row = findPlayerIndexById(id);
    if (row >= 0)
    {
        delete m_playerList->takeItem(row);
        refreshPlayerCards();
    }
    updateHeaderCounts();
    syncCurrentGameUsersCount();

    const QString color = infoMessageColor();
    m_gameChat->append("<span style='color:" + color + ";'>" + timestamp(color) + "* " + name.toHtmlEscaped() + " left the game</span>");

    // Update this user's status in the lobby user table
    updateUserStatus(name, "Idle", 1);

    if (m_pendingJoinGameId > 0 && id == n02::getUserId())
    {
        const QString pendingGameName = m_pendingJoinGameName;
        const unsigned int pendingGameId = m_pendingJoinGameId;
        m_pendingJoinGameName.clear();
        m_pendingJoinGameId = 0;
        QTimer::singleShot(0, this, [this, pendingGameName, pendingGameId]() {
            m_currentGameName = pendingGameName;
            m_currentGameId = pendingGameId;
#ifdef RMGK_GAME_STATS
            if (m_replayRecordCheck)
                m_replayRecordCheck->setVisible(m_currentGameName == "SmashRemix2.0.1");
#endif
            QByteArray gameBytes = pendingGameName.toUtf8();
            kaillera_join_game(pendingGameId, gameBytes.constData());
        });
    }
}

void KailleraServerBrowserDialog::onGameChatReceived(QString name, QString message)
{
    m_gameChat->append(timestamp() + "&lt;" + name.toHtmlEscaped() + "&gt; " + linkify(message));
}

void KailleraServerBrowserDialog::onUserKicked()
{
    m_pendingJoinGameName.clear();
    m_pendingJoinGameId = 0;
    QMessageBox::warning(this, "Kicked", "You have been kicked from the game.");
    switchToLobby();
}

void KailleraServerBrowserDialog::onPlayerDropped(QString name, int player)
{
    if (m_pendingJoinGameId > 0 && player == playerno)
    {
        return;
    }

    constexpr const char* warnColor = "orange";
    m_gameChat->append("<span style='color:orange;'>" + timestamp(warnColor)
        + "* " + name.toHtmlEscaped() + " (player " + QString::number(player) + ") has dropped</span>");

    // Host (player 1) dropping forces everyone else to drop too.
    if (player == 1 && player != playerno)
    {
        kaillera_game_drop();
    }
}

void KailleraServerBrowserDialog::onGameStarted(QString game, int player, int numPlayers)
{
#ifdef RMGK_GAME_STATS
    // Make this checkbox authoritative for the match, in case something else
    // touched the shared override since it was last set.
    if (m_replayRecordCheck)
    {
        Replay::SetEnabledOverride(m_replayRecordCheck->isChecked());
    }
#endif

    const QString color = infoMessageColor();
    m_gameChat->append("<span style='color:" + color + ";'>" + timestamp(color)
        + "* Starting: " + game.toHtmlEscaped()
        + " (" + QString::number(player) + "/" + QString::number(numPlayers) + ")</span>");
    m_gameChat->append("<span style='color:" + color + ";'>" + timestamp(color)
        + "- press \"Drop\" if emulator fails</span>");

    if (n02_kaillera_recording_enabled)
    {
        m_gameChat->append("<span style='color:" + color + ";'>" + timestamp(color)
            + "- your game will be recorded</span>");
    }

    // Populate recording_player_names from current player table
    memset(recording_player_names, 0, sizeof(recording_player_names));
    for (int i = 0; i < m_playerList->count() && i < 4; i++)
    {
        QListWidgetItem* playerItem = m_playerList->item(i);
        if (playerItem != nullptr)
        {
            QByteArray nameBytes = playerItem->data(PlayerNameRole).toString().toUtf8();
            strncpy(recording_player_names[i], nameBytes.constData(), 31);
            recording_player_names[i][31] = '\0';
        }
    }

    std::array<std::string, 4> playerNames;
    for (size_t i = 0; i < playerNames.size(); ++i)
    {
        playerNames[i] = recording_player_names[i];
    }
    OnScreenDisplaySetKailleraPortLabels(numPlayers, playerNames);
}

void KailleraServerBrowserDialog::onGameEnded()
{
    // Guard: kaillera_end_game_callback() can fire multiple times
    // (once from kaillera_game_drop(), again from server echo GAMRDROP,
    // and possibly again from GAMESHUT). Only act on the first.
    if (!CoreHasInitKaillera())
        return;

    OnScreenDisplayClearKailleraPortLabels();
    CoreMarkKailleraGameInactive();
    CoreStopEmulation();
}

void KailleraServerBrowserDialog::onNetsyncWait(int tx)
{
    int secs = tx / 1000;
    int tenths = (tx % 1000) / 100;
    m_fpsLabel->setText(QString("<span style='color:#8b8b8b;'>Sync wait</span> <span style='font-weight:600;'>%1.%2s</span>")
        .arg(secs, 3, 10, QChar('0'))
        .arg(tenths));
}

// ---- Context menus ----

void KailleraServerBrowserDialog::onUserListContextMenu(const QPoint& pos)
{
    int row = m_userTable->rowAt(pos.y());
    if (row < 0) return;

    m_userTable->selectRow(row);

    // Get user ID from UID column and name from row
    unsigned short userId = 0;
    QTableWidgetItem* uidItem = m_userTable->item(row, 2);
    if (uidItem)
    {
        userId = static_cast<unsigned short>(uidItem->data(Qt::UserRole).toUInt());
    }
    QString username = m_userTable->item(row, 0)->text();

    QMenu menu(this);
    QAction* sendMsg = menu.addAction("Send Message");
    QAction* findUser = menu.addAction("Find User");
    menu.addSeparator();
    QAction* ignoreUser = menu.addAction("Ignore");
    QAction* unignoreUser = menu.addAction("Unignore");

    QAction* chosen = menu.exec(m_userTable->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == sendMsg)
    {
        // Fill chat input with /msg <UserID> and focus it
        m_lobbyChatInput->setText(QString("/msg %1 ").arg(userId));
        m_lobbyChatInput->setFocus();
    }
    else if (chosen == findUser)
    {
        QByteArray cmd = QString("/finduser %1").arg(username).toUtf8();
        kaillera_chat_send(cmd.data());
    }
    else if (chosen == ignoreUser)
    {
        QByteArray cmd = QString("/ignore %1").arg(userId).toUtf8();
        kaillera_chat_send(cmd.data());
    }
    else if (chosen == unignoreUser)
    {
        QByteArray cmd = QString("/unignore %1").arg(userId).toUtf8();
        kaillera_chat_send(cmd.data());
    }
}

void KailleraServerBrowserDialog::onGameListContextMenu(const QPoint& pos)
{
    int row = m_gameTable->rowAt(pos.y());
    bool hasSelection = (row >= 0);

    if (hasSelection)
    {
        m_gameTable->selectRow(row);
    }

    QMenu menu(this);

    // "Create" submenu with game list
    QMenu* createSub = menu.addMenu("Create");
    addCreateMenuEntries(createSub);

    // "Join" option only if a game row is selected
    QAction* joinAction = nullptr;
    if (hasSelection)
    {
        joinAction = menu.addAction("Join");
    }

    QAction* chosen = menu.exec(m_gameTable->viewport()->mapToGlobal(pos));
    if (chosen == nullptr)
    {
        return;
    }

    if (chosen == joinAction)
    {
        onJoinGame();
        return;
    }

    const QString selectedGame = chosen->data().toString();
    if (!selectedGame.isEmpty())
    {
        requestCreateGame(selectedGame);
    }
}

void KailleraServerBrowserDialog::onPlayerListContextMenu(const QPoint& pos)
{
    int row = m_playerList->indexAt(pos).row();
    if (row < 0)
        return;

    m_playerList->setCurrentRow(row);

    QMenu menu(this);
    QAction* kickAction = menu.addAction(
        QIcon::fromTheme("user-trash", style()->standardIcon(QStyle::SP_DialogCancelButton)),
        "Kick Player");

    if (!m_isHost)
    {
        kickAction->setEnabled(false);
        kickAction->setText("Kick Player (Host only)");
    }
    else if (row == 0)
    {
        kickAction->setEnabled(false);
        kickAction->setText("Kick Player (Cannot kick host)");
    }

    QAction* chosen = menu.exec(m_playerList->viewport()->mapToGlobal(pos));
    if (chosen == kickAction && kickAction->isEnabled())
    {
        onKickPlayer();
    }
}

// ---- Button actions ----

void KailleraServerBrowserDialog::onCreateOrSwap()
{
    if (!m_inGameRoom)
    {
        // Normal mode: show game list menu to create a game
        if (m_gameListMenu == nullptr)
        {
            m_gameListMenu = new QMenu(this);
        }
        buildGameListMenu();

        const QSize menuSize = m_gameListMenu->sizeHint();
        const QPoint popupPos = m_btnCreateSwap->mapToGlobal(
            QPoint(m_btnCreateSwap->width() - menuSize.width(), -menuSize.height() + 6));

        QAction* chosen = m_gameListMenu->exec(popupPos);
        if (chosen != nullptr)
        {
            const QString selectedGame = chosen->data().toString();
            if (!selectedGame.isEmpty())
            {
                requestCreateGame(selectedGame);
            }
        }
        return;
    }

    // Game room mode: toggle only the left game-chat card between chat and lobby snapshot
    setRoomChatSwapView(!m_roomShowingLobbies);
}

void KailleraServerBrowserDialog::onJoinGame()
{
    int row = m_gameTable->currentRow();
    tryJoinGameFromTable(m_gameTable, row, false);
}

void KailleraServerBrowserDialog::onStartGame()
{
    if (m_isHost)
    {
        kaillera_start_game();
    }
}

void KailleraServerBrowserDialog::onDropGame()
{
    // Matches n02-rmg: just call kaillera_game_drop().
    // The callback chain handles the rest:
    //   kaillera_game_drop() -> kaillera_end_game_callback() ->
    //   UIBridge kailleraGameEnded -> onGameEnded() -> CoreStopEmulation()
    kaillera_game_drop();
}

void KailleraServerBrowserDialog::onLeaveGame()
{
    CoreMarkKailleraGameInactive();
    CoreStopEmulation();
    kaillera_leave_game();
    switchToLobby();
}

void KailleraServerBrowserDialog::onKickPlayer()
{
    if (!m_isHost) return;

    int row = m_playerList->currentRow();
    if (row < 0)
    {
        QMessageBox::information(this, "Kick Player", "Select a player to kick.");
        return;
    }

    // Prevent kicking self (row 0 is always the host)
    if (row == 0)
    {
        return;
    }

    // Get player ID from the Name column's UserRole data
    unsigned short playerId = 0;
    QListWidgetItem* playerItem = m_playerList->item(row);
    if (playerItem != nullptr)
    {
        playerId = static_cast<unsigned short>(playerItem->data(PlayerIdRole).toUInt());
    }

    if (playerId > 0)
    {
        kaillera_kick_user(playerId);
    }
}

void KailleraServerBrowserDialog::onLagStat()
{
    QByteArray cmd("/lagstat");
    kaillera_game_chat_send(cmd.data());
}

void KailleraServerBrowserDialog::onOptions()
{
    KailleraOptionsDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted)
    {
        if (m_isHost)
        {
            m_roomMaxPlayers = CoreSettingsGetIntValue(SettingsID::Kaillera_MaxPlayers);
            updateHeaderCounts();
        }
        executeOptions();
    }
}

void KailleraServerBrowserDialog::onAdvertise()
{
    if (!m_inGameRoom || m_currentGameName.isEmpty())
        return;

    int playerCount = m_playerList->count();
    QString ad;

    if (m_isHost)
    {
        ad = QString("%1 - %2 player(s)").arg(m_currentGameName).arg(playerCount);
    }
    else
    {
        // Get host name (first player in list, row 0)
        QString hostName;
        if (m_playerList->count() > 0 && m_playerList->item(0) != nullptr)
        {
            hostName = m_playerList->item(0)->data(PlayerNameRole).toString();
        }
        ad = QString("<%1> | %2 - %3 player(s)").arg(hostName, m_currentGameName).arg(playerCount);
    }

    QByteArray adBytes = ad.toUtf8();
    kaillera_chat_send(adBytes.data());
}

void KailleraServerBrowserDialog::onSendLobbyChat()
{
    QString text = m_lobbyChatInput->text().trimmed();
    if (text.isEmpty()) return;

    // Handle client-side commands
    if (text.startsWith("/"))
    {
        if (text == "/pcs")
        {
            m_lobbyChat->append("<span style='color:gray;'>============= Core status begin ===============</span>");
            kaillera_print_core_status();
            unsigned int sta = KSSDFA.state;
            unsigned int inp = KSSDFA.input;
            m_lobbyChat->append(QString("<span style='color:gray;'>KSSDFA { state: %1, input: %2 }</span>").arg(sta).arg(inp));
            m_lobbyChat->append(QString("<span style='color:gray;'>PACKETLOSSCOUNT=%1</span>").arg(PACKETLOSSCOUNT));
            m_lobbyChat->append(QString("<span style='color:gray;'>PACKETMISOTDERCOUNT=%1</span>").arg(PACKETMISOTDERCOUNT));
            m_lobbyChat->append("<span style='color:gray;'>============ Core status end =================</span>");
            m_lobbyChatInput->clear();
            return;
        }
        else if (text == "/swm")
        {
            // Toggle only the game-chat card between chat and lobby snapshot
            if (m_inGameRoom)
            {
                setRoomChatSwapView(!m_roomShowingLobbies);
            }
            m_lobbyChatInput->clear();
            return;
        }
        else if (text == "/clear")
        {
            m_lobbyChat->clear();
            m_lobbyChatInput->clear();
            return;
        }
        else if (text == "/stats")
        {
            m_lobbyChat->append("<span style='color:gray;'>--- Network Statistics ---</span>");
            m_lobbyChat->append(QString("<span style='color:gray;'>Recv: %1 packets, %2 bytes, %3 retransmits</span>")
                .arg(SOCK_RECV_PACKETS).arg(SOCK_RECV_BYTES).arg(SOCK_RECV_RETR));
            m_lobbyChat->append(QString("<span style='color:gray;'>Send: %1 packets, %2 bytes, %3 retransmits</span>")
                .arg(SOCK_SEND_PACKETS).arg(SOCK_SEND_BYTES).arg(SOCK_SEND_RETR));
            m_lobbyChat->append(QString("<span style='color:gray;'>Packet loss: %1, Misordered: %2</span>")
                .arg(PACKETLOSSCOUNT).arg(PACKETMISOTDERCOUNT));
            m_lobbyChatInput->clear();
            return;
        }
    }

    QByteArray textBytes = text.toUtf8();
    kaillera_chat_send(textBytes.data());
    m_lobbyChatInput->clear();
}

void KailleraServerBrowserDialog::onSendGameChat()
{
    QString text = m_gameChatInput->text().trimmed();
    if (text.isEmpty()) return;

    // Handle /halfdelay locally (toggle 30fps mode)
    if (text == "/halfdelay")
    {
        kaillera_30fps_mode = !kaillera_30fps_mode;
        m_gameChat->append(QString("<span style='color:gray;'>Half delay mode %1 (for 30fps games: Mario Kart, 1080, THPS)</span>")
            .arg(kaillera_30fps_mode ? "ENABLED" : "DISABLED"));
        m_gameChatInput->clear();
        return;
    }

    // Split long messages at 127 chars (Kaillera protocol limit)
    QByteArray textBytes = text.toUtf8();
    while (textBytes.size() > 0)
    {
        int len = qMin(textBytes.size(), 127);
        QByteArray chunk = textBytes.left(len);
        chunk.append('\0');
        kaillera_game_chat_send(chunk.data());
        textBytes = textBytes.mid(len);
    }
    m_gameChatInput->clear();
}

// ---- Stats timer ----

void KailleraServerBrowserDialog::onStatsTimer()
{
    if (!m_inGameRoom) return;

    int currentFrames = kaillera_get_frames_count();
    int fps = currentFrames - m_lastFrameCount;
    m_lastFrameCount = currentFrames;

    int currentPPS = SOCK_SEND_PACKETS;
    int pps = currentPPS - m_lastPPS;
    m_lastPPS = currentPPS;

    m_fpsLabel->setText(QString("<span style='color:#8b8b8b;'>Rate</span> <span style='font-weight:600;'>%1 fps / %2 pps</span>")
        .arg(fps)
        .arg(pps));

}

#endif // NETPLAY
