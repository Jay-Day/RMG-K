/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "UserInterface/Dialog/Netplay/NetplayCommon.hpp"
#include "UserInterface/Widget/KeybindButton.hpp"
#include "Utilities/QtMessageBox.hpp"
#include "OnScreenDisplay.hpp"
#include "SettingsDialog.hpp"
#ifdef NETPLAY
#include "Lobby/LobbyTheme.hpp"
#endif

#include <QAction>
#include <QApplication>
#include <QRegularExpressionValidator>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QColorDialog>
#include <QFontMetrics>
#include <QDirIterator>
#include <QLabel>
#include <QDir>
#include <QIcon>
#include <QList>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QTreeWidgetItemIterator>
#include <QTabBar>
#include <QWheelEvent>

#include <RMG-Core/CachedRomHeaderAndSettings.hpp>
#include <RMG-Core/Directories.hpp>
#include <RMG-Core/Emulation.hpp>
#include <RMG-Core/Settings.hpp>
#include <RMG-Core/Error.hpp>
#include <RMG-Core/Rom.hpp>
#include <RMG-Core/Kaillera.hpp>

#ifdef _WIN32
#include <windows.h>
#include <set>
#endif

using namespace UserInterface::Dialog;
using namespace Utilities;

namespace
{
class ClickToAdjustWheelFilter final : public QObject
{
public:
    explicit ClickToAdjustWheelFilter(QWidget* field)
        : QObject(field), m_field(field)
    {
        m_field->setFocusPolicy(Qt::StrongFocus);
        m_field->installEventFilter(this);
        if (auto* editor = m_field->findChild<QLineEdit*>(QString(), Qt::FindDirectChildrenOnly))
        {
            editor->installEventFilter(this);
        }
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::MouseButtonPress
            && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton)
        {
            m_clicked = true;
        }
        // Returning from a dropdown popup is part of the same click interaction.
        else if (event->type() == QEvent::FocusIn
                 && static_cast<QFocusEvent*>(event)->reason() != Qt::PopupFocusReason)
        {
            m_clicked = false;
        }
        else if (event->type() == QEvent::Wheel)
        {
            const auto* wheel = static_cast<QWheelEvent*>(event);
            const auto position = m_field->mapFromGlobal(wheel->globalPosition());
            if (!m_clicked || !m_field->hasFocus() || !QRectF(m_field->rect()).contains(position))
            {
                // Let the page's scroll area handle the wheel.
                event->ignore();
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QWidget* const m_field;
    bool m_clicked = false;
};
}

static QString getDisplayDirectoryPath(const QString& directoryPath)
{
    if (directoryPath.isEmpty())
    {
        return QString();
    }

    QString displayPath = directoryPath;
    if (!QDir(displayPath).isAbsolute())
    {
        displayPath = QDir::current().absoluteFilePath(displayPath);
    }

    return QDir::toNativeSeparators(displayPath);
}

static void setupDirectoryChangeButtonIcon(QPushButton* button)
{
    if (button == nullptr)
    {
        return;
    }

    // Match the icon sizing approach used by input-mapping icon buttons.
    button->setIcon(QIcon::fromTheme("folder-open-line"));
    button->setIconSize(QSize(20, 16));
}

static QFont settingsHeaderFont(int size)
{
#ifdef NETPLAY
    return LobbyTheme::pixelFont(size);
#else
    QFont font = QApplication::font();
    font.setPixelSize(size);
    font.setBold(true);
    return font;
#endif
}

struct ThemeOption
{
    QString label;
    QString value;
    bool legacy;
};

static QList<ThemeOption> collectAvailableThemeOptions()
{
    QList<ThemeOption> options;

    auto addOption = [&options](const QString& label, const QString& value, bool legacy)
    {
        for (const ThemeOption& existing : options)
        {
            if (existing.value == value)
            {
                return;
            }
        }

        options.append({label, value, legacy});
    };

    addOption("Modern", "Modern", false);
    addOption("Fusion", "Fusion", false);
    addOption("Fusion Dark", "Fusion Dark", false);
    addOption("Native", "Native", true);
    addOption("Fusion Warm", "Fusion Warm", true);

#ifdef _WIN32
    addOption("Windows Vista", "Windows Vista", true);
#endif

    QString directory = QString::fromStdString(CoreGetSharedDataDirectory().string());
    directory += CORE_DIR_SEPERATOR_STR;
    directory += "Styles";
    directory += CORE_DIR_SEPERATOR_STR;

    const QStringList filter{"*.qss"};
    QDirIterator stylesDirectoryIter(directory, filter, QDir::Files, QDirIterator::NoIteratorFlags);
    while (stylesDirectoryIter.hasNext())
    {
        QFileInfo fileInfo(stylesDirectoryIter.next());
        const QString value = fileInfo.fileName();
        const QString label = fileInfo.completeBaseName().isEmpty() ? value : fileInfo.completeBaseName();
        addOption(label, value, false);
    }

    return options;
}

static bool isLegacyThemeValue(const QList<ThemeOption>& options, const QString& themeValue)
{
    for (const ThemeOption& option : options)
    {
        if (option.value == themeValue)
        {
            return option.legacy;
        }
    }

    return false;
}

static QString selectedThemeValue(const QComboBox* comboBox)
{
    if (comboBox == nullptr)
    {
        return QString();
    }

    const QVariant data = comboBox->currentData();
    if (data.isValid() && !data.toString().isEmpty())
    {
        return data.toString();
    }

    return comboBox->currentText();
}

static void populateThemeCombo(QComboBox* comboBox, const QList<ThemeOption>& options, bool showLegacy, const QString& selectedValue)
{
    if (comboBox == nullptr)
    {
        return;
    }

    comboBox->clear();

    int selectedIndex = -1;
    for (const ThemeOption& option : options)
    {
        if (!showLegacy && option.legacy)
        {
            continue;
        }

        comboBox->addItem(option.label, option.value);
        if (option.value == selectedValue)
        {
            selectedIndex = comboBox->count() - 1;
        }
    }

    if (selectedIndex >= 0)
    {
        comboBox->setCurrentIndex(selectedIndex);
    }
}

//
// Local Enums
//

enum class SettingsDialogTab
{
    InterfaceGeneral    = 0,
    InterfaceEmulation  = 1,
    InterfaceRomBrowser = 2,
    InterfaceLog = 3,
    InterfaceOSD = 4,
    InterfaceNetplay = 5,
    Hotkey     = 6,
    Core       = 7,
    Game       = 8,
    GameCore   = 9,
    GamePlugin = 10,
    Plugin     = 11,
    Directory  = 12,
    N64DD      = 13,
    Rollback   = 14,
    Diagnostics = 15,
    Recording  = 16,
    NetplayOverlay = 17,
    Invalid    = 18
};


//
// Exported Functions
//

SettingsDialog::SettingsDialog(QWidget *parent, QString file) : QDialog(parent)
{
    this->setupUi(this);
    connect(this->inputPluginsComboBox, &QComboBox::activated, this, [this](int)
    {
        this->manualInputChoice = true;
    });

    this->languageComboBox->setItemData(0, "system");
    this->languageComboBox->setItemData(1, "en");
    this->languageComboBox->setItemData(2, "ja_JP");

    connect(this->showLegacyThemesCheckBox, &QCheckBox::toggled, this, [this](bool checked)
    {
        const QString currentTheme = selectedThemeValue(this->themeComboBox);
        populateThemeCombo(this->themeComboBox, collectAvailableThemeOptions(), checked, currentTheme);
    });
    connect(this->osdEnabledCheckBox, &QCheckBox::toggled, this, &SettingsDialog::updateOSDSettingsEnabledState);
    connect(this->osdChatEnabledCheckBox, &QCheckBox::toggled, this, &SettingsDialog::updateOSDSettingsEnabledState);

    setupDirectoryChangeButtonIcon(this->changeScreenShotDirButton);
    setupDirectoryChangeButtonIcon(this->changeSaveStateDirButton);
    setupDirectoryChangeButtonIcon(this->changeSaveSramDirButton);
    setupDirectoryChangeButtonIcon(this->changeKailleraRecordsDirectoryButton);

    QWidget* rollbackTab = new QWidget(this->tabWidget);
    QVBoxLayout* rollbackLayout = new QVBoxLayout(rollbackTab);
    QWidget* diagnosticsPage = new QWidget(this);
    QVBoxLayout* diagnosticsLayout = new QVBoxLayout(diagnosticsPage);
    auto* loggingHeading = new QLabel(tr("Logging"), diagnosticsPage);
    loggingHeading->setProperty("settingsHeadingOffset", 0);
    this->rollbackEnableLocalTestingCheckBox = new QCheckBox(tr("Use rollback engine for local play"), diagnosticsPage);
    this->rollbackVerboseStatsCheckBox = new QCheckBox(tr("Verbose rollback stats"), diagnosticsPage);
    this->rollbackStallDiagnosticsCheckBox = new QCheckBox(tr("Log netplay stalls"), diagnosticsPage);
    this->rollbackStallDiagnosticsCheckBox->setToolTip(
        tr("Records per-player network stats during freezes only."));
    this->rollbackPacingTraceCheckBox = new QCheckBox(tr("Frame pacing traces"), diagnosticsPage);
    this->rollbackEmulationDiagnosticsCheckBox = new QCheckBox(tr("Emulation freeze diagnostics"), diagnosticsPage);
    this->rollbackEmulationDiagnosticsCheckBox->setToolTip(
        tr("Logs EEPROM, rollback and frame state. Enable on every player before reproducing a freeze."));
    this->rollbackPacingTraceCheckBox->setToolTip(
        tr("Writes pacing CSV files when the session ends."));
    this->rollbackVerbosePifInputLoggingCheckBox = new QCheckBox(tr("Verbose PIF input"), diagnosticsPage);
    this->rollbackVerboseGlideInputLoggingCheckBox = new QCheckBox(tr("Verbose Glide input"), diagnosticsPage);
    this->rollbackPingDiagnosticsCheckBox = new QCheckBox(tr("Lobby connection diagnostics"), diagnosticsPage);
    this->rollbackPingDiagnosticsCheckBox->setToolTip(
        tr("Logs connection setup and peer pings, including IP addresses. Applies on your next lobby connection."));
    this->rollbackHideLocationCheckBox = new QCheckBox(tr("Hide my country in the lobby"), rollbackTab);
    this->rollbackHideLocationCheckBox->setToolTip(
        tr("Hides your country flag and name after reconnecting. Players in your game still exchange IP addresses."));
    this->rollbackRememberInputDelayCheckBox = new QCheckBox(tr("Remember my last input delay when joining a room"), rollbackTab);
    this->autoStartNetplayOnStartupCheckBox->setText(tr("Open netplay on startup"));
    rollbackLayout->addWidget(this->autoStartNetplayOnStartupCheckBox);
    rollbackLayout->addWidget(this->rollbackRememberInputDelayCheckBox);
    rollbackLayout->addWidget(this->rollbackHideLocationCheckBox);
    diagnosticsLayout->addWidget(this->rollbackEnableLocalTestingCheckBox);
    diagnosticsLayout->addWidget(loggingHeading);
    diagnosticsLayout->addWidget(this->rollbackVerboseStatsCheckBox);
    diagnosticsLayout->addWidget(this->rollbackStallDiagnosticsCheckBox);
    diagnosticsLayout->addWidget(this->rollbackEmulationDiagnosticsCheckBox);
    diagnosticsLayout->addWidget(this->rollbackPacingTraceCheckBox);
    diagnosticsLayout->addWidget(this->rollbackVerbosePifInputLoggingCheckBox);
    diagnosticsLayout->addWidget(this->rollbackVerboseGlideInputLoggingCheckBox);
    diagnosticsLayout->addWidget(this->rollbackPingDiagnosticsCheckBox);
    diagnosticsLayout->addStretch();
    rollbackLayout->addStretch();
    this->tabWidget->addTab(rollbackTab, tr("Rollback"));

    this->setIconsForEmulationInfoText();

    // if ROM is open, we should retrieve the current settings,
    // if it's not opened but we got a filename, try to 
    // retrieve cached entries
    if (CoreHasRomOpen())
    {
        std::filesystem::path romPath;
        // only show game tab once retrieving info succeeds
        this->showGameSettings = CoreGetRomPath(romPath) && 
                                    CoreGetRomType(this->currentGameType) &&
                                    CoreGetCurrentRomHeader(this->currentGameHeader) &&
                                    CoreGetCurrentRomSettings(this->currentGameSettings) && 
                                    CoreGetCurrentDefaultRomSettings(this->defaultGameSettings);
        this->currentGameFile = QString::fromStdU32String(romPath.u32string());
    }
    else if (!file.isEmpty())
    {
        // only show game tab once retrieving cached entry succeeds
        this->showGameSettings = CoreGetCachedRomHeaderAndSettings(file.toStdU32String(), &this->currentGameType, 
                                    &this->currentGameHeader, &this->defaultGameSettings, &this->currentGameSettings);
        if (this->showGameSettings)
        {
            this->currentGameFile = file;
        }
    }

    if (this->showGameSettings)
    {
        this->gameSection = this->currentGameSettings.MD5;

        // no need to show emulation info text,
        // when we're not running/paused
        if (!CoreIsEmulationRunning() && !CoreIsEmulationPaused())
        {
            this->hideEmulationInfoText();
        }
    }
    else
    {
        this->hideEmulationInfoText();
        this->tabWidget->setTabEnabled(3, false);
    }

    pluginList = CoreGetAllPlugins();

    for (int i = 0; i < static_cast<int>(SettingsDialogTab::Invalid); i++)
    {
        this->loadSettings(i);
    }

    // connect hotkey settings to slot
    this->commonHotkeySettings(SettingsDialogAction::ConnectSignals);

#ifndef UPDATER
    this->checkForUpdatesCheckBox->setHidden(true);
#endif // !UPDATER

#ifndef NETPLAY
    this->autoStartNetplayOnStartupCheckBox->setHidden(true);
#endif // !NETPLAY

#ifdef _WIN32
    connect(this->exclusiveFullscreenCheckBox, &QCheckBox::toggled,
            this, &SettingsDialog::setExclusiveFullscreenControlsEnabled);
    connect(this->betaFullscreenBackendCheckBox, &QCheckBox::toggled, this, [this](bool checked)
    {
        if (checked)
        {
            this->automaticFullscreenCheckbox->setChecked(true);
        }
    });
    connect(this->exclusiveMonitorComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int)
    {
        this->populateExclusiveFullscreenModes();
    });
    connect(this->exclusiveResolutionComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int)
    {
        this->populateExclusiveFullscreenModes();
    });
#else
    this->exclusiveFullscreenSection->setVisible(false);
#endif
    this->infoIconLabel_7->hide();
    this->label_91->hide();
    this->setupNavigation(rollbackTab, diagnosticsPage);
}

SettingsDialog::~SettingsDialog(void)
{
}

void SettingsDialog::SetControllerConfigAction(QAction* action)
{
    controllerConfigButton->setEnabled(action->isEnabled());
    connect(action, &QAction::enabledChanged, controllerConfigButton, &QWidget::setEnabled);
    connect(controllerConfigButton, &QPushButton::clicked, action, [this, action]()
    {
        const auto previousPlugin = CoreSettingsGetStringValue(SettingsID::Core_INPUT_Plugin);
        action->trigger();
        const auto currentPlugin = CoreSettingsGetStringValue(SettingsID::Core_INPUT_Plugin);
        if (currentPlugin == previousPlugin)
        {
            return;
        }

        // The input dialog applies its plugin selection before Settings is saved.
        const auto fileName = QFileInfo(QString::fromStdString(currentPlugin)).fileName();
        int index = inputPluginsComboBox->findData(fileName);
        if (index < 0)
        {
            inputPluginsComboBox->addItem(getInputPluginDisplayName(fileName, fileName), fileName);
            index = inputPluginsComboBox->count() - 1;
        }
        inputPluginsComboBox->setCurrentIndex(index);
        manualInputChoice = false;
    });
}

