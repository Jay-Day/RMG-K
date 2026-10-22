/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define CORE_INTERNAL
#include "RankedJournal.hpp"
#include "Library.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>

static_assert(std::endian::native == std::endian::little,
    "the ranked journal assumes a little-endian host");

static constexpr uint32_t kRdramSize = 0x800000;

//
// Internal Functions
//

static void put_u16(std::string& out, uint16_t value)
{
    out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

static void put_u32(std::string& out, uint32_t value)
{
    out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

// whether `frame` comes before position (record, offset)
static bool frame_before(const CoreRankedJournalFrame& frame, int record, uint32_t offset)
{
    return frame.Record < record || (frame.Record == record && frame.Offset < offset);
}

static void apply_due(CoreRankedJournalPlayer& player, uint8_t* rdram,
    CoreRankedInvalidateCodeFn invalidate, std::string* mismatch)
{
    while (player.Next < player.Frames.size())
    {
        const CoreRankedJournalFrame& frame = player.Frames[player.Next];
        if (frame.Record > player.Consumed ||
            (frame.Record == player.Consumed && frame.Offset > player.Since))
        {
            break;
        }
        if (player.Started && !player.MismatchReported &&
            (frame.Record != player.Consumed || frame.Offset != player.Since))
        {
            player.MismatchReported = true;
            if (mismatch != nullptr)
            {
                *mismatch = "out of step with the recording at input " + std::to_string(player.Consumed) +
                    " (+" + std::to_string(player.Since) + " frames), expected input " +
                    std::to_string(frame.Record) + " (+" + std::to_string(frame.Offset) + ")";
            }
        }
        for (const CoreRankedJournalWrite& write : frame.Writes)
        {
            uint32_t current = 0;
            if (!player.MismatchReported && CoreRankedReadMemory(rdram, write.Kind, write.Address, current) &&
                current != write.Before)
            {
                player.MismatchReported = true;
                if (mismatch != nullptr)
                {
                    char detail[160];
                    std::snprintf(detail, sizeof(detail),
                        "game memory differs from the recording at input %d (+%u frames): %08X holds %X, was %X",
                        player.Consumed, player.Since, write.Address, current, write.Before);
                    *mismatch = detail;
                }
            }
            CoreRankedWriteMemory(rdram, write.Kind, write.Address, write.Value, invalidate);
        }
        player.Next++;
        player.Applied++;
    }
    player.Started = true;
}

//
// Exported Functions
//

CORE_EXPORT std::string CoreRankedEncodeJournal(const std::vector<CoreRankedJournalFrame>& frames,
    size_t begin, size_t end, int endRecord)
{
    std::string out;
    end = std::min(end, frames.size());
    for (size_t index = begin; index < end; index++)
    {
        const CoreRankedJournalFrame& frame = frames[index];
        const size_t count = std::min<size_t>(frame.Writes.size(), 0xFFFF);
        put_u32(out, static_cast<uint32_t>(frame.Frame));
        put_u32(out, static_cast<uint32_t>(frame.Record < 0 ? endRecord : frame.Record));
        put_u32(out, frame.Record < 0 ? 0 : frame.Offset);
        put_u16(out, static_cast<uint16_t>(count));
        for (size_t i = 0; i < count; i++)
        {
            out.push_back(static_cast<char>(frame.Writes[i].Kind));
            put_u32(out, frame.Writes[i].Address);
            put_u32(out, frame.Writes[i].Value);
            put_u32(out, frame.Writes[i].Before);
        }
    }
    return out;
}

CORE_EXPORT bool CoreRankedDecodeJournal(const std::string& data, std::vector<CoreRankedJournalFrame>& frames)
{
    frames.clear();
    size_t pos = 0;
    auto read = [&](void* out, size_t size)
    {
        if (data.size() - pos < size)
        {
            return false;
        }
        std::memcpy(out, data.data() + pos, size);
        pos += size;
        return true;
    };
    while (pos < data.size())
    {
        uint32_t frameNumber, record, offset;
        uint16_t count;
        if (!read(&frameNumber, 4) || !read(&record, 4) || !read(&offset, 4) || !read(&count, 2))
        {
            return false;
        }
        CoreRankedJournalFrame frame;
        frame.Frame  = static_cast<int32_t>(frameNumber);
        frame.Record = static_cast<int32_t>(record);
        frame.Offset = offset;
        if (frame.Record < 0)
        {
            return false;
        }
        frame.Writes.resize(count);
        for (CoreRankedJournalWrite& write : frame.Writes)
        {
            if (!read(&write.Kind, 1) || !read(&write.Address, 4) || !read(&write.Value, 4) ||
                !read(&write.Before, 4))
            {
                return false;
            }
        }
        if (!frames.empty() && !frame_before(frames.back(), frame.Record, frame.Offset))
        {
            return false;
        }
        frames.push_back(std::move(frame));
    }
    return true;
}

// physical RDRAM offset of a write, false when out of range
static bool rdram_offset(uint8_t kind, uint32_t address, uint32_t& physical)
{
    const uint32_t size = (kind == kCoreRankedJournalWrite8) ? 1 : (kind == kCoreRankedJournalWrite16) ? 2 : 4;
    physical = (address & 0x1FFFFFFF) & ~(size - 1);
    return physical < kRdramSize && kRdramSize - physical >= size;
}

CORE_EXPORT bool CoreRankedReadMemory(const uint8_t* rdram, uint8_t kind, uint32_t address, uint32_t& value)
{
    uint32_t physical;
    if (rdram == nullptr || !rdram_offset(kind, address, physical))
    {
        return false;
    }
    // RDRAM is stored as native 32-bit words: bytes and halfwords are swizzled
    if (kind == kCoreRankedJournalWrite8)
    {
        value = rdram[physical ^ 3];
    }
    else if (kind == kCoreRankedJournalWrite16)
    {
        uint16_t half;
        std::memcpy(&half, rdram + (physical ^ 2), sizeof(half));
        value = half;
    }
    else
    {
        std::memcpy(&value, rdram + physical, sizeof(value));
    }
    return true;
}

CORE_EXPORT bool CoreRankedWriteMemory(uint8_t* rdram, uint8_t kind, uint32_t address, uint32_t value,
    CoreRankedInvalidateCodeFn invalidate, uint32_t* before)
{
    uint32_t physical;
    if (rdram == nullptr || !rdram_offset(kind, address, physical))
    {
        return false;
    }
    if (before != nullptr)
    {
        CoreRankedReadMemory(rdram, kind, address, *before);
    }

    // RDRAM is stored as native 32-bit words: bytes and halfwords are swizzled
    switch (kind)
    {
    case kCoreRankedJournalWrite8:
    {
        uint8_t& target = rdram[physical ^ 3];
        if (target == static_cast<uint8_t>(value))
        {
            return false;
        }
        target = static_cast<uint8_t>(value);
        return true;
    }
    case kCoreRankedJournalWrite16:
    {
        uint16_t current, value16 = static_cast<uint16_t>(value);
        std::memcpy(&current, rdram + (physical ^ 2), sizeof(current));
        if (current == value16)
        {
            return false;
        }
        std::memcpy(rdram + (physical ^ 2), &value16, sizeof(value16));
        return true;
    }
    case kCoreRankedJournalWrite32:
    case kCoreRankedJournalWriteCode32:
    {
        uint32_t current;
        std::memcpy(&current, rdram + physical, sizeof(current));
        if (current == value)
        {
            return false;
        }
        std::memcpy(rdram + physical, &value, sizeof(value));
        if (kind == kCoreRankedJournalWriteCode32 && invalidate != nullptr)
        {
            invalidate(address & ~3u, 4);
        }
        return true;
    }
    default:
        return false;
    }
}

CORE_EXPORT void CoreRankedJournalReset(CoreRankedJournalPlayer& player)
{
    player.Next = 0;
    player.Base = 0;
    player.Consumed = 0;
    player.Since = 0;
    player.Rebase = -1;
    player.Started = false;
    player.MismatchReported = false;
    player.Applied = 0;
}

CORE_EXPORT bool CoreRankedJournalAppend(CoreRankedJournalPlayer& player, const std::string& data)
{
    std::vector<CoreRankedJournalFrame> frames;
    if (!CoreRankedDecodeJournal(data, frames))
    {
        return false;
    }
    for (CoreRankedJournalFrame& frame : frames)
    {
        if (player.Frames.empty() || frame_before(player.Frames.back(), frame.Record, frame.Offset))
        {
            player.Frames.push_back(std::move(frame));
        }
    }
    return true;
}

CORE_EXPORT void CoreRankedJournalAdvance(CoreRankedJournalPlayer& player, int localRecords, uint8_t* rdram,
    CoreRankedInvalidateCodeFn invalidate, std::string* mismatch)
{
    if (player.Rebase >= 0)
    {
        return; // the state is about to be replaced
    }
    const int consumed = player.Base + localRecords;
    if (consumed != player.Consumed)
    {
        player.Consumed = consumed;
        player.Since = 0;
    }
    else
    {
        player.Since++;
    }
    apply_due(player, rdram, invalidate, mismatch);
}

CORE_EXPORT void CoreRankedJournalRebase(CoreRankedJournalPlayer& player, int record, uint8_t* rdram,
    CoreRankedInvalidateCodeFn invalidate, std::string* mismatch)
{
    player.Base     = record;
    player.Consumed = record;
    player.Since    = 0;
    player.Rebase   = -1;
    player.Next     = 0;
    while (player.Next < player.Frames.size() && frame_before(player.Frames[player.Next], record, 0))
    {
        player.Next++;
    }
    player.Started = false;
    apply_due(player, rdram, invalidate, mismatch);
}
