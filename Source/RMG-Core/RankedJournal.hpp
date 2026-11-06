/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_RANKEDJOURNAL_HPP
#define CORE_RANKEDJOURNAL_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

//
// Ranked write journal: the memory writes a ranked game module made during a
// match. A ranked .krec only plays back in sync when the same writes are made
// again, so every replayer (playback, live spectators, exporters) applies the
// journal at the same frame boundaries instead of running the module.
//
// Frames are placed by their position in the input recording, which any
// replayer can reproduce: Record = .krec input records before the frame,
// Offset = frames since the frame of the last of those records.
//

constexpr uint8_t kCoreRankedJournalWrite8      = 1;
constexpr uint8_t kCoreRankedJournalWrite16     = 2;
constexpr uint8_t kCoreRankedJournalWrite32     = 4;
constexpr uint8_t kCoreRankedJournalWriteCode32 = 0x84; // + cached code invalidation

struct CoreRankedJournalWrite
{
    uint8_t  Kind = 0;
    uint32_t Address = 0;
    uint32_t Value = 0;
    // what the address held before the write: a replay whose memory differs
    // there has diverged from the match
    uint32_t Before = 0;
};

struct CoreRankedJournalFrame
{
    int64_t  Frame = 0;  // live frame, informational
    int      Record = -1; // -1 until known (live only)
    uint32_t Offset = 0;
    std::vector<CoreRankedJournalWrite> Writes;
};

// invalidates cached (recompiled) code, see the core's RmgkInvalidateCachedCode
using CoreRankedInvalidateCodeFn = void (*)(unsigned int address, unsigned int size);

// Applies a journal while a recording plays.
struct CoreRankedJournalPlayer
{
    std::vector<CoreRankedJournalFrame> Frames;
    size_t   Next = 0;
    int      Base = 0;     // record index of the reader's local record 0
    int      Consumed = 0; // records consumed so far
    uint32_t Since = 0;    // frames since the last consumed record
    int      Rebase = -1;  // pending CoreRankedJournalRebase record
    size_t   Applied = 0;  // frames applied so far
    bool     Started = false;
    bool     MismatchReported = false;
};

// encodes frames [begin, end); frames without a record are placed at
// `endRecord` (the end of the recording). Format 3, little-endian, per frame:
//   int32 frame, int32 record, uint32 offset, uint16 count,
//   count x { uint8 kind, uint32 address, uint32 value, uint32 before }
std::string CoreRankedEncodeJournal(const std::vector<CoreRankedJournalFrame>& frames,
    size_t begin, size_t end, int endRecord);

// decodes a journal, false when malformed or out of order
bool CoreRankedDecodeJournal(const std::string& data, std::vector<CoreRankedJournalFrame>& frames);

// writes RDRAM (a virtual or physical address), returns whether memory
// changed; `before` receives the previous value
bool CoreRankedWriteMemory(uint8_t* rdram, uint8_t kind, uint32_t address, uint32_t value,
    CoreRankedInvalidateCodeFn invalidate, uint32_t* before = nullptr);

// reads RDRAM the way CoreRankedWriteMemory writes it, false when out of range
bool CoreRankedReadMemory(const uint8_t* rdram, uint8_t kind, uint32_t address, uint32_t& value);

// rewinds the player to the start of its journal, keeping the frames
void CoreRankedJournalReset(CoreRankedJournalPlayer& player);

// appends an encoded chunk (frames already present are skipped), false when malformed
bool CoreRankedJournalAppend(CoreRankedJournalPlayer& player, const std::string& data);

// at each frame boundary (core frame callback): `localRecords` input records
// were consumed so far. Applies the frames up to that position. `mismatch` is
// set once when the replay has diverged: a frame was due earlier, or memory
// about to be written doesn't hold what it held in the match.
void CoreRankedJournalAdvance(CoreRankedJournalPlayer& player, int localRecords, uint8_t* rdram,
    CoreRankedInvalidateCodeFn invalidate, std::string* mismatch);

// after loading a savestate taken right after the frame of record (record - 1)
// (a spectator keyframe), with the reader rewound to make `record` its local
// record 0: skips what the state contains and applies that boundary's writes
void CoreRankedJournalRebase(CoreRankedJournalPlayer& player, int record, uint8_t* rdram,
    CoreRankedInvalidateCodeFn invalidate, std::string* mismatch);

#endif // CORE_RANKEDJOURNAL_HPP