void SettingsDialog::setupNavigation(QWidget* rollbackPage, QWidget* diagnosticsPage)
{
    // Keep the existing controls and their persistence bindings, but give each
    // settings scope a stable ID independent of its position in the sidebar.
    auto* shell = new QWidget(this);
    auto* shellLayout = new QHBoxLayout(shell);
    shellLayout->setContentsMargins(0, 0, 0, 0);
    shellLayout->setSpacing(16);
    auto* sidebar = new QFrame(shell);
    sidebar->setObjectName("SettingsSidebar");
    sidebar->setFixedWidth(240);
    auto* sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(12, 12, 12, 12);
    sidebarLayout->setSpacing(12);
    settingsSearch = new QLineEdit(sidebar);
    settingsSearch->setObjectName("SettingsSearch");
    settingsSearch->setPlaceholderText(tr("Find a setting…"));
    settingsSearch->setAccessibleName(tr("Find a setting"));
    settingsSearch->setToolTip(tr("Ctrl+F"));
    settingsSearch->setClearButtonEnabled(true);
    sidebarLayout->addWidget(settingsSearch);
    settingsNavigation = new QTreeWidget(sidebar);
    settingsNavigation->setObjectName("SettingsNavigation");
    settingsNavigation->setHeaderHidden(true);
    settingsNavigation->setRootIsDecorated(false);
    settingsNavigation->setIndentation(0);
    settingsNavigation->setExpandsOnDoubleClick(false);
    settingsNavigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    settingsNavigation->setAccessibleName(tr("Settings categories"));
    sidebarLayout->addWidget(settingsNavigation, 1);
    controllerConfigButton = new QPushButton(QIcon::fromTheme("gamepad-line"), tr("Controller Config"), sidebar);
    controllerConfigButton->setObjectName("SettingsControllerConfig");
    controllerConfigButton->setIconSize(QSize(16, 16));
    controllerConfigButton->setAutoDefault(false);
    controllerConfigButton->setEnabled(false);
    sidebarLayout->addWidget(controllerConfigButton);
    shellLayout->addWidget(sidebar);
    settingsPages = new QStackedWidget(shell);
    settingsPages->setObjectName("SettingsPages");
    shellLayout->addWidget(settingsPages, 1);

    auto section = [this](const QString& title)
    {
        auto* item = new QTreeWidgetItem(settingsNavigation, {title});
        item->setFlags(Qt::ItemIsEnabled);
        item->setData(0, Qt::UserRole, static_cast<int>(SettingsDialogTab::Invalid));
        item->setExpanded(true);
        return item;
    };
    auto page = [this](QTreeWidgetItem* group, SettingsDialogTab id, QWidget* controls,
                       const QString& title, std::initializer_list<SettingsDialogTab> extraScopes = {},
                       const QString& note = QString())
    {
        auto* scroll = new QScrollArea(settingsPages);
        scroll->setObjectName("SettingsPageScroll");
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* content = new QWidget;
        content->setObjectName("SettingsPage");
        auto* layout = new QVBoxLayout(content);
        layout->setContentsMargins(24, 24, 24, 24);
        layout->setSpacing(18);
        auto* heading = new QLabel(title, content);
        heading->setObjectName("SettingsPageTitle");
        heading->setProperty("settingsHeadingOffset", 12);
        heading->setWordWrap(true);
        layout->addWidget(heading);
        if (!note.isEmpty())
        {
            auto* summary = new QLabel(note, content);
            summary->setWordWrap(true);
            summary->setProperty("role", "muted");
            layout->addWidget(summary);
        }
        controls->setParent(content);
        controls->setEnabled(true);
        if (controls->layout())
        {
            controls->layout()->setContentsMargins(0, 0, 0, 0);
            if (controls->layout()->spacing() < 12) controls->layout()->setSpacing(12);
        }
        const bool shortcuts = id == SettingsDialogTab::Hotkey;
        // Let the page own its unused space so notes stay next to their controls.
        for (auto* formLayout : controls->findChildren<QLayout*>())
        {
            for (int i = formLayout->count() - 1; i >= 0; --i)
            {
                auto* spacer = formLayout->itemAt(i)->spacerItem();
                if (spacer && spacer->expandingDirections().testFlag(Qt::Vertical)
                    && (!shortcuts || spacer->sizeHint().height() > 0))
                    delete formLayout->takeAt(i);
            }
        }
        layout->addWidget(controls, shortcuts ? 1 : 0);
        if (!shortcuts) layout->addStretch();
        controls->show();
        scroll->setWidget(content);
        auto* item = new QTreeWidgetItem(group, {title});
        item->setData(0, Qt::UserRole, static_cast<int>(id));
        item->setData(0, Qt::UserRole + 1, settingsPages->addWidget(scroll));
        QVariantList resetScopes;
        for (const auto scope : extraScopes) resetScopes.append(static_cast<int>(scope));
        item->setData(0, Qt::UserRole + 3, resetScopes);

        // Search labels and help text, never the user's paths or entered values.
        QStringList terms{title, note, group->text(0)};
        for (auto* widget : controls->findChildren<QWidget*>())
        {
            if (auto* label = qobject_cast<QLabel*>(widget)) terms << label->text();
            if (auto* button = qobject_cast<QAbstractButton*>(widget)) terms << button->text();
            if (auto* box = qobject_cast<QGroupBox*>(widget)) terms << box->title();
            terms << widget->toolTip();
        }
        item->setData(0, Qt::UserRole + 2, terms.join(' '));
    };

    // Compose pages by task rather than by the old tab or config section.
    auto combine = [this](std::initializer_list<std::pair<QString, QWidget*>> sections)
    {
        auto* content = new QWidget(this);
        auto* layout = new QVBoxLayout(content);
        layout->setSpacing(24);
        for (const auto& [title, form] : sections)
        {
            auto* section = new QVBoxLayout;
            section->setSpacing(10);
            if (!title.isEmpty())
            {
                auto* heading = new QLabel(title, content);
                heading->setProperty("settingsHeadingOffset", 1);
                section->addWidget(heading);
            }
            form->setParent(content);
            if (form->layout())
            {
                form->layout()->setContentsMargins(0, 0, 0, 0);
                form->layout()->setSpacing(12);
            }
            section->addWidget(form);
            layout->addLayout(section);
            form->show();
        }
        return content;
    };

    // Capture before reparenting: taking a page out changes QTabWidget indices.
    QWidget* general = innerInterfaceTabWidget->widget(0);
    QWidget* display = innerInterfaceTabWidget->widget(1);
    QWidget* library = innerInterfaceTabWidget->widget(2);
    QWidget* logs = innerInterfaceTabWidget->widget(3);
    QWidget* overlay = innerInterfaceTabWidget->widget(4);
    // These controls also apply to rollback despite their historical names.
    osdChatEnabledCheckBox->setText(tr("Show chat on-screen"));
    osdKailleraPortLabelsCheckBox->setText(tr("Show player labels"));
    kailleraRecordByDefaultCheckBox->setText(tr("Record matches by default"));
    kailleraRecordsFolderLabel->setText(tr("Replay folder"));

    // General application behavior is independent of the game display.
    auto* statusDuration = new QWidget(this);
    auto* statusLayout = new QHBoxLayout(statusDuration);
    statusLayout->setContentsMargins(0, 0, 0, 0);
    label->setWordWrap(true);
    statusLayout->addWidget(label);
    statusLayout->addWidget(statusBarMessageDurationSpinBox);
    auto* generalLayout = qobject_cast<QVBoxLayout*>(general->layout());
    int behaviorIndex = generalLayout->indexOf(checkForUpdatesCheckBox) + 1;
    generalLayout->insertWidget(behaviorIndex++, confirmExitWhileInGameCheckBox);
    generalLayout->insertWidget(behaviorIndex, statusDuration);

    // Rendering API belongs with video plugin selection, not window behavior.
    auto* renderingApi = new QHBoxLayout;
    renderingApi->addWidget(label_6);
    renderingApi->addWidget(openglTypeComboBox);
    videoPluginSectionLayout->addLayout(renderingApi);
    for (auto* form : {pluginsTab, innerGamePluginsTab})
    {
        for (auto* combo : form->findChildren<QComboBox*>())
        {
            combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            combo->setMinimumContentsLength(12);
            connect(combo, &QComboBox::currentTextChanged, combo, &QComboBox::setToolTip);
            combo->setToolTip(combo->currentText());
        }
    }

    auto* saveNames = new QWidget(this);
    auto* saveNamesLayout = new QHBoxLayout(saveNames);
    saveNamesLayout->setContentsMargins(0, 0, 0, 0);
    saveNamesLayout->addWidget(label_25);
    saveNamesLayout->addWidget(coreSaveFilenameFormatComboBox);
    qobject_cast<QVBoxLayout*>(directoriesTab->layout())->insertWidget(0, saveNames);

    setupShortcutsPage();

    auto* everyday = section(QString());
#ifdef NETPLAY
    auto* netplayOverlay = new QWidget(this);
    auto* netplayOverlayLayout = new QVBoxLayout(netplayOverlay);
    netplayOverlayLayout->addWidget(osdKailleraPortLabelsCheckBox);
    netplayOverlayLayout->addWidget(osdLiveReplayStatusCheckBox);
    netplayOverlayLayout->addWidget(osdChatEnabledCheckBox);
    netplayOverlayLayout->addWidget(osdChatSettings);
    page(everyday, SettingsDialogTab::Rollback,
        combine({{QString(), rollbackPage}, {QString(), replaySettings},
                 {tr("In-game overlay"), netplayOverlay}}), tr("Netplay"),
        {SettingsDialogTab::NetplayOverlay, SettingsDialogTab::Recording});
#endif
    page(everyday, SettingsDialogTab::InterfaceEmulation,
        combine({{tr("Window"), display}, {tr("Overlay"), overlay}}), tr("Display"),
        {SettingsDialogTab::InterfaceOSD});
    page(everyday, SettingsDialogTab::Hotkey, hotkeysTab, tr("Shortcuts"));
    page(everyday, SettingsDialogTab::Directory,
        combine({{tr("ROM search"), library}, {tr("Save files"), directoriesTab}}),
        tr("Games & saves"), {SettingsDialogTab::InterfaceRomBrowser});
    page(everyday, SettingsDialogTab::InterfaceGeneral, general, tr("Application"));
    if (showGameSettings)
    {
        auto* game = section(tr("This game"));
        page(game, SettingsDialogTab::Game, innerGameTab, tr("Game settings"));
        page(game, SettingsDialogTab::GameCore, innerGameCoreTab, tr("Game emulation"));
        page(game, SettingsDialogTab::GamePlugin, innerGamePluginsTab, tr("Game plugins"));
    }
    auto* advanced = section(tr("Advanced"));
    page(advanced, SettingsDialogTab::Plugin, pluginsTab, tr("Plugins"));
    page(advanced, SettingsDialogTab::Core, coreTab, tr("Emulation core"));
#ifdef NETPLAY
    page(advanced, SettingsDialogTab::Diagnostics,
        combine({{tr("Application log"), logs}, {tr("Netplay"), diagnosticsPage}}),
        tr("Diagnostics"), {SettingsDialogTab::InterfaceLog}, tr("Detailed logging can affect performance."));
    auto* legacy = section(tr("Legacy"));
    page(legacy, SettingsDialogTab::N64DD, DDTab, tr("Kaillera"));
#else
    page(advanced, SettingsDialogTab::InterfaceLog, logs, tr("Diagnostics"));
    rollbackPage->hide();
    diagnosticsPage->hide();
#endif

    auto* empty = new QLabel(tr("No matching settings"), settingsPages);
    empty->setObjectName("SettingsEmpty");
    empty->setWordWrap(true);
    empty->setAlignment(Qt::AlignCenter);
    const int emptyIndex = settingsPages->addWidget(empty);
    connect(settingsNavigation, &QTreeWidget::currentItemChanged, this,
        [this](QTreeWidgetItem* item, QTreeWidgetItem*)
    {
        if (!item || !item->parent()) return;
        settingsPages->setCurrentIndex(item->data(0, Qt::UserRole + 1).toInt());
        buttonBox->button(QDialogButtonBox::RestoreDefaults)->setEnabled(true);
    });
    connect(settingsSearch, &QLineEdit::textChanged, this, [this, emptyIndex](const QString& text)
    {
        const auto words = text.simplified().split(' ', Qt::SkipEmptyParts);
        QTreeWidgetItem* first = nullptr;
        for (int i = 0; i < settingsNavigation->topLevelItemCount(); ++i)
        {
            auto* group = settingsNavigation->topLevelItem(i);
            bool any = false;
            for (int j = 0; j < group->childCount(); ++j)
            {
                auto* item = group->child(j);
                bool match = true;
                const auto terms = item->data(0, Qt::UserRole + 2).toString();
                for (const auto& word : words) match &= terms.contains(word, Qt::CaseInsensitive);
                item->setHidden(!match);
                any |= match;
                if (match && !item->isDisabled() && !first) first = item;
            }
            group->setHidden(!any);
            group->setExpanded(true);
        }
        auto* current = settingsNavigation->currentItem();
        if (!first)
        {
            settingsNavigation->setCurrentItem(nullptr);
            settingsPages->setCurrentIndex(emptyIndex);
            buttonBox->button(QDialogButtonBox::RestoreDefaults)->setEnabled(false);
        }
        else if (!current || current->isHidden()) settingsNavigation->setCurrentItem(first);
    });
    auto* find = new QShortcut(QKeySequence::Find, this);
    connect(find, &QShortcut::activated, settingsSearch, [this]() { settingsSearch->setFocus(); settingsSearch->selectAll(); });
    // Enter in the filter moves to the results instead of submitting the dialog.
    for (const auto key : {Qt::Key_Return, Qt::Key_Enter})
    {
        auto* browseResults = new QShortcut(QKeySequence(key), settingsSearch);
        browseResults->setContext(Qt::WidgetShortcut);
        connect(browseResults, &QShortcut::activated, settingsNavigation, [this]() { settingsNavigation->setFocus(); });
    }
    horizontalLayout->removeWidget(tabWidget);
    tabWidget->hide();
    horizontalLayout->addWidget(shell);
    gridLayout->setContentsMargins(16, 16, 16, 12);
    verticalLayout->setSpacing(12);
    buttonBox->button(QDialogButtonBox::Ok)->setText(tr("Save"));
    buttonBox->button(QDialogButtonBox::Ok)->setObjectName("SaveSettings");
    buttonBox->button(QDialogButtonBox::Ok)->setDefault(true);
    buttonBox->button(QDialogButtonBox::RestoreDefaults)->setAutoDefault(false);
    buttonBox->button(QDialogButtonBox::RestoreDefaults)->setText(tr("Reset page"));
    buttonBox->button(QDialogButtonBox::RestoreDefaults)->setToolTip(tr("Restore defaults for this page."));

    // Share the lobby palette. Non-netplay builds retain the application palette.
    QColor bg = palette().color(QPalette::Window), panel = palette().color(QPalette::Base);
    QColor ink = palette().color(QPalette::WindowText), muted = palette().color(QPalette::Disabled, QPalette::WindowText);
    QColor line = palette().color(QPalette::Mid), accent = palette().color(QPalette::Highlight);
    QColor tint = palette().color(QPalette::AlternateBase), well = panel;
    QColor shortcutSurface = bg, stepperPressed = bg;
    QString style;
#ifdef NETPLAY
    const auto& t = LobbyTheme::tokens();
    bg = t.bg; panel = t.panel; ink = t.ink; muted = t.muted;
    line = t.line2; accent = t.accent; tint = t.accentTint; well = t.well;
    shortcutSurface = t.panelB;
    stepperPressed = t.raise;
    style = LobbyTheme::styleSheet();
#endif
    style += QStringLiteral(R"(
        QDialog#SettingsDialog { background: @bg; }
        QFrame#SettingsSidebar { background: @bg; border: 1px solid @line; border-radius: 12px; }
        QWidget#SettingsPage, QLabel#SettingsEmpty { background: @panel; color: @ink; border-radius: 12px; }
        QLabel { color: @ink; }
        QLabel[role="muted"] { color: @muted; }
        QTreeWidget#SettingsNavigation { background: transparent; border: none; outline: 0; color: @muted; }
        QTreeWidget#SettingsNavigation::item { padding: 0 8px; border: 2px solid transparent; border-radius: 7px; }
        QTreeWidget#SettingsNavigation::item:hover { background: @tint; color: @ink; }
        QTreeWidget#SettingsNavigation::item:has-children { padding-left: 0; color: @ink; }
        QTreeWidget#SettingsNavigation::item:has-children:hover { background: transparent; }
        QTreeWidget#SettingsNavigation::item:selected { background: @tint; color: @ink; border-left-color: @accent; }
        QTreeWidget#SettingsNavigation::item:disabled { color: @muted; }
        QScrollArea { border: none; background: transparent; }
        QScrollArea#SettingsPageScroll QScrollBar:horizontal { height: 10px; background: transparent; }
        QScrollArea#SettingsPageScroll QScrollBar::handle:horizontal { background: @line; border-radius: 4px; min-width: 24px; }
        QScrollArea#SettingsPageScroll QScrollBar::add-line:horizontal,
        QScrollArea#SettingsPageScroll QScrollBar::sub-line:horizontal { width: 0; }
        QScrollArea#SettingsPageScroll QScrollBar::add-page:horizontal,
        QScrollArea#SettingsPageScroll QScrollBar::sub-page:horizontal { background: transparent; }
        QGroupBox { border: none; margin: 0; padding: 0; color: @ink; }
        QGroupBox[checkable="true"] {
            border: 1px solid @line; border-radius: 9px;
            margin-top: 16px; padding: 20px 14px 14px;
        }
        QGroupBox[checkable="true"]::title {
            subcontrol-origin: margin; subcontrol-position: top left;
            left: 12px; padding: 3px 6px; spacing: 8px; color: @ink;
        }
        QGroupBox::indicator { width: 18px; height: 18px; border: 1px solid @line; border-radius: 4px; background: @well; }
        QGroupBox::indicator:checked { background: @accent; border-color: @accent; image: url(:/icons/white/svg/check-line.svg); }
        QGroupBox::indicator:hover { border-color: @accent; }
        QCheckBox { color: @ink; spacing: 8px; padding: 4px 0; }
        QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid @line; border-radius: 4px; background: @well; }
        QCheckBox::indicator:checked { background: @accent; border-color: @accent; image: url(:/icons/white/svg/check-line.svg); }
        QCheckBox::indicator:hover { border-color: @accent; }
        QCheckBox::indicator:disabled { background: @bg; border-color: @line; }
        QCheckBox:disabled, QLabel:disabled, QGroupBox:disabled { color: @muted; }
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { background: @well; color: @ink; border: 1px solid @line; border-radius: 6px; padding: 6px 10px; min-height: 24px; }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color: @accent; }
        QComboBox { padding-right: 24px; }
        QComboBox::drop-down { border: none; width: 24px; }
        QComboBox::down-arrow { image: url(@chevron); width: 12px; height: 12px; }
        QComboBox:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled { color: @muted; }
        QSpinBox, QDoubleSpinBox { padding-right: 38px; }
        QSpinBox::up-button, QSpinBox::down-button,
        QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {
            subcontrol-origin: padding;
            position: relative; right: 5px;
            width: 26px; height: 16px;
            border: none; border-radius: 4px; background: transparent;
        }
        QSpinBox::up-button, QDoubleSpinBox::up-button { subcontrol-position: top right; top: 1px; }
        QSpinBox::down-button, QDoubleSpinBox::down-button { subcontrol-position: bottom right; bottom: 1px; }
        QSpinBox::up-button:hover:enabled, QSpinBox::down-button:hover:enabled,
        QDoubleSpinBox::up-button:hover:enabled, QDoubleSpinBox::down-button:hover:enabled { background: @tint; }
        QSpinBox::up-button:pressed:enabled, QSpinBox::down-button:pressed:enabled,
        QDoubleSpinBox::up-button:pressed:enabled, QDoubleSpinBox::down-button:pressed:enabled { background: @stepperPressed; }
        QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image: url(@chevronUp); width: 16px; height: 16px; }
        QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image: url(@chevron); width: 16px; height: 16px; }
        QPushButton { background: @bg; color: @ink; border: 1px solid @line; border-radius: 6px; padding: 6px 12px; min-height: 24px; }
        QPushButton:hover, QPushButton:focus { border-color: @accent; }
        QPushButton:pressed { background: @tint; }
        QPushButton:disabled { color: @muted; }
        QPushButton#SettingsControllerConfig { background: @panel; }
        QPushButton#SettingsControllerConfig:hover:enabled,
        QPushButton#SettingsControllerConfig:pressed { background: @tint; }
        QPushButton#SaveSettings { background: @accent; color: white; border-color: @accent; font-weight: 600; padding: 7px 22px; }
        QPushButton#SaveSettings:focus, QPushButton#SaveSettings:hover { border-color: @ink; }
        QTabWidget#keybindingsMenuTabWidget::pane {
            top: -1px; border: none; border-radius: 9px;
            border-top-left-radius: 0; padding: 3px; background: @shortcutSurface;
        }
        QWidget[settingsShortcutPage="true"] { background: @shortcutSurface; }
        QTabBar#SettingsShortcutTabs { background: transparent; qproperty-drawBase: 0; }
        QTabBar#SettingsShortcutTabs::tab {
            height: 34px; padding: 0 10px; margin-right: 2px;
            color: @muted; background: transparent; border: 1px solid transparent;
            border-top-left-radius: 7px; border-top-right-radius: 7px;
        }
        QTabBar#SettingsShortcutTabs::tab:!selected:hover { color: @ink; background: @tint; }
        QTabBar#SettingsShortcutTabs::tab:selected {
            color: @ink; background: @shortcutSurface; border-top: 2px solid @accent;
        }
        QTabBar#SettingsShortcutTabs::scroller { width: 48px; }
        QTabBar#SettingsShortcutTabs QToolButton {
            color: @ink; background: @shortcutSurface; border: 1px solid @line; border-radius: 5px;
        }
        QTabBar#SettingsShortcutTabs QToolButton:hover { background: @tint; }
        QTabBar#SettingsShortcutTabs QToolButton:disabled { color: @muted; }
        QPushButton[shortcutBinding="true"] { background: @well; font-weight: 600; }
    )");
    const std::pair<const char*, QColor> colors[] = {
        {"@bg", bg}, {"@panel", panel}, {"@ink", ink}, {"@muted", muted},
        {"@line", line}, {"@accent", accent}, {"@tint", tint}, {"@well", well},
        {"@shortcutSurface", shortcutSurface}, {"@stepperPressed", stepperPressed}
    };
    for (const auto& [name, color] : colors)
        style.replace(QLatin1String(name), color.name(QColor::HexArgb));
    style.replace(QStringLiteral("@chevronUp"), bg.lightness() < 128
        ? QStringLiteral(":/icons/white/svg/arrow-up-s-line.svg")
        : QStringLiteral(":/icons/black/svg/arrow-up-s-line.svg"));
    style.replace(QStringLiteral("@chevron"), bg.lightness() < 128
        ? QStringLiteral(":/icons/white/svg/arrow-down-s-line.svg")
        : QStringLiteral(":/icons/black/svg/arrow-down-s-line.svg"));
    setStyleSheet(style);
    setupSettingsFonts(sidebar);
    for (auto* spinBox : settingsPages->findChildren<QAbstractSpinBox*>())
    {
        new ClickToAdjustWheelFilter(spinBox);
    }
    for (auto* comboBox : settingsPages->findChildren<QComboBox*>())
    {
        new ClickToAdjustWheelFilter(comboBox);
    }
    setMinimumSize(760, 540);
    resize(980, 760);
