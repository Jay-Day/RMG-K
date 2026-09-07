#include "UnifiedInputDialog.hpp"

#include <UserInterface/Widget/ControllerImageWidget.hpp>
#include <Utilities/QtKeyToSdl3Key.hpp>

#include <RMG-Core/Settings.hpp>
#include <RMG-Core/Plugins.hpp>
#include <RMG-Input-GCA/ControllerPorts.hpp>

#include <SDL3/SDL.h>
#include <hidapi.h>
#include <libusb.h>

#include <QCheckBox>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScreen>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace
{
using InputPluginType = UserInterface::Dialog::UnifiedInputDialog::InputPluginType;
using BindingValue = UserInterface::Dialog::UnifiedInputDialog::BindingValue;
using ControllerPage = UserInterface::Dialog::UnifiedInputDialog::ControllerPage;
using UsbDeviceChoice = UserInterface::Dialog::UnifiedInputDialog::UsbDeviceChoice;

class ControllerMessageArea : public QWidget
{
  public:
    ControllerMessageArea(QWidget* parent, int playerIndex) : QWidget(parent)
    {
        this->setObjectName(QStringLiteral("controllerMessageArea%1").arg(playerIndex));
        QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Minimum);
        policy.setHeightForWidth(true);
        this->setSizePolicy(policy);
        this->box = new QFrame(this);
        this->box->setObjectName(QStringLiteral("controllerMessageBox%1").arg(playerIndex));
        this->box->setProperty("controllerWarningBox", true);
        auto* layout = new QHBoxLayout(this->box);
        layout->setContentsMargins(10, 4, 10, 4);
        layout->setSpacing(8);
        auto* icon = new QLabel(this->box);
        icon->setObjectName(QStringLiteral("controllerWarningIcon%1").arg(playerIndex));
        icon->setAccessibleName(tr("Warning"));
        icon->setPixmap(this->style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(20, 20));
        icon->setFixedSize(20, 20);
        layout->addWidget(icon, 0, Qt::AlignVCenter);
        this->messages = new QLabel(this->box);
        this->messages->setObjectName(QStringLiteral("controllerMessages%1").arg(playerIndex));
        this->messages->setAccessibleName(tr("Controller messages"));
        this->messages->setTextFormat(Qt::PlainText);
        this->messages->setWordWrap(true);
        this->messages->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        this->messages->setTextInteractionFlags(Qt::TextSelectableByMouse);
        this->messages->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        layout->addWidget(this->messages, 1);
        this->box->hide();
    }

    QLabel* Messages() const { return this->messages; }

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override
    {
        if (this->messages == nullptr || this->messages->text().isEmpty()) return 0;
        const int textWidth = std::max(1, std::min(width, this->naturalWidth()) - 48);
        return std::max(20, this->messages->heightForWidth(textWidth)) + 8;
    }
    QSize sizeHint() const override { return {this->naturalWidth(), this->heightForWidth(this->naturalWidth())}; }
    QSize minimumSizeHint() const override { return {0, 0}; }

    void SetMessages(const QString& message)
    {
        if (this->messages->text() == message) return;
        this->messages->setText(message);
        this->box->setVisible(!message.isEmpty());
        this->updateGeometry();
        this->positionBox();
    }

  protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        this->positionBox();
    }

    void changeEvent(QEvent* event) override
    {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::FontChange)
        {
            this->updateGeometry();
            this->positionBox();
        }
    }

  private:
    int naturalWidth() const
    {
        if (this->messages == nullptr) return 0;
        int textWidth = 0;
        const QFontMetrics metrics(this->messages->font());
        for (const auto& line : this->messages->text().split('\n'))
            textWidth = std::max(textWidth, metrics.boundingRect(line).width());
        // Include icon, spacing and comfortable padding around the actual text.
        return textWidth + 20 + 8 + 20 + 12;
    }

    void positionBox()
    {
        if (this->box == nullptr) return;
        this->box->setGeometry(0, 0, std::min(this->width(), this->naturalWidth()), this->height());
    }

    QFrame* box = nullptr;
    QLabel* messages = nullptr;
};

class ControllerPreviewPanel : public QWidget
{
  public:
    explicit ControllerPreviewPanel(QWidget* parent)
        : QWidget(parent)
    {
        this->setMinimumSize(240, 180);
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

        this->controllerImageWidget = new UserInterface::Widget::ControllerImageWidget(this);
        this->controllerImageWidget->setMinimumSize(240, 180);
        this->controllerImageWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    UserInterface::Widget::ControllerImageWidget* ControllerImage(void) const
    {
        return this->controllerImageWidget;
    }

    void SetOutputWidget(QWidget* widget)
    {
        this->outputWidget = widget;
        if (this->outputWidget != nullptr)
        {
            this->outputWidget->setParent(this);
            this->outputWidget->raise();
            this->positionOutputWidget();
        }
    }

  protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        this->controllerImageWidget->setGeometry(this->rect());
        this->positionOutputWidget();
    }

  private:
    void positionOutputWidget(void)
    {
        if (this->outputWidget == nullptr)
        {
            return;
        }

        constexpr double viewBoxWidth = 475.0;
        constexpr double viewBoxHeight = 450.0;
        constexpr double outputCenterX = 130.5;
        constexpr double outputCenterY = 291.1;

        if (this->width() <= 0 || this->height() <= 0)
        {
            return;
        }

        this->outputWidget->adjustSize();
        this->outputWidget->resize(this->outputWidget->sizeHint());
        const QSize outputSize = this->outputWidget->size();

        const double scale = std::min(
            static_cast<double>(this->width()) / viewBoxWidth,
            static_cast<double>(this->height()) / viewBoxHeight);
        const double imageWidth = viewBoxWidth * scale;
        const double imageHeight = viewBoxHeight * scale;
        const double imageLeft = (static_cast<double>(this->width()) - imageWidth) / 2.0;
        const double imageTop = (static_cast<double>(this->height()) - imageHeight) / 2.0;

        const int x = std::clamp(
            static_cast<int>(std::round(imageLeft + outputCenterX * scale - outputSize.width() / 2.0)),
            0,
            std::max(0, this->width() - outputSize.width()));
        const int y = std::clamp(
            static_cast<int>(std::round(imageTop + outputCenterY * scale - outputSize.height() / 2.0)),
            0,
            std::max(0, this->height() - outputSize.height()));
        this->outputWidget->move(x, y);
        this->outputWidget->raise();
    }

    UserInterface::Widget::ControllerImageWidget* controllerImageWidget = nullptr;
    QWidget* outputWidget = nullptr;
};

constexpr uint16_t kGameCubeAdapterVendorId = 0x057e;
constexpr uint16_t kGameCubeAdapterProductId = 0x0337;
constexpr uint8_t kGameCubeCommandPoll = 0x13;
constexpr double kGameCubeN64AxisPeak = 85.0;
constexpr double kGameCubeSensitivityOffset = 0.90;
constexpr double kGameCubeSensitivityPerPercent = 0.0045;
constexpr int kAxisMotionThreshold = SDL_AXIS_PEAK / 2;

constexpr uint16_t kRaphnetVendorId = 0x289b;
constexpr uint8_t kRaphnetRawSiCommand = 0x80;
constexpr uint8_t kRaphnetSuspendPolling = 0x03;
constexpr uint8_t kN64GetStatus = 0x01;

constexpr uint16_t kN64ButtonA = 0x8000;
constexpr uint16_t kN64ButtonB = 0x4000;
constexpr uint16_t kN64ButtonZ = 0x2000;
constexpr uint16_t kN64ButtonStart = 0x1000;
constexpr uint16_t kN64ButtonDUp = 0x0800;
constexpr uint16_t kN64ButtonDDown = 0x0400;
constexpr uint16_t kN64ButtonDLeft = 0x0200;
constexpr uint16_t kN64ButtonDRight = 0x0100;
constexpr uint16_t kN64ButtonL = 0x0020;
constexpr uint16_t kN64ButtonR = 0x0010;
constexpr uint16_t kN64ButtonCUp = 0x0008;
constexpr uint16_t kN64ButtonCDown = 0x0004;
constexpr uint16_t kN64ButtonCLeft = 0x0002;
constexpr uint16_t kN64ButtonCRight = 0x0001;

struct RaphnetAdapterDef
{
    uint16_t productId;
    int interfaceNumber;
    int rawChannels;
    int reportSize;
};

const RaphnetAdapterDef kRaphnetAdapters[] = {
    { 0x0017, 1, 1, 40 },
    { 0x001D, 1, 1, 40 },
    { 0x0020, 1, 1, 40 },
    { 0x0021, 1, 1, 40 },
    { 0x0022, 1, 2, 40 },
    { 0x0030, 1, 2, 40 },
    { 0x0031, 1, 2, 40 },
    { 0x0032, 1, 1, 63 },
    { 0x0033, 1, 1, 63 },
    { 0x0034, 1, 1, 63 },
    { 0x0035, 1, 2, 63 },
    { 0x0036, 1, 2, 63 },
    { 0x0037, 1, 2, 63 },
    { 0x0038, 1, 1, 63 },
    { 0x0039, 1, 1, 63 },
    { 0x003A, 1, 1, 63 },
    { 0x003B, 2, 2, 63 },
    { 0x003C, 2, 2, 63 },
    { 0x003D, 2, 2, 63 },
    { 0x0060, 1, 1, 63 },
    { 0x0061, 1, 1, 63 },
    { 0x0063, 2, 2, 63 },
    { 0x0064, 2, 2, 63 },
    { 0x0067, 1, 1, 63 },
};

enum class GCInput : int
{
    None = -1,
    A = 0,
    B,
    X,
    Y,
    Z,
    Start,
    L,
    R,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    LeftTrigger,
    RightTrigger,
    CStickUp,
    CStickDown,
    CStickLeft,
    CStickRight
};

struct GameCubeState
{
    uint8_t status = 0;
    uint8_t buttons1 = 0;
    uint8_t buttons2 = 0;
    uint8_t leftStickX = 128;
    uint8_t leftStickY = 128;
    uint8_t rightStickX = 128;
    uint8_t rightStickY = 128;
    uint8_t leftTrigger = 0;
    uint8_t rightTrigger = 0;
};

struct BindingTarget
{
    const char* label;
    N64ControllerButton imageButton;
    int axisXDirection;
    int axisYDirection;
    SettingsID usbInputType;
    SettingsID usbName;
    SettingsID usbData;
    SettingsID usbExtraData;
    bool hasGamecubeMapping;
    SettingsID gamecubeMapping;
};

const std::array<BindingTarget, 19> kBindingTargets = {{
    { "A", N64ControllerButton::A, 0, 0, SettingsID::Input_A_InputType, SettingsID::Input_A_Name, SettingsID::Input_A_Data, SettingsID::Input_A_ExtraData, true, SettingsID::GCAInput_Map_A },
    { "B", N64ControllerButton::B, 0, 0, SettingsID::Input_B_InputType, SettingsID::Input_B_Name, SettingsID::Input_B_Data, SettingsID::Input_B_ExtraData, true, SettingsID::GCAInput_Map_B },
    { "Start", N64ControllerButton::Start, 0, 0, SettingsID::Input_Start_InputType, SettingsID::Input_Start_Name, SettingsID::Input_Start_Data, SettingsID::Input_Start_ExtraData, true, SettingsID::GCAInput_Map_Start },
    { "Z", N64ControllerButton::ZTrigger, 0, 0, SettingsID::Input_ZTrigger_InputType, SettingsID::Input_ZTrigger_Name, SettingsID::Input_ZTrigger_Data, SettingsID::Input_ZTrigger_ExtraData, true, SettingsID::GCAInput_Map_Z },
    { "Z 2", N64ControllerButton::ZTrigger2, 0, 0, SettingsID::Input_ZTrigger2_InputType, SettingsID::Input_ZTrigger2_Name, SettingsID::Input_ZTrigger2_Data, SettingsID::Input_ZTrigger2_ExtraData, true, SettingsID::GCAInput_Map_Z2 },
    { "L", N64ControllerButton::LeftShoulder, 0, 0, SettingsID::Input_LeftShoulder_InputType, SettingsID::Input_LeftShoulder_Name, SettingsID::Input_LeftShoulder_Data, SettingsID::Input_LeftShoulder_ExtraData, true, SettingsID::GCAInput_Map_L },
    { "R", N64ControllerButton::RightShoulder, 0, 0, SettingsID::Input_RightShoulder_InputType, SettingsID::Input_RightShoulder_Name, SettingsID::Input_RightShoulder_Data, SettingsID::Input_RightShoulder_ExtraData, true, SettingsID::GCAInput_Map_R },
    { "D-Up", N64ControllerButton::DpadUp, 0, 0, SettingsID::Input_DpadUp_InputType, SettingsID::Input_DpadUp_Name, SettingsID::Input_DpadUp_Data, SettingsID::Input_DpadUp_ExtraData, true, SettingsID::GCAInput_Map_DpadUp },
    { "D-Down", N64ControllerButton::DpadDown, 0, 0, SettingsID::Input_DpadDown_InputType, SettingsID::Input_DpadDown_Name, SettingsID::Input_DpadDown_Data, SettingsID::Input_DpadDown_ExtraData, true, SettingsID::GCAInput_Map_DpadDown },
    { "D-Left", N64ControllerButton::DpadLeft, 0, 0, SettingsID::Input_DpadLeft_InputType, SettingsID::Input_DpadLeft_Name, SettingsID::Input_DpadLeft_Data, SettingsID::Input_DpadLeft_ExtraData, true, SettingsID::GCAInput_Map_DpadLeft },
    { "D-Right", N64ControllerButton::DpadRight, 0, 0, SettingsID::Input_DpadRight_InputType, SettingsID::Input_DpadRight_Name, SettingsID::Input_DpadRight_Data, SettingsID::Input_DpadRight_ExtraData, true, SettingsID::GCAInput_Map_DpadRight },
    { "C-Up", N64ControllerButton::CButtonUp, 0, 0, SettingsID::Input_CButtonUp_InputType, SettingsID::Input_CButtonUp_Name, SettingsID::Input_CButtonUp_Data, SettingsID::Input_CButtonUp_ExtraData, true, SettingsID::GCAInput_Map_CUp },
    { "C-Down", N64ControllerButton::CButtonDown, 0, 0, SettingsID::Input_CButtonDown_InputType, SettingsID::Input_CButtonDown_Name, SettingsID::Input_CButtonDown_Data, SettingsID::Input_CButtonDown_ExtraData, true, SettingsID::GCAInput_Map_CDown },
    { "C-Left", N64ControllerButton::CButtonLeft, 0, 0, SettingsID::Input_CButtonLeft_InputType, SettingsID::Input_CButtonLeft_Name, SettingsID::Input_CButtonLeft_Data, SettingsID::Input_CButtonLeft_ExtraData, true, SettingsID::GCAInput_Map_CLeft },
    { "C-Right", N64ControllerButton::CButtonRight, 0, 0, SettingsID::Input_CButtonRight_InputType, SettingsID::Input_CButtonRight_Name, SettingsID::Input_CButtonRight_Data, SettingsID::Input_CButtonRight_ExtraData, true, SettingsID::GCAInput_Map_CRight },
    { "Stick Up", N64ControllerButton::Invalid, 0, 1, SettingsID::Input_AnalogStickUp_InputType, SettingsID::Input_AnalogStickUp_Name, SettingsID::Input_AnalogStickUp_Data, SettingsID::Input_AnalogStickUp_ExtraData, false, SettingsID::GCAInput_Map_A },
    { "Stick Down", N64ControllerButton::Invalid, 0, -1, SettingsID::Input_AnalogStickDown_InputType, SettingsID::Input_AnalogStickDown_Name, SettingsID::Input_AnalogStickDown_Data, SettingsID::Input_AnalogStickDown_ExtraData, false, SettingsID::GCAInput_Map_A },
    { "Stick Left", N64ControllerButton::Invalid, -1, 0, SettingsID::Input_AnalogStickLeft_InputType, SettingsID::Input_AnalogStickLeft_Name, SettingsID::Input_AnalogStickLeft_Data, SettingsID::Input_AnalogStickLeft_ExtraData, false, SettingsID::GCAInput_Map_A },
    { "Stick Right", N64ControllerButton::Invalid, 1, 0, SettingsID::Input_AnalogStickRight_InputType, SettingsID::Input_AnalogStickRight_Name, SettingsID::Input_AnalogStickRight_Data, SettingsID::Input_AnalogStickRight_ExtraData, false, SettingsID::GCAInput_Map_A },
}};

const std::array<int, 4> kDpadBindingIndexes = {{ 7, 9, 10, 8 }};
const std::array<int, 4> kAnalogBindingIndexes = {{ 15, 17, 18, 16 }};
const std::array<int, 7> kButtonBindingIndexes = {{ 0, 1, 2, 3, 4, 5, 6 }};
const std::array<int, 4> kCButtonBindingIndexes = {{ 11, 13, 14, 12 }};

