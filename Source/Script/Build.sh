#!/usr/bin/env bash
set -ex
script_dir="$(dirname "$0")"
toplvl_dir="$(realpath "$script_dir/../../")"
build_config="Debug"
threads="$(nproc)"
kaillera_app_version_override=""
generator="Unix Makefiles"
install_cheats="ON"
bundle_dependencies="ON"
use_angrylion="ON"

if [[ "$1" = "--help" ]] || [[ "$1" = "-h" ]]; then
    echo "$0 [Build Config] [Thread Count] [--kaillera-app-version <version>] [--no-cheats] [--no-angrylion] [--no-bundle-dependencies] [--fast-dev]"
    echo ""
    echo "Options:"
    echo "  --no-cheats              Skip installing bundled cheat files"
    echo "  --no-angrylion           Skip building/installing the angrylion video plugin"
    echo "  --no-bundle-dependencies Skip Windows dependency bundling"
    echo "  --fast-dev               Equivalent to --no-cheats --no-angrylion --no-bundle-dependencies"
    echo ""
    echo "Examples:"
    echo "  $0 Release"
    echo "  $0 Release 12"
    echo "  $0 Release --kaillera-app-version v0.8.21"
    echo "  $0 Release --fast-dev"
    exit
fi

positionals=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-cheats)
            install_cheats="OFF"
            shift
            ;;
        --no-angrylion)
            use_angrylion="OFF"
            shift
            ;;
        --no-bundle-dependencies)
            bundle_dependencies="OFF"
            shift
            ;;
        --fast-dev)
            install_cheats="OFF"
            bundle_dependencies="OFF"
            use_angrylion="OFF"
            shift
            ;;
        --kaillera-app-version)
            kaillera_app_version_override="${2:-}"
            if [[ -z "$kaillera_app_version_override" ]]; then
                echo "Missing value for --kaillera-app-version"
                exit 1
            fi
            shift 2
            ;;
        *)
            positionals+=("$1")
            shift
            ;;
    esac
done

if [[ ${#positionals[@]} -ge 1 ]]; then
    build_config="${positionals[0]}"
fi
if [[ ${#positionals[@]} -ge 2 ]]; then
    threads="${positionals[1]}"
fi

build_dir="$toplvl_dir/Build/$build_config"

if [[ $(uname -s) = *MINGW64* ]]
then
    generator="MSYS Makefiles"
fi

mkdir -p "$build_dir"

cmake_args=(
    -DCMAKE_BUILD_TYPE="$build_config"
    -DPORTABLE_INSTALL=ON
    -DUSE_ANGRYLION="$use_angrylion"
    -DINSTALL_CHEATS="$install_cheats"
)
if [[ -n "$kaillera_app_version_override" ]]; then
    cmake_args+=(-DKAILLERA_APP_VERSION_OVERRIDE="$kaillera_app_version_override")
fi

cmake -S "$toplvl_dir" -B "$build_dir" "${cmake_args[@]}" -G "$generator"

cmake --build "$build_dir" --parallel "$threads"

if [[ "$build_config" = "Debug" ]] ||
    [[ "$build_config" = "RelWithDebInfo" ]]
then
    cmake --install "$build_dir" --prefix="$toplvl_dir"
else
    cmake --install "$build_dir" --strip --prefix="$toplvl_dir"
fi

if [[ $(uname -s) = *MINGW64* ]] && [[ "$bundle_dependencies" = "ON" ]]
then
    cmake --build "$build_dir" --target=bundle_dependencies
fi
