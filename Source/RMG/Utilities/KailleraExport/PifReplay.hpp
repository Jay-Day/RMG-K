#pragma once

#include "EmulatorProxy.hpp"
#include "KrecParser.hpp"

namespace KailleraExport
{

void InitializePifReplay(const KrecData* krecData);
void ResetPifReplayFrameSync(void);
bool IsPifReplayFinished(void);
// input records consumed so far
int GetPifReplayFrameIndex(void);
void PifReplayCallback(struct pif* pifState);

} // namespace KailleraExport