#ifdef NETPLAY
    selectSettingsPage(static_cast<int>(SettingsDialogTab::Rollback));
#else
    selectSettingsPage(static_cast<int>(SettingsDialogTab::InterfaceGeneral));
#endif
    settingsNavigation->verticalScrollBar()->setValue(0);
}

void SettingsDialog::setupSettingsFonts(QWidget* sidebar)
{
    auto body = this->font();
    body.setPixelSize(15);
    this->setFont(body);
    for (auto* button : buttonBox->buttons()) button->setFont(body);
    // Styled controls may retain their application font after a parent changes.
    for (auto* widget : settingsPages->findChildren<QWidget*>())
    {
        const auto offset = widget->property("settingsHeadingOffset");
        QFont font = body;
        if (offset.isValid())
        {
            const int size = body.pixelSize() + offset.toInt();
            if (widget->objectName() == "SettingsPageTitle")
            {
                font = settingsHeaderFont(size);
            }
            else
            {
                font.setPixelSize(size);
                font.setBold(true);
            }
        }
        widget->setFont(font);
    }

    auto navigation = body;
    navigation.setPixelSize(18);
    sidebar->setFont(navigation);
    settingsNavigation->setFont(navigation);
    settingsSearch->setFont(navigation);
    controllerConfigButton->setFont(body);
    const auto sectionFont = settingsHeaderFont(navigation.pixelSize() + 3);
    const QFontMetrics rowMetrics(navigation), sectionMetrics(sectionFont);
    int sidebarWidth = 240;
    for (QTreeWidgetItemIterator it(settingsNavigation); *it; ++it)
    {
        auto* item = *it;
        const bool section = !item->parent();
        const auto& metrics = section ? sectionMetrics : rowMetrics;
        if (section) item->setFont(0, sectionFont);
        item->setSizeHint(0, QSize(0, item->text(0).isEmpty() ? 1 : metrics.height() + (section ? 22 : 19)));
        sidebarWidth = qMax(sidebarWidth, metrics.horizontalAdvance(item->text(0)) + 76);
    }
    const auto margins = sidebar->layout()->contentsMargins();
    sidebar->setFixedWidth(qMax(sidebarWidth, controllerConfigButton->sizeHint().width() + margins.left() + margins.right()));

    // The native style can give the chat combo box and spin boxes different heights.
    const QList<QWidget*> chatFields{osdLocationComboBox, osdDurationSpinBox, osdHorizontalPaddingSpinBox,
                                    osdVerticalPaddingSpinBox, osdScaleDoubleSpinBox, osdMaxMessagesSpinBox};
    int chatFieldHeight = 0;
    for (auto* field : chatFields)
    {
        field->ensurePolished();
        chatFieldHeight = qMax(chatFieldHeight, field->sizeHint().height());
    }
    for (auto* field : chatFields) field->setFixedHeight(chatFieldHeight);
}

void SettingsDialog::setupShortcutsPage()
{
    // The tabs stay in place while each list scrolls, as in the lobby's chat.
    // Remove the old scroll area around the entire tab widget.
    verticalLayout_33->removeWidget(scrollArea);
    scrollArea->hide();
    verticalLayout_33->insertWidget(0, keybindingsMenuTabWidget);
    keybindingsMenuTabWidget->show();
    keybindingsMenuTabWidget->setDocumentMode(false);
    keybindingsMenuTabWidget->setUsesScrollButtons(true);
    keybindingsMenuTabWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    keybindingsMenuTabWidget->setMinimumHeight(180);
    auto* tabs = keybindingsMenuTabWidget->tabBar();
    tabs->setObjectName("SettingsShortcutTabs");
    tabs->setExpanding(false);
    tabs->setDrawBase(false);

    for (int i = 0; i < keybindingsMenuTabWidget->count(); ++i)
    {
        auto* form = keybindingsMenuTabWidget->widget(i);
        const auto title = keybindingsMenuTabWidget->tabText(i);
        keybindingsMenuTabWidget->removeTab(i);
        form->setProperty("settingsShortcutPage", true);
        if (auto* formLayout = qobject_cast<QVBoxLayout*>(form->layout()))
        {
            formLayout->setContentsMargins(16, 14, 16, 14);
            formLayout->setSpacing(8);
            formLayout->addStretch();
        }
        auto* list = new QScrollArea(keybindingsMenuTabWidget);
        list->setWidgetResizable(true);
        list->setFrameShape(QFrame::NoFrame);
        list->setWidget(form);
        keybindingsMenuTabWidget->insertTab(i, list, title);
    }
    keybindingsMenuTabWidget->setCurrentIndex(0);
    for (auto* binding : hotkeysTab->findChildren<KeybindButton*>())
    {
        binding->setProperty("shortcutBinding", true);
        binding->setFixedWidth(180);
        binding->setAutoDefault(false);
    }
}

void SettingsDialog::selectSettingsPage(int id)
{
    settingsSearch->clear();
    for (QTreeWidgetItemIterator it(settingsNavigation); *it; ++it)
    {
        if ((*it)->parent() && !(*it)->isDisabled() && (*it)->data(0, Qt::UserRole).toInt() == id)
        {
            settingsNavigation->setCurrentItem(*it);
            settingsNavigation->scrollToItem(*it);
            return;
        }
    }
}

#ifdef _WIN32
void SettingsDialog::populateExclusiveFullscreenModes(void)
{
    // save current selections before repopulating
    QString savedMonitor = this->exclusiveMonitorComboBox->currentData().toString();
    QString savedResolution = this->exclusiveResolutionComboBox->currentData().toString();
    int savedRefreshRate = this->exclusiveRefreshRateComboBox->currentData().toInt();

    // block signals during population to avoid recursive calls
    bool monBlocked = this->exclusiveMonitorComboBox->blockSignals(true);
    bool resBlocked = this->exclusiveResolutionComboBox->blockSignals(true);
    bool rateBlocked = this->exclusiveRefreshRateComboBox->blockSignals(true);

    // populate monitors (only do full enumeration once)
    if (!this->exclusiveMonitorComboBox->property("populated").toBool())
    {
        this->exclusiveMonitorComboBox->clear();
        this->exclusiveMonitorComboBox->addItem(tr("Primary Monitor"), "");
        DISPLAY_DEVICEW displayDevice = {};
        displayDevice.cb = sizeof(displayDevice);
        for (DWORD i = 0; EnumDisplayDevicesW(NULL, i, &displayDevice, 0); i++)
        {
            if (!(displayDevice.StateFlags & DISPLAY_DEVICE_ACTIVE))
            {
                continue;
            }
            // get friendly name from child device
            DISPLAY_DEVICEW monitor = {};
            monitor.cb = sizeof(monitor);
            // extract display number from device name (e.g. "\\\\.\\DISPLAY1" -> "Display 1")
            QString devName = QString::fromWCharArray(displayDevice.DeviceName);
            QString displayNum = devName;
            int displayIdx = devName.lastIndexOf("DISPLAY");
            if (displayIdx >= 0)
            {
                displayNum = tr("Display %1").arg(devName.mid(displayIdx + 7));
            }
            QString label;
            if (EnumDisplayDevicesW(displayDevice.DeviceName, 0, &monitor, 0))
            {
                label = QString("%1 - %2").arg(QString::fromWCharArray(monitor.DeviceString), displayNum);
            }
            else
            {
                label = displayNum;
            }
            this->exclusiveMonitorComboBox->addItem(label, QString::fromWCharArray(displayDevice.DeviceName));
        }

        // restore monitor selection
        int monIdx = this->exclusiveMonitorComboBox->findData(savedMonitor);
        if (monIdx >= 0)
        {
            this->exclusiveMonitorComboBox->setCurrentIndex(monIdx);
        }
        this->exclusiveMonitorComboBox->setProperty("populated", true);
    }

    // get selected monitor device name for enumeration
    QString selectedMonitor = this->exclusiveMonitorComboBox->currentData().toString();
    LPCWSTR deviceName = nullptr;
    std::wstring deviceNameStr;
    if (!selectedMonitor.isEmpty())
    {
        deviceNameStr = selectedMonitor.toStdWString();
        deviceName = deviceNameStr.c_str();
    }

    // enumerate display modes for selected monitor
    struct Resolution
    {
        DWORD width;
        DWORD height;
        bool operator<(const Resolution& o) const
        {
            if (width != o.width) return width < o.width;
            return height < o.height;
        }
    };

    std::set<Resolution> resolutions;
    std::set<int> refreshRates;

    DEVMODEW devmode = {};
    devmode.dmSize = sizeof(devmode);
    for (DWORD i = 0; EnumDisplaySettingsW(deviceName, i, &devmode); i++)
    {
        if (devmode.dmBitsPerPel != 32)
        {
            continue;
        }
        resolutions.insert({devmode.dmPelsWidth, devmode.dmPelsHeight});
    }

    // populate resolutions
    this->exclusiveResolutionComboBox->clear();
    this->exclusiveResolutionComboBox->addItem(tr("Desktop Default"), "");
    for (const auto& res : resolutions)
    {
        QString text = QString("%1x%2").arg(res.width).arg(res.height);
        this->exclusiveResolutionComboBox->addItem(text, text);
    }

    // restore resolution selection
    int resIdx = this->exclusiveResolutionComboBox->findData(savedResolution);
    if (resIdx >= 0)
    {
        this->exclusiveResolutionComboBox->setCurrentIndex(resIdx);
    }

    // enumerate refresh rates for selected resolution
    QString selectedRes = this->exclusiveResolutionComboBox->currentData().toString();
    devmode = {};
    devmode.dmSize = sizeof(devmode);
    for (DWORD i = 0; EnumDisplaySettingsW(deviceName, i, &devmode); i++)
    {
        if (devmode.dmBitsPerPel != 32)
        {
            continue;
        }
        if (!selectedRes.isEmpty())
        {
            QString modeRes = QString("%1x%2").arg(devmode.dmPelsWidth).arg(devmode.dmPelsHeight);
            if (modeRes != selectedRes)
            {
                continue;
            }
        }
        refreshRates.insert(static_cast<int>(devmode.dmDisplayFrequency));
    }

    // populate refresh rates
    this->exclusiveRefreshRateComboBox->clear();
    this->exclusiveRefreshRateComboBox->addItem(tr("Desktop Default"), 0);
    for (int rate : refreshRates)
    {
        QString text = tr("%1 Hz").arg(rate);
        this->exclusiveRefreshRateComboBox->addItem(text, rate);
    }

    // restore refresh rate selection
    int rateIdx = this->exclusiveRefreshRateComboBox->findData(savedRefreshRate);
    if (rateIdx >= 0)
    {
        this->exclusiveRefreshRateComboBox->setCurrentIndex(rateIdx);
    }

    this->exclusiveMonitorComboBox->blockSignals(monBlocked);
    this->exclusiveResolutionComboBox->blockSignals(resBlocked);
    this->exclusiveRefreshRateComboBox->blockSignals(rateBlocked);
}
#endif // _WIN32

