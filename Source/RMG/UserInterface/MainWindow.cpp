/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "MainWindow.hpp"

#include "UserInterface/Dialog/AboutDialog.hpp"
#include "Dialog/Cheats/CheatsDialog.hpp"
#include "Dialog/SettingsDialog.hpp"
#include "Dialog/RomInfoDialog.hpp"
#ifdef UPDATER
#include "UserInterface/Dialog/Update/DownloadUpdateDialog.hpp"
#include "UserInterface/Dialog/Update/InstallUpdateDialog.hpp"
#include "UserInterface/Dialog/Update/UpdateDialog.hpp"
#endif // UPDATER
#ifdef NETPLAY
#include "Dialog/Netplay/NetplaySessionBrowserDialog.hpp"
#include "Dialog/Netplay/CreateNetplaySessionDialog.hpp"
#include "Dialog/Netplay/NetplaySessionDialog.hpp"
#include "KailleraUIBridge.hpp"
#include "Dialog/Kaillera/KailleraPlaybackDialog.hpp"
#include "n02_client.h"
#include "kailleraclient.h"
#endif // NETPLAY
#include "Dialog/RaphnetInputDialog.hpp"
#include "UserInterface/EventFilter.hpp"
#include "Utilities/QtKeyToSdl3Key.hpp"
#include "Utilities/QtMessageBox.hpp"
#include "OnScreenDisplay.hpp"
#include "Callbacks.hpp"
#include "VidExt.hpp"

#ifdef UPDATER
#include <QNetworkAccessManager>
#include <QJsonDocument>
#include <QJsonObject>
#endif // UPDATER
#ifdef NETPLAY
#include <QWebSocket>
#endif // NETPLAY
#include <QApplication>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QStyleFactory>
#include <QActionGroup>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QMimeData>
#include <QSettings>
#include <QStatusBar>
#include <QMenuBar>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QShowEvent>
#include <QDir>
#include <QUrl>
#include <QRegularExpression>
#include <QScreen>

#ifdef KCA_DRAG_DROP
#include <KUrlMimeData>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#include <QPainter>
#include <QProxyStyle>

#include <cstdlib>
#include <string>

#ifdef _WIN32
class NoAccentProxyStyle final : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option,
        QPainter* painter, const QWidget* widget) const override
    {
        if (element == PE_PanelItemViewItem)
        {
            if (const auto* vopt = qstyleoption_cast<const QStyleOptionViewItem*>(option))
            {
                // Paint alternating row background
                if (vopt->features & QStyleOptionViewItem::Alternate)
                {
                    painter->fillRect(vopt->rect, vopt->palette.alternateBase());
                }

                if (vopt->state & State_Selected)
                {
                    QColor sel = vopt->palette.highlight().color();
                    sel.setAlpha(80);
                    painter->fillRect(vopt->rect, sel);
                }
                else if (vopt->state & State_MouseOver)
                {
                    QColor hover = vopt->palette.highlight().color();
                    hover.setAlpha(30);
                    painter->fillRect(vopt->rect, hover);
                }
                return;
            }
        }
        if (element == PE_FrameFocusRect || element == PE_PanelItemViewRow)
        {
            return;
        }
        // Suppress the raised panel/border on toolbar buttons when not hovered/pressed
        if (element == PE_PanelButtonTool)
        {
            if (!(option->state & (State_MouseOver | State_Sunken)))
            {
                return;
            }
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

    void drawControl(ControlElement element, const QStyleOption* option,
        QPainter* painter, const QWidget* widget) const override
    {
        if (element == CE_ItemViewItem)
        {
            // Strip focus only  Ekeep Selected so text color changes correctly
            QStyleOptionViewItem opt(*qstyleoption_cast<const QStyleOptionViewItem*>(option));
            opt.state &= ~State_HasFocus;
            QProxyStyle::drawControl(element, &opt, painter, widget);
            return;
        }
        QProxyStyle::drawControl(element, option, painter, widget);
    }
};
#endif
#include <chrono>
#include <cmath>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <vector>

#include <RMG-Core/CachedRomHeaderAndSettings.hpp>
#include <RMG-Core/SpeedLimiter.hpp>
#include <RMG-Core/Directories.hpp>
#include <RMG-Core/SpeedFactor.hpp>
#include <RMG-Core/Screenshot.hpp>
#include <RMG-Core/Emulation.hpp>
#include <RMG-Core/RollbackNetcode.hpp>
#include <RMG-Core/rmgk_gekko.hpp>
#include <RMG-Core/SaveState.hpp>
#include <RMG-Core/Settings.hpp>
#include <RMG-Core/Plugins.hpp>
#include <RMG-Core/Raphnet.hpp>
#include <RMG-Core/Netplay.hpp>
#include <RMG-Core/Kaillera.hpp>
#include <RMG-Core/Version.hpp>
#include <RMG-Core/Cheats.hpp>
#include <RMG-Core/Volume.hpp>
#include <RMG-Core/Error.hpp>
#include <RMG-Core/Video.hpp>
#include <RMG-Core/Core.hpp>
#include <RMG-Core/Key.hpp>

using namespace UserInterface;
using namespace Utilities;

static bool isRaphnetRawPlugin()
{
    std::string pluginName = CoreSettingsGetStringValue(SettingsID::Core_INPUT_Plugin);
    return pluginName.find("raphnetraw") != std::string::npos;
}

namespace
{
constexpr int kRollbackDebugReplayFrames = 1200;
constexpr int kRollbackDebugReplayPlayers = 4;
constexpr int kRollbackDebugStressInterval = 5;
constexpr int kRollbackDebugStressRollbackFrames = 2;
constexpr const char* kRollbackDebugReplayFilePath = "rollback_sanity_test.replay";
constexpr const char* kRollbackDebugReplayLogFileName = "rollback_sanity_test.log";
constexpr uint32_t kRollbackDebugReplayMagic = 0x52534452;
constexpr uint32_t kRollbackDebugReplayVersion = 9;

enum class RollbackDebugReplayMode
{
    Idle,
    Recording,
    Verifying,
    Stressing
};

struct RollbackDebugReplayState
{
    std::mutex mutex;
    RollbackDebugReplayMode mode = RollbackDebugReplayMode::Idle;
    CoreRollbackState initialState;
    CoreRollbackState finalState;
    std::array<CoreRollbackState, kRollbackDebugStressRollbackFrames + 1> stressCheckpoints;
    std::vector<std::array<uint32_t, kRollbackDebugReplayPlayers>> inputs;
    std::vector<uint64_t> inputHashes;
    std::vector<uint64_t> frameHashes;
    size_t verifyInputIndex = 0;
    size_t verifyFrameIndex = 0;
    int firstInputMismatchFrame = -1;
    uint64_t firstInputMismatchExpectedHash = 0;
    uint64_t firstInputMismatchActualHash = 0;
    int firstMismatchFrame = -1;
    uint64_t firstMismatchExpectedHash = 0;
    uint64_t firstMismatchActualHash = 0;
    uint64_t recordedInputHash = 0;
    uint64_t replayedInputHash = 0;
    uint64_t finalHash = 0;
    bool frameAdvancedThisFrame = false;
    bool pendingInitialSave = false;
    bool pendingInitialLoad = false;
    bool countReplayInputHash = true;
    bool verifyWithGraphics = false;
    bool lastVerifyCompleted = false;
    bool lastVerifyMatched = false;
    uint64_t lastVerifyExpectedHash = 0;
    uint64_t lastVerifyActualHash = 0;
    size_t lastVerifyRecordedInputFrames = 0;
    size_t lastVerifyReplayedInputFrames = 0;
    int stressRollbackCount = 0;
    int stressRollbackFrames = 0;
    int stressResimulatedFrames = 0;
    bool ready = false;
};

RollbackDebugReplayState g_RollbackDebugReplay;

bool RollbackDebugReplayBeginFrame(void* userData);
bool RollbackDebugReplayInputProvider(uint32_t* inputs, int players, void* userData);
bool RollbackDebugReplayEndFrame(void* userData);
bool RollbackDebugRecordFrameHashFromRollbackFrame();
bool RollbackDebugFinishRecordingFromRollbackFrame();
bool RollbackDebugVerifyFromRollbackFrame();
bool RollbackDebugRunStressRollbackFromRollbackFrame(size_t frame);
void FreeRollbackDebugStressCheckpoints();
bool SaveRollbackDebugStressCheckpoint(size_t frame);
void WriteRollbackDebugReplayLog(const std::string& phase, bool matched, uint64_t expectedHash,
    uint64_t actualHash, size_t recordedInputFrames, size_t replayedInputFrames,
    const CoreRollbackState& expectedState, const CoreRollbackState& actualState);
void WriteRollbackDebugReplayEventLog(const std::string& phase, const std::string& message);
std::string FormatRollbackRunFrameStats(const CoreRollbackRunFrameStats& stats);
bool RollbackDebugShouldIgnoreStopFailure(const std::string& error);

bool RollbackDebugEnsureStableFrameControl()
{
    if (!CoreIsEmulationRunning())
    {
        return false;
    }
    if (CoreIsEmulationPaused())
    {
        return CoreResumeEmulation();
    }
    return true;
}

void RollbackDebugCloseSession()
{
    rmgk_gekko::set_debug_hooks(nullptr, nullptr, nullptr, nullptr);
    rmgk_gekko::set_debug_frame_output(-1);
    CoreSetFrameOutput(CoreFrameOutput_All);
    FreeRollbackDebugStressCheckpoints();
    std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
    g_RollbackDebugReplay.frameAdvancedThisFrame = false;
    g_RollbackDebugReplay.pendingInitialSave = false;
    g_RollbackDebugReplay.pendingInitialLoad = false;
    g_RollbackDebugReplay.stressRollbackCount = 0;
    g_RollbackDebugReplay.stressRollbackFrames = 0;
    g_RollbackDebugReplay.stressResimulatedFrames = 0;
}

std::string FormatRollbackRunFrameStats(const CoreRollbackRunFrameStats& stats)
{
    std::ostringstream stream;
    stream << " core_total_us=" << stats.totalUs
           << " r4300_us=" << stats.r4300Us
           << " vi_us=" << stats.viUs
           << " new_frame_us=" << stats.newFrameUs
           << " cheats_us=" << stats.cheatsUs
           << " pacing_us=" << stats.pacingUs
           << " input_us=" << stats.inputUs
           << " pause_us=" << stats.pauseUs
           << " netplay_us=" << stats.netplayUs
           << " dynarec_recompiles=" << stats.dynarecRecompileCount
           << " dynarec_recompile_us=" << stats.dynarecRecompileUs
           << " dynarec_invalidate_us=" << stats.dynarecInvalidateUs
           << " dynarec_full_invalidates=" << stats.dynarecFullInvalidateCount
           << " dynarec_range_invalidates=" << stats.dynarecRangeInvalidateCount
           << " dynarec_block_invalidates=" << stats.dynarecBlockInvalidateCount
           << " dynarec_verify_dirty_count=" << stats.dynarecVerifyDirtyCount
           << " dynarec_verify_dirty_us=" << stats.dynarecVerifyDirtyUs
           << " dynarec_get_addr_count=" << stats.dynarecGetAddrCount
           << " dynarec_get_addr_us=" << stats.dynarecGetAddrUs
           << " dynarec_get_addr_ht_count=" << stats.dynarecGetAddrHtCount
           << " dynarec_get_addr_32_count=" << stats.dynarecGetAddr32Count
           << " dynarec_dynamic_linker_count=" << stats.dynarecDynamicLinkerCount
           << " dynarec_dynamic_linker_us=" << stats.dynarecDynamicLinkerUs
           << " dynarec_dynamic_linker_ds_count=" << stats.dynarecDynamicLinkerDsCount
           << " dynarec_dynamic_linker_ds_us=" << stats.dynarecDynamicLinkerDsUs
           << " cached_code_full_invalidates=" << stats.cachedCodeFullInvalidateCount
           << " cached_code_range_invalidates=" << stats.cachedCodeRangeInvalidateCount
           << " interrupt_count=" << stats.interruptCount
           << " interrupt_us=" << stats.interruptUs
           << " interrupt_max_us=" << stats.interruptMaxUs
           << " interrupt_max_type=" << stats.interruptMaxType
           << " interrupt_vi_count=" << stats.interruptViCount
           << " interrupt_vi_us=" << stats.interruptViUs
           << " interrupt_compare_count=" << stats.interruptCompareCount
           << " interrupt_compare_us=" << stats.interruptCompareUs
           << " interrupt_check_count=" << stats.interruptCheckCount
           << " interrupt_check_us=" << stats.interruptCheckUs
           << " interrupt_si_count=" << stats.interruptSiCount
           << " interrupt_si_us=" << stats.interruptSiUs
           << " interrupt_pi_count=" << stats.interruptPiCount
           << " interrupt_pi_us=" << stats.interruptPiUs
           << " interrupt_ai_count=" << stats.interruptAiCount
           << " interrupt_ai_us=" << stats.interruptAiUs
           << " interrupt_sp_count=" << stats.interruptSpCount
           << " interrupt_sp_us=" << stats.interruptSpUs
           << " interrupt_dp_count=" << stats.interruptDpCount
           << " interrupt_dp_us=" << stats.interruptDpUs
           << " interrupt_rsp_dma_count=" << stats.interruptRspDmaCount
           << " interrupt_rsp_dma_us=" << stats.interruptRspDmaUs
           << " interrupt_rsp_task_count=" << stats.interruptRspTaskCount
           << " interrupt_rsp_task_us=" << stats.interruptRspTaskUs
           << " ai_set_frequency_count=" << stats.aiSetFrequencyCount
           << " ai_set_frequency_us=" << stats.aiSetFrequencyUs
           << " ai_push_samples_count=" << stats.aiPushSamplesCount
           << " ai_push_samples_us=" << stats.aiPushSamplesUs
           << " ai_fifo_pop_count=" << stats.aiFifoPopCount
           << " ai_fifo_pop_us=" << stats.aiFifoPopUs
           << " ai_raise_interrupt_count=" << stats.aiRaiseInterruptCount
           << " ai_raise_interrupt_us=" << stats.aiRaiseInterruptUs
           << " emumode=" << stats.emumode
           << " cp0_count_before=" << stats.cp0CountBefore
           << " cp0_count_after=" << stats.cp0CountAfter
           << " cp0_count_delta=" << (stats.cp0CountAfter - stats.cp0CountBefore)
           << " next_interrupt_before=" << stats.nextInterruptBefore
           << " next_interrupt_after=" << stats.nextInterruptAfter
           << " current_frame_before=" << stats.currentFrameBefore
           << " current_frame_after=" << stats.currentFrameAfter
           << " load_before_cp0_count=" << stats.loadBeforeCp0Count
           << " load_before_next_interrupt=" << stats.loadBeforeNextInterrupt
           << " load_before_current_frame=" << stats.loadBeforeCurrentFrame
           << " load_before_cycle_count=" << stats.loadBeforeCycleCount
           << " load_before_pending_exception=" << stats.loadBeforePendingException
           << " load_before_stop=" << stats.loadBeforeStop
           << " load_probe_cp0_count=" << stats.loadProbeCp0Count
           << " load_probe_next_interrupt=" << stats.loadProbeNextInterrupt
           << " load_probe_current_frame=" << stats.loadProbeCurrentFrame
           << " load_probe_cycle_count=" << stats.loadProbeCycleCount
           << " load_probe_pending_exception=" << stats.loadProbePendingException
           << " load_probe_stop=" << stats.loadProbeStop
           << " hidden_begin_cp0_count=" << stats.hiddenBeginCp0Count
           << " hidden_begin_next_interrupt=" << stats.hiddenBeginNextInterrupt
           << " hidden_begin_current_frame=" << stats.hiddenBeginCurrentFrame
           << " hidden_begin_cycle_count=" << stats.hiddenBeginCycleCount
           << " hidden_begin_pending_exception=" << stats.hiddenBeginPendingException
           << " hidden_begin_stop=" << stats.hiddenBeginStop
           << " resume_probe_cp0_count=" << stats.resumeProbeCp0Count
           << " resume_probe_next_interrupt=" << stats.resumeProbeNextInterrupt
           << " resume_probe_current_frame=" << stats.resumeProbeCurrentFrame
           << " resume_probe_cycle_count=" << stats.resumeProbeCycleCount
           << " resume_probe_pending_exception=" << stats.resumeProbePendingException
           << " resume_probe_stop=" << stats.resumeProbeStop
           << " dynarec_cycle_count_before=" << stats.dynarecCycleCountBefore
           << " dynarec_cycle_count_after=" << stats.dynarecCycleCountAfter
           << " dynarec_pending_exception_before=" << stats.dynarecPendingExceptionBefore
           << " dynarec_pending_exception_after=" << stats.dynarecPendingExceptionAfter
           << " dynarec_stop_before=" << stats.dynarecStopBefore
           << " dynarec_stop_after=" << stats.dynarecStopAfter
           << " delay_slot_before=" << stats.delaySlotBefore
           << " delay_slot_after=" << stats.delaySlotAfter
           << " output_flags=" << stats.outputFlags
           << std::hex << std::setfill('0')
           << " pc_before=0x" << std::setw(8) << stats.pcBefore
           << " pc_after=0x" << std::setw(8) << stats.pcAfter
           << " load_before_pc=0x" << std::setw(8) << stats.loadBeforePc
           << " load_probe_pc=0x" << std::setw(8) << stats.loadProbePc
           << " hidden_begin_pc=0x" << std::setw(8) << stats.hiddenBeginPc
           << " resume_probe_pc=0x" << std::setw(8) << stats.resumeProbePc
           << " dynarec_pcaddr_before=0x" << std::setw(8) << stats.dynarecPcaddrBefore
           << " dynarec_pcaddr_after=0x" << std::setw(8) << stats.dynarecPcaddrAfter
           << " cp0_last_addr_before=0x" << std::setw(8) << stats.cp0LastAddrBefore
           << " cp0_last_addr_after=0x" << std::setw(8) << stats.cp0LastAddrAfter
           << std::dec;
    return stream.str();
}

bool RollbackDebugShouldIgnoreStopFailure(const std::string& error)
{
    return !CoreIsEmulationRunning() ||
        error.find("INVALID_STATE") != std::string::npos ||
        error.find("stop") != std::string::npos ||
        error.find("Stop") != std::string::npos;
}

bool RollbackDebugIsPlaybackMode(RollbackDebugReplayMode mode)
{
    return mode == RollbackDebugReplayMode::Verifying || mode == RollbackDebugReplayMode::Stressing;
}

size_t RollbackDebugStressCheckpointIndex(size_t frame)
{
    return frame % g_RollbackDebugReplay.stressCheckpoints.size();
}

void FreeRollbackDebugStressCheckpoints()
{
    for (auto& checkpoint : g_RollbackDebugReplay.stressCheckpoints)
    {
        CoreRollbackFreeGameState(checkpoint);
    }
}

bool SaveRollbackDebugStressCheckpoint(size_t frame)
{
    CoreRollbackState& checkpoint =
        g_RollbackDebugReplay.stressCheckpoints[RollbackDebugStressCheckpointIndex(frame)];
    CoreRollbackFreeGameState(checkpoint);
    const auto beginTime = std::chrono::steady_clock::now();
    const bool result = CoreRollbackSaveGameState(checkpoint, CoreGetCurrentFrameCount());
    const auto elapsedUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - beginTime).count();
    WriteRollbackDebugReplayEventLog(result ? "stress_checkpoint_save" : "stress_checkpoint_save_failed",
        "frame=" + std::to_string(frame) +
        " len=" + std::to_string(checkpoint.len) +
        " elapsed_us=" + std::to_string(elapsedUs) +
        (result ? "" : (" error=" + CoreGetError())));
    return result;
}

bool WriteReplayBytes(std::ofstream& file, const void* data, size_t size)
{
    file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return file.good();
}

bool ReadReplayBytes(std::ifstream& file, void* data, size_t size)
{
    file.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    return file.good();
}

uint64_t HashRollbackDebugReplayInputFrame(const std::array<uint32_t, kRollbackDebugReplayPlayers>& inputs)
{
    constexpr uint64_t OffsetBasis = 14695981039346656037ull;
    constexpr uint64_t Prime = 1099511628211ull;
    uint64_t hash = OffsetBasis;

    for (uint32_t input : inputs)
    {
        for (int byte = 0; byte < 4; byte++)
        {
            hash ^= static_cast<unsigned char>((input >> (byte * 8)) & 0xff);
            hash *= Prime;
        }
    }

    return hash;
}

uint64_t AppendRollbackDebugReplayHash(uint64_t hash, uint64_t value)
{
    constexpr uint64_t Prime = 1099511628211ull;

    if (hash == 0)
    {
        hash = 14695981039346656037ull;
    }

    for (int byte = 0; byte < 8; byte++)
    {
        hash ^= static_cast<unsigned char>((value >> (byte * 8)) & 0xff);
        hash *= Prime;
    }

    return hash;
}

bool SaveRollbackDebugReplayFile(std::string& error, const CoreRollbackState& finalState)
{
    std::ofstream file(kRollbackDebugReplayFilePath, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        error = "could not open rollback_sanity_test.replay for writing";
        return false;
    }

    std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
    if (g_RollbackDebugReplay.initialState.buffer == nullptr || g_RollbackDebugReplay.initialState.len <= 0)
    {
        error = "debug replay has no initial rollback state";
        return false;
    }
    if (finalState.buffer == nullptr || finalState.len <= 0)
    {
        error = "debug replay has no final rollback state";
        return false;
    }

    const uint32_t magic = kRollbackDebugReplayMagic;
    const uint32_t version = kRollbackDebugReplayVersion;
    const int32_t frames = kRollbackDebugReplayFrames;
    const int32_t players = kRollbackDebugReplayPlayers;
    const int32_t stateLen = g_RollbackDebugReplay.initialState.len;
    const int32_t stateChecksum = g_RollbackDebugReplay.initialState.checksum;
    const int32_t stateFrame = g_RollbackDebugReplay.initialState.frame;
    const int32_t finalStateLen = finalState.len;
    const int32_t finalStateChecksum = finalState.checksum;
    const int32_t finalStateFrame = finalState.frame;
    const int32_t inputCount = static_cast<int32_t>(g_RollbackDebugReplay.inputs.size());
    const int32_t inputHashCount = static_cast<int32_t>(g_RollbackDebugReplay.inputHashes.size());
    const int32_t frameHashCount = static_cast<int32_t>(g_RollbackDebugReplay.frameHashes.size());
    const uint64_t recordedInputHash = g_RollbackDebugReplay.recordedInputHash;
    const uint64_t finalHash = g_RollbackDebugReplay.finalHash;

    if (!WriteReplayBytes(file, &magic, sizeof(magic)) ||
        !WriteReplayBytes(file, &version, sizeof(version)) ||
        !WriteReplayBytes(file, &frames, sizeof(frames)) ||
        !WriteReplayBytes(file, &players, sizeof(players)) ||
        !WriteReplayBytes(file, &stateLen, sizeof(stateLen)) ||
        !WriteReplayBytes(file, &stateChecksum, sizeof(stateChecksum)) ||
        !WriteReplayBytes(file, &stateFrame, sizeof(stateFrame)) ||
        !WriteReplayBytes(file, &finalStateLen, sizeof(finalStateLen)) ||
        !WriteReplayBytes(file, &finalStateChecksum, sizeof(finalStateChecksum)) ||
        !WriteReplayBytes(file, &finalStateFrame, sizeof(finalStateFrame)) ||
        !WriteReplayBytes(file, &inputCount, sizeof(inputCount)) ||
        !WriteReplayBytes(file, &inputHashCount, sizeof(inputHashCount)) ||
        !WriteReplayBytes(file, &frameHashCount, sizeof(frameHashCount)) ||
        !WriteReplayBytes(file, &recordedInputHash, sizeof(recordedInputHash)) ||
        !WriteReplayBytes(file, &finalHash, sizeof(finalHash)) ||
        !WriteReplayBytes(file, g_RollbackDebugReplay.initialState.buffer, static_cast<size_t>(stateLen)) ||
        !WriteReplayBytes(file, finalState.buffer, static_cast<size_t>(finalStateLen)))
    {
        error = "could not write rollback_sanity_test.replay header/state";
        return false;
    }

    for (const auto& input : g_RollbackDebugReplay.inputs)
    {
        if (!WriteReplayBytes(file, input.data(), input.size() * sizeof(input[0])))
        {
            error = "could not write rollback_sanity_test.replay inputs";
            return false;
        }
    }

    if (!g_RollbackDebugReplay.inputHashes.empty() &&
        !WriteReplayBytes(file, g_RollbackDebugReplay.inputHashes.data(),
            g_RollbackDebugReplay.inputHashes.size() * sizeof(g_RollbackDebugReplay.inputHashes[0])))
    {
        error = "could not write rollback_sanity_test.replay input hashes";
        return false;
    }

    if (!g_RollbackDebugReplay.frameHashes.empty() &&
        !WriteReplayBytes(file, g_RollbackDebugReplay.frameHashes.data(),
            g_RollbackDebugReplay.frameHashes.size() * sizeof(g_RollbackDebugReplay.frameHashes[0])))
    {
        error = "could not write rollback_sanity_test.replay frame hashes";
        return false;
    }

    return true;
}

std::filesystem::path GetRollbackDebugLogDirectory()
{
    std::error_code errorCode;
    std::filesystem::path directory = CoreGetLibraryDirectory() / "Logs";

    if (std::filesystem::is_directory(directory, errorCode) ||
        std::filesystem::create_directories(directory, errorCode))
    {
        return directory.make_preferred();
    }

    errorCode.clear();
    directory = "Logs";
    if (std::filesystem::is_directory(directory, errorCode) ||
        std::filesystem::create_directories(directory, errorCode))
    {
        return directory.make_preferred();
    }

    return std::filesystem::path();
}

std::filesystem::path GetRollbackDebugReplayLogPath()
{
    const std::filesystem::path directory = GetRollbackDebugLogDirectory();
    if (!directory.empty())
    {
        return directory / kRollbackDebugReplayLogFileName;
    }

    return std::filesystem::path(kRollbackDebugReplayLogFileName);
}

std::string GetRollbackDebugReplayLogPathString()
{
    return GetRollbackDebugReplayLogPath().string();
}

std::string GetRollbackDebugReplayPayloadRegion(int payloadOffset)
{
    constexpr int RdramPayloadOffset = 440;
    constexpr int RdramSize = 8 * 1024 * 1024;

    if (payloadOffset < 0)
    {
        return "none";
    }

    if (payloadOffset >= RdramPayloadOffset && payloadOffset < RdramPayloadOffset + RdramSize)
    {
        return "rdram+0x" + QString::number(payloadOffset - RdramPayloadOffset, 16).toStdString();
    }

    if (payloadOffset < RdramPayloadOffset)
    {
        return "core-registers";
    }

    return "post-rdram-state";
}

bool LoadRollbackDebugReplayFile(std::string& error)
{
    std::ifstream file(kRollbackDebugReplayFilePath, std::ios::binary);
    if (!file.is_open())
    {
        error = "could not open rollback_sanity_test.replay for reading";
        return false;
    }

    uint32_t magic = 0;
    uint32_t version = 0;
    int32_t frames = 0;
    int32_t players = 0;
    int32_t stateLen = 0;
    int32_t stateChecksum = 0;
    int32_t stateFrame = 0;
    int32_t finalStateLen = 0;
    int32_t finalStateChecksum = 0;
    int32_t finalStateFrame = 0;
    int32_t inputCount = 0;
    int32_t inputHashCount = 0;
    int32_t frameHashCount = 0;
    uint64_t recordedInputHash = 0;
    uint64_t finalHash = 0;

    if (!ReadReplayBytes(file, &magic, sizeof(magic)) ||
        !ReadReplayBytes(file, &version, sizeof(version)) ||
        !ReadReplayBytes(file, &frames, sizeof(frames)) ||
        !ReadReplayBytes(file, &players, sizeof(players)) ||
        !ReadReplayBytes(file, &stateLen, sizeof(stateLen)) ||
        !ReadReplayBytes(file, &stateChecksum, sizeof(stateChecksum)) ||
        !ReadReplayBytes(file, &stateFrame, sizeof(stateFrame)) ||
        !ReadReplayBytes(file, &finalStateLen, sizeof(finalStateLen)) ||
        !ReadReplayBytes(file, &finalStateChecksum, sizeof(finalStateChecksum)) ||
        !ReadReplayBytes(file, &finalStateFrame, sizeof(finalStateFrame)) ||
        !ReadReplayBytes(file, &inputCount, sizeof(inputCount)) ||
        !ReadReplayBytes(file, &inputHashCount, sizeof(inputHashCount)) ||
        !ReadReplayBytes(file, &frameHashCount, sizeof(frameHashCount)) ||
        !ReadReplayBytes(file, &recordedInputHash, sizeof(recordedInputHash)) ||
        !ReadReplayBytes(file, &finalHash, sizeof(finalHash)))
    {
        error = "could not read rollback_sanity_test.replay header";
        return false;
    }

    if (magic != kRollbackDebugReplayMagic || version != kRollbackDebugReplayVersion ||
        frames != kRollbackDebugReplayFrames || players != kRollbackDebugReplayPlayers ||
        stateLen <= 0 || finalStateLen <= 0 || inputCount < 0 || inputCount > kRollbackDebugReplayFrames ||
        inputHashCount < 0 || inputHashCount > kRollbackDebugReplayFrames ||
        frameHashCount < 0 || frameHashCount > kRollbackDebugReplayFrames)
    {
        error = "rollback_sanity_test.replay has an invalid or stale format; record it again";
        return false;
    }

    CoreRollbackState replayState;
    CoreRollbackState replayFinalState;
    replayState.buffer = static_cast<unsigned char*>(std::malloc(static_cast<size_t>(stateLen)));
    replayState.len = stateLen;
    replayState.checksum = stateChecksum;
    replayState.frame = stateFrame;
    replayFinalState.buffer = static_cast<unsigned char*>(std::malloc(static_cast<size_t>(finalStateLen)));
    replayFinalState.len = finalStateLen;
    replayFinalState.checksum = finalStateChecksum;
    replayFinalState.frame = finalStateFrame;
    if (replayState.buffer == nullptr || replayFinalState.buffer == nullptr)
    {
        std::free(replayState.buffer);
        std::free(replayFinalState.buffer);
        error = "could not allocate rollback_sanity_test.replay state";
        return false;
    }

    if (!ReadReplayBytes(file, replayState.buffer, static_cast<size_t>(stateLen)))
    {
        std::free(replayState.buffer);
        std::free(replayFinalState.buffer);
        error = "could not read rollback_sanity_test.replay state";
        return false;
    }
    if (!ReadReplayBytes(file, replayFinalState.buffer, static_cast<size_t>(finalStateLen)))
    {
        std::free(replayState.buffer);
        std::free(replayFinalState.buffer);
        error = "could not read rollback_sanity_test.replay final state";
        return false;
    }

    std::vector<std::array<uint32_t, kRollbackDebugReplayPlayers>> replayInputs(static_cast<size_t>(inputCount));
    std::vector<uint64_t> replayInputHashes(static_cast<size_t>(inputHashCount));
    std::vector<uint64_t> replayFrameHashes(static_cast<size_t>(frameHashCount));
    for (auto& input : replayInputs)
    {
        if (!ReadReplayBytes(file, input.data(), input.size() * sizeof(input[0])))
        {
            std::free(replayState.buffer);
            std::free(replayFinalState.buffer);
            error = "could not read rollback_sanity_test.replay inputs";
            return false;
        }
    }
    if (!replayInputHashes.empty() &&
        !ReadReplayBytes(file, replayInputHashes.data(), replayInputHashes.size() * sizeof(replayInputHashes[0])))
    {
        std::free(replayState.buffer);
        std::free(replayFinalState.buffer);
        error = "could not read rollback_sanity_test.replay input hashes";
        return false;
    }
    if (!replayFrameHashes.empty() &&
        !ReadReplayBytes(file, replayFrameHashes.data(), replayFrameHashes.size() * sizeof(replayFrameHashes[0])))
    {
        std::free(replayState.buffer);
        std::free(replayFinalState.buffer);
        error = "could not read rollback_sanity_test.replay frame hashes";
        return false;
    }

    CoreRollbackState oldState;
    CoreRollbackState oldFinalState;
    {
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        oldState = g_RollbackDebugReplay.initialState;
        oldFinalState = g_RollbackDebugReplay.finalState;
        g_RollbackDebugReplay.initialState = replayState;
        g_RollbackDebugReplay.finalState = replayFinalState;
        g_RollbackDebugReplay.inputs = std::move(replayInputs);
        g_RollbackDebugReplay.inputHashes = std::move(replayInputHashes);
        g_RollbackDebugReplay.frameHashes = std::move(replayFrameHashes);
        g_RollbackDebugReplay.verifyInputIndex = 0;
        g_RollbackDebugReplay.verifyFrameIndex = 0;
        g_RollbackDebugReplay.firstInputMismatchFrame = -1;
        g_RollbackDebugReplay.firstInputMismatchExpectedHash = 0;
        g_RollbackDebugReplay.firstInputMismatchActualHash = 0;
        g_RollbackDebugReplay.firstMismatchFrame = -1;
        g_RollbackDebugReplay.firstMismatchExpectedHash = 0;
        g_RollbackDebugReplay.firstMismatchActualHash = 0;
        g_RollbackDebugReplay.recordedInputHash = recordedInputHash;
        g_RollbackDebugReplay.replayedInputHash = 0;
        g_RollbackDebugReplay.finalHash = finalHash;
        g_RollbackDebugReplay.ready = true;
    }
    CoreRollbackFreeGameState(oldState);
    CoreRollbackFreeGameState(oldFinalState);
    return true;
}

void GetRollbackStatePayload(const CoreRollbackState& state, const unsigned char*& buffer, int& len)
{
    constexpr int RollbackHeaderInts = 6;
    constexpr int RollbackHeaderSize = RollbackHeaderInts * static_cast<int>(sizeof(int32_t));
    constexpr int32_t RollbackHeaderMagic =
        (static_cast<int32_t>('R') << 24) |
        (static_cast<int32_t>('L') << 16) |
        (static_cast<int32_t>('B') << 8) |
        static_cast<int32_t>('K');
    constexpr int32_t LegacyRollbackHeaderMagic =
        (static_cast<int32_t>('G') << 24) |
        (static_cast<int32_t>('G') << 16) |
        (static_cast<int32_t>('P') << 8) |
        static_cast<int32_t>('O');

    buffer = state.buffer;
    len = state.len;

    if (buffer == nullptr)
    {
        len = 0;
        return;
    }

    if (len >= RollbackHeaderSize)
    {
        int32_t magic = 0;
        int32_t headerSize = 0;
        std::memcpy(&magic, buffer, sizeof(magic));
        std::memcpy(&headerSize, buffer + sizeof(magic), sizeof(headerSize));
        if ((magic == RollbackHeaderMagic || magic == LegacyRollbackHeaderMagic) && headerSize == RollbackHeaderSize)
        {
            buffer += RollbackHeaderSize;
            len -= RollbackHeaderSize;
        }
    }
}

uint64_t HashRollbackState(const CoreRollbackState& state)
{
    constexpr uint64_t OffsetBasis = 14695981039346656037ull;
    constexpr uint64_t Prime = 1099511628211ull;
    uint64_t hash = OffsetBasis;
    const unsigned char* buffer;
    int len;

    GetRollbackStatePayload(state, buffer, len);

    for (int i = 0; i < len; i++)
    {
        hash ^= buffer[i];
        hash *= Prime;
    }

    return hash;
}

bool RollbackDebugReplayBeginFrame(void* userData)
{
    (void)userData;

    RollbackDebugReplayMode mode;
    bool pendingInitialSave;
    bool pendingInitialLoad;
    {
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        mode = g_RollbackDebugReplay.mode;
        pendingInitialSave = g_RollbackDebugReplay.pendingInitialSave;
        pendingInitialLoad = g_RollbackDebugReplay.pendingInitialLoad;
        g_RollbackDebugReplay.frameAdvancedThisFrame = false;
    }

    if (mode == RollbackDebugReplayMode::Recording && pendingInitialSave)
    {
        CoreRollbackState initialState = {};
        const auto saveBeginTime = std::chrono::steady_clock::now();
        if (!CoreRollbackSaveGameState(initialState, CoreGetCurrentFrameCount()))
        {
            const auto saveUs =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - saveBeginTime).count();
            WriteRollbackDebugReplayEventLog("record_initial_save_failed",
                "elapsed_us=" + std::to_string(saveUs) + " error=" + CoreGetError());
            std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
            g_RollbackDebugReplay.mode = RollbackDebugReplayMode::Idle;
            g_RollbackDebugReplay.pendingInitialSave = false;
            return false;
        }
        const auto saveUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - saveBeginTime).count();

        {
            std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
            g_RollbackDebugReplay.initialState = initialState;
            g_RollbackDebugReplay.pendingInitialSave = false;
        }
        WriteRollbackDebugReplayEventLog("record_initial_save",
            "frame=" + std::to_string(initialState.frame) +
            " save_us=" + std::to_string(saveUs) +
            " hash=" + std::to_string(HashRollbackState(initialState)));
        return true;
    }

    if (!RollbackDebugIsPlaybackMode(mode) || !pendingInitialLoad)
    {
        return true;
    }

    const auto loadBeginTime = std::chrono::steady_clock::now();
    if (!CoreRollbackLoadGameState(g_RollbackDebugReplay.initialState))
    {
        const auto loadUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - loadBeginTime).count();
        WriteRollbackDebugReplayEventLog("verify_initial_load_failed",
            "load_us=" + std::to_string(loadUs) + " error=" + CoreGetError());
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        g_RollbackDebugReplay.mode = RollbackDebugReplayMode::Idle;
        g_RollbackDebugReplay.pendingInitialSave = false;
        g_RollbackDebugReplay.pendingInitialLoad = false;
        return false;
    }
    const auto loadUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - loadBeginTime).count();

    CoreRollbackState loadedInitialState = {};
    const auto verifySaveBeginTime = std::chrono::steady_clock::now();
    if (CoreRollbackSaveGameState(loadedInitialState, CoreGetCurrentFrameCount()))
    {
        const auto verifySaveUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - verifySaveBeginTime).count();
        CoreRollbackState expectedInitialState;
        size_t recordedInputFrames;
        {
            std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
            expectedInitialState = g_RollbackDebugReplay.initialState;
            recordedInputFrames = g_RollbackDebugReplay.inputs.size();
        }
        const uint64_t expectedInitialHash = HashRollbackState(expectedInitialState);
        const uint64_t loadedInitialHash = HashRollbackState(loadedInitialState);
        WriteRollbackDebugReplayLog(mode == RollbackDebugReplayMode::Stressing ? "stress_initial_load" : "verify_initial_load",
            expectedInitialHash == loadedInitialHash,
            expectedInitialHash, loadedInitialHash, recordedInputFrames, 0, expectedInitialState, loadedInitialState);
        WriteRollbackDebugReplayEventLog(mode == RollbackDebugReplayMode::Stressing ? "stress_initial_load_timing" : "verify_initial_load_timing",
            "load_us=" + std::to_string(loadUs) +
            " verify_save_us=" + std::to_string(verifySaveUs));
    }
    CoreRollbackFreeGameState(loadedInitialState);

    if (mode == RollbackDebugReplayMode::Stressing && !SaveRollbackDebugStressCheckpoint(0))
    {
        WriteRollbackDebugReplayEventLog("stress_initial_checkpoint_failed", CoreGetError());
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        g_RollbackDebugReplay.mode = RollbackDebugReplayMode::Idle;
        g_RollbackDebugReplay.pendingInitialLoad = false;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        g_RollbackDebugReplay.pendingInitialLoad = false;
    }
    return true;
}