double gamecube_sensitivity_percent_to_scale(int sensitivityPercent)
{
    return kGameCubeSensitivityOffset + (static_cast<double>(sensitivityPercent) * kGameCubeSensitivityPerPercent);
}

const RaphnetAdapterDef* find_raphnet_adapter(uint16_t productId, int interfaceNumber)
{
    for (const RaphnetAdapterDef& adapter : kRaphnetAdapters)
    {
        if (adapter.productId == productId && adapter.interfaceNumber == interfaceNumber)
        {
            return &adapter;
        }
    }

    return nullptr;
}

QString string_from_const_char(const char* text)
{
    return QString::fromUtf8(text == nullptr ? "" : text);
}

QString format_usb_id(uint16_t vendorId, uint16_t productId)
{
    return QStringLiteral("%1:%2")
        .arg(vendorId, 4, 16, QChar('0'))
        .arg(productId, 4, 16, QChar('0'));
}

std::string usb_profile_section(int pageIndex)
{
    return "Rosalie's Mupen GUI - Input Plugin Profile " + std::to_string(pageIndex);
}

std::string usb_effective_section(int pageIndex)
{
    const std::string section = usb_profile_section(pageIndex);
    if (!CoreSettingsSectionExists(section))
        return section;
    const std::string profile = CoreSettingsGetStringValue(SettingsID::Input_UseProfile, section);
    const auto profiles = CoreSettingsGetStringListValue(SettingsID::Input_Profiles);
    const std::string userSection = "Rosalie's Mupen GUI - Input Plugin User Profile \"" + profile + "\"";
    if (!profile.empty() && std::find(profiles.begin(), profiles.end(), profile) != profiles.end() &&
        CoreSettingsSectionExists(userSection))
        return userSection;
    return section;
}

bool usb_device_supports_raphnet_raw(const UsbDeviceChoice& device)
{
    return device.vendorId == kRaphnetVendorId &&
        std::any_of(std::begin(kRaphnetAdapters), std::end(kRaphnetAdapters),
            [&device](const RaphnetAdapterDef& adapter) { return adapter.productId == device.productId; });
}

void show_status(QLabel* label, const QString& text)
{
    if (label == nullptr)
    {
        return;
    }

    label->setText(text);
    label->setVisible(!text.isEmpty());
}

void clear_status(QLabel* label)
{
    if (label == nullptr)
    {
        return;
    }

    label->clear();
    label->setVisible(false);
}

bool binding_is_empty(const BindingValue& binding)
{
    if (binding.types.isEmpty())
    {
        return true;
    }

    for (int type : binding.types)
    {
        if (static_cast<InputType>(type) != InputType::Invalid)
        {
            return false;
        }
    }

    return true;
}

QString binding_text(const BindingValue& binding)
{
    if (binding_is_empty(binding))
    {
        return QStringLiteral("Not set");
    }

    QStringList parts;
    const int count = std::min(static_cast<int>(binding.types.size()),
        std::min(static_cast<int>(binding.text.size()),
            std::min(static_cast<int>(binding.data.size()), static_cast<int>(binding.extraData.size()))));
    for (int i = 0; i < count; i++)
    {
        if (static_cast<InputType>(binding.types[i]) == InputType::Invalid)
        {
            continue;
        }

        parts.append(binding.text[i].isEmpty() ? QStringLiteral("Input %1").arg(binding.data[i]) : binding.text[i]);
    }

    return parts.isEmpty() ? QStringLiteral("Not set") : parts.join(QStringLiteral(", "));
}

void set_single_binding(BindingValue& binding, InputType type, int data, int extraData, const QString& text)
{
    binding.types = { static_cast<int>(type) };
    binding.data = { data };
    binding.extraData = { extraData };
    binding.text = { text };
}

void clear_binding(BindingValue& binding)
{
    set_single_binding(binding, InputType::Invalid, 0, 0, QString());
}

std::vector<int> to_std_vector(const QVector<int>& values)
{
    std::vector<int> result;
    result.reserve(static_cast<size_t>(values.size()));
    for (int value : values)
    {
        result.push_back(value);
    }
    return result;
}

std::vector<std::string> to_std_string_vector(const QVector<QString>& values)
{
    std::vector<std::string> result;
    result.reserve(static_cast<size_t>(values.size()));
    for (const QString& value : values)
    {
        result.push_back(value.toStdString());
    }
    return result;
}

QVector<int> to_qvector(const std::vector<int>& values)
{
    QVector<int> result;
    result.reserve(static_cast<int>(values.size()));
    for (int value : values)
    {
        result.append(value);
    }
    return result;
}

QVector<QString> to_qstring_vector(const std::vector<std::string>& values)
{
    QVector<QString> result;
    result.reserve(static_cast<int>(values.size()));
    for (const std::string& value : values)
    {
        result.append(QString::fromStdString(value));
    }
    return result;
}

BindingValue load_usb_binding(const BindingTarget& target, const std::string& section)
{
    BindingValue binding;
    binding.types = to_qvector(CoreSettingsGetIntListValue(target.usbInputType, section));
    binding.text = to_qstring_vector(CoreSettingsGetStringListValue(target.usbName, section));
    binding.data = to_qvector(CoreSettingsGetIntListValue(target.usbData, section));
    binding.extraData = to_qvector(CoreSettingsGetIntListValue(target.usbExtraData, section));

    const int count = std::min(static_cast<int>(binding.types.size()),
        std::min(static_cast<int>(binding.text.size()),
            std::min(static_cast<int>(binding.data.size()), static_cast<int>(binding.extraData.size()))));
    if (count > 0)
    {
        binding.types.resize(count);
        binding.text.resize(count);
        binding.data.resize(count);
        binding.extraData.resize(count);
        return binding;
    }

    const int type = CoreSettingsGetIntValue(target.usbInputType, section);
    const std::string name = CoreSettingsGetStringValue(target.usbName, section);
    if (name.empty())
    {
        clear_binding(binding);
        return binding;
    }
    const int data = CoreSettingsGetIntValue(target.usbData, section);
    const int extraData = CoreSettingsGetIntValue(target.usbExtraData, section);
    set_single_binding(binding, static_cast<InputType>(type), data, extraData, QString::fromStdString(name));
    return binding;
}

QString gc_input_to_string(GCInput input)
{
    switch (input)
    {
    case GCInput::A: return QStringLiteral("A");
    case GCInput::B: return QStringLiteral("B");
    case GCInput::X: return QStringLiteral("X");
    case GCInput::Y: return QStringLiteral("Y");
    case GCInput::Z: return QStringLiteral("Z");
    case GCInput::Start: return QStringLiteral("Start");
    case GCInput::L: return QStringLiteral("L (digital)");
    case GCInput::R: return QStringLiteral("R (digital)");
    case GCInput::DpadUp: return QStringLiteral("D-Up");
    case GCInput::DpadDown: return QStringLiteral("D-Down");
    case GCInput::DpadLeft: return QStringLiteral("D-Left");
    case GCInput::DpadRight: return QStringLiteral("D-Right");
    case GCInput::LeftTrigger: return QStringLiteral("L (analog)");
    case GCInput::RightTrigger: return QStringLiteral("R (analog)");
    case GCInput::CStickUp: return QStringLiteral("C-Stick Up");
    case GCInput::CStickDown: return QStringLiteral("C-Stick Down");
    case GCInput::CStickLeft: return QStringLiteral("C-Stick Left");
    case GCInput::CStickRight: return QStringLiteral("C-Stick Right");
    case GCInput::None:
    default:
        return QStringLiteral("Not set");
    }
}

GCInput gc_input_with_trigger_mode(GCInput input, bool analog)
{
    switch (input)
    {
    case GCInput::L:
    case GCInput::LeftTrigger:
        return analog ? GCInput::LeftTrigger : GCInput::L;
    case GCInput::R:
    case GCInput::RightTrigger:
        return analog ? GCInput::RightTrigger : GCInput::R;
    default:
        return input;
    }
}

void apply_gamecube_trigger_mode(QVector<int>& bindings, bool leftTrigger, bool analog)
{
    const GCInput digitalInput = leftTrigger ? GCInput::L : GCInput::R;
    const GCInput analogInput = leftTrigger ? GCInput::LeftTrigger : GCInput::RightTrigger;

    for (int& binding : bindings)
    {
        const GCInput input = static_cast<GCInput>(binding);
        if (input == digitalInput || input == analogInput)
        {
            binding = static_cast<int>(gc_input_with_trigger_mode(input, analog));
        }
    }
}

bool gc_input_active(const GameCubeState& state, GCInput input,
    double triggerThreshold, double cStickThreshold, bool leftTriggerAnalog, bool rightTriggerAnalog)
{
    const int triggerThresh = static_cast<int>(127.0 * triggerThreshold);
    const int cStickThresh = static_cast<int>(127.0 * cStickThreshold);
    const int8_t cX = static_cast<int8_t>(state.rightStickX + 128);
    const int8_t cY = static_cast<int8_t>(state.rightStickY + 128);

    switch (input)
    {
    case GCInput::A: return (state.buttons1 & 0x01) != 0;
    case GCInput::B: return (state.buttons1 & 0x02) != 0;
    case GCInput::X: return (state.buttons1 & 0x04) != 0;
    case GCInput::Y: return (state.buttons1 & 0x08) != 0;
    case GCInput::DpadLeft: return (state.buttons1 & 0x10) != 0;
    case GCInput::DpadRight: return (state.buttons1 & 0x20) != 0;
    case GCInput::DpadDown: return (state.buttons1 & 0x40) != 0;
    case GCInput::DpadUp: return (state.buttons1 & 0x80) != 0;
    case GCInput::Start: return (state.buttons2 & 0x01) != 0;
    case GCInput::Z: return (state.buttons2 & 0x02) != 0;
    case GCInput::R: return !rightTriggerAnalog && (state.buttons2 & 0x04) != 0;
    case GCInput::L: return !leftTriggerAnalog && (state.buttons2 & 0x08) != 0;
    case GCInput::LeftTrigger: return leftTriggerAnalog && state.leftTrigger > triggerThresh;
    case GCInput::RightTrigger: return rightTriggerAnalog && state.rightTrigger > triggerThresh;
    case GCInput::CStickUp: return cY > cStickThresh;
    case GCInput::CStickDown: return cY < -cStickThresh;
    case GCInput::CStickLeft: return cX < -cStickThresh;
    case GCInput::CStickRight: return cX > cStickThresh;
    default: return false;
    }
}

GCInput detect_gamecube_input(const GameCubeState& state,
    double triggerThreshold, double cStickThreshold, bool leftTriggerAnalog, bool rightTriggerAnalog)
{
    const std::array<GCInput, 6> leadingInputs = {{
        GCInput::A, GCInput::B, GCInput::X, GCInput::Y, GCInput::Z, GCInput::Start
    }};

    for (GCInput input : leadingInputs)
    {
        if (gc_input_active(state, input, triggerThreshold, cStickThreshold, leftTriggerAnalog, rightTriggerAnalog))
        {
            return input;
        }
    }

    if ((state.buttons2 & 0x08) != 0)
    {
        return leftTriggerAnalog ? GCInput::LeftTrigger : GCInput::L;
    }
    if ((state.buttons2 & 0x04) != 0)
    {
        return rightTriggerAnalog ? GCInput::RightTrigger : GCInput::R;
    }

    const std::array<GCInput, 10> remainingInputs = {{
        GCInput::DpadUp, GCInput::DpadDown, GCInput::DpadLeft, GCInput::DpadRight,
        GCInput::LeftTrigger, GCInput::RightTrigger,
        GCInput::CStickUp, GCInput::CStickDown, GCInput::CStickLeft, GCInput::CStickRight
    }};

    for (GCInput input : remainingInputs)
    {
        if (gc_input_active(state, input, triggerThreshold, cStickThreshold, leftTriggerAnalog, rightTriggerAnalog))
        {
            return input;
        }
    }

    return GCInput::None;
}

int scale_axis(double input, double deadzone, double n64Max)
{
    const double inputAbs = std::abs(input);
    if (inputAbs <= deadzone)
    {
        return 0;
    }

    const double deadzoneRelation = 1.0 / (1.0 - deadzone);
    const double scaled = (inputAbs - deadzone) * deadzoneRelation * n64Max;
    const int result = static_cast<int>(std::min(scaled, n64Max));
    return input >= 0 ? result : -result;
}

int normalize_axis_to_percent(int value, int maxValue)
{
    if (maxValue <= 0)
    {
        return 0;
    }

    const int clamped = std::clamp(value, -maxValue, maxValue);
    return std::clamp((clamped * 100) / maxValue, -100, 100);
}

void set_button(UserInterface::Widget::ControllerImageWidget* widget, N64ControllerButton button, bool pressed)
{
    if (widget != nullptr && button != N64ControllerButton::Invalid)
    {
        widget->SetButtonState(button, pressed);
    }
}

void apply_n64_buttons(UserInterface::Widget::ControllerImageWidget* widget, uint16_t buttons)
{
    set_button(widget, N64ControllerButton::A, (buttons & kN64ButtonA) != 0);
    set_button(widget, N64ControllerButton::B, (buttons & kN64ButtonB) != 0);
    set_button(widget, N64ControllerButton::ZTrigger, (buttons & kN64ButtonZ) != 0);
    set_button(widget, N64ControllerButton::Start, (buttons & kN64ButtonStart) != 0);
    set_button(widget, N64ControllerButton::DpadUp, (buttons & kN64ButtonDUp) != 0);
    set_button(widget, N64ControllerButton::DpadDown, (buttons & kN64ButtonDDown) != 0);
    set_button(widget, N64ControllerButton::DpadLeft, (buttons & kN64ButtonDLeft) != 0);
    set_button(widget, N64ControllerButton::DpadRight, (buttons & kN64ButtonDRight) != 0);
    set_button(widget, N64ControllerButton::LeftShoulder, (buttons & kN64ButtonL) != 0);
    set_button(widget, N64ControllerButton::RightShoulder, (buttons & kN64ButtonR) != 0);
    set_button(widget, N64ControllerButton::CButtonUp, (buttons & kN64ButtonCUp) != 0);
    set_button(widget, N64ControllerButton::CButtonDown, (buttons & kN64ButtonCDown) != 0);
    set_button(widget, N64ControllerButton::CButtonLeft, (buttons & kN64ButtonCLeft) != 0);
    set_button(widget, N64ControllerButton::CButtonRight, (buttons & kN64ButtonCRight) != 0);
}

QPushButton* add_mapping_row(QGridLayout* layout, ControllerPage* page, int row, int bindingIndex, QWidget* parent)
{
    const BindingTarget& target = kBindingTargets[static_cast<size_t>(bindingIndex)];
    auto* label = new QLabel(QString::fromLatin1(target.label), parent);
    auto* button = new QPushButton(parent);
    auto* clearButton = new QPushButton(parent);

    button->setMinimumWidth(96);
    button->setObjectName(QStringLiteral("binding%1").arg(bindingIndex));
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    clearButton->setText(QStringLiteral("×"));
    clearButton->setFixedWidth(24);
    clearButton->setToolTip(QCoreApplication::translate("UnifiedInputDialog", "Clear binding"));
    clearButton->setAccessibleName(QCoreApplication::translate("UnifiedInputDialog", "Clear %1 binding")
        .arg(QString::fromLatin1(target.label)));
    clearButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    layout->addWidget(label, row, 0);
    layout->addWidget(button, row, 1);
    layout->addWidget(clearButton, row, 2);

    page->bindingButtons[bindingIndex] = button;
    page->clearButtons[bindingIndex] = clearButton;
    return button;
}

QWidget* create_slider_value_row(QWidget* parent, QSlider* slider, QLabel* valueLabel)
{
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    valueLabel->setMinimumWidth(44);
    valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(slider, 1);
    layout->addWidget(valueLabel);

    return row;
}

} // namespace

using namespace UserInterface::Dialog;

UnifiedInputDialog::UnifiedInputDialog(QWidget* parent, InputPluginType currentPlugin)
    : QDialog(parent),
      selectedPlugin(currentPlugin)
{
    this->raphnetConnectionSlow = CoreGetRaphnetHealth() == 2;
    CorePauseRaphnetMonitoring(true);
    this->raphnetPlayer1Port = std::clamp(CoreSettingsGetIntValue(SettingsID::RaphnetRaw_Player1AdapterPort), 1, 4) - 1;
    this->setupUi();
    this->settingsLoaded = true;
    this->refreshDetection();
    this->lastDeviceTopology = this->deviceTopology();
    this->deviceTimer->start();
    qApp->installEventFilter(this);
}

