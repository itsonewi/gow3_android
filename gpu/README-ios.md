# Building the GPU library for iOS

This is the remaining step between the CPU-only bring-up (which boots the guest
until its first graphics import) and a game that presents frames.

The good news: **the renderer itself already has the Apple path**. shadPS4's
`video_core/renderer_vulkan/vk_platform.cpp` compiles
`VK_USE_PLATFORM_METAL_EXT` on `__APPLE__` and creates the `VkSurfaceKHR` from a
`CAMetalLayer` with `vkCreateMetalSurfaceEXT`. `gpu/shim/window_ios.mm`
implements the `WindowSDL` interface on UIKit and reports
`WindowSystemInfo::type = Metal`, so the core gets a real Metal surface without
touching the Vulkan code.

The bad news: this library was written for Windows/Linux, and the iOS build has
to supply the pieces those systems get from packages (MoltenVK, SDL3, Zydis,
fmt, xxhash, VMA, ...). The CMake branch exists (`-DSPARTAN_IOS=ON`) but has not
been run on a Mac yet. Treat the commands below as the build recipe to iterate
on, not as a verified green build.

## What the iOS branch changes

- `SPARTAN_IOS=ON` selects the Metal surface, links MoltenVK and the Apple
  frameworks, and makes `spartanportgpu` a static library.
- `shim/window.cpp` (SDL window) is replaced by `shim/window_ios.mm`.
- X11/Wayland are skipped.
- AMD FidelityFX is skipped: `fsr_driver.cpp` is excluded and
  `shim/fsr_stub.cpp` reports FSR unavailable. The in-game menu disables the FSR
  entries (TAA and the port's other paths still work).
- FFmpeg is skipped: `spartanport_png.cpp` and `spartanport_trophy_media.cpp`
  are replaced by `shim/ios_stub.cpp`. Trophy icons and save-list pictures are
  blank; the game's VideoOut path does not use FFmpeg.
- Everything from `renderer_vulkan` onwards (shader recompiler, Gnm driver,
  VideoOut, presenter, ImGui overlay) is the same code the Android build runs.

## Dependencies

| Dependency | Why | iOS build |
|---|---|---|
| MoltenVK 1.4.x | Vulkan on Metal | prebuilt `MoltenVK-ios.tar` |
| Vulkan-Headers | `vulkan.h`/`vulkan.hpp` | MoltenVK tar or Khronos |
| SDL3 3.2.x | audio, gamepad, events, overlay text | build static for iOS |
| Zydis | guest instruction decode | build static for iOS |
| fmt, xxhash | logging/hashing | reuse the FEX iOS build |
| Boost headers | asio/containers | Homebrew headers (portable) |
| tsl-robin-map, VMA, magic_enum, miniz, sirit, imgui | header/vendored | submodules / Homebrew |
| glslang | compiles the host shaders at build time | Homebrew (host tool) |

## Recipe (macOS)

```sh
# Host tools
brew install cmake ninja glslang boost tsl-robin-map

# FEX iOS build: supplies libfmt.a and libxxhash.a (same versions FEXCore uses)
bash ios/scripts/fetch-fex.sh
bash ios/scripts/build-fex-ios.sh

# Submodules the GPU library compiles
git submodule update --init gpu/third_party/imgui gpu/third_party/magic_enum \
    gpu/third_party/miniz gpu/third_party/x11 2>/dev/null || true

# SDL3 for iOS
git clone --depth 1 --branch release-3.2.x https://github.com/libsdl-org/SDL deps/SDL
cmake -S deps/SDL -B deps/SDL-build -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
  -DCMAKE_INSTALL_PREFIX="$PWD/ios/.build/ios" \
  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
cmake --build deps/SDL-build --target install

# Zydis for iOS
git clone --depth 1 https://github.com/zyantific/zydis deps/zydis
cmake -S deps/zydis -B deps/zydis-build -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
  -DCMAKE_INSTALL_PREFIX="$PWD/ios/.build/ios" \
  -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_EXAMPLES=OFF
cmake --build deps/zydis-build --target install

# MoltenVK (prebuilt)
curl -L -o deps/MoltenVK-ios.tar \
  https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-ios.tar
mkdir -p deps/moltenvk && tar -xf deps/MoltenVK-ios.tar -C deps/moltenvk
mkdir -p deps/moltenvk/lib
ln -sf "$PWD/deps/moltenvk/MoltenVK.xcframework/ios-arm64/libMoltenVK.a" \
       deps/moltenvk/lib/libvulkan.a

# GPU library
export VULKAN_SDK="$PWD/deps/moltenvk"
export PKG_CONFIG_PATH="$PWD/ios/.build/ios/lib/pkgconfig"
export CMAKE_PREFIX_PATH="$PWD/ios/.build/ios;$PWD/deps/moltenvk"
cmake -S gpu -B out/gpu-ios -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSPARTAN_IOS=ON -DSPARTAN_LTO=OFF \
  -DSPARTAN_PGO=off
ninja -C out/gpu-ios spartanportgpu
```

The MoltenVK tar layout differs between releases; the only requirement is
`$VULKAN_SDK/include/vulkan/vulkan.h` and `$VULKAN_SDK/lib/libvulkan.a`
(FindVulkan's non-Windows layout).

## Wiring the app

The `SpartanPortGPU` target in `ios/project.yml` is already wired:

1. `SPARTAN_IOS` CMake builds the renderer; `build-gpu-ios.sh` combines it and
   every static dependency into `out/gpu-ios/libspartanportgpu-all.a`.
2. `SpartanPortGPU` links that archive, defines `SPARTANPORT_GPU`, and links the
   Apple frameworks MoltenVK/SDL3 need.
3. The GPU stub in `Runtime/gpu/sp_gpu_stub.c` is `__attribute__((weak))`, so
   the real `spartanportgpu_*` symbols override it without a second runtime
   target.
4. `RuntimeController.play` compiles `cpu_only = false` for the GPU target, so
   the loader calls `spartanportgpu_init` instead of skipping it.
5. `window_ios.mm` creates the CAMetalLayer; the renderer presents through the
   normal VideoOut swapchain.

## What still has to be validated on a device

- MoltenVK's Vulkan 1.3 feature set against shadPS4's requirements (descriptor
  indexing, buffer device address, transform feedback, conditional rendering,
  depth clip, stencil export, 16-bit storage). The renderer requests what it
  needs in `vk_platform.cpp`/`vk_instance.cpp`; whichever extension is missing
  is reported there.
- The GCN shader recompiler on ARM (it emits SPIR-V, so this is mostly
  correctness, not codegen).
- SDL3 audio/gamepad on iOS; touch controls are a separate overlay job.
- FFmpeg stubs: trophy/save pictures and the in-game movie picture are blank
  until a VideoToolbox-backed decoder replaces them.
- Performance and the 5 GiB guest pool against the device's jetsam limit.

## Status

| Piece | State |
|---|---|
| Metal surface / iOS window | written (`window_ios.mm`) |
| SDL text/pixel-density and refresh-rate guards | written |
| FSR stub | written |
| FFmpeg PNG/trophy stubs | written |
| `SPARTAN_IOS` CMake branch | written, **not yet built on macOS** |
| `build-gpu-ios.sh` (SDL3, Zydis, MoltenVK, libtool archive) | written, **not yet run** |
| `SpartanPortGPU` app target | written |
| First device run / feature validation | to do |