bool RollbackDebugReplayInputProvider(uint32_t* inputs, int players, void* userData)
{
    (void)userData;
    if (inputs == nullptr || players < 1)
    {
        return false;
    }

    std::array<uint32_t, kRollbackDebugReplayPlayers> frameInputs = {};
    RollbackDebugReplayMode mode;
    bool verifyWithGraphics = false;
    {
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        mode = g_RollbackDebugReplay.mode;
        verifyWithGraphics = g_RollbackDebugReplay.verifyWithGraphics;
    }

    if (mode == RollbackDebugReplayMode::Recording)
    {
        CoreSetFrameOutput(CoreFrameOutput_All);
        const int playerCount = std::min(players, kRollbackDebugReplayPlayers);
        for (int i = 0; i < playerCount; i++)
        {
            frameInputs[i] = inputs[i];
        }

        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        if (g_RollbackDebugReplay.inputs.size() < kRollbackDebugReplayFrames)
        {
            const uint64_t inputHash = HashRollbackDebugReplayInputFrame(frameInputs);
            g_RollbackDebugReplay.inputHashes.push_back(inputHash);
            g_RollbackDebugReplay.recordedInputHash =
                AppendRollbackDebugReplayHash(g_RollbackDebugReplay.recordedInputHash, inputHash);
            g_RollbackDebugReplay.inputs.push_back(frameInputs);
        }
        g_RollbackDebugReplay.frameAdvancedThisFrame = true;
    }
    else if (RollbackDebugIsPlaybackMode(mode))
    {
        CoreSetFrameOutput(verifyWithGraphics ? CoreFrameOutput_All : CoreFrameOutput_None);
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        if (g_RollbackDebugReplay.verifyInputIndex >= g_RollbackDebugReplay.inputs.size())
        {
            return false;
        }

        frameInputs = g_RollbackDebugReplay.inputs[g_RollbackDebugReplay.verifyInputIndex++];
        const uint64_t inputHash = HashRollbackDebugReplayInputFrame(frameInputs);
        if (g_RollbackDebugReplay.verifyInputIndex <= g_RollbackDebugReplay.inputHashes.size())
        {
            const uint64_t expectedInputHash = g_RollbackDebugReplay.inputHashes[g_RollbackDebugReplay.verifyInputIndex - 1];
            if (inputHash != expectedInputHash && g_RollbackDebugReplay.firstInputMismatchFrame < 0)
            {
                g_RollbackDebugReplay.firstInputMismatchFrame = static_cast<int>(g_RollbackDebugReplay.verifyInputIndex);
                g_RollbackDebugReplay.firstInputMismatchExpectedHash = expectedInputHash;
                g_RollbackDebugReplay.firstInputMismatchActualHash = inputHash;
            }
        }
        if (g_RollbackDebugReplay.countReplayInputHash)
        {
            g_RollbackDebugReplay.replayedInputHash =
                AppendRollbackDebugReplayHash(g_RollbackDebugReplay.replayedInputHash, inputHash);
        }
        g_RollbackDebugReplay.frameAdvancedThisFrame = true;
    }
    else
    {
        return CoreRollbackSampleInput(inputs, static_cast<int>(sizeof(uint32_t)), players);
    }

    std::memset(inputs, 0, static_cast<size_t>(players) * sizeof(uint32_t));
    const int playerCount = std::min(players, kRollbackDebugReplayPlayers);
    for (int i = 0; i < playerCount; i++)
    {
        inputs[i] = frameInputs[i];
    }
    return true;
}

bool RollbackDebugReplayEndFrame(void* userData)
{
    (void)userData;
    RollbackDebugReplayMode mode;
    bool shouldRecordFrameHash = false;
    bool shouldFinishRecording = false;
    bool shouldVerifyFrame = false;
    {
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        mode = g_RollbackDebugReplay.mode;
        shouldRecordFrameHash =
            mode == RollbackDebugReplayMode::Recording &&
            g_RollbackDebugReplay.frameAdvancedThisFrame;
        shouldFinishRecording =
            shouldRecordFrameHash &&
            g_RollbackDebugReplay.inputs.size() >= static_cast<size_t>(kRollbackDebugReplayFrames);
        shouldVerifyFrame =
            RollbackDebugIsPlaybackMode(mode) &&
            g_RollbackDebugReplay.frameAdvancedThisFrame;
        g_RollbackDebugReplay.frameAdvancedThisFrame = false;
    }

    if (shouldRecordFrameHash && !RollbackDebugRecordFrameHashFromRollbackFrame())
    {
        return false;
    }
    if (shouldFinishRecording)
    {
        return RollbackDebugFinishRecordingFromRollbackFrame();
    }
    if (shouldVerifyFrame)
    {
        return RollbackDebugVerifyFromRollbackFrame();
    }

    return true;
}

bool RollbackDebugRecordFrameHashFromRollbackFrame()
{
    size_t frameHashCount;
    {
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        frameHashCount = g_RollbackDebugReplay.inputs.size();
    }
    if (frameHashCount <= 5 || (frameHashCount % 60) == 0)
    {
        WriteRollbackDebugReplayEventLog("record_frame_hash",
            "frame=" + std::to_string(frameHashCount) +
            " skipped=1 reason=hot_path_debug_hash_disabled");
    }

    return true;
}

bool RollbackDebugFinishRecordingFromRollbackFrame()
{
    CoreRollbackState finalState;
    const auto saveBeginTime = std::chrono::steady_clock::now();
    if (!CoreRollbackSaveGameState(finalState, CoreGetCurrentFrameCount()))
    {
        const auto saveUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - saveBeginTime).count();
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        g_RollbackDebugReplay.mode = RollbackDebugReplayMode::Idle;
        g_RollbackDebugReplay.ready = false;
        WriteRollbackDebugReplayEventLog("record_failed",
            "save_us=" + std::to_string(saveUs) + " error=" + CoreGetError());
        return false;
    }
    const auto saveUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - saveBeginTime).count();

    const uint64_t finalHash = HashRollbackState(finalState);
    size_t recordedInputFrames;
    {
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        if (g_RollbackDebugReplay.frameHashes.empty())
        {
            g_RollbackDebugReplay.frameHashes.push_back(finalHash);
        }
        else if (g_RollbackDebugReplay.frameHashes.size() < static_cast<size_t>(kRollbackDebugReplayFrames))
        {
            g_RollbackDebugReplay.frameHashes.push_back(finalHash);
        }
        g_RollbackDebugReplay.finalHash = finalHash;
        g_RollbackDebugReplay.mode = RollbackDebugReplayMode::Idle;
        recordedInputFrames = g_RollbackDebugReplay.inputs.size();
    }

    std::string replayFileError;
    const bool replayFileSaved = SaveRollbackDebugReplayFile(replayFileError, finalState);
    {
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        g_RollbackDebugReplay.ready = replayFileSaved;
    }
    WriteRollbackDebugReplayLog("record", replayFileSaved, finalHash, finalHash,
        recordedInputFrames, recordedInputFrames, finalState, finalState);
    WriteRollbackDebugReplayEventLog(replayFileSaved ? "record_finished" : "record_failed",
        replayFileSaved ? ("frames=" + std::to_string(recordedInputFrames) +
            " final_save_us=" + std::to_string(saveUs)) : replayFileError);
    CoreRollbackFreeGameState(finalState);
    RollbackDebugCloseSession();
    return replayFileSaved;
}

bool RollbackDebugVerifyFromRollbackFrame()
{
    RollbackDebugReplayMode mode;
    bool hasPerFrameHashes;
    size_t pendingFrameIndex;
    bool willFinish;
    {
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        mode = g_RollbackDebugReplay.mode;
        hasPerFrameHashes =
            g_RollbackDebugReplay.frameHashes.size() == static_cast<size_t>(kRollbackDebugReplayFrames);
        pendingFrameIndex = g_RollbackDebugReplay.verifyFrameIndex + 1;
        willFinish = pendingFrameIndex >= static_cast<size_t>(kRollbackDebugReplayFrames);
    }

    CoreRollbackState finalState;
    long long saveUs = 0;
    uint64_t finalHash = 0;
    const bool shouldHashState = hasPerFrameHashes || willFinish;
    if (shouldHashState)
    {
        const auto saveBeginTime = std::chrono::steady_clock::now();
        if (!CoreRollbackSaveGameState(finalState, CoreGetCurrentFrameCount()))
        {
            saveUs =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - saveBeginTime).count();
            std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
            g_RollbackDebugReplay.mode = RollbackDebugReplayMode::Idle;
            WriteRollbackDebugReplayEventLog("verify_frame_save_failed",
                "save_us=" + std::to_string(saveUs) + " error=" + CoreGetError());
            return false;
        }
        saveUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - saveBeginTime).count();
        finalHash = HashRollbackState(finalState);
    }

    uint64_t expectedHash;
    CoreRollbackState expectedFinalState;
    size_t replayedInputFrames;
    size_t recordedInputFrames;
    size_t verifyFrameIndex;
    bool finished;
    bool matched;
    bool shouldStressRollback = false;
    {
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        if (hasPerFrameHashes && g_RollbackDebugReplay.verifyFrameIndex < g_RollbackDebugReplay.frameHashes.size())
        {
            const uint64_t expectedFrameHash = g_RollbackDebugReplay.frameHashes[g_RollbackDebugReplay.verifyFrameIndex];
            if (finalHash != expectedFrameHash && g_RollbackDebugReplay.firstMismatchFrame < 0)
            {
                g_RollbackDebugReplay.firstMismatchFrame = static_cast<int>(g_RollbackDebugReplay.verifyFrameIndex + 1);
                g_RollbackDebugReplay.firstMismatchExpectedHash = expectedFrameHash;
                g_RollbackDebugReplay.firstMismatchActualHash = finalHash;
                WriteRollbackDebugReplayEventLog("verify_frame_mismatch",
                    "frame=" + std::to_string(g_RollbackDebugReplay.firstMismatchFrame) +
                    " expected_hash=" + std::to_string(expectedFrameHash) +
                    " actual_hash=" + std::to_string(finalHash) +
                    " replayed_inputs=" + std::to_string(g_RollbackDebugReplay.verifyInputIndex));
            }
        }
        g_RollbackDebugReplay.verifyFrameIndex++;
        expectedHash = g_RollbackDebugReplay.finalHash;
        expectedFinalState = g_RollbackDebugReplay.finalState;
        replayedInputFrames = g_RollbackDebugReplay.verifyInputIndex;
        recordedInputFrames = g_RollbackDebugReplay.inputs.size();
        verifyFrameIndex = g_RollbackDebugReplay.verifyFrameIndex;
        finished = verifyFrameIndex >= static_cast<size_t>(kRollbackDebugReplayFrames);
        shouldStressRollback =
            mode == RollbackDebugReplayMode::Stressing &&
            !finished &&
            verifyFrameIndex >= static_cast<size_t>(kRollbackDebugStressRollbackFrames) &&
            (verifyFrameIndex % static_cast<size_t>(kRollbackDebugStressInterval)) == 0;
        matched = finished &&
            finalHash == expectedHash &&
            recordedInputFrames == replayedInputFrames &&
            g_RollbackDebugReplay.recordedInputHash == g_RollbackDebugReplay.replayedInputHash &&
            g_RollbackDebugReplay.firstInputMismatchFrame < 0;
        if (finished)
        {
            g_RollbackDebugReplay.mode = RollbackDebugReplayMode::Idle;
            g_RollbackDebugReplay.lastVerifyCompleted = true;
            g_RollbackDebugReplay.lastVerifyMatched = matched;
            g_RollbackDebugReplay.lastVerifyExpectedHash = expectedHash;
            g_RollbackDebugReplay.lastVerifyActualHash = finalHash;
            g_RollbackDebugReplay.lastVerifyRecordedInputFrames = recordedInputFrames;
            g_RollbackDebugReplay.lastVerifyReplayedInputFrames = replayedInputFrames;
        }
    }

    if (!finished && (verifyFrameIndex <= 5 || (verifyFrameIndex % 60) == 0))
    {
        WriteRollbackDebugReplayEventLog("verify_frame_progress",
            "frame=" + std::to_string(verifyFrameIndex) +
            " save_us=" + std::to_string(saveUs) +
            " hash=" + (shouldHashState ? std::to_string(finalHash) : std::string("skipped")) +
            " replayed_inputs=" + std::to_string(replayedInputFrames));
    }

    if (mode == RollbackDebugReplayMode::Stressing && !SaveRollbackDebugStressCheckpoint(verifyFrameIndex))
    {
        CoreRollbackFreeGameState(finalState);
        WriteRollbackDebugReplayEventLog("stress_checkpoint_failed",
            "frame=" + std::to_string(verifyFrameIndex) + " error=" + CoreGetError());
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        g_RollbackDebugReplay.mode = RollbackDebugReplayMode::Idle;
        return false;
    }

    if (shouldStressRollback && !RollbackDebugRunStressRollbackFromRollbackFrame(verifyFrameIndex))
    {
        CoreRollbackFreeGameState(finalState);
        return false;
    }

    if (finished)
    {
        WriteRollbackDebugReplayLog(mode == RollbackDebugReplayMode::Stressing ? "stress_verify" : "verify",
            matched, expectedHash, finalHash,
            recordedInputFrames, replayedInputFrames, expectedFinalState, finalState);
        RollbackDebugCloseSession();
    }
    CoreRollbackFreeGameState(finalState);
    return true;
}