UnifiedInputDialog::~UnifiedInputDialog(void)
{
    qApp->removeEventFilter(this);
    this->closePreviewSource();
    CorePauseRaphnetMonitoring(false);
    for (ControllerPage* page : this->controllerPages)
    {
        delete page;
    }
}

UnifiedInputDialog::InputPluginType UnifiedInputDialog::GetSelectedPlugin(void) const
{
    return this->selectedPlugin;
}

void UnifiedInputDialog::setupUi(void)
{
    this->setWindowTitle(tr("Input Settings"));
    this->setMinimumSize(720, 480);
    this->resize(QSize(1100, 760).boundedTo(this->screen()->availableGeometry().size() - QSize(40, 60)));

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);

    this->tabWidget = new QTabWidget(this);
    this->tabWidget->setObjectName("players");
    for (int i = 0; i < 4; i++)
    {
        QWidget* pageWidget = this->createControllerPage(i);
        auto* scroll = new QScrollArea(this->tabWidget);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(pageWidget);
        this->tabWidget->addTab(scroll, tr("Player %1").arg(i + 1));
    }
    mainLayout->addWidget(this->tabWidget, 1);

    this->debugDevicesCheckBox = new QCheckBox(tr("Device details and troubleshooting"), this);
    mainLayout->addWidget(this->debugDevicesCheckBox);

    this->detectedDevicesGroupBox = new QGroupBox(tr("Device details"), this);
    auto* detectedLayout = new QVBoxLayout(this->detectedDevicesGroupBox);
    this->detectedDevicesPlainTextEdit = new QPlainTextEdit(this->detectedDevicesGroupBox);
    this->detectedDevicesPlainTextEdit->setReadOnly(true);
    this->detectedDevicesPlainTextEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
    this->detectedDevicesPlainTextEdit->setMaximumHeight(88);
    detectedLayout->addWidget(this->detectedDevicesPlainTextEdit);
    this->raphnetTimingLabel = new QLabel(this->detectedDevicesGroupBox);
    this->raphnetTimingLabel->setWordWrap(true);
    detectedLayout->addWidget(this->raphnetTimingLabel);
    this->detectedDevicesGroupBox->setVisible(false);
    mainLayout->addWidget(this->detectedDevicesGroupBox);

    this->buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, this);
    if (QPushButton* okButton = this->buttonBox->button(QDialogButtonBox::Ok))
    {
        okButton->setText(tr("Save and Close"));
    }
    mainLayout->addWidget(this->buttonBox);

    this->pollTimer = new QTimer(this);
    this->pollTimer->setInterval(16);
    this->deviceTimer = new QTimer(this);
    this->deviceTimer->setObjectName("deviceDetectionTimer");
    this->deviceTimer->setInterval(2000);
    connect(this->deviceTimer, &QTimer::timeout, this, [this]()
    {
        const QStringList topology = this->deviceTopology();
        if (topology != this->lastDeviceTopology)
        {
            this->lastDeviceTopology = topology;
            this->raphnetConnectionSlow = false;
            this->refreshDetection();
        }
        else if (this->previewBackend == PreviewBackend::None && this->listeningPageIndex < 0)
        {
            this->openPreviewSource();
        }
    });

    connect(this->debugDevicesCheckBox, &QCheckBox::toggled, this->detectedDevicesGroupBox, &QGroupBox::setVisible);
    connect(this->pollTimer, &QTimer::timeout, this, &UnifiedInputDialog::pollPreview);
    connect(this->tabWidget, &QTabWidget::currentChanged, this, [this](int pageIndex)
    {
        this->stopListeningForBinding(true);
        this->syncSharedUsbProfile(this->previousPageIndex);
        this->previousPageIndex = pageIndex;
        this->updatePageBindingButtons(pageIndex);
        this->openPreviewSource();
    });
    connect(this->buttonBox, &QDialogButtonBox::accepted, this, [this]()
    {
        if (this->selectedPlugin == InputPluginType::Gamecube)
        {
            std::array<bool, 4> usedPorts{};
            for (ControllerPage* page : this->controllerPages)
            {
                if (!page->gamecubeEnabled) continue;
                if (usedPorts[static_cast<size_t>(page->gamecubePort)])
                {
                    QMessageBox::warning(this, tr("Adapter ports"),
                        tr("Choose a different adapter port for each enabled player."));
                    return;
                }
                usedPorts[static_cast<size_t>(page->gamecubePort)] = true;
            }
        }
        this->saveAllSettings();
        this->accept();
    });
    connect(this->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(this->buttonBox->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this,
        &UnifiedInputDialog::restoreCurrentPageDefaults);

    this->setStyleSheet(
        "QFrame[controllerWarningBox=\"true\"] {"
        "  background: #fff3cd;"
        "  border: 0;"
        "  border-radius: 4px;"
        "}"
        "QFrame[controllerWarningBox=\"true\"] QLabel {"
        "  background: transparent;"
        "  color: #664d03;"
        "  border: 0;"
        "  padding: 0;"
        "}"
        "QGroupBox[plainSurface=\"true\"] {"
        "  border: 0;"
        "  margin-top: 0;"
        "}"
        "QGroupBox[plainSurface=\"true\"]::title {"
        "  height: 0;"
        "  color: transparent;"
        "}"
    );
}

QWidget* UnifiedInputDialog::createControllerPage(int playerIndex)
{
    auto* page = new ControllerPage();
    page->bindingButtons.resize(static_cast<int>(kBindingTargets.size()));
    page->clearButtons.resize(static_cast<int>(kBindingTargets.size()));
    page->usbBindings.resize(static_cast<int>(kBindingTargets.size()));
    page->gamecubeBindings.resize(static_cast<int>(kBindingTargets.size()));
    this->controllerPages.append(page);

    auto* root = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(8, 8, 8, 8);
    rootLayout->setSpacing(8);

    auto* topLayout = new QHBoxLayout();
    page->portGroupBox = new QGroupBox(playerIndex == 0 ? tr("Controller") : tr("Player Port"), root);
    page->portGroupBox->setObjectName(QStringLiteral("controllerControls%1").arg(playerIndex));
    page->portGroupBox->setProperty("plainSurface", true);
    auto* portLayout = new QFormLayout(page->portGroupBox);
    portLayout->setContentsMargins(0, 0, 0, 0);

    if (playerIndex == 0)
    {
        page->backendComboBox = new QComboBox(page->portGroupBox);
        page->backendComboBox->setObjectName("controllerType");
        page->backendComboBox->setMaximumWidth(420);
        portLayout->addRow(tr("Controller type:"), page->backendComboBox);
        page->backendComboBox->setToolTip(tr("The controller type applies to all four players."));
        connect(page->backendComboBox, &QComboBox::activated, this, [this](int)
        {
            this->manualInputChoice = true;
        });
        connect(page->backendComboBox, &QComboBox::currentIndexChanged, this, [this, page](int)
        {
            if (page->backendComboBox->currentIndex() >= 0)
            {
                this->setSelectedPlugin(static_cast<InputPluginType>(page->backendComboBox->currentData().toInt()));
            }
        });
    }

    page->deviceComboBox = new QComboBox(page->portGroupBox);
    page->deviceComboBox->setObjectName(QStringLiteral("device%1").arg(playerIndex));
    page->deviceComboBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    page->deviceComboBox->setMinimumContentsLength(24);
    page->deviceComboBox->setMaximumWidth(420);
    page->pluggedInCheckBox = new QCheckBox(tr("Enabled for this player"), page->portGroupBox);
    auto* deviceRowWidget = new QWidget(page->portGroupBox);
    auto* deviceRowLayout = new QHBoxLayout(deviceRowWidget);
    deviceRowLayout->setContentsMargins(0, 0, 0, 0);
    deviceRowLayout->setSpacing(12);
    deviceRowLayout->addWidget(page->deviceComboBox, 1);
    deviceRowLayout->addWidget(page->pluggedInCheckBox, 0);
    portLayout->addRow(playerIndex == 0 ? tr("Device / port:") : tr("Device:"), deviceRowWidget);
    connect(page->deviceComboBox, &QComboBox::activated, this, [this, playerIndex](int)
    {
        this->manualInputChoice = true;
        this->offerRaphnetInputType(playerIndex);
    });
    connect(page->deviceComboBox, &QComboBox::currentIndexChanged, this, [this, playerIndex](int)
    {
        ControllerPage* page = this->controllerPages[playerIndex];
        if (this->settingsLoaded && this->selectedPlugin == InputPluginType::USB) page->usbDirty = true;
        const int value = page->deviceComboBox->currentData().toInt();
        if (this->selectedPlugin == InputPluginType::USB && value >= 0 && value < this->usbDevices.size())
            page->usbDevice = this->usbDevices[value];
        else if (this->selectedPlugin == InputPluginType::Gamecube)
            page->gamecubePort = std::clamp(value, 0, 3);
        else if (this->selectedPlugin == InputPluginType::Raphnet && playerIndex == 0)
        {
            this->raphnetPlayer1Port = std::clamp(value, 0, 3);
            for (int i = 1; i < this->controllerPages.size(); ++i)
                this->updatePageDeviceChoices(i);
        }
        this->stopListeningForBinding(true);
        if (playerIndex == this->currentPageIndex())
        {
            this->openPreviewSource();
        }
        this->updateWarningLabel();
    });
    connect(page->pluggedInCheckBox, &QCheckBox::toggled, this, [this, page](bool checked)
    {
        if (this->selectedPlugin == InputPluginType::USB)
        {
            page->usbEnabled = checked;
            if (this->settingsLoaded) page->usbDirty = true;
        }
        if (this->selectedPlugin == InputPluginType::Gamecube) page->gamecubeEnabled = checked;
        this->stopListeningForBinding(true);
        this->openPreviewSource();
    });

    page->portGroupBox->setMaximumWidth(560);
    page->portGroupBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    topLayout->addWidget(page->portGroupBox, 0);
    // Match the controls when possible; allow wrapped/multiple warnings to fit.
    auto* messageArea = new ControllerMessageArea(root, playerIndex);
    page->messageArea = messageArea;
    page->messages = messageArea->Messages();
    topLayout->addWidget(messageArea, 1);
    rootLayout->addLayout(topLayout);

    page->mappingsGroupBox = new QGroupBox(tr("Controller bindings"), root);
    page->mappingsGroupBox->setProperty("plainSurface", true);
    auto* mappingsLayout = new QHBoxLayout(page->mappingsGroupBox);
    mappingsLayout->setContentsMargins(0, 0, 0, 0);
    auto* leftColumn = new QVBoxLayout();
    auto* centerColumn = new QVBoxLayout();
    auto* rightColumn = new QVBoxLayout();

    auto* dpadGroup = new QGroupBox(tr("Digital Pad"), page->mappingsGroupBox);
    auto* dpadLayout = new QGridLayout(dpadGroup);
    for (int row = 0; row < static_cast<int>(kDpadBindingIndexes.size()); row++)
    {
        add_mapping_row(dpadLayout, page, row, kDpadBindingIndexes[static_cast<size_t>(row)], dpadGroup);
    }
    leftColumn->addWidget(dpadGroup);

    auto* analogGroup = new QGroupBox(tr("Analog Stick"), page->mappingsGroupBox);
    auto* analogLayout = new QGridLayout(analogGroup);
    for (int row = 0; row < static_cast<int>(kAnalogBindingIndexes.size()); row++)
    {
        add_mapping_row(analogLayout, page, row, kAnalogBindingIndexes[static_cast<size_t>(row)], analogGroup);
    }
    leftColumn->addWidget(analogGroup);
    leftColumn->addStretch();

    auto* controllerPanel = new ControllerPreviewPanel(page->mappingsGroupBox);
    page->controllerImageWidget = controllerPanel->ControllerImage();

    auto* axisGroup = new QGroupBox(tr("Stick"), controllerPanel);
    axisGroup->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    axisGroup->setMaximumWidth(120);
    auto* axisLayout = new QGridLayout(axisGroup);
    axisLayout->setHorizontalSpacing(8);
    axisLayout->setVerticalSpacing(4);
    page->axisXLabel = new QLabel(QStringLiteral("0"), axisGroup);
    page->axisYLabel = new QLabel(QStringLiteral("0"), axisGroup);
    page->axisXLabel->setMinimumWidth(24);
    page->axisYLabel->setMinimumWidth(24);
    axisLayout->addWidget(new QLabel(tr("X:"), axisGroup), 0, 0);
    axisLayout->addWidget(page->axisXLabel, 0, 1);
    axisLayout->addWidget(new QLabel(tr("Y:"), axisGroup), 1, 0);
    axisLayout->addWidget(page->axisYLabel, 1, 1);
    controllerPanel->SetOutputWidget(axisGroup);
    centerColumn->addWidget(controllerPanel, 1);

    auto* buttonGroup = new QGroupBox(tr("Buttons"), page->mappingsGroupBox);
    auto* buttonLayout = new QGridLayout(buttonGroup);
    for (int row = 0; row < static_cast<int>(kButtonBindingIndexes.size()); row++)
    {
        add_mapping_row(buttonLayout, page, row, kButtonBindingIndexes[static_cast<size_t>(row)], buttonGroup);
    }
    rightColumn->addWidget(buttonGroup);

    auto* cButtonGroup = new QGroupBox(tr("C Buttons"), page->mappingsGroupBox);
    auto* cButtonLayout = new QGridLayout(cButtonGroup);
    for (int row = 0; row < static_cast<int>(kCButtonBindingIndexes.size()); row++)
    {
        add_mapping_row(cButtonLayout, page, row, kCButtonBindingIndexes[static_cast<size_t>(row)], cButtonGroup);
    }
    rightColumn->addWidget(cButtonGroup);
    rightColumn->addStretch();

    mappingsLayout->addLayout(leftColumn, 1);
    mappingsLayout->addLayout(centerColumn, 2);
    mappingsLayout->addLayout(rightColumn, 1);
    rootLayout->addWidget(page->mappingsGroupBox, 1);

    page->usbStickGroupBox = new QGroupBox(tr("Stick Settings"), page->mappingsGroupBox);
    page->usbStickGroupBox->setMaximumWidth(560);
    auto* usbStickLayout = new QFormLayout(page->usbStickGroupBox);
    page->usbDeadzoneSlider = new QSlider(Qt::Horizontal, page->usbStickGroupBox);
    page->usbDeadzoneSlider->setRange(0, 100);
    page->usbDeadzoneSlider->setTickInterval(10);
    page->usbDeadzoneSlider->setTickPosition(QSlider::TicksBelow);
    page->usbRangeSlider = new QSlider(Qt::Horizontal, page->usbStickGroupBox);
    page->usbRangeSlider->setObjectName(QStringLiteral("range%1").arg(playerIndex));
    page->usbRangeSlider->setRange(0, 200);
    page->usbRangeSlider->setTickInterval(10);
    page->usbRangeSlider->setTickPosition(QSlider::TicksBelow);
    page->realN64RangeCheckBox = new QCheckBox(tr("Use real N64 stick range"), page->usbStickGroupBox);
    page->realN64RangeCheckBox->setObjectName(QStringLiteral("n64Range%1").arg(playerIndex));
    page->usbDeadzoneValueLabel = new QLabel(page->usbStickGroupBox);
    page->usbRangeValueLabel = new QLabel(page->usbStickGroupBox);
    usbStickLayout->addRow(tr("Deadzone:"), create_slider_value_row(page->usbStickGroupBox,
        page->usbDeadzoneSlider, page->usbDeadzoneValueLabel));
    usbStickLayout->addRow(tr("Range:"), create_slider_value_row(page->usbStickGroupBox,
        page->usbRangeSlider, page->usbRangeValueLabel));
    usbStickLayout->addRow(QString(), page->realN64RangeCheckBox);
    leftColumn->insertWidget(2, page->usbStickGroupBox, 0, Qt::AlignLeft);

    page->gamecubeStickGroupBox = new QGroupBox(tr("Stick Settings"), page->mappingsGroupBox);
    page->gamecubeStickGroupBox->setMaximumWidth(560);
    auto* gamecubeStickLayout = new QFormLayout(page->gamecubeStickGroupBox);
    page->gamecubeDeadzoneSlider = new QSlider(Qt::Horizontal, page->gamecubeStickGroupBox);
    page->gamecubeDeadzoneSlider->setRange(0, 100);
    page->gamecubeSensitivitySlider = new QSlider(Qt::Horizontal, page->gamecubeStickGroupBox);
    page->gamecubeSensitivitySlider->setRange(0, 200);
    page->gamecubeDeadzoneValueLabel = new QLabel(page->gamecubeStickGroupBox);
    page->gamecubeSensitivityValueLabel = new QLabel(page->gamecubeStickGroupBox);
    gamecubeStickLayout->addRow(tr("Deadzone:"), create_slider_value_row(page->gamecubeStickGroupBox,
        page->gamecubeDeadzoneSlider, page->gamecubeDeadzoneValueLabel));
    gamecubeStickLayout->addRow(tr("Stick sensitivity:"), create_slider_value_row(page->gamecubeStickGroupBox,
        page->gamecubeSensitivitySlider, page->gamecubeSensitivityValueLabel));
    leftColumn->insertWidget(3, page->gamecubeStickGroupBox, 0, Qt::AlignLeft);

    page->gamecubeTriggerGroupBox = new QGroupBox(tr("Trigger Settings"), page->mappingsGroupBox);
    page->gamecubeTriggerGroupBox->setMaximumWidth(560);
    auto* gamecubeTriggerLayout = new QFormLayout(page->gamecubeTriggerGroupBox);
    auto* leftTriggerModeWidget = new QWidget(page->gamecubeTriggerGroupBox);
    auto* leftTriggerModeLayout = new QHBoxLayout(leftTriggerModeWidget);
    leftTriggerModeLayout->setContentsMargins(0, 0, 0, 0);
    leftTriggerModeLayout->setSpacing(12);
    page->gamecubeLeftTriggerDigitalRadioButton = new QRadioButton(tr("Digital"), leftTriggerModeWidget);
    page->gamecubeLeftTriggerAnalogRadioButton = new QRadioButton(tr("Analog"), leftTriggerModeWidget);
    page->gamecubeLeftTriggerDigitalRadioButton->setObjectName(QStringLiteral("leftTriggerDigital%1").arg(playerIndex));
    page->gamecubeLeftTriggerAnalogRadioButton->setObjectName(QStringLiteral("leftTriggerAnalog%1").arg(playerIndex));
    leftTriggerModeLayout->addWidget(page->gamecubeLeftTriggerDigitalRadioButton);
    leftTriggerModeLayout->addWidget(page->gamecubeLeftTriggerAnalogRadioButton);
    leftTriggerModeLayout->addStretch(1);

    auto* rightTriggerModeWidget = new QWidget(page->gamecubeTriggerGroupBox);
    auto* rightTriggerModeLayout = new QHBoxLayout(rightTriggerModeWidget);
    rightTriggerModeLayout->setContentsMargins(0, 0, 0, 0);
    rightTriggerModeLayout->setSpacing(12);
    page->gamecubeRightTriggerDigitalRadioButton = new QRadioButton(tr("Digital"), rightTriggerModeWidget);
    page->gamecubeRightTriggerAnalogRadioButton = new QRadioButton(tr("Analog"), rightTriggerModeWidget);
    page->gamecubeRightTriggerDigitalRadioButton->setObjectName(QStringLiteral("rightTriggerDigital%1").arg(playerIndex));
    page->gamecubeRightTriggerAnalogRadioButton->setObjectName(QStringLiteral("rightTriggerAnalog%1").arg(playerIndex));
    rightTriggerModeLayout->addWidget(page->gamecubeRightTriggerDigitalRadioButton);
    rightTriggerModeLayout->addWidget(page->gamecubeRightTriggerAnalogRadioButton);
    rightTriggerModeLayout->addStretch(1);

    page->gamecubeTriggerThresholdSlider = new QSlider(Qt::Horizontal, page->gamecubeTriggerGroupBox);
    page->gamecubeTriggerThresholdSlider->setRange(0, 100);
    page->gamecubeTriggerThresholdValueLabel = new QLabel(page->gamecubeTriggerGroupBox);
    gamecubeTriggerLayout->addRow(tr("L trigger:"), leftTriggerModeWidget);
    gamecubeTriggerLayout->addRow(tr("R trigger:"), rightTriggerModeWidget);
    gamecubeTriggerLayout->addRow(tr("Trigger threshold:"), create_slider_value_row(page->gamecubeTriggerGroupBox,
        page->gamecubeTriggerThresholdSlider, page->gamecubeTriggerThresholdValueLabel));
    leftColumn->insertWidget(4, page->gamecubeTriggerGroupBox, 0, Qt::AlignLeft);

    page->statusLabel = new QLabel(root);
    page->statusLabel->setWordWrap(true);
    page->statusLabel->setVisible(false);
    rootLayout->addWidget(page->statusLabel);

    for (int i = 0; i < static_cast<int>(page->bindingButtons.size()); i++)
    {
        connect(page->bindingButtons[i], &QPushButton::clicked, this, [this, playerIndex, i]()
        {
            this->startListeningForBinding(playerIndex, i);
        });
        connect(page->clearButtons[i], &QPushButton::clicked, this, [this, playerIndex, i]()
        {
            this->clearBinding(playerIndex, i);
        });
    }

    auto updateSliders = [this, playerIndex](int)
    {
        if (this->settingsLoaded && this->selectedPlugin == InputPluginType::USB)
            this->controllerPages[playerIndex]->usbDirty = true;
        this->updateSliderLabels(playerIndex);
    };
    connect(page->usbDeadzoneSlider, &QSlider::valueChanged, this, updateSliders);
    connect(page->usbRangeSlider, &QSlider::valueChanged, this, updateSliders);
    connect(page->realN64RangeCheckBox, &QCheckBox::toggled, this, [this, playerIndex](bool checked)
    {
        ControllerPage* page = this->controllerPages[playerIndex];
        page->usbRangeSlider->setEnabled(!checked);
        if (this->settingsLoaded) page->usbDirty = true;
        if (checked) page->usbRangeSlider->setValue(66);
        this->updateSliderLabels(playerIndex);
    });
    connect(page->gamecubeDeadzoneSlider, &QSlider::valueChanged, this, updateSliders);
    connect(page->gamecubeSensitivitySlider, &QSlider::valueChanged, this, updateSliders);
    connect(page->gamecubeTriggerThresholdSlider, &QSlider::valueChanged, this, updateSliders);
    connect(page->gamecubeLeftTriggerAnalogRadioButton, &QRadioButton::toggled, this, [this, playerIndex](bool checked)
    {
        if (playerIndex == 0)
        {
            this->setGamecubeTriggerAnalog(true, checked);
        }
    });
    connect(page->gamecubeRightTriggerAnalogRadioButton, &QRadioButton::toggled, this, [this, playerIndex](bool checked)
    {
        if (playerIndex == 0)
        {
            this->setGamecubeTriggerAnalog(false, checked);
        }
    });

    this->loadPageSettings(playerIndex);
    return root;
}

