/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "RaphnetInputDialog.hpp"

#include <hidapi.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QPushButton>
#include <QComboBox>
#include <cstring>
#include <cstdint>

// Raphnet adapter USB vendor ID
#define RAPHNET_VID 0x289b

// raphnet HID protocol request for raw SI command
#define RQ_GCN64_RAW_SI_COMMAND 0x80
#define RQ_GCN64_SUSPEND_POLLING 0x03

// N64 controller command to read button/axis state
#define N64_GET_STATUS 0x01

// N64 button bit masks (byte 0, MSB first)
#define N64_BTN_A       0x8000
#define N64_BTN_B       0x4000
#define N64_BTN_Z       0x2000
#define N64_BTN_START   0x1000
#define N64_BTN_DUP     0x0800
#define N64_BTN_DDOWN   0x0400
#define N64_BTN_DLEFT   0x0200
#define N64_BTN_DRIGHT  0x0100
// byte 1
#define N64_BTN_L       0x0020
#define N64_BTN_R       0x0010
#define N64_BTN_CUP     0x0008
#define N64_BTN_CDOWN   0x0004
#define N64_BTN_CLEFT   0x0002
#define N64_BTN_CRIGHT  0x0001

namespace
{

struct RaphnetAdapterDef
{
    uint16_t productId;
    int interfaceNumber;
    int rawChannels;
    int reportSize;
};

// Keep this in sync with the raphnetraw plugin's supported_adapters table.
const RaphnetAdapterDef g_RaphnetAdapters[] = {
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

constexpr int64_t kSlowPollThresholdUs = 4000;
constexpr int64_t kFrameBudgetUs = 16667;

const RaphnetAdapterDef* findRaphnetAdapter(uint16_t productId, int interfaceNumber)
{
    for (const RaphnetAdapterDef& adapter : g_RaphnetAdapters)
    {
        if (adapter.productId == productId && adapter.interfaceNumber == interfaceNumber)
        {
            return &adapter;
        }
    }

    return nullptr;
}

QString formatCount(uint64_t value)
{
    return QString::number(static_cast<qulonglong>(value));
}

QString formatDuration(int64_t elapsedUs)
{
    return QString("%1 ms").arg(static_cast<double>(elapsedUs) / 1000.0, 0, 'f', 3);
}

} // namespace

using namespace UserInterface;

RaphnetInputDialog::RaphnetInputDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Raphnet Adapter - Input Test");
    setMinimumSize(460, 520);

    setupUi();

    m_PollTimer = new QTimer(this);
    connect(m_PollTimer, &QTimer::timeout, this, &RaphnetInputDialog::onPollTimer);

    if (openAdapter())
    {
        m_StatusLabel->setText("Adapter connected. Press buttons on the N64 controller.");
        m_PollTimer->start(16); // ~60Hz
    }
    else
    {
        m_StatusLabel->setText("No raphnet adapter found. Connect one and reopen this dialog.");
    }
}

RaphnetInputDialog::~RaphnetInputDialog()
{
    if (m_PollTimer->isActive())
    {
        m_PollTimer->stop();
    }
    closeAdapter();
}

