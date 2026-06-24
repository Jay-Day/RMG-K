# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

RMG-K (Rosalie's Mupen GUI - Kaillera) is a Nintendo 64 emulator frontend built on mupen64plus with Kaillera netplay support. Written in C++20, it uses Qt6 for the UI, SDL3 for input/audio, and integrates with the mupen64plus plugin architecture.

## Build Commands

### Linux (Portable)

**Debian/Ubuntu:**
```bash
sudo apt-get -y install cmake libusb-1.0-0-dev libhidapi-dev libsamplerate0-dev libspeex-dev libminizip-dev libsdl3-dev libfreetype6-dev libgl1-mesa-dev libglu1-mesa-dev pkg-config zlib1g-dev binutils-dev libspeexdsp-dev qt6-base-dev qt6-websockets-dev libqt6svg6-dev libvulkan-dev build-essential nasm git zip ninja-build libcurl4-openssl-dev
./Source/Script/Build.sh Release
```

**Fedora:**
```bash
sudo dnf install libusb1-devel hidapi-devel libsamplerate-devel minizip-compat-devel SDL3-devel freetype-devel mesa-libGL-devel mesa-libGLU-devel pkgconfig zlib-ng-devel binutils-devel speexdsp-devel qt6-qtbase-devel qt6-qtsvg-devel qt6-qtwebsockets-devel vulkan-devel gcc-c++ nasm git ninja-build libcurl-devel
./Source/Script/Build.sh Release
```

**Arch Linux:**
```bash
sudo pacman -S --needed make cmake gcc libusb hidapi freetype2 libpng qt6 sdl3 libsamplerate nasm minizip pkgconf vulkan-headers git curl
./Source/Script/Build.sh Release
```

**OpenSUSE Tumbleweed:**
```bash
sudo zypper install SDL3-devel cmake freetype2-devel gcc gcc-c++ libusb-1_0-devel libhidapi-devel libhidapi-hidraw0 libpng16-devel libsamplerate-devel make nasm ninja pkgconf-pkg-config speex-devel vulkan-devel zlib-devel qt6-tools-devel qt6-opengl-devel qt6-widgets-devel qt6-svg-devel minizip-devel git libcurl-devel
./Source/Script/Build.sh Release
```

Executables are found in `Bin/Release` after building.

### Linux (System Installation)

For packaging/distribution:
```bash
export src_dir="$(pwd)"
export build_dir="$(pwd)/build"
mkdir -p "$build_dir"
cmake -S "$src_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE="Release" -DPORTABLE_INSTALL="OFF" -DCMAKE_INSTALL_PREFIX="/usr" -G "Ninja"
cmake --build "$build_dir"
cmake --install "$build_dir" --prefix="/usr"
```

### Windows (MSYS2 UCRT64)

```bash
pacman -S --needed make mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-libusb mingw-w64-ucrt-x86_64-hidapi mingw-w64-ucrt-x86_64-freetype mingw-w64-ucrt-x86_64-libpng mingw-w64-ucrt-x86_64-qt6 mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-speexdsp mingw-w64-ucrt-x86_64-libsamplerate mingw-w64-ucrt-x86_64-libusb mingw-w64-ucrt-x86_64-nasm mingw-w64-ucrt-x86_64-minizip mingw-w64-ucrt-x86_64-vulkan-headers git
./Source/Script/Build.sh Release
```

Executables are found in `Bin/Release` after building.

### CMake Build Options

Configure via `-D` flags in CMake command:

- `PORTABLE_INSTALL` (ON): Portable installation (all files in Bin/Release)
- `NETPLAY` (ON): Enables netplay support
- `UPDATER` (WIN32): Enables application updater
- `APPIMAGE_UPDATER` (OFF): Enables AppImage updater on Linux
- `USE_ANGRYLION` (OFF): Build angrylion-rdp-plus (non-GPL license)
- `NO_ASM` (OFF): Disables assembly in mupen64plus-core
- `USE_CCACHE` (ON): Use ccache if available

## Architecture Overview

### Component Structure

The project uses a modular architecture with clear separation:

```
RMG (Qt6 GUI) → RMG-Core (Shared Library) → mupen64plus-core + Plugins
                      ↓
        ┌─────────────┼──────────────┬──────────────┐
        ↓             ↓              ↓              ↓
    RMG-Input    RMG-Audio    RMG-Input-GCA    mupen64plus
   (Plugin)     (Plugin)      (Plugin)         plugins
```

### Key Components

**RMG** (`Source/RMG/`):
- Qt6-based user interface and main application
- MainWindow: Primary window, ROM browser, settings
- EmulationThread: QThread running mupen64plus in background
- KailleraSessionManager: Qt wrapper for Kaillera netplay (Windows only)
- OnScreenDisplay: ImGui-based OSD rendering
- Dialogs: Settings, cheats, netplay, ROM info, updates

**RMG-Core** (`Source/RMG-Core/`):
- Shared library providing C++20 abstraction over mupen64plus
- `m64p/`: API wrappers (CoreApi, PluginApi, ConfigApi)
- `Emulation.cpp`: Lifecycle management (start/stop/pause/reset)
- `Plugins.cpp`: Plugin discovery, loading, and attachment
- `Kaillera.cpp`: Windows-only Kaillera DLL wrapper for netplay
- `VidExt.cpp`: Video Extension override for Qt widget rendering
- ROM/SaveState/Cheats/Settings management

**RMG-Input** (`Source/RMG-Input/`):
- mupen64plus input plugin using SDL3
- Per-axis scaling with configurable range (default 66% for N-Rage compatibility)
- Per-axis deadzone (not circular) for USBtoN64v2 adapter compatibility
- Hotkey system and controller pak support

**RMG-Audio** (`Source/RMG-Audio/`):
- mupen64plus audio plugin with SDL3 backend
- Multiple resampling algorithms (trivial, speex, libsamplerate)

**RMG-Input-GCA** (`Source/RMG-Input-GCA/`):
- Specialized input plugin for GameCube controller adapters

### Critical Architecture Patterns

#### 1. mupen64plus Plugin Architecture

RMG-K extends mupen64plus with custom plugins:
- **RSP**: Reality Signal Processor emulation (HLE/CXD4/Parallel)
- **GFX**: Graphics rendering (GLideN64/Parallel/Angrylion)
- **Audio**: Audio output (RMG-Audio)
- **Input**: Controller input (RMG-Input/RMG-Input-GCA)

Plugins are dynamically loaded libraries with standardized APIs. RMG adds custom extensions:
- `PluginConfig()`: GUI configuration dialog
- `PluginConfigWithRomConfig()`: ROM-specific settings

#### 2. Video Extension (VidExt) Override

Instead of SDL windowing, RMG provides custom video extension:
1. RMG-Core overrides VidExt functions via `CoreOverrideVidExt()`
2. Graphics plugins call these instead of SDL
3. RMG creates OpenGL/Vulkan contexts in Qt widgets (OGLWidget/VKWidget)
4. Enables proper Qt integration and fullscreen handling

#### 3. Kaillera Netplay Synchronization (Windows Only)

The most complex subsystem. Architecture:
```
Kaillera Server ↔ kailleraclient.dll ↔ RMG-Core (Kaillera.cpp) ↔ KailleraSessionManager ↔ MainWindow
```

**Synchronization Flow (every frame)**:
1. mupen64plus-core polls controllers via PIF (Peripheral Interface)
2. `KailleraPifSyncCallback()` in `Emulation.cpp` intercepts first JCMD_CONTROLLER_READ
3. Reads local controller state from PIF RAM
4. Calls `CoreModifyKailleraPlayValues()` with 4-byte input buffer
5. Kaillera DLL sends local input to server, receives all players' synchronized inputs
6. Synchronized inputs cached and written back to PIF RAM
7. Subsequent PIF polls in same frame use cached data
8. Frame advances, sync flag reset, repeat

**Critical Details**:
- Exactly one sync per frame via `s_SyncedThisFrame` flag
- Only syncs on JCMD_CONTROLLER_READ (0x01), not JCMD_STATUS (0x00)
- Frame delay override: User can set 0-9 frames (0 = server decides)
- Connection persistence: Keeps alive after emulation ends for quick restarts
- Thread safety: Kaillera callbacks run on Kaillera thread, marshaled via Qt signals

#### 4. Threading Model

- **Main Thread**: Qt event loop, UI interactions
- **EmulationThread**: mupen64plus-core execution
- **SDLThread** (RMG-Input): SDL event polling for input
- **HotkeysThread** (RMG-Input): Hotkey state monitoring
- **Kaillera Thread**: Managed by kailleraclient.dll (Windows only)

Communication via Qt signals/slots for thread safety. mupen64plus callbacks queued and polled by timer in MainWindow.

## Development Workflow

### Directory Structure

```
Source/
├── RMG/              # Main Qt application
├── RMG-Core/         # Core library (shared across plugins)
├── RMG-Audio/        # Audio plugin
├── RMG-Input/        # Input plugin
├── RMG-Input-GCA/    # GameCube adapter plugin
├── 3rdParty/         # External dependencies (git submodules)
├── Installer/        # Windows installer
└── Script/           # Build scripts (Build.sh, BundleDependencies.sh)

Data/                 # Runtime data
├── Cheats/          # Cheat files (.cht)
├── InputProfileDB.json
├── font.ttf
└── kailleraclient.dll  # Windows Kaillera DLL

Bin/Release/          # Build output (portable mode)
```

### Code Patterns

**Always read files before editing**: This codebase uses specific patterns (Qt signals/slots, mupen64plus callbacks, threading). Understand existing code before modifications.

**Plugin Development**: When creating/modifying plugins:
1. Follow mupen64plus plugin API (see `Source/3rdParty/mupen64plus-core/src/api/m64p_plugin.h`)
2. Implement `PluginStartup`, `PluginShutdown`, `PluginGetVersion`
3. For input plugins: `InitiateControllers`, `ControllerCommand`, `ReadController`
4. For audio plugins: `AiDacrateChanged`, `AiLenChanged`, `ProcessAList`
5. Optionally implement `PluginConfig()` for GUI (RMG extension)

**Thread Safety**: When working with emulation callbacks or Kaillera:
- Never call Qt functions directly from mupen64plus callbacks
- Queue messages and process on UI thread via signals/slots
- Use `QMetaObject::invokeMethod()` for cross-thread calls
- Kaillera callbacks execute on Kaillera's thread - marshal via KailleraSessionManager

**Input Handling**: RMG-Input uses per-axis scaling (not circular deadzone):
- Range slider: 0-100% (default 66% matches N-Rage)
- Linear scale: 100% = 127 (protocol maximum)
- Each axis (X, Y) has independent deadzone
- Compatible with USBtoN64v2 adapters and N-Rage plugin

### Important Files

**Emulation Lifecycle**: `Source/RMG-Core/Emulation.cpp`
- Contains core emulation loop control and frame callbacks
- **KailleraPifSyncCallback()**: Critical netplay synchronization (lines ~400-500)
- Handles pause/resume, reset, speed control

**Kaillera Integration**: `Source/RMG-Core/Kaillera.cpp`
- Windows DLL wrapper with C++ API
- Callback bridges and thread marshaling
- Frame delay override logic

**Plugin Management**: `Source/RMG-Core/Plugins.cpp`
- Plugin discovery from filesystem
- Dynamic library loading/unloading
- Configuration handling

**Main Window**: `Source/RMG/UserInterface/MainWindow.cpp`
- UI coordination and event handling
- ROM browser management
- Emulation thread lifecycle

**Settings**: `Source/RMG-Core/Settings.cpp`
- Centralized configuration storage
- Per-ROM overrides
- Plugin settings

### Special Considerations

**Kaillera is Windows-Only**: Netplay code in `Kaillera.cpp` only compiles on Windows. Guarded by `#ifdef _WIN32` and CMake option `NETPLAY`.

**Portable vs System Install**:
- Portable: All files in `Bin/Release/`, plugins in subdirectories
- System: Standard Linux paths (`/usr/lib/RMG`, `/usr/share/RMG`)
- Controlled by `PORTABLE_INSTALL` CMake option

**Assembly Optimization**: mupen64plus-core includes x86/ARM assembly. Disable with `NO_ASM` if targeting unsupported architectures.

**License Considerations**: angrylion-rdp-plus uses non-GPL license. Only build if legally acceptable via `USE_ANGRYLION` option.

## Key Features Unique to RMG-K

1. **Kaillera Netplay**: Windows-only online multiplayer via Kaillera protocol
2. **Frame Delay Override**: User-configurable frame delay (0-9) for ping compensation
3. **Connection Persistence**: Kaillera connection stays alive after game ends
4. **N-Rage Input Compatibility**: Per-axis scaling and deadzone matching N-Rage plugin
5. **USBtoN64v2 Adapter Support**: Compatible with popular USB-to-N64 hardware adapters
6. **VidExt Override**: Custom Qt-based rendering instead of SDL windows