void UnifiedInputDialog::refreshDetection(void)
{
    this->stopListeningForBinding(true);
    this->closePreviewSource();
    this->keyboardState.clear();
    this->detectionReport = UnifiedInputDialog::ScanInputDevices();
    this->refreshUsbDevices();

    if (this->detectedDevicesPlainTextEdit != nullptr)
    {
        this->detectedDevicesPlainTextEdit->setPlainText(this->detectionReport.lines.join(QStringLiteral("\n")));
    }

    this->updateAllPages();
    this->openPreviewSource();
}

QStringList UnifiedInputDialog::deviceTopology(void)
{
    // Enumerate identities without reopening the active preview or interrupting
    // its measurements. Only an actual device change requires a full refresh.
    QStringList devices;
    SDL_UpdateJoysticks();
    int count = 0;
    SDL_JoystickID* joysticks = SDL_GetJoysticks(&count);
    for (int i = 0; i < count; ++i)
        devices.append(QStringLiteral("sdl:%1").arg(joysticks[i]));
    SDL_free(joysticks);

    if (hid_init() == 0)
    {
        hid_device_info* adapters = hid_enumerate(kRaphnetVendorId, 0);
        for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->next)
            if (find_raphnet_adapter(adapter->product_id, adapter->interface_number))
                devices.append(QStringLiteral("hid:%1").arg(QString::fromUtf8(adapter->path)));
        hid_free_enumeration(adapters);
        if (this->hidDevice == nullptr) hid_exit();
    }

    libusb_context* context = nullptr;
    if (libusb_init(&context) == LIBUSB_SUCCESS)
    {
        libusb_device** adapters = nullptr;
        const ssize_t adapterCount = libusb_get_device_list(context, &adapters);
        for (ssize_t i = 0; i < adapterCount; ++i)
        {
            libusb_device_descriptor descriptor = {};
            if (libusb_get_device_descriptor(adapters[i], &descriptor) == LIBUSB_SUCCESS &&
                descriptor.idVendor == kGameCubeAdapterVendorId && descriptor.idProduct == kGameCubeAdapterProductId)
                devices.append(QStringLiteral("gca:%1:%2")
                    .arg(libusb_get_bus_number(adapters[i])).arg(libusb_get_device_address(adapters[i])));
        }
        if (adapters != nullptr) libusb_free_device_list(adapters, 1);
        libusb_exit(context);
    }
    devices.sort();
    return devices;
}

void UnifiedInputDialog::updateWarningLabel(void)
{
    for (int pageIndex = 0; pageIndex < this->controllerPages.size(); ++pageIndex)
    {
        auto* page = this->controllerPages[pageIndex];
        if (page->messages == nullptr) continue;
        QStringList warnings;

        if (this->selectedPlugin == InputPluginType::Raphnet && this->raphnetConnectionSlow)
            warnings.append(tr("Slow controller USB polling detected.\nTry another USB port directly on your computer, without a hub."));

        // This warning describes the adapter's physical mode, not the input type.
        if (this->detectionReport.foundUsbModeMayflash)
            warnings.append(tr("GameCube adapter is in USB mode.\nSet its switch to Wii U/NS (native) mode."));

        if (this->selectedPlugin == InputPluginType::Gamecube)
        {
            if (this->detectionReport.foundBlockedNativeGamecube)
                warnings.append(tr("Cannot access the native GameCube adapter.\nCheck its driver and close other apps using it."));
            if (pageIndex == this->currentPageIndex() && this->gamecubeSelectedPortMissingController)
                warnings.append(tr("No GameCube controller answered on the selected adapter port."));
        }

        if (this->selectedPlugin == InputPluginType::USB && page->deviceComboBox != nullptr)
        {
            const int deviceIndex = page->deviceComboBox->currentData().toInt();
            if (deviceIndex >= 0 && deviceIndex < this->usbDevices.size())
            {
                const auto& device = this->usbDevices[deviceIndex];
                if (device.connected && device.type == InputDeviceType::Joystick)
                {
                    if (usb_device_supports_raphnet_raw(device))
                        warnings.prepend(tr("Raphnet adapter selected as USB input.\nChoose N64 Controller (Raphnet) for better support."));
                }
            }
        }

        const QString message = warnings.join(QStringLiteral("\n\n"));
        static_cast<ControllerMessageArea*>(page->messageArea)->SetMessages(message);
    }
}

void UnifiedInputDialog::offerRaphnetInputType(int pageIndex)
{
    if (this->selectedPlugin != InputPluginType::USB) return;
    const auto* page = this->controllerPages[pageIndex];
    const int index = page->deviceComboBox->currentData().toInt();
    if (index < 0 || index >= this->usbDevices.size()) return;
    const UsbDeviceChoice device = this->usbDevices[index];
    const bool supported = usb_device_supports_raphnet_raw(device);
    if (!device.connected || device.type != InputDeviceType::Joystick || !supported) return;

    const QString key = device.path.isEmpty() ?
        QStringLiteral("%1:%2:%3").arg(device.vendorId).arg(device.productId).arg(device.name) : device.path;
    if (this->raphnetUsbPrompts.contains(key)) return;
    this->raphnetUsbPrompts.insert(key);

    QMessageBox prompt(QMessageBox::Information, tr("Raphnet adapter detected"),
        tr("For this adapter, N64 Controller (Raphnet) is the recommended controller type. Switch to it now?"),
        QMessageBox::Yes | QMessageBox::No, this);
    prompt.setObjectName("raphnetUsbRecommendation");
    prompt.button(QMessageBox::Yes)->setText(tr("Use Raphnet"));
    prompt.button(QMessageBox::No)->setText(tr("Keep USB"));
    prompt.setDefaultButton(QMessageBox::Yes);
    if (prompt.exec() == QMessageBox::Yes) this->setSelectedPlugin(InputPluginType::Raphnet);
}

void UnifiedInputDialog::refreshUsbDevices(void)
{
    this->usbDevices.clear();
    this->usbDevices.append({ InputDeviceType::None, 0, false, QStringLiteral("None"), QString(), QString(), 0, 0 });
    this->usbDevices.append({ InputDeviceType::Keyboard, 0, false, QStringLiteral("Keyboard"), QString(), QString(), 0, 0 });

    if (!SDL_WasInit(SDL_INIT_GAMEPAD) && !SDL_InitSubSystem(SDL_INIT_GAMEPAD))
    {
        return;
    }

    SDL_UpdateJoysticks();

    int joystickCount = 0;
    SDL_JoystickID* joysticks = SDL_GetJoysticks(&joystickCount);
    for (int i = 0; i < joystickCount; i++)
    {
        const SDL_JoystickID joystickId = joysticks[i];
        const bool isGamepad = SDL_IsGamepad(joystickId);
        const char* name = isGamepad ? SDL_GetGamepadNameForID(joystickId) : SDL_GetJoystickNameForID(joystickId);
        const char* path = isGamepad ? SDL_GetGamepadPathForID(joystickId) : SDL_GetJoystickPathForID(joystickId);
        const uint16_t vendorId = isGamepad ? SDL_GetGamepadVendorForID(joystickId) : SDL_GetJoystickVendorForID(joystickId);
        const uint16_t productId = isGamepad ? SDL_GetGamepadProductForID(joystickId) : SDL_GetJoystickProductForID(joystickId);

        this->usbDevices.append({
            InputDeviceType::Joystick,
            joystickId,
            isGamepad,
            string_from_const_char(name),
            string_from_const_char(path),
            QString(),
            vendorId,
            productId
        });
    }

    if (joysticks != nullptr)
    {
        SDL_free(joysticks);
    }

    // Keep disconnected selections visible instead of assigning a different controller.
    for (ControllerPage* page : this->controllerPages)
    {
        const UsbDeviceChoice& saved = page->usbDevice;
        if (saved.type == InputDeviceType::Automatic)
        {
            if (std::none_of(this->usbDevices.begin(), this->usbDevices.end(), [](const UsbDeviceChoice& device)
                { return device.type == InputDeviceType::Automatic; })) this->usbDevices.append(saved);
            continue;
        }
        if (saved.type != InputDeviceType::Joystick) continue;
        const auto matches = [&saved](const UsbDeviceChoice& device)
        {
            return device.type == saved.type && device.name == saved.name &&
                (saved.path.isEmpty() || saved.path == device.path);
        };
        if (std::none_of(this->usbDevices.begin(), this->usbDevices.end(), matches))
        {
            UsbDeviceChoice missing = saved;
            missing.connected = false;
            missing.id = 0;
            this->usbDevices.append(missing);
        }
    }
}

void UnifiedInputDialog::updateAllPages(void)
{
    for (int i = 0; i < static_cast<int>(this->controllerPages.size()); i++)
    {
        this->updatePageMode(i);
    }
    this->updateWarningLabel();
    this->updateRaphnetDiagnostics();
}



void UnifiedInputDialog::updateBackendChoices(void)
{
    if (this->controllerPages.isEmpty() || this->controllerPages[0]->backendComboBox == nullptr)
    {
        return;
    }

    QComboBox* comboBox = this->controllerPages[0]->backendComboBox;
    QSignalBlocker blocker(comboBox);
    comboBox->clear();

    if (this->detectionReport.foundRaphnet || this->selectedPlugin == InputPluginType::Raphnet)
    {
        comboBox->addItem(tr("N64 Controller (Raphnet)"), static_cast<int>(InputPluginType::Raphnet));
    }
    if (this->detectionReport.foundNativeGamecube || this->selectedPlugin == InputPluginType::Gamecube)
    {
        comboBox->addItem(tr("GameCube Controller (Native)"), static_cast<int>(InputPluginType::Gamecube));
    }
    comboBox->addItem(tr("USB Controller / Keyboard"), static_cast<int>(InputPluginType::USB));

    const int targetIndex = comboBox->findData(static_cast<int>(this->selectedPlugin));
    comboBox->setCurrentIndex(targetIndex >= 0 ? targetIndex : 0);
}