bool RollbackDebugRunStressRollbackFromRollbackFrame(size_t frame)
{
    const auto burstBeginTime = std::chrono::steady_clock::now();
    const size_t rollbackFrame = frame - static_cast<size_t>(kRollbackDebugStressRollbackFrames);
    CoreRollbackState& checkpoint =
        g_RollbackDebugReplay.stressCheckpoints[RollbackDebugStressCheckpointIndex(rollbackFrame)];
    if (checkpoint.buffer == nullptr)
    {
        WriteRollbackDebugReplayEventLog("stress_failed",
            "missing checkpoint for frame=" + std::to_string(rollbackFrame));
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        g_RollbackDebugReplay.mode = RollbackDebugReplayMode::Idle;
        return false;
    }

    const auto loadBeginTime = std::chrono::steady_clock::now();
    if (!CoreRollbackLoadGameState(checkpoint))
    {
        const std::string error = CoreGetError();
        WriteRollbackDebugReplayEventLog("stress_failed",
            "load frame=" + std::to_string(rollbackFrame) + " error=" + error);
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        g_RollbackDebugReplay.mode = RollbackDebugReplayMode::Idle;
        if (RollbackDebugShouldIgnoreStopFailure(error))
        {
            WriteRollbackDebugReplayEventLog("stress_stopped",
                "ignored shutdown load failure at frame=" + std::to_string(rollbackFrame) + " error=" + error);
            return true;
        }
        return false;
    }
    const auto loadUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - loadBeginTime).count();

    std::array<std::array<uint32_t, kRollbackDebugReplayPlayers>, kRollbackDebugStressRollbackFrames> resimInputs = {};
    {
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        if (frame > g_RollbackDebugReplay.inputs.size())
        {
            g_RollbackDebugReplay.mode = RollbackDebugReplayMode::Idle;
            return false;
        }
        for (int i = 0; i < kRollbackDebugStressRollbackFrames; i++)
        {
            resimInputs[static_cast<size_t>(i)] =
                g_RollbackDebugReplay.inputs[rollbackFrame + static_cast<size_t>(i)];
        }
        g_RollbackDebugReplay.countReplayInputHash = false;
        g_RollbackDebugReplay.stressRollbackCount++;
        g_RollbackDebugReplay.stressRollbackFrames += kRollbackDebugStressRollbackFrames;
        g_RollbackDebugReplay.stressResimulatedFrames += kRollbackDebugStressRollbackFrames;
    }

    long long resimTotalUs = 0;
    long long resimMaxUs = 0;
    for (int i = 0; i < kRollbackDebugStressRollbackFrames; i++)
    {
        const size_t resimFrame = rollbackFrame + static_cast<size_t>(i) + 1;
        const auto resimBeginTime = std::chrono::steady_clock::now();
        const bool resimOk = rmgk_gekko::debug_run_frame_with_inputs(
            resimInputs[static_cast<size_t>(i)].data(), kRollbackDebugReplayPlayers, CoreFrameOutput_None);
        const long long resimUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - resimBeginTime).count();
        CoreRollbackRunFrameStats runFrameStats;
        const bool hasRunFrameStats = CoreRollbackGetRunFrameStats(runFrameStats);
        std::string statsMessage =
            "frame=" + std::to_string(resimFrame) +
            " source_frame=" + std::to_string(rollbackFrame + static_cast<size_t>(i)) +
            " resim_us=" + std::to_string(resimUs) +
            " p1=" + std::to_string(resimInputs[static_cast<size_t>(i)][0]) +
            " p2=" + std::to_string(resimInputs[static_cast<size_t>(i)][1]) +
            " p3=" + std::to_string(resimInputs[static_cast<size_t>(i)][2]) +
            " p4=" + std::to_string(resimInputs[static_cast<size_t>(i)][3]);
        if (hasRunFrameStats)
        {
            statsMessage += FormatRollbackRunFrameStats(runFrameStats);
        }

        if (!resimOk)
        {
            const std::string error = CoreGetError();
            WriteRollbackDebugReplayEventLog("stress_resim_failed", statsMessage + " error=" + error);
            std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
            g_RollbackDebugReplay.mode = RollbackDebugReplayMode::Idle;
            g_RollbackDebugReplay.countReplayInputHash = true;
            if (RollbackDebugShouldIgnoreStopFailure(error))
            {
                WriteRollbackDebugReplayEventLog("stress_stopped",
                    "ignored shutdown failure at frame=" + std::to_string(resimFrame) + " error=" + error);
                return true;
            }
            return false;
        }
        WriteRollbackDebugReplayEventLog("stress_resim_frame", statsMessage);
        resimTotalUs += resimUs;
        resimMaxUs = std::max(resimMaxUs, resimUs);
    }

    {
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        g_RollbackDebugReplay.countReplayInputHash = true;
    }

    const auto burstTotalUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - burstBeginTime).count();
    WriteRollbackDebugReplayEventLog("stress_rollback",
        "frame=" + std::to_string(frame) +
        " rollback_to=" + std::to_string(rollbackFrame) +
        " load_us=" + std::to_string(loadUs) +
        " resim_total_us=" + std::to_string(resimTotalUs) +
        " resim_max_us=" + std::to_string(resimMaxUs) +
        " total_us=" + std::to_string(burstTotalUs) +
        " resimulated=" + std::to_string(kRollbackDebugStressRollbackFrames));
    return true;
}

int FirstRollbackStatePayloadDifference(const CoreRollbackState& expected, const CoreRollbackState& actual, int& expectedLen, int& actualLen)
{
    const unsigned char* expectedBuffer;
    const unsigned char* actualBuffer;
    GetRollbackStatePayload(expected, expectedBuffer, expectedLen);
    GetRollbackStatePayload(actual, actualBuffer, actualLen);

    const int compareLen = std::min(expectedLen, actualLen);
    for (int i = 0; i < compareLen; i++)
    {
        if (expectedBuffer[i] != actualBuffer[i])
        {
            return i;
        }
    }

    if (expectedLen != actualLen)
    {
        return compareLen;
    }

    return -1;
}

void WriteRollbackDebugReplayLog(const std::string& phase,
    bool matched,
    uint64_t expectedHash,
    uint64_t actualHash,
    size_t recordedInputFrames,
    size_t replayedInputFrames,
    const CoreRollbackState& expectedState,
    const CoreRollbackState& actualState)
{
    int expectedPayloadLen = 0;
    int actualPayloadLen = 0;
    const int firstDiff = FirstRollbackStatePayloadDifference(expectedState, actualState, expectedPayloadLen, actualPayloadLen);
    const unsigned char* expectedPayload = nullptr;
    const unsigned char* actualPayload = nullptr;
    GetRollbackStatePayload(expectedState, expectedPayload, expectedPayloadLen);
    GetRollbackStatePayload(actualState, actualPayload, actualPayloadLen);

    std::ofstream log(GetRollbackDebugReplayLogPath(), std::ios::app);
    if (!log.is_open())
    {
        return;
    }

    log << "phase=" << phase << "\n";
    log << "matched=" << (matched ? "true" : "false") << "\n";
    log << "expected_hash=" << expectedHash << "\n";
    log << "actual_hash=" << actualHash << "\n";
    log << "recorded_input_frames=" << recordedInputFrames << "\n";
    log << "replayed_input_frames=" << replayedInputFrames << "\n";
    log << "recorded_input_hashes=" << g_RollbackDebugReplay.inputHashes.size() << "\n";
    log << "recorded_input_hash=" << g_RollbackDebugReplay.recordedInputHash << "\n";
    log << "replayed_input_hash=" << g_RollbackDebugReplay.replayedInputHash << "\n";
    log << "inputs_match=" << (g_RollbackDebugReplay.recordedInputHash == g_RollbackDebugReplay.replayedInputHash ? "true" : "false") << "\n";
    log << "first_input_mismatch_frame=" << g_RollbackDebugReplay.firstInputMismatchFrame << "\n";
    log << "first_input_mismatch_expected_hash=" << g_RollbackDebugReplay.firstInputMismatchExpectedHash << "\n";
    log << "first_input_mismatch_actual_hash=" << g_RollbackDebugReplay.firstInputMismatchActualHash << "\n";
    for (size_t i = 0; i < std::min<size_t>(g_RollbackDebugReplay.inputs.size(), 4); i++)
    {
        log << "input_frame_" << (i + 1) << "=";
        for (size_t player = 0; player < g_RollbackDebugReplay.inputs[i].size(); player++)
        {
            log << std::hex << g_RollbackDebugReplay.inputs[i][player] << (player + 1 < g_RollbackDebugReplay.inputs[i].size() ? ' ' : '\n');
        }
        log << std::dec;
    }
    log << "recorded_frame_hashes=" << g_RollbackDebugReplay.frameHashes.size() << "\n";
    log << "verify_frame_index=" << g_RollbackDebugReplay.verifyFrameIndex << "\n";
    log << "stress_rollback_count=" << g_RollbackDebugReplay.stressRollbackCount << "\n";
    log << "stress_rollback_frames=" << g_RollbackDebugReplay.stressRollbackFrames << "\n";
    log << "stress_resimulated_frames=" << g_RollbackDebugReplay.stressResimulatedFrames << "\n";
    log << "first_mismatch_frame=" << g_RollbackDebugReplay.firstMismatchFrame << "\n";
    log << "first_mismatch_expected_hash=" << g_RollbackDebugReplay.firstMismatchExpectedHash << "\n";
    log << "first_mismatch_actual_hash=" << g_RollbackDebugReplay.firstMismatchActualHash << "\n";
    log << "replay_file=" << kRollbackDebugReplayFilePath << "\n";
    log << "replay_version=" << kRollbackDebugReplayVersion << "\n";
    log << "expected_state_len=" << expectedState.len << "\n";
    log << "actual_state_len=" << actualState.len << "\n";
    log << "expected_payload_len=" << expectedPayloadLen << "\n";
    log << "actual_payload_len=" << actualPayloadLen << "\n";
    log << "first_payload_diff=" << firstDiff << "\n";
    log << "first_payload_diff_region=" << GetRollbackDebugReplayPayloadRegion(firstDiff) << "\n";

    if (firstDiff >= 0)
    {
        const int start = std::max(0, firstDiff - 16);
        const int end = std::min(std::min(expectedPayloadLen, actualPayloadLen), firstDiff + 16);
        log << "diff_window_start=" << start << "\n";
        log << "expected_bytes=";
        for (int i = start; i < end; i++)
        {
            log << std::hex << static_cast<int>(expectedPayload[i]) << (i + 1 < end ? ' ' : '\n');
        }
        log << std::dec;
        log << "actual_bytes=";
        for (int i = start; i < end; i++)
        {
            log << std::hex << static_cast<int>(actualPayload[i]) << (i + 1 < end ? ' ' : '\n');
        }
        log << std::dec;
    }

    log << "---\n";
}

void WriteRollbackDebugReplayEventLog(const std::string& phase, const std::string& message)
{
    std::ofstream log(GetRollbackDebugReplayLogPath(), std::ios::app);
    if (!log.is_open())
    {
        return;
    }

    log << "phase=" << phase << "\n";
    log << "message=" << message << "\n";
    log << "replay_file=" << kRollbackDebugReplayFilePath << "\n";
    log << "replay_version=" << kRollbackDebugReplayVersion << "\n";
    log << "---\n";
}

QString resolveStyleFactoryKey(const QString& styleName)
{
    const QStringList styleKeys = QStyleFactory::keys();
    for (const QString& styleKey : styleKeys)
    {
        if (QString::compare(styleKey, styleName, Qt::CaseInsensitive) == 0)
        {
            return styleKey;
        }
    }

    return styleName;
}

QPalette resolveStyleStandardPalette(const QString& styleName, const QPalette& fallbackPalette)
{
    QStyle* style = QStyleFactory::create(styleName);
    if (style == nullptr)
    {
        return fallbackPalette;
    }

    const QPalette palette = style->standardPalette();
    delete style;
    return palette;
}

#ifdef NETPLAY
constexpr std::chrono::seconds kLocalEchoMaxAge(2);
constexpr size_t kLocalEchoMaxEntries = 8;

QString NormalizeOsdKailleraChatMessage(QString message)
{
    message = message.trimmed();
    message.replace('\r', ' ');
    message.replace('\n', ' ');
    return message;
}

#ifdef _WIN32
std::array<std::string, 4> GetLiveKailleraPortLabelNames()
{
    std::array<std::string, 4> playerNames;
    for (size_t i = 0; i < playerNames.size(); ++i)
    {
        playerNames[i] = recording_player_names[i];
    }
    return playerNames;
}
#endif // _WIN32
#endif // NETPLAY
} // namespace

MainWindow::MainWindow() : QMainWindow(nullptr)
{
}

MainWindow::~MainWindow()
{
    CoreRollbackFreeGameState(this->ui_RollbackDebugState);
    CoreRollbackFreeGameState(g_RollbackDebugReplay.initialState);
    CoreRollbackFreeGameState(g_RollbackDebugReplay.finalState);
    FreeRollbackDebugStressCheckpoints();
}

bool MainWindow::Init(QApplication* app, bool showUI, bool launchROM)
{
    if (!CoreInit())
    {
        this->showErrorMessage("CoreInit() Failed", QString::fromStdString(CoreGetError()));
        return false;
    }

    if (!CoreApplyPluginSettings())
    {
        this->showErrorMessage("CoreApplyPluginSettings() Failed", QString::fromStdString(CoreGetError()));
    }

    this->configureTheme(app);

    this->initializeUI(launchROM);
    this->initializeActions();
    this->configureUI(app, showUI);
#ifdef NETPLAY
    this->refreshKailleraRecordingStorageStatus(true);
#endif // NETPLAY

    this->connectActionSignals();
    this->configureActions();
    this->updateActions(false, false);

#ifdef UPDATER
    this->checkForUpdates(true, false);
#else
    this->action_Help_Update->setVisible(false);
#endif // UPDATER

#ifndef NETPLAY
    this->menuNetplay->menuAction()->setVisible(false);
#endif // NETPLAY

    this->initializeEmulationThread();
    this->connectEmulationThreadSignals();

    if (!SetupVidExt(this->emulationThread, this, &this->ui_Widget_OpenGL, &this->ui_Widget_Vulkan))
    {
        this->showErrorMessage("SetupVidExt() Failed", QString::fromStdString(CoreGetError()));
        return false;
    }

    this->coreCallBacks = new CoreCallbacks(this);

    // connect signals early due to pending debug callbacks
    connect(coreCallBacks, &CoreCallbacks::OnCoreDebugCallback, this, &MainWindow::on_Core_DebugCallback);
    connect(coreCallBacks, &CoreCallbacks::OnCoreStateCallback, this, &MainWindow::on_Core_StateCallback);
    connect(app, &QGuiApplication::applicationStateChanged, this, &MainWindow::on_QGuiApplication_applicationStateChanged);

    if (!this->coreCallBacks->Init())
    {
        this->showErrorMessage("CoreCallbacks::Init() Failed", QString::fromStdString(CoreGetError()));
        return false;
    }

    // add actions when there's no UI
    if (!showUI)
    {
        this->addActions();
    }

#ifdef NETPLAY
    this->ui_AutoStartNetplayOnStartupPending =
        showUI &&
        !launchROM &&
        CoreSettingsGetBoolValue(SettingsID::GUI_AutoStartNetplayOnStartup);
    // Note: tryAutoStartNetplayOnStartup() is called from showEvent()
    // to ensure the main window is visible before opening Kaillera dialog
#endif // NETPLAY

    // Check for raphnet plugin mismatch after window is visible
    this->ui_CheckRaphnetPluginMismatchPending = showUI && !launchROM;

    return true;
}

void MainWindow::OpenROM(QString file, QString disk, bool fullscreen, bool quitAfterEmulation, int stateSlot)
{
    this->ui_LaunchInFullscreen = fullscreen;
    this->ui_QuitAfterEmulation = quitAfterEmulation;

    // ensure we don't switch to the ROM browser
    // because it can cause a slight flicker,
    // if we just ensure the UI is in an emulation
    // state, then the transition will be smoother
    this->updateUI(true, false);

    this->launchEmulationThread(file, disk, true, stateSlot);
}

#ifdef _WIN32
void MainWindow::restoreDisplayMode(void)
{
    if (!this->ui_DisplayModeChanged)
    {
        return;
    }
    if (!this->ui_DisplayModeDevice.empty())
    {
        ChangeDisplaySettingsExW(this->ui_DisplayModeDevice.c_str(), NULL, NULL, 0, NULL);
    }
    else
    {
        ChangeDisplaySettingsW(NULL, 0);
    }
    this->ui_DisplayModeChanged = false;
    this->ui_DisplayModeDevice.clear();
}

bool MainWindow::applyExclusiveFullscreen(void)
{
    std::string monitor = CoreSettingsGetStringValue(SettingsID::GUI_ExclusiveFullscreenMonitor);
    std::string resolution = CoreSettingsGetStringValue(SettingsID::GUI_ExclusiveFullscreenResolution);
    int refreshRate = CoreSettingsGetIntValue(SettingsID::GUI_ExclusiveFullscreenRefreshRate);

    // determine device name
    LPCWSTR deviceName = nullptr;
    std::wstring deviceNameStr;
    if (!monitor.empty())
    {
        deviceNameStr = std::wstring(monitor.begin(), monitor.end());
        deviceName = deviceNameStr.c_str();
    }

    if (resolution.empty() && refreshRate == 0 && monitor.empty())
    {
        // all desktop defaults  Enothing to change
        return true;
    }

    DEVMODEW devmode = {};
    devmode.dmSize = sizeof(devmode);

    // start from current display settings of the target monitor
    if (!EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &devmode))
    {
        return false;
    }

    if (!resolution.empty())
    {
        size_t xPos = resolution.find('x');
        if (xPos != std::string::npos)
        {
            devmode.dmPelsWidth  = std::stoul(resolution.substr(0, xPos));
            devmode.dmPelsHeight = std::stoul(resolution.substr(xPos + 1));
            devmode.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT;
        }
    }

    if (refreshRate > 0)
    {
        devmode.dmDisplayFrequency = refreshRate;
        devmode.dmFields |= DM_DISPLAYFREQUENCY;
    }

    // move window to target monitor before display mode change
    // deviceName=NULL targets the primary monitor, so always position correctly
    {
        DEVMODEW currentMode = {};
        currentMode.dmSize = sizeof(currentMode);
        if (EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &currentMode))
        {
            this->move(currentMode.dmPosition.x, currentMode.dmPosition.y);
        }
    }

    LONG result;
    if (deviceName != nullptr)
    {
        result = ChangeDisplaySettingsExW(deviceName, &devmode, NULL, CDS_FULLSCREEN, NULL);
    }
    else
    {
        result = ChangeDisplaySettingsW(&devmode, CDS_FULLSCREEN);
    }

    if (result == DISP_CHANGE_SUCCESSFUL)
    {
        this->ui_DisplayModeChanged = true;
        if (deviceName != nullptr)
        {
            this->ui_DisplayModeDevice = deviceNameStr;
        }
        return true;
    }
    return false;
}
#endif // _WIN32

void MainWindow::closeEvent(QCloseEvent *event)
{
    bool inEmulation = this->emulationThread->isRunning();

    if (this->ui_ShowUI &&
        !this->ui_ForceClose &&
        inEmulation &&
        CoreSettingsGetBoolValue(SettingsID::GUI_ConfirmExitWhileInGame))
    {
        bool skipExitConfirmation = false;
        bool ret = QtMessageBox::Question(this, "Are you sure you want to exit RMG?", "Don't ask for confirmation again", skipExitConfirmation);
        if (!ret)
        {
            event->ignore();
            return;
        }

        // only save setting when user accepted
        CoreSettingsSetValue(SettingsID::GUI_ConfirmExitWhileInGame, !skipExitConfirmation);
    }

    // we have to make sure we save the geomtry
    // for the ROM browser when emulation
    // isn't running (or hasn't run at all)
    if (!this->ui_QuitAfterEmulation &&
        !inEmulation)
    {
        this->storeGeometry();
    }

    // store toolbar location to settings
    Qt::ToolBarArea toolbarArea = this->toolBarArea(this->toolBar);
    CoreSettingsSetValue(SettingsID::GUI_ToolbarArea, this->getToolbarSettingAreaFromArea(toolbarArea));

    // attempt to shutdown emulation
    this->ui_NoSwitchToRomBrowser = true;
    this->on_Action_System_Shutdown();

    // wait until emulation has shut down
    while (this->emulationThread->isRunning())
    {
        QCoreApplication::processEvents();
    }

    this->ui_Widget_RomBrowser->StopRefreshRomList();

    this->coreCallBacks->Stop();

#ifdef NETPLAY
    if (this->netplaySessionDialog != nullptr)
    {
        this->netplaySessionDialog->close();
    }

    // Close ALL top-level Kaillera dialogs, including those with nullptr parent
    // (e.g. KailleraServerBrowserDialog, KailleraP2PDialog).
    // Their reject() handlers call kaillera_disconnect() / p2p_disconnect() for
    // graceful server disconnect before we tear down the session.
    for (QWidget* w : QApplication::topLevelWidgets())
    {
        if (w != this && w->isVisible())
        {
            QDialog* dlg = qobject_cast<QDialog*>(w);
            if (dlg)
                dlg->close();
        }
    }

    // Process events so the dialog chain fully unwinds
    // (server browser close ↁEnetplay dialog close ↁEshowServerDialog returns)
    QCoreApplication::processEvents();

    // Shutdown Kaillera if still active (safety net  Edialogs should have cleaned up)
    if (this->kailleraSessionManager != nullptr)
    {
        CoreEndKailleraGame();
        CoreShutdownKaillera();
        delete this->kailleraSessionManager;
        this->kailleraSessionManager = nullptr;
    }
#endif // NETPLAY

    this->logDialog.close();

#ifdef _WIN32
    this->restoreDisplayMode();
#endif
    CoreSettingsSave();
    CoreShutdown();

    QMainWindow::closeEvent(event);
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);

#ifdef NETPLAY
    // Try to auto-start netplay after the window is visible
    // singleShot(0) queues the call to run after this event completes,
    // ensuring the window is painted before the Kaillera dialog opens
    if (this->ui_AutoStartNetplayOnStartupPending)
    {
        QTimer::singleShot(0, this, [this]() {
            this->tryAutoStartNetplayOnStartup();
        });
    }
#endif // NETPLAY

    // Check for raphnet plugin mismatch after the window is visible
    if (this->ui_CheckRaphnetPluginMismatchPending)
    {
        this->ui_CheckRaphnetPluginMismatchPending = false;
        QTimer::singleShot(0, this, [this]() {
            this->checkRaphnetPluginMismatch();
        });
    }
}

void MainWindow::initializeUI(bool launchROM)
{
    this->setupUi(this);

    this->ui_Widgets = new QStackedWidget(this);
    this->ui_Widget_RomBrowser = new Widget::RomBrowserWidget(this);
    this->ui_Widget_Dummy = new Widget::DummyWidget(this);

    this->ui_EventFilter = new EventFilter(this);
    this->ui_StatusBar_Label = new QLabel(this);
    this->ui_StatusBar_RenderModeLabel = new QLabel(this);

    // only start refreshing the ROM browser
    // when RMG isn't launched with a ROM
    // specified on the commandline
    if (!launchROM)
    {
        this->ui_Widget_RomBrowser->RefreshRomList();
    }

    connect(this->ui_Widget_RomBrowser, &Widget::RomBrowserWidget::PlayGame, this,
            &MainWindow::on_RomBrowser_PlayGame);
    connect(this->ui_Widget_RomBrowser, &Widget::RomBrowserWidget::PlayGameWith, this,
            &MainWindow::on_RomBrowser_PlayGameWith);
    connect(this->ui_Widget_RomBrowser, &Widget::RomBrowserWidget::PlayGameWithDisk, this,
            &MainWindow::on_RomBrowser_PlayGameWithDisk);
    connect(this->ui_Widget_RomBrowser, &Widget::RomBrowserWidget::PlayGameWithSlot, this,
            &MainWindow::on_RomBrowser_PlayGameWithSlot);
    connect(this->ui_Widget_RomBrowser, &Widget::RomBrowserWidget::EditGameSettings, this,
            &MainWindow::on_RomBrowser_EditGameSettings);
    connect(this->ui_Widget_RomBrowser, &Widget::RomBrowserWidget::EditGameInputSettings, this,
            &MainWindow::on_RomBrowser_EditGameInputSettings);
    connect(this->ui_Widget_RomBrowser, &Widget::RomBrowserWidget::Cheats, this,
            &MainWindow::on_RomBrowser_Cheats);
    connect(this->ui_Widget_RomBrowser, &Widget::RomBrowserWidget::ChangeRomDirectory, this,
            &MainWindow::on_RomBrowser_ChangeRomDirectory);
    connect(this->ui_Widget_RomBrowser, &Widget::RomBrowserWidget::RomInformation, this,
            &MainWindow::on_RomBrowser_RomInformation);
    connect(this->ui_Widget_RomBrowser, &Widget::RomBrowserWidget::FileDropped, this,
            &MainWindow::on_EventFilter_FileDropped);
#ifdef NETPLAY
    connect(this->ui_Widget_RomBrowser, &Widget::RomBrowserWidget::RomListRefreshFinished, this,
            &MainWindow::on_RomBrowser_RomListRefreshFinished);
#endif // NETPLAY

    connect(this->ui_EventFilter, &EventFilter::on_EventFilter_KeyPressed, this,
            &MainWindow::on_EventFilter_KeyPressed);
    connect(this->ui_EventFilter, &EventFilter::on_EventFilter_KeyReleased, this,
            &MainWindow::on_EventFilter_KeyReleased);
    connect(this->ui_EventFilter, &EventFilter::on_EventFilter_FileDropped, this,
            &MainWindow::on_EventFilter_FileDropped);
}

void MainWindow::configureUI(QApplication* app, bool showUI)
{
    this->setCentralWidget(this->ui_Widgets);

    QString geometry = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::RomBrowser_Geometry));
    bool maximized = CoreSettingsGetBoolValue(SettingsID::RomBrowser_Maximized);
    if (maximized)
    {
        this->showMaximized();
    }
    else if (!geometry.isEmpty())
    {
        this->restoreGeometry(QByteArray::fromBase64(geometry.toLocal8Bit()));
    }

    this->ui_ShowUI = showUI;

    if (this->ui_ShowUI)
    {
        this->ui_ShowMenubar = true;
        this->ui_ShowToolbar = CoreSettingsGetBoolValue(SettingsID::GUI_Toolbar);
        this->ui_ShowStatusbar = CoreSettingsGetBoolValue(SettingsID::GUI_StatusBar);
    }
    else
    {
        this->ui_ShowMenubar = false;
        this->ui_ShowToolbar = false;
        this->ui_ShowStatusbar = false;
    }

    this->menuBar()->setVisible(this->ui_ShowMenubar);
    this->menuRollback->menuAction()->setVisible(CoreSettingsGetBoolValue(SettingsID::Rollback_EnableLocalTesting));
    this->toolBar->setVisible(this->ui_ShowToolbar);
    this->statusBar()->setVisible(this->ui_ShowStatusbar);
    this->statusBar()->addPermanentWidget(this->ui_StatusBar_Label, 99);
    this->statusBar()->addPermanentWidget(this->ui_StatusBar_RenderModeLabel, 1);

    // set toolbar position according to setting
    int toolbarAreaSetting = CoreSettingsGetIntValue(SettingsID::GUI_ToolbarArea);
    Qt::ToolBarArea toolbarArea = this->getToolbarAreaFromSettingArea(toolbarAreaSetting);
    if (this->toolBarArea(this->toolBar) != toolbarArea)
    {
        this->removeToolBar(this->toolBar);
        this->addToolBar(toolbarArea, this->toolBar);
    }

    this->ui_StatusBarTimerTimeout = CoreSettingsGetIntValue(SettingsID::GUI_StatusbarMessageDuration);

    this->ui_Widgets->addWidget(this->ui_Widget_RomBrowser);
    this->ui_Widgets->addWidget(this->ui_Widget_Dummy);
    this->ui_Widgets->setCurrentWidget(this->ui_Widget_RomBrowser);

    this->setFocusPolicy(Qt::FocusPolicy::StrongFocus);
    this->installEventFilter(this->ui_EventFilter);
    this->ui_Widget_Dummy->installEventFilter(this->ui_EventFilter);

    this->ui_WindowTitle = this->getWindowTitle();
    this->setWindowTitle(this->ui_WindowTitle);
}

void MainWindow::configureTheme(QApplication* app)
{
    static const QString defaultStyleName = resolveStyleFactoryKey(app->style()->objectName());
    static const QPalette defaultPalette = app->palette();
    static const QPalette defaultStylePalette = resolveStyleStandardPalette(defaultStyleName, defaultPalette);
    static const QString defaultStyleSheet = app->styleSheet();
    static const QString defaultFallbackThemeName = QIcon::themeName();

    // we have to retrieve the fallback icon theme
    // before applying the app theme
    QString fallbackThemeName = defaultFallbackThemeName;

    // set theme style
    QString fallbackStyleSheet = "QTableView { border: none; color: #0096d3; selection-color: #FFFFFF; selection-background-color: #0096d3; }";
    this->setStyleSheet(fallbackStyleSheet);

    app->setStyleSheet(defaultStyleSheet);
    app->setPalette(defaultPalette);

    // set application theme
    QString theme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_Theme));
    if (theme == "Modern" || theme == "Native")
    {
        QStyle* style = QStyleFactory::create(defaultStyleName);
        if (style != nullptr)
        {
#ifdef _WIN32
            style = new NoAccentProxyStyle(style);
#endif
            app->setStyle(style);
        }
        app->setPalette(defaultPalette);
    }