void SettingsDialog::ShowGameTab(void)
{
    this->selectSettingsPage(static_cast<int>(SettingsDialogTab::Game));
}

void SettingsDialog::ShowPluginsTab(void)
{
    this->selectSettingsPage(static_cast<int>(SettingsDialogTab::Plugin));
}

int SettingsDialog::currentIndex(void)
{
    auto* item = this->settingsNavigation->currentItem();
    return item ? item->data(0, Qt::UserRole).toInt() : static_cast<int>(SettingsDialogTab::Invalid);
}

void SettingsDialog::restoreDefaults(int stackedWidgetIndex)
{
    if (stackedWidgetIndex < 0 || 
        stackedWidgetIndex > static_cast<int>(SettingsDialogTab::Invalid))
    {
        return;
    }

    switch (static_cast<SettingsDialogTab>(stackedWidgetIndex))
    {
    default:
        break;
    case SettingsDialogTab::InterfaceGeneral:
        this->loadDefaultInterfaceGeneralSettings();
        break;
    case SettingsDialogTab::InterfaceEmulation:
        this->loadDefaultInterfaceEmulationSettings();
        break;
    case SettingsDialogTab::InterfaceRomBrowser:
        this->loadDefaultInterfaceRomBrowserSettings();
        break;
    case SettingsDialogTab::InterfaceLog:
        this->loadDefaultInterfaceLogSettings();
        break;
    case SettingsDialogTab::InterfaceOSD:
        this->loadDefaultInterfaceOSDSettings();
        break;
    case SettingsDialogTab::InterfaceNetplay:
        this->loadDefaultInterfaceNetplaySettings();
        break;
    case SettingsDialogTab::Hotkey:
        this->loadDefaultHotkeySettings();
        break;
    case SettingsDialogTab::Core:
        this->loadDefaultCoreSettings();
        break;
    case SettingsDialogTab::Game:
        this->loadDefaultGameSettings();
        break;
    case SettingsDialogTab::GameCore:
        this->loadDefaultGameCoreSettings();
        break;
    case SettingsDialogTab::GamePlugin:
        this->loadDefaultGamePluginSettings();
        break;
    case SettingsDialogTab::Plugin:
        this->loadDefaultPluginSettings();
        break;
    case SettingsDialogTab::Directory:
        this->loadDefaultDirectorySettings();
        break;
    case SettingsDialogTab::N64DD:
        this->loadDefault64DDSettings();
        break;
    case SettingsDialogTab::Rollback:
        this->loadDefaultRollbackSettings();
        break;
    case SettingsDialogTab::Diagnostics:
        this->loadDefaultDiagnosticsSettings();
        break;
    case SettingsDialogTab::Recording:
        this->loadDefaultRecordingSettings();
        break;
    case SettingsDialogTab::NetplayOverlay:
        this->loadDefaultNetplayOverlaySettings();
        break;
    }
}

void SettingsDialog::loadSettings(int stackedWidgetIndex)
{
    if (stackedWidgetIndex < 0 || 
        stackedWidgetIndex > static_cast<int>(SettingsDialogTab::Invalid))
    {
        return;
    }

    switch (static_cast<SettingsDialogTab>(stackedWidgetIndex))
    {
    default:
        break;
    case SettingsDialogTab::InterfaceGeneral:
        this->loadInterfaceGeneralSettings();
        break;
    case SettingsDialogTab::InterfaceEmulation:
        this->loadInterfaceEmulationSettings();
        break;
    case SettingsDialogTab::InterfaceRomBrowser:
        this->loadInterfaceRomBrowserSettings();
        break;
    case SettingsDialogTab::InterfaceLog:
        this->loadInterfaceLogSettings();
        break;
    case SettingsDialogTab::InterfaceOSD:
        this->loadInterfaceOSDSettings();
        break;
    case SettingsDialogTab::InterfaceNetplay:
        this->loadInterfaceNetplaySettings();
        break;
    case SettingsDialogTab::Hotkey:
        this->loadHotkeySettings();
        break;
    case SettingsDialogTab::Core:
        this->loadCoreSettings();
        break;
    case SettingsDialogTab::Game:
        this->loadGameSettings();
        break;
    case SettingsDialogTab::GameCore:
        this->loadGameCoreSettings();
        break;
    case SettingsDialogTab::GamePlugin:
        this->loadGamePluginSettings();
        break;
    case SettingsDialogTab::Plugin:
        this->loadPluginSettings();
        break;
    case SettingsDialogTab::Directory:
        this->loadDirectorySettings();
        break;
    case SettingsDialogTab::N64DD:
        this->load64DDSettings();
        break;
    case SettingsDialogTab::Rollback:
        this->loadRollbackSettings();
        break;
    }
}

void SettingsDialog::loadCoreSettings(void)
{
    bool disableExtraMem = CoreSettingsGetBoolValue(SettingsID::CoreOverlay_DisableExtraMem);
    int counterFactor = CoreSettingsGetIntValue(SettingsID::CoreOverlay_CountPerOp);
    const int cpuEmulator = CoreSettingsGetIntValue(SettingsID::CoreOverlay_CPU_Emulator);
    const QString videoCaptureBackend = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::CoreOverlay_GbCameraVideoCaptureBackend1));
    const int saveFilenameFormat = CoreSettingsGetIntValue(SettingsID::CoreOverLay_SaveFileNameFormat);
    int siDmaDuration = CoreSettingsGetIntValue(SettingsID::CoreOverlay_SiDmaDuration);
    const bool randomizeInterrupt = CoreSettingsGetBoolValue(SettingsID::CoreOverlay_RandomizeInterrupt);
    const bool usePIFROM = CoreSettingsGetBoolValue(SettingsID::Core_PIF_Use);
    const QString ntscPifROM = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Core_PIF_NTSC));
    const QString palPifRom = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Core_PIF_PAL));
    const bool overrideGameSettings = CoreSettingsGetBoolValue(SettingsID::Core_OverrideGameSpecificSettings);

    this->coreCpuEmulatorComboBox->setCurrentIndex(cpuEmulator);
    this->coreVideoCaptureBackendComboBox->setCurrentIndex(videoCaptureBackend == "sdl3" ? 1 : 0);
    this->coreSaveFilenameFormatComboBox->setCurrentIndex(saveFilenameFormat);
    this->coreRandomizeTimingCheckBox->setChecked(randomizeInterrupt);

    this->usePifRomGroupBox->setChecked(usePIFROM);
    this->ntscPifRomLineEdit->setText(ntscPifROM);
    this->palPifRomLineEdit->setText(palPifRom);

    this->coreOverrideGameSettingsGroup->setChecked(overrideGameSettings);

    if (!overrideGameSettings)
    {
        disableExtraMem = false;
        counterFactor = 2;
        siDmaDuration = 2304;
    }

    this->coreMemorySizeComboBox->setCurrentIndex(!disableExtraMem);
    this->coreCounterFactorComboBox->setCurrentIndex(counterFactor - 1);
    this->coreSiDmaDurationSpinBox->setValue(siDmaDuration);
}

void SettingsDialog::loadGameSettings(void)
{
    this->gameGoodNameLineEdit->setText(QString::fromStdString(this->currentGameSettings.GoodName));
    this->gameMemorySizeComboBox->setCurrentIndex(!this->currentGameSettings.DisableExtraMem);
    this->gameSaveTypeComboBox->setCurrentIndex(this->currentGameSettings.SaveType);
    this->gameCounterFactorComboBox->setCurrentIndex(this->currentGameSettings.CountPerOp - 1);
    this->gameTransferPakComboBox->setCurrentIndex(this->currentGameSettings.TransferPak);
    this->gameSiDmaDurationSpinBox->setValue(this->currentGameSettings.SiDMADuration);
}

void SettingsDialog::loadGameCoreSettings(void)
{
    const bool overrideEnabled = CoreSettingsGetBoolValue(SettingsID::Game_OverrideCoreSettings, this->gameSection);
    const int cpuEmulator = CoreSettingsGetIntValue(SettingsID::Game_CPU_Emulator, this->gameSection);
    const int overclockingFactor = CoreSettingsGetIntValue(SettingsID::Game_CountPerOpDenomPot, this->gameSection);
    const bool randomizeInterrupt = CoreSettingsGetBoolValue(SettingsID::Game_RandomizeInterrupt, this->gameSection);

    gameOverrideCoreSettingsGroupBox->setChecked(overrideEnabled);
    gameCoreCpuEmulatorComboBox->setCurrentIndex(cpuEmulator);
    gameOverclockingFactorSpinBox->setValue(overclockingFactor);
    gameRandomizeTimingCheckBox->setChecked(randomizeInterrupt);
}

void SettingsDialog::loadGamePluginSettings(void)
{
    QComboBox *comboBoxArray[] = {this->gameRspPluginsComboBox, this->gameVideoPluginsComboBox,
                                   this->gameAudioPluginsComboBox, this->gameInputPluginsComboBox};
    SettingsID settingsId[] = {SettingsID::Game_RSP_Plugin, SettingsID::Game_GFX_Plugin, 
                                    SettingsID::Game_AUDIO_Plugin, SettingsID::Game_INPUT_Plugin};
    bool pluginFound[] = {false, false, false, false};
    QString pluginFileNames[4];
    QString pluginFileName;
    QString pluginName;

    QComboBox *comboBox;
    int index = 0;

    for (int i = 0; i < 4; i++)
    {
        pluginFileName = QString::fromStdString(CoreSettingsGetStringValue(settingsId[i], this->gameSection));

        if (!pluginFileName.isEmpty())
        {
            // account for full path (<v0.3.5 we used the full path)
            pluginFileName = QFileInfo(pluginFileName).fileName();
            pluginFileNames[i] = pluginFileName;
        }
    }

    for (QComboBox *comboBox : comboBoxArray)
    {
        comboBox->clear();
        comboBox->addItem(tr("**Use Core Plugin Settings**"));
    }

    for (const auto &p : this->pluginList)
    {
        index = (static_cast<int>(p.Type) - 1);

        comboBox = comboBoxArray[index];
        pluginName = QString::fromStdString(p.Name);

        // Use friendly display names for input plugins (index 3)
        if (index == 3)
        {
            pluginName = getInputPluginDisplayName(QString::fromStdString(p.File), pluginName);
        }

        comboBox->addItem(pluginName, QString::fromStdString(p.File));

        if (pluginFileNames[index] == QString::fromStdString(p.File))
        {
            comboBox->setCurrentText(pluginName);
            pluginFound[index] = true;
        }
    }

    for (int i = 0; i < 4; i++)
    {
        comboBox = comboBoxArray[i];

        if (CoreSettingsGetStringValue(settingsId[i], this->gameSection).empty())
        { // skip empty values
            continue;
        }

        if (!pluginFound[i])
        {
            pluginName = pluginFileNames[i] + " (not found)";

            comboBox->addItem(pluginName, pluginFileNames[i]);
            comboBox->setCurrentText(pluginName);
        }
    }
}

void SettingsDialog::loadPluginSettings(void)
{
    this->commonPluginSettings(SettingsDialogAction::LoadSettings);
}

void SettingsDialog::loadDirectorySettings(void)
{
    const std::string screenshotDir = CoreSettingsGetStringValue(SettingsID::Core_ScreenshotPath);
    const std::string saveStateDir = CoreSettingsGetStringValue(SettingsID::Core_SaveStatePath);
    const std::string saveSramDir = CoreSettingsGetStringValue(SettingsID::Core_SaveSRAMPath);

    this->screenshotDirLineEdit->setText(QString::fromStdString(screenshotDir));
    this->saveStateDirLineEdit->setText(QString::fromStdString(saveStateDir));
    this->saveSramDirLineEdit->setText(QString::fromStdString(saveSramDir));
}

void SettingsDialog::load64DDSettings(void)
{
    int kailleraPort = CoreSettingsGetIntValue(SettingsID::Kaillera_Port);
    if (kailleraPort < 1 || kailleraPort > 65535)
    {
        kailleraPort = 27886;
    }

    QString recordsDirectoryRaw = QString::fromStdString(
        CoreSettingsGetStringValue(SettingsID::Kaillera_RecordsDirectory));
    if (recordsDirectoryRaw.isEmpty())
    {
        recordsDirectoryRaw = "records";
    }

    int recordingCapMB = CoreSettingsGetIntValue(SettingsID::Kaillera_RecordingCapMB);
    if (recordingCapMB < 1)
    {
        recordingCapMB = 1;
    }

    this->kailleraRecordByDefaultCheckBox->setChecked(
        CoreSettingsGetBoolValue(SettingsID::Kaillera_RecordingEnabled));
    this->kailleraPortSpinBox->setValue(kailleraPort);
    this->kailleraRecordsDirectoryLineEdit->setProperty("rawPath", recordsDirectoryRaw);
    this->kailleraRecordsDirectoryLineEdit->setText(getDisplayDirectoryPath(recordsDirectoryRaw));
    this->kailleraRecordsDirectoryLineEdit->setToolTip(this->kailleraRecordsDirectoryLineEdit->text());
    this->kailleraRecordingCapEnabledCheckBox->setChecked(
        CoreSettingsGetBoolValue(SettingsID::Kaillera_RecordingCapEnabled));
    this->kailleraRecordingCapMBSpinBox->setValue(recordingCapMB);
    this->updateKailleraRecordingCapControls();
    this->kailleraFlashOnJoinCheckBox->setChecked(
        CoreSettingsGetBoolValue(SettingsID::Kaillera_FlashOnJoin));
    this->kailleraBeepOnJoinCheckBox->setChecked(
        CoreSettingsGetBoolValue(SettingsID::Kaillera_BeepOnJoin));
}

void SettingsDialog::loadHotkeySettings(void)
{
    this->commonHotkeySettings(SettingsDialogAction::LoadSettings);
}

void SettingsDialog::loadInterfaceGeneralSettings(void)
{
    const QList<ThemeOption> themeOptions = collectAvailableThemeOptions();
    const QString currentTheme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_Theme));
    const bool showLegacy = isLegacyThemeValue(themeOptions, currentTheme);
    const bool blocked = this->showLegacyThemesCheckBox->blockSignals(true);
    this->showLegacyThemesCheckBox->setChecked(showLegacy);
    this->showLegacyThemesCheckBox->blockSignals(blocked);
    populateThemeCombo(this->themeComboBox, themeOptions, showLegacy, currentTheme);
    this->iconThemeComboBox->setCurrentText(QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_IconTheme)));
    const int languageIndex = this->languageComboBox->findData(
        QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_Language)));
    this->languageComboBox->setCurrentIndex(languageIndex >= 0 ? languageIndex : 0);
    this->autoStartNetplayOnStartupCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::GUI_AutoStartNetplayOnStartup));
#ifdef UPDATER
    this->checkForUpdatesCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::GUI_CheckForUpdates));
#endif // UPDATER
}

void SettingsDialog::loadInterfaceEmulationSettings(void)
{
    this->pauseEmulationOnFocusCheckbox->setChecked(CoreSettingsGetBoolValue(SettingsID::GUI_PauseEmulationOnFocusLoss));
    this->resumeEmulationOnFocusCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::GUI_ResumeEmulationOnFocus));
    this->hideCursorCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::GUI_HideCursorInEmulation));
    this->hideCursorFullscreenCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::GUI_HideCursorInFullscreenEmulation));
    this->automaticFullscreenCheckbox->setChecked(CoreSettingsGetBoolValue(SettingsID::GUI_AutomaticFullscreen));