void UnifiedInputDialog::updatePageMode(int pageIndex)
{
    ControllerPage* page = this->controllerPages[pageIndex];

    if (pageIndex == 0 && page->backendComboBox != nullptr)
    {
        this->updateBackendChoices();
    }

    this->updatePageDeviceChoices(pageIndex);

    const bool usbMode = this->selectedPlugin == InputPluginType::USB;
    const bool gamecubeMode = this->selectedPlugin == InputPluginType::Gamecube;

    page->mappingsGroupBox->setVisible(true);
    page->usbStickGroupBox->setVisible(usbMode);
    page->gamecubeStickGroupBox->setVisible(gamecubeMode && pageIndex == 0);
    page->gamecubeTriggerGroupBox->setVisible(gamecubeMode && pageIndex == 0);
    page->pluggedInCheckBox->setVisible(pageIndex > 0 && (usbMode || gamecubeMode));
    QSignalBlocker enabledBlocker(page->pluggedInCheckBox);
    page->pluggedInCheckBox->setChecked(usbMode ? page->usbEnabled : page->gamecubeEnabled);

    if (usbMode)
    {
        page->portGroupBox->setTitle(pageIndex == 0 ? tr("Controller") : tr("Input Device"));
    }
    else if (gamecubeMode)
    {
        page->portGroupBox->setTitle(pageIndex == 0 ? tr("Controller") : tr("Adapter Port"));
    }
    else
    {
        page->portGroupBox->setTitle(pageIndex == 0 ? tr("Controller") : tr("Adapter Port"));
    }

    for (int i = 0; i < static_cast<int>(page->bindingButtons.size()); i++)
    {
        const bool analogBinding = kBindingTargets[static_cast<size_t>(i)].axisXDirection != 0 ||
            kBindingTargets[static_cast<size_t>(i)].axisYDirection != 0;
        const bool enabled = usbMode || (gamecubeMode && pageIndex == 0 && !analogBinding);
        page->bindingButtons[i]->setEnabled(enabled);
        page->clearButtons[i]->setEnabled(enabled);
    }

    this->updatePageBindingButtons(pageIndex);
    this->updateSliderLabels(pageIndex);
}

void UnifiedInputDialog::updatePageDeviceChoices(int pageIndex)
{
    ControllerPage* page = this->controllerPages[pageIndex];
    QSignalBlocker blocker(page->deviceComboBox);
    page->deviceComboBox->clear();
    page->deviceComboBox->setEnabled(true);

    if (this->selectedPlugin == InputPluginType::USB)
    {
        int selectedIndex = -1;
        for (int i = 0; i < this->usbDevices.size(); ++i)
        {
            const UsbDeviceChoice& device = this->usbDevices[i];
            if (pageIndex == 0 && device.type == InputDeviceType::None) continue;
            const QString name = device.name.isEmpty() ? tr("Unnamed controller") : device.name;
            page->deviceComboBox->addItem(device.connected ? name : tr("%1 (disconnected)").arg(name), i);
            const UsbDeviceChoice& saved = page->usbDevice;
            if (device.type == saved.type && device.name == saved.name &&
                (saved.path.isEmpty() || device.path == saved.path))
                selectedIndex = page->deviceComboBox->count() - 1;
        }
        if (selectedIndex < 0) selectedIndex = 0;
        page->deviceComboBox->setCurrentIndex(selectedIndex);
        const int index = page->deviceComboBox->currentData().toInt();
        if (index >= 0 && index < this->usbDevices.size())
        {
            const QString serial = page->usbDevice.serial;
            page->usbDevice = this->usbDevices[index];
            if (page->usbDevice.serial.isEmpty()) page->usbDevice.serial = serial;
        }
        return;
    }

    if (this->selectedPlugin == InputPluginType::Gamecube)
    {
        for (int port = 0; port < 4; ++port)
            page->deviceComboBox->addItem(tr("Adapter port %1").arg(port + 1), port);
        page->deviceComboBox->setCurrentIndex(page->gamecubePort);
        return;
    }

    // raphnet swaps Player 1 with the selected channel; other slots follow that same map.
    const int portCount = std::max(this->raphnetChannelCount, this->raphnetPlayer1Port + 1);
    if (pageIndex == 0)
    {
        for (int port = 0; port < portCount; ++port)
            page->deviceComboBox->addItem(tr("Adapter port %1").arg(port + 1), port);
        page->deviceComboBox->setCurrentIndex(this->raphnetPlayer1Port);
    }
    else
    {
        const int port = pageIndex == this->raphnetPlayer1Port ? 0 : pageIndex;
        page->deviceComboBox->addItem(port < this->raphnetChannelCount ?
            tr("Adapter port %1").arg(port + 1) : tr("No adapter port"), port);
        page->deviceComboBox->setEnabled(false);
    }
}

void UnifiedInputDialog::updatePageBindingButtons(int pageIndex)
{
    ControllerPage* page = this->controllerPages[pageIndex];
    const bool usbMode = this->selectedPlugin == InputPluginType::USB;
    const bool gamecubeMode = this->selectedPlugin == InputPluginType::Gamecube;

    for (int i = 0; i < static_cast<int>(page->bindingButtons.size()); i++)
    {
        if (this->listeningPageIndex == pageIndex && this->listeningBindingIndex == i)
        {
            page->bindingButtons[i]->setText(tr("Press input..."));
            continue;
        }

        if (usbMode)
        {
            page->bindingButtons[i]->setText(binding_text(page->usbBindings[i]));
        }
        else if (gamecubeMode && kBindingTargets[static_cast<size_t>(i)].hasGamecubeMapping)
        {
            page->bindingButtons[i]->setText(gc_input_to_string(static_cast<GCInput>(this->controllerPages[0]->gamecubeBindings[i])));
        }
        else
        {
            page->bindingButtons[i]->setText(tr("Fixed"));
        }
    }
}

void UnifiedInputDialog::updateSliderLabels(int pageIndex)
{
    ControllerPage* page = this->controllerPages[pageIndex];
    if (page->usbDeadzoneValueLabel != nullptr && page->usbRangeValueLabel != nullptr)
    {
        page->usbDeadzoneValueLabel->setText(tr("%1%").arg(page->usbDeadzoneSlider->value()));
        page->usbRangeValueLabel->setText(tr("%1%")
            .arg(page->realN64RangeCheckBox->isChecked() ? 66 : page->usbRangeSlider->value()));
    }

    if (page->gamecubeDeadzoneValueLabel != nullptr &&
        page->gamecubeSensitivityValueLabel != nullptr &&
        page->gamecubeTriggerThresholdValueLabel != nullptr)
    {
        page->gamecubeDeadzoneValueLabel->setText(tr("%1%").arg(page->gamecubeDeadzoneSlider->value()));
        page->gamecubeSensitivityValueLabel->setText(tr("%1%").arg(page->gamecubeSensitivitySlider->value()));
        page->gamecubeTriggerThresholdValueLabel->setText(tr("%1%").arg(page->gamecubeTriggerThresholdSlider->value()));
    }
}

void UnifiedInputDialog::setSelectedPlugin(InputPluginType plugin)
{
    this->stopListeningForBinding(true);
    this->syncSharedUsbProfile(this->currentPageIndex());
    this->selectedPlugin = plugin;
    this->updateAllPages();
    this->openPreviewSource();
}

int UnifiedInputDialog::currentPageIndex(void) const
{
    if (this->tabWidget == nullptr || this->controllerPages.isEmpty())
    {
        return 0;
    }

    return std::clamp(this->tabWidget->currentIndex(), 0, static_cast<int>(this->controllerPages.size()) - 1);
}

void UnifiedInputDialog::loadPageSettings(int pageIndex)
{
    ControllerPage* page = this->controllerPages[pageIndex];
    page->usbSection = usb_effective_section(pageIndex);
    const std::string& section = page->usbSection;
    QSignalBlocker enabledBlocker(page->pluggedInCheckBox);
    const bool usbSectionExists = CoreSettingsSectionExists(section);

    if (usbSectionExists)
    {
        page->pluggedInCheckBox->setChecked(pageIndex == 0 ||
            CoreSettingsGetBoolValue(SettingsID::Input_PluggedIn, section));
        page->usbDeadzoneSlider->setValue(CoreSettingsGetIntValue(SettingsID::Input_Deadzone, section));
        page->usbRangeSlider->setValue(CoreSettingsGetIntValue(SettingsID::Input_Range, section));
        page->realN64RangeCheckBox->setChecked(CoreSettingsGetBoolValue(SettingsID::Input_RealN64Range, section));
    }
    else
    {
        page->usbDirty = true;
        page->pluggedInCheckBox->setChecked(pageIndex == 0);
        page->usbDeadzoneSlider->setValue(9);
        page->usbRangeSlider->setValue(66);
        page->realN64RangeCheckBox->setChecked(true);
    }

    page->usbRangeSlider->setEnabled(!page->realN64RangeCheckBox->isChecked());

    for (int i = 0; i < static_cast<int>(kBindingTargets.size()); i++)
    {
        if (usbSectionExists)
        {
            page->usbBindings[i] = load_usb_binding(kBindingTargets[static_cast<size_t>(i)], section);
        }
        else
        {
            clear_binding(page->usbBindings[i]);
        }
        if (!kBindingTargets[static_cast<size_t>(i)].hasGamecubeMapping)
        {
            page->gamecubeBindings[i] = static_cast<int>(GCInput::None);
            continue;
        }

        page->gamecubeBindings[i] = CoreSettingsGetIntValue(kBindingTargets[static_cast<size_t>(i)].gamecubeMapping);
    }

    page->gamecubeDeadzoneSlider->setValue(CoreSettingsGetIntValue(SettingsID::GCAInput_Deadzone));
    page->gamecubeSensitivitySlider->setValue(CoreSettingsGetIntValue(SettingsID::GCAInput_Sensitivity));
    page->gamecubeTriggerThresholdSlider->setValue(CoreSettingsGetIntValue(SettingsID::GCAInput_TriggerTreshold));
    const bool leftTriggerAnalog = CoreSettingsGetBoolValue(SettingsID::GCAInput_LeftTriggerAnalog);
    const bool rightTriggerAnalog = CoreSettingsGetBoolValue(SettingsID::GCAInput_RightTriggerAnalog);
    {
        QSignalBlocker leftDigitalBlocker(page->gamecubeLeftTriggerDigitalRadioButton);
        QSignalBlocker leftAnalogBlocker(page->gamecubeLeftTriggerAnalogRadioButton);
        QSignalBlocker rightDigitalBlocker(page->gamecubeRightTriggerDigitalRadioButton);
        QSignalBlocker rightAnalogBlocker(page->gamecubeRightTriggerAnalogRadioButton);
        page->gamecubeLeftTriggerDigitalRadioButton->setChecked(!leftTriggerAnalog);
        page->gamecubeLeftTriggerAnalogRadioButton->setChecked(leftTriggerAnalog);
        page->gamecubeRightTriggerDigitalRadioButton->setChecked(!rightTriggerAnalog);
        page->gamecubeRightTriggerAnalogRadioButton->setChecked(rightTriggerAnalog);
    }
    apply_gamecube_trigger_mode(page->gamecubeBindings, true, leftTriggerAnalog);
    apply_gamecube_trigger_mode(page->gamecubeBindings, false, rightTriggerAnalog);

    page->usbEnabled = page->pluggedInCheckBox->isChecked();
    if (usbSectionExists)
    {
        page->usbDevice.type = static_cast<InputDeviceType>(CoreSettingsGetIntValue(SettingsID::Input_DeviceType, section));
        page->usbDevice.name = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Input_DeviceName, section));
        page->usbDevice.path = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Input_DevicePath, section));
        page->usbDevice.serial = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Input_DeviceSerial, section));
    }
    else
    {
        page->usbDevice.type = pageIndex == 0 ? InputDeviceType::Keyboard : InputDeviceType::None;
        page->usbDevice.name = pageIndex == 0 ? QStringLiteral("Keyboard") : QStringLiteral("None");
    }
    const std::array<SettingsID, 4> portSettings = {{
        SettingsID::GCAInput_Port1Enabled, SettingsID::GCAInput_Port2Enabled,
        SettingsID::GCAInput_Port3Enabled, SettingsID::GCAInput_Port4Enabled
    }};
    std::array<bool, 4> enabledPorts{};
    for (size_t port = 0; port < enabledPorts.size(); ++port)
        enabledPorts[port] = CoreSettingsGetBoolValue(portSettings[port]);
    const auto ports = ResolveGameCubeControllerPorts(
        CoreSettingsGetIntListValue(SettingsID::GCAInput_ControllerPorts), enabledPorts);
    page->gamecubeEnabled = ports[static_cast<size_t>(pageIndex)] >= 0;
    page->gamecubePort = page->gamecubeEnabled ? std::clamp(ports[static_cast<size_t>(pageIndex)], 0, 3) : pageIndex;
    if (pageIndex == 0) page->gamecubeEnabled = true;

    this->updatePageBindingButtons(pageIndex);
    this->updateSliderLabels(pageIndex);
}

void UnifiedInputDialog::saveAllSettings(void)
{
    this->syncSharedUsbProfile(this->currentPageIndex());
    if (this->selectedPlugin == InputPluginType::USB)
    {
        for (int i = 0; i < static_cast<int>(this->controllerPages.size()); i++)
        {
            this->saveUsbSettings(i);
        }
    }
    else if (this->selectedPlugin == InputPluginType::Gamecube)
    {
        this->saveGamecubeSettings();
    }
    else if (this->selectedPlugin == InputPluginType::Raphnet)
    {
        this->saveRaphnetSettings();
    }

    CoreSettingsSave();
}

void UnifiedInputDialog::syncSharedUsbProfile(int pageIndex)
{
    if (this->selectedPlugin != InputPluginType::USB) return;
    const ControllerPage* source = this->controllerPages[pageIndex];
    if (!source->usbDirty) return;
    for (int i = 0; i < this->controllerPages.size(); ++i)
    {
        ControllerPage* page = this->controllerPages[i];
        if (page == source || page->usbSection != source->usbSection) continue;
        QSignalBlocker enabled(page->pluggedInCheckBox);
        QSignalBlocker deadzone(page->usbDeadzoneSlider);
        QSignalBlocker range(page->usbRangeSlider);
        QSignalBlocker n64Range(page->realN64RangeCheckBox);
        page->usbDirty = true;
        page->usbEnabled = source->usbEnabled;
        page->usbDevice = source->usbDevice;
        page->usbBindings = source->usbBindings;
        page->pluggedInCheckBox->setChecked(page->usbEnabled);
        page->usbDeadzoneSlider->setValue(source->usbDeadzoneSlider->value());
        page->usbRangeSlider->setValue(source->usbRangeSlider->value());
        page->realN64RangeCheckBox->setChecked(source->realN64RangeCheckBox->isChecked());
        page->usbRangeSlider->setEnabled(!page->realN64RangeCheckBox->isChecked());
        this->updatePageDeviceChoices(i);
        this->updatePageBindingButtons(i);
        this->updateSliderLabels(i);
    }
}

void UnifiedInputDialog::saveUsbSettings(int pageIndex)
{
    ControllerPage* page = this->controllerPages[pageIndex];
    const std::string& section = page->usbSection;
    if (!page->usbDirty && CoreSettingsSectionExists(section)) return;
    const int deviceIndex = page->deviceComboBox->currentData().toInt();
    UsbDeviceChoice device;
    if (deviceIndex >= 0 && deviceIndex < static_cast<int>(this->usbDevices.size()))
    {
        device = this->usbDevices[deviceIndex];
    }

    if (pageIndex == 0 && device.type == InputDeviceType::None)
    {
        for (const UsbDeviceChoice& fallbackDevice : this->usbDevices)
        {
            if (fallbackDevice.type == InputDeviceType::Joystick)
            {
                device = fallbackDevice;
                break;
            }
        }
        if (device.type == InputDeviceType::None)
        {
            for (const UsbDeviceChoice& fallbackDevice : this->usbDevices)
            {
                if (fallbackDevice.type == InputDeviceType::Keyboard)
                {
                    device = fallbackDevice;
                    break;
                }
            }
        }
    }

    const bool pluggedIn = pageIndex == 0 ||
        (page->pluggedInCheckBox->isChecked() && device.type != InputDeviceType::None);
    CoreSettingsSetValue(SettingsID::Input_PluggedIn, section, pluggedIn);
    CoreSettingsSetValue(SettingsID::Input_DeviceName, section, device.name.toStdString());
    CoreSettingsSetValue(SettingsID::Input_DeviceType, section, static_cast<int>(device.type));
    CoreSettingsSetValue(SettingsID::Input_DevicePath, section, device.path.toStdString());
    CoreSettingsSetValue(SettingsID::Input_DeviceSerial, section, (device.serial.isEmpty() ? page->usbDevice.serial : device.serial).toStdString());
    CoreSettingsSetValue(SettingsID::Input_Deadzone, section, page->usbDeadzoneSlider->value());
    CoreSettingsSetValue(SettingsID::Input_Range, section, page->realN64RangeCheckBox->isChecked() ? 66 : page->usbRangeSlider->value());
    CoreSettingsSetValue(SettingsID::Input_RealN64Range, section, page->realN64RangeCheckBox->isChecked());
    if (!CoreSettingsKeyExists(section, "Pak"))
        CoreSettingsSetValue(SettingsID::Input_Pak, section, static_cast<int>(N64ControllerPak::MemoryPak));

    for (int i = 0; i < static_cast<int>(kBindingTargets.size()); i++)
    {
        const BindingTarget& target = kBindingTargets[static_cast<size_t>(i)];
        const BindingValue& binding = page->usbBindings[i];
        CoreSettingsSetValue(target.usbInputType, section, to_std_vector(binding.types));
        CoreSettingsSetValue(target.usbName, section, to_std_string_vector(binding.text));
        CoreSettingsSetValue(target.usbData, section, to_std_vector(binding.data));
        CoreSettingsSetValue(target.usbExtraData, section, to_std_vector(binding.extraData));
    }
}