#ifdef _WIN32
    else if (theme == "Windows Vista")
    {
        app->setStyle(new NoAccentProxyStyle(QStyleFactory::create("WindowsVista")));
        app->setPalette(app->style()->standardPalette());
    }
#endif
    else if (theme == "Fusion")
    {
        app->setStyle(QStyleFactory::create("Fusion"));
        app->setPalette(app->style()->standardPalette());
    }
    else if (theme == "Fusion Warm")
    {
        app->setStyle(QStyleFactory::create("Fusion"));
        app->setPalette(defaultStylePalette);
    }
    else if (theme == "Fusion Dark")
    {
        // adapted from https://gist.github.com/QuantumCD/6245215
        app->setStyle(QStyleFactory::create("Fusion"));

        const QColor lighterGray(75, 75, 75);
        const QColor darkGray(53, 53, 53);
        const QColor gray(128, 128, 128);
        const QColor black(25, 25, 25);
        const QColor blue(198, 238, 255);

        QPalette darkPalette;
        darkPalette.setColor(QPalette::Window, darkGray);
        darkPalette.setColor(QPalette::WindowText, Qt::white);
        darkPalette.setColor(QPalette::Base, black);
        darkPalette.setColor(QPalette::AlternateBase, darkGray);
        darkPalette.setColor(QPalette::ToolTipBase, darkGray);
        darkPalette.setColor(QPalette::ToolTipText, Qt::white);
        darkPalette.setColor(QPalette::Text, Qt::white);
        darkPalette.setColor(QPalette::Button, darkGray);
        darkPalette.setColor(QPalette::ButtonText, Qt::white);
        darkPalette.setColor(QPalette::Link, blue);
        darkPalette.setColor(QPalette::Highlight, lighterGray);
        darkPalette.setColor(QPalette::HighlightedText, Qt::white);
        darkPalette.setColor(QPalette::PlaceholderText, QColor(Qt::white).darker());

        darkPalette.setColor(QPalette::Active, QPalette::Button, darkGray);
        darkPalette.setColor(QPalette::Disabled, QPalette::ButtonText, gray);
        darkPalette.setColor(QPalette::Disabled, QPalette::WindowText, gray);
        darkPalette.setColor(QPalette::Disabled, QPalette::Text, gray);
        darkPalette.setColor(QPalette::Disabled, QPalette::Light, darkGray);

        app->setPalette(darkPalette);

        app->setStyleSheet("QToolTip { color: #ffffff; background-color: #2a82da; border: 1px solid white; }");
    }
    else if (theme.endsWith(".qss"))
    {
        QString themePath;
        themePath = QString::fromStdString(CoreGetSharedDataDirectory().string());
        themePath += CORE_DIR_SEPERATOR_STR;
        themePath += "Styles";
        themePath += CORE_DIR_SEPERATOR_STR;
        themePath += theme;

        // use Fusion as a base for the stylesheet
        app->setStyle(QStyleFactory::create("Fusion"));
        app->setPalette(app->style()->standardPalette());

        // set the stylesheet theme,
        // if the file exists and can be opened
        QFile themeFile(themePath);
        if (themeFile.exists() &&
            themeFile.open(QIODevice::ReadOnly))
        {
            app->setStyleSheet(themeFile.readAll());
        }
    }

    // set application icon theme
    QString iconTheme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_IconTheme));
    if (iconTheme == "White" || iconTheme == "Black")
    {
        QIcon::setThemeName(iconTheme.toLower());
    }
    else
    { // fallback to automatic
        QPalette palette = app->palette();
        bool dark = palette.windowText().color().value() > palette.window().color().value();
        QIcon::setThemeName(dark ? "white" : "black");
    }

    // fallback for icons we don't provide (i.e standard system icons)
    QIcon::setFallbackThemeName(fallbackThemeName);
}

void MainWindow::reapplyTheme(void)
{
    QApplication* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (app == nullptr)
    {
        return;
    }

    this->configureTheme(app);

    const QWidgetList widgets = QApplication::allWidgets();
    for (QWidget* widget : widgets)
    {
        if (widget == nullptr)
        {
            continue;
        }

        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
        widget->update();
    }
}

QString MainWindow::getWindowTitle(void)
{
    QString windowTitle = QCoreApplication::applicationName();
    windowTitle += " (";
    windowTitle += QString::fromStdString(CoreGetVersion());
    windowTitle += ")";

    return windowTitle;
}

void MainWindow::showErrorMessage(QString text, QString details, bool force)
{
    // fallback to helper when forced
    if (force)
    {
        QtMessageBox::Error(this, text, details);
        return;
    }

    // update the message box list and ensure
    // that we don't already have one open with
    // the error that we want to display
    for (const QMessageBox* messageBox : this->ui_MessageBoxList)
    {
        if (!messageBox->isVisible())
        {
            this->ui_MessageBoxList.removeOne(messageBox);
            continue;
        }

        if (messageBox->text() == text &&
            messageBox->detailedText() == details)
        {
            return;
        }
    }

    // ensure we only display 10 errors at
    // the same time, to prevent message dialog spam
    if (this->ui_MessageBoxList.size() >= 10)
    {
        return;
    }

    QMessageBox* msgBox = new QMessageBox(this);
    msgBox->setIcon(QMessageBox::Icon::Critical);
    msgBox->setWindowTitle("Error");
    msgBox->setText(text);
    msgBox->setDetailedText(details);
    msgBox->addButton(QMessageBox::Ok);

    // expand details by default
    if (!details.isEmpty())
    {
        for (const auto& button : msgBox->buttons())
        {
            if (msgBox->buttonRole(button) == QMessageBox::ActionRole)
            {
                button->click();
                break;
            }
        }
    }

    msgBox->show();

    this->ui_MessageBoxList.append(msgBox);
}

void MainWindow::checkRaphnetPluginMismatch(void)
{
    // Check if user has previously declined this prompt
    if (CoreSettingsGetBoolValue(SettingsID::GUI_DontAskRaphnetPluginSwitch))
    {
        return;
    }

    // Check if the current input plugin is the generic RMG-Input
    std::string inputPlugin = CoreSettingsGetStringValue(SettingsID::Core_INPUT_Plugin);

    // Only check if using RMG-Input (not raphnetraw or GCA)
    if (inputPlugin.find("RMG-Input") == std::string::npos ||
        inputPlugin.find("raphnetraw") != std::string::npos ||
        inputPlugin.find("GCA") != std::string::npos)
    {
        return;
    }

    // Check each player's configured device name for raphnet 3.0+ adapters
    bool foundRaphnet = false;
    for (int i = 0; i < 4; i++)
    {
        std::string section = "Rosalie's Mupen GUI - Input Plugin Profile " + std::to_string(i);
        std::string deviceName = CoreSettingsGetStringValue(SettingsID::Input_DeviceName, section);

        if (isRaphnet3Plus(deviceName))
        {
            foundRaphnet = true;
            break;
        }
    }

    if (!foundRaphnet)
    {
        return;
    }

    // Show dialog asking user if they want to switch to raphnetraw
    QMessageBox::StandardButton result = QMessageBox::question(
        this,
        tr("raphnet Adapter Detected"),
        tr("A raphnet adapter is configured but you're using the generic input plugin. "
           "Would you like to switch to the raphnetraw plugin? (recommended)"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes
    );

    if (result == QMessageBox::Yes)
    {
#ifdef _WIN32
        CoreSettingsSetValue(SettingsID::Core_INPUT_Plugin, std::string("mupen64plus-input-raphnetraw.dll"));
#else
        CoreSettingsSetValue(SettingsID::Core_INPUT_Plugin, std::string("mupen64plus-input-raphnetraw.so"));
#endif
        CoreSettingsSave();

        if (!CoreApplyPluginSettings())
        {
            this->showErrorMessage("CoreApplyPluginSettings() Failed", QString::fromStdString(CoreGetError()));
        }

        // Update input settings button enabled state
        bool hasInputConfig = CorePluginsHasConfig(CorePluginType::Input) ||
            (isRaphnetRawPlugin() && !CoreIsEmulationRunning());
        this->action_Settings_Input->setEnabled(hasInputConfig);
        this->action_Toolbar_Input->setEnabled(hasInputConfig);
    }
    else
    {
        // User declined, don't ask again
        CoreSettingsSetValue(SettingsID::GUI_DontAskRaphnetPluginSwitch, true);
        CoreSettingsSave();
    }
}

void MainWindow::updateUI(bool inEmulation, bool isPaused)
{
    if (!this->ui_NoSwitchToRomBrowser)
    {
        this->updateActions(inEmulation, isPaused);
    }

    if (inEmulation)
    {
        CoreRomSettings settings;
        CoreGetCurrentRomSettings(settings);

        if (!settings.GoodName.empty())
        {
            QString goodName = QString::fromStdString(settings.GoodName);
            if (goodName.endsWith("(unknown rom)") ||
                goodName.endsWith("(unknown disk)"))
            {
                std::filesystem::path romPath;
                if (CoreGetRomPath(romPath))
                {
                    goodName = QString::fromStdString(romPath.filename().string());
                }
            }

            this->setWindowTitle(goodName + QString(" - ") + this->ui_WindowTitle);
        }

        if (this->ui_VidExtRenderMode == VidExtRenderMode::OpenGL)
        {
            if (QSurfaceFormat::defaultFormat().renderableType() == QSurfaceFormat::OpenGLES)
            {
                this->ui_StatusBar_RenderModeLabel->setText("OpenGL ES");
            }
            else
            {
                this->ui_StatusBar_RenderModeLabel->setText("OpenGL");
            }
            this->ui_Widgets->setCurrentWidget(this->ui_Widget_OpenGL->GetWidget());
        }
        else if (this->ui_VidExtRenderMode == VidExtRenderMode::Vulkan)
        {
            this->ui_StatusBar_RenderModeLabel->setText("Vulkan");
            this->ui_Widgets->setCurrentWidget(this->ui_Widget_Vulkan->GetWidget());
        }
        else
        {
            // when the video extension hasn't been initialized correctly
            // yet, we'll show a dummy widget with a black color pallete
            // to minimize the flicker that would occur when switching
            // from the ROM browser to the render widget when you i.e
            // launch RMG with a ROM on the commandline or drag & drop
            this->ui_Widgets->setCurrentWidget(this->ui_Widget_Dummy);
        }

        this->storeGeometry();
    }
    else if (!this->ui_NoSwitchToRomBrowser)
    {
        this->setWindowTitle(this->ui_WindowTitle);
        this->ui_Widgets->setCurrentWidget(this->ui_Widget_RomBrowser);
        this->ui_StatusBar_RenderModeLabel->clear();
        this->loadGeometry();
    }
    else
    {
        this->ui_NoSwitchToRomBrowser = false;
    }

    // update timer timeout
    this->ui_StatusBarTimerTimeout = CoreSettingsGetIntValue(SettingsID::GUI_StatusbarMessageDuration);
    this->menuRollback->menuAction()->setVisible(CoreSettingsGetBoolValue(SettingsID::Rollback_EnableLocalTesting));
}

void MainWindow::setDebugReplayStatusMessage(const std::string& message)
{
    OnScreenDisplaySetMessage(message);

    if (!this->ui_ShowStatusbar)
    {
        return;
    }

    this->ui_StatusBar_Label->setText(QString::fromStdString(message));

    if (this->ui_ResetStatusBarTimerId != 0)
    {
        this->killTimer(this->ui_ResetStatusBarTimerId);
    }
    this->ui_ResetStatusBarTimerId = this->startTimer(this->ui_StatusBarTimerTimeout * 1000);
}

void MainWindow::storeGeometry(void)
{
    if (this->ui_Geometry_Saved)
    {
        return;
    }

    this->ui_Geometry = this->saveGeometry();
    this->ui_Geometry_Maximized = this->isMaximized();
    this->ui_Geometry_Saved = true;

    std::string geometryStr = this->ui_Geometry.toBase64().toStdString();
    CoreSettingsSetValue(SettingsID::RomBrowser_Geometry, geometryStr);
    CoreSettingsSetValue(SettingsID::RomBrowser_Maximized, this->ui_Geometry_Maximized);
}

void MainWindow::loadGeometry(void)
{
    if (!this->ui_Geometry_Saved)
    {
        return;
    }

    if (this->ui_Geometry_Maximized)
    {
        this->showMaximized();
    }
    else
    {
        this->restoreGeometry(this->ui_Geometry);
    }

    if (this->isFullScreen())
    {
#ifdef _WIN32
        this->restoreDisplayMode();
#endif
        this->showNormal();
    }

    if (this->ui_ShowMenubar && this->menuBar()->isHidden())
    {
        this->menuBar()->show();
    }
    else if (!this->ui_ShowMenubar && !this->menuBar()->isHidden())
    {
        this->menuBar()->hide();
    }

    if (this->ui_ShowToolbar && this->toolBar->isHidden())
    {
        this->toolBar->show();
    }
    else if (!this->ui_ShowToolbar && !this->toolBar->isHidden())
    {
        this->toolBar->hide();
    }

    if (this->ui_ShowStatusbar && this->statusBar()->isHidden())
    {
        this->statusBar()->show();
    }
    else if (!this->ui_ShowStatusbar && !this->statusBar()->isHidden())
    {
        this->statusBar()->hide();
    }

    this->ui_Geometry_Saved = false;
}

void MainWindow::initializeEmulationThread(void)
{
    this->emulationThread = new Thread::EmulationThread(this);
}

void MainWindow::connectEmulationThreadSignals(void)
{
    connect(this->emulationThread, &Thread::EmulationThread::on_Emulation_Finished, this,
            &MainWindow::on_Emulation_Finished);
    connect(this->emulationThread, &Thread::EmulationThread::on_Emulation_Started, this,
            &MainWindow::on_Emulation_Started);

    connect(this->emulationThread, &Thread::EmulationThread::on_VidExt_Init, this, &MainWindow::on_VidExt_Init,
            Qt::BlockingQueuedConnection);
    connect(this->emulationThread, &Thread::EmulationThread::on_VidExt_SetupOGL, this, &MainWindow::on_VidExt_SetupOGL,
            Qt::BlockingQueuedConnection);
    connect(this->emulationThread, &Thread::EmulationThread::on_VidExt_SetWindowedMode, this,
            &MainWindow::on_VidExt_SetWindowedMode, Qt::BlockingQueuedConnection);
    connect(this->emulationThread, &Thread::EmulationThread::on_VidExt_SetFullscreenMode, this,
            &MainWindow::on_VidExt_SetFullscreenMode, Qt::BlockingQueuedConnection);
    connect(this->emulationThread, &Thread::EmulationThread::on_VidExt_ResizeWindow, this,
            &MainWindow::on_VidExt_ResizeWindow, Qt::BlockingQueuedConnection);
    connect(this->emulationThread, &Thread::EmulationThread::on_VidExt_ToggleFS, this, &MainWindow::on_VidExt_ToggleFS,
            Qt::BlockingQueuedConnection);
    connect(this->emulationThread, &Thread::EmulationThread::on_VidExt_Quit, this, &MainWindow::on_VidExt_Quit,
            Qt::BlockingQueuedConnection);
}

void MainWindow::launchEmulationThread(QString cartRom, QString address, int port, int player)
{
    CoreSettingsSave();

    if (this->emulationThread->isRunning())
    {
        this->on_Action_System_Shutdown();

        while (this->emulationThread->isRunning())
        {
            QCoreApplication::processEvents();
        }
    }

    this->emulationThread->SetNetplay(address, port, player);
    this->launchEmulationThread(cartRom, "", false, -1, true);
}

void MainWindow::launchEmulationThread(QString cartRom, QString diskRom, bool refreshRomListAfterEmulation, int slot, bool netplay, bool dragdrop)
{
#ifdef NETPLAY
    if (this->netplaySessionDialog != nullptr && !netplay)
    {
        this->showErrorMessage("EmulationThread::run Failed", "Cannot start emulation when netplay session is active");
        return;
    }

    if (!netplay)
    {
        this->ui_AutoStartNetplayOnStartupPending = false;
    }
#endif // NETPLAY

    if (!dragdrop && this->emulationThread->isRunning())
    {
        this->showErrorMessage("EmulationThread::run Failed", "Cannot start emulation when emulation is already running or being started");
        return;
    }

    CoreSettingsSave();

    if (this->emulationThread->isRunning())
    {
        this->on_Action_System_Shutdown();

        while (this->emulationThread->isRunning())
        {
            QCoreApplication::processEvents();
        }
    }

    this->ui_RefreshRomListAfterEmulation = refreshRomListAfterEmulation || this->ui_Widget_RomBrowser->IsRefreshingRomList();
    if (this->ui_RefreshRomListAfterEmulation)
    {
        this->ui_Widget_RomBrowser->StopRefreshRomList();
    }

    if (!CoreArePluginsReady())
    {
        // always go back to ROM Browser
        this->ui_NoSwitchToRomBrowser = false;
        this->updateUI(false, false);

        this->showErrorMessage("CoreArePluginsReady() Failed", QString::fromStdString(CoreGetError()));
        return;
    }

    const bool startInFullscreen = this->ui_LaunchInFullscreen || CoreSettingsGetBoolValue(SettingsID::GUI_AutomaticFullscreen);
#ifdef _WIN32
    const bool useNativeFullscreenStartup = startInFullscreen &&
        CoreSettingsGetBoolValue(SettingsID::GUI_ExclusiveFullscreen) &&
        CoreSettingsGetBoolValue(SettingsID::GUI_BetaFullscreenBackend);
    qputenv("RMG_GLIDEN64_START_FULLSCREEN", useNativeFullscreenStartup ? "1" : "0");
    if (useNativeFullscreenStartup)
    {
        int nativeWidth = 0;
        int nativeHeight = 0;
        QString resolution = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_ExclusiveFullscreenResolution));
        QStringList resolutionParts = resolution.split('x');
        if (resolutionParts.size() == 2)
        {
            bool widthOk = false;
            bool heightOk = false;
            nativeWidth = resolutionParts.at(0).toInt(&widthOk);
            nativeHeight = resolutionParts.at(1).toInt(&heightOk);
            if (!widthOk || !heightOk)
            {
                nativeWidth = 0;
                nativeHeight = 0;
            }
        }
        if ((nativeWidth <= 0 || nativeHeight <= 0) && this->screen() != nullptr)
        {
            QSize screenSize = this->screen()->geometry().size();
            nativeWidth = screenSize.width();
            nativeHeight = screenSize.height();
        }
        qputenv("RMG_GLIDEN64_START_FULLSCREEN_WIDTH", nativeWidth > 0 ? QByteArray::number(nativeWidth) : "");
        qputenv("RMG_GLIDEN64_START_FULLSCREEN_HEIGHT", nativeHeight > 0 ? QByteArray::number(nativeHeight) : "");
    }
    else
    {
        qputenv("RMG_GLIDEN64_START_FULLSCREEN_WIDTH", "");
        qputenv("RMG_GLIDEN64_START_FULLSCREEN_HEIGHT", "");
    }
#endif

    if (startInFullscreen)
    {
#ifdef _WIN32
        if (!useNativeFullscreenStartup)
#endif
        {
            this->ui_FullscreenTimerId = this->startTimer(100);
        }
        this->ui_LaunchInFullscreen = false;
    }

    this->ui_LoadSaveStateSlotCounter = 0;
    this->ui_LoadSaveStateSlot = slot;
    if (slot != -1)
    {
        this->ui_LoadSaveStateSlotTimerId = this->startTimer(100);
    }

    this->ui_CheckVideoSizeTimerId = this->startTimer(2000);

    this->ui_HideCursorInEmulation = CoreSettingsGetBoolValue(SettingsID::GUI_HideCursorInEmulation);
    this->ui_HideCursorInFullscreenEmulation = CoreSettingsGetBoolValue(SettingsID::GUI_HideCursorInFullscreenEmulation);
#ifdef _WIN32
    this->ui_ExclusiveFullscreen = CoreSettingsGetBoolValue(SettingsID::GUI_ExclusiveFullscreen);
#endif

    if (this->ui_ShowUI)
    {
        this->ui_ShowToolbar = CoreSettingsGetBoolValue(SettingsID::GUI_Toolbar);
        this->ui_ShowStatusbar = CoreSettingsGetBoolValue(SettingsID::GUI_StatusBar);
    }

    this->emulationThread->SetRomFile(cartRom);
    this->emulationThread->SetDiskFile(diskRom);
    this->emulationThread->start();
}

void MainWindow::updateActions(bool inEmulation, bool isPaused)
{
    QString keyBinding;
    bool rollbackDebugReplayIdle;
    bool rollbackDebugReplayReady;
    const bool rollbackDebugReplayFileExists = std::ifstream(kRollbackDebugReplayFilePath, std::ios::binary).good();

    {
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        rollbackDebugReplayIdle = g_RollbackDebugReplay.mode == RollbackDebugReplayMode::Idle;
        rollbackDebugReplayReady = g_RollbackDebugReplay.ready || rollbackDebugReplayFileExists;
    }

    this->menuRollback->menuAction()->setVisible(CoreSettingsGetBoolValue(SettingsID::Rollback_EnableLocalTesting));

    const bool synchronizedNetplayActive = CoreIsSynchronizedNetplayActive();
    const bool blockEmulationPauseForNetplay = this->shouldBlockEmulationPauseForNetplay();
    const bool netplaySessionActive = blockEmulationPauseForNetplay || CoreHasInitKaillera();

    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_StartROM));
    this->action_System_StartRom->setShortcut(QKeySequence(keyBinding));
#ifdef NETPLAY
    this->action_System_StartRom->setEnabled(!inEmulation && this->kailleraSessionManager == nullptr);
#else
    this->action_System_StartRom->setEnabled(!inEmulation);
#endif
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_StartCombo));
    this->action_System_OpenCombo->setShortcut(QKeySequence(keyBinding));
    this->action_System_OpenCombo->setEnabled(!inEmulation);
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_Shutdown));
    this->action_System_Shutdown->setShortcut(QKeySequence(keyBinding));
#ifdef NETPLAY
    this->action_System_Shutdown->setEnabled(inEmulation && !CoreHasInitNetplay());
#else
    this->action_System_Shutdown->setEnabled(inEmulation && !CoreHasInitNetplay());
#endif
    this->menuReset->setEnabled(inEmulation && !netplaySessionActive);
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_SoftReset));
#ifdef NETPLAY
    this->action_Netplay_Start->setEnabled(!inEmulation && this->netplaySessionDialog == nullptr && this->kailleraSessionManager == nullptr);
#else
    this->action_Netplay_Start->setEnabled(inEmulation && !CoreHasInitNetplay() && !CoreHasInitKaillera());
#endif
    this->action_Netplay_Start->setShortcut(QKeySequence(keyBinding));
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_HardReset));
    this->action_System_HardReset->setEnabled(inEmulation && !netplaySessionActive);
    this->action_System_HardReset->setShortcut(QKeySequence(keyBinding));
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_Resume));
    this->action_System_Pause->setChecked(isPaused);
    this->action_System_Pause->setEnabled(inEmulation && !blockEmulationPauseForNetplay);
    this->action_System_Pause->setShortcut(QKeySequence(keyBinding));
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_Screenshot));
    this->action_System_Screenshot->setEnabled(inEmulation);
    this->action_System_Screenshot->setShortcut(QKeySequence(keyBinding));
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_LimitFPS));
    this->action_System_LimitFPS->setEnabled(inEmulation && !synchronizedNetplayActive);
    this->action_System_LimitFPS->setShortcut(QKeySequence(keyBinding));
    this->action_System_LimitFPS->setChecked(CoreIsSpeedLimiterEnabled());
    this->menuSpeedFactor->setEnabled(inEmulation && !synchronizedNetplayActive);
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_SaveState));
    this->action_System_SaveState->setEnabled(inEmulation);
    this->action_System_SaveState->setShortcut(QKeySequence(keyBinding));
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_SaveAs));
    this->action_System_SaveAs->setEnabled(inEmulation);
    this->action_System_SaveAs->setShortcut(QKeySequence(keyBinding));
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_LoadState));
    this->action_System_LoadState->setEnabled(inEmulation && !synchronizedNetplayActive);
    this->action_System_LoadState->setShortcut(QKeySequence(keyBinding));
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_Load));
    this->action_System_Load->setEnabled(inEmulation && !synchronizedNetplayActive);
    this->action_System_Load->setShortcut(QKeySequence(keyBinding));
    this->menuCurrent_Save_State->setEnabled(inEmulation);
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_Cheats));
    this->action_System_Cheats->setEnabled(inEmulation && !synchronizedNetplayActive);
    this->action_System_Cheats->setShortcut(QKeySequence(keyBinding));
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_GSButton));
    this->action_System_GSButton->setEnabled(inEmulation && !synchronizedNetplayActive);
    this->action_System_GSButton->setShortcut(QKeySequence(keyBinding));
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_Exit));
    this->action_System_Exit->setShortcut(QKeySequence(keyBinding));

    // rollback actions
    this->action_Rollback_SaveState->setEnabled(inEmulation && rollbackDebugReplayIdle);
    this->action_Rollback_LoadState->setEnabled(inEmulation && rollbackDebugReplayIdle && this->ui_RollbackDebugState.buffer != nullptr);
    this->action_Rollback_StartDebugReplay->setEnabled(inEmulation && rollbackDebugReplayIdle);
    this->action_Rollback_VerifyDebugReplay->setEnabled(inEmulation && rollbackDebugReplayReady && rollbackDebugReplayIdle);
    this->action_Rollback_VerifyDebugReplayWithGraphics->setEnabled(inEmulation && rollbackDebugReplayReady && rollbackDebugReplayIdle);
    this->action_Rollback_StressDebugReplay->setEnabled(inEmulation && rollbackDebugReplayReady && rollbackDebugReplayIdle);
    this->action_Rollback_ClientInputReplay->setEnabled(inEmulation);
    if (!inEmulation)
    {
        this->action_Rollback_ClientInputReplay->setChecked(false);
    }

    // configure keybindings for speed factor
    QAction* speedActions[] =
    {
        this->actionSpeed25, this->actionSpeed50, this->actionSpeed75,
        this->actionSpeed100, this->actionSpeed125, this->actionSpeed150,
        this->actionSpeed175, this->actionSpeed200, this->actionSpeed225,
        this->actionSpeed250, this->actionSpeed275, this->actionSpeed300
    };
    SettingsID speedKeybindSettingsId[] =
    {
        SettingsID::KeyBinding_SpeedFactor25, SettingsID::KeyBinding_SpeedFactor50,
        SettingsID::KeyBinding_SpeedFactor75, SettingsID::KeyBinding_SpeedFactor100,
        SettingsID::KeyBinding_SpeedFactor125, SettingsID::KeyBinding_SpeedFactor150,
        SettingsID::KeyBinding_SpeedFactor175, SettingsID::KeyBinding_SpeedFactor200,
        SettingsID::KeyBinding_SpeedFactor225, SettingsID::KeyBinding_SpeedFactor250,
        SettingsID::KeyBinding_SpeedFactor275, SettingsID::KeyBinding_SpeedFactor300
    };
    for (int i = 0; i < 12; i++)
    {
        keyBinding = QString::fromStdString(CoreSettingsGetStringValue(speedKeybindSettingsId[i]));
        speedActions[i]->setShortcut(QKeySequence(keyBinding));
    }

    // configure keybindings for save slots
    SettingsID slotKeybindSettingsId[] =
    {
        SettingsID::KeyBinding_SaveStateSlot0, SettingsID::KeyBinding_SaveStateSlot1,
        SettingsID::KeyBinding_SaveStateSlot2, SettingsID::KeyBinding_SaveStateSlot3,
        SettingsID::KeyBinding_SaveStateSlot4, SettingsID::KeyBinding_SaveStateSlot5,
        SettingsID::KeyBinding_SaveStateSlot6, SettingsID::KeyBinding_SaveStateSlot7,
        SettingsID::KeyBinding_SaveStateSlot8, SettingsID::KeyBinding_SaveStateSlot9
    };
    int currentSlot = CoreGetSaveStateSlot();
    for (int i = 0; i < 10; i++)
    {
        keyBinding = QString::fromStdString(CoreSettingsGetStringValue(slotKeybindSettingsId[i]));
        this->ui_SlotActions[i]->setShortcut(QKeySequence(keyBinding));
        this->ui_SlotActions[i]->setChecked(i == currentSlot);
    }

    // configure text and filename data for save slots
    this->updateSaveStateSlotActions(inEmulation, isPaused);

    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_GraphicsSettings));
    this->action_Settings_Graphics->setEnabled(CorePluginsHasConfig(CorePluginType::Gfx));
    this->action_Settings_Graphics->setShortcut(QKeySequence(keyBinding));
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_AudioSettings));
    this->action_Settings_Audio->setEnabled(CorePluginsHasConfig(CorePluginType::Audio));
    this->action_Settings_Audio->setShortcut(QKeySequence(keyBinding));
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_RspSettings));
    this->action_Settings_Rsp->setEnabled(CorePluginsHasConfig(CorePluginType::Rsp));
    this->action_Settings_Rsp->setShortcut(QKeySequence(keyBinding));
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_InputSettings));
    bool hasInputConfig = CorePluginsHasConfig(CorePluginType::Input);
    bool hasRaphnetRawInputTest = isRaphnetRawPlugin() && !inEmulation;
    this->action_Settings_Input->setEnabled(hasInputConfig || hasRaphnetRawInputTest);
    this->action_Settings_Input->setShortcut(QKeySequence(keyBinding));
    this->action_Toolbar_Input->setEnabled(hasInputConfig || hasRaphnetRawInputTest);
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_Settings));
    this->action_Settings_Settings->setShortcut(QKeySequence(keyBinding));

    this->action_View_GameList->setEnabled(!inEmulation);
    this->action_View_GameGrid->setEnabled(!inEmulation);
    this->action_View_UniformSize->setEnabled(!inEmulation);
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_Fullscreen));
    this->action_View_Fullscreen->setEnabled(inEmulation);
    this->action_View_Fullscreen->setShortcut(QKeySequence(keyBinding));
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_RefreshROMList));
    this->action_View_RefreshRoms->setEnabled(!inEmulation);
    this->action_View_RefreshRoms->setShortcut(QKeySequence(keyBinding));
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_ViewLog));
    this->action_View_Log->setShortcut(QKeySequence(keyBinding));
    this->action_View_ClearRomCache->setEnabled(!inEmulation);
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_ViewSearch));
    this->action_View_Search->setShortcut(QKeySequence(keyBinding));
    this->action_View_Search->setEnabled(!inEmulation);