void RaphnetInputDialog::setupUi()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    m_StatusLabel = new QLabel("Searching for adapter...", this);
    mainLayout->addWidget(m_StatusLabel);

    QHBoxLayout* portLayout = new QHBoxLayout();
    portLayout->addWidget(new QLabel("Port:", this));
    m_PortComboBox = new QComboBox(this);
    m_PortComboBox->addItem("1", 0);
    m_PortComboBox->setEnabled(false);
    portLayout->addWidget(m_PortComboBox);
    portLayout->addStretch();
    mainLayout->addLayout(portLayout);

    // Buttons group
    QGroupBox* buttonsGroup = new QGroupBox("Buttons", this);
    QGridLayout* buttonsGrid = new QGridLayout(buttonsGroup);

    // Define all N64 buttons with their masks
    struct ButtonDef { QString name; uint16_t mask; };
    ButtonDef buttonDefs[] = {
        { "A",       N64_BTN_A },
        { "B",       N64_BTN_B },
        { "Z",       N64_BTN_Z },
        { "Start",   N64_BTN_START },
        { "L",       N64_BTN_L },
        { "R",       N64_BTN_R },
        { "D-Up",    N64_BTN_DUP },
        { "D-Down",  N64_BTN_DDOWN },
        { "D-Left",  N64_BTN_DLEFT },
        { "D-Right", N64_BTN_DRIGHT },
        { "C-Up",    N64_BTN_CUP },
        { "C-Down",  N64_BTN_CDOWN },
        { "C-Left",  N64_BTN_CLEFT },
        { "C-Right", N64_BTN_CRIGHT },
    };

    int col = 0, row = 0;
    for (auto& def : buttonDefs)
    {
        QLabel* nameLabel = new QLabel(def.name + ":", buttonsGroup);
        QLabel* stateLabel = new QLabel(buttonsGroup);
        stateLabel->setFixedSize(16, 16);
        stateLabel->setAlignment(Qt::AlignCenter);
        stateLabel->setStyleSheet("QLabel { background-color: gray; border-radius: 3px; }");

        buttonsGrid->addWidget(nameLabel, row, col * 3);
        buttonsGrid->addWidget(stateLabel, row, col * 3 + 1);

        // Add spacer between columns
        if (col == 0)
        {
            QWidget* spacer = new QWidget(buttonsGroup);
            spacer->setFixedWidth(20);
            buttonsGrid->addWidget(spacer, row, 2);
        }

        m_ButtonIndicators.push_back({ def.name, def.mask, stateLabel });

        col++;
        if (col >= 2)
        {
            col = 0;
            row++;
        }
    }
    mainLayout->addWidget(buttonsGroup);

    // Axes group
    QGroupBox* axesGroup = new QGroupBox("Analog Stick", this);
    QGridLayout* axesGrid = new QGridLayout(axesGroup);

    axesGrid->addWidget(new QLabel("X Axis:", axesGroup), 0, 0);
    m_XAxisValue = new QLabel("0", axesGroup);
    m_XAxisValue->setFixedWidth(40);
    m_XAxisValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    axesGrid->addWidget(m_XAxisValue, 0, 1);
    m_XAxisBar = new QProgressBar(axesGroup);
    m_XAxisBar->setRange(-128, 127);
    m_XAxisBar->setValue(0);
    m_XAxisBar->setTextVisible(false);
    axesGrid->addWidget(m_XAxisBar, 0, 2);

    axesGrid->addWidget(new QLabel("Y Axis:", axesGroup), 1, 0);
    m_YAxisValue = new QLabel("0", axesGroup);
    m_YAxisValue->setFixedWidth(40);
    m_YAxisValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    axesGrid->addWidget(m_YAxisValue, 1, 1);
    m_YAxisBar = new QProgressBar(axesGroup);
    m_YAxisBar->setRange(-128, 127);
    m_YAxisBar->setValue(0);
    m_YAxisBar->setTextVisible(false);
    axesGrid->addWidget(m_YAxisBar, 1, 2);

    mainLayout->addWidget(axesGroup);

    // USB timing/debug stats
    QGroupBox* statsGroup = new QGroupBox("USB Timing", this);
    QGridLayout* statsGrid = new QGridLayout(statsGroup);

    m_StatsPollsLabel = new QLabel(statsGroup);
    m_StatsLatencyLabel = new QLabel(statsGroup);
    m_StatsSlowPollsLabel = new QLabel(statsGroup);
    m_StatsErrorsLabel = new QLabel(statsGroup);
    m_StatsResponseLabel = new QLabel(statsGroup);

    statsGrid->addWidget(new QLabel("Polls:", statsGroup), 0, 0);
    statsGrid->addWidget(m_StatsPollsLabel, 0, 1);
    statsGrid->addWidget(new QLabel("Latency:", statsGroup), 1, 0);
    statsGrid->addWidget(m_StatsLatencyLabel, 1, 1);
    statsGrid->addWidget(new QLabel("Slow:", statsGroup), 2, 0);
    statsGrid->addWidget(m_StatsSlowPollsLabel, 2, 1);
    statsGrid->addWidget(new QLabel("Errors:", statsGroup), 3, 0);
    statsGrid->addWidget(m_StatsErrorsLabel, 3, 1);
    statsGrid->addWidget(new QLabel("Last response:", statsGroup), 4, 0);
    statsGrid->addWidget(m_StatsResponseLabel, 4, 1);

    QPushButton* resetStatsButton = new QPushButton("Reset Stats", statsGroup);
    connect(resetStatsButton, &QPushButton::clicked, this, &RaphnetInputDialog::onResetStatsClicked);
    statsGrid->addWidget(resetStatsButton, 5, 1, Qt::AlignRight);

    mainLayout->addWidget(statsGroup);
    updateDebugStatsLabels();

    // Close button
    QDialogButtonBox* buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttonBox);
}

bool RaphnetInputDialog::openAdapter()
{
    hid_init();

    // Enumerate all raphnet devices and open the raw-access interface.
    struct hid_device_info* devs = hid_enumerate(RAPHNET_VID, 0);
    struct hid_device_info* cur = devs;

    while (cur)
    {
        const RaphnetAdapterDef* adapter = findRaphnetAdapter(
            static_cast<uint16_t>(cur->product_id), cur->interface_number);

        if (adapter != nullptr)
        {
            m_HidDevice = hid_open_path(cur->path);
            if (m_HidDevice)
            {
                m_ReportSize = adapter->reportSize;
                m_ChannelCount = adapter->rawChannels;
                hid_set_nonblocking(m_HidDevice, 1);
                break;
            }
        }
        cur = cur->next;
    }

    hid_free_enumeration(devs);

    if (m_HidDevice != nullptr)
    {
        m_PortComboBox->clear();
        for (int i = 0; i < m_ChannelCount; i++)
        {
            m_PortComboBox->addItem(QString::number(i + 1), i);
        }
        m_PortComboBox->setEnabled(m_ChannelCount > 1);
        setAdapterPollingSuspended(true);
    }

    return m_HidDevice != nullptr;
}

