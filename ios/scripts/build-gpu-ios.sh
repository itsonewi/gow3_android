#!/usr/bin/env bash
# Build the shadPS4 renderer for iOS (Metal through MoltenVK) and combine it
# with its static dependencies for the SpartanPortGPU app target.
#
# This is the iterative part of the port: it prepares SDL3, Zydis, MoltenVK and
# Vulkan-Headers for iOS, builds gpu/ with -DSPARTAN_IOS=ON, and lets
# libtool -static produce out/gpu-ios/libspartanportgpu-all.a, which
# ios/project.yml's SpartanPortGPU target links.
#
# Requirements: macOS with Xcode 16+, cmake, ninja, glslang, Boost headers.
#   brew install cmake ninja glslang boost tsl-robin-map
#
# Usage: bash ios/scripts/build-gpu-ios.sh [--app]
#   --app also generates the Xcode project and builds SpartanPortGPU.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEPS="$ROOT/ios/.build"
PREFIX="$DEPS/ios"
OUT="$ROOT/out/gpu-ios"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
BUILD_APP=0
[[ "${1:-}" == "--app" ]] && BUILD_APP=1

export MACOSX_DEPLOYMENT_TARGET=17.0
export IPHONEOS_DEPLOYMENT_TARGET=17.0

require() {
    command -v "$1" >/dev/null || { echo "missing $1 (see the header of this script)" >&2; exit 1; }
}
require cmake
require ninja
require xcrun
require libtool
[[ "$(uname -s)" == Darwin ]] || { echo "this script builds iOS binaries and needs macOS" >&2; exit 1; }

mkdir -p "$DEPS" "$PREFIX" "$OUT"

# --- FEX (fmt + xxhash for the GPU library) --------------------------------
bash "$ROOT/ios/scripts/fetch-fex.sh"
bash "$ROOT/ios/scripts/build-fex-ios.sh"
FEX_IOS="$ROOT/third_party/FEX/build-ios"

# --- submodules the GPU library compiles -----------------------------------
git -C "$ROOT" submodule update --init --recursive \
    gpu/third_party/imgui gpu/third_party/magic_enum gpu/third_party/miniz gpu/third_party/xbyak

# --- SDL3 ------------------------------------------------------------------
SDL_SRC="$DEPS/SDL"
if [[ ! -d "$SDL_SRC" ]]; then
    git clone --depth 1 --branch release-3.2.x https://github.com/libsdl-org/SDL "$SDL_SRC"
fi
cmake -S "$SDL_SRC" -B "$DEPS/SDL-build" -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_INSTALL=ON
cmake --build "$DEPS/SDL-build" --parallel "$JOBS" --target install

# --- Zydis -----------------------------------------------------------------
ZY_SRC="$DEPS/zydis"
if [[ ! -d "$ZY_SRC" ]]; then
    git clone --depth 1 https://github.com/zyantific/zydis "$ZY_SRC"
fi
cmake -S "$ZY_SRC" -B "$DEPS/zydis-build" -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_EXAMPLES=OFF -DZYDIS_BUILD_DOXYGEN=OFF \
    -DZYDIS_BUILD_MAN=OFF
cmake --build "$DEPS/zydis-build" --parallel "$JOBS" --target install

# --- MoltenVK + Vulkan headers ---------------------------------------------
MOLTEN="$DEPS/moltenvk"
if [[ ! -f "$MOLTEN/lib/libvulkan.a" ]]; then
    mkdir -p "$MOLTEN"
    if [[ ! -f "$DEPS/MoltenVK-ios.tar" ]]; then
        curl -L -o "$DEPS/MoltenVK-ios.tar" \
            https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-ios.tar
    fi
    tar -xf "$DEPS/MoltenVK-ios.tar" -C "$MOLTEN"
    mkdir -p "$MOLTEN/lib"
    ln -sf "$(find "$MOLTEN" -path '*ios-arm64/libMoltenVK.a' -print -quit)" "$MOLTEN/lib/libvulkan.a"
fi
if [[ ! -f "$MOLTEN/include/vulkan/vulkan.h" ]]; then
    VK="$DEPS/Vulkan-Headers"
    [[ -d "$VK" ]] || git clone --depth 1 https://github.com/KhronosGroup/Vulkan-Headers "$VK"
    mkdir -p "$MOLTEN/include"
    cp -R "$VK/include/vulkan" "$MOLTEN/include/"
fi

# VulkanMemoryAllocator is a header the CMake build looks for as vma/vk_mem_alloc.h.
if [[ ! -f "$PREFIX/include/vma/vk_mem_alloc.h" ]]; then
    mkdir -p "$PREFIX/include/vma"
    curl -L -o "$PREFIX/include/vma/vk_mem_alloc.h" \
        https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/master/include/vk_mem_alloc.h
fi

# --- GPU library -----------------------------------------------------------
export VULKAN_SDK="$MOLTEN"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export CMAKE_PREFIX_PATH="$PREFIX;$MOLTEN"

cmake -S "$ROOT/gpu" -B "$OUT" -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DSPARTAN_IOS=ON -DSPARTAN_LTO=OFF -DSPARTAN_PGO=off \
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build "$OUT" --parallel "$JOBS" --target spartanportgpu

GPU_LIB="$(find "$OUT" -maxdepth 2 -name 'libgportgpu.a' -print -quit)"
[[ -f "$GPU_LIB" ]] || { echo "spartanportgpu did not produce libgportgpu.a" >&2; exit 1; }

# --- combine everything into the one archive the Xcode target links ---------
LIBS=(
    "$GPU_LIB"
    "$PREFIX/lib/libSDL3.a"
    "$PREFIX/lib/libZydis.a"
    "$MOLTEN/lib/libvulkan.a"
)
for lib in $(find "$OUT" -name 'libminiz.a' -o -name 'libsirit.a'); do
    LIBS+=("$lib")
done
for lib in "${LIBS[@]}"; do
    [[ -f "$lib" ]] || { echo "missing dependency archive: $lib" >&2; exit 1; }
done
libtool -static -o "$OUT/libspartanportgpu-all.a" "${LIBS[@]}"
echo "Built $OUT/libspartanportgpu-all.a"
echo "Link it with the SpartanPortGPU scheme (ios/project.yml)."

if [[ "$BUILD_APP" == 1 ]]; then
    cd "$ROOT/ios"
    xcodegen generate
    xcodebuild -project SpartanPort.xcodeproj -scheme SpartanPortGPU \
        -configuration Release -sdk iphoneos -destination 'generic/platform=iOS' \
        -derivedDataPath build-gpu CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO \
        CODE_SIGN_IDENTITY="" CODE_SIGN_ENTITLEMENTS="" build
    echo "App: ios/build-gpu/Build/Products/Release-iphoneos/SpartanPortGPU.app"
fi