#ifdef NETPLAY
    this->action_Netplay_BrowseSessions->setEnabled(!inEmulation && this->netplaySessionDialog == nullptr && this->kailleraSessionManager == nullptr);
#endif // NETPLAY

    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_IncreaseVolume));
    this->action_Audio_IncreaseVolume->setShortcut(QKeySequence(keyBinding));
    this->action_Audio_IncreaseVolume->setEnabled(inEmulation);
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_DecreaseVolume));
    this->action_Audio_DecreaseVolume->setShortcut(QKeySequence(keyBinding));
    this->action_Audio_DecreaseVolume->setEnabled(inEmulation);
    keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_ToggleMuteVolume));
    this->action_Audio_ToggleVolumeMute->setShortcut(QKeySequence(keyBinding));
    this->action_Audio_ToggleVolumeMute->setEnabled(inEmulation);
}

void MainWindow::updateSaveStateSlotActions(bool inEmulation, bool isPaused)
{
    std::filesystem::path saveStatePath;

    // do nothing when paused
    if (isPaused)
    {
        return;
    }

    for (int i = 0; i < 10; i++)
    {
        if (inEmulation &&
            CoreGetSaveStatePath(i, saveStatePath))
        {
            this->ui_SlotActions[i]->setData(QString::fromStdString(saveStatePath.string()));
            this->ui_SlotActions[i]->setText(this->getSaveStateSlotText(this->ui_SlotActions[i], i));
        }
        else
        {
            this->ui_SlotActions[i]->setData("");
            this->ui_SlotActions[i]->setText(this->getSaveStateSlotText(this->ui_SlotActions[i], i));
        }
    }
}

void MainWindow::addActions(void)
{
    if (this->ui_AddedActions)
    {
        return;
    }

    for (QAction* action : this->ui_Actions)
    {
        this->addAction(action);
    }

    this->ui_AddedActions = true;
}

void MainWindow::removeActions(void)
{
    if (!this->ui_AddedActions)
    {
        return;
    }

    for (QAction* action : this->ui_Actions)
    {
        this->removeAction(action);
    }

    this->ui_AddedActions = false;
}

QString MainWindow::getSaveStateSlotDateTimeText(QAction* action)
{
    QString saveStateSlotText;
    QString filePath;

    // retrieve file name
    filePath = action->data().toString();

    // check if file exists, if it does,
    // return a string with the datetime
    QFileInfo saveStateFileInfo(filePath);
    if (!filePath.isEmpty() && saveStateFileInfo.exists())
    {
        saveStateSlotText = saveStateFileInfo.lastModified().toString("dd/MM/yyyy hh:mm");
    }

    return saveStateSlotText;
}

QString MainWindow::getSaveStateSlotText(QAction* action, int slot)
{
    QString saveStateSlotText;
    QString dateTimeText;
    QString filePath;

    // base text
    saveStateSlotText = "Slot " + QString::number(slot);

    // add date and time text when it isnt empty
    dateTimeText = this->getSaveStateSlotDateTimeText(action);
    if (!dateTimeText.isEmpty())
    {
        saveStateSlotText += " - ";
        saveStateSlotText += dateTimeText;
    }

    return saveStateSlotText;
}

int MainWindow::getToolbarSettingAreaFromArea(Qt::ToolBarArea area)
{
    switch (area)
    {
    case Qt::ToolBarArea::TopToolBarArea:
        return 0;
    case Qt::ToolBarArea::BottomToolBarArea:
        return 1;
    case Qt::ToolBarArea::LeftToolBarArea:
        return 2;
    case Qt::ToolBarArea::RightToolBarArea:
        return 3;

    default:
        return 0;
    }
}

Qt::ToolBarArea MainWindow::getToolbarAreaFromSettingArea(int value)
{
    switch (value)
    {
    default:
    case 0:
        return Qt::ToolBarArea::TopToolBarArea;
    case 1:
        return Qt::ToolBarArea::BottomToolBarArea;
    case 2:
        return Qt::ToolBarArea::LeftToolBarArea;
    case 3:
        return Qt::ToolBarArea::RightToolBarArea;
    }
}

void MainWindow::initializeActions(void)
{
    // Audio actions
    this->action_Audio_IncreaseVolume   = new QAction(this);
    this->action_Audio_DecreaseVolume   = new QAction(this);
    this->action_Audio_ToggleVolumeMute = new QAction(this);
    this->action_Audio_IncreaseVolume->setEnabled(false);
    this->action_Audio_DecreaseVolume->setEnabled(false);
    this->action_Audio_ToggleVolumeMute->setEnabled(false);

    // because these are hotkey exclusive actions,
    // we only have to add them once
    this->addAction(this->action_Audio_IncreaseVolume);
    this->addAction(this->action_Audio_DecreaseVolume);
    this->addAction(this->action_Audio_ToggleVolumeMute);
}


void MainWindow::configureActions(void)
{
    // configure actions list
    this->ui_Actions.append(
    {
        // System actions
        this->action_System_StartRom, this->action_System_OpenCombo,
        this->action_System_Shutdown, this->action_Netplay_Start,
        this->action_System_HardReset, this->action_System_Pause,
        this->action_System_Screenshot, this->action_System_LimitFPS,
        this->actionSpeed25, this->actionSpeed50, this->actionSpeed75,
        this->actionSpeed100, this->actionSpeed125, this->actionSpeed150,
        this->actionSpeed175, this->actionSpeed200, this->actionSpeed225,
        this->actionSpeed250, this->actionSpeed275, this->actionSpeed300,
        this->action_System_SaveState, this->action_System_SaveAs,
        this->action_System_LoadState, this->action_System_Load,
        this->actionSlot_0, this->actionSlot_1, this->actionSlot_2,
        this->actionSlot_3, this->actionSlot_4, this->actionSlot_5,
        this->actionSlot_6, this->actionSlot_7, this->actionSlot_8,
        this->actionSlot_9, this->action_System_Cheats,
        this->action_System_GSButton, this->action_System_Exit,
        this->action_Rollback_SaveState, this->action_Rollback_LoadState,
        this->action_Rollback_StartDebugReplay, this->action_Rollback_VerifyDebugReplay,
        this->action_Rollback_VerifyDebugReplayWithGraphics, this->action_Rollback_StressDebugReplay,
        this->action_Rollback_ClientInputReplay,
        // Settings actions
        this->action_Settings_Graphics, this->action_Settings_Audio,
        this->action_Settings_Rsp, this->action_Settings_Input,
        this->action_Settings_Settings,
        // View actions
        this->action_View_Fullscreen, this->action_View_RefreshRoms,
        this->action_View_Log,
        // Help actions
        this->action_Help_Github, this->action_Help_About,
    });

    // configure save slot actions
    this->ui_SlotActions.append(
    {
        this->actionSlot_0, this->actionSlot_1, this->actionSlot_2,
        this->actionSlot_3, this->actionSlot_4, this->actionSlot_5,
        this->actionSlot_6, this->actionSlot_7, this->actionSlot_8,
        this->actionSlot_9
    });

    // configure emulation speed actions
    QActionGroup* speedActionGroup = new QActionGroup(this);
    QAction* speedActions[] =
    {
        this->actionSpeed25, this->actionSpeed50, this->actionSpeed75,
        this->actionSpeed100, this->actionSpeed125, this->actionSpeed150,
        this->actionSpeed175, this->actionSpeed200, this->actionSpeed225,
        this->actionSpeed250, this->actionSpeed275, this->actionSpeed300
    };
    int speedActionNumbers[] =
    {
        25, 50, 75, 100,
        125, 150, 175, 200,
        225, 250, 275, 300
    };
    int currentSpeedFactor = CoreGetSpeedFactor();
    for (int i = 0; i < 12; i++)
    {
        QAction* speedAction = speedActions[i];
        int speedActionNumber = speedActionNumbers[i];

        speedAction->setCheckable(true);
        speedAction->setChecked(currentSpeedFactor == speedActionNumber);
        speedAction->setActionGroup(speedActionGroup);

        // connect emulation speed action here because we need to do
        // something special for them
        connect(speedAction, &QAction::triggered, [this, speedActionNumber](bool checked)
        {
            if (checked)
            {
                this->on_Action_System_SpeedFactor(speedActionNumber);
            }
        });
    }

    // configure save slot actions
    QActionGroup* slotActionGroup = new QActionGroup(this);
    for (int i = 0; i < 10; i++)
    {
        QAction* slotAction = this->ui_SlotActions[i];

        slotAction->setCheckable(true);
        slotAction->setChecked(i == 0);
        slotAction->setActionGroup(slotActionGroup);

        // connect slot action here because we need to do
        // something special for them
        connect(slotAction, &QAction::triggered, [this, i](bool checked)
        {
            if (checked)
            {
                this->on_Action_System_CurrentSaveState(i);
            }
        });
    }

    // configure toolbar & statusbar actions
    this->action_View_Toolbar->setChecked(CoreSettingsGetBoolValue(SettingsID::GUI_Toolbar));
    this->action_View_StatusBar->setChecked(CoreSettingsGetBoolValue(SettingsID::GUI_StatusBar));

    // configure ROM browser view actions
    QActionGroup* romBrowserViewActionGroup = new QActionGroup(this);
    int currentView = CoreSettingsGetIntValue(SettingsID::RomBrowser_ViewMode);
    QAction* romBrowserViewActions[] =
    {
        this->action_View_GameList,
        this->action_View_GameGrid
    };
    for (int i = 0; i < 2; i++)
    {
        QAction* action = romBrowserViewActions[i];

        action->setCheckable(true);
        action->setChecked(i == currentView);
        action->setActionGroup(romBrowserViewActionGroup);
    }

    // configure grid view options actions
    this->action_View_UniformSize->setChecked(CoreSettingsGetBoolValue(SettingsID::RomBrowser_GridViewUniformItemSizes));
}

void MainWindow::connectActionSignals(void)
{
    connect(this->action_System_StartRom, &QAction::triggered, this, &MainWindow::on_Action_System_OpenRom);
    connect(this->action_System_OpenCombo, &QAction::triggered, this, &MainWindow::on_Action_System_OpenCombo);
    connect(this->action_System_Exit, &QAction::triggered, this, &MainWindow::on_Action_System_Exit);

    connect(this->action_System_Shutdown, &QAction::triggered, this, &MainWindow::on_Action_System_Shutdown);
#ifdef NETPLAY
    connect(this->action_Netplay_Start, &QAction::triggered, this, &MainWindow::on_Action_Netplay_BrowseSessions);
#else
    connect(this->action_Netplay_Start, &QAction::triggered, this, &MainWindow::on_Action_System_SoftReset);
#endif
    connect(this->action_System_HardReset, &QAction::triggered, this, &MainWindow::on_Action_System_HardReset);
    connect(this->action_System_Pause, &QAction::triggered, this, &MainWindow::on_Action_System_Pause);
    connect(this->action_System_Screenshot, &QAction::triggered, this,
            &MainWindow::on_Action_System_Screenshot);
    connect(this->action_System_LimitFPS, &QAction::triggered, this, &MainWindow::on_Action_System_LimitFPS);
    connect(this->action_System_SaveState, &QAction::triggered, this, &MainWindow::on_Action_System_SaveState);
    connect(this->action_System_SaveAs, &QAction::triggered, this, &MainWindow::on_Action_System_SaveAs);
    connect(this->action_System_LoadState, &QAction::triggered, this, &MainWindow::on_Action_System_LoadState);
    connect(this->action_System_Load, &QAction::triggered, this, &MainWindow::on_Action_System_Load);
    connect(this->action_System_Cheats, &QAction::triggered, this, &MainWindow::on_Action_System_Cheats);
    connect(this->action_System_GSButton, &QAction::triggered, this, &MainWindow::on_Action_System_GSButton);

    connect(this->action_Rollback_SaveState, &QAction::triggered, this, &MainWindow::on_Action_Rollback_SaveState);
    connect(this->action_Rollback_LoadState, &QAction::triggered, this, &MainWindow::on_Action_Rollback_LoadState);
    connect(this->action_Rollback_StartDebugReplay, &QAction::triggered, this, &MainWindow::on_Action_Rollback_StartDebugReplay);
    connect(this->action_Rollback_VerifyDebugReplay, &QAction::triggered, this, &MainWindow::on_Action_Rollback_VerifyDebugReplay);
    connect(this->action_Rollback_VerifyDebugReplayWithGraphics, &QAction::triggered, this, &MainWindow::on_Action_Rollback_VerifyDebugReplayWithGraphics);
    connect(this->action_Rollback_StressDebugReplay, &QAction::triggered, this, &MainWindow::on_Action_Rollback_StressDebugReplay);
    connect(this->action_Rollback_ClientInputReplay, &QAction::triggered, this, &MainWindow::on_Action_Rollback_ClientInputReplay);

    connect(this->action_Settings_Graphics, &QAction::triggered, this, &MainWindow::on_Action_Settings_Graphics);
    connect(this->action_Settings_Audio, &QAction::triggered, this, &MainWindow::on_Action_Settings_Audio);
    connect(this->action_Settings_Rsp, &QAction::triggered, this, &MainWindow::on_Action_Settings_Rsp);
    connect(this->action_Settings_Input, &QAction::triggered, this,
            &MainWindow::on_Action_Settings_Input);
    connect(this->action_Settings_Settings, &QAction::triggered, this, &MainWindow::on_Action_Settings_Settings);
    connect(this->action_Settings_Plugins, &QAction::triggered, this, &MainWindow::on_Action_Settings_Plugins);
    connect(this->action_Toolbar_Input, &QAction::triggered, this, &MainWindow::on_Action_Settings_Input);
    connect(this->action_Toolbar_Playback, &QAction::triggered, this, &MainWindow::on_Action_Playback);

    connect(this->action_View_Toolbar, &QAction::toggled, this, &MainWindow::on_Action_View_Toolbar);
    connect(this->action_View_StatusBar, &QAction::toggled, this, &MainWindow::on_Action_View_StatusBar);
    connect(this->action_View_GameList, &QAction::toggled, this, &MainWindow::on_Action_View_GameList);
    connect(this->action_View_GameGrid, &QAction::toggled, this, &MainWindow::on_Action_View_GameGrid);
    connect(this->action_View_UniformSize, &QAction::toggled, this, &MainWindow::on_Action_View_UniformSize);
    connect(this->action_View_Fullscreen, &QAction::triggered, this, &MainWindow::on_Action_View_Fullscreen);
    connect(this->action_View_RefreshRoms, &QAction::triggered, this, &MainWindow::on_Action_View_RefreshRoms);
    connect(this->action_View_ClearRomCache, &QAction::triggered, this, &MainWindow::on_Action_View_ClearRomCache);
    connect(this->action_View_Log, &QAction::triggered, this, &MainWindow::on_Action_View_Log);
    connect(this->action_View_Search, &QAction::triggered, this, &MainWindow::on_Action_View_Search);

    connect(this->action_Netplay_BrowseSessions, &QAction::triggered, this, &MainWindow::on_Action_Netplay_BrowseSessions);

    connect(this->action_Help_Github, &QAction::triggered, this, &MainWindow::on_Action_Help_Github);
    connect(this->action_Help_About, &QAction::triggered, this, &MainWindow::on_Action_Help_About);
    connect(this->action_Help_Update, &QAction::triggered, this, &MainWindow::on_Action_Help_Update);

    connect(this->action_Audio_IncreaseVolume, &QAction::triggered, this, &MainWindow::on_Action_Audio_IncreaseVolume);
    connect(this->action_Audio_DecreaseVolume, &QAction::triggered, this, &MainWindow::on_Action_Audio_DecreaseVolume);
    connect(this->action_Audio_ToggleVolumeMute, &QAction::triggered, this, &MainWindow::on_Action_Audio_ToggleVolumeMute);
}

#ifdef UPDATER
void MainWindow::checkForUpdates(bool silent, bool force)
{
    if (!force && !CoreSettingsGetBoolValue(SettingsID::GUI_CheckForUpdates))
    {
        return;
    }

    // only check for updates on stable versions unless forced
    QString currentVersion = QString::fromStdString(CoreGetVersion());
    if (!force)
    {
        static const QRegularExpression stableVersionRegex("^v?\\d+(?:\\.\\d+){0,2}$");
        if (!stableVersionRegex.match(currentVersion).hasMatch())
        {
            return;
        }
    }

    QString dateTimeFormat = "dd-MM-yyyy_hh:mm";
    QString lastUpdateCheckDateTimeString = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_LastUpdateCheck));
    QDateTime lastUpdateCheckDateTime = QDateTime::fromString(lastUpdateCheckDateTimeString, dateTimeFormat);
    QDateTime currentDateTime = QDateTime::currentDateTime();

    // only check for updates once every hour unless forced
    if (!force &&
        lastUpdateCheckDateTime.isValid() &&
        lastUpdateCheckDateTime.addSecs(3600) > currentDateTime)
    {
        return;
    }

    // update last update check date & time
    CoreSettingsSetValue(SettingsID::GUI_LastUpdateCheck, currentDateTime.toString(dateTimeFormat).toStdString());

    // whether or not the update check is silent
    this->ui_SilentUpdateCheck = silent;

    // execute update check
    QNetworkAccessManager* networkAccessManager = new QNetworkAccessManager(this);
    connect(networkAccessManager, &QNetworkAccessManager::finished, this, &MainWindow::on_networkAccessManager_Finished);
    networkAccessManager->setTransferTimeout(15000);
    networkAccessManager->get(QNetworkRequest(QUrl("https://api.github.com/repos/Jay-Day/RMG-K/releases/latest")));
}
#endif // UPDATER

#ifdef NETPLAY
void MainWindow::showNetplaySessionDialog(QWebSocket* webSocket, QJsonObject json, QString sessionFile)
{
    if (this->netplaySessionDialog != nullptr)
    {
        this->netplaySessionDialog->deleteLater();
        this->netplaySessionDialog = nullptr;
    }

    this->netplaySessionDialog = new Dialog::NetplaySessionDialog(nullptr, webSocket, json, sessionFile);
    connect(this->netplaySessionDialog, &Dialog::NetplaySessionDialog::OnPlayGame, this, &MainWindow::on_Netplay_PlayGame);
    connect(this->netplaySessionDialog, &Dialog::NetplaySessionDialog::rejected, this, &MainWindow::on_NetplaySessionDialog_rejected);
    this->netplaySessionDialog->show();

    // force refresh of actions
    this->updateActions(false, false);
}

void MainWindow::tryAutoStartNetplayOnStartup(void)
{
    if (!this->ui_AutoStartNetplayOnStartupPending)
    {
        return;
    }

    if (this->ui_Widget_RomBrowser != nullptr &&
        this->ui_Widget_RomBrowser->IsRefreshingRomList())
    {
        return;
    }

    if (this->kailleraSessionManager != nullptr)
    {
        this->ui_AutoStartNetplayOnStartupPending = false;
        return;
    }

    this->ui_AutoStartNetplayOnStartupPending = false;
    QTimer::singleShot(0, this, [this]() {
        this->on_Action_Netplay_BrowseSessions();
    });
}

void MainWindow::refreshKailleraRecordingStorageStatus(bool showStartupWarning)
{
    const bool overCap = CoreRefreshKailleraRecordingStorageStatus();

    if (!showStartupWarning || !overCap || !this->ui_ShowStatusbar)
    {
        return;
    }

    int capMB = CoreSettingsGetIntValue(SettingsID::Kaillera_RecordingCapMB);
    if (capMB < 1)
    {
        capMB = 1;
    }

    const uint64_t bytes = CoreGetKailleraRecordingStorageBytes();
    QString usageText;
    if (bytes >= (1024ULL * 1024ULL * 1024ULL))
    {
        const double gib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
        usageText = QString::number(gib, 'f', 2) + " GB";
    }
    else
    {
        const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
        usageText = QString::number(mib, 'f', 1) + " MB";
    }

    this->ui_StatusBar_Label->setText(
        QString("Kaillera recordings folder is over cap (%1 used / %2 MB cap). Record game defaults to off.")
            .arg(usageText)
            .arg(capMB));
}

void MainWindow::on_RomBrowser_RomListRefreshFinished(bool canceled)
{
    if (canceled)
    {
        return;
    }

    this->tryAutoStartNetplayOnStartup();
}
#endif // NETPLAY

void MainWindow::timerEvent(QTimerEvent *event)
{
    int timerId = event->timerId();

    if (timerId == this->ui_ResetStatusBarTimerId)
    {
        this->ui_StatusBar_Label->clear();
    }
    else if (timerId == this->ui_FullscreenTimerId)
    {
        // only try to go to fullscreen
        // when emulation is running
        if (!CoreIsEmulationRunning())
        {
            return;
        }

        // we're finished when we're in fullscreen already,
        // or when switching to fullscreen succeeds
        if (this->isFullScreen() || CoreToggleFullscreen())
        {
            this->killTimer(timerId);
            this->ui_FullscreenTimerId = 0;
        }
    }
    else if (timerId == this->ui_GamesharkButtonTimerId)
    {
        if (!CoreIsEmulationRunning())
        {
            return;
        }

        if (CorePressGamesharkButton(false))
        {
            this->killTimer(timerId);
            this->ui_GamesharkButtonTimerId = 0;
        }
    }
    else if (timerId == this->ui_UpdateSaveStateSlotTimerId)
    {
        this->updateSaveStateSlotActions(CoreIsEmulationRunning(), false);
        this->killTimer(timerId);
        this->ui_UpdateSaveStateSlotTimerId = 0;
    }
    else if (timerId == this->ui_CheckVideoSizeTimerId)
    {
#ifdef NETPLAY
        if (this->ui_RollbackLivePumpPending)
        {
            return;
        }
#endif // NETPLAY

        if (!CoreIsEmulationRunning())
        {
            return;
        }

        int width  = 0;
        int height = 0;
        if (!CoreGetVideoSize(width, height))
        {
            return;
        }

        int expectedWidth  = 0;
        int expectedHeight = 0;
        if (this->ui_VidExtRenderMode == VidExtRenderMode::OpenGL)
        {
            expectedWidth  = this->ui_Widget_OpenGL->GetWidget()->width()  * this->devicePixelRatio();
            expectedHeight = this->ui_Widget_OpenGL->GetWidget()->height() * this->devicePixelRatio();
        }
        else if (this->ui_VidExtRenderMode == VidExtRenderMode::Vulkan)
        {
            expectedWidth  = this->ui_Widget_Vulkan->GetWidget()->width()  * this->devicePixelRatio();
            expectedHeight = this->ui_Widget_Vulkan->GetWidget()->height() * this->devicePixelRatio();
        }

        if (width  != expectedWidth ||
            height != expectedHeight)
        {
            CoreSetVideoSize(expectedWidth, expectedHeight);
        }
    }
    else if (timerId == this->ui_LoadSaveStateSlotTimerId)
    {
        if (!CoreIsEmulationRunning())
        {
            return;
        }

        if (!CoreSetSaveStateSlot(this->ui_LoadSaveStateSlot) ||
            !CoreLoadSaveState())
        {
            this->ui_LoadSaveStateSlotCounter++;
            if (this->ui_LoadSaveStateSlotCounter >= 5)
            { // give up after 5 attempts
                this->killTimer(this->ui_LoadSaveStateSlotTimerId);
                this->ui_LoadSaveStateSlotCounter = 0;
                this->ui_LoadSaveStateSlotTimerId = -1;
                this->ui_LoadSaveStateSlot        = -1;
            }
            return;
        }

        this->killTimer(this->ui_LoadSaveStateSlotTimerId);
    }
#ifdef NETPLAY
    else if (timerId == this->ui_RollbackLivePumpTimerId)
    {
        this->killTimer(timerId);
        this->ui_RollbackLivePumpTimerId = 0;
    }
#endif // NETPLAY
}

void MainWindow::on_EventFilter_KeyPressed(QKeyEvent *event)
{
#ifdef NETPLAY
    if (this->handleNetplayChatKeyPress(event))
    {
        return;
    }
#endif // NETPLAY

    if (!CoreIsEmulationRunning())
    {
        QMainWindow::keyPressEvent(event);
        return;
    }

    int key = Utilities::QtKeyToSdl3Key(event->key());
    int mod = Utilities::QtModKeyToSdl3ModKey(event->modifiers());

    CoreSetKeyDown(key, mod);
}

void MainWindow::on_EventFilter_KeyReleased(QKeyEvent *event)
{
#ifdef NETPLAY
    if (this->ui_NetplayChatInputActive)
    {
        return;
    }
#endif // NETPLAY

    if (!CoreIsEmulationRunning())
    {
        QMainWindow::keyReleaseEvent(event);
        return;
    }

    int key = Utilities::QtKeyToSdl3Key(event->key());
    int mod = Utilities::QtModKeyToSdl3ModKey(event->modifiers());

    CoreSetKeyUp(key, mod);
}