#ifdef _WIN32
    this->exclusiveFullscreenCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::GUI_ExclusiveFullscreen));
    this->betaFullscreenBackendCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::GUI_BetaFullscreenBackend));
    if (this->betaFullscreenBackendCheckBox->isChecked())
    {
        this->automaticFullscreenCheckbox->setChecked(true);
    }
    {
        // set saved values into combobox data before populating so they get selected
        QString savedMonitor = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_ExclusiveFullscreenMonitor));
        QString savedRes = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_ExclusiveFullscreenResolution));
        int savedRate = CoreSettingsGetIntValue(SettingsID::GUI_ExclusiveFullscreenRefreshRate);
        this->exclusiveMonitorComboBox->blockSignals(true);
        this->exclusiveMonitorComboBox->clear();
        // monitor combobox is populated in populateExclusiveFullscreenModes
        // seed with saved value so it gets selected after population
        this->exclusiveMonitorComboBox->addItem(tr("Primary Monitor"), "");
        if (!savedMonitor.isEmpty())
        {
            this->exclusiveMonitorComboBox->addItem(savedMonitor, savedMonitor);
            this->exclusiveMonitorComboBox->setCurrentIndex(1);
        }
        this->exclusiveMonitorComboBox->blockSignals(false);
        this->exclusiveResolutionComboBox->blockSignals(true);
        this->exclusiveResolutionComboBox->clear();
        this->exclusiveResolutionComboBox->addItem(tr("Desktop Default"), "");
        if (!savedRes.isEmpty())
        {
            this->exclusiveResolutionComboBox->addItem(savedRes, savedRes);
            this->exclusiveResolutionComboBox->setCurrentIndex(1);
        }
        this->exclusiveResolutionComboBox->blockSignals(false);
        this->exclusiveRefreshRateComboBox->blockSignals(true);
        this->exclusiveRefreshRateComboBox->clear();
        this->exclusiveRefreshRateComboBox->addItem(tr("Desktop Default"), 0);
        if (savedRate > 0)
        {
            this->exclusiveRefreshRateComboBox->addItem(tr("%1 Hz").arg(savedRate), savedRate);
            this->exclusiveRefreshRateComboBox->setCurrentIndex(1);
        }
        this->exclusiveRefreshRateComboBox->blockSignals(false);
        this->populateExclusiveFullscreenModes();
    }
    this->setExclusiveFullscreenControlsEnabled(this->exclusiveFullscreenCheckBox->isChecked());
#endif
    this->confirmExitWhileInGameCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::GUI_ConfirmExitWhileInGame));
    this->statusBarMessageDurationSpinBox->setValue(CoreSettingsGetIntValue(SettingsID::GUI_StatusbarMessageDuration));
    this->openglTypeComboBox->setCurrentIndex(CoreSettingsGetBoolValue(SettingsID::GUI_OpenGLES));
}

void SettingsDialog::loadInterfaceRomBrowserSettings(void)
{
    this->searchSubDirectoriesCheckbox->setChecked(CoreSettingsGetBoolValue(SettingsID::RomBrowser_Recursive));
    this->romSearchLimitSpinBox->setValue(CoreSettingsGetIntValue(SettingsID::RomBrowser_MaxItems));
}

void SettingsDialog::loadInterfaceLogSettings(void)
{
    this->showVerboseLogMessagesCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::GUI_ShowVerboseLogMessages));
}

void SettingsDialog::loadInterfaceOSDSettings(void)
{
    this->osdEnabledCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::GUI_OnScreenDisplayEnabled));
    this->osdChatEnabledCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::GUI_OnScreenDisplayChatEnabled));
    this->osdLocationComboBox->setCurrentIndex(CoreSettingsGetIntValue(SettingsID::GUI_OnScreenDisplayLocation));
    this->osdVerticalPaddingSpinBox->setValue(CoreSettingsGetIntValue(SettingsID::GUI_OnScreenDisplayPaddingY));
    this->osdHorizontalPaddingSpinBox->setValue(CoreSettingsGetIntValue(SettingsID::GUI_OnScreenDisplayPaddingX));
    this->osdDurationSpinBox->setValue(CoreSettingsGetIntValue(SettingsID::GUI_OnScreenDisplayDuration));
    this->osdScaleDoubleSpinBox->setValue(CoreSettingsGetFloatValue(SettingsID::GUI_OnScreenDisplayScale));
    this->osdMaxMessagesSpinBox->setValue(CoreSettingsGetIntValue(SettingsID::GUI_OnScreenDisplayMaxMessages));
    this->osdKailleraPortLabelsCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::GUI_OnScreenDisplayKailleraPortLabels));
    this->osdLiveReplayStatusCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::GUI_OnScreenDisplayLiveReplayStatus));

    std::vector<int> backgroundColor = CoreSettingsGetIntListValue(SettingsID::GUI_OnScreenDisplayBackgroundColor);
    std::vector<int> textColor = CoreSettingsGetIntListValue(SettingsID::GUI_OnScreenDisplayTextColor);
    if (backgroundColor.size() == 4)
    {
        this->currentBackgroundColor = QColor(backgroundColor.at(0), backgroundColor.at(1), 
                                              backgroundColor.at(2), backgroundColor.at(3));
        this->chooseColor(this->changeBackgroundColorButton, &this->currentBackgroundColor, true);
    }
    if (textColor.size() == 4)
    {
        this->currentTextColor = QColor(textColor.at(0), textColor.at(1), textColor.at(2), textColor.at(3));
        this->chooseColor(this->changeTextColorButton, &this->currentTextColor, true);
    }

    this->updateOSDSettingsEnabledState();
}

void SettingsDialog::loadInterfaceNetplaySettings(void)
{
    // TODO: maybe add initialize functions for each tab?
    static bool initialized = false;
    if (!initialized)
    { // set regexp for nickname
        QRegularExpression re(NETPLAYCOMMON_NICKNAME_REGEX);
        this->netplayNicknameLineEdit->setValidator(new QRegularExpressionValidator(re, this));
        initialized = true;
    }

    this->netplayNicknameLineEdit->setText(QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Netplay_Nickname)));
    // Kaillera uses built-in server list, no need for custom URLs
    // this->netplayServerUrlLineEdit->setText(QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Netplay_ServerJsonUrl)));
    // this->netplayDispatcherUrlLineEdit->setText(QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Netplay_DispatcherUrl)));
}

void SettingsDialog::loadRollbackSettings(void)
{
    this->rollbackVerboseStatsCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::Rollback_VerboseStats));
    this->rollbackStallDiagnosticsCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::Rollback_StallDiagnostics));
    this->rollbackEmulationDiagnosticsCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::Rollback_EmulationDiagnostics));
    this->rollbackPacingTraceCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::Rollback_PacingTrace));
    this->rollbackEnableLocalTestingCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::Rollback_EnableLocalTesting));
    this->rollbackVerbosePifInputLoggingCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::Rollback_VerbosePifInputLogging));
    this->rollbackVerboseGlideInputLoggingCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::Rollback_VerboseGlideInputLogging));
    this->rollbackPingDiagnosticsCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::Rollback_PingDiagnostics));
    this->rollbackHideLocationCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::Rollback_HideLocation));
    this->rollbackRememberInputDelayCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::Rollback_RememberInputDelay));
}

void SettingsDialog::loadDefaultCoreSettings(void)
{
    bool disableExtraMem = CoreSettingsGetDefaultBoolValue(SettingsID::CoreOverlay_DisableExtraMem);
    int counterFactor = CoreSettingsGetDefaultIntValue(SettingsID::CoreOverlay_CountPerOp);
    const int cpuEmulator = CoreSettingsGetDefaultIntValue(SettingsID::CoreOverlay_CPU_Emulator);
    const QString videoCaptureBackend = QString::fromStdString(CoreSettingsGetDefaultStringValue(SettingsID::CoreOverlay_GbCameraVideoCaptureBackend1));
    int siDmaDuration = CoreSettingsGetDefaultIntValue(SettingsID::CoreOverlay_SiDmaDuration);
    const bool randomizeInterrupt = CoreSettingsGetDefaultBoolValue(SettingsID::CoreOverlay_RandomizeInterrupt);
    const bool usePIFROM = CoreSettingsGetDefaultBoolValue(SettingsID::Core_PIF_Use);
    const QString ntscPifROM = QString::fromStdString(CoreSettingsGetDefaultStringValue(SettingsID::Core_PIF_NTSC));
    const QString palPifRom = QString::fromStdString(CoreSettingsGetDefaultStringValue(SettingsID::Core_PIF_PAL));
    const bool overrideGameSettings = CoreSettingsGetDefaultBoolValue(SettingsID::Core_OverrideGameSpecificSettings);

    this->coreCpuEmulatorComboBox->setCurrentIndex(cpuEmulator);
    this->coreVideoCaptureBackendComboBox->setCurrentIndex(videoCaptureBackend == "sdl3" ? 1 : 0);
    this->coreRandomizeTimingCheckBox->setChecked(randomizeInterrupt);

    this->usePifRomGroupBox->setChecked(usePIFROM);
    this->ntscPifRomLineEdit->setText(ntscPifROM);
    this->palPifRomLineEdit->setText(palPifRom);

    this->coreOverrideGameSettingsGroup->setChecked(overrideGameSettings);

    if (!this->coreOverrideGameSettingsGroup->isChecked())
    {
        disableExtraMem = false;
        counterFactor = 2;
        siDmaDuration = 2304;
    }

    this->coreMemorySizeComboBox->setCurrentIndex(!disableExtraMem);
    this->coreCounterFactorComboBox->setCurrentIndex(counterFactor - 1);
    this->coreSiDmaDurationSpinBox->setValue(siDmaDuration);
}

void SettingsDialog::loadDefaultGameSettings(void)
{
    this->gameGoodNameLineEdit->setText(QString::fromStdString(this->defaultGameSettings.GoodName));
    this->gameMemorySizeComboBox->setCurrentIndex(!this->defaultGameSettings.DisableExtraMem);
    this->gameSaveTypeComboBox->setCurrentIndex(this->defaultGameSettings.SaveType);
    this->gameCounterFactorComboBox->setCurrentIndex(this->defaultGameSettings.CountPerOp - 1);
    this->gameTransferPakComboBox->setCurrentIndex(this->defaultGameSettings.TransferPak);
    this->gameSiDmaDurationSpinBox->setValue(this->defaultGameSettings.SiDMADuration);
}

void SettingsDialog::loadDefaultGameCoreSettings(void)
{
    this->gameOverrideCoreSettingsGroupBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::Game_OverrideCoreSettings));
    this->gameCoreCpuEmulatorComboBox->setCurrentIndex(CoreSettingsGetDefaultIntValue(SettingsID::Game_CPU_Emulator));
    this->gameOverclockingFactorSpinBox->setValue(CoreSettingsGetDefaultIntValue(SettingsID::Game_CountPerOpDenomPot));
    this->gameRandomizeTimingCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::Game_RandomizeInterrupt));
}

void SettingsDialog::loadDefaultGamePluginSettings(void)
{
    QComboBox *comboBoxArray[] = {this->gameVideoPluginsComboBox, this->gameAudioPluginsComboBox,
                                   this->gameInputPluginsComboBox, this->gameRspPluginsComboBox};

    for (QComboBox *comboBox : comboBoxArray)
    {
        comboBox->setCurrentIndex(0);
    }
}

void SettingsDialog::loadDefaultPluginSettings(void)
{
    this->openglTypeComboBox->setCurrentIndex(CoreSettingsGetDefaultBoolValue(SettingsID::GUI_OpenGLES));
    this->commonPluginSettings(SettingsDialogAction::LoadDefaultSettings);
}

void SettingsDialog::loadDefaultDirectorySettings(void)
{
    this->coreSaveFilenameFormatComboBox->setCurrentIndex(
        CoreSettingsGetDefaultIntValue(SettingsID::CoreOverLay_SaveFileNameFormat));
    this->screenshotDirLineEdit->setText(QString::fromStdString(CoreSettingsGetDefaultStringValue(SettingsID::Core_ScreenshotPath)));
    this->saveStateDirLineEdit->setText(QString::fromStdString(CoreSettingsGetDefaultStringValue(SettingsID::Core_SaveStatePath)));
    this->saveSramDirLineEdit->setText(QString::fromStdString(CoreSettingsGetDefaultStringValue(SettingsID::Core_SaveSRAMPath)));
}

void SettingsDialog::loadDefault64DDSettings(void)
{
    int kailleraPort = CoreSettingsGetDefaultIntValue(SettingsID::Kaillera_Port);
    if (kailleraPort < 1 || kailleraPort > 65535)
    {
        kailleraPort = 27886;
    }
    this->kailleraPortSpinBox->setValue(kailleraPort);
    this->kailleraFlashOnJoinCheckBox->setChecked(
        CoreSettingsGetDefaultBoolValue(SettingsID::Kaillera_FlashOnJoin));
    this->kailleraBeepOnJoinCheckBox->setChecked(
        CoreSettingsGetDefaultBoolValue(SettingsID::Kaillera_BeepOnJoin));
}

void SettingsDialog::loadDefaultRecordingSettings(void)
{
    QString recordsDirectoryRaw = QString::fromStdString(
        CoreSettingsGetDefaultStringValue(SettingsID::Kaillera_RecordsDirectory));
    if (recordsDirectoryRaw.isEmpty())
    {
        recordsDirectoryRaw = "records";
    }
    int recordingCapMB = CoreSettingsGetDefaultIntValue(SettingsID::Kaillera_RecordingCapMB);
    if (recordingCapMB < 1)
    {
        recordingCapMB = 1;
    }
    this->kailleraRecordByDefaultCheckBox->setChecked(
        CoreSettingsGetDefaultBoolValue(SettingsID::Kaillera_RecordingEnabled));
    this->kailleraRecordsDirectoryLineEdit->setProperty("rawPath", recordsDirectoryRaw);
    this->kailleraRecordsDirectoryLineEdit->setText(getDisplayDirectoryPath(recordsDirectoryRaw));
    this->kailleraRecordsDirectoryLineEdit->setToolTip(this->kailleraRecordsDirectoryLineEdit->text());
    this->kailleraRecordingCapEnabledCheckBox->setChecked(
        CoreSettingsGetDefaultBoolValue(SettingsID::Kaillera_RecordingCapEnabled));
    this->kailleraRecordingCapMBSpinBox->setValue(recordingCapMB);
    this->updateKailleraRecordingCapControls();
}

void SettingsDialog::loadDefaultHotkeySettings(void)
{
    this->commonHotkeySettings(SettingsDialogAction::LoadDefaultSettings);
}

void SettingsDialog::loadDefaultInterfaceGeneralSettings(void)
{
    this->confirmExitWhileInGameCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::GUI_ConfirmExitWhileInGame));
    this->statusBarMessageDurationSpinBox->setValue(CoreSettingsGetDefaultIntValue(SettingsID::GUI_StatusbarMessageDuration));
    const QList<ThemeOption> themeOptions = collectAvailableThemeOptions();
    const QString defaultTheme = QString::fromStdString(CoreSettingsGetDefaultStringValue(SettingsID::GUI_Theme));
    const bool showLegacy = isLegacyThemeValue(themeOptions, defaultTheme);
    const bool blocked = this->showLegacyThemesCheckBox->blockSignals(true);
    this->showLegacyThemesCheckBox->setChecked(showLegacy);
    this->showLegacyThemesCheckBox->blockSignals(blocked);
    populateThemeCombo(this->themeComboBox, themeOptions, showLegacy, defaultTheme);
    this->iconThemeComboBox->setCurrentText(QString::fromStdString(CoreSettingsGetDefaultStringValue(SettingsID::GUI_IconTheme)));
    this->languageComboBox->setCurrentIndex(0);
#ifdef UPDATER
    this->checkForUpdatesCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::GUI_CheckForUpdates));
#endif // UPDATER
}

void SettingsDialog::loadDefaultInterfaceEmulationSettings(void)
{
    this->pauseEmulationOnFocusCheckbox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::GUI_PauseEmulationOnFocusLoss));
    this->resumeEmulationOnFocusCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::GUI_ResumeEmulationOnFocus));
    this->hideCursorCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::GUI_HideCursorInEmulation));
    this->hideCursorFullscreenCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::GUI_HideCursorInFullscreenEmulation));
    this->automaticFullscreenCheckbox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::GUI_AutomaticFullscreen));