void UnifiedInputDialog::saveGamecubeSettings(void)
{
    ControllerPage* page = this->controllerPages[0];
    CoreSettingsSetValue(SettingsID::GCAInput_Deadzone, page->gamecubeDeadzoneSlider->value());
    CoreSettingsSetValue(SettingsID::GCAInput_Sensitivity, page->gamecubeSensitivitySlider->value());
    CoreSettingsSetValue(SettingsID::GCAInput_TriggerTreshold, page->gamecubeTriggerThresholdSlider->value());
    CoreSettingsSetValue(SettingsID::GCAInput_LeftTriggerAnalog, page->gamecubeLeftTriggerAnalogRadioButton->isChecked());
    CoreSettingsSetValue(SettingsID::GCAInput_RightTriggerAnalog, page->gamecubeRightTriggerAnalogRadioButton->isChecked());

    const std::array<SettingsID, 4> portSettings = {{
        SettingsID::GCAInput_Port1Enabled,
        SettingsID::GCAInput_Port2Enabled,
        SettingsID::GCAInput_Port3Enabled,
        SettingsID::GCAInput_Port4Enabled
    }};
    std::array<bool, 4> enabledPorts = {{ false, false, false, false }};
    std::vector<int> controllerPorts(4, -1);
    for (int i = 0; i < static_cast<int>(this->controllerPages.size()); i++)
    {
        ControllerPage* playerPage = this->controllerPages[i];
        if (i > 0 && !playerPage->pluggedInCheckBox->isChecked())
        {
            continue;
        }

        const int port = std::clamp(playerPage->deviceComboBox->currentData().toInt(), 0, 3);
        enabledPorts[static_cast<size_t>(port)] = true;
        controllerPorts[static_cast<size_t>(i)] = port;
    }

    CoreSettingsSetValue(SettingsID::GCAInput_ControllerPorts, controllerPorts);
    for (int i = 0; i < 4; i++)
    {
        CoreSettingsSetValue(portSettings[static_cast<size_t>(i)], enabledPorts[static_cast<size_t>(i)]);
    }

    for (int i = 0; i < static_cast<int>(kBindingTargets.size()); i++)
    {
        const BindingTarget& target = kBindingTargets[static_cast<size_t>(i)];
        if (!target.hasGamecubeMapping)
        {
            continue;
        }

        CoreSettingsSetValue(target.gamecubeMapping, page->gamecubeBindings[i]);
    }
}

void UnifiedInputDialog::saveRaphnetSettings(void)
{
    if (this->controllerPages.isEmpty())
    {
        return;
    }

    const int port = std::max(0, this->controllerPages[0]->deviceComboBox->currentData().toInt());
    CoreSettingsSetValue(SettingsID::RaphnetRaw_Player1AdapterPort, port + 1);
}

void UnifiedInputDialog::restoreCurrentPageDefaults(void)
{
    const int pageIndex = this->currentPageIndex();
    ControllerPage* page = this->controllerPages[pageIndex];

    this->stopListeningForBinding(true);

    if (this->selectedPlugin == InputPluginType::USB)
    {
        page->usbDirty = true;
        page->pluggedInCheckBox->setChecked(pageIndex == 0);
        page->usbDeadzoneSlider->setValue(9);
        page->usbRangeSlider->setValue(66);
        page->realN64RangeCheckBox->setChecked(true);
        for (BindingValue& binding : page->usbBindings)
        {
            clear_binding(binding);
        }
    }
    else if (this->selectedPlugin == InputPluginType::Gamecube && pageIndex == 0)
    {
        page->gamecubeDeadzoneSlider->setValue(CoreSettingsGetDefaultIntValue(SettingsID::GCAInput_Deadzone));
        page->gamecubeSensitivitySlider->setValue(CoreSettingsGetDefaultIntValue(SettingsID::GCAInput_Sensitivity));
        page->gamecubeTriggerThresholdSlider->setValue(CoreSettingsGetDefaultIntValue(SettingsID::GCAInput_TriggerTreshold));
        const bool leftTriggerAnalog = CoreSettingsGetDefaultBoolValue(SettingsID::GCAInput_LeftTriggerAnalog);
        const bool rightTriggerAnalog = CoreSettingsGetDefaultBoolValue(SettingsID::GCAInput_RightTriggerAnalog);
        {
            QSignalBlocker leftDigitalBlocker(page->gamecubeLeftTriggerDigitalRadioButton);
            QSignalBlocker leftAnalogBlocker(page->gamecubeLeftTriggerAnalogRadioButton);
            QSignalBlocker rightDigitalBlocker(page->gamecubeRightTriggerDigitalRadioButton);
            QSignalBlocker rightAnalogBlocker(page->gamecubeRightTriggerAnalogRadioButton);
            page->gamecubeLeftTriggerDigitalRadioButton->setChecked(!leftTriggerAnalog);
            page->gamecubeLeftTriggerAnalogRadioButton->setChecked(leftTriggerAnalog);
            page->gamecubeRightTriggerDigitalRadioButton->setChecked(!rightTriggerAnalog);
            page->gamecubeRightTriggerAnalogRadioButton->setChecked(rightTriggerAnalog);
        }
        for (int i = 0; i < static_cast<int>(kBindingTargets.size()); i++)
        {
            const BindingTarget& target = kBindingTargets[static_cast<size_t>(i)];
            page->gamecubeBindings[i] = target.hasGamecubeMapping ?
                CoreSettingsGetDefaultIntValue(target.gamecubeMapping) :
                static_cast<int>(GCInput::None);
        }
        apply_gamecube_trigger_mode(page->gamecubeBindings, true, leftTriggerAnalog);
        apply_gamecube_trigger_mode(page->gamecubeBindings, false, rightTriggerAnalog);
    }

    this->updatePageBindingButtons(pageIndex);
    this->updateSliderLabels(pageIndex);
}

void UnifiedInputDialog::startListeningForBinding(int pageIndex, int bindingIndex)
{
    if (this->selectedPlugin == InputPluginType::Raphnet)
    {
        return;
    }

    if (this->selectedPlugin == InputPluginType::Gamecube &&
        (pageIndex != 0 || !kBindingTargets[static_cast<size_t>(bindingIndex)].hasGamecubeMapping))
    {
        return;
    }

    this->stopListeningForBinding(true);
    this->listeningPageIndex = pageIndex;
    this->listeningBindingIndex = bindingIndex;
    this->listeningArmed = false;
    this->listeningTimer.start();
    this->updatePageBindingButtons(pageIndex);
    this->pollTimer->start();
}

void UnifiedInputDialog::stopListeningForBinding(bool restoreText)
{
    const int oldPageIndex = this->listeningPageIndex;
    this->listeningPageIndex = -1;
    this->listeningBindingIndex = -1;

    if (restoreText && oldPageIndex >= 0 && oldPageIndex < static_cast<int>(this->controllerPages.size()))
    {
        this->updatePageBindingButtons(oldPageIndex);
    }
}

void UnifiedInputDialog::clearBinding(int pageIndex, int bindingIndex)
{
    ControllerPage* page = this->controllerPages[pageIndex];
    this->stopListeningForBinding(true);
    if (this->selectedPlugin == InputPluginType::USB)
    {
        page->usbDirty = true;
        clear_binding(page->usbBindings[bindingIndex]);
        this->manualInputChoice = true;
    }
    else if (this->selectedPlugin == InputPluginType::Gamecube &&
             kBindingTargets[static_cast<size_t>(bindingIndex)].hasGamecubeMapping)
    {
        page->gamecubeBindings[bindingIndex] = static_cast<int>(GCInput::None);
        this->manualInputChoice = true;
    }

    this->updatePageBindingButtons(pageIndex);
}

void UnifiedInputDialog::setGamecubeTriggerAnalog(bool leftTrigger, bool analog)
{
    if (this->selectedPlugin != InputPluginType::Gamecube ||
        this->controllerPages.isEmpty())
    {
        return;
    }

    if (this->settingsLoaded) this->manualInputChoice = true;
    apply_gamecube_trigger_mode(this->controllerPages[0]->gamecubeBindings, leftTrigger, analog);
    this->updatePageBindingButtons(0);
}

void UnifiedInputDialog::keyPressEvent(QKeyEvent* event)
{
    if (event->isAutoRepeat())
    {
        QDialog::keyPressEvent(event);
        return;
    }

    const int key = Utilities::QtKeyToSdl3Key(event->key());
    this->keyboardState[key] = true;
    if (event->key() == Qt::Key_Escape && this->listeningPageIndex >= 0)
    {
        this->stopListeningForBinding(true);
        return;
    }

    if (this->selectedPlugin == InputPluginType::USB &&
        this->listeningPageIndex >= 0 &&
        this->listeningBindingIndex >= 0)
    {
        if (key == SDL_SCANCODE_UNKNOWN) return;
        ControllerPage* page = this->controllerPages[this->listeningPageIndex];
        page->usbDirty = true;
        set_single_binding(page->usbBindings[this->listeningBindingIndex], InputType::Keyboard, key, 0,
            QString::fromUtf8(SDL_GetScancodeName(static_cast<SDL_Scancode>(key))));
        this->manualInputChoice = true;
        const int pageIndex = this->listeningPageIndex;
        this->stopListeningForBinding(false);
        this->updatePageBindingButtons(pageIndex);
        return;
    }

    QDialog::keyPressEvent(event);
}

void UnifiedInputDialog::keyReleaseEvent(QKeyEvent* event)
{
    if (event->isAutoRepeat())
    {
        QDialog::keyReleaseEvent(event);
        return;
    }

    const int key = Utilities::QtKeyToSdl3Key(event->key());
    this->keyboardState[key] = false;
    QDialog::keyReleaseEvent(event);
}

void UnifiedInputDialog::done(int result)
{
    this->deviceTimer->stop();
    this->stopListeningForBinding(true);
    this->closePreviewSource();
    QDialog::done(result);
}

bool UnifiedInputDialog::eventFilter(QObject* object, QEvent* event)
{
    auto* widget = qobject_cast<QWidget*>(object);
    if (widget == nullptr || widget->window() != this) return false;
    if (event->type() == QEvent::WindowDeactivate)
    {
        this->keyboardState.clear();
        this->stopListeningForBinding(true);
    }
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease)
    {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (!keyEvent->isAutoRepeat())
        {
            const int key = Utilities::QtKeyToSdl3Key(keyEvent->key());
            this->keyboardState[key] = event->type() == QEvent::KeyPress;
        }
        if (event->type() == QEvent::KeyPress && this->listeningPageIndex >= 0)
        {
            this->keyPressEvent(keyEvent);
            return true;
        }
    }
    return QDialog::eventFilter(object, event);
}

void UnifiedInputDialog::clearPreview(void)
{
    const int pageIndex = this->currentPageIndex();
    if (pageIndex < 0 || pageIndex >= static_cast<int>(this->controllerPages.size()))
    {
        return;
    }

    ControllerPage* page = this->controllerPages[pageIndex];
    if (page->controllerImageWidget != nullptr)
    {
        page->controllerImageWidget->ClearControllerState();
    }

    if (page->axisXLabel != nullptr && page->axisYLabel != nullptr)
    {
        page->axisXLabel->setText(QStringLiteral("0"));
        page->axisYLabel->setText(QStringLiteral("0"));
    }
}

void UnifiedInputDialog::openPreviewSource(void)
{
    this->closePreviewSource();
    this->clearPreview();
    this->gamecubeSelectedPortMissingController = false;
    this->keyboardState.clear();
    this->raphnetPollingHealth.reset();
    this->raphnetPollingHealth.connection.cached = this->raphnetConnectionSlow;
    this->raphnetMeasurementTimer.start();
    this->updateRaphnetDiagnostics();
    this->updateWarningLabel();
    ControllerPage* page = this->controllerPages[this->currentPageIndex()];
    if ((this->selectedPlugin == InputPluginType::USB && !page->usbEnabled) ||
        (this->selectedPlugin == InputPluginType::Gamecube && !page->gamecubeEnabled))
    {
        show_status(page->statusLabel, tr("This player is disabled."));
        return;
    }

    bool opened = false;
    switch (this->selectedPlugin)
    {
    case InputPluginType::Raphnet:
        opened = this->openRaphnetPreview();
        break;
    case InputPluginType::Gamecube:
        opened = this->openGamecubePreview();
        break;
    case InputPluginType::USB:
    default:
        opened = this->openUsbPreview();
        break;
    }

    if (opened)
    {
        this->pollTimer->start();
    }
    else
    {
        this->raphnetConnectionSlow = false;
        this->updateWarningLabel();
        this->closePreviewSource();
    }
}

void UnifiedInputDialog::closePreviewSource(void)
{
    if (this->pollTimer != nullptr)
    {
        this->pollTimer->stop();
    }

    if (this->hidDevice != nullptr)
    {
        this->setRaphnetPollingSuspended(false);
        hid_close(this->hidDevice);
        this->hidDevice = nullptr;
        hid_exit();
    }

    if (this->gamecubeHandle != nullptr)
    {
        if (this->gamecubeInterfaceClaimed)
        {
            libusb_release_interface(this->gamecubeHandle, 0);
        }
        libusb_close(this->gamecubeHandle);
        this->gamecubeHandle = nullptr;
        this->gamecubeInterfaceClaimed = false;
    }
    if (this->usbContext != nullptr)
    {
        libusb_exit(this->usbContext);
        this->usbContext = nullptr;
    }

    if (this->sdlGamepad != nullptr)
    {
        SDL_CloseGamepad(this->sdlGamepad);
        this->sdlGamepad = nullptr;
        this->sdlJoystick = nullptr;
    }
    else if (this->sdlJoystick != nullptr)
    {
        SDL_CloseJoystick(this->sdlJoystick);
        this->sdlJoystick = nullptr;
    }

    this->previewBackend = PreviewBackend::None;
}

void UnifiedInputDialog::pollPreview(void)
{
    bool ok = false;
    switch (this->previewBackend)
    {
    case PreviewBackend::Raphnet:
        ok = this->pollRaphnetPreview();
        break;
    case PreviewBackend::Gamecube:
        ok = this->pollGamecubePreview();
        break;
    case PreviewBackend::USB:
        ok = this->pollUsbPreview();
        break;
    case PreviewBackend::None:
    default:
        break;
    }

    if (!ok)
    {
        this->clearPreview();
    }

    if (this->listeningPageIndex >= 0)
    {
        if (this->listeningTimer.elapsed() >= 5000)
        {
            this->stopListeningForBinding(true);
        }
    }
}

bool UnifiedInputDialog::openRaphnetPreview(void)
{
    const int pageIndex = this->currentPageIndex();
    ControllerPage* page = this->controllerPages[pageIndex];

    hid_init();

    struct hid_device_info* devices = hid_enumerate(kRaphnetVendorId, 0);
    struct hid_device_info* current = devices;

    while (current != nullptr)
    {
        const RaphnetAdapterDef* adapter = find_raphnet_adapter(
            static_cast<uint16_t>(current->product_id), current->interface_number);
        if (adapter != nullptr)
        {
            this->hidDevice = hid_open_path(current->path);
            if (this->hidDevice != nullptr)
            {
                this->raphnetReportSize = adapter->reportSize;
                this->raphnetChannelCount = adapter->rawChannels;
                hid_set_nonblocking(this->hidDevice, 1);
                break;
            }
        }

        current = current->next;
    }

    hid_free_enumeration(devices);

    if (this->hidDevice == nullptr)
    {
        show_status(page->statusLabel, tr("No raphnet raw-access adapter was detected."));
        hid_exit();
        return false;
    }

    for (int i = 0; i < this->controllerPages.size(); ++i) this->updatePageDeviceChoices(i);
    if (!this->setRaphnetPollingSuspended(true))
    {
        show_status(page->statusLabel, tr("Could not start the raphnet input test. Close other apps using the adapter; RMG-K will retry automatically."));
        return false;
    }
    this->previewBackend = PreviewBackend::Raphnet;
    clear_status(page->statusLabel);
    return true;
}

