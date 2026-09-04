#pragma once

#include "EmulatorProxy.hpp"
#include "KrecParser.hpp"

namespace KailleraExport
{

void InitializePifReplay(const KrecData* krecData);
void ResetPifReplayFrameSync(void);
bool IsPifReplayFinished(void);
void PifReplayCallback(struct pif* pifState);

// How many of the .krec's own input frames have been consumed so far this
// export - i.e. how many real emulated frames into the *original* recording
// session this point in headless replay corresponds to, independent of how
// fast the headless emulator itself is currently running (up to 2000%
// speed - see ReplayFileExport.cpp). Assuming the original recording ran at
// a constant 60fps (true for any real Kaillera session), this frame count
// divided by 60 is the number of real-world seconds elapsed since the
// .krec's own recording-start timestamp - see Replay::SetRecordedAtBaseOverride().
int GetPifReplayFrameIndex(void);

} // namespace KailleraExport