void RaphnetInputDialog::closeAdapter()
{
    if (m_HidDevice)
    {
        setAdapterPollingSuspended(false);
        hid_close(m_HidDevice);
        m_HidDevice = nullptr;
    }
    hid_exit();
}

bool RaphnetInputDialog::exchangeCommand(const unsigned char* command, int commandLength, unsigned char* response,
    int& responseLength, ExchangeTiming* timing)
{
    if (timing != nullptr)
    {
        *timing = {};
    }

    QElapsedTimer timer;
    timer.start();

    auto finish = [&](ExchangeFailure failure) {
        if (timing != nullptr)
        {
            timing->elapsedUs = timer.nsecsElapsed() / 1000;
            timing->failure = failure;
        }

        return failure == ExchangeFailure::None;
    };

    if (!m_HidDevice || !command || !response || commandLength <= 0 || commandLength > m_ReportSize)
    {
        return finish(ExchangeFailure::InvalidArgument);
    }

    unsigned char buffer[64];
    memset(buffer, 0, sizeof(buffer));
    buffer[0] = 0x00; // HID report ID
    memcpy(buffer + 1, command, static_cast<size_t>(commandLength));

    int n = hid_send_feature_report(m_HidDevice, buffer, m_ReportSize + 1);
    if (n < 0)
    {
        return finish(ExchangeFailure::SendError);
    }

    for (int attempt = 0; attempt < 8; attempt++)
    {
        if (timing != nullptr)
        {
            timing->attempts++;
        }

        memset(buffer, 0, sizeof(buffer));
        buffer[0] = 0x00;

        n = hid_get_feature_report(m_HidDevice, buffer, m_ReportSize + 1);
        if (n < 0)
        {
            return finish(ExchangeFailure::ReceiveError);
        }

        if (n <= 1)
        {
            continue;
        }

        responseLength = n - 1;
        if (timing != nullptr)
        {
            timing->responseLength = responseLength;
        }
        memcpy(response, buffer + 1, static_cast<size_t>(responseLength));

        if (response[0] == command[0])
        {
            return finish(ExchangeFailure::None);
        }
    }

    return finish(ExchangeFailure::NoResponse);
}

bool RaphnetInputDialog::setAdapterPollingSuspended(bool suspended)
{
    unsigned char command[2];
    unsigned char response[64];
    int responseLength = 0;

    command[0] = RQ_GCN64_SUSPEND_POLLING;
    command[1] = suspended ? 1 : 0;

    return exchangeCommand(command, sizeof(command), response, responseLength);
}

bool RaphnetInputDialog::pollController(uint16_t& buttons, int8_t& xAxis, int8_t& yAxis)
{
    if (!m_HidDevice)
        return false;

    // Build raw SI command: request N64 controller status
    // Protocol: [RQ_GCN64_RAW_SI_COMMAND, channel, tx_len, N64_GET_STATUS]
    unsigned char cmd[4];
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = RQ_GCN64_RAW_SI_COMMAND;
    cmd[1] = static_cast<unsigned char>(m_PortComboBox->currentData().toInt());
    cmd[2] = 0x01; // tx_len = 1 byte
    cmd[3] = N64_GET_STATUS;

    // Read response
    unsigned char response[64];
    int responseLength = 0;
    memset(response, 0, sizeof(response));
    ExchangeTiming timing;

    if (!exchangeCommand(cmd, sizeof(cmd), response, responseLength, &timing))
    {
        recordPollStats(false, timing, false);
        return false;
    }

    // Response format: [report_type, channel, rx_len, rx_data...]
    if (responseLength < 7)
    {
        recordPollStats(false, timing, true);
        return false;
    }

    unsigned char rxLen = response[2];
    if (rxLen < 4)
    {
        recordPollStats(false, timing, true);
        return false;
    }

    // Parse N64 controller status (4 bytes)
    buttons = (static_cast<uint16_t>(response[3]) << 8) | response[4];
    xAxis = static_cast<int8_t>(response[5]);
    yAxis = static_cast<int8_t>(response[6]);

    recordPollStats(true, timing, false);
    return true;
}

