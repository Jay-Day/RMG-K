#ifndef UNIFIEDINPUTDIALOG_HPP
#define UNIFIEDINPUTDIALOG_HPP

#include "RaphnetPollingHealth.hpp"

#include <common.hpp>

#include <QVector>
#include <QElapsedTimer>
#include <QDialog>
#include <QStringList>
#include <QSet>
#include <cstdint>
#include <optional>
#include <unordered_map>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QGroupBox;
class QKeyEvent;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QSlider;
class QTabWidget;
class QTimer;
class QWidget;

struct hid_device_;
typedef struct hid_device_ hid_device;
typedef struct SDL_Gamepad SDL_Gamepad;
typedef struct SDL_Joystick SDL_Joystick;

struct libusb_context;
struct libusb_device_handle;

namespace UserInterface
{
namespace Widget
{
class ControllerImageWidget;
}

namespace Dialog
{
class UnifiedInputDialog : public QDialog
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

    enum class RecommendationStyle
    {
        None,
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

    struct Recommendation
    {
        InputPluginType plugin = InputPluginType::USB;
        QString reason;
        bool hasRecommendation = false;
        RecommendationStyle style = RecommendationStyle::None;
    };

    UnifiedInputDialog(QWidget* parent, InputPluginType currentPlugin);
    ~UnifiedInputDialog(void) override;

    InputPluginType GetSelectedPlugin(void) const;
    bool ShouldRememberInputChoice(void) const { return this->result() == QDialog::Accepted && this->manualInputChoice; }

    static InputDetectionReport ScanInputDevices(void);
    static bool IsUsbModeGamecubeAdapter(uint16_t vendorId, uint16_t productId, const QString& name);
    static Recommendation DetectRecommendedPlugin(const InputDetectionReport& report);
    static InputPluginType DetectStartupPlugin(InputPluginType currentPlugin, const InputDetectionReport& report,
        std::optional<InputPluginType> preferredPlugin = std::nullopt);

  private:
    enum class PreviewBackend
    {
        None,
        Raphnet,
        Gamecube,
        USB
    };

    void setupUi(void);
    QWidget* createControllerPage(int playerIndex);
    void refreshDetection(void);
    QStringList deviceTopology(void);
    void updateWarningLabel(void);
    void offerRaphnetInputType(int pageIndex);
    void refreshUsbDevices(void);
    void updateAllPages(void);
    void updateBackendChoices(void);
    void updatePageMode(int pageIndex);
    void updatePageDeviceChoices(int pageIndex);
    void updatePageBindingButtons(int pageIndex);
    void updateSliderLabels(int pageIndex);
    void setSelectedPlugin(InputPluginType plugin);
    int currentPageIndex(void) const;
    void saveAllSettings(void);
    void saveUsbSettings(int pageIndex);
    void syncSharedUsbProfile(int pageIndex);
    void saveGamecubeSettings(void);
    void saveRaphnetSettings(void);
    void loadPageSettings(int pageIndex);
    void restoreCurrentPageDefaults(void);

    void startListeningForBinding(int pageIndex, int bindingIndex);
    void stopListeningForBinding(bool restoreText);
    void clearBinding(int pageIndex, int bindingIndex);
    void setGamecubeTriggerAnalog(bool leftTrigger, bool analog);
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void done(int result) override;
    bool eventFilter(QObject* object, QEvent* event) override;

    void openPreviewSource(void);
    void closePreviewSource(void);
    void pollPreview(void);
    void clearPreview(void);

    bool openRaphnetPreview(void);
    bool pollRaphnetPreview(void);
    void updateRaphnetDiagnostics(void);
    bool setRaphnetPollingSuspended(bool suspended);
    bool exchangeRaphnetCommand(const unsigned char* command, int commandLength, unsigned char* response, int& responseLength);

    bool openGamecubePreview(void);
    bool pollGamecubePreview(void);

    bool openUsbPreview(void);
    bool pollUsbPreview(void);

  public:
    struct BindingValue
    {
        QVector<int> types;
        QVector<int> data;
        QVector<int> extraData;
        QVector<QString> text;
    };

    struct UsbDeviceChoice
    {
        InputDeviceType type = InputDeviceType::None;
        SDL_JoystickID id = 0;
        bool isGamepad = false;
        QString name;
        QString path;
        QString serial;
        uint16_t vendorId = 0;
        uint16_t productId = 0;
        bool connected = true;
    };

