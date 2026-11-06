#pragma once

#include "EmulatorProxy.hpp"
#include "KrecParser.hpp"

namespace KailleraExport
{

void InitializePifReplay(const KrecData* krecData);
void ResetPifReplayFrameSync(void);
// input records the replay has read
int GetPifReplayFrameIndex(void);
bool IsPifReplayFinished(void);
void PifReplayCallback(struct pif* pifState);

} // namespace KailleraExport