void MainWindow::on_EventFilter_FileDropped(QDropEvent *event)
{
    const QMimeData *mimeData = event->mimeData();

    QList<QUrl> urls = mimeData->urls();
#ifdef KCA_DRAG_DROP
    urls = KUrlMimeData::urlsFromMimeData(mimeData, KUrlMimeData::PreferLocalUrls);
#endif

    if (!mimeData->hasUrls() || urls.empty() ||
        !urls.first().isLocalFile())
    {
        return;
    }

    bool inEmulation     = (this->ui_Widgets->currentIndex() != 0);
    bool confirmDragDrop = CoreSettingsGetBoolValue(SettingsID::GUI_ConfirmDragDrop);
    bool refreshRomList  = false;
    QString file;

    // when we're still opening the ROM while emulation is running,
    // ignore the event
    if (this->emulationThread->isRunning() && !CoreHasRomOpen())
    {
        return;
    }

    if (inEmulation && confirmDragDrop)
    {
        confirmDragDrop = false;
        bool ret = QtMessageBox::Question(this, "Are you sure you want to launch the drag & dropped ROM?",
                                                "Don't ask for confirmation again", confirmDragDrop);
        if (!ret)
        {
            return;
        }

        CoreSettingsSetValue(SettingsID::GUI_ConfirmDragDrop, !confirmDragDrop);
    }

    file = urls.first().toLocalFile();

    if (inEmulation)
    {
        this->ui_NoSwitchToRomBrowser = true;
        // we have to keep the state of this->ui_RefreshRomListAfterEmulation,
        // because when that's reset on every launch, and because when
        // RMG is launched with a ROM specified, the ROM browser
        // hasn't loaded yet, we should pass on the state to ensure
        // it does refresh when you do the following:
        // launch RMG with a ROM -> drag & drop -> return to ROM browser
        refreshRomList = this->ui_RefreshRomListAfterEmulation;
    }

    this->launchEmulationThread(file, "", refreshRomList, -1, false, true);
}

void MainWindow::on_QGuiApplication_applicationStateChanged(Qt::ApplicationState state)
{
    bool isRunning = CoreIsEmulationRunning();
    bool isPaused = CoreIsEmulationPaused();

    bool pauseOnFocusLoss = CoreSettingsGetBoolValue(SettingsID::GUI_PauseEmulationOnFocusLoss);
    bool resumeOnFocus = CoreSettingsGetBoolValue(SettingsID::GUI_ResumeEmulationOnFocus);
    bool blockEmulationPauseForNetplay = this->shouldBlockEmulationPauseForNetplay();

    switch (state)
    {
        default:
            break;

        case Qt::ApplicationState::ApplicationInactive:
        {
            if (pauseOnFocusLoss && isRunning && !isPaused && !blockEmulationPauseForNetplay)
            {
                if (CorePauseEmulation())
                {
                    this->ui_FocusPausedEmulation = true;
                    this->updateUI(true, true);
                }
            }
        } break;

        case Qt::ApplicationState::ApplicationActive:
        {
            if (resumeOnFocus && isPaused && this->ui_FocusPausedEmulation)
            {
                if (!blockEmulationPauseForNetplay && CoreResumeEmulation())
                {
                    this->ui_FocusPausedEmulation = false;
                    this->updateUI(true, false);
                }
            }

            if (!resumeOnFocus || !CoreIsEmulationPaused() || blockEmulationPauseForNetplay)
            {
                this->ui_FocusPausedEmulation = false;
            }
        } break;
    }
}

bool MainWindow::shouldBlockEmulationPauseForNetplay(void) const
{
    if (CoreIsSynchronizedNetplayActive())
    {
        return true;
    }

#ifdef NETPLAY
    if (this->kailleraSessionManager != nullptr || this->netplaySessionDialog != nullptr ||
        this->ui_RollbackNetplayRoomActive || this->ui_RollbackNetplayLaunchActive)
    {
        return true;
    }
#endif // NETPLAY

    return false;
}

#ifdef UPDATER

namespace
{
    // Parse a semver-ish version tag ("0.9.3", "v0.9.3") into numeric
    // parts. Returns an empty vector for inputs that aren't pure
    // dot-separated integers (e.g. "vdev-100", "0.9.3-rc1"); callers
    // treat empty as "newer than any released version" so dev builds
    // never get prompted to downgrade.
    QVector<int> parseSemverParts(const QString& in)
    {
        QString s = in.trimmed();
        if (s.startsWith('v') || s.startsWith('V')) s.remove(0, 1);
        if (s.isEmpty()) return {};

        const QStringList parts = s.split('.');
        QVector<int> out;
        out.reserve(parts.size());
        for (const QString& p : parts)
        {
            bool ok = false;
            const int v = p.toInt(&ok);
            if (!ok) return {};
            out.append(v);
        }
        return out;
    }

    // Returns -1 if a < b, 0 if a == b, +1 if a > b. Treat an empty
    // parts vector as "+inf" so unparseable dev builds compare as
    // newer than anything tagged.
    int compareSemver(const QVector<int>& a, const QVector<int>& b)
    {
        if (a.isEmpty() && b.isEmpty()) return 0;
        if (a.isEmpty()) return 1;
        if (b.isEmpty()) return -1;
        const int n = std::max(a.size(), b.size());
        for (int i = 0; i < n; ++i)
        {
            const int av = (i < a.size()) ? a[i] : 0;
            const int bv = (i < b.size()) ? b[i] : 0;
            if (av < bv) return -1;
            if (av > bv) return 1;
        }
        return 0;
    }
}

void MainWindow::on_networkAccessManager_Finished(QNetworkReply* reply)
{
    if (reply->error())
    {
        if (!this->ui_SilentUpdateCheck)
        {
            this->showErrorMessage("Failed to check for updates", reply->errorString());
        }
        reply->deleteLater();
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    QJsonObject jsonObject = jsonDocument.object();

    QString currentVersion = QString::fromStdString(CoreGetVersion());
    QString latestVersion = jsonObject.value("tag_name").toString();

    reply->deleteLater();

    // GitHub's /releases/latest endpoint only returns the latest
    // *non-prerelease*. If we're running a pre-release tag (e.g. 0.9.3
    // marked prerelease on GitHub), the API hands back the previous
    // stable (0.9.2). A naive string compare then prompts the user to
    // "update" to an older version. Compare semantically and only
    // prompt when latest is genuinely newer.
    const QVector<int> currentParts = parseSemverParts(currentVersion);
    const QVector<int> latestParts  = parseSemverParts(latestVersion);
    if (compareSemver(currentParts, latestParts) >= 0)
    {
        if (!this->ui_SilentUpdateCheck)
        {
            Utilities::QtMessageBox::Info(this, "You're already on the latest version");
        }
        return;
    }

    int ret = 0;

    Dialog::UpdateDialog updateDialog(this, jsonObject, !this->ui_SilentUpdateCheck);
    ret = updateDialog.exec();
    if (ret != QDialog::Accepted)
    {
        return;
    }

    Dialog::DownloadUpdateDialog downloadUpdateDialog(this, updateDialog.GetUrl(), updateDialog.GetFileName());
    ret = downloadUpdateDialog.exec();
    if (ret != QDialog::Accepted)
    {
        return;
    }

#ifdef APPIMAGE_UPDATER
    this->ui_ForceClose = true;
    this->close();
#else // normal updater
    Dialog::InstallUpdateDialog installUpdateDialog(this, QCoreApplication::applicationDirPath(), downloadUpdateDialog.GetTempDirectory(), downloadUpdateDialog.GetFileName());
    ret = installUpdateDialog.exec();
    if (ret != QDialog::Accepted)
    {
        return;
    }
#endif // APPIMAGE_UPDATER
}
#endif // UPDATER

void MainWindow::on_Action_System_OpenRom(void)
{
    bool isRunning = CoreIsEmulationRunning();
    bool isPaused = CoreIsEmulationPaused();

    if (isRunning && !isPaused)
    {
        this->on_Action_System_Pause();
    }

    QString romFile = QFileDialog::getOpenFileName(this, tr("Open N64 ROM or 64DD Disk"), "", "N64 ROMs & Disks (*.n64 *.z64 *.v64 *.ndd *.d64 *.zip *.7z)");
    if (romFile.isEmpty())
    {
        if (isRunning && !isPaused)
        {
            this->on_Action_System_Pause();
        }
        return;
    }

    if (this->ui_Widgets->currentIndex() != 0)
    {
        this->ui_NoSwitchToRomBrowser = true;
    }

    this->launchEmulationThread(romFile);
}

void MainWindow::on_Action_System_OpenCombo(void)
{
    bool isRunning = CoreIsEmulationRunning();
    bool isPaused = CoreIsEmulationPaused();

    if (isRunning && !isPaused)
    {
        this->on_Action_System_Pause();
    }

    QString cartRom = QFileDialog::getOpenFileName(this, tr("Open N64 ROM"), "", "N64 ROMs (*.n64 *.z64 *.v64 *.zip *.7z)");
    if (cartRom.isEmpty())
    {
        if (isRunning && !isPaused)
        {
            this->on_Action_System_Pause();
        }
        return;
    }


    QString diskRom = QFileDialog::getOpenFileName(this, tr("Open 64DD Disk"), "", "N64DD Disk Image (*.ndd *.d64 *.zip *.7z)");
    if (diskRom.isEmpty())
    {
        if (isRunning && !isPaused)
        {
            this->on_Action_System_Pause();
        }
        return;
    }

    if (this->ui_Widgets->currentIndex() != 0)
    {
        this->ui_NoSwitchToRomBrowser = true;
    }

    this->launchEmulationThread(cartRom, diskRom);
}

void MainWindow::on_Action_System_Shutdown(void)
{
    if (CoreIsEmulationPaused())
    {
        this->on_Action_System_Pause();
    }

    if (!CoreIsEmulationRunning())
    {
        return;
    }

#ifdef NETPLAY
    if (this->kailleraSessionManager != nullptr && this->kailleraSessionManager->isGameActive())
    {
        this->kailleraSessionManager->endGame();
        return;
    }
    if (CoreHasInitKaillera())
    {
        CoreEndKailleraGame();
    }
#endif

    if (!CoreStopEmulation())
    {
        this->showErrorMessage("CoreStopEmulation() Failed", QString::fromStdString(CoreGetError()));
    }
}

void MainWindow::on_Action_System_Exit(void)
{
    this->close();
}

void MainWindow::on_Action_System_SoftReset(void)
{
    if (!CoreResetEmulation(false))
    {
        this->showErrorMessage("CoreResetEmulation() Failed", QString::fromStdString(CoreGetError()));
    }
}

void MainWindow::on_Action_System_HardReset(void)
{
    if (!CoreResetEmulation(true))
    {
        this->showErrorMessage("CoreResetEmulation() Failed", QString::fromStdString(CoreGetError()));
    }
}
void MainWindow::on_Action_System_Pause(void)
{
    if (!this->action_System_Pause->isEnabled())
    {
        return;
    }

    bool isPaused = CoreIsEmulationPaused();

    bool ret;
    QString error;

    if (!isPaused)
    {
        ret = CorePauseEmulation();
        error = "CorePauseEmulation() Failed";
    }
    else
    {
        ret = CoreResumeEmulation();
        error = "CoreResumeEmulation() Failed";
    }

    if (!ret)
    {
        this->showErrorMessage(error, QString::fromStdString(CoreGetError()));
    }

    this->updateUI(true, (!isPaused && ret));
    if (ret)
    {
        this->ui_FocusPausedEmulation = false;
    }
}

void MainWindow::on_Action_System_Screenshot(void)
{
    if (!CoreTakeScreenshot())
    {
        this->showErrorMessage("CoreTakeScreenshot() Failed", QString::fromStdString(CoreGetError()));
    }
}

void MainWindow::on_Action_System_LimitFPS(void)
{
    bool enabled = this->action_System_LimitFPS->isChecked();

    if (!CoreSetSpeedLimiterState(enabled))
    {
        this->showErrorMessage("CoreSetSpeedLimiterState() Failed", QString::fromStdString(CoreGetError()));
    }
}

void MainWindow::on_Action_System_SpeedFactor(int factor)
{
    if (!CoreSetSpeedFactor(factor))
    {
        this->showErrorMessage("CoreSetSpeedFactor() Failed", QString::fromStdString(CoreGetError()));
    }
}

void MainWindow::on_Action_System_SaveState(void)
{
    this->ui_ManuallySavedState = true;

    if (!CoreSaveState())
    {
        this->ui_ManuallySavedState = false;
        this->showErrorMessage("CoreSaveState() Failed", QString::fromStdString(CoreGetError()));
    }
    else
    {
        OnScreenDisplaySetMessage("Saved state to slot: " + std::to_string(CoreGetSaveStateSlot()));
    }
}

void MainWindow::on_Action_System_SaveAs(void)
{
    bool isRunning = CoreIsEmulationRunning();
    bool isPaused = CoreIsEmulationPaused();

    if (isRunning && !isPaused)
    {
        this->on_Action_System_Pause();
    }

    QString fileName = QFileDialog::getSaveFileName(this, tr("Save State"), "", tr("Save State (*.state);;Project64 Save State (*.pj);;All Files (*)"));
    if (!fileName.isEmpty())
    {
        this->ui_ManuallySavedState = true;

        CoreSaveStateType type = fileName.endsWith(".pj") ?
                                    CoreSaveStateType::Project64 :
                                    CoreSaveStateType::Mupen64Plus;

        if (!CoreSaveState(fileName.toStdU32String(), type))
        {
            this->ui_ManuallySavedState = false;
            this->showErrorMessage("CoreSaveState() Failed", QString::fromStdString(CoreGetError()));
        }
        else
        {
            OnScreenDisplaySetMessage("Saved state to: " + QDir::toNativeSeparators(fileName).toStdString());
        }
    }

    if (isRunning && !isPaused)
    {
        this->on_Action_System_Pause();
    }
}

void MainWindow::on_Action_System_LoadState(void)
{
    this->ui_ManuallyLoadedState = true;

    if (!CoreLoadSaveState())
    {
        this->ui_ManuallyLoadedState = false;
        this->showErrorMessage("CoreLoadSaveState() Failed", QString::fromStdString(CoreGetError()));
    }
    else
    {
        OnScreenDisplaySetMessage("State loaded from slot: " + std::to_string(CoreGetSaveStateSlot()));
    }
}

void MainWindow::on_Action_System_Load(void)
{
    bool isRunning = CoreIsEmulationRunning();
    bool isPaused = CoreIsEmulationPaused();

    if (isRunning && !isPaused)
    {
        this->on_Action_System_Pause();
    }

    QString fileName =
        QFileDialog::getOpenFileName(this, tr("Open Save State"), "", tr("Save State (*.dat *.state *.st* *.pj*);;All Files (*)"));

    if (!fileName.isEmpty())
    {
        this->ui_ManuallyLoadedState = true;

        if (!CoreLoadSaveState(fileName.toStdU32String()))
        {
            this->ui_ManuallyLoadedState = false;
            this->showErrorMessage("CoreLoadSaveState() Failed", QString::fromStdString(CoreGetError()));
        }
        else
        {
            OnScreenDisplaySetMessage("State loaded from: " + QDir::toNativeSeparators(fileName).toStdString());
        }
    }

    if (isRunning && !isPaused)
    {
        this->on_Action_System_Pause();
    }
}

void MainWindow::on_Action_System_CurrentSaveState(int slot)
{
    if (!CoreSetSaveStateSlot(slot))
    {
        this->showErrorMessage("CoreSetSaveStateSlot() Failed", QString::fromStdString(CoreGetError()));
    }
}

void MainWindow::on_Action_System_Cheats(void)
{
    bool isRunning = CoreIsEmulationRunning();
    bool isPaused = CoreIsEmulationPaused();

    if (isRunning && !isPaused)
    {
        this->on_Action_System_Pause();
    }

    Dialog::CheatsDialog dialog(this);
    if (!dialog.HasFailed())
    {
        dialog.exec();
    }

    if (isRunning && !isPaused)
    {
        this->on_Action_System_Pause();
    }
}

void MainWindow::on_Action_System_GSButton(void)
{
    if (!CorePressGamesharkButton(true))
    {
        this->showErrorMessage("CorePressGamesharkButton() Failed", QString::fromStdString(CoreGetError()));
    }
    else
    {
        // only hold the gameshark button for 1 second
        this->ui_GamesharkButtonTimerId = this->startTimer(1000);
    }
}

void MainWindow::on_Action_Settings_Graphics(void)
{
    CorePluginsOpenConfig(CorePluginType::Gfx, this);
}

void MainWindow::on_Action_Settings_Audio(void)
{
    CorePluginsOpenConfig(CorePluginType::Audio, this);
}

void MainWindow::on_Action_Settings_Rsp(void)
{
    CorePluginsOpenConfig(CorePluginType::Rsp, this);
}


void MainWindow::on_Action_Settings_Input(void)
{
    // If raphnetraw is the active input plugin, open the input test dialog
    // (only when no ROM is running to avoid interfering with game input)
    if (isRaphnetRawPlugin())
    {
        if (!CoreIsEmulationRunning())
        {
            UserInterface::RaphnetInputDialog dialog(this);
            dialog.exec();
        }
        return;
    }

    // Clear the plugin switch flag before opening config
    CoreSettingsSetValue(SettingsID::Internal_InputPluginSwitchRequested, false);

    CorePluginsOpenConfig(CorePluginType::Input, this);

    // Check if a plugin switch was requested (e.g., raphnet to raphnetraw)
    if (CoreSettingsGetBoolValue(SettingsID::Internal_InputPluginSwitchRequested))
    {
        CoreSettingsSetValue(SettingsID::Internal_InputPluginSwitchRequested, false);
        if (!CoreApplyPluginSettings())
        {
            this->showErrorMessage("CoreApplyPluginSettings() Failed", QString::fromStdString(CoreGetError()));
        }

        // Update input settings button enabled state
        bool hasInputConfig = CorePluginsHasConfig(CorePluginType::Input) ||
            (isRaphnetRawPlugin() && !CoreIsEmulationRunning());
        this->action_Settings_Input->setEnabled(hasInputConfig);
        this->action_Toolbar_Input->setEnabled(hasInputConfig);
    }
}

void MainWindow::on_Action_Playback(void)
{
#ifdef NETPLAY
    // If already open, just bring it to front
    auto* existing = findChild<KailleraPlaybackDialog*>();
    if (existing)
    {
        existing->raise();
        existing->activateWindow();
        return;
    }

    // Initialize Kaillera if not already initialized
    if (!CoreInitKaillera())
    {
        this->showErrorMessage("Kaillera Error", QString::fromStdString(CoreGetError()));
        return;
    }

    // Ensure session manager exists so gameCallback is wired up.
    // Without this, the state machine reaches state 1 but has no
    // callback to actually start emulation.
    if (this->kailleraSessionManager == nullptr)
    {
        this->kailleraSessionManager = new KailleraSessionManager(this);
        connect(this->kailleraSessionManager, &KailleraSessionManager::gameStarted,
                this, &MainWindow::on_Kaillera_GameStarted);
        connect(this->kailleraSessionManager, &KailleraSessionManager::chatReceived,
                this, &MainWindow::on_Kaillera_ChatReceived);
        connect(&KailleraUIBridge::instance(), &KailleraUIBridge::kailleraGameChatReceived,
                this, &MainWindow::on_Kaillera_ChatReceived);
        connect(&KailleraUIBridge::instance(), &KailleraUIBridge::p2pChatReceived,
                this, &MainWindow::on_Kaillera_ChatReceived);
        connect(&KailleraUIBridge::instance(), &KailleraUIBridge::recordingFileClosed,
                this, &MainWindow::on_Kaillera_RecordingFileClosed);
        connect(this->kailleraSessionManager, &KailleraSessionManager::playerDropped,
                this, &MainWindow::on_Kaillera_PlayerDropped);
        connect(this->kailleraSessionManager, &KailleraSessionManager::gameEnded,
                this, &MainWindow::on_Kaillera_GameEnded);
    }

    // Activate playback mode so the state machine uses the playback module
    n02::activateMode(2);

    auto* dialog = new KailleraPlaybackDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::destroyed, this, [this]() {
        if (this->kailleraSessionManager != nullptr)
        {
            disconnect(&KailleraUIBridge::instance(), &KailleraUIBridge::kailleraGameChatReceived,
                       this, &MainWindow::on_Kaillera_ChatReceived);
            disconnect(&KailleraUIBridge::instance(), &KailleraUIBridge::p2pChatReceived,
                       this, &MainWindow::on_Kaillera_ChatReceived);
            disconnect(&KailleraUIBridge::instance(), &KailleraUIBridge::recordingFileClosed,
                       this, &MainWindow::on_Kaillera_RecordingFileClosed);
            delete this->kailleraSessionManager;
            this->kailleraSessionManager = nullptr;
            CoreShutdownKaillera();
            this->updateUI(this->emulationThread->isRunning(), CoreIsEmulationPaused());
        }
    });
    dialog->show();
#endif // NETPLAY
}

void MainWindow::on_Action_Settings_Settings(void)
{
    bool isRunning = CoreIsEmulationRunning();
    bool isPaused = CoreIsEmulationPaused();
    const QString previousTheme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_Theme));
    const QString previousIconTheme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_IconTheme));

    if (isRunning && !isPaused)
    {
        this->on_Action_System_Pause();
    }

    Dialog::SettingsDialog dialog(this);
    const int result = dialog.exec();

    if (result == QDialog::Accepted)
    {
        CoreRollbackSetVerboseStats(CoreSettingsGetBoolValue(SettingsID::Rollback_VerboseStats));
        const QString currentTheme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_Theme));
        const QString currentIconTheme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_IconTheme));
        if (currentTheme != previousTheme || currentIconTheme != previousIconTheme)
        {
            this->reapplyTheme();
        }
    }

    // reload UI,
    // because we need to keep Settings -> {type}
    // up-to-date
    this->updateActions(emulationThread->isRunning(), isPaused);

    // update core callbacks settings
    this->coreCallBacks->LoadSettings();

    if (isRunning && !isPaused)
    {
        this->on_Action_System_Pause();
    }
}

void MainWindow::on_Action_Settings_Plugins(void)
{
    bool isRunning = CoreIsEmulationRunning();
    bool isPaused = CoreIsEmulationPaused();
    const QString previousTheme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_Theme));
    const QString previousIconTheme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_IconTheme));

    if (isRunning && !isPaused)
    {
        this->on_Action_System_Pause();
    }

    Dialog::SettingsDialog dialog(this);
    dialog.ShowPluginsTab();
    const int result = dialog.exec();

    if (result == QDialog::Accepted)
    {
        CoreRollbackSetVerboseStats(CoreSettingsGetBoolValue(SettingsID::Rollback_VerboseStats));
        const QString currentTheme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_Theme));
        const QString currentIconTheme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_IconTheme));
        if (currentTheme != previousTheme || currentIconTheme != previousIconTheme)
        {
            this->reapplyTheme();
        }
    }

    // reload UI,
    // because we need to keep Settings -> {type}
    // up-to-date
    this->updateActions(emulationThread->isRunning(), isPaused);

    // update core callbacks settings
    this->coreCallBacks->LoadSettings();

    if (isRunning && !isPaused)
    {
        this->on_Action_System_Pause();
    }
}

void MainWindow::on_Action_View_Toolbar(bool checked)
{
    if (!this->ui_ShowUI)
    {
        return;
    }

    CoreSettingsSetValue(SettingsID::GUI_Toolbar, checked);
    this->toolBar->setVisible(checked);
    this->ui_ShowToolbar = checked;
}

void MainWindow::on_Action_View_StatusBar(bool checked)
{
    if (!this->ui_ShowUI)
    {
        return;
    }

    CoreSettingsSetValue(SettingsID::GUI_StatusBar, checked);
    this->statusBar()->setVisible(checked);
    this->ui_ShowStatusbar = checked;
}

void MainWindow::on_Action_View_GameList(bool checked)
{
    if (checked)
    {
        this->ui_Widget_RomBrowser->ShowList();
        CoreSettingsSetValue(SettingsID::RomBrowser_ViewMode, 0);
    }
}

void MainWindow::on_Action_View_GameGrid(bool checked)
{
    if (checked)
    {
        this->ui_Widget_RomBrowser->ShowGrid();
        CoreSettingsSetValue(SettingsID::RomBrowser_ViewMode, 1);
    }
}

void MainWindow::on_Action_View_UniformSize(bool checked)
{
    this->ui_Widget_RomBrowser->SetGridViewUniformSizes(checked);
    CoreSettingsSetValue(SettingsID::RomBrowser_GridViewUniformItemSizes, checked);
}

void MainWindow::on_Action_View_Fullscreen(void)
{
    if (!CoreToggleFullscreen())
    {
        this->showErrorMessage("CoreToggleFullscreen() Failed", QString::fromStdString(CoreGetError()));
    }
}

void MainWindow::on_Action_View_RefreshRoms(void)
{
    if (!this->ui_Widget_RomBrowser->IsRefreshingRomList())
    {
        this->ui_Widget_RomBrowser->RefreshRomList();
    }
}

void MainWindow::on_Action_View_ClearRomCache(void)
{
    if (!CoreClearRomHeaderAndSettingsCache())
    {
        this->showErrorMessage("CoreClearRomHeaderAndSettingsCache() Failed", QString::fromStdString(CoreGetError()));
    }
}

void MainWindow::on_Action_View_Log(void)
{
    this->logDialog.show();
}

void MainWindow::on_Action_View_Search(void)
{
    this->ui_Widget_RomBrowser->SetToggleSearch();
}

void MainWindow::on_Action_Netplay_CreateSession(void)
{
#ifdef NETPLAY
    static QWebSocket webSocket;

    Dialog::CreateNetplaySessionDialog dialog(this, &webSocket, this->ui_Widget_RomBrowser->GetModelData());
    int ret = dialog.exec();
    if (ret == QDialog::Accepted)
    {
        this->showNetplaySessionDialog(&webSocket, dialog.GetSessionJson(), dialog.GetSessionFile());
    }
#endif // NETPLAY
}

