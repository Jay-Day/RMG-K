#include "PluginAPI.h"
#include "Types.h"
#include "mupenplus/GLideN64_mupenplus.h"
#include "N64.h"
#include "RSP.h"

extern "C" {

EXPORT int CALL RomOpen(void)
{
	if (rdram_size != nullptr)
		RDRAMSize = *rdram_size - 1;
	else
		RDRAMSize = 0;

	return api().RomOpen();
}

EXPORT m64p_error CALL PluginGetVersion(
	m64p_plugin_type * _PluginType,
	int * _PluginVersion,
	int * _APIVersion,
	const char ** _PluginNamePtr,
	int * _Capabilities
)
{
	return api().PluginGetVersion(_PluginType, _PluginVersion, _APIVersion, _PluginNamePtr, _Capabilities);
}

EXPORT m64p_error CALL PluginStartup(
	m64p_dynlib_handle CoreLibHandle,
	void *Context,
	void (*DebugCallback)(void *, int, const char *)
)
{
	return api().PluginStartup(CoreLibHandle, Context, DebugCallback);
}

#ifdef M64P_GLIDENUI
EXPORT m64p_error CALL PluginConfig(void* parent)
{
	return api().PluginConfig(parent);
}
#endif // M64P_GLIDENUI

EXPORT m64p_error CALL PluginShutdown(void)
{
	return api().PluginShutdown();
}

EXPORT void CALL ReadScreen2(void *dest, int *width, int *height, int front)
{
	api().ReadScreen2(dest, width, height, front);
}

EXPORT void CALL SetRenderingCallback(void (*callback)(int))
{
	api().SetRenderingCallback(callback);
}

EXPORT void CALL ResizeVideoOutput(int width, int height)
{
	api().ResizeVideoOutput(width, height);
}

// RMG-K: skips rendering display lists (see RSP_SetSkipRendering) and reports
// how many were skipped and how many still rendered since the last call
EXPORT void CALL RmgkSetSkipRendering(int skip, unsigned int * skipped, unsigned int * rendered)
{
	RSP_SetSkipRendering(skip != 0, skipped, rendered);
}

} // extern "C"
