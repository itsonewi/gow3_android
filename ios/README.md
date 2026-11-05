# SpartanPort iOS

The iOS host for the SpartanPort runtime: God of War III Remastered's x86-64
guest on **FEXCore**, with the JIT and memory layer that
[Madeira](https://github.com/willfaust/Madeira) uses to run emulators on jailed
iOS.

This is not a wrapper around the Android build. iOS cannot execute the branch's
Linux/glibc ELF, so the runtime is rebuilt as Mach-O inside the app:

| Layer | Where | State |
|---|---|---|
| SwiftUI launcher, game import, diagnostics | `SpartanPort/` | implemented |
| JIT: StikDebug or built-in StikJIT, BRK pool | `SpartanPort/Native/JITAllocator.c`, `Core/JITPool.swift`, `SpartanPortJITHelper/` | implemented |
| Memory+: entitlements, 512 GB map, donation extension | `SpartanPort/Resources/`, `SpartanPortMemoryHost/` | implemented |
| PS4 memory, loader, kernel HLE | stock `src/runtime_*.c` + `src/probe.c`, built by the app | compiled in |
| CPU: FEXCore on the JIT pool | `Runtime/cpu/` | compiled in |
| GPU: shadPS4 → Vulkan → MoltenVK | `gpu/`, `out/gpu-ios/libspartanportgpu-all.a` | **linked** (device run pending) |
| Audio/video/pad: SDL3/LibAtrac9/FFmpeg | SDL3 pad+audio and ATRAC9 compiled in; MP3/movies need FFmpeg | **partial** |

A run therefore goes: JIT pool → guest memory → prepared `boot.bin` → imports
resolved from the HLE → FEXCore starts → the game's entry point runs → the GPU
app creates its Metal surface and Vulkan device through MoltenVK. The CPU-only
app still stops at the first graphics call, which the stub logs; the Play card's
CPU-only switch decides which one starts.

## The JIT layer

iOS gives an app executable memory only while a debugger is attached. The layer
is Madeira's, not an approximation:

1. **Enable JIT.** There are two routes, chosen in the JIT card or
   automatically:
   * **StikDebug**, if its `stikdebug://` scheme is installed:
     `RuntimeController` opens `stikdebug://enable-jit` with this process's
     bundle id, pid and `spartanport-jit.js`.
   * **Built-in StikJIT** (no second app): `SpartanPortJITHelper.appex` is an
     `com.apple.ar.viewer` app extension that runs the bundled
     `StikJIT.framework`, attaches to this app, prepares the Developer Disk
     Image and runs the same script. It needs iOS 26+, LocalDevVPN and this
     iPhone's pairing file (import it with **Pairing file**). The framework is
     in `ios/Frameworks` under MPL-2.0 (`ios/legal`).
   Either way the debugger sets `CS_DEBUGGED` and serves the script.
2. **Allocate the pool.** `sp_pool_prepare()` executes `BRK #0xf00d` with
   `x16 = 1`, `x0 = 0`, `x1 = size`. The script answers with StikDebug's `_M`
   allocation and `prepare_memory_region()`, which authorizes the RX run and
   returns it in `x0`. Pages are 16 KB.
3. **Map the writable alias.** `vm_remap` creates a second view of the same VM
   object, `vm_protect` makes it RW, and `jit_make_region_no_footprint()` walks
   the same `mach_memory_entry_ownership` ladder as Madeira
   (`plain/TASK_NULL`, `plain/self`, `share/self`, `share/TASK_NULL`) so the
   code cache does not count against jetsam. Every step is logged with
   `phys_footprint` either side.
4. **Detach.** `x16 = 0` asks the debugger to leave. The pool stays mapped.
5. **FEX uses it.** `sp_jit_mmap.c` interposes the app's `mmap`/`munmap`:
   executable anonymous mappings are carved from the pool, everything else goes
   to libSystem. FEXCore's code buffers therefore land in the authorized pages
   instead of failing at `mmap(PROT_EXEC)`.

The SIGTRAP fallback only skips this protocol's `BRK #0xf00d` and returns 0, so
a pool request without a debugger fails instead of crashing the app.

### Built-in StikJIT

`SpartanPortJITHelper` follows Madeira's helper design:

* a classic app extension on `com.apple.ar.viewer`, started through the private
  `NSExtension` API by the helper's own bundle id, in a separate process (a
  process cannot synchronously debug itself);