void MainWindow::on_Action_Netplay_BrowseSessions(void)
{
#ifdef NETPLAY
    // Initialize Kaillera if not already initialized
    if (!CoreInitKaillera())
    {
        this->showErrorMessage("Kaillera Error", QString::fromStdString(CoreGetError()));
        return;
    }

    // Set Kaillera app info (app name and game list)
    std::string appName = "RMG-K " + CoreGetVersion();
    // Build game list from ROM browser (null-terminated strings with double-null at end)
    // Must use std::string directly to preserve embedded null characters
    std::string gameList;
    QMap<QString, CoreRomSettings> romData = this->ui_Widget_RomBrowser->GetModelData();

    // Collect and sort game names alphabetically for Kaillera.
    // Fallback to filename when GoodName is empty/invalid to avoid creating
    // an empty first entry in the null-delimited list.
    std::vector<std::string> goodNames;
    for (auto it = romData.begin(); it != romData.end(); ++it)
    {
        std::string goodName = it.value().GoodName;
        // Strip "(unknown rom)" suffix for better Kaillera compatibility
        const std::string suffix = " (unknown rom)";
        if (goodName.size() >= suffix.size() &&
            goodName.compare(goodName.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            goodName = goodName.substr(0, goodName.size() - suffix.size());
        }

        QString displayName = QString::fromStdString(goodName).trimmed();
        if (displayName.isEmpty())
        {
            QFileInfo fileInfo(it.key());
            displayName = fileInfo.completeBaseName().trimmed();
            if (displayName.isEmpty())
            {
                displayName = fileInfo.fileName().trimmed();
            }
        }

        if (!displayName.isEmpty())
        {
            goodNames.push_back(displayName.toStdString());
        }
    }
    std::sort(goodNames.begin(), goodNames.end(), [](const std::string& a, const std::string& b) {
        return QString::fromStdString(a).compare(QString::fromStdString(b), Qt::CaseInsensitive) < 0;
    });

    for (const auto& name : goodNames)
    {
        gameList += name;
        gameList += '\0';
    }
    gameList += '\0'; // Double null terminator
    CoreSetKailleraAppInfo(appName, gameList);

    // Create Kaillera session manager
    if (this->kailleraSessionManager != nullptr)
    {
        delete this->kailleraSessionManager;
    }
    this->kailleraSessionManager = new KailleraSessionManager(this);

    // Connect signals
    connect(this->kailleraSessionManager, &KailleraSessionManager::gameStarted,
            this, &MainWindow::on_Kaillera_GameStarted);
    connect(this->kailleraSessionManager, &KailleraSessionManager::rollbackSessionPreparing,
            this, [this]() {
                this->ui_RollbackNetplayRoomActive = true;
            });
    connect(this->kailleraSessionManager, &KailleraSessionManager::rollbackSessionRequested,
            this, &MainWindow::on_Rollback_SessionRequested);
    connect(this->kailleraSessionManager, &KailleraSessionManager::chatReceived,
            this, &MainWindow::on_Kaillera_ChatReceived);
    connect(&KailleraUIBridge::instance(), &KailleraUIBridge::kailleraGameChatReceived,
            this, &MainWindow::on_Kaillera_ChatReceived);
    connect(&KailleraUIBridge::instance(), &KailleraUIBridge::p2pChatReceived,
            this, &MainWindow::on_Kaillera_ChatReceived);
    connect(&KailleraUIBridge::instance(), &KailleraUIBridge::recordingFileClosed,
            this, &MainWindow::on_Kaillera_RecordingFileClosed);
    connect(this->kailleraSessionManager, &KailleraSessionManager::playerDropped,
            this, &MainWindow::on_Kaillera_PlayerDropped);
    connect(this->kailleraSessionManager, &KailleraSessionManager::gameEnded,
            this, &MainWindow::on_Kaillera_GameEnded);

    // Disable buttons while Kaillera dialog is open
    this->action_Netplay_BrowseSessions->setEnabled(false);
    this->action_Netplay_Start->setEnabled(false);
    this->action_System_StartRom->setEnabled(false);

    // Show Kaillera's built-in server browser dialog
    // This is a blocking call - user will select server, join/create game
    // When they start a game, gameStarted signal will be emitted
    // Dialog stays open until user closes it
    this->kailleraSessionManager->showServerDialog();
    if (!this->ui_RollbackNetplayLaunchActive)
    {
        this->ui_RollbackNetplayRoomActive = false;
    }

    // Dialog closed - clean up Kaillera session
    // (emulation may still be running - user can manually stop it)
    // Guard: closeEvent may have already cleaned up if the main window was closed
    if (this->kailleraSessionManager != nullptr)
    {
        disconnect(&KailleraUIBridge::instance(), &KailleraUIBridge::kailleraGameChatReceived,
                   this, &MainWindow::on_Kaillera_ChatReceived);
        disconnect(&KailleraUIBridge::instance(), &KailleraUIBridge::p2pChatReceived,
                   this, &MainWindow::on_Kaillera_ChatReceived);
        disconnect(&KailleraUIBridge::instance(), &KailleraUIBridge::recordingFileClosed,
                   this, &MainWindow::on_Kaillera_RecordingFileClosed);
        delete this->kailleraSessionManager;
        this->kailleraSessionManager = nullptr;
        CoreShutdownKaillera();

        // Re-enable buttons and update UI
        this->action_Netplay_BrowseSessions->setEnabled(true);
        this->action_Netplay_Start->setEnabled(true);
        this->action_System_StartRom->setEnabled(true);
        this->updateUI(this->emulationThread->isRunning(), CoreIsEmulationPaused());
    }
#endif // NETPLAY
}

void MainWindow::on_Action_Netplay_ViewSession(void)
{
#ifdef NETPLAY
    if (this->netplaySessionDialog != nullptr &&
        this->netplaySessionDialog->isHidden())
    {
        this->netplaySessionDialog->show();
    }
#endif
}

#ifdef NETPLAY
void MainWindow::on_Kaillera_GameStarted(QString gameName, int playerNum, int totalPlayers)
{
    if (this->ui_RollbackNetplayRoomActive || this->ui_RollbackNetplayLaunchActive)
    {
        return;
    }

    // Find ROM file by game name
    QString romFile = this->findRomByName(gameName);
    if (romFile.isEmpty())
    {
        this->showErrorMessage("ROM Not Found",
            "Could not find ROM: " + gameName + "\n\nPlease add it to your ROM directory and refresh the list.");
        // Just end the Kaillera game - full cleanup happens when dialog closes
        CoreEndKailleraGame();
        return;
    }

    // Wait for any previous emulation to finish
    // This is needed when restarting after Drop
    if (this->emulationThread->isRunning())
    {
        CoreStopEmulation();
        // Wait for thread to finish with timeout
        // Use a simple loop since we're in Kaillera's message pump context
        int timeout = 5000;
        while (this->emulationThread->isRunning() && timeout > 0)
        {
            QThread::msleep(10);
            timeout -= 10;
        }
    }

    this->emulationThread->SetNetplay("KAILLERA", 0, playerNum); // "KAILLERA" is the marker
#ifdef _WIN32
    OnScreenDisplaySetKailleraPortLabels(totalPlayers, GetLiveKailleraPortLabelNames());
#endif
    this->launchEmulationThread(romFile, "", false, -1, true);
}

void MainWindow::on_Rollback_SessionRequested(QString gameName, QString remoteAddress, int localPort, int remotePort, int localPlayer, int frameDelay, int predictionWindow)
{
    QString romFile = this->findRomByName(gameName);
    if (romFile.isEmpty())
    {
        this->showErrorMessage("ROM Not Found",
            "Could not find ROM: " + gameName + "\n\nPlease add it to your ROM directory and refresh the list.");
        return;
    }

    if (this->emulationThread->isRunning())
    {
        CoreStopEmulation();
        QTimer::singleShot(50, this,
            [this, gameName, remoteAddress, localPort, remotePort, localPlayer, frameDelay, predictionWindow]()
            {
                this->on_Rollback_SessionRequested(gameName, remoteAddress, localPort, remotePort, localPlayer, frameDelay, predictionWindow);
            });
        return;
    }

    if (this->ui_RollbackNetplayLaunchActive)
    {
        return;
    }

    this->ui_RollbackNetplayLaunchActive = true;

    if (this->ui_CheckVideoSizeTimerId != 0)
    {
        this->killTimer(this->ui_CheckVideoSizeTimerId);
        this->ui_CheckVideoSizeTimerId = 0;
    }

#ifdef _WIN32
    OnScreenDisplaySetKailleraPortLabels(2, GetLiveKailleraPortLabelNames());
#endif
    this->emulationThread->SetGekkoNetplay(remoteAddress, localPort, remotePort, localPlayer, frameDelay, predictionWindow);
    this->launchEmulationThread(romFile, "", false, -1, true);
}

void MainWindow::on_Kaillera_ChatReceived(QString nickname, QString message)
{
    // Only show in-game Kaillera chat (not lobby chat).
    if (!CoreHasInitKaillera() || !this->emulationThread->isRunning())
    {
        return;
    }

    nickname = nickname.trimmed();
    message = NormalizeOsdKailleraChatMessage(message);

    const QString localNickname = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Kaillera_Username)).trimmed();
    if (!localNickname.isEmpty() && nickname.compare(localNickname, Qt::CaseInsensitive) == 0)
    {
        const auto now = std::chrono::steady_clock::now();
        while (!this->ui_PendingLocalChatEchoes.empty())
        {
            const auto age = now - this->ui_PendingLocalChatEchoes.front().time;
            if (age <= kLocalEchoMaxAge)
            {
                break;
            }
            this->ui_PendingLocalChatEchoes.pop_front();
        }

        auto pendingEcho = std::find_if(this->ui_PendingLocalChatEchoes.begin(), this->ui_PendingLocalChatEchoes.end(),
            [&message](const PendingLocalChatEcho& pending) {
                return pending.message == message;
            });
        if (pendingEcho != this->ui_PendingLocalChatEchoes.end())
        {
            this->ui_PendingLocalChatEchoes.erase(pendingEcho);
            return;
        }

        const std::string chatLine = "<" + nickname.toStdString() + "> " + message.toStdString();
        OnScreenDisplaySetKailleraChatMessageImmediate(chatLine);
    }
    else
    {
        const std::string chatLine = "<" + nickname.toStdString() + "> " + message.toStdString();
        OnScreenDisplaySetKailleraChatMessage(chatLine);
    }
}

void MainWindow::on_Kaillera_PlayerDropped(QString nickname, int playerNum)
{
    // Show notification based on who dropped
    if (playerNum == 1)
    {
        OnScreenDisplaySetMessage("Host " + nickname.toStdString() + " dropped - game ending.");
    }
    else
    {
        OnScreenDisplaySetMessage(nickname.toStdString() + " (P" + std::to_string(playerNum) + ") dropped.");
    }

    // Note: Emulation stop is handled by gameEnded signal from KailleraSessionManager
    // Player 1 drop = everyone stops, Players 2-4 drop = only they stop
}

void MainWindow::on_Kaillera_GameEnded(void)
{
    if (this->ui_RollbackNetplayRoomActive || this->ui_RollbackNetplayLaunchActive)
    {
        return;
    }

    // Mark game as inactive to re-enable UI buttons
    CoreMarkKailleraGameInactive();

    OnScreenDisplaySetKailleraChatMessage("");
    OnScreenDisplayClearKailleraPortLabels();
#ifdef NETPLAY
    this->ui_PendingLocalChatEchoes.clear();
    this->closeNetplayChatPrompt();
#endif // NETPLAY

    // Stop emulation when game ends (user dropped or was dropped)
    if (this->emulationThread->isRunning())
    {
        CoreStopEmulation();
    }
}

#ifdef NETPLAY
void MainWindow::on_Kaillera_RecordingFileClosed(void)
{
    this->refreshKailleraRecordingStorageStatus(false);
}

bool MainWindow::handleNetplayChatKeyPress(QKeyEvent *event)
{
    if (!CoreIsEmulationRunning())
    {
        return false;
    }

    if (this->kailleraSessionManager == nullptr ||
        !this->kailleraSessionManager->isGameActive())
    {
        return false;
    }

    if (this->ui_NetplayChatInputActive)
    {
        const int key = event->key();
        if ((key == Qt::Key_Escape) && !event->isAutoRepeat())
        {
            this->closeNetplayChatPrompt();
            return true;
        }

        if ((key == Qt::Key_Return || key == Qt::Key_Enter) && !event->isAutoRepeat())
        {
            QString message = this->ui_NetplayChatInput.trimmed();
            if (!message.isEmpty())
            {
                const QString normalizedMessage = NormalizeOsdKailleraChatMessage(message);
                this->kailleraSessionManager->sendChatMessage(normalizedMessage);

                const QString localNickname = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Kaillera_Username)).trimmed();
                const bool useImmediateLocalEcho = (n02::getActiveMode() != 1);
                if (useImmediateLocalEcho && !localNickname.isEmpty())
                {
                    const auto now = std::chrono::steady_clock::now();
                    while (!this->ui_PendingLocalChatEchoes.empty())
                    {
                        const auto age = now - this->ui_PendingLocalChatEchoes.front().time;
                        if (age <= kLocalEchoMaxAge)
                        {
                            break;
                        }
                        this->ui_PendingLocalChatEchoes.pop_front();
                    }

                    this->ui_PendingLocalChatEchoes.push_back({normalizedMessage, std::chrono::steady_clock::now()});
                    while (this->ui_PendingLocalChatEchoes.size() > kLocalEchoMaxEntries)
                    {
                        this->ui_PendingLocalChatEchoes.pop_front();
                    }

                    const std::string chatLine = "<" + localNickname.toStdString() + "> " + normalizedMessage.toStdString();
                    OnScreenDisplaySetKailleraChatMessageImmediate(chatLine);
                }
            }
            this->closeNetplayChatPrompt();
            return true;
        }

        if (key == Qt::Key_Backspace)
        {
            if (!this->ui_NetplayChatInput.isEmpty())
            {
                this->ui_NetplayChatInput.chop(1);
                this->updateNetplayChatPrompt();
            }
            return true;
        }

        const QString text = event->text();
        if (!text.isEmpty())
        {
            const int maxLength = 127;
            if (this->ui_NetplayChatInput.size() < maxLength)
            {
                const QChar ch = text.at(0);
                if (!ch.isNull() && (ch.isPrint() || ch.isSpace()))
                {
                    this->ui_NetplayChatInput.append(ch);
                    this->updateNetplayChatPrompt();
                }
            }
            return true;
        }

        return true;
    }

    if (event->isAutoRepeat())
    {
        return false;
    }

    QString keyBinding = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::KeyBinding_NetplayChat));
    if (keyBinding.isEmpty())
    {
        return false;
    }

    const int key = event->key();
    QKeySequence expected(keyBinding);
    QKeySequence pressed(key | event->modifiers());
    if (expected.matches(pressed) == QKeySequence::ExactMatch)
    {
        this->ui_NetplayChatInputActive = true;
        this->ui_NetplayChatInput.clear();
        this->updateNetplayChatPrompt();
        return true;
    }

    if (key == Qt::Key_Enter)
    {
        QKeySequence altPressed(Qt::Key_Return | event->modifiers());
        if (expected.matches(altPressed) == QKeySequence::ExactMatch)
        {
            this->ui_NetplayChatInputActive = true;
            this->ui_NetplayChatInput.clear();
            this->updateNetplayChatPrompt();
            return true;
        }
    }
    else if (key == Qt::Key_Return)
    {
        QKeySequence altPressed(Qt::Key_Enter | event->modifiers());
        if (expected.matches(altPressed) == QKeySequence::ExactMatch)
        {
            this->ui_NetplayChatInputActive = true;
            this->ui_NetplayChatInput.clear();
            this->updateNetplayChatPrompt();
            return true;
        }
    }

    return false;
}

void MainWindow::updateNetplayChatPrompt(void)
{
    if (!this->ui_NetplayChatInputActive)
    {
        return;
    }

    const std::string prompt = "> " + this->ui_NetplayChatInput.toStdString();
    OnScreenDisplaySetInputPrompt(prompt);
}

void MainWindow::closeNetplayChatPrompt(void)
{
    this->ui_NetplayChatInputActive = false;
    this->ui_NetplayChatInput.clear();
    OnScreenDisplaySetInputPrompt("");
}
#endif // NETPLAY

#ifdef NETPLAY
QString MainWindow::ResolveKailleraRomByName(QString gameName)
{
    return this->findRomByName(gameName);
}
#endif // NETPLAY

QString MainWindow::findRomByName(QString gameName)
{
    // Search ROM browser data for matching game name
    QMap<QString, CoreRomSettings> romData = this->ui_Widget_RomBrowser->GetModelData();

    // Helper to normalize game names for comparison
    auto normalizeGameName = [](QString name) -> QString {
        // Remove "(unknown rom)" suffix
        if (name.endsWith(" (unknown rom)"))
            name = name.left(name.length() - 14);
        // Remove common suffixes like version numbers, regions, etc.
        name = name.remove(QRegularExpression("\\s*\\([^)]*\\)"));  // Remove (anything)
        name = name.remove(QRegularExpression("\\s*\\[[^\\]]*\\]")); // Remove [anything]
        name = name.remove(QRegularExpression("[^a-zA-Z0-9]")); // Keep only alphanumeric
        return name.toLower();
    };

    // Strip "(unknown rom)" from search term
    QString searchName = gameName;
    if (searchName.endsWith(" (unknown rom)"))
        searchName = searchName.left(searchName.length() - 14);

    // Try exact match first (with "(unknown rom)" stripped)
    for (auto it = romData.begin(); it != romData.end(); ++it)
    {
        QString localName = QString::fromStdString(it.value().GoodName);
        if (localName.endsWith(" (unknown rom)"))
            localName = localName.left(localName.length() - 14);

        if (localName == searchName)
        {
            return it.key();
        }
    }

    // Try case-insensitive match
    for (auto it = romData.begin(); it != romData.end(); ++it)
    {
        QString localName = QString::fromStdString(it.value().GoodName);
        if (localName.endsWith(" (unknown rom)"))
            localName = localName.left(localName.length() - 14);

        if (localName.compare(searchName, Qt::CaseInsensitive) == 0)
        {
            return it.key();
        }
    }

    // Try normalized comparison (removes special chars, version numbers, etc.)
    QString normalizedSearch = normalizeGameName(searchName);
    for (auto it = romData.begin(); it != romData.end(); ++it)
    {
        QString localName = QString::fromStdString(it.value().GoodName);
        QString normalizedLocal = normalizeGameName(localName);

        if (normalizedLocal == normalizedSearch)
        {
            return it.key();
        }
    }

    // Try substring match (if one contains the other)
    if (!normalizedSearch.isEmpty())
    {
        for (auto it = romData.begin(); it != romData.end(); ++it)
        {
            QString localName = QString::fromStdString(it.value().GoodName);
            QString normalizedLocal = normalizeGameName(localName);

            if (!normalizedLocal.isEmpty() &&
                (normalizedLocal.contains(normalizedSearch) || normalizedSearch.contains(normalizedLocal)))
            {
                return it.key();
            }
        }
    }

    return QString(); // Not found
}
#endif // NETPLAY

void MainWindow::on_Action_Help_Github(void)
{
    QDesktopServices::openUrl(QUrl("https://github.com/Jay-Day/RMG-K"));
}

void MainWindow::on_Action_Help_About(void)
{
    Dialog::AboutDialog dialog(this);
    dialog.exec();
}

void MainWindow::on_Action_Help_Update(void)
{
#ifdef UPDATER
    this->checkForUpdates(false, true);
#endif // UPDATER
}

void MainWindow::on_Action_Audio_IncreaseVolume(void)
{
    CoreIncreaseVolume();
}

void MainWindow::on_Action_Audio_DecreaseVolume(void)
{
    CoreDecreaseVolume();
}

void MainWindow::on_Action_Audio_ToggleVolumeMute(void)
{
    if (!CoreToggleMuteVolume())
    {
        this->showErrorMessage("CoreToggleMuteVolume() Failed", QString::fromStdString(CoreGetError()));
    }
}

void MainWindow::on_Emulation_Started(void)
{
    this->ui_FocusPausedEmulation = false;

    // only clear log dialog when we've gone over the limit
    if (this->logDialog.GetLineCount() >= 500000)
    {
        this->logDialog.Clear();
    }

    this->ui_MessageBoxList.clear();
    this->ui_DebugCallbackErrors.clear();
}

void MainWindow::on_Emulation_Finished(bool ret, QString error)
{
    this->ui_FocusPausedEmulation = false;

#ifdef _WIN32
    this->restoreDisplayMode();
#endif

#ifdef NETPLAY
    if (this->ui_RollbackLivePumpTimerId != 0)
    {
        this->killTimer(this->ui_RollbackLivePumpTimerId);
        this->ui_RollbackLivePumpTimerId = 0;
    }
    this->ui_RollbackLivePumpPending = false;
    this->ui_RollbackLivePumpActive = false;
    this->ui_RollbackNetplayRoomActive = false;
    this->ui_RollbackNetplayLaunchActive = false;
    OnScreenDisplayClearKailleraPortLabels();
#endif // NETPLAY

    if (!ret)
    {
        // whatever we do on failure,
        // always return to the rombrowser
        this->ui_NoSwitchToRomBrowser = false;
    }

#ifdef NETPLAY
    if (this->netplaySessionDialog != nullptr)
    {
        this->netplaySessionDialog->deleteLater();
        this->netplaySessionDialog = nullptr;
    }
#endif // NETPLAY

    if (!this->ui_QuitAfterEmulation &&
        !this->ui_NoSwitchToRomBrowser &&
        this->ui_RefreshRomListAfterEmulation)
    {
        this->ui_Widget_RomBrowser->RefreshRomList();
        this->ui_RefreshRomListAfterEmulation = false;
    }

    if (this->ui_FullscreenTimerId != 0)
    {
        this->killTimer(this->ui_FullscreenTimerId);
        this->ui_FullscreenTimerId = 0;
    }

    if (this->ui_CheckVideoSizeTimerId != 0)
    {
        this->killTimer(this->ui_CheckVideoSizeTimerId);
        this->ui_CheckVideoSizeTimerId = 0;
    }

    if (this->ui_QuitAfterEmulation)
    {
        // show error message when
        // the user has requested we quit
        // after emulation
        if (!ret)
        {
            this->showErrorMessage("EmulationThread::run Failed", error);
        }

        this->ui_ForceClose = true;
        this->close();
        return;
    }

    // always refresh UI
    this->updateUI(false, false);

    // show error message to the user
    // after switching back to the ROM browser
    if (!ret)
    {
        this->showErrorMessage("EmulationThread::run Failed", error);
    }
}

void MainWindow::on_RomBrowser_PlayGame(QString file)
{
    this->launchEmulationThread(file);
}

void MainWindow::on_RomBrowser_PlayGameWith(CoreRomType type, QString file)
{
    QString mainRom;
    QString otherRom;

    if (type == CoreRomType::Cartridge)
    { // cartridge
        mainRom = file;
        otherRom = QFileDialog::getOpenFileName(this, tr("Open 64DD Disk"), "", "N64DD Disk Image (*.ndd *.d64 *.zip *.7z)");
    }
    else
    { // disk
        mainRom = QFileDialog::getOpenFileName(this, tr("Open N64 ROM"), "", "N64 ROMs (*.n64 *.z64 *.v64 *.zip *.7z)");
        otherRom = file;
    }

    if (mainRom.isEmpty() || otherRom.isEmpty())
    {
        return;
    }

    this->launchEmulationThread(mainRom, otherRom);
}

void MainWindow::on_RomBrowser_PlayGameWithDisk(QString cartridge, QString disk)
{
    this->launchEmulationThread(cartridge, disk);
}

void MainWindow::on_RomBrowser_PlayGameWithSlot(QString file, int slot)
{
    this->launchEmulationThread(file, "", false, slot);
}

void MainWindow::on_RomBrowser_ChangeRomDirectory(void)
{
    QString currentDir = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::RomBrowser_Directory));
    if (!QDir(currentDir).exists())
    {
        currentDir = "";
    }

    QString dir = QFileDialog::getExistingDirectory(this, tr("Select ROM Directory"), currentDir);
    if (!dir.isEmpty() && currentDir != dir)
    {
        CoreSettingsSetValue(SettingsID::RomBrowser_Directory, dir.toStdString());
        this->ui_Widget_RomBrowser->RefreshRomList();
    }
}

void MainWindow::on_RomBrowser_RomInformation(QString file)
{
    bool isRefreshingRomList = this->ui_Widget_RomBrowser->IsRefreshingRomList();
    if (isRefreshingRomList)
    {
        this->ui_Widget_RomBrowser->StopRefreshRomList();
    }

    CoreRomType romType;
    CoreRomHeader romHeader;
    CoreRomSettings romSettings;

    if (!CoreGetCachedRomHeaderAndSettings(file.toStdU32String(), &romType, &romHeader, nullptr, &romSettings))
    {
        this->showErrorMessage("CoreGetCachedRomHeaderAndSettings() Failed", QString::fromStdString(CoreGetError()));
        return;
    }

    Dialog::RomInfoDialog dialog(this, file, romType, romHeader, romSettings);
    dialog.exec();

    if (isRefreshingRomList)
    {
        this->ui_Widget_RomBrowser->RefreshRomList();
    }
}

void MainWindow::on_RomBrowser_EditGameSettings(QString file)
{
    bool isRefreshingRomList = this->ui_Widget_RomBrowser->IsRefreshingRomList();
    const QString previousTheme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_Theme));
    const QString previousIconTheme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_IconTheme));
    if (isRefreshingRomList)
    {
        this->ui_Widget_RomBrowser->StopRefreshRomList();
    }

    Dialog::SettingsDialog dialog(this, file);
    dialog.ShowGameTab();
    const int result = dialog.exec();

    if (result == QDialog::Accepted)
    {
        CoreRollbackSetVerboseStats(CoreSettingsGetBoolValue(SettingsID::Rollback_VerboseStats));
        const QString currentTheme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_Theme));
        const QString currentIconTheme = QString::fromStdString(CoreSettingsGetStringValue(SettingsID::GUI_IconTheme));
        if (currentTheme != previousTheme || currentIconTheme != previousIconTheme)
        {
            this->reapplyTheme();
        }
    }

    this->updateActions(false, false);
    this->coreCallBacks->LoadSettings();

    if (isRefreshingRomList)
    {
        this->ui_Widget_RomBrowser->RefreshRomList();
    }
}

void MainWindow::on_RomBrowser_EditGameInputSettings(QString file)
{
    bool isRefreshingRomList = this->ui_Widget_RomBrowser->IsRefreshingRomList();
    if (isRefreshingRomList)
    {
        this->ui_Widget_RomBrowser->StopRefreshRomList();
    }

    if (!CorePluginsOpenROMConfig(CorePluginType::Input, this, file.toStdU32String()))
    {
        this->showErrorMessage("CorePluginsOpenROMConfig() Failed", QString::fromStdString(CoreGetError()));
        return;
    }

    if (isRefreshingRomList)
    {
        this->ui_Widget_RomBrowser->RefreshRomList();
    }
}

void MainWindow::on_RomBrowser_Cheats(QString file)
{
    bool isRefreshingRomList = this->ui_Widget_RomBrowser->IsRefreshingRomList();
    if (isRefreshingRomList)
    {
        this->ui_Widget_RomBrowser->StopRefreshRomList();
    }

    Dialog::CheatsDialog dialog(this, file);
    if (!dialog.HasFailed())
    {
        dialog.exec();
    }

    if (isRefreshingRomList)
    {
        this->ui_Widget_RomBrowser->RefreshRomList();
    }
}

void MainWindow::on_Netplay_PlayGame(QString file, QString address, int port, int player)
{
    this->launchEmulationThread(file, address, port, player);
}

void MainWindow::on_NetplaySessionDialog_rejected()
{
#ifdef NETPLAY
    bool isRunning = CoreIsEmulationRunning();
    bool isPaused = CoreIsEmulationPaused();

    if (this->netplaySessionDialog != nullptr)
    {
        this->netplaySessionDialog->deleteLater();
        this->netplaySessionDialog = nullptr;
    }

    // force refresh of actions
    this->updateActions(isRunning, isPaused);
#endif // NETPLAY
}

void MainWindow::on_VidExt_Init(VidExtRenderMode renderMode)
{
    this->ui_VidExtRenderMode   = renderMode;
    this->ui_VidExtForceSetMode = true;

    if (CoreSettingsGetBoolValue(SettingsID::GUI_OpenGLES))
    {
        QSurfaceFormat format = QSurfaceFormat::defaultFormat();
        format.setSwapInterval(0);
        format.setMajorVersion(2);
        format.setMinorVersion(0);
        format.setRenderableType(QSurfaceFormat::OpenGLES);
        QSurfaceFormat::setDefaultFormat(format);
    }
    else
    {
        QSurfaceFormat format = QSurfaceFormat::defaultFormat();
        format.setSwapInterval(0);
        format.setMajorVersion(3);
        format.setMinorVersion(3);
        format.setRenderableType(QSurfaceFormat::OpenGL);
        QSurfaceFormat::setDefaultFormat(format);
    }

    if (renderMode == VidExtRenderMode::OpenGL)
    {
        this->ui_Widget_OpenGL = new Widget::OGLWidget(this);
        this->ui_Widget_OpenGL->installEventFilter(this->ui_EventFilter);
        this->ui_Widget_OpenGL->SetHideCursor(this->ui_HideCursorInEmulation);

        this->ui_Widgets->addWidget(this->ui_Widget_OpenGL->GetWidget());
    }
    else if (renderMode == VidExtRenderMode::Vulkan)
    {
        this->ui_Widget_Vulkan = new Widget::VKWidget(this);
        this->ui_Widget_Vulkan->installEventFilter(this->ui_EventFilter);
        this->ui_Widget_Vulkan->SetHideCursor(this->ui_HideCursorInEmulation);

        this->ui_Widgets->addWidget(this->ui_Widget_Vulkan->GetWidget());
    }

    this->updateUI(true, false);
}

