#ifndef FIRSTLAUNCHDIALOG_HPP
#define FIRSTLAUNCHDIALOG_HPP

#include <QDialog>
#include <QButtonGroup>
#include <QStringList>

#include "ui_FirstLaunchDialog.h"

namespace UserInterface
{
namespace Dialog
{
class FirstLaunchDialog : public QDialog, private Ui::FirstLaunchDialog
{
    Q_OBJECT

  public:
    enum class InputPluginType
    {
        USB = 0,
        Raphnet = 1,
        Gamecube = 2
    };
    Q_ENUM(InputPluginType)

    FirstLaunchDialog(QWidget* parent, InputPluginType currentPlugin, bool autoSelectRecommended = true);

    void SetSelectedPlugin(InputPluginType plugin);
    InputPluginType GetSelectedPlugin(void) const;
    bool ShouldRememberInputChoice(void) const { return this->result() == QDialog::Accepted && this->manualInputChoice; }

    void SetRomDirectory(const QString& directory);
    QString GetRomDirectory(void) const;

  private slots:
    void on_romDirectoryBrowseButton_clicked(void);

  private:
    enum class RecommendationStyle
    {
        Recommended,
        Advisory
    };

    struct InputDetectionReport
    {
        bool foundAnySdlDevice = false;
        bool foundRaphnet = false;
        bool foundNativeGamecube = false;
        bool foundBlockedNativeGamecube = false;
        bool foundUsbModeMayflash = false;
        bool foundOtherUsb = false;
        QStringList lines;
    };

    void clearRecommendationLabels(void);
    void setRecommendationLabel(InputPluginType plugin, const QString& reason, RecommendationStyle style);
    void updateDetectedRecommendationLabels(const InputDetectionReport& report);
    bool isPluginAvailable(InputPluginType plugin) const;
    InputPluginType availablePluginOrFallback(InputPluginType plugin) const;
    void updateAvailableControllerOptions(void);
    void setSelectedPluginInternal(InputPluginType plugin);
    void updateButtonStyles(void);
    void updateDetectedDevices(const InputDetectionReport& report);

    InputDetectionReport scanInputDevices(void) const;
    InputPluginType detectRecommendedPlugin(const InputDetectionReport& report, QString& reason, bool& hasRecommendation) const;

    QButtonGroup* pluginGroup = nullptr;
    InputPluginType selectedPlugin = InputPluginType::USB;
    InputPluginType recommendedPlugin = InputPluginType::USB;
    QString recommendedReason;
    InputDetectionReport detectionReport;
    bool hasRecommendation = false;
    bool manualInputChoice = false;
};
} // namespace Dialog
} // namespace UserInterface

#endif // FIRSTLAUNCHDIALOG_HPP