* the JSON request carries the target pid, the pairing file and the base64 of
  `spartanport-jit.js`; the helper writes the pairing file to a temporary
  `Library/StikJIT` path and deletes it after the request;
* `StikJIT.enableJIT(... script: .customBase64(...), forceScript: true)` attaches
  and keeps the script running until the app prepares its pool and sends the
  detach BRK; the request then completes;
* both apps embed their own helper because iOS requires an extension id to be
  prefixed by the host app's id (`com.onewi.spartanport.jithelper` and
  `com.onewi.spartanport.gpu.jithelper`), and both embed the same framework in
  `Frameworks/`.

Built-in JIT requires iOS 26 or later (as Madeira's does) and a normal
sideloaded install; the app, helper and framework must be signed together and
the sideloader must keep app extensions. The pairing file can be imported with
the **Pairing file** button (or dropped into `Documents/StikJIT/pairingFile.plist`
through Finder file sharing); on-device pairing generation is not implemented,
so use a computer-made pairing file.

The bundled framework's Swift interface was emitted by Swift 6.4. Its module
selectors (`Module::Type`) are stripped, and the interface compiler version is
marked 6.3, so Xcode 26 can import it; regenerate the interface if the framework
is updated.

## The memory layer

* **Memory+** is `com.apple.developer.kernel.increased-memory-limit`; **extended
  virtual addressing** is `com.apple.developer.kernel.extended-virtual-addressing`
  (512 GB instead of 63 GB). Neither can be turned on in-app: the signing profile
  must carry them. The Memory card reads the real signature and the real
  `TASK_VM_INFO`, so a sideloader that drops them is visible immediately.
* **The guest maps everything above 64 GiB.** `src/runtime_memory.c` places the
  guest image, direct-memory views and handles in `[0x1000000000, 0xfc00000000)`,
  which is above the 63 GiB map iOS gives an app by default; every guest mapping
  therefore depends on extended virtual addressing. `sp_check_guest_window()`
  reads the real `TASK_VM_INFO` and reports a missing entitlement instead of
  failing later inside the loader. The app's own image stays at the normal
  `0x100000000`; the guest range does not overlap it.
* **`SpartanPortMemoryHost`** is the same experiment as Madeira's
  `MadeiraMemoryHost`: an `com.apple.ar.viewer` extension, an extension point
  iOS 27's jetsam table leaves without a memory limit, that creates memory
  objects for the app and reports both processes' footprints. It is bundled and
  started with `SPARTAN_MEMORY_HOST=1`; the log then shows the extension's
  budget. Backing the direct-memory pool with its memory entries (instead of the
  sparse file) is the next step of that experiment.

## Building

The IPA is built by `.github/workflows/ios.yml` on a macOS runner:

```sh
bash ios/scripts/fetch-fex.sh        # Madeira's FEX fork (iOS support)
bash ios/scripts/build-fex-ios.sh    # FEXCore + dependencies as .a for iOS
cd ios && xcodegen generate
xcodebuild -project SpartanPort.xcodeproj -scheme SpartanPort \
           -sdk iphoneos -configuration Release CODE_SIGNING_ALLOWED=NO build
```

The app target links `SpartanRuntime` (the stock loader and HLE plus the iOS
shims) and the FEXCore archives from `third_party/FEX/build-ios`.

### The GPU app

`ios/scripts/build-gpu-ios.sh` builds the shadPS4 renderer for iOS (Metal through
MoltenVK), combines it with SDL3/Zydis/fmt/xxhash/MoltenVK into
`out/gpu-ios/libspartanportgpu-all.a`, and `--app` builds the `SpartanPortGPU`
scheme. That target defines `SPARTANPORT_GPU`, links the archive, and starts the
loader without `--cpu-only`, so the real `spartanportgpu_*` symbols (which
override the weak stubs) create the CAMetalLayer and the Vulkan device.

`.github/workflows/ios-gpu.yml` runs that script on `macos-15` and publishes the
`spartanport-ios-gpu` artifact (`SpartanPortGPU-unsigned.ipa` plus the static
archive). The build is green; running it on a device is the next step. See
[`gpu/README-ios.md`](../gpu/README-ios.md) for the dependency recipe, the iOS
shims and what still has to be validated.

Two pieces are deliberately outside the current GPU build:

* **AvPlayer** (`gpu/shim/avplayer_stub.cpp`): its source and file streamer are
  libav clients, and there is no iOS FFmpeg yet. The HLE entry points exist, but
  movie playback returns "no source". The scene renderer does not depend on it.
* **FSR 4 / frame generation** (`gpu/shim/fsr_stub.cpp`,
  `frame_generation_stub.cpp`): the FFX provider is a desktop driver component;
  the menu reports them unavailable and the game's own TAA still works.

## Installing and preparing the game

The artifact is unsigned. Install it with SideStore, AltStore, Sideloadly,
ESign or Xcode, and make sure the profile grants:

| Entitlement | Why |
|---|---|
| `get-task-allow` | lets StikDebug attach; without it JIT cannot be enabled |
| `com.apple.developer.kernel.increased-memory-limit` | Memory+ (jetsam budget) |
| `com.apple.developer.kernel.extended-virtual-addressing` | 512 GB map for the guest's flexible memory |

The game folder has to be **prepared** first. `scripts/prepare.py` builds
`boot.bin` from the dump and applies the port's patches; it is Python, and this
app cannot run Python (the Android package runs it inside nixport's rootfs).
Prepare on a computer:

```sh
python3 scripts/prepare.py <dump> --out <prepared>   # see scripts/prepare.py --help
```

then import `<prepared>` (the folder containing `boot.bin`) with **Import
folder**. The game dump itself (`eboot.bin`, `sce_sys`, `sce_module`) is only
needed for `param.sfo` and the pause-menu WAD; importing the parent folder that
holds both is fine.

## Running

1. Enable JIT: StikDebug, or the built-in helper with LocalDevVPN + a pairing file.
2. Allocate the pool (128–896 MB; 512 MB is a good start).
3. Import the prepared folder.
4. Play. The CPU app is CPU-only: the Play card says so, and the log shows the
   runtime's own lines prefixed `[runtime]` — the loader's `Mapped N bytes…`,
   FEXCore startup, then the first missing HLE import (normally the first
   graphics call). The stock loader exits at that point.
   The GPU app (`SpartanPortGPU.ipa`) links the shadPS4/MoltenVK renderer and
   starts the loader without `--cpu-only`, so it proceeds to the Metal surface
   and Vulkan device; that path still needs device validation. Movies are the
   known gap (AvPlayer is stubbed until an iOS FFmpeg build exists).

A fatal runtime error still calls `exit()` as it does on every platform; iOS
then ends the app. There is no safe unwind point for FEX and guest threads, so
the Stop button only reports the request.

## Limits to expect

* The PS4 reports 5056 MiB of direct memory. An 8 GB iPhone's per-app jetsam
  limit is smaller, so a session that touches the whole pool will be killed;
  `SPARTAN_DMEM_MB` (for example 2048) is the runtime's own knob, and the direct
  backing is sparse, so only pages the game actually maps are charged.
* The prepared folder is made on a computer. `prepare.py` also writes the patch
  set and the ISA rewrite file; this build runs the boot image without them.
  Settings, mods and save import from the Android launcher are not ported yet.
* The first run needs LocalDevVPN and a pairing file. Use StikDebug if it is
  installed, or the bundled built-in StikJIT helper (iOS 26+) with no second app.

## Next

* **Device validation of the GPU app**: MoltenVK device/features, Metal surface,
  the GCN shader path, then presents and frame pacing. The GPU app now enables
  MoltenVK's portability instance/device extensions and takes
  `vkGetInstanceProcAddr` from the statically linked library.
* **Remaining media/touch work**: an iOS FFmpeg build (H.264/AAC/MP3) to replace
  `shim/avplayer_stub.cpp` and the MP3 half of `runtime_ajm.c`; a touch overlay
  for iPhone. SDL3 pad and audio, and the ATRAC9 decoder, are compiled into the
  GPU app.
* **Launcher parity**: the Python launcher's settings, patches and save import
  would move into the SwiftUI cards.

## Licensing

The BRK protocol and the no-footprint ladder are Madeira's
(GPL-3.0-or-later); the implementation in this tree is a rewrite of the same
documented protocol, not a copy of Madeira's file. SpartanPort is GPL-2.0.