bool UnifiedInputDialog::exchangeRaphnetCommand(const unsigned char* command, int commandLength, unsigned char* response, int& responseLength)
{
    if (this->hidDevice == nullptr || command == nullptr || response == nullptr ||
        commandLength <= 0 || commandLength > this->raphnetReportSize)
    {
        return false;
    }

    unsigned char buffer[64] = {};
    buffer[0] = 0x00;
    std::memcpy(buffer + 1, command, static_cast<size_t>(commandLength));

    int result = hid_send_feature_report(this->hidDevice, buffer, this->raphnetReportSize + 1);
    if (result < 0)
    {
        return false;
    }

    for (int attempt = 0; attempt < 8; attempt++)
    {
        std::memset(buffer, 0, sizeof(buffer));
        buffer[0] = 0x00;

        result = hid_get_feature_report(this->hidDevice, buffer, this->raphnetReportSize + 1);
        if (result < 0)
        {
            return false;
        }
        if (result <= 1)
        {
            continue;
        }

        responseLength = result - 1;
        std::memcpy(response, buffer + 1, static_cast<size_t>(responseLength));
        if (response[0] == command[0])
        {
            return true;
        }
    }

    return false;
}

bool UnifiedInputDialog::setRaphnetPollingSuspended(bool suspended)
{
    unsigned char command[2] = { kRaphnetSuspendPolling, static_cast<unsigned char>(suspended ? 1 : 0) };
    unsigned char response[64] = {};
    int responseLength = 0;
    return this->exchangeRaphnetCommand(command, sizeof(command), response, responseLength);
}

bool UnifiedInputDialog::pollRaphnetPreview(void)
{
    if (this->hidDevice == nullptr)
    {
        return false;
    }

    const int pageIndex = this->currentPageIndex();
    ControllerPage* page = this->controllerPages[pageIndex];
    const int channel = std::max(0, page->deviceComboBox->currentData().toInt());
    unsigned char command[4] = { kRaphnetRawSiCommand, static_cast<unsigned char>(channel), 0x01, kN64GetStatus };
    unsigned char response[64] = {};
    int responseLength = 0;

    QElapsedTimer timer;
    timer.start();
    const bool success = this->exchangeRaphnetCommand(command, sizeof(command), response, responseLength);
    const qint64 elapsedUs = timer.nsecsElapsed() / 1000;
    if (!success || responseLength < 7 || response[1] != channel || response[2] != 4)
    {
        this->raphnetPollingHealth.reset();
        this->raphnetConnectionSlow = false;
        this->updateWarningLabel();
        this->updateRaphnetDiagnostics();
        show_status(page->statusLabel, tr("No valid controller response. Check the selected adapter port and connection."));
        return false;
    }
    this->raphnetPollingHealth.observe(elapsedUs, this->raphnetMeasurementTimer.nsecsElapsed() / 1000);
    const bool slow = this->raphnetPollingHealth.connection.cached != 0;
    if (slow != this->raphnetConnectionSlow)
    {
        this->raphnetConnectionSlow = slow;
        this->updateWarningLabel();
    }
    this->updateRaphnetDiagnostics();

    const uint16_t buttons = (static_cast<uint16_t>(response[3]) << 8) | response[4];
    const int8_t xAxis = static_cast<int8_t>(response[5]);
    const int8_t yAxis = static_cast<int8_t>(response[6]);

    apply_n64_buttons(page->controllerImageWidget, buttons);
    page->controllerImageWidget->SetXAxisState(-normalize_axis_to_percent(xAxis, 127));
    page->controllerImageWidget->SetYAxisState(normalize_axis_to_percent(yAxis, 127));
    page->controllerImageWidget->UpdateImage();
    page->axisXLabel->setText(QString::number(xAxis));
    page->axisYLabel->setText(QString::number(yAxis));
    clear_status(page->statusLabel);
    return true;
}

void UnifiedInputDialog::updateRaphnetDiagnostics(void)
{
    if (this->raphnetTimingLabel == nullptr) return;
    const bool raphnet = this->selectedPlugin == InputPluginType::Raphnet;
    this->raphnetTimingLabel->setVisible(raphnet);
    const auto& health = this->raphnetPollingHealth;
    this->raphnetTimingLabel->setText(health.samples == 0 ?
        tr("Waiting for a controller response. Plug a controller into the adapter and select its port. A missing response is not a latency measurement.") :
        tr("Controller response: %1 samples · average %2 ms · peak %3 ms.")
            .arg(health.samples).arg(health.totalUs / (1000.0 * health.samples), 0, 'f', 2)
            .arg(health.maximumUs / 1000.0, 0, 'f', 2));
}

bool UnifiedInputDialog::openGamecubePreview(void)
{
    const int pageIndex = this->currentPageIndex();
    ControllerPage* page = this->controllerPages[pageIndex];

    int result = libusb_init(&this->usbContext);
    if (result != LIBUSB_SUCCESS)
    {
        show_status(page->statusLabel,
            tr("GameCube adapter scan unavailable: %1").arg(QString::fromLatin1(libusb_error_name(result))));
        this->usbContext = nullptr;
        return false;
    }

    this->gamecubeHandle = libusb_open_device_with_vid_pid(
        this->usbContext, kGameCubeAdapterVendorId, kGameCubeAdapterProductId);
    if (this->gamecubeHandle == nullptr)
    {
        show_status(page->statusLabel, tr("No GameCube adapter detected in Wii U/NS (native) mode."));
        return false;
    }

    libusb_control_transfer(this->gamecubeHandle, 0x21, 11, 0x0001, 0, nullptr, 0, 1000);

    if (libusb_kernel_driver_active(this->gamecubeHandle, 0) == 1)
    {
        result = libusb_detach_kernel_driver(this->gamecubeHandle, 0);
        if (result != LIBUSB_SUCCESS)
        {
            show_status(page->statusLabel, tr("GameCube adapter access failed. Check the driver and close other apps using the adapter."));
            return false;
        }
    }

    result = libusb_claim_interface(this->gamecubeHandle, 0);
    if (result != LIBUSB_SUCCESS)
    {
        show_status(page->statusLabel, tr("GameCube adapter access failed. Check the driver and close other apps using the adapter."));
        return false;
    }
    this->gamecubeInterfaceClaimed = true;

    // Match the native plugin's endpoint discovery, including GC Pocket/RP2040 adapters.
    this->gamecubeEndpointIn = 0x81;
    this->gamecubeEndpointOut = 0x02;
    libusb_config_descriptor* config = nullptr;
    if (libusb_get_active_config_descriptor(libusb_get_device(this->gamecubeHandle), &config) == LIBUSB_SUCCESS)
    {
        for (int i = 0; i < config->bNumInterfaces; ++i)
        {
            const auto& usbInterface = config->interface[i];
            for (int alt = 0; alt < usbInterface.num_altsetting; ++alt)
            {
                const auto& descriptor = usbInterface.altsetting[alt];
                if (descriptor.bInterfaceNumber != 0 || descriptor.bAlternateSetting != 0) continue;
                for (int ep = 0; ep < descriptor.bNumEndpoints; ++ep)
                {
                    const auto& endpoint = descriptor.endpoint[ep];
                    if ((endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_INTERRUPT) continue;
                    if (endpoint.bEndpointAddress & LIBUSB_ENDPOINT_IN)
                        this->gamecubeEndpointIn = endpoint.bEndpointAddress;
                    else
                        this->gamecubeEndpointOut = endpoint.bEndpointAddress;
                }
            }
        }
        libusb_free_config_descriptor(config);
    }

    uint8_t command = kGameCubeCommandPoll;
    result = libusb_interrupt_transfer(this->gamecubeHandle, this->gamecubeEndpointOut, &command, sizeof(command), nullptr, 16);
    if (result != LIBUSB_SUCCESS)
    {
        show_status(page->statusLabel,
            tr("GameCube adapter detected, but polling failed: %1")
                .arg(QString::fromLatin1(libusb_error_name(result))));
        return false;
    }

    this->previewBackend = PreviewBackend::Gamecube;
    clear_status(page->statusLabel);
    return true;
}

bool UnifiedInputDialog::pollGamecubePreview(void)
{
    if (this->gamecubeHandle == nullptr)
    {
        return false;
    }

    const int pageIndex = this->currentPageIndex();
    ControllerPage* page = this->controllerPages[pageIndex];
    uint8_t readBuffer[37] = {};
    int transferred = 0;
    const int result = libusb_interrupt_transfer(
        this->gamecubeHandle, this->gamecubeEndpointIn, readBuffer, sizeof(readBuffer), &transferred, 16);
    if (result != LIBUSB_SUCCESS || transferred != static_cast<int>(sizeof(readBuffer)))
    {
        return false;
    }

    const int port = std::clamp(page->deviceComboBox->currentData().toInt(), 0, 3);
    const int offset = port * 9;
    GameCubeState state;
    state.status = readBuffer[offset + 1];
    state.buttons1 = readBuffer[offset + 2];
    state.buttons2 = readBuffer[offset + 3];
    state.leftStickX = readBuffer[offset + 4];
    state.leftStickY = readBuffer[offset + 5];
    state.rightStickX = readBuffer[offset + 6];
    state.rightStickY = readBuffer[offset + 7];
    state.leftTrigger = readBuffer[offset + 8];
    state.rightTrigger = readBuffer[offset + 9];

    if (!state.status)
    {
        clear_status(page->statusLabel);
        if (!this->gamecubeSelectedPortMissingController)
        {
            this->gamecubeSelectedPortMissingController = true;
            this->updateWarningLabel();
        }
        return false;
    }

    if (this->gamecubeSelectedPortMissingController)
    {
        this->gamecubeSelectedPortMissingController = false;
        this->updateWarningLabel();
    }

    ControllerPage* settingsPage = this->controllerPages[0];
    const double triggerThreshold = static_cast<double>(settingsPage->gamecubeTriggerThresholdSlider->value()) / 100.0;
    const double cStickThreshold = static_cast<double>(CoreSettingsGetIntValue(SettingsID::GCAInput_CButtonTreshold)) / 100.0;
    const bool leftTriggerAnalog = settingsPage->gamecubeLeftTriggerAnalogRadioButton->isChecked();
    const bool rightTriggerAnalog = settingsPage->gamecubeRightTriggerAnalogRadioButton->isChecked();

    if (this->listeningPageIndex == pageIndex && this->listeningBindingIndex >= 0)
    {
        const GCInput detected = detect_gamecube_input(state, triggerThreshold, cStickThreshold,
            leftTriggerAnalog, rightTriggerAnalog);
        if (detected == GCInput::None) this->listeningArmed = true;
        if (detected != GCInput::None && this->listeningArmed)
        {
            settingsPage->gamecubeBindings[this->listeningBindingIndex] = static_cast<int>(detected);
            this->manualInputChoice = true;
            const int oldPageIndex = this->listeningPageIndex;
            this->stopListeningForBinding(false);
            this->updatePageBindingButtons(oldPageIndex);
        }
    }

    for (int i = 0; i < static_cast<int>(kBindingTargets.size()); i++)
    {
        const BindingTarget& target = kBindingTargets[static_cast<size_t>(i)];
        if (!target.hasGamecubeMapping)
        {
            continue;
        }

        set_button(page->controllerImageWidget, target.imageButton,
            gc_input_active(state, static_cast<GCInput>(settingsPage->gamecubeBindings[i]), triggerThreshold, cStickThreshold,
                leftTriggerAnalog, rightTriggerAnalog));
    }

    const int8_t x = static_cast<int8_t>(state.leftStickX + 128);
    const int8_t y = static_cast<int8_t>(state.leftStickY + 128);
    const double inputX = static_cast<double>(x) / static_cast<double>(INT8_MAX);
    const double inputY = static_cast<double>(y) / static_cast<double>(INT8_MAX);
    const double deadzone = static_cast<double>(settingsPage->gamecubeDeadzoneSlider->value()) / 100.0;
    const double n64Max = kGameCubeN64AxisPeak *
        gamecube_sensitivity_percent_to_scale(settingsPage->gamecubeSensitivitySlider->value());
    const int xValue = std::clamp(scale_axis(inputX, deadzone, n64Max), -INT8_MAX, INT8_MAX);
    const int yValue = std::clamp(scale_axis(inputY, deadzone, n64Max), -INT8_MAX, INT8_MAX);

    page->controllerImageWidget->SetXAxisState(-normalize_axis_to_percent(xValue, 127));
    page->controllerImageWidget->SetYAxisState(normalize_axis_to_percent(yValue, 127));
    page->controllerImageWidget->UpdateImage();
    page->axisXLabel->setText(QString::number(xValue));
    page->axisYLabel->setText(QString::number(yValue));
    clear_status(page->statusLabel);
    return true;
}

bool UnifiedInputDialog::openUsbPreview(void)
{
    const int pageIndex = this->currentPageIndex();
    ControllerPage* page = this->controllerPages[pageIndex];
    const int deviceIndex = page->deviceComboBox->currentData().toInt();
    if (deviceIndex < 0 || deviceIndex >= static_cast<int>(this->usbDevices.size()))
    {
        show_status(page->statusLabel, tr("No SDL controller selected."));
        return false;
    }

    UsbDeviceChoice device = this->usbDevices[deviceIndex];
    if (device.type == InputDeviceType::Automatic)
    {
        const auto found = std::find_if(this->usbDevices.begin(), this->usbDevices.end(), [](const UsbDeviceChoice& candidate)
            { return candidate.connected && candidate.type == InputDeviceType::Joystick; });
        if (found != this->usbDevices.end()) device = *found;
        else device.type = InputDeviceType::Keyboard;
    }
    if (!device.connected)
    {
        show_status(page->statusLabel, tr("Selected controller is disconnected. Reconnect it or select another controller."));
        return false;
    }
    if (device.type == InputDeviceType::None)
    {
        show_status(page->statusLabel, tr("This player is disabled."));
        return false;
    }
    if (device.type == InputDeviceType::Keyboard)
    {
        this->previewBackend = PreviewBackend::USB;
        clear_status(page->statusLabel);
        return true;
    }

    if (!SDL_WasInit(SDL_INIT_GAMEPAD) && !SDL_InitSubSystem(SDL_INIT_GAMEPAD))
    {
        show_status(page->statusLabel, tr("SDL input unavailable: %1").arg(QString::fromUtf8(SDL_GetError())));
        return false;
    }

    if (device.isGamepad && SDL_IsGamepad(device.id))
    {
        this->sdlGamepad = SDL_OpenGamepad(device.id);
        if (this->sdlGamepad == nullptr)
        {
            show_status(page->statusLabel, tr("Could not open SDL gamepad: %1").arg(QString::fromUtf8(SDL_GetError())));
            return false;
        }

        this->sdlJoystick = SDL_GetGamepadJoystick(this->sdlGamepad);
    }
    else
    {
        this->sdlJoystick = SDL_OpenJoystick(device.id);
        if (this->sdlJoystick == nullptr)
        {
            show_status(page->statusLabel, tr("Could not open SDL joystick: %1").arg(QString::fromUtf8(SDL_GetError())));
            return false;
        }
    }

    this->previewBackend = PreviewBackend::USB;
    clear_status(page->statusLabel);
    return true;
}

bool UnifiedInputDialog::pollUsbPreview(void)
{
    const int pageIndex = this->currentPageIndex();
    ControllerPage* page = this->controllerPages[pageIndex];
    const int deviceIndex = page->deviceComboBox->currentData().toInt();
    if (deviceIndex < 0 || deviceIndex >= static_cast<int>(this->usbDevices.size()))
    {
        return false;
    }

    const UsbDeviceChoice& device = this->usbDevices[deviceIndex];
    if (device.type == InputDeviceType::Joystick)
    {
        if (this->sdlGamepad == nullptr && this->sdlJoystick == nullptr)
        {
            return false;
        }
        SDL_UpdateJoysticks();
    }

    auto bindingPressed = [this](const BindingValue& binding) -> bool
    {
        const int count = std::min(static_cast<int>(binding.types.size()),
            std::min(static_cast<int>(binding.data.size()), static_cast<int>(binding.extraData.size())));
        for (int i = 0; i < count; i++)
        {
            switch (static_cast<InputType>(binding.types[i]))
            {
            case InputType::Keyboard:
            {
                const auto it = this->keyboardState.find(binding.data[i]);
                if (it != this->keyboardState.end() && it->second)
                {
                    return true;
                }
                break;
            }
            case InputType::GamepadButton:
                if (this->sdlGamepad != nullptr &&
                    SDL_GetGamepadButton(this->sdlGamepad, static_cast<SDL_GamepadButton>(binding.data[i])))
                {
                    return true;
                }
                break;
            case InputType::GamepadAxis:
                if (this->sdlGamepad != nullptr)
                {
                    const int value = SDL_GetGamepadAxis(this->sdlGamepad, static_cast<SDL_GamepadAxis>(binding.data[i]));
                    if (std::abs(value) >= kAxisMotionThreshold && ((binding.extraData[i] != 0) == (value > 0)))
                    {
                        return true;
                    }
                }
                break;
            case InputType::JoystickButton:
                if (this->sdlJoystick != nullptr && SDL_GetJoystickButton(this->sdlJoystick, binding.data[i]))
                {
                    return true;
                }
                break;
            case InputType::JoystickAxis:
                if (this->sdlJoystick != nullptr)
                {
                    const int value = SDL_GetJoystickAxis(this->sdlJoystick, binding.data[i]);
                    if (std::abs(value) >= kAxisMotionThreshold && ((binding.extraData[i] != 0) == (value > 0)))
                    {
                        return true;
                    }
                }
                break;
            case InputType::JoystickHat:
                if (this->sdlJoystick != nullptr &&
                    (SDL_GetJoystickHat(this->sdlJoystick, binding.data[i]) & binding.extraData[i]) != 0)
                {
                    return true;
                }
                break;
            default:
                break;
            }
        }

        return false;
    };

    auto bindingAxis = [this](const BindingValue& binding, int direction) -> double
    {
        double axisState = 0.0;
        const int count = std::min(static_cast<int>(binding.types.size()),
            std::min(static_cast<int>(binding.data.size()), static_cast<int>(binding.extraData.size())));
        for (int i = 0; i < count; i++)
        {
            switch (static_cast<InputType>(binding.types[i]))
            {
            case InputType::Keyboard:
            {
                const auto it = this->keyboardState.find(binding.data[i]);
                if (it != this->keyboardState.end() && it->second)
                {
                    return static_cast<double>(direction);
                }
                break;
            }
            case InputType::GamepadButton:
                if (this->sdlGamepad != nullptr &&
                    SDL_GetGamepadButton(this->sdlGamepad, static_cast<SDL_GamepadButton>(binding.data[i])))
                {
                    return static_cast<double>(direction);
                }
                break;
            case InputType::JoystickButton:
                if (this->sdlJoystick != nullptr && SDL_GetJoystickButton(this->sdlJoystick, binding.data[i]))
                {
                    return static_cast<double>(direction);
                }
                break;
            case InputType::JoystickHat:
                if (this->sdlJoystick != nullptr &&
                    (SDL_GetJoystickHat(this->sdlJoystick, binding.data[i]) & binding.extraData[i]) != 0)
                {
                    return static_cast<double>(direction);
                }
                break;
            case InputType::GamepadAxis:
                if (this->sdlGamepad != nullptr)
                {
                    int value = SDL_GetGamepadAxis(this->sdlGamepad, static_cast<SDL_GamepadAxis>(binding.data[i]));
                    if (value < -SDL_AXIS_PEAK)
                    {
                        value = -SDL_AXIS_PEAK;
                    }
                    if ((binding.extraData[i] != 0) == (value > 0))
                    {
                        axisState = std::abs(static_cast<double>(value) / SDL_AXIS_PEAK) * direction;
                    }
                }
                break;
            case InputType::JoystickAxis:
                if (this->sdlJoystick != nullptr)
                {
                    int value = SDL_GetJoystickAxis(this->sdlJoystick, binding.data[i]);
                    if (value < -SDL_AXIS_PEAK)
                    {
                        value = -SDL_AXIS_PEAK;
                    }
                    if ((binding.extraData[i] != 0) == (value > 0))
                    {
                        axisState = std::abs(static_cast<double>(value) / SDL_AXIS_PEAK) * direction;
                    }
                }
                break;
            default:
                break;
            }
        }

        return axisState;
    };

    if (this->listeningPageIndex == pageIndex && this->listeningBindingIndex >= 0)
    {
        bool captured = false;
        BindingValue capturedBinding;
        if (this->sdlGamepad != nullptr)
        {
            for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT && !captured; i++)
            {
                if (SDL_GetGamepadButton(this->sdlGamepad, static_cast<SDL_GamepadButton>(i)))
                {
                    set_single_binding(capturedBinding, InputType::GamepadButton, i, 0,
                        string_from_const_char(SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(i))));
                    captured = true;
                }
            }
            for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT && !captured; i++)
            {
                const int value = SDL_GetGamepadAxis(this->sdlGamepad, static_cast<SDL_GamepadAxis>(i));
                if (std::abs(value) >= kAxisMotionThreshold)
                {
                    QString text = string_from_const_char(SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(i)));
                    text += value > 0 ? QStringLiteral("+") : QStringLiteral("-");
                    set_single_binding(capturedBinding, InputType::GamepadAxis, i, value > 0 ? 1 : 0, text);
                    captured = true;
                }
            }
        }
        else if (this->sdlJoystick != nullptr)
        {
            const int buttonCount = SDL_GetNumJoystickButtons(this->sdlJoystick);
            for (int i = 0; i < buttonCount && !captured; i++)
            {
                if (SDL_GetJoystickButton(this->sdlJoystick, i))
                {
                    set_single_binding(capturedBinding, InputType::JoystickButton, i, 0, tr("button %1").arg(i));
                    captured = true;
                }
            }

            const int axisCount = SDL_GetNumJoystickAxes(this->sdlJoystick);
            for (int i = 0; i < axisCount && !captured; i++)
            {
                const int value = SDL_GetJoystickAxis(this->sdlJoystick, i);
                if (std::abs(value) >= kAxisMotionThreshold)
                {
                    set_single_binding(capturedBinding, InputType::JoystickAxis, i, value > 0 ? 1 : 0,
                        tr("axis %1%2").arg(i).arg(value > 0 ? QStringLiteral("+") : QStringLiteral("-")));
                    captured = true;
                }
            }

            const int hatCount = SDL_GetNumJoystickHats(this->sdlJoystick);
            for (int i = 0; i < hatCount && !captured; i++)
            {
                const int value = SDL_GetJoystickHat(this->sdlJoystick, i);
                if (value != SDL_HAT_CENTERED)
                {
                    set_single_binding(capturedBinding, InputType::JoystickHat, i, value, tr("hat %1:%2").arg(i).arg(value));
                    captured = true;
                }
            }
        }

        if (!captured) this->listeningArmed = true;
        if (captured && this->listeningArmed)
        {
            page->usbDirty = true;
            page->usbBindings[this->listeningBindingIndex] = capturedBinding;
            this->manualInputChoice = true;
            const int oldPageIndex = this->listeningPageIndex;
            this->stopListeningForBinding(false);
            this->updatePageBindingButtons(oldPageIndex);
        }
    }

    for (int i = 0; i < static_cast<int>(kBindingTargets.size()); i++)
    {
        const BindingTarget& target = kBindingTargets[static_cast<size_t>(i)];
        if (target.imageButton != N64ControllerButton::Invalid)
        {
            set_button(page->controllerImageWidget, target.imageButton, bindingPressed(page->usbBindings[i]));
        }
    }

    double inputY = bindingAxis(page->usbBindings[15], 1);
    const double inputYDown = bindingAxis(page->usbBindings[16], -1);
    if (inputY != 0.0 && inputYDown != 0.0)
    {
        inputY = 0.0;
    }
    else if (inputYDown != 0.0)
    {
        inputY = inputYDown;
    }

    double inputX = bindingAxis(page->usbBindings[18], 1);
    const double inputXLeft = bindingAxis(page->usbBindings[17], -1);
    if (inputX != 0.0 && inputXLeft != 0.0)
    {
        inputX = 0.0;
    }
    else if (inputXLeft != 0.0)
    {
        inputX = inputXLeft;
    }

    const double deadzone = static_cast<double>(page->usbDeadzoneSlider->value()) / 100.0;
    const double range = page->realN64RangeCheckBox->isChecked() ? 66.0 : static_cast<double>(page->usbRangeSlider->value());
    const double n64Max = 127.0 * (range / 100.0);
    const int xValue = scale_axis(inputX, deadzone, n64Max);
    const int yValue = scale_axis(inputY, deadzone, n64Max);

    page->controllerImageWidget->SetXAxisState(n64Max > 0 ? static_cast<int>(-xValue * 100.0 / 127.0) : 0);
    page->controllerImageWidget->SetYAxisState(n64Max > 0 ? static_cast<int>(yValue * 100.0 / 127.0) : 0);
    page->controllerImageWidget->UpdateImage();
    page->axisXLabel->setText(QString::number(xValue));
    page->axisYLabel->setText(QString::number(yValue));
    return true;
}

