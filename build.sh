#!/usr/bin/env bash
# Builds SuperTerminal on macOS and Linux.
#
#   ./build.sh                  build ./build/<os>/st
#   ./build.sh --test           ...then run the routing/safety tests and the offline end-to-end tests
#   ./build.sh --test --live    ...plus real plain-English requests (needs ANTHROPIC_API_KEY)
#   ./build.sh --install        ...then copy st to $PREFIX/bin (default ~/.local/bin)
#
# Needs: a C++20 compiler, CMake 3.20+, and the libcurl development package.
# Extra CMake arguments can go in ST_CMAKE_ARGS, e.g.
#   ST_CMAKE_ARGS="-DCURL_INCLUDE_DIR=/opt/curl/include -DCURL_LIBRARY=/opt/curl/lib/libcurl.so" ./build.sh
set -euo pipefail
cd "$(dirname "$0")"

test=0 live=0 install=0
for a in "$@"; do
    case $a in
        --test) test=1 ;;
        --live) live=1 ;;
        --install) install=1 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "Unknown option: $a" >&2; exit 2 ;;
    esac
done

os=$(uname -s | tr '[:upper:]' '[:lower:]')
cmake=${CMAKE:-cmake}

install_hint() {
    echo >&2
    echo "Install the build tools, then run ./build.sh again:" >&2
    if [[ $os == darwin ]]; then
        echo "  xcode-select --install && brew install cmake        # libcurl ships with macOS" >&2
    elif command -v apt-get >/dev/null; then
        echo "  sudo apt-get install -y build-essential cmake libcurl4-openssl-dev" >&2
    elif command -v dnf >/dev/null; then
        echo "  sudo dnf install -y gcc-c++ cmake libcurl-devel" >&2
    elif command -v pacman >/dev/null; then
        echo "  sudo pacman -S --needed base-devel cmake curl" >&2
    elif command -v zypper >/dev/null; then
        echo "  sudo zypper install -y gcc-c++ cmake libcurl-devel" >&2
    elif command -v apk >/dev/null; then
        echo "  sudo apk add build-base cmake curl-dev" >&2
    else
        echo "  a C++20 compiler, CMake 3.20+ and libcurl (with headers)" >&2
    fi
}

if ! command -v "$cmake" >/dev/null; then
    echo "CMake isn't installed." >&2
    install_hint
    exit 1
fi
if ! command -v c++ >/dev/null && ! command -v g++ >/dev/null && ! command -v clang++ >/dev/null; then
    echo "No C++ compiler found." >&2
    install_hint
    exit 1
fi

build="build/$os"
# shellcheck disable=SC2086
if ! "$cmake" -S . -B "$build" -DCMAKE_BUILD_TYPE=Release ${ST_CMAKE_ARGS:-}; then
    echo "CMake configuration failed (often: libcurl development headers are missing)." >&2
    install_hint
    exit 1
fi
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
"$cmake" --build "$build" -j "$jobs"
echo "Built $PWD/$build/st"

if ((install)); then
    prefix=${PREFIX:-$HOME/.local}
    mkdir -p "$prefix/bin"
    cp "$build/st" "$prefix/bin/st"
    echo "Installed $prefix/bin/st"
    case ":$PATH:" in *":$prefix/bin:"*) ;; *) echo "Add $prefix/bin to your PATH to run 'st' from anywhere." ;; esac
fi

if ((test)); then
    python3 tests/route_tests.py "$build/st"
    if command -v pwsh >/dev/null; then pwsh -NoProfile -File tests/route-tests.ps1 -Exe "$build/st"; fi
    mode=()
    ((live)) || mode=(--offline)
    ran=0
    for sh in bash zsh pwsh; do
        command -v "$sh" >/dev/null || continue
        # macOS ships bash 3.2, which can't host the integration (it needs bash 4+).
        if [[ $sh == bash ]] && (($(bash -c 'echo ${BASH_VERSINFO[0]}') < 4)); then continue; fi
        echo "--- end-to-end: $sh"
        python3 tests/e2e_unix.py "$build/st" --shell "$sh" ${mode[@]+"${mode[@]}"}
        ran=1
    done
    ((ran)) || { echo "No supported shell found for the end-to-end test." >&2; exit 1; }
fi
