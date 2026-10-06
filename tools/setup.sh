#!/usr/bin/env bash
# Fetch everything the build needs. Idempotent: what is already there is kept.
#   - git submodules in lib/ (pico-sdk + its tinyusb and mbedtls, ioLibrary, picotool)
#   - Arm GNU toolchain 14.3 into .deps/arm-none-eabi (checksum-verified)
#     (skipped when PICO_TOOLCHAIN_PATH is set: use your own toolchain)
#   - picotool built from lib/picotool into .deps/picotool (the SDK's prebuilt
#     one lacks `seal`, used for image hashing; built without USB support)
# Needs: git, cmake, make, a host C/C++ compiler, python3, curl, tar, xz.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
JOBS=${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)}
ARM_VER=14.3.rel1

for tool in git cmake make python3 curl tar; do
    command -v "$tool" >/dev/null || { echo "Missing required tool: $tool" >&2; exit 1; }
done

echo "== Submodules"
git submodule update --init lib/pico-sdk lib/ioLibrary_Driver lib/picotool
# Only the pico-sdk submodules this firmware uses (the others are large).
git -C lib/pico-sdk submodule update --init --depth 1 lib/tinyusb lib/mbedtls

if [ -n "${PICO_TOOLCHAIN_PATH:-}" ]; then
    echo "== Toolchain: using PICO_TOOLCHAIN_PATH=$PICO_TOOLCHAIN_PATH"
elif [ ! -x .deps/arm-none-eabi/bin/arm-none-eabi-gcc ]; then
    case "$(uname -s)-$(uname -m)" in
        Linux-x86_64) host=x86_64 ;;
        Linux-aarch64 | Linux-arm64) host=aarch64 ;;
        Darwin-arm64) host=darwin-arm64 ;;
        Darwin-x86_64) host=darwin-x86_64 ;;
        *) echo "No Arm toolchain download for $(uname -s)-$(uname -m): install arm-none-eabi-gcc 14 and set PICO_TOOLCHAIN_PATH" >&2; exit 1 ;;
    esac
    name=arm-gnu-toolchain-${ARM_VER}-${host}-arm-none-eabi
    url=https://developer.arm.com/-/media/Files/downloads/gnu/${ARM_VER}/binrel/${name}.tar.xz
    echo "== Toolchain: $name"
    mkdir -p .deps
    curl -fL --retry 3 -o ".deps/${name}.tar.xz" "$url"
    curl -fsSL --retry 3 -o ".deps/${name}.tar.xz.sha256asc" "${url}.sha256asc"
    want=$(awk '{print $1}' ".deps/${name}.tar.xz.sha256asc")
    have=$( (sha256sum ".deps/${name}.tar.xz" 2>/dev/null || shasum -a 256 ".deps/${name}.tar.xz") | awk '{print $1}')
    if [ "$want" != "$have" ]; then
        echo "Toolchain checksum mismatch ($have, expected $want)" >&2
        exit 1
    fi
    rm -rf .deps/arm-none-eabi ".deps/${name}"
    tar -xf ".deps/${name}.tar.xz" -C .deps
    mv ".deps/${name}" .deps/arm-none-eabi
    rm ".deps/${name}.tar.xz" ".deps/${name}.tar.xz.sha256asc"
fi

if [ ! -x .deps/picotool/picotool/picotool ]; then
    echo "== picotool (from lib/picotool)"
    cmake -S lib/picotool -B .deps/picotool-build -DPICO_SDK_PATH="$ROOT/lib/pico-sdk" \
        -DPICOTOOL_NO_LIBUSB=1 -DPICOTOOL_FLAT_INSTALL=1 -DCMAKE_INSTALL_PREFIX="$ROOT/.deps/picotool" \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build .deps/picotool-build -j"$JOBS"
    cmake --install .deps/picotool-build
fi

echo "== Dependencies ready"
