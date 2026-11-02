#pragma once

class QCommandLineParser;

namespace KailleraExport
{

// Same overall shape as KrecMp4Export's IsReplayExportRequested()/
// RunReplayExportFromCommandLine(), but for producing a .rmgr file instead
// of an .mp4 - headless and unthrottled, driven by the same PIF-level krec
// input replay, just without any video/audio capture or encoding.
bool IsReplayFileExportRequested(const QCommandLineParser& parser);
int RunReplayFileExportFromCommandLine(const QCommandLineParser& parser);

} // namespace KailleraExport
