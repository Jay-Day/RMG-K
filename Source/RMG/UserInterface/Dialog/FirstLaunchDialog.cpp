/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "FirstLaunchDialog.hpp"
#include "UnifiedInputDialog.hpp"


#include <QComboBox>
#include <QCheckBox>
#include <QFileDialog>
#include <QDir>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>

using namespace UserInterface::Dialog;

static void set_button_checked(QButtonGroup* group, int id)
{
    if (group == nullptr)
    {
        return;
    }

    QAbstractButton* button = group->button(id);
    if (button != nullptr)
    {
        button->setChecked(true);
    }
}

FirstLaunchDialog::FirstLaunchDialog(QWidget* parent, InputPluginType currentPlugin, bool autoSelectRecommended)
    : QDialog(parent),
      selectedPlugin(currentPlugin)
{
    this->setupUi(this);

    this->pluginGroup = new QButtonGroup(this);
    this->pluginGroup->setExclusive(true);

    auto addButton = [this](QToolButton* button, InputPluginType plugin)
    {
        button->setCheckable(true);
        this->pluginGroup->addButton(button, static_cast<int>(plugin));
    };

    addButton(this->gamecubeButton, InputPluginType::Gamecube);
    addButton(this->raphnetButton, InputPluginType::Raphnet);
    addButton(this->usbButton, InputPluginType::USB);

    connect(this->pluginGroup, &QButtonGroup::idClicked, this, [this](int id)
    {
        this->manualInputChoice = true;
        this->setSelectedPluginInternal(static_cast<InputPluginType>(id));
    });

    const QSize iconSize(112, 112);
    const QSize buttonMinSize(190, 164);
    const QSizePolicy buttonSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    const QSizePolicy labelSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);

    this->gamecubeButton->setIcon(QIcon(":/onboarding/gamecube.png"));
    this->gamecubeButton->setIconSize(iconSize);
    this->gamecubeButton->setMinimumSize(buttonMinSize);
    this->gamecubeButton->setSizePolicy(buttonSizePolicy);

    this->raphnetButton->setIcon(QIcon(":/onboarding/raphnet.png"));
    this->raphnetButton->setIconSize(iconSize);
    this->raphnetButton->setMinimumSize(buttonMinSize);
    this->raphnetButton->setSizePolicy(buttonSizePolicy);

    this->usbButton->setIcon(QIcon(":/onboarding/usb.png"));
    this->usbButton->setIconSize(iconSize);
    this->usbButton->setMinimumSize(buttonMinSize);
    this->usbButton->setSizePolicy(buttonSizePolicy);

    for (QLabel* label : {this->gamecubeRecommendedLabel, this->raphnetRecommendedLabel, this->usbRecommendedLabel})
    {
        label->setProperty("recommendedBadge", false);
        label->setProperty("advisoryBadge", false);
        label->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
        label->setWordWrap(true);
        label->setMinimumHeight(58);
        label->setSizePolicy(labelSizePolicy);
        label->setText(" ");
        label->setVisible(true);
    }

    this->romDirectoryLineEdit->setReadOnly(true);
    this->romDirectoryLineEdit->setMinimumHeight(30);
    this->romDirectoryBrowseButton->setMinimumHeight(30);

    this->detectedDevicesPlainTextEdit->setLineWrapMode(QPlainTextEdit::NoWrap);

    auto* details = new QCheckBox(tr("Show detected devices"), this);
    this->verticalLayout->insertWidget(this->verticalLayout->count() - 1, details);
    this->detectedDevicesPlainTextEdit->parentWidget()->setVisible(false);
    connect(details, &QCheckBox::toggled, this->detectedDevicesPlainTextEdit->parentWidget(), &QWidget::setVisible);
    connect(this->buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(this->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    if (QPushButton* okButton = this->buttonBox->button(QDialogButtonBox::Ok))
    {
        okButton->setText(tr("Continue to Input Settings"));
    }

    this->setStyleSheet(
        "QToolButton {"
        "  border: 1px solid #b8c0cc;"
        "  border-radius: 6px;"
        "  padding: 10px;"
        "  font-size: 13px;"
        "}"
        "QToolButton:checked {"
        "  border: 2px solid #2f74c0;"
        "  background: #eaf3ff;"
        "}"
        "QLabel[recommendedBadge=\"true\"] {"
        "  background: #e7f5eb;"
        "  color: #1f6f3a;"
        "  border-radius: 4px;"
        "  padding: 3px 8px;"
        "}"
        "QLabel[advisoryBadge=\"true\"] {"
        "  background: #fff3cd;"
        "  color: #7a4b00;"
        "  border-radius: 4px;"
        "  padding: 3px 8px;"
        "}"
        "QLineEdit {"
        "  padding: 6px;"
        "}"
    );

    this->detectionReport = this->scanInputDevices();
    this->updateAvailableControllerOptions();
    this->updateDetectedDevices(this->detectionReport);

    this->recommendedPlugin = this->detectRecommendedPlugin(this->detectionReport, this->recommendedReason, this->hasRecommendation);
    this->updateDetectedRecommendationLabels(this->detectionReport);

    InputPluginType initialPlugin = currentPlugin;
    if (autoSelectRecommended && this->hasRecommendation)
    {
        initialPlugin = this->recommendedPlugin;
    }
    initialPlugin = this->availablePluginOrFallback(initialPlugin);

    this->setSelectedPluginInternal(initialPlugin);
}

void FirstLaunchDialog::SetSelectedPlugin(InputPluginType plugin)
{
    this->setSelectedPluginInternal(plugin);
}

FirstLaunchDialog::InputPluginType FirstLaunchDialog::GetSelectedPlugin(void) const
{
    return this->selectedPlugin;
}

QString FirstLaunchDialog::GetRomDirectory(void) const
{
    return QDir::fromNativeSeparators(this->romDirectoryLineEdit->text());
}

void FirstLaunchDialog::SetRomDirectory(const QString& directory)
{
    this->romDirectoryLineEdit->setText(directory);
}

void FirstLaunchDialog::on_romDirectoryBrowseButton_clicked(void)
{
    QString currentDir = this->romDirectoryLineEdit->text();
    if (!QDir(currentDir).exists())
    {
        currentDir = "";
    }

    QString dir = QFileDialog::getExistingDirectory(this, tr("Select ROM Directory"), currentDir);
    if (dir.isEmpty())
    {
        return;
    }

    QString nativeDir = QDir::toNativeSeparators(dir);
    this->romDirectoryLineEdit->setText(nativeDir);

}

void FirstLaunchDialog::clearRecommendationLabels(void)
{
    QLabel* labels[] = {this->gamecubeRecommendedLabel, this->raphnetRecommendedLabel, this->usbRecommendedLabel};
    for (QLabel* label : labels)
    {
        label->setText(" ");
        label->setProperty("recommendedBadge", false);
        label->setProperty("advisoryBadge", false);
        label->style()->unpolish(label);
        label->style()->polish(label);
    }
}

void FirstLaunchDialog::setRecommendationLabel(InputPluginType plugin, const QString& reason, RecommendationStyle style)
{
    QToolButton* targetButton = nullptr;
    QLabel* targetLabel = nullptr;
    switch (plugin)
    {
    case InputPluginType::Gamecube:
        targetButton = this->gamecubeButton;
        targetLabel = this->gamecubeRecommendedLabel;
        break;
    case InputPluginType::Raphnet:
        targetButton = this->raphnetButton;
        targetLabel = this->raphnetRecommendedLabel;
        break;
    case InputPluginType::USB:
        targetButton = this->usbButton;
        targetLabel = this->usbRecommendedLabel;
        break;
    }

    if (targetLabel != nullptr && targetButton != nullptr && targetButton->isVisible())
    {
        targetLabel->setText(reason);
        targetLabel->setProperty("recommendedBadge", style == RecommendationStyle::Recommended);
        targetLabel->setProperty("advisoryBadge", style == RecommendationStyle::Advisory);
        targetLabel->setVisible(true);
        targetLabel->style()->unpolish(targetLabel);
        targetLabel->style()->polish(targetLabel);
    }
}

bool FirstLaunchDialog::isPluginAvailable(InputPluginType plugin) const
{
    switch (plugin)
    {
    case InputPluginType::Raphnet:
        return this->detectionReport.foundRaphnet;
    case InputPluginType::Gamecube:
        return this->detectionReport.foundNativeGamecube;
    case InputPluginType::USB:
    default:
        return true;
    }
}

FirstLaunchDialog::InputPluginType FirstLaunchDialog::availablePluginOrFallback(InputPluginType plugin) const
{
    if (this->isPluginAvailable(plugin))
    {
        return plugin;
    }

    if (this->detectionReport.foundRaphnet)
    {
        return InputPluginType::Raphnet;
    }
    if (this->detectionReport.foundNativeGamecube)
    {
        return InputPluginType::Gamecube;
    }

    return InputPluginType::USB;
}

void FirstLaunchDialog::updateAvailableControllerOptions(void)
{
    this->raphnetButton->setVisible(this->detectionReport.foundRaphnet);
    this->raphnetRecommendedLabel->setVisible(this->detectionReport.foundRaphnet);
    this->gamecubeButton->setVisible(this->detectionReport.foundNativeGamecube);
    this->gamecubeRecommendedLabel->setVisible(this->detectionReport.foundNativeGamecube);
    this->usbButton->setVisible(true);
    this->usbRecommendedLabel->setVisible(true);
}

void FirstLaunchDialog::updateDetectedRecommendationLabels(const InputDetectionReport& report)
{
    this->clearRecommendationLabels();

    if (report.foundRaphnet)
    {
        this->setRecommendationLabel(InputPluginType::Raphnet,
            tr("Recommended: raphnet adapter detected"), RecommendationStyle::Recommended);
    }

    if (report.foundNativeGamecube)
    {
        this->setRecommendationLabel(InputPluginType::Gamecube,
            tr("Recommended: GameCube adapter detected in native mode"), RecommendationStyle::Recommended);
    }
    else if (report.foundBlockedNativeGamecube)
    {
        this->setRecommendationLabel(InputPluginType::Gamecube,
            tr("Wii U/NS (native) adapter detected, but driver is missing"), RecommendationStyle::Advisory);
    }

    if (report.foundUsbModeMayflash)
    {
        this->setRecommendationLabel(InputPluginType::USB,
            tr("Mayflash USB mode detected; switch to Wii U/NS (native) mode for better support"),
            RecommendationStyle::Advisory);
    }
    else if (report.foundOtherUsb)
    {
        this->setRecommendationLabel(InputPluginType::USB,
            tr("Recommended: USB controller detected"), RecommendationStyle::Recommended);
    }
}

void FirstLaunchDialog::setSelectedPluginInternal(InputPluginType plugin)
{
    plugin = this->availablePluginOrFallback(plugin);

    if (this->selectedPlugin != plugin)
    {
        this->selectedPlugin = plugin;
    }

    set_button_checked(this->pluginGroup, static_cast<int>(plugin));
    this->updateButtonStyles();

}

void FirstLaunchDialog::updateButtonStyles(void)
{
    for (QToolButton* button : {this->gamecubeButton, this->raphnetButton, this->usbButton})
    {
        button->style()->unpolish(button);
        button->style()->polish(button);
    }
}

void FirstLaunchDialog::updateDetectedDevices(const InputDetectionReport& report)
{
    this->detectedDevicesPlainTextEdit->setPlainText(report.lines.join(QStringLiteral("\n")));
}

FirstLaunchDialog::InputDetectionReport FirstLaunchDialog::scanInputDevices(void) const
{
    const auto report = UnifiedInputDialog::ScanInputDevices();
    return { report.foundAnySdlDevice, report.foundRaphnet, report.foundNativeGamecube,
        report.foundBlockedNativeGamecube, report.foundUsbModeMayflash, report.foundOtherUsb, report.lines };
}

FirstLaunchDialog::InputPluginType FirstLaunchDialog::detectRecommendedPlugin(
    const InputDetectionReport& report, QString& reason, bool& hasRecommendation) const
{
    hasRecommendation = false;
    reason.clear();

    if (report.foundRaphnet)
    {
        hasRecommendation = true;
        reason = tr("Recommended: raphnet adapter detected");
        return InputPluginType::Raphnet;
    }

    if (report.foundNativeGamecube)
    {
        hasRecommendation = true;
        reason = tr("Recommended: GameCube adapter detected in native mode");
        return InputPluginType::Gamecube;
    }

    if (report.foundAnySdlDevice)
    {
        hasRecommendation = true;
        reason = tr("Recommended: USB controller detected");
        return InputPluginType::USB;
    }

    return InputPluginType::USB;
}
