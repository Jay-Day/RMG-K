# RMG-K (Rosalie's Mupen GUI - Kaillera+ GekkoNet)

Rosalie's Mupen GUI is a free and open-source mupen64plus front-end written in C++.

This fork is focused around netplay with the Kaillera protocol, supporting both traditional Kaillera servers and direct peer-to-peer connections. Available on Windows and Linux.



<p align="center">
<img width="962" height="860" alt="image" src="https://github.com/user-attachments/assets/adc7a1b3-0c4f-4d5d-9f88-79622e87f6ee" />
</p>

## Netplay

### Peer-to-Peer (P2P) Mode
- **NAT traversal via connect codes**: No port forwarding required
- **Persistent connect codes**: Your code is saved across sessions
- **Frame delay selector**: Choose Auto or 1-9 frames, with ping-range labels showing the recommended range for each setting
- **Live ping display**: See your connection quality in real-time
- **Public game listing**: Optionally list your hosted game on the public waiting-games browser
- **Ready sync**: Both players must ready up before the game starts
<img width="676" height="613" alt="image" src="https://github.com/user-attachments/assets/6cf1cfcb-891f-457e-af8d-e348c99c2ffd" />
<img width="822" height="573" alt="image" src="https://github.com/user-attachments/assets/13915a82-54b5-44ce-bb1a-f0aac5304220" />


### Kaillera Server Mode



- **Server browser** with sortable columns, ping, and region info
- **Favorite servers** with custom server support
- **Right-click tools**: Copy IP, Ping, Traceroute (with live output dialog)
- **Frame delay**: Auto (server-assigned) or manual override (1-9 frames)
- **Connection persistence**: Stay connected between games for quick restarts
- **Beep/flash on join**: Get notified when a player joins your game room
- **Auto host message**: Configurable message sent to players joining your room
<p align="center">
<img width="825" height="593" alt="image" src="https://github.com/user-attachments/assets/d5145a6c-4bc2-4f52-901b-7da7f0f0ae8d" />
<img width="961" height="620" alt="image" src="https://github.com/user-attachments/assets/549ba26f-03d7-4950-a3cb-7a68cc9e6c98" />
<img width="283" height="38" alt="image" src="https://github.com/user-attachments/assets/6616186e-ab5d-4598-8b2c-4916d4a54dc4" />
</p>




### In-Game Chat
- **On-Screen Chat Display**: See what your opponent is saying without alt-tabbing. Customizable or disableable in Settings -> OSD
- **Press Enter to Chat**: Chat while in-game without switching windows. Press ESC to cancel. Rebindable in Hotkeys -> System

### Replays

- Record and replay Kaillera sessions (.krec files)
- Configurable storage cap to auto-disable recording when full
- Playback with pause, resume, and frame-advance controls
- Player names visible in the replay list

<img width="702" height="482" alt="image" src="https://github.com/user-attachments/assets/99a90662-7cae-400f-b5bf-0831750b8375" />


## Input

### Raphnet N64 Adapter Support
- Yes, your N64 controller works :D
<img width="462" height="604" alt="image" src="https://github.com/user-attachments/assets/c8579f1e-3e0e-4f2d-a1cc-a9e6bf2bdcd9" />

### GCC Adapter Support
- OEM Nintendo / Mayflash / Input Integrity Lossless / HHL GC Pocket+ adapters all have been tested and work online

<img width="582" height="754" alt="image" src="https://github.com/user-attachments/assets/54ca80cb-10d7-402a-8c0e-15a0378d5a5f" />


### RMG-Input (pronounced Nrage)
RMG-Input uses independent per-axis scaling similar to the Ownasaurus [USBtoN64v2](https://github.com/Ownasaurus/USBtoN64v2) adapter and N-Rage input plugin:
- Should support most xinput devices
- Configurable range slider (0-100%) with default 66% to match N-Rage
- Linear scale: 100% = 127 (protocol max)
- Per-axis deadzone handling instead of circular deadzone

<img width="919" height="793" alt="image" src="https://github.com/user-attachments/assets/6192636d-4bcb-4104-b08a-696dcb416e7d" />


## Building

#### Linux
* Portable Debian/Ubuntu

  ```bash
  sudo apt-get -y install cmake libusb-1.0-0-dev libhidapi-dev libsamplerate0-dev libspeex-dev libminizip-dev libsdl3-dev libfreetype6-dev libgl1-mesa-dev libglu1-mesa-dev pkg-config zlib1g-dev binutils-dev libspeexdsp-dev qt6-base-dev qt6-websockets-dev libqt6svg6-dev libvulkan-dev build-essential nasm git zip ninja-build
  ./Source/Script/Build.sh Release
  ```
  
* Portable Fedora
  ```bash
  sudo dnf install libusb1-devel hidapi-devel libsamplerate-devel minizip-compat-devel SDL3-devel freetype-devel mesa-libGL-devel mesa-libGLU-devel pkgconfig zlib-ng-devel binutils-devel speexdsp-devel qt6-qtbase-devel qt6-qtsvg-devel qt6-qtwebsockets-devel vulkan-devel gcc-c++ nasm git ninja-build
  ./Source/Script/Build.sh Release
  ```

* Portable Arch Linux
  ```bash
  sudo pacman -S --needed make cmake gcc libusb hidapi freetype2 libpng qt6 sdl3 libsamplerate nasm minizip pkgconf vulkan-headers git
  ./Source/Script/Build.sh Release
  ```

* Portable OpenSUSE Tumbleweed
  ```bash
  sudo zypper install SDL3-devel cmake freetype2-devel gcc gcc-c++ libusb-1_0-devel libhidapi-devel libhidapi-hidraw0 libpng16-devel libsamplerate-devel make nasm ninja pkgconf-pkg-config speex-devel vulkan-devel zlib-devel qt6-tools-devel qt6-opengl-devel qt6-widgets-devel qt6-svg-devel minizip-devel git
  ./Source/Script/Build.sh Release
  ```

When it's done building, executables can be found in `Bin/Release`

* Installation/Packaging
```bash
export src_dir="$(pwd)"
export build_dir="$(pwd)/build"
mkdir -p "$build_dir"
cmake -S "$src_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE="Release" -DPORTABLE_INSTALL="OFF" -DCMAKE_INSTALL_PREFIX="/usr" -G "Ninja"
cmake --build "$build_dir"
cmake --install "$build_dir" --prefix="/usr"
```

#### Windows
* Download & Install [MSYS2](https://www.msys2.org/) (UCRT64)
```bash
pacman -S --needed make mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-libusb mingw-w64-ucrt-x86_64-hidapi mingw-w64-ucrt-x86_64-freetype mingw-w64-ucrt-x86_64-libpng mingw-w64-ucrt-x86_64-qt6 mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-speexdsp mingw-w64-ucrt-x86_64-libsamplerate mingw-w64-ucrt-x86_64-libusb mingw-w64-ucrt-x86_64-nasm mingw-w64-ucrt-x86_64-minizip mingw-w64-ucrt-x86_64-vulkan-headers git
./Source/Script/Build.sh Release
```

When it's done building, executables can be found in `Bin/Release`


## License

Rosalie's Mupen GUI is licensed under the [GNU General Public License v3.0](https://www.gnu.org/licenses/gpl-3.0.en.html).
