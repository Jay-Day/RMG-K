/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef M64P_COREAPI_HPP
#define M64P_COREAPI_HPP

#include "api/m64p_common.h"
#include "api/m64p_frontend.h"
#include "api/m64p_debugger.h"

#include <string>

namespace m64p
{
class CoreApi
{
  public:
    CoreApi();
    ~CoreApi();

    CoreApi(const CoreApi&) = delete;

    bool Hook(m64p_dynlib_handle handle);
    bool Unhook(void);
    bool IsHooked(void);

    m64p_dynlib_handle GetHandle(void);
    std::string GetLastError(void);

    ptr_CoreStartup Startup;
    ptr_CoreShutdown Shutdown;
    ptr_CoreAttachPlugin AttachPlugin;
    ptr_CoreDetachPlugin DetachPlugin;
    ptr_CoreDoCommand DoCommand;
    ptr_CoreOverrideVidExt OverrideVidExt;
    ptr_CoreAddCheat AddCheat;
    ptr_CoreCheatEnabled CheatEnabled;
    ptr_CoreGetRomSettings GetRomSettings;
    ptr_CoreGetAPIVersions GetAPIVersions;
    ptr_CoreErrorMessage ErrorMessage;

    // debugger memory access (used by GameStats; requires the core to be
    // built with DEBUGGER=1, otherwise these are no-ops that return 0/NULL)
    ptr_DebugMemGetPointer DebugMemGetPointer;
    ptr_DebugMemRead8 DebugMemRead8;
    ptr_DebugMemRead16 DebugMemRead16;
    ptr_DebugMemRead32 DebugMemRead32;

  private:
    bool hooked = false;

    std::string errorMessage;
    m64p_dynlib_handle handle;
};
} // namespace m64p

#endif // M64P_COREAPI_HPP
