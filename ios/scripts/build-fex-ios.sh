#!/usr/bin/env bash
# Build the FEXCore static libraries the app links, for iOS arm64.
#
# This is Madeira's recipe (build/fex-ios/build.sh and tools/build/build-fex-ios.sh)
# with the same options: no Source/Common (the app provides its own host-feature
# detection and config), no FEX allocator, no tests/thunks, iOS 17 deployment.
# Output: third_party/FEX/build-ios/FEXCore/Source/*.a and External/*.a.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/third_party/FEX"
BUILD="$SRC/build-ios"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 8)}"

if [[ ! -d "$SRC/FEXCore" ]]; then
    echo "third_party/FEX is missing; run ios/scripts/fetch-fex.sh first" >&2
    exit 1
fi

required=(
    "$BUILD/FEXCore/Source/libFEXCore.a"
    "$BUILD/FEXCore/Source/libFEXCore_Base.a"
    "$BUILD/FEXCore/Source/libJemallocLibs.a"
    "$BUILD/External/fmt/libfmt.a"
    "$BUILD/External/cephes/libcephes_128bit.a"
    "$BUILD/External/SoftFloat-3e/libsoftfloat_3e.a"
    "$BUILD/External/xxhash/cmake_unofficial/libxxhash.a"
)

if [[ "${FEX_IOS_REUSE:-1}" == 1 ]]; then
    complete=1
    for lib in "${required[@]}"; do [[ -s "$lib" ]] || complete=0; done
    if [[ $complete == 1 ]]; then
        echo "FEX iOS libraries already built in $BUILD"
        exit 0
    fi
fi

cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_SYSTEM_PROCESSOR=arm64 \
    -DCMAKE_OSX_SYSROOT=iphoneos \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_DISABLE_FIND_PACKAGE_fmt=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_range-v3=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_unordered_dense=ON \
    -DBUILD_TESTING=OFF \
    -DBUILD_FEX_LINUX_TESTS=OFF \
    -DBUILD_THUNKS=OFF \
    -DBUILD_FEXCONFIG=OFF \
    -DBUILD_STEAM_SUPPORT=OFF \
    -DENABLE_FEX_ALLOCATOR=OFF \
    -DENABLE_ASSERTIONS=OFF \
    -DENABLE_LTO=OFF \
    -DENABLE_CCACHE=OFF \
    -DTUNE_CPU=generic \
    -DTUNE_ARCH=generic

cmake --build "$BUILD" --parallel "$JOBS" --target \
    FEXCore FEXCore_Base JemallocLibs fmt cephes_128bit softfloat_3e xxhash

for lib in "${required[@]}"; do
    [[ -f "$lib" ]] || { echo "ERROR: FEX build did not produce $lib" >&2; exit 1; }
done
echo "FEX iOS libraries are in $BUILD"