#ifdef _WIN32
    this->exclusiveFullscreenCheckBox->setChecked(false);
    this->betaFullscreenBackendCheckBox->setChecked(false);
    this->exclusiveMonitorComboBox->setCurrentIndex(0);
    this->exclusiveResolutionComboBox->setCurrentIndex(0);
    this->exclusiveRefreshRateComboBox->setCurrentIndex(0);
#endif
}

void SettingsDialog::loadDefaultInterfaceRomBrowserSettings(void)
{
    this->searchSubDirectoriesCheckbox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::RomBrowser_Recursive));
    this->romSearchLimitSpinBox->setValue(CoreSettingsGetDefaultIntValue(SettingsID::RomBrowser_MaxItems));
}

void SettingsDialog::loadDefaultInterfaceLogSettings(void)
{
    this->showVerboseLogMessagesCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::GUI_ShowVerboseLogMessages));
}

void SettingsDialog::loadDefaultInterfaceOSDSettings(void)
{
    this->osdEnabledCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::GUI_OnScreenDisplayEnabled));
#ifndef NETPLAY
    this->loadDefaultNetplayOverlaySettings();
#endif

    const std::vector<int> backgroundColor = CoreSettingsGetDefaultIntListValue(SettingsID::GUI_OnScreenDisplayBackgroundColor);
    const std::vector<int> textColor = CoreSettingsGetDefaultIntListValue(SettingsID::GUI_OnScreenDisplayTextColor);
    if (backgroundColor.size() == 4)
    {
        this->currentBackgroundColor = QColor(backgroundColor.at(0), backgroundColor.at(1), 
                                              backgroundColor.at(2), backgroundColor.at(3));
        this->chooseColor(this->changeBackgroundColorButton, &this->currentBackgroundColor, true);
    }
    if (textColor.size() == 4)
    {
        this->currentTextColor = QColor(textColor.at(0), textColor.at(1), textColor.at(2), textColor.at(3));
        this->chooseColor(this->changeTextColorButton, &this->currentTextColor, true);
    }

    this->updateOSDSettingsEnabledState();
}

void SettingsDialog::loadDefaultNetplayOverlaySettings(void)
{
    this->osdChatEnabledCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::GUI_OnScreenDisplayChatEnabled));
    this->osdLocationComboBox->setCurrentIndex(CoreSettingsGetDefaultIntValue(SettingsID::GUI_OnScreenDisplayLocation));
    this->osdVerticalPaddingSpinBox->setValue(CoreSettingsGetDefaultIntValue(SettingsID::GUI_OnScreenDisplayPaddingY));
    this->osdHorizontalPaddingSpinBox->setValue(CoreSettingsGetDefaultIntValue(SettingsID::GUI_OnScreenDisplayPaddingX));
    this->osdDurationSpinBox->setValue(CoreSettingsGetDefaultIntValue(SettingsID::GUI_OnScreenDisplayDuration));
    this->osdScaleDoubleSpinBox->setValue(CoreSettingsGetDefaultFloatValue(SettingsID::GUI_OnScreenDisplayScale));
    this->osdMaxMessagesSpinBox->setValue(CoreSettingsGetDefaultIntValue(SettingsID::GUI_OnScreenDisplayMaxMessages));
    this->osdKailleraPortLabelsCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::GUI_OnScreenDisplayKailleraPortLabels));
    this->osdLiveReplayStatusCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::GUI_OnScreenDisplayLiveReplayStatus));
    this->updateOSDSettingsEnabledState();
}

void SettingsDialog::loadDefaultInterfaceNetplaySettings(void)
{
    this->netplayNicknameLineEdit->setText(QString::fromStdString(CoreSettingsGetDefaultStringValue(SettingsID::Netplay_Nickname)));
    // Kaillera uses built-in server list, no need for custom URLs
    // this->netplayServerUrlLineEdit->setText(QString::fromStdString(CoreSettingsGetDefaultStringValue(SettingsID::Netplay_ServerJsonUrl)));
    // this->netplayDispatcherUrlLineEdit->setText(QString::fromStdString(CoreSettingsGetDefaultStringValue(SettingsID::Netplay_DispatcherUrl)));
}

void SettingsDialog::loadDefaultRollbackSettings(void)
{
    this->autoStartNetplayOnStartupCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::GUI_AutoStartNetplayOnStartup));
    this->rollbackHideLocationCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::Rollback_HideLocation));
    this->rollbackRememberInputDelayCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::Rollback_RememberInputDelay));
}

void SettingsDialog::loadDefaultDiagnosticsSettings(void)
{
    this->rollbackVerboseStatsCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::Rollback_VerboseStats));
    this->rollbackStallDiagnosticsCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::Rollback_StallDiagnostics));
    this->rollbackEmulationDiagnosticsCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::Rollback_EmulationDiagnostics));
    this->rollbackPacingTraceCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::Rollback_PacingTrace));
    this->rollbackEnableLocalTestingCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::Rollback_EnableLocalTesting));
    this->rollbackVerbosePifInputLoggingCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::Rollback_VerbosePifInputLogging));
    this->rollbackVerboseGlideInputLoggingCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::Rollback_VerboseGlideInputLogging));
    this->rollbackPingDiagnosticsCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::Rollback_PingDiagnostics));
}

void SettingsDialog::saveSettings(void)
{
    this->saveCoreSettings();
    if (this->showGameSettings)
    {
        // clean 'game settings'
        CoreSettingsDeleteSection(this->gameSection);
        this->saveGameSettings();
        this->saveGameCoreSettings();
        this->saveGamePluginSettings();
    }
    this->savePluginSettings();
    this->saveDirectorySettings();
    this->save64DDSettings();
    this->saveHotkeySettings();
    this->saveInterfaceGeneralSettings();
    this->saveInterfaceEmulationSettings();
    this->saveInterfaceRomBrowserSettings();
    this->saveInterfaceLogSettings();
    this->saveInterfaceOSDSettings();
    this->saveInterfaceNetplaySettings();
    this->saveRollbackSettings();
    CoreSettingsSave();
}

void SettingsDialog::saveCoreSettings(void)
{
    bool disableExtraMem = (this->coreMemorySizeComboBox->currentIndex() == 0);
    int counterFactor = this->coreCounterFactorComboBox->currentIndex() + 1;
    const int cpuEmulator = this->coreCpuEmulatorComboBox->currentIndex();
    const int videoCaptureBackend = this->coreVideoCaptureBackendComboBox->currentIndex();
    const int saveFilenameFormat = this->coreSaveFilenameFormatComboBox->currentIndex();
    int siDmaDuration = this->coreSiDmaDurationSpinBox->value();
    const bool randomizeInterrupt = this->coreRandomizeTimingCheckBox->isChecked();
    const bool usePIF = this->usePifRomGroupBox->isChecked();
    const QString ntscPifROM = this->ntscPifRomLineEdit->text();
    const QString palPifROM = this->palPifRomLineEdit->text();
    const bool overrideGameSettings = this->coreOverrideGameSettingsGroup->isChecked();

    CoreSettingsSetValue(SettingsID::CoreOverlay_CPU_Emulator, cpuEmulator);
    CoreSettingsSetValue(SettingsID::CoreOverlay_GbCameraVideoCaptureBackend1, std::string((videoCaptureBackend == 1) ? "sdl3" : ""));
    CoreSettingsSetValue(SettingsID::CoreOverLay_SaveFileNameFormat, saveFilenameFormat);
    CoreSettingsSetValue(SettingsID::CoreOverlay_RandomizeInterrupt, randomizeInterrupt);
    CoreSettingsSetValue(SettingsID::Core_PIF_Use, usePIF);
    CoreSettingsSetValue(SettingsID::Core_PIF_NTSC, ntscPifROM.toStdString());
    CoreSettingsSetValue(SettingsID::Core_PIF_PAL, palPifROM.toStdString());
    CoreSettingsSetValue(SettingsID::Core_OverrideGameSpecificSettings, overrideGameSettings);

    if (!overrideGameSettings)
    {
        disableExtraMem = false;
        counterFactor = 0;
        siDmaDuration = -1;
    }

    CoreSettingsSetValue(SettingsID::CoreOverlay_DisableExtraMem, disableExtraMem);
    CoreSettingsSetValue(SettingsID::CoreOverlay_CountPerOp, counterFactor);
    CoreSettingsSetValue(SettingsID::CoreOverlay_SiDmaDuration, siDmaDuration);
}

void SettingsDialog::saveGameSettings(void)
{
    const bool disableExtraMem = this->gameMemorySizeComboBox->currentIndex() == 0;
    const int saveType = this->gameSaveTypeComboBox->currentIndex();
    const int countPerOp = this->gameCounterFactorComboBox->currentIndex() + 1;
    const bool transferPak = this->gameTransferPakComboBox->currentIndex() != 0;
    const int siDmaDuration = this->gameSiDmaDurationSpinBox->value();

    if ((this->defaultGameSettings.DisableExtraMem != disableExtraMem) ||
        (this->defaultGameSettings.SaveType != saveType) ||
        (this->defaultGameSettings.CountPerOp != countPerOp) ||
        (this->defaultGameSettings.TransferPak != transferPak) ||
        (this->defaultGameSettings.SiDMADuration != siDmaDuration))
    {
        CoreSettingsSetValue(SettingsID::Game_OverrideSettings, this->gameSection, true);
        CoreSettingsSetValue(SettingsID::Game_TransferPak, this->gameSection, transferPak);
        CoreSettingsSetValue(SettingsID::Game_DisableExtraMem, this->gameSection, disableExtraMem);
        CoreSettingsSetValue(SettingsID::Game_SaveType, this->gameSection, saveType);
        CoreSettingsSetValue(SettingsID::Game_CountPerOp, this->gameSection, countPerOp);
        CoreSettingsSetValue(SettingsID::Game_SiDmaDuration, this->gameSection, siDmaDuration);
    }

    // update cache when needed
    if (!this->currentGameFile.isEmpty())
    {
        CoreRomSettings romSettings = this->currentGameSettings;
        romSettings.SaveType = saveType;
        romSettings.DisableExtraMem = disableExtraMem;
        romSettings.CountPerOp = countPerOp;
        romSettings.TransferPak = transferPak;
        romSettings.SiDMADuration = siDmaDuration;
        CoreUpdateCachedRomHeaderAndSettings(this->currentGameFile.toStdU32String(), this->currentGameType, this->currentGameHeader, this->defaultGameSettings, romSettings);
    }
}

void SettingsDialog::saveGameCoreSettings(void)
{
    const bool overrideEnabled = gameOverrideCoreSettingsGroupBox->isChecked();
    const int cpuEmulator = gameCoreCpuEmulatorComboBox->currentIndex();
    const int overclockingFactor = gameOverclockingFactorSpinBox->value();
    const bool randomizeInterrupt = gameRandomizeTimingCheckBox->isChecked();

    const bool defaultOverrideEnabled = CoreSettingsGetDefaultBoolValue(SettingsID::Game_OverrideCoreSettings);
    const int defaultCpuEmulator = CoreSettingsGetDefaultIntValue(SettingsID::Game_CPU_Emulator);
    const int defaultoverclockingFactor = CoreSettingsGetDefaultIntValue(SettingsID::Game_CountPerOpDenomPot);
    const bool defaultRandomizeInterrupt = CoreSettingsGetDefaultBoolValue(SettingsID::Game_RandomizeInterrupt);

    if ((defaultOverrideEnabled != overrideEnabled) ||
        (defaultCpuEmulator != cpuEmulator) ||
        (defaultoverclockingFactor != overclockingFactor) ||
        (defaultRandomizeInterrupt != randomizeInterrupt))
    {
        CoreSettingsSetValue(SettingsID::Game_OverrideCoreSettings, this->gameSection, overrideEnabled);
        CoreSettingsSetValue(SettingsID::Game_CPU_Emulator, this->gameSection, cpuEmulator);
        CoreSettingsSetValue(SettingsID::Game_CountPerOpDenomPot, this->gameSection, overclockingFactor);
        CoreSettingsSetValue(SettingsID::Game_RandomizeInterrupt, this->gameSection, randomizeInterrupt);
    }
}

void SettingsDialog::saveGamePluginSettings(void)
{
    QComboBox *comboBoxArray[] = {this->gameVideoPluginsComboBox, this->gameAudioPluginsComboBox,
                                   this->gameInputPluginsComboBox, this->gameRspPluginsComboBox};
    SettingsID settingsIdArray[] = {SettingsID::Game_GFX_Plugin, SettingsID::Game_AUDIO_Plugin,
                                     SettingsID::Game_INPUT_Plugin, SettingsID::Game_RSP_Plugin};
    QComboBox *comboBox;
    SettingsID id;

    for (int i = 0; i < 4; i++)
    {
        comboBox = comboBoxArray[i];
        id = settingsIdArray[i];

        if (comboBox->currentIndex() != 0)
        {
            CoreSettingsSetValue(id, this->gameSection, comboBox->currentData().toString().toStdString());
        }
    }
}

void SettingsDialog::savePluginSettings(void)
{
    QComboBox *comboBoxArray[] = {this->videoPluginsComboBox, this->audioPluginsComboBox, this->inputPluginsComboBox,
                                   this->rspPluginsComboBox};
    SettingsID settings[] = {SettingsID::Core_GFX_Plugin, SettingsID::Core_AUDIO_Plugin, 
                                SettingsID::Core_INPUT_Plugin, SettingsID::Core_RSP_Plugin};
    QComboBox *comboBox;
    SettingsID settingId;

    for (int i = 0; i < 4; i++)
    {
        comboBox = comboBoxArray[i];
        settingId = settings[i];

        CoreSettingsSetValue(settingId, comboBox->currentData().toString().toStdString());
    }
}

void SettingsDialog::saveDirectorySettings(void)
{
    CoreSettingsSetValue(SettingsID::Core_ScreenshotPath, this->screenshotDirLineEdit->text().toStdString());
    CoreSettingsSetValue(SettingsID::Core_SaveStatePath, this->saveStateDirLineEdit->text().toStdString());
    CoreSettingsSetValue(SettingsID::Core_SaveSRAMPath, this->saveSramDirLineEdit->text().toStdString());
}

void SettingsDialog::save64DDSettings(void)
{
    QString recordsDirectoryRaw = this->kailleraRecordsDirectoryLineEdit->property("rawPath").toString();
    if (recordsDirectoryRaw.isEmpty())
    {
        recordsDirectoryRaw = "records";
    }
    std::string recordsDirectory = recordsDirectoryRaw.toStdString();

    CoreSettingsSetValue(SettingsID::Kaillera_RecordingEnabled, this->kailleraRecordByDefaultCheckBox->isChecked());
    CoreSettingsSetValue(SettingsID::Kaillera_Port, this->kailleraPortSpinBox->value());
    CoreSettingsSetValue(SettingsID::Kaillera_RecordsDirectory, recordsDirectory);
    CoreSettingsSetValue(SettingsID::Kaillera_RecordingCapEnabled, this->kailleraRecordingCapEnabledCheckBox->isChecked());
    CoreSettingsSetValue(SettingsID::Kaillera_RecordingCapMB, this->kailleraRecordingCapMBSpinBox->value());
    CoreSettingsSetValue(SettingsID::Kaillera_FlashOnJoin, this->kailleraFlashOnJoinCheckBox->isChecked());
    CoreSettingsSetValue(SettingsID::Kaillera_BeepOnJoin, this->kailleraBeepOnJoinCheckBox->isChecked());

    // Keep recording default/cap behavior in sync immediately after settings changes.
    CoreRefreshKailleraRecordingStorageStatus();
}

void SettingsDialog::saveHotkeySettings(void)
{
    this->commonHotkeySettings(SettingsDialogAction::SaveSettings);
}

void SettingsDialog::saveInterfaceGeneralSettings(void)
{
    CoreSettingsSetValue(SettingsID::GUI_Theme, selectedThemeValue(this->themeComboBox).toStdString());
    CoreSettingsSetValue(SettingsID::GUI_IconTheme, this->iconThemeComboBox->currentText().toStdString());
    CoreSettingsSetValue(SettingsID::GUI_Language, this->languageComboBox->currentData().toString().toStdString());
    CoreSettingsSetValue(SettingsID::GUI_AutoStartNetplayOnStartup, this->autoStartNetplayOnStartupCheckBox->isChecked());
#ifdef UPDATER
    CoreSettingsSetValue(SettingsID::GUI_CheckForUpdates, this->checkForUpdatesCheckBox->isChecked());
#endif // UPDATER
}