void MainWindow::on_VidExt_SetupOGL(QSurfaceFormat format, QThread* thread)
{
    this->ui_Widget_OpenGL->MoveContextToThread(thread);
    // on wayland setting the surface format
    // fails for some reason, and if we set it anyways
    // ->makeCurrent() will fail in VidExt.cpp,
    // so to resolve that I've set OpenGL 3.3 as
    // default surface format in main.cpp and we
    // skip it here only on when on wayland
    if (QGuiApplication::platformName() != "wayland")
    {
        this->ui_Widget_OpenGL->setFormat(format);
    }
}

void MainWindow::on_VidExt_SetWindowedMode(int width, int height, int bps, int flags)
{
    bool returnedFromFullscreen = false;

    if (this->isFullScreen())
    {
#ifdef _WIN32
        this->restoreDisplayMode();
#endif
        returnedFromFullscreen = true;
        this->showNormal();
    }

    if (this->ui_ShowMenubar && this->menuBar()->isHidden())
    {
        this->menuBar()->show();
    }

    if (this->ui_ShowToolbar && this->toolBar->isHidden())
    {
        this->toolBar->show();
    }

    if (this->ui_ShowStatusbar && this->statusBar()->isHidden())
    {
        this->statusBar()->show();
    }

    if (!this->ui_HideCursorInEmulation && this->ui_HideCursorInFullscreenEmulation)
    {
        if (this->ui_VidExtRenderMode == VidExtRenderMode::OpenGL)
        {
            this->ui_Widget_OpenGL->SetHideCursor(false);
        }
        else if (this->ui_VidExtRenderMode == VidExtRenderMode::Vulkan)
        {
            this->ui_Widget_Vulkan->SetHideCursor(false);
        }
    }

    if (this->ui_ShowUI)
    {
        this->removeActions();
    }

    // only resize window when we're
    // not returning from fullscreen
    if (!returnedFromFullscreen)
    {
        this->on_VidExt_ResizeWindow(width, height);
    }
}

void MainWindow::on_VidExt_SetFullscreenMode(int width, int height, int bps, int flags)
{
    if (!this->isFullScreen())
    {
#ifdef _WIN32
        if (this->ui_ExclusiveFullscreen)
        {
            this->applyExclusiveFullscreen();
        }
#endif
        this->showFullScreen();
    }

    if (!this->menuBar()->isHidden())
    {
        this->menuBar()->hide();
    }

    if (this->ui_ShowToolbar && !this->toolBar->isHidden())
    {
        this->toolBar->hide();
    }

    if (this->ui_ShowStatusbar && !this->statusBar()->isHidden())
    {
        this->statusBar()->hide();
    }

    if (!this->ui_HideCursorInEmulation && this->ui_HideCursorInFullscreenEmulation)
    {
        if (this->ui_VidExtRenderMode == VidExtRenderMode::OpenGL)
        {
            this->ui_Widget_OpenGL->SetHideCursor(true);
        }
        else if (this->ui_VidExtRenderMode == VidExtRenderMode::Vulkan)
        {
            this->ui_Widget_Vulkan->SetHideCursor(true);
        }
    }

    if (this->ui_ShowUI)
    {
        this->addActions();
    }
}

void MainWindow::on_VidExt_ResizeWindow(int width, int height)
{
    // account for HiDPI scaling
    if (this->devicePixelRatio() != 1)
    {
        width  = static_cast<int>(std::ceil(static_cast<double>(static_cast<double>(width)  / this->devicePixelRatio())));
        height = static_cast<int>(std::ceil(static_cast<double>(static_cast<double>(height) / this->devicePixelRatio())));
    }

    if (!this->menuBar()->isHidden())
    {
        height += this->menuBar()->height();
    }

    if (!this->toolBar->isHidden())
    {
        Qt::ToolBarArea area = this->toolBarArea(this->toolBar);

        // dont resize when toolbar is floating
        if (this->toolBar->isFloating())
        {
            return;
        }

        if (area == Qt::ToolBarArea::TopToolBarArea ||
            area == Qt::ToolBarArea::BottomToolBarArea)
        {
            height += this->toolBar->height();
        }
        else if (area == Qt::ToolBarArea::LeftToolBarArea ||
                 area == Qt::ToolBarArea::RightToolBarArea)
        {
            width += this->toolBar->width();
        }
    }

    if (!this->statusBar()->isHidden())
    {
        height += this->statusBar()->height();
    }

    if (!this->ui_VidExtForceSetMode)
    {
        if (this->size() == QSize(width, height))
        {
            return;
        }
    }

    if (this->isMaximized() || this->isMinimized())
    {
        this->showNormal();
    }

    this->resize(width, height);

    // we've force set the size once,
    // we can safely disable it now
    this->ui_VidExtForceSetMode = false;
}

void MainWindow::on_VidExt_ToggleFS(bool fullscreen)
{
    if (fullscreen)
    {
        if (!this->isFullScreen())
        {
#ifdef _WIN32
            if (this->ui_ExclusiveFullscreen)
            {
                this->applyExclusiveFullscreen();
            }
#endif
            this->showFullScreen();
        }

        if (this->ui_ShowMenubar && !this->menuBar()->isHidden())
        {
            this->menuBar()->hide();
        }

        if (this->ui_ShowToolbar && !this->toolBar->isHidden())
        {
            this->toolBar->hide();
        }

        if (this->ui_ShowStatusbar && !this->statusBar()->isHidden())
        {
            this->statusBar()->hide();
        }

        if (!this->ui_HideCursorInEmulation && this->ui_HideCursorInFullscreenEmulation)
        {
            if (this->ui_VidExtRenderMode == VidExtRenderMode::OpenGL)
            {
                this->ui_Widget_OpenGL->SetHideCursor(true);
            }
            else if (this->ui_VidExtRenderMode == VidExtRenderMode::Vulkan)
            {
                this->ui_Widget_Vulkan->SetHideCursor(true);
            }
        }

        if (this->ui_ShowUI)
        {
            this->addActions();
        }
    }
    else
    {
        if (this->isFullScreen())
        {
#ifdef _WIN32
            this->restoreDisplayMode();
#endif
            this->showNormal();
        }

        if (this->ui_ShowMenubar && this->menuBar()->isHidden())
        {
            this->menuBar()->show();
        }

        if (this->ui_ShowToolbar && this->toolBar->isHidden())
        {
            this->toolBar->show();
        }

        if (this->ui_ShowStatusbar && this->statusBar()->isHidden())
        {
            this->statusBar()->show();
        }

        if (!this->ui_HideCursorInEmulation && this->ui_HideCursorInFullscreenEmulation)
        {
            if (this->ui_VidExtRenderMode == VidExtRenderMode::OpenGL)
            {
                this->ui_Widget_OpenGL->SetHideCursor(false);
            }
            else if (this->ui_VidExtRenderMode == VidExtRenderMode::Vulkan)
            {
                this->ui_Widget_Vulkan->SetHideCursor(false);
            }
        }

        if (this->ui_ShowUI)
        {
            this->removeActions();
        }
    }
}

void MainWindow::on_Core_DebugCallback(QList<CoreCallbackMessage> messages)
{
    // pass callback messages to the log window
    this->logDialog.AddMessages(messages);

    // only display in statusbar when emulation is running
    if (!this->emulationThread->isRunning())
    {
        return;
    }

    // attempt to find last core message
    CoreCallbackMessage statusbarMessage = {};
    qsizetype i = messages.size() - 1;
    for (; i >= 0; i--)
    {
        if (messages[i].Context.startsWith("[CORE]") &&
            messages[i].Type != CoreDebugMessageType::Verbose &&
            !messages[i].Message.startsWith("IS64:"))
        {
            statusbarMessage = messages[i];
            break;
        }
    }
    if (i < 0)
    { // no wanted core message found
        return;
    }

    if (statusbarMessage.Type == CoreDebugMessageType::Error)
    {
        // when we've reached 50 of the same error in the same
        // emulation run, we'll stop displaying it
        if (this->ui_DebugCallbackErrors.count(statusbarMessage.Message) < 50)
        {
            this->showErrorMessage("Core Error", statusbarMessage.Message, false);
        }
        this->ui_DebugCallbackErrors.append(statusbarMessage.Message);
        return;
    }

    if (!this->ui_ShowStatusbar)
    {
        return;
    }

    this->ui_StatusBar_Label->setText(statusbarMessage.Message);

    // reset label deletion timer
    if (this->ui_ResetStatusBarTimerId != 0)
    {
        this->killTimer(this->ui_ResetStatusBarTimerId);
    }
    this->ui_ResetStatusBarTimerId = this->startTimer(this->ui_StatusBarTimerTimeout * 1000);
}

void MainWindow::on_Core_StateCallback(CoreStateCallbackType type, int value)
{
    switch (type)
    {
        default:
            break;
        case CoreStateCallbackType::EmulationState:
        {
            // update Pause button
            this->action_System_Pause->setChecked(value == static_cast<int>(CoreEmulationState::Paused));
            // update OSD state
            if (value == (int)CoreEmulationState::Paused)
            {
                OnScreenDisplayPause();
            }
            else if (value == (int)CoreEmulationState::Running)
            {
                OnScreenDisplayResume();
            }

            if (value == static_cast<int>(CoreEmulationState::Paused))
            {
            }
        } break;
        case CoreStateCallbackType::SaveStateSlot:
        {
            QAction* slotAction  = this->ui_SlotActions[value];
            QString dateTimeText = this->getSaveStateSlotDateTimeText(slotAction);
            std::string message  = "Selected save slot: " + std::to_string(value);

            if (this->ui_LoadSaveStateSlotTimerId != -1)
            {
                return;
            }

            // add date and time when available
            if (!dateTimeText.isEmpty())
            {
                message += " [";
                message += dateTimeText.toStdString();
                message += "]";
            }
            else
            {
                message += " [N/A]";
            }

            // display message
            OnScreenDisplaySetMessage(message);

            // update UI
            this->ui_SlotActions[value]->setChecked(true);
        } break;
        case CoreStateCallbackType::SpeedFactor:
        {
            OnScreenDisplaySetMessage("Playback speed: " + std::to_string(value) + "%");
        } break;
        case CoreStateCallbackType::AudioVolume:
        {
            OnScreenDisplaySetMessage("Volume: " + std::to_string(value) + "%");
        } break;
        case CoreStateCallbackType::AudioMute:
        {
            if (value == 0)
            {
                OnScreenDisplaySetMessage("Volume unmuted");
            }
            else
            {
                OnScreenDisplaySetMessage("Volume muted");
            }
        } break;
        case CoreStateCallbackType::SaveStateLoaded:
        {
            if (this->ui_LoadSaveStateSlotTimerId != -1 && value == 0)
            {
                this->ui_LoadSaveStateSlotCounter++;
                if (this->ui_LoadSaveStateSlotCounter >= 5)
                { // give up after 5 attempts
                    this->showErrorMessage("Failed to load save state");
                    this->ui_LoadSaveStateSlotCounter = 0;
                    this->ui_LoadSaveStateSlotTimerId = -1;
                    this->ui_LoadSaveStateSlot        = -1;
                }
                else
                {
                    this->ui_LoadSaveStateSlotTimerId = this->startTimer(500);
                }
            }
            else if (this->ui_LoadSaveStateSlotTimerId != -1 && value != 0)
            {
                this->ui_LoadSaveStateSlotCounter = 0;
                this->ui_LoadSaveStateSlotTimerId = -1;
                this->ui_LoadSaveStateSlot        = -1;
            }
            else if (value == 0)
            {
                OnScreenDisplaySetMessage("Failed to load save state.");
            }
            else if (!this->ui_ManuallyLoadedState)
            {
                OnScreenDisplaySetMessage("Loaded save state.");
            }

            this->ui_ManuallyLoadedState = false;
        } break;
        case CoreStateCallbackType::SaveStateSaved:
        {
            if (value == 0)
            {
                OnScreenDisplaySetMessage("Failed to save state.");
            }
            else if (!this->ui_ManuallySavedState)
            {
                OnScreenDisplaySetMessage("Saved state.");
            }

            // refresh savestate slot times in 1 second,
            // kill any existing timers
            if (this->ui_UpdateSaveStateSlotTimerId != 0)
            {
                this->killTimer(this->ui_UpdateSaveStateSlotTimerId);
                this->ui_UpdateSaveStateSlotTimerId = 0;
            }
            this->ui_UpdateSaveStateSlotTimerId = this->startTimer(1000);

            this->action_Rollback_LoadState->setEnabled(this->ui_RollbackDebugState.buffer != nullptr);

            this->ui_ManuallySavedState = false;
        } break;
        case CoreStateCallbackType::ScreenshotCaptured:
        {
            if (value == 0)
            {
                OnScreenDisplaySetMessage("Failed to capture screenshot.");
            }
            else
            {
                OnScreenDisplaySetMessage("Captured screenshot.");
            }
        } break;
    }
}

void MainWindow::on_Action_Rollback_SaveState(void)
{
    if (!RollbackDebugEnsureStableFrameControl())
    {
        this->showErrorMessage("Save Rollback State Failed", QString::fromStdString(CoreGetError()));
        return;
    }

    CoreRollbackFreeGameState(this->ui_RollbackDebugState);
    this->action_Rollback_LoadState->setEnabled(false);

    if (!CoreRollbackSaveGameState(this->ui_RollbackDebugState, CoreGetCurrentFrameCount()))
    {
        this->showErrorMessage("Save Rollback State Failed", QString::fromStdString(CoreGetError()));
        return;
    }

    this->action_Rollback_LoadState->setEnabled(true);
    OnScreenDisplaySetMessage("Saved rollback state: " + std::to_string(this->ui_RollbackDebugState.len) + " bytes");
}

void MainWindow::on_Action_Rollback_LoadState(void)
{
    if (!RollbackDebugEnsureStableFrameControl())
    {
        this->showErrorMessage("Load Rollback State Failed", QString::fromStdString(CoreGetError()));
        return;
    }

    if (this->ui_RollbackDebugState.buffer == nullptr)
    {
        return;
    }

    if (!CoreRollbackLoadGameState(this->ui_RollbackDebugState))
    {
        this->showErrorMessage("Load Rollback State Failed", QString::fromStdString(CoreGetError()));
        return;
    }

    OnScreenDisplaySetMessage("Loaded rollback state from frame " + std::to_string(this->ui_RollbackDebugState.frame));
}

void MainWindow::on_Action_Rollback_StartDebugReplay(void)
{
    if (!RollbackDebugEnsureStableFrameControl())
    {
        this->showErrorMessage("Debug Replay Pause Failed", QString::fromStdString(CoreGetError()));
        return;
    }

    CoreRollbackFreeGameState(g_RollbackDebugReplay.initialState);
    CoreRollbackFreeGameState(g_RollbackDebugReplay.finalState);
    {
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        g_RollbackDebugReplay.inputs.clear();
        g_RollbackDebugReplay.inputHashes.clear();
        g_RollbackDebugReplay.frameHashes.clear();
        g_RollbackDebugReplay.verifyInputIndex = 0;
        g_RollbackDebugReplay.verifyFrameIndex = 0;
        g_RollbackDebugReplay.firstInputMismatchFrame = -1;
        g_RollbackDebugReplay.firstInputMismatchExpectedHash = 0;
        g_RollbackDebugReplay.firstInputMismatchActualHash = 0;
        g_RollbackDebugReplay.firstMismatchFrame = -1;
        g_RollbackDebugReplay.firstMismatchExpectedHash = 0;
        g_RollbackDebugReplay.firstMismatchActualHash = 0;
        g_RollbackDebugReplay.recordedInputHash = 0;
        g_RollbackDebugReplay.replayedInputHash = 0;
        g_RollbackDebugReplay.finalHash = 0;
        g_RollbackDebugReplay.frameAdvancedThisFrame = false;
        g_RollbackDebugReplay.pendingInitialSave = true;
        g_RollbackDebugReplay.pendingInitialLoad = false;
        g_RollbackDebugReplay.countReplayInputHash = true;
        g_RollbackDebugReplay.verifyWithGraphics = false;
        g_RollbackDebugReplay.stressRollbackCount = 0;
        g_RollbackDebugReplay.stressRollbackFrames = 0;
        g_RollbackDebugReplay.stressResimulatedFrames = 0;
        g_RollbackDebugReplay.ready = false;
        g_RollbackDebugReplay.mode = RollbackDebugReplayMode::Idle;
    }

    if (!CoreRollbackSetDeterministic(true))
    {
        this->showErrorMessage("Debug Replay Deterministic Mode Failed", QString::fromStdString(CoreGetError()));
        return;
    }

    rmgk_gekko::set_debug_hooks(RollbackDebugReplayInputProvider, RollbackDebugReplayBeginFrame, RollbackDebugReplayEndFrame, nullptr);
    CoreSetFrameOutput(CoreFrameOutput_All);
    rmgk_gekko::set_debug_frame_output(CoreFrameOutput_All);

    {
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        g_RollbackDebugReplay.mode = RollbackDebugReplayMode::Recording;
    }

    this->updateActions(true, true);
    this->setDebugReplayStatusMessage("Recording debug replay for " + std::to_string(kRollbackDebugReplayFrames) + " frames");

    QTimer* progressTimer = new QTimer(this);
    progressTimer->setInterval(250);
    connect(progressTimer, &QTimer::timeout, this, [this, progressTimer]()
    {
        RollbackDebugReplayMode mode;
        bool ready;
        size_t recordedInputFrames;
        uint64_t finalHash;
        {
            std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
            mode = g_RollbackDebugReplay.mode;
            ready = g_RollbackDebugReplay.ready;
            recordedInputFrames = g_RollbackDebugReplay.inputs.size();
            finalHash = g_RollbackDebugReplay.finalHash;
        }

        if (mode == RollbackDebugReplayMode::Recording)
        {
            this->setDebugReplayStatusMessage("Recording debug replay: " + std::to_string(recordedInputFrames) +
                "/" + std::to_string(kRollbackDebugReplayFrames) + " frames");
            return;
        }

        progressTimer->stop();
        progressTimer->deleteLater();
        this->updateActions(true, false);
        if (ready)
        {
            this->setDebugReplayStatusMessage("Recorded debug replay: " + std::to_string(recordedInputFrames) +
                " input frames, payload hash " + std::to_string(finalHash) + ", wrote " + kRollbackDebugReplayFilePath +
                " and " + GetRollbackDebugReplayLogPathString());
        }
        else
        {
            this->setDebugReplayStatusMessage("Debug replay recording stopped before a replay was written; wrote " +
                GetRollbackDebugReplayLogPathString());
        }
    });
    progressTimer->start();
}

void MainWindow::on_Action_Rollback_VerifyDebugReplay(void)
{
    this->startVerifyDebugReplay(false);
}

void MainWindow::on_Action_Rollback_VerifyDebugReplayWithGraphics(void)
{
    this->startVerifyDebugReplay(true);
}

void MainWindow::on_Action_Rollback_StressDebugReplay(void)
{
    this->startVerifyDebugReplay(true, true);
}

void MainWindow::on_Action_Rollback_ClientInputReplay(bool checked)
{
    if (!rmgk_gekko::toggle_client_input_replay())
    {
        this->action_Rollback_ClientInputReplay->setChecked(false);
        OnScreenDisplaySetMessage("Client input replay only works on the rollback client while netplay is active.");
        return;
    }

    if (checked)
    {
        OnScreenDisplaySetMessage("Client input replay: recording 600 frames, then looping them.");
    }
    else
    {
        OnScreenDisplaySetMessage("Client input replay disabled.");
    }
}

void MainWindow::startVerifyDebugReplay(bool withGraphics, bool stress)
{
    if (!RollbackDebugEnsureStableFrameControl())
    {
        this->showErrorMessage("Debug Replay Pause Failed", QString::fromStdString(CoreGetError()));
        return;
    }

    std::string replayFileError;
    if (!LoadRollbackDebugReplayFile(replayFileError))
    {
        this->setDebugReplayStatusMessage("Could not load debug replay: " + replayFileError);
        WriteRollbackDebugReplayEventLog("verify_load_failed", replayFileError);
        return;
    }
    FreeRollbackDebugStressCheckpoints();

    {
        std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
        if (!g_RollbackDebugReplay.ready || g_RollbackDebugReplay.initialState.buffer == nullptr)
        {
            this->setDebugReplayStatusMessage("No debug replay recorded yet");
            return;
        }
        g_RollbackDebugReplay.verifyInputIndex = 0;
        g_RollbackDebugReplay.verifyFrameIndex = 0;
        g_RollbackDebugReplay.firstInputMismatchFrame = -1;
        g_RollbackDebugReplay.firstInputMismatchExpectedHash = 0;
        g_RollbackDebugReplay.firstInputMismatchActualHash = 0;
        g_RollbackDebugReplay.firstMismatchFrame = -1;
        g_RollbackDebugReplay.firstMismatchExpectedHash = 0;
        g_RollbackDebugReplay.firstMismatchActualHash = 0;
        g_RollbackDebugReplay.replayedInputHash = 0;
        g_RollbackDebugReplay.frameAdvancedThisFrame = false;
        g_RollbackDebugReplay.pendingInitialSave = false;
        g_RollbackDebugReplay.pendingInitialLoad = true;
        g_RollbackDebugReplay.countReplayInputHash = true;
        g_RollbackDebugReplay.verifyWithGraphics = withGraphics;
        g_RollbackDebugReplay.lastVerifyCompleted = false;
        g_RollbackDebugReplay.lastVerifyMatched = false;
        g_RollbackDebugReplay.lastVerifyExpectedHash = 0;
        g_RollbackDebugReplay.lastVerifyActualHash = 0;
        g_RollbackDebugReplay.lastVerifyRecordedInputFrames = 0;
        g_RollbackDebugReplay.lastVerifyReplayedInputFrames = 0;
        g_RollbackDebugReplay.stressRollbackCount = 0;
        g_RollbackDebugReplay.stressRollbackFrames = 0;
        g_RollbackDebugReplay.stressResimulatedFrames = 0;
        g_RollbackDebugReplay.mode = stress ? RollbackDebugReplayMode::Stressing : RollbackDebugReplayMode::Verifying;
    }

    if (!CoreRollbackSetDeterministic(true))
    {
        this->showErrorMessage("Debug Replay Deterministic Mode Failed", QString::fromStdString(CoreGetError()));
        return;
    }

    rmgk_gekko::set_debug_hooks(RollbackDebugReplayInputProvider, RollbackDebugReplayBeginFrame, RollbackDebugReplayEndFrame, nullptr);
    CoreSetFrameOutput(withGraphics ? CoreFrameOutput_All : CoreFrameOutput_None);
    rmgk_gekko::set_debug_frame_output(withGraphics ? CoreFrameOutput_All : CoreFrameOutput_None);

    this->updateActions(true, true);
    this->setDebugReplayStatusMessage(std::string(stress ? "Stress verifying debug replay" : "Verifying debug replay") +
        (withGraphics ? " with graphics" : "") + " for " + std::to_string(kRollbackDebugReplayFrames) + " frames");
    WriteRollbackDebugReplayEventLog(stress ? "stress_started" : "verify_started",
        stress ? "with graphics; rollback 2 frames every 5 frames" : (withGraphics ? "with graphics" : "hidden"));

    QTimer* progressTimer = new QTimer(this);
    progressTimer->setInterval(250);
    connect(progressTimer, &QTimer::timeout, this, [this, progressTimer, withGraphics, stress]()
    {
        RollbackDebugReplayMode mode;
        bool completed;
        bool matched;
        uint64_t expectedHash;
        uint64_t actualHash;
        size_t verifyFrameIndex;
        size_t recordedInputFrames;
        size_t replayedInputFrames;
        {
            std::lock_guard<std::mutex> lock(g_RollbackDebugReplay.mutex);
            mode = g_RollbackDebugReplay.mode;
            completed = g_RollbackDebugReplay.lastVerifyCompleted;
            matched = g_RollbackDebugReplay.lastVerifyMatched;
            expectedHash = g_RollbackDebugReplay.lastVerifyExpectedHash;
            actualHash = g_RollbackDebugReplay.lastVerifyActualHash;
            verifyFrameIndex = g_RollbackDebugReplay.verifyFrameIndex;
            recordedInputFrames = g_RollbackDebugReplay.lastVerifyRecordedInputFrames != 0 ?
                g_RollbackDebugReplay.lastVerifyRecordedInputFrames : g_RollbackDebugReplay.inputs.size();
            replayedInputFrames = g_RollbackDebugReplay.lastVerifyReplayedInputFrames != 0 ?
                g_RollbackDebugReplay.lastVerifyReplayedInputFrames : g_RollbackDebugReplay.verifyInputIndex;
        }

        if (RollbackDebugIsPlaybackMode(mode))
        {
            this->setDebugReplayStatusMessage(std::string(stress ? "Stress verifying debug replay" : "Verifying debug replay") +
                (withGraphics ? " with graphics: " : ": ") +
                std::to_string(verifyFrameIndex) + "/" + std::to_string(kRollbackDebugReplayFrames) + " frames");
            return;
        }

        progressTimer->stop();
        progressTimer->deleteLater();
        this->updateActions(true, false);
        if (!completed)
        {
            this->setDebugReplayStatusMessage("Debug replay verify stopped before completion; wrote " +
                GetRollbackDebugReplayLogPathString());
            return;
        }

        if (matched)
        {
            this->setDebugReplayStatusMessage("Debug replay matched: " + std::to_string(replayedInputFrames) +
                "/" + std::to_string(recordedInputFrames) + " input frames, payload hash " +
                std::to_string(actualHash) + ", wrote " + GetRollbackDebugReplayLogPathString());
        }
        else
        {
            this->setDebugReplayStatusMessage("Debug replay mismatch: expected " + std::to_string(expectedHash) +
                ", got " + std::to_string(actualHash) + ", replayed " + std::to_string(replayedInputFrames) +
                "/" + std::to_string(recordedInputFrames) + " input frames, wrote " + GetRollbackDebugReplayLogPathString());
        }
    });
    progressTimer->start();
}

void MainWindow::on_VidExt_Quit(void)
{
    if (this->ui_VidExtRenderMode == VidExtRenderMode::OpenGL)
    {
        this->ui_Widgets->removeWidget(this->ui_Widget_OpenGL->GetWidget());
        this->ui_Widget_OpenGL->GetWidget()->deleteLater();
        this->ui_Widget_OpenGL = nullptr;
    }
    else if (this->ui_VidExtRenderMode == VidExtRenderMode::Vulkan)
    {
        this->ui_Widgets->removeWidget(this->ui_Widget_Vulkan->GetWidget());
        this->ui_Widget_Vulkan->GetWidget()->deleteLater();
        this->ui_Widget_Vulkan = nullptr;
    }

    this->ui_VidExtRenderMode = VidExtRenderMode::Invalid;
}
