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
| JIT: StikDebug, BRK pool, RW alias, no-footprint | `SpartanPort/Native/JITAllocator.c`, `Core/JITPool.swift` | implemented |
| Memory+: entitlements, 512 GB map, donation extension | `SpartanPort/Resources/`, `SpartanPortMemoryHost/` | implemented |
| PS4 memory, loader, kernel HLE | stock `src/runtime_*.c` + `src/probe.c`, built by the app | compiled in |
| CPU: FEXCore on the JIT pool | `Runtime/cpu/` | compiled in |
| GPU: shadPS4 → Vulkan → MoltenVK | not linked yet | **stub** |
| Audio/video/pad: SDL3/FFmpeg | not linked yet | **stub** |

A run therefore goes: JIT pool → guest memory → prepared `boot.bin` → imports
resolved from the HLE → FEXCore starts → the game's entry point runs → it stops
at the first graphics call, which the GPU stub logs. That is the honest state of
this tree; the Play card's CPU-only switch is what starts it.

## The JIT layer

iOS gives an app executable memory only while a debugger is attached. The layer
is Madeira's, not an approximation:

1. **Enable JIT.** `RuntimeController` opens StikDebug's `stikdebug://enable-jit`
   URL with this process's bundle id, pid and `spartanport-jit.js`. StikDebug
   attaches, runs the script and sets `CS_DEBUGGED`.
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

1. Enable JIT (StikDebug + LocalDevVPN + pairing file).
2. Allocate the pool (128–896 MB; 512 MB is a good start).
3. Import the prepared folder.
4. Play. The build is CPU-only: the Play card says so, and the log shows the
   runtime's own lines prefixed `[runtime]` — the loader's `Mapped N bytes…`,
   FEXCore startup, then the first missing HLE import (normally the first
   graphics call). The stock loader exits at that point; there is no GPU to
   present a frame yet.

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
* The first run needs StikDebug, LocalDevVPN and a pairing file. The built-in
  StikJIT helper is not bundled, so the StikDebug route is the only one.

## Next

* **GPU**: build the shadPS4 video core and MoltenVK for iOS and replace
  `Runtime/gpu/sp_gpu_stub.c`; the stock `SpartanGpuConfig`/`spartanportgpu_init`
  interface is already what the loader calls. SDL3 has an iOS backend for the
  window/surface; MoltenVK provides Vulkan 1.3.
* **Audio/video/pad**: SDL3 audio and gamepad backends, FFmpeg/VideoToolbox for
  `runtime_videodec.c`, and the touch overlay.
* **Launcher parity**: the Python launcher's settings, patches and save import
  would move into the SwiftUI cards.

## Licensing

The BRK protocol and the no-footprint ladder are Madeira's
(GPL-3.0-or-later); the implementation in this tree is a rewrite of the same
documented protocol, not a copy of Madeira's file. SpartanPort is GPL-2.0.