void RaphnetInputDialog::onPollTimer()
{
    uint16_t buttons = 0;
    int8_t xAxis = 0, yAxis = 0;

    if (pollController(buttons, xAxis, yAxis))
    {
        m_FailedPollCount = 0;
        m_StatusLabel->setText("Adapter connected. Press buttons on the N64 controller.");
        updateButtonIndicators(buttons);
        updateAxisDisplay(xAxis, yAxis);
    }
    else
    {
        m_FailedPollCount++;
        if (m_FailedPollCount >= 30)
        {
            m_StatusLabel->setText("Adapter connected, but the selected port did not answer.");
        }
    }
}

void RaphnetInputDialog::onResetStatsClicked()
{
    m_DebugStats = {};
    m_FailedPollCount = 0;
    updateDebugStatsLabels();
}

void RaphnetInputDialog::updateButtonIndicators(uint16_t buttons)
{
    for (auto& indicator : m_ButtonIndicators)
    {
        bool pressed = (buttons & indicator.mask) != 0;
        indicator.stateLabel->setStyleSheet(pressed
            ? "QLabel { background-color: #4CAF50; border-radius: 3px; }"
            : "QLabel { background-color: gray; border-radius: 3px; }");
    }
}

void RaphnetInputDialog::updateAxisDisplay(int8_t x, int8_t y)
{
    m_XAxisValue->setText(QString::number(x));
    m_YAxisValue->setText(QString::number(y));
    m_XAxisBar->setValue(x);
    m_YAxisBar->setValue(y);
}

void RaphnetInputDialog::recordPollStats(bool success, const ExchangeTiming& timing, bool invalidResponse)
{
    m_DebugStats.pollCount++;
    m_DebugStats.totalExchangeUs += static_cast<uint64_t>(timing.elapsedUs);
    m_DebugStats.lastExchangeUs = timing.elapsedUs;
    if (timing.elapsedUs > m_DebugStats.maxExchangeUs)
    {
        m_DebugStats.maxExchangeUs = timing.elapsedUs;
    }

    m_DebugStats.lastAttempts = timing.attempts;
    if (timing.attempts > m_DebugStats.maxAttempts)
    {
        m_DebugStats.maxAttempts = timing.attempts;
    }
    m_DebugStats.lastResponseLength = timing.responseLength;

    if (timing.elapsedUs >= kSlowPollThresholdUs)
    {
        m_DebugStats.slowPollCount++;
    }
    if (timing.elapsedUs >= kFrameBudgetUs)
    {
        m_DebugStats.frameBudgetMissCount++;
    }

    if (success)
    {
        m_DebugStats.successCount++;
    }
    else
    {
        m_DebugStats.failureCount++;
    }

    if (invalidResponse)
    {
        m_DebugStats.invalidResponseCount++;
    }

    switch (timing.failure)
    {
    case ExchangeFailure::SendError:
        m_DebugStats.sendErrorCount++;
        break;
    case ExchangeFailure::ReceiveError:
        m_DebugStats.receiveErrorCount++;
        break;
    case ExchangeFailure::NoResponse:
        m_DebugStats.noResponseCount++;
        break;
    default:
        break;
    }

    updateDebugStatsLabels();
}

void RaphnetInputDialog::updateDebugStatsLabels()
{
    if (m_StatsPollsLabel == nullptr)
    {
        return;
    }

    const int64_t averageUs = m_DebugStats.pollCount > 0 ?
        static_cast<int64_t>(m_DebugStats.totalExchangeUs / m_DebugStats.pollCount) : 0;

    m_StatsPollsLabel->setText(QString("%1 ok / %2 failed / %3 total")
        .arg(formatCount(m_DebugStats.successCount),
             formatCount(m_DebugStats.failureCount),
             formatCount(m_DebugStats.pollCount)));
    m_StatsLatencyLabel->setText(QString("last %1 / avg %2 / max %3")
        .arg(formatDuration(m_DebugStats.lastExchangeUs),
             formatDuration(averageUs),
             formatDuration(m_DebugStats.maxExchangeUs)));
    m_StatsSlowPollsLabel->setText(QString(">=4 ms: %1 / >=16.7 ms: %2")
        .arg(formatCount(m_DebugStats.slowPollCount),
             formatCount(m_DebugStats.frameBudgetMissCount)));
    m_StatsErrorsLabel->setText(QString("send %1 / read %2 / no response %3 / malformed %4")
        .arg(formatCount(m_DebugStats.sendErrorCount),
             formatCount(m_DebugStats.receiveErrorCount),
             formatCount(m_DebugStats.noResponseCount),
             formatCount(m_DebugStats.invalidResponseCount)));
    m_StatsResponseLabel->setText(QString("attempts %1 (max %2) / bytes %3")
        .arg(m_DebugStats.lastAttempts)
        .arg(m_DebugStats.maxAttempts)
        .arg(m_DebugStats.lastResponseLength));
}