    struct ControllerPage
    {
        QWidget* widget = nullptr;
        QComboBox* backendComboBox = nullptr;
        QComboBox* deviceComboBox = nullptr;
        QLabel* statusLabel = nullptr;
        QCheckBox* pluggedInCheckBox = nullptr;
        QGroupBox* mappingsGroupBox = nullptr;
        QGroupBox* usbStickGroupBox = nullptr;
        QGroupBox* gamecubeStickGroupBox = nullptr;
        QGroupBox* gamecubeTriggerGroupBox = nullptr;
        QGroupBox* portGroupBox = nullptr;
        QWidget* messageArea = nullptr;
        QLabel* messages = nullptr;
        QVector<QPushButton*> bindingButtons;
        QVector<QPushButton*> clearButtons;
        QVector<BindingValue> usbBindings;
        QVector<int> gamecubeBindings;
        QSlider* usbDeadzoneSlider = nullptr;
        QSlider* usbRangeSlider = nullptr;
        QCheckBox* realN64RangeCheckBox = nullptr;
        QLabel* usbDeadzoneValueLabel = nullptr;
        QLabel* usbRangeValueLabel = nullptr;
        QSlider* gamecubeDeadzoneSlider = nullptr;
        QSlider* gamecubeSensitivitySlider = nullptr;
        QSlider* gamecubeTriggerThresholdSlider = nullptr;
        QRadioButton* gamecubeLeftTriggerDigitalRadioButton = nullptr;
        QRadioButton* gamecubeLeftTriggerAnalogRadioButton = nullptr;
        QRadioButton* gamecubeRightTriggerDigitalRadioButton = nullptr;
        QRadioButton* gamecubeRightTriggerAnalogRadioButton = nullptr;
        QLabel* gamecubeDeadzoneValueLabel = nullptr;
        QLabel* gamecubeSensitivityValueLabel = nullptr;
        QLabel* gamecubeTriggerThresholdValueLabel = nullptr;
        QLabel* axisXLabel = nullptr;
        QLabel* axisYLabel = nullptr;
        Widget::ControllerImageWidget* controllerImageWidget = nullptr;
        UsbDeviceChoice usbDevice;
        bool usbEnabled = false;
        bool usbDirty = false;
        bool gamecubeEnabled = false;
        int gamecubePort = 0;
        std::string usbSection;
    };

  private:
    InputPluginType selectedPlugin = InputPluginType::USB;
    InputDetectionReport detectionReport;
    PreviewBackend previewBackend = PreviewBackend::None;

    QVector<ControllerPage*> controllerPages;
    QVector<UsbDeviceChoice> usbDevices;
    QTabWidget* tabWidget = nullptr;
    QCheckBox* debugDevicesCheckBox = nullptr;
    QGroupBox* detectedDevicesGroupBox = nullptr;
    QPlainTextEdit* detectedDevicesPlainTextEdit = nullptr;
    QTimer* deviceTimer = nullptr;
    QStringList lastDeviceTopology;
    QDialogButtonBox* buttonBox = nullptr;
    QTimer* pollTimer = nullptr;
    QLabel* raphnetTimingLabel = nullptr;
    QElapsedTimer listeningTimer;
    int raphnetPlayer1Port = 0;
    RaphnetPollingHealth raphnetPollingHealth;
    QElapsedTimer raphnetMeasurementTimer;
    bool raphnetConnectionSlow = false;
    bool settingsLoaded = false;
    bool manualInputChoice = false;
    QSet<QString> raphnetUsbPrompts;
    int previousPageIndex = 0;
    uint8_t gamecubeEndpointIn = 0x81;
    uint8_t gamecubeEndpointOut = 0x02;

    hid_device* hidDevice = nullptr;
    int raphnetReportSize = 63;
    int raphnetChannelCount = 1;

    libusb_context* usbContext = nullptr;
    libusb_device_handle* gamecubeHandle = nullptr;
    bool gamecubeInterfaceClaimed = false;
    bool gamecubeSelectedPortMissingController = false;

    SDL_Gamepad* sdlGamepad = nullptr;
    SDL_Joystick* sdlJoystick = nullptr;

    int listeningPageIndex = -1;
    int listeningBindingIndex = -1;
    bool listeningArmed = false;
    std::unordered_map<int, bool> keyboardState;
};
} // namespace Dialog
} // namespace UserInterface

#endif // UNIFIEDINPUTDIALOG_HPP