void SettingsDialog::saveInterfaceEmulationSettings(void)
{
    CoreSettingsSetValue(SettingsID::GUI_HideCursorInEmulation, this->hideCursorCheckBox->isChecked());
    CoreSettingsSetValue(SettingsID::GUI_HideCursorInFullscreenEmulation, this->hideCursorFullscreenCheckBox->isChecked());
    CoreSettingsSetValue(SettingsID::GUI_PauseEmulationOnFocusLoss, this->pauseEmulationOnFocusCheckbox->isChecked());
    CoreSettingsSetValue(SettingsID::GUI_ResumeEmulationOnFocus, this->resumeEmulationOnFocusCheckBox->isChecked());
#ifdef _WIN32
    const bool useExperimentalFullscreen = this->betaFullscreenBackendCheckBox->isChecked();
    CoreSettingsSetValue(SettingsID::GUI_AutomaticFullscreen, this->automaticFullscreenCheckbox->isChecked() || useExperimentalFullscreen);
    CoreSettingsSetValue(SettingsID::GUI_ExclusiveFullscreen, this->exclusiveFullscreenCheckBox->isChecked());
    CoreSettingsSetValue(SettingsID::GUI_BetaFullscreenBackend, useExperimentalFullscreen);
    CoreSettingsSetValue(SettingsID::GUI_ExclusiveFullscreenMonitor, this->exclusiveMonitorComboBox->currentData().toString().toStdString());
    CoreSettingsSetValue(SettingsID::GUI_ExclusiveFullscreenResolution, this->exclusiveResolutionComboBox->currentData().toString().toStdString());
    CoreSettingsSetValue(SettingsID::GUI_ExclusiveFullscreenRefreshRate, this->exclusiveRefreshRateComboBox->currentData().toInt());
#else
    CoreSettingsSetValue(SettingsID::GUI_AutomaticFullscreen, this->automaticFullscreenCheckbox->isChecked());
#endif
    CoreSettingsSetValue(SettingsID::GUI_ConfirmExitWhileInGame, this->confirmExitWhileInGameCheckBox->isChecked());
    CoreSettingsSetValue(SettingsID::GUI_StatusbarMessageDuration, this->statusBarMessageDurationSpinBox->value());
    CoreSettingsSetValue(SettingsID::GUI_OpenGLES, this->openglTypeComboBox->currentIndex() == 1);
}

void SettingsDialog::saveInterfaceRomBrowserSettings(void)
{
    CoreSettingsSetValue(SettingsID::RomBrowser_Recursive, this->searchSubDirectoriesCheckbox->isChecked());
    CoreSettingsSetValue(SettingsID::RomBrowser_MaxItems, this->romSearchLimitSpinBox->value());
}

void SettingsDialog::saveInterfaceLogSettings(void)
{
    CoreSettingsSetValue(SettingsID::GUI_ShowVerboseLogMessages, this->showVerboseLogMessagesCheckBox->isChecked());
}

void SettingsDialog::saveInterfaceOSDSettings(void)
{
    CoreSettingsSetValue(SettingsID::GUI_OnScreenDisplayEnabled, this->osdEnabledCheckBox->isChecked());
    CoreSettingsSetValue(SettingsID::GUI_OnScreenDisplayChatEnabled, this->osdChatEnabledCheckBox->isChecked());
    CoreSettingsSetValue(SettingsID::GUI_OnScreenDisplayLocation, this->osdLocationComboBox->currentIndex());
    CoreSettingsSetValue(SettingsID::GUI_OnScreenDisplayPaddingY, this->osdVerticalPaddingSpinBox->value());
    CoreSettingsSetValue(SettingsID::GUI_OnScreenDisplayPaddingX, this->osdHorizontalPaddingSpinBox->value());
    CoreSettingsSetValue(SettingsID::GUI_OnScreenDisplayDuration, this->osdDurationSpinBox->value());
    CoreSettingsSetValue(SettingsID::GUI_OnScreenDisplayScale, static_cast<float>(this->osdScaleDoubleSpinBox->value()));
    CoreSettingsSetValue(SettingsID::GUI_OnScreenDisplayMaxMessages, this->osdMaxMessagesSpinBox->value());
    CoreSettingsSetValue(SettingsID::GUI_OnScreenDisplayKailleraPortLabels, this->osdKailleraPortLabelsCheckBox->isChecked());
    CoreSettingsSetValue(SettingsID::GUI_OnScreenDisplayLiveReplayStatus, this->osdLiveReplayStatusCheckBox->isChecked());
    CoreSettingsSetValue(SettingsID::GUI_OnScreenDisplayBackgroundColor, std::vector<int>({ this->currentBackgroundColor.red(),
                                                                                            this->currentBackgroundColor.green(),
                                                                                            this->currentBackgroundColor.blue(),
                                                                                            this->currentBackgroundColor.alpha()
                                                                                           }));
    CoreSettingsSetValue(SettingsID::GUI_OnScreenDisplayTextColor, std::vector<int>({ this->currentTextColor.red(),
                                                                                      this->currentTextColor.green(),
                                                                                      this->currentTextColor.blue(),
                                                                                      this->currentTextColor.alpha()
                                                                                    }));
}

void SettingsDialog::saveInterfaceNetplaySettings(void)
{
    CoreSettingsSetValue(SettingsID::Netplay_Nickname, this->netplayNicknameLineEdit->text().toStdString());
    // Kaillera uses built-in server list, no need for custom URLs
    // CoreSettingsSetValue(SettingsID::Netplay_ServerJsonUrl, this->netplayServerUrlLineEdit->text().toStdString());
    // CoreSettingsSetValue(SettingsID::Netplay_DispatcherUrl, this->netplayDispatcherUrlLineEdit->text().toStdString());
}

void SettingsDialog::saveRollbackSettings(void)
{
    CoreSettingsSetValue(SettingsID::Rollback_VerboseStats, this->rollbackVerboseStatsCheckBox->isChecked());
    CoreSettingsSetValue(SettingsID::Rollback_StallDiagnostics, this->rollbackStallDiagnosticsCheckBox->isChecked());
    CoreSettingsSetValue(SettingsID::Rollback_EmulationDiagnostics, this->rollbackEmulationDiagnosticsCheckBox->isChecked());
    CoreSettingsSetValue(SettingsID::Rollback_PacingTrace, this->rollbackPacingTraceCheckBox->isChecked());
    CoreSettingsSetValue(SettingsID::Rollback_EnableLocalTesting, this->rollbackEnableLocalTestingCheckBox->isChecked());
    CoreSettingsSetValue(SettingsID::Rollback_VerbosePifInputLogging, this->rollbackVerbosePifInputLoggingCheckBox->isChecked());
    CoreSettingsSetValue(SettingsID::Rollback_VerboseGlideInputLogging, this->rollbackVerboseGlideInputLoggingCheckBox->isChecked());
    CoreSettingsSetValue(SettingsID::Rollback_PingDiagnostics, this->rollbackPingDiagnosticsCheckBox->isChecked());
    CoreSettingsSetValue(SettingsID::Rollback_HideLocation, this->rollbackHideLocationCheckBox->isChecked());
    CoreSettingsSetValue(SettingsID::Rollback_RememberInputDelay, this->rollbackRememberInputDelayCheckBox->isChecked());
}

void SettingsDialog::commonHotkeySettings(SettingsDialogAction action)
{
    struct keybinding
    {
        KeybindButton* button;
        SettingsID settingId;
    };

    std::vector<keybinding> keybindings;

    std::vector<keybinding> keybindings_System =
    {
        { this->startRomKeyButton, SettingsID::KeyBinding_StartROM },
        { this->startComboKeyButton, SettingsID::KeyBinding_StartCombo },
        { this->shutdownKeyButton, SettingsID::KeyBinding_Shutdown },
        { this->refreshRomListKeyButton, SettingsID::KeyBinding_RefreshROMList },
        { this->exitKeyButton, SettingsID::KeyBinding_Exit },
        { this->softResetKeyButton, SettingsID::KeyBinding_SoftReset },
        { this->hardResetKeyButton, SettingsID::KeyBinding_HardReset },
        { this->pauseKeyButton, SettingsID::KeyBinding_Resume },
        { this->generateBitmapKeyButton, SettingsID::KeyBinding_Screenshot },
        { this->limitFPSKeyButton, SettingsID::KeyBinding_LimitFPS },
        { this->netplayChatKeyButton, SettingsID::KeyBinding_NetplayChat },
        { this->saveStateKeyButton, SettingsID::KeyBinding_SaveState },
        { this->saveAsKeyButton, SettingsID::KeyBinding_SaveAs },
        { this->loadStateKeyButton, SettingsID::KeyBinding_LoadState },
        { this->loadKeyButton, SettingsID::KeyBinding_Load },
        { this->cheatsKeyButton, SettingsID::KeyBinding_Cheats },
        { this->gsButtonKeyButton, SettingsID::KeyBinding_GSButton },
    };

    std::vector<keybinding> keybindings_SpeedFactor =
    {
        { this->speedFactor25KeyButton, SettingsID::KeyBinding_SpeedFactor25 },
        { this->speedFactor50KeyButton, SettingsID::KeyBinding_SpeedFactor50 },
        { this->speedFactor75KeyButton, SettingsID::KeyBinding_SpeedFactor75 },
        { this->speedFactor100KeyButton, SettingsID::KeyBinding_SpeedFactor100 },
        { this->speedFactor125KeyButton, SettingsID::KeyBinding_SpeedFactor125 },
        { this->speedFactor150KeyButton, SettingsID::KeyBinding_SpeedFactor150 },
        { this->speedFactor175KeyButton, SettingsID::KeyBinding_SpeedFactor175 },
        { this->speedFactor200KeyButton, SettingsID::KeyBinding_SpeedFactor200 },
        { this->speedFactor225KeyButton, SettingsID::KeyBinding_SpeedFactor225 },
        { this->speedFactor250KeyButton, SettingsID::KeyBinding_SpeedFactor250 },
        { this->speedFactor275KeyButton, SettingsID::KeyBinding_SpeedFactor275 },
        { this->speedFactor300KeyButton, SettingsID::KeyBinding_SpeedFactor300 },
    };

    std::vector<keybinding> keybindings_CurrentSaveState =
    {
        { this->saveState0KeyButton, SettingsID::KeyBinding_SaveStateSlot0 },
        { this->saveState1KeyButton, SettingsID::KeyBinding_SaveStateSlot1 },
        { this->saveState2KeyButton, SettingsID::KeyBinding_SaveStateSlot2 },
        { this->saveState3KeyButton, SettingsID::KeyBinding_SaveStateSlot3 },
        { this->saveState4KeyButton, SettingsID::KeyBinding_SaveStateSlot4 },
        { this->saveState5KeyButton, SettingsID::KeyBinding_SaveStateSlot5 },
        { this->saveState6KeyButton, SettingsID::KeyBinding_SaveStateSlot6 },
        { this->saveState7KeyButton, SettingsID::KeyBinding_SaveStateSlot7 },
        { this->saveState8KeyButton, SettingsID::KeyBinding_SaveStateSlot8 },
        { this->saveState9KeyButton, SettingsID::KeyBinding_SaveStateSlot9 },
    };

    std::vector<keybinding> keybindings_Audio =
    {
        { this->increaseVolumeKeyButton, SettingsID::KeyBinding_IncreaseVolume },
        { this->decreaseVolumeKeyButton, SettingsID::KeyBinding_DecreaseVolume },
        { this->muteVolumeKeyButton, SettingsID::KeyBinding_ToggleMuteVolume },
    };

    std::vector<keybinding> keybindings_Settings =
    {
        { this->graphicsSettingsKeyButton, SettingsID::KeyBinding_GraphicsSettings },
        { this->audioSettingsKeyButton, SettingsID::KeyBinding_AudioSettings },
        { this->rspSettingsKeyButton, SettingsID::KeyBinding_RspSettings },
        { this->inputSettingsKeyButton, SettingsID::KeyBinding_InputSettings },
        { this->settingsKeyButton, SettingsID::KeyBinding_Settings },
    };

    std::vector<keybinding> keybindings_View =
    {
        { this->fullscreenKeyButton, SettingsID::KeyBinding_Fullscreen },
        { this->logKeyButton, SettingsID::KeyBinding_ViewLog },
        { this->refreshRomListKeyButton, SettingsID::KeyBinding_RefreshROMList },
        { this->searchKeyButton, SettingsID::KeyBinding_ViewSearch }
    };


    // Reset this page covers every shortcuts tab, just like load and save.
    keybindings.insert(keybindings.end(), keybindings_System.begin(), keybindings_System.end());
    keybindings.insert(keybindings.end(), keybindings_SpeedFactor.begin(), keybindings_SpeedFactor.end());
    keybindings.insert(keybindings.end(), keybindings_CurrentSaveState.begin(), keybindings_CurrentSaveState.end());
    keybindings.insert(keybindings.end(), keybindings_Audio.begin(), keybindings_Audio.end());
    keybindings.insert(keybindings.end(), keybindings_Settings.begin(), keybindings_Settings.end());
    keybindings.insert(keybindings.end(), keybindings_View.begin(), keybindings_View.end());

    switch (action)
    {
        default:
            break;
        case SettingsDialogAction::LoadSettings:
            this->removeDuplicateHotkeysCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::KeyBinding_RemoveDuplicates));
            break;
        case SettingsDialogAction::LoadDefaultSettings:
            this->removeDuplicateHotkeysCheckBox->setChecked(CoreSettingsGetDefaultBoolValue(SettingsID::KeyBinding_RemoveDuplicates));
            break;
        case SettingsDialogAction::SaveSettings:
            CoreSettingsSetValue(SettingsID::KeyBinding_RemoveDuplicates, this->removeDuplicateHotkeysCheckBox->isChecked());
            break;
    }

    for (const auto& keybinding : keybindings)
    {
        switch (action)
        {
        default:
        case SettingsDialogAction::ConnectSignals:
            connect(keybinding.button, &KeybindButton::KeybindingChanged, this, &SettingsDialog::on_KeybindButton_KeybindingChanged);
            connect(keybinding.button, &KeybindButton::Clicked, this, &SettingsDialog::on_KeybindButton_Clicked);
            break;
        case SettingsDialogAction::LoadSettings:
            keybinding.button->SetText(QString::fromStdString(CoreSettingsGetStringValue(keybinding.settingId)));
            break;
        case SettingsDialogAction::LoadDefaultSettings:
            keybinding.button->SetText(QString::fromStdString(CoreSettingsGetDefaultStringValue(keybinding.settingId)));
            break;
        case SettingsDialogAction::SaveSettings:
            CoreSettingsSetValue(keybinding.settingId, keybinding.button->GetCurrentText().toStdString());
            break;
        }
    }
}

QString SettingsDialog::getInputPluginDisplayName(const QString& fileName, const QString& originalName)
{
    // Map input plugin filenames to friendly display names
    if (fileName.contains("RMG-Input-GCA", Qt::CaseInsensitive))
    {
        return tr("GameCube Adapter");
    }
    else if (fileName.contains("RMG-Input", Qt::CaseInsensitive))
    {
        return tr("Generic USB Input");
    }
    else if (fileName.contains("raphnetraw", Qt::CaseInsensitive))
    {
        return tr("Raphnetraw N64 Adapter");
    }
    // Return original name for unknown plugins
    return originalName;
}

