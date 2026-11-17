/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_ROLLBACK_NETCODE_HPP
#define CORE_ROLLBACK_NETCODE_HPP

#include "m64p/api/m64p_types.h"

#include <cstdint>

struct CoreRollbackState
{
    unsigned char* buffer = nullptr;
    int len = 0;
    int checksum = 0;
    int frame = 0;
};

struct CoreRollbackRunFrameStats
{
    uint64_t totalUs = 0;
    uint64_t r4300Us = 0;
    uint64_t viUs = 0;
    uint64_t newFrameUs = 0;
    uint64_t cheatsUs = 0;
    uint64_t pacingUs = 0;
    uint64_t inputUs = 0;
    uint64_t pauseUs = 0;
    uint64_t netplayUs = 0;
    uint64_t dynarecRecompileCount = 0;
    uint64_t dynarecRecompileUs = 0;
    uint64_t dynarecInvalidateUs = 0;
    uint64_t dynarecFullInvalidateCount = 0;
    uint64_t dynarecRangeInvalidateCount = 0;
    uint64_t dynarecBlockInvalidateCount = 0;
    uint64_t dynarecVerifyDirtyCount = 0;
    uint64_t dynarecVerifyDirtyUs = 0;
    uint64_t dynarecGetAddrCount = 0;
    uint64_t dynarecGetAddrUs = 0;
    uint64_t dynarecGetAddrHtCount = 0;
    uint64_t dynarecGetAddr32Count = 0;
    uint64_t dynarecDynamicLinkerCount = 0;
    uint64_t dynarecDynamicLinkerUs = 0;
    uint64_t dynarecDynamicLinkerDsCount = 0;
    uint64_t dynarecDynamicLinkerDsUs = 0;
    uint64_t cachedCodeFullInvalidateCount = 0;
    uint64_t cachedCodeRangeInvalidateCount = 0;
    uint64_t interruptCount = 0;
    uint64_t interruptUs = 0;
    uint64_t interruptMaxUs = 0;
    uint64_t interruptViCount = 0;
    uint64_t interruptViUs = 0;
    uint64_t interruptCompareCount = 0;
    uint64_t interruptCompareUs = 0;
    uint64_t interruptCheckCount = 0;
    uint64_t interruptCheckUs = 0;
    uint64_t interruptSiCount = 0;
    uint64_t interruptSiUs = 0;
    uint64_t interruptPiCount = 0;
    uint64_t interruptPiUs = 0;
    uint64_t interruptAiCount = 0;
    uint64_t interruptAiUs = 0;
    uint64_t interruptSpCount = 0;
    uint64_t interruptSpUs = 0;
    uint64_t interruptDpCount = 0;
    uint64_t interruptDpUs = 0;
    uint64_t interruptRspDmaCount = 0;
    uint64_t interruptRspDmaUs = 0;
    uint64_t interruptRspTaskCount = 0;
    uint64_t interruptRspTaskUs = 0;
    uint64_t aiSetFrequencyCount = 0;
    uint64_t aiSetFrequencyUs = 0;
    uint64_t aiPushSamplesCount = 0;
    uint64_t aiPushSamplesUs = 0;
    uint64_t aiFifoPopCount = 0;
    uint64_t aiFifoPopUs = 0;
    uint64_t aiRaiseInterruptCount = 0;
    uint64_t aiRaiseInterruptUs = 0;
    uint32_t emumode = 0;
    uint32_t interruptMaxType = 0;
    uint32_t cp0CountBefore = 0;
    uint32_t cp0CountAfter = 0;
    uint32_t nextInterruptBefore = 0;
    uint32_t nextInterruptAfter = 0;
    uint32_t pcBefore = 0;
    uint32_t pcAfter = 0;
    uint32_t currentFrameBefore = 0;
    uint32_t currentFrameAfter = 0;
    uint32_t dynarecPcaddrBefore = 0;
    uint32_t dynarecPcaddrAfter = 0;
    uint32_t cp0LastAddrBefore = 0;
    uint32_t cp0LastAddrAfter = 0;
    uint32_t loadBeforePc = 0;
    uint32_t loadBeforeCp0Count = 0;
    uint32_t loadBeforeNextInterrupt = 0;
    uint32_t loadBeforeCurrentFrame = 0;
    uint32_t loadProbePc = 0;
    uint32_t loadProbeCp0Count = 0;
    uint32_t loadProbeNextInterrupt = 0;
    uint32_t loadProbeCurrentFrame = 0;
    uint32_t hiddenBeginPc = 0;
    uint32_t hiddenBeginCp0Count = 0;
    uint32_t hiddenBeginNextInterrupt = 0;
    uint32_t hiddenBeginCurrentFrame = 0;
    uint32_t resumeProbePc = 0;
    uint32_t resumeProbeCp0Count = 0;
    uint32_t resumeProbeNextInterrupt = 0;
    uint32_t resumeProbeCurrentFrame = 0;
    int32_t dynarecCycleCountBefore = 0;
    int32_t dynarecCycleCountAfter = 0;
    int32_t dynarecPendingExceptionBefore = 0;
    int32_t dynarecPendingExceptionAfter = 0;
    int32_t dynarecStopBefore = 0;
    int32_t dynarecStopAfter = 0;
    int32_t loadBeforeCycleCount = 0;
    int32_t loadBeforePendingException = 0;
    int32_t loadBeforeStop = 0;
    int32_t loadProbeCycleCount = 0;
    int32_t loadProbePendingException = 0;
    int32_t loadProbeStop = 0;
    int32_t hiddenBeginCycleCount = 0;
    int32_t hiddenBeginPendingException = 0;
    int32_t hiddenBeginStop = 0;
    int32_t resumeProbeCycleCount = 0;
    int32_t resumeProbePendingException = 0;
    int32_t resumeProbeStop = 0;
    int32_t delaySlotBefore = 0;
    int32_t delaySlotAfter = 0;
    int outputFlags = 0;
};