bool UnifiedInputDialog::IsUsbModeGamecubeAdapter(uint16_t vendorId, uint16_t productId, const QString& name)
{
    // The Mayflash PC/USB identities in the bundled gamecontrollerdb can report
    // "Nintendo GameCube Controller", without "Mayflash" in either SDL name.
    if (vendorId == 0x0079 && (productId == 0x1843 || productId == 0x1844)) return true;
    if (vendorId == kGameCubeAdapterVendorId && productId == kGameCubeAdapterProductId) return false;
    const QString lowered = name.toLower();
    return lowered.contains("mayflash") && (lowered.contains("gamecube") || lowered.contains("gcn"));
}

UnifiedInputDialog::InputDetectionReport UnifiedInputDialog::ScanInputDevices(void)
{
    struct MonitorPause {
        MonitorPause() { CorePauseRaphnetMonitoring(true); }
        ~MonitorPause() { CorePauseRaphnetMonitoring(false); }
    } monitorPause;
    InputDetectionReport report;

    if (hid_init() == 0)
    {
        struct hid_device_info* devices = hid_enumerate(kRaphnetVendorId, 0);
        for (struct hid_device_info* current = devices; current != nullptr; current = current->next)
        {
            const RaphnetAdapterDef* adapter = find_raphnet_adapter(
                static_cast<uint16_t>(current->product_id), current->interface_number);
            if (adapter == nullptr)
            {
                continue;
            }

            report.foundRaphnet = true;
            report.lines.append(tr("raphnet adapter: HID %1 raw interface detected (%2 channel%3)")
                .arg(format_usb_id(kRaphnetVendorId, static_cast<uint16_t>(current->product_id)))
                .arg(adapter->rawChannels)
                .arg(adapter->rawChannels == 1 ? QString() : QStringLiteral("s")));
        }
        hid_free_enumeration(devices);
        hid_exit();
    }

    libusb_context* context = nullptr;
    int usbResult = libusb_init(&context);
    if (usbResult == LIBUSB_SUCCESS)
    {
        libusb_device** devices = nullptr;
        const ssize_t deviceCount = libusb_get_device_list(context, &devices);
        bool sawNativeGamecube = false;
        bool openedNativeGamecube = false;

        if (deviceCount >= 0)
        {
            for (ssize_t i = 0; i < deviceCount; i++)
            {
                libusb_device_descriptor descriptor = {};
                if (libusb_get_device_descriptor(devices[i], &descriptor) != LIBUSB_SUCCESS)
                {
                    continue;
                }
                if (descriptor.idVendor != kGameCubeAdapterVendorId ||
                    descriptor.idProduct != kGameCubeAdapterProductId)
                {
                    continue;
                }

                sawNativeGamecube = true;
                libusb_device_handle* handle = nullptr;
                const int openResult = libusb_open(devices[i], &handle);
                if (openResult == LIBUSB_SUCCESS)
                {
                    openedNativeGamecube = true;
                    libusb_close(handle);
                }
            }

            libusb_free_device_list(devices, 1);
        }

        if (openedNativeGamecube)
        {
            report.foundNativeGamecube = true;
            report.lines.append(tr("Native GameCube adapter: USB %1 detected and openable -> GameCube native")
                .arg(format_usb_id(kGameCubeAdapterVendorId, kGameCubeAdapterProductId)));
        }
        else if (sawNativeGamecube)
        {
            report.foundBlockedNativeGamecube = true;
            report.lines.append(tr("Native GameCube adapter: USB %1 detected, but access failed (check the driver and other apps)")
                .arg(format_usb_id(kGameCubeAdapterVendorId, kGameCubeAdapterProductId)));
        }
        else if (deviceCount >= 0)
        {
            report.lines.append(tr("Native GameCube adapter: USB %1 not detected")
                .arg(format_usb_id(kGameCubeAdapterVendorId, kGameCubeAdapterProductId)));
        }
        else
        {
            report.lines.append(tr("Native GameCube adapter scan failed: %1")
                .arg(QString::fromLatin1(libusb_error_name(static_cast<int>(deviceCount)))));
        }

        libusb_exit(context);
    }
    else
    {
        report.lines.append(tr("Native GameCube adapter scan unavailable: %1")
            .arg(QString::fromLatin1(libusb_error_name(usbResult))));
    }

    if (!SDL_WasInit(SDL_INIT_GAMEPAD) && !SDL_InitSubSystem(SDL_INIT_GAMEPAD))
    {
        report.lines.append(tr("SDL scan unavailable: %1").arg(QString::fromUtf8(SDL_GetError())));
        return report;
    }

    SDL_UpdateJoysticks();

    int joysticksCount = 0;
    SDL_JoystickID* joysticks = SDL_GetJoysticks(&joysticksCount);
    for (int i = 0; i < joysticksCount; i++)
    {
        const SDL_JoystickID joystickId = joysticks[i];
        const bool isGamepad = SDL_IsGamepad(joystickId);
        const char* deviceNamePtr = isGamepad ?
            SDL_GetGamepadNameForID(joystickId) :
            SDL_GetJoystickNameForID(joystickId);
        const uint16_t vendorId = isGamepad ?
            SDL_GetGamepadVendorForID(joystickId) :
            SDL_GetJoystickVendorForID(joystickId);
        const uint16_t productId = isGamepad ?
            SDL_GetGamepadProductForID(joystickId) :
            SDL_GetJoystickProductForID(joystickId);

        const QString deviceKind = isGamepad ? tr("SDL gamepad") : tr("SDL joystick");
        const QString deviceName = QString::fromUtf8(deviceNamePtr == nullptr ? "" : deviceNamePtr);
        const QString usbId = (vendorId == 0 && productId == 0) ?
            tr("VID:PID unknown") :
            tr("VID:PID %1").arg(format_usb_id(vendorId, productId));

        const bool usbModeGamecube = IsUsbModeGamecubeAdapter(vendorId, productId, deviceName);
        if (deviceName.isEmpty() && !usbModeGamecube)
        {
            report.lines.append(tr("%1 id %2: name unavailable [%3] -> ignored")
                .arg(deviceKind)
                .arg(static_cast<qlonglong>(joystickId))
                .arg(usbId));
            continue;
        }

        report.foundAnySdlDevice = true;

        const QString lowered = deviceName.toLower();
        QString classification;
        if (lowered.contains("raphnet"))
        {
            classification = report.foundRaphnet ? tr("Raphnet raw interface confirmed") : tr("Raphnet USB device; compatible raw interface not detected");
        }
        else if (usbModeGamecube)
        {
            report.foundUsbModeMayflash = true;
            classification = tr("Mayflash GameCube adapter in USB mode; switch to Wii U/NS (native) mode for better support");
        }
        else if (lowered.contains("gamecube") || lowered.contains("gcn") || lowered.contains("mayflash"))
        {
            classification = tr("GameCube-like SDL name; native mode not confirmed");
        }
        else
        {
            classification = tr("Other USB");
        }

        report.lines.append(tr("%1 id %2: \"%3\" [%4] -> %5")
            .arg(deviceKind)
            .arg(static_cast<qlonglong>(joystickId))
            .arg(deviceName, usbId, classification));
    }

    if (joysticks != nullptr)
    {
        SDL_free(joysticks);
    }

    if (!report.foundAnySdlDevice)
    {
        report.lines.append(tr("SDL scan: no named gamepad or joystick devices detected"));
    }

    return report;
}

UnifiedInputDialog::InputPluginType UnifiedInputDialog::DetectStartupPlugin(
    InputPluginType currentPlugin, const InputDetectionReport& report, std::optional<InputPluginType> preferredPlugin)
{
    // USB/keyboard is always available, including when no controller is plugged
    // in. Native adapter preferences require the corresponding usable hardware.
    if (preferredPlugin && (*preferredPlugin == InputPluginType::USB ||
        (*preferredPlugin == InputPluginType::Raphnet && report.foundRaphnet) ||
        (*preferredPlugin == InputPluginType::Gamecube && report.foundNativeGamecube)))
        return *preferredPlugin;

    // Prefer the last selected native adapter when it is still present, so
    // connecting both adapter types does not undo the user's choice.
    if (currentPlugin == InputPluginType::Raphnet && report.foundRaphnet) return currentPlugin;
    if (currentPlugin == InputPluginType::Gamecube && report.foundNativeGamecube) return currentPlugin;
    if (report.foundRaphnet) return InputPluginType::Raphnet;
    if (report.foundNativeGamecube) return InputPluginType::Gamecube;
    if (report.foundAnySdlDevice) return InputPluginType::USB;

    // A missing preferred adapter falls back to keyboard without forgetting it.
    return InputPluginType::USB;
}