void SettingsDialog::commonPluginSettings(SettingsDialogAction action)
{
    QComboBox *comboBoxArray[] = {this->rspPluginsComboBox, this->videoPluginsComboBox,
                                    this->audioPluginsComboBox, this->inputPluginsComboBox};
    SettingsID settingsIdArray[] = {SettingsID::Core_RSP_Plugin, SettingsID::Core_GFX_Plugin, 
                                    SettingsID::Core_AUDIO_Plugin, SettingsID::Core_INPUT_Plugin};
    bool pluginFound[] = {false, false, false, false};
    QString pluginFileNames[4];

    QComboBox *comboBox;
    QString pluginName;
    QString pluginFileName;
    int index = 0;

    // clear combobox items
    for (const auto& c : comboBoxArray)
    {
        c->clear();
    }

    for (int i = 0; i < 4; i++)
    {
        pluginFileName = action == SettingsDialogAction::LoadSettings ? 
                            QString::fromStdString(CoreSettingsGetStringValue(settingsIdArray[i])) :
                            QString::fromStdString(CoreSettingsGetDefaultStringValue(settingsIdArray[i]));

        // account for full path (<v0.3.5 we used the full path)
        pluginFileName = QFileInfo(pluginFileName).fileName();
        
        pluginFileNames[i] = pluginFileName;
    }

    for (const auto &p : this->pluginList)
    {
        index = (static_cast<int>(p.Type) - 1);
        comboBox = comboBoxArray[index];
        pluginFileName = pluginFileNames[index];
        pluginName = QString::fromStdString(p.Name);

        // Use friendly display names for input plugins (index 3)
        if (index == 3)
        {
            pluginName = getInputPluginDisplayName(QString::fromStdString(p.File), pluginName);
        }

        comboBox->addItem(pluginName, QString::fromStdString(p.File));

        if (pluginFileName == QString::fromStdString(p.File))
        {
            comboBox->setCurrentText(pluginName);
            pluginFound[index] = true;
        }
    }

    for (int i = 0; i < 4; i++)
    {
        comboBox = comboBoxArray[i];
        if (!pluginFound[i])
        {
            pluginName = pluginFileNames[i] + " (not found)";

            comboBox->addItem(pluginName, pluginFileNames[i]);
            comboBox->setCurrentText(pluginName);
        }
        else
        { // find duplicates and append filename
            for (int i = 0; i < comboBox->count(); i++)
            {
                pluginName = comboBox->itemText(i);
                bool foundDuplicate = false;
                for (int x = i + 1; x < comboBox->count(); x++)
                {
                    if (comboBox->itemText(x) == pluginName)
                    {
                        pluginFileName = QFileInfo(comboBox->itemData(x).toString()).fileName();
                        comboBox->setItemText(x, pluginName + " (" + pluginFileName + ")");
                        foundDuplicate = true;
                    }
                }
                if (foundDuplicate)
                {
                    pluginFileName = QFileInfo(comboBox->itemData(i).toString()).fileName();
                    comboBox->setItemText(i, pluginName + " (" + pluginFileName + ")");
                }
            }
        }
    }
}

void SettingsDialog::setIconsForEmulationInfoText(void)
{
    QLabel* labels[] = {
        this->infoIconLabel_0, this->infoIconLabel_1, this->infoIconLabel_2,
        this->infoIconLabel_3, this->infoIconLabel_5,
        this->infoIconLabel_7, this->infoIconLabel_6, this->infoIconLabel_8
    };

    QIcon infoIcon = QIcon::fromTheme("information-line");
    QPixmap infoIconPixmap = infoIcon.pixmap(16, 16);

    for (QLabel* label : labels)
    {
        label->setPixmap(infoIconPixmap);
    }
}

void SettingsDialog::hideEmulationInfoText(void)
{
    QHBoxLayout *layouts[] = {this->emulationInfoLayout_0, this->emulationInfoLayout_1, 
                                this->emulationInfoLayout_2,
                                this->emulationInfoLayout_9};

    for (const auto &layout : layouts)
    {
        for (int i = 0; i < layout->count(); i++)
        {
            QWidget *widget = layout->itemAt(i)->widget();
            widget->hide();
        }
    }
}

#ifdef _WIN32
void SettingsDialog::setExclusiveFullscreenControlsEnabled(bool enabled)
{
    this->betaFullscreenBackendCheckBox->setEnabled(enabled);
    this->exclusiveMonitorLabel->setEnabled(enabled);
    this->exclusiveMonitorComboBox->setEnabled(enabled);
    this->exclusiveResolutionLabel->setEnabled(enabled);
    this->exclusiveResolutionComboBox->setEnabled(enabled);
    this->exclusiveRefreshRateLabel->setEnabled(enabled);
    this->exclusiveRefreshRateComboBox->setEnabled(enabled);
}
#endif

void SettingsDialog::updateKailleraRecordingCapControls(void)
{
    const bool recordingByDefault = this->kailleraRecordByDefaultCheckBox->isChecked();
    const bool capEnabled = recordingByDefault && this->kailleraRecordingCapEnabledCheckBox->isChecked();

    this->kailleraRecordingCapEnabledCheckBox->setEnabled(recordingByDefault);
    this->kailleraRecordingCapMBSpinBox->setEnabled(capEnabled);
}

void SettingsDialog::updateOSDSettingsEnabledState(void)
{
    const bool osdEnabled = this->osdEnabledCheckBox->isChecked();
    const bool chatEnabled = this->osdChatEnabledCheckBox->isChecked();

    this->osdGlobalSettings->setEnabled(osdEnabled);
    this->osdChatEnabledCheckBox->setEnabled(osdEnabled);
    this->osdChatSettings->setEnabled(osdEnabled && chatEnabled);
    this->osdKailleraPortLabelsCheckBox->setEnabled(osdEnabled);
    this->osdLiveReplayStatusCheckBox->setEnabled(osdEnabled);
}

void SettingsDialog::chooseDirectory(QLineEdit *lineEdit, QString caption)
{
    QString currentDir = lineEdit->text();
    if (!QDir(currentDir).exists())
    {
        currentDir = "";
    }

    QString dir = QFileDialog::getExistingDirectory(this, caption, currentDir);
    if (dir.isEmpty())
    {
        return;
    }

    lineEdit->setText(QDir::toNativeSeparators(dir));
}

void SettingsDialog::chooseFile(QLineEdit *lineEdit, QString caption, QString filter, QStringList md5List)
{
    QString file = QFileDialog::getOpenFileName(this, caption, "", filter);
    if (file.isEmpty())
    {
        return;
    }

    if (!md5List.isEmpty())
    {
        QFile qFile(file);
        if (!qFile.open(QFile::ReadOnly))
        {
            QtMessageBox::Error(this, "Failed to open file", "QFile::open() Failed");
            return;
        }

        QCryptographicHash hash(QCryptographicHash::Algorithm::Md5);
        if (!hash.addData(&qFile))
        {
            QtMessageBox::Error(this, "MD5 hashing failed", "QCryptographicHash::addData() Failed");
            return;
        }

        QString md5Hash = QString(hash.result().toHex());

        bool hashFound = false;
        for (const QString& md5 : md5List)
        {
            if (md5Hash == md5)
            {
                hashFound = true;
                break;
            }
        }

        if (!hashFound)
        {
            QtMessageBox::Error(this, "MD5 mismatch", "Expected file with MD5: \"" + md5List.first() + "\"");
            return;
        }
    }

    lineEdit->setText(QDir::toNativeSeparators(file));
}

void SettingsDialog::chooseColor(QPushButton *pushButton, QColor *initialColor, bool skipChoice)
{
    QColor color = QColor::Invalid;

    if (skipChoice)
    {
        color = *initialColor;
    }
    else
    {
        color = QColorDialog::getColor(*initialColor, this, "", QColorDialog::ColorDialogOption::ShowAlphaChannel);
    }

    if (!color.isValid())
    {
        return;
    }

    pushButton->setStyleSheet("background-color: " + color.name());
    *initialColor = color;
}

bool SettingsDialog::applyPluginSettings(void)
{
    // attempt to apply plugin settings when emulation
    // isn't running, when it fails, show the user the error and
    // don't allow the user to save invalid settings
    if (!CoreIsEmulationPaused() && !CoreIsEmulationRunning())
    {
        if (!CoreApplyPluginSettings())
        {
            QtMessageBox::Error(this, "CoreApplyPluginSettings() Failed", QString::fromStdString(CoreGetError()));
            return false;
        }
    }
    return true;
}

void SettingsDialog::closeEvent(QCloseEvent* event)
{
    if (this->applyPluginSettings())
    {
        event->accept();
    }
    else
    {
        event->ignore();
    }
}

void SettingsDialog::timerEvent(QTimerEvent* event)
{
    this->keybindButtonTimeLeft--;

    if (this->currentKeybindButton != nullptr)
    {
        this->currentKeybindButton->SetSecondsLeft(this->keybindButtonTimeLeft);
    }

    if (this->keybindButtonTimeLeft == 0)
    {
        this->killTimer(this->keybindButtonTimerId);
        this->keybindButtonTimerId = -1;
        this->currentKeybindButton = nullptr;
    }
}

void SettingsDialog::on_buttonBox_clicked(QAbstractButton *button)
{
    QPushButton *pushButton = (QPushButton *)button;
    QPushButton *defaultButton = this->buttonBox->button(QDialogButtonBox::RestoreDefaults);
    QPushButton *cancelButton = this->buttonBox->button(QDialogButtonBox::Cancel);
    QPushButton *okButton = this->buttonBox->button(QDialogButtonBox::Ok);


    if (pushButton == cancelButton || pushButton == okButton)
    {
        if (pushButton == okButton)
        {
            this->saveSettings();

            // re-load OSD settings
            OnScreenDisplayLoadSettings();
        }

        if (!this->applyPluginSettings())
        {
            return;
        }
    }

    if (pushButton == defaultButton)
    {
        this->restoreDefaults(this->currentIndex());
        if (auto* item = this->settingsNavigation->currentItem())
        {
            for (const auto& scope : item->data(0, Qt::UserRole + 3).toList())
                this->restoreDefaults(scope.toInt());
        }
    }
    else if (pushButton == cancelButton)
    {
        this->reject();
    }
    else if (pushButton == okButton)
    {
        this->accept();
    }
}

void SettingsDialog::on_changeScreenShotDirButton_clicked(void)
{
    this->chooseDirectory(this->screenshotDirLineEdit, tr("Select Screenshot Directory"));
}

void SettingsDialog::on_changeSaveStateDirButton_clicked(void)
{
    this->chooseDirectory(this->saveStateDirLineEdit, tr("Select Save (State) Directory"));
}

void SettingsDialog::on_changeSaveSramDirButton_clicked(void)
{
    this->chooseDirectory(this->saveSramDirLineEdit, tr("Select Save (SRAM) Directory"));
}

void SettingsDialog::on_changeKailleraRecordsDirectoryButton_clicked(void)
{
    QString currentPathRaw = this->kailleraRecordsDirectoryLineEdit->property("rawPath").toString();
    if (currentPathRaw.isEmpty())
    {
        currentPathRaw = "records";
    }

    QString currentPath = currentPathRaw;
    if (!QDir(currentPath).isAbsolute())
    {
        currentPath = QDir::current().absoluteFilePath(currentPath);
    }

    QString dir = QFileDialog::getExistingDirectory(this, tr("Select Kaillera Records Directory"), currentPath);
    if (dir.isEmpty())
    {
        return;
    }

    QString nativePath = QDir::toNativeSeparators(dir);
    this->kailleraRecordsDirectoryLineEdit->setProperty("rawPath", nativePath);
    this->kailleraRecordsDirectoryLineEdit->setText(nativePath);
    this->kailleraRecordsDirectoryLineEdit->setToolTip(nativePath);
}

void SettingsDialog::on_kailleraRecordByDefaultCheckBox_toggled(bool checked)
{
    (void)checked;
    this->updateKailleraRecordingCapControls();
}

void SettingsDialog::on_kailleraRecordingCapEnabledCheckBox_toggled(bool checked)
{
    (void)checked;
    this->updateKailleraRecordingCapControls();
}

void SettingsDialog::on_changeBackgroundColorButton_clicked(void)
{
    this->chooseColor(this->changeBackgroundColorButton, &this->currentBackgroundColor);
}

void SettingsDialog::on_changeTextColorButton_clicked(void)
{
    this->chooseColor(this->changeTextColorButton, &this->currentTextColor);
}

void SettingsDialog::on_KeybindButton_KeybindingChanged(KeybindButton* button)
{
    if (this->keybindButtonTimerId != -1)
    {
        this->killTimer(this->keybindButtonTimerId);
        this->keybindButtonTimerId = -1;
    }

    if (this->currentKeybindButton != nullptr)
    {
        this->currentKeybindButton->Reset();
        this->currentKeybindButton = nullptr;
    }
   
    if (!this->removeDuplicateHotkeysCheckBox->isChecked())
    {
        return;
    }

    QString text = button->GetCurrentText();
    if (text.isEmpty())
    {
        return;
    }

    QList<KeybindButton*> rombrowserKeybindButtons =
    {
        this->startRomKeyButton,
        this->startComboKeyButton,
        this->shutdownKeyButton,
        this->exitKeyButton,
        this->graphicsSettingsKeyButton,
        this->audioSettingsKeyButton,
        this->rspSettingsKeyButton,
        this->inputSettingsKeyButton,
        this->settingsKeyButton,
        this->logKeyButton,
        this->refreshRomListKeyButton,
    };

    QList<KeybindButton*> emulationKeybindButtons =
    {
        this->shutdownKeyButton,
        this->softResetKeyButton,
        this->hardResetKeyButton,
        this->pauseKeyButton,
        this->generateBitmapKeyButton,
        this->limitFPSKeyButton,
        this->speedFactor25KeyButton,
        this->speedFactor50KeyButton,
        this->speedFactor75KeyButton,
        this->speedFactor100KeyButton,
        this->speedFactor125KeyButton,
        this->speedFactor150KeyButton,
        this->speedFactor175KeyButton,
        this->speedFactor200KeyButton,
        this->speedFactor225KeyButton,
        this->speedFactor250KeyButton,
        this->speedFactor275KeyButton,
        this->speedFactor300KeyButton,
        this->saveStateKeyButton,
        this->saveAsKeyButton, 
        this->loadStateKeyButton,
        this->loadKeyButton,
        this->cheatsKeyButton, 
        this->gsButtonKeyButton,
        this->saveState0KeyButton,
        this->saveState1KeyButton,
        this->saveState2KeyButton,
        this->saveState3KeyButton,
        this->saveState4KeyButton,
        this->saveState5KeyButton,
        this->saveState6KeyButton,
        this->saveState7KeyButton,
        this->saveState8KeyButton,
        this->saveState9KeyButton,
        this->increaseVolumeKeyButton,
        this->decreaseVolumeKeyButton,
        this->muteVolumeKeyButton,
        this->exitKeyButton,
        this->graphicsSettingsKeyButton,
        this->audioSettingsKeyButton,
        this->rspSettingsKeyButton,
        this->inputSettingsKeyButton,
        this->settingsKeyButton,
        this->logKeyButton,
        this->fullscreenKeyButton,
    };

    QList<KeybindButton*> keybindButtons;

    if (rombrowserKeybindButtons.contains(button))
    {
        keybindButtons.append(rombrowserKeybindButtons);
    }
    if (emulationKeybindButtons.contains(button))
    {
        keybindButtons.append(emulationKeybindButtons);
    }

    for (KeybindButton* keybindButton : keybindButtons)
    {
        if (keybindButton != button)
        {
            if (keybindButton->GetCurrentText() == text)
            {
                keybindButton->Clear();
            }
        }
    }
}

void SettingsDialog::on_KeybindButton_Clicked(KeybindButton* button)
{
    if (this->currentKeybindButton == button)
    {
        return;
    }

    if (this->currentKeybindButton != nullptr)
    {
        this->currentKeybindButton->Reset();
        this->currentKeybindButton = nullptr;
    }

    if (this->keybindButtonTimerId != -1)
    {
        this->killTimer(this->keybindButtonTimerId);
        this->keybindButtonTimerId = -1;
    }

    this->currentKeybindButton  = button;
    this->keybindButtonTimeLeft = 5;
    this->keybindButtonTimerId  = this->startTimer(1000);

    // notify button
    this->currentKeybindButton->SetSecondsLeft(5);
}

void SettingsDialog::on_coreCpuEmulatorComboBox_currentIndexChanged(int index)
{
    // hide PIF ROM options when using dynamic recompiler
    // because the dynarec crashes when using a PIF ROM
    this->usePifRomGroupBox->setVisible(index != 2);
}

void SettingsDialog::on_changeNTSCPifRomButton_clicked(void)
{
    this->chooseFile(this->ntscPifRomLineEdit, tr("Open NTSC PIF ROM"), tr("PIF ROMs (*.rom)"), { "5C124E7948ADA85DA603A522782940D0", "4921D5F2165DEE6E2496F4388C4C81DA" });
}

void SettingsDialog::on_changePALPifRomButton_clicked(void)
{
    this->chooseFile(this->palPifRomLineEdit, tr("Open PAL PIF ROM"), tr("PIF ROMs (*.rom)"), { "D4232DC935CAD0650AC2664D52281F3A", "2B6EEC586FAA43F3462333B844834554" });
}