using CoreRollbackInputCallback = int (*)(void* values, int size, int players);

bool CoreRollbackSaveGameState(CoreRollbackState& state, int frame);
bool CoreRollbackSaveGameStateInto(CoreRollbackState& state, unsigned char* buffer, int capacity, int frame);
// Like CoreRollbackSaveGameStateInto but produces a FULL normal-format savestate (TLB LUT
// included, buffer zeroed) instead of the stripped rollback variant. Use for the spectate
// keyframe so a cold spectator can load a complete state via the normal path. Safe to call
// at GekkoNet's frame-aligned save point — it's read-only on the device and calls no plugins.
bool CoreRollbackSaveFullStateInto(CoreRollbackState& state, unsigned char* buffer, int capacity, int frame);
bool CoreRollbackLoadGameState(const CoreRollbackState& state);
// Like CoreRollbackLoadGameState but the core performs the load at its next safe interrupt
// boundary instead of synchronously. Use this when calling from inside the emulation loop
// (e.g. the PIF/SI callback), where an immediate full-state load would corrupt the
// in-flight SI/dynarec operation. The state buffer must remain valid until the next frame.
bool CoreRollbackLoadGameStateDeferred(const CoreRollbackState& state);
void CoreRollbackFreeGameState(CoreRollbackState& state);
bool CoreRollbackAdvanceFrame(void);
bool CoreRollbackSampleInput(void* values, int size, int players);
bool CoreRollbackSetInputCallback(CoreRollbackInputCallback callback);
bool CoreRollbackSetInputPlayers(int players);
bool CoreRollbackSetDeterministic(bool enabled);
bool CoreRollbackSetVerboseStats(bool enabled);
bool CoreRollbackSetTimesyncScale(double scale);
bool CoreRollbackExecute(m64p_rollback_execute_callbacks& callbacks);
bool CoreRollbackRunFrame(int flags);
bool CoreRollbackGetRunFrameStats(CoreRollbackRunFrameStats& stats);

#endif // CORE_ROLLBACK_NETCODE_HPP
