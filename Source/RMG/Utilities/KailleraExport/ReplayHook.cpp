#include "ReplayHook.hpp"

// Deliberately never includes EmulatorProxy.hpp - see ReplayHook.hpp's own
// comment for why these two header worlds can't coexist in one TU.
#ifdef RMGK_GAME_STATS
#include <RMG-Core/Replay.hpp>
#include <RMG-Core/m64p/Api.hpp>
#endif

namespace KailleraExport
{

#ifdef RMGK_GAME_STATS

bool HookReplayCoreApi(void* coreLibraryHandle)
{
    return m64p::Core.Hook(static_cast<m64p_dynlib_handle>(coreLibraryHandle));
}

bool IsReplayCoreApiHooked()
{
    return m64p::Core.IsHooked();
}

void ReplaySetEnabledOverride(bool enabled)
{
    Replay::SetEnabledOverride(enabled);
}

void ReplaySetOutputPathOverride(const std::string& path)
{
    Replay::SetOutputPathOverride(path);
}

void ReplaySetPlayerNamesOverride(const std::array<std::string, 4>& names)
{
    Replay::SetPlayerNamesOverride(names);
}

void ReplaySetRecordedAtBaseOverride(uint64_t krecBaseEpochSeconds, int (*frameIndexProvider)(void))
{
    Replay::SetRecordedAtBaseOverride(krecBaseEpochSeconds, frameIndexProvider);
}

void ReplayOnEmulationStart()
{
    Replay::OnEmulationStart();
}

void ReplayOnFrame()
{
    Replay::OnFrame();
}

void ReplayOnEmulationStop()
{
    Replay::OnEmulationStop();
}

#else // !RMGK_GAME_STATS

bool HookReplayCoreApi(void*)
{
    return false;
}

bool IsReplayCoreApiHooked()
{
    return false;
}

void ReplaySetEnabledOverride(bool) {}
void ReplaySetOutputPathOverride(const std::string&) {}
void ReplaySetPlayerNamesOverride(const std::array<std::string, 4>&) {}
void ReplaySetRecordedAtBaseOverride(uint64_t, int (*)(void)) {}
void ReplayOnEmulationStart() {}
void ReplayOnFrame() {}
void ReplayOnEmulationStop() {}

#endif

} // namespace KailleraExport
