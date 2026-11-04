# SpartanPort iOS

The iOS host for the SpartanPort runtime: a SwiftUI app that carries the JIT
and memory layers ported from [Madeira](https://github.com/willfaust/Madeira),
imports the game folder, and will host the ported ARM64 runtime
(FEXCore + MoltenVK).

## What is in this first build

- **JIT layer** – `Native/JITAllocator.c/.h` is Madeira's allocator, trimmed to
  the parts this app uses:
  - dual-mapped RW/RX regions;
  - `VM_LEDGER_FLAG_NO_FOOTPRINT` exemption for the pool;
  - `CS_DEBUGGED` / address-map / available-memory probes;
  - SIGTRAP fallback that skips only `BRK #0xf00d`;
  - the iOS BRK debugger protocol (`x16 = 0` detach, `x16 = 1` prepare).
- **StikDebug flow** – `spartanport-jit.js` is Madeira's debugger script with
  the log prefix changed. `RuntimeController` opens
  `stikdebug://enable-jit` with the script, waits for `CS_DEBUGGED`, asks the
  debugger for an RX pool, maps the RW alias with `vm_remap`, applies the
  no-footprint exemption, then detaches.
- **Memory+** – `SpartanPort.entitlements` carries
  `com.apple.developer.kernel.increased-memory-limit` and
  `com.apple.developer.kernel.extended-virtual-addressing`. The UI reads the
  signed process's real entitlements and the actual TASK_VM_INFO address map,
  so a signature that drops them is visible immediately.
- **Launcher shell** – folder import with a security-scoped bookmark,
  `eboot.bin` check, diagnostics log.

## Build

The GitHub Actions workflow `.github/workflows/ios.yml` builds an unsigned IPA
on a standard `macos-15` runner; no Mac is required locally.

```
ios/
  project.yml                        XcodeGen project
  SpartanPort/
    Core/                            Swift JIT/memory/library controllers
    Native/                          JITAllocator.c/.h + bridging header
    Resources/                       Info.plist, entitlements, JIT script
    UI/                              SwiftUI app
```

Local generation (on a Mac, if you ever have one):

```sh
cd ios
xcodegen generate
open SpartanPort.xcodeproj
```

## Signing

The artifact is unsigned. Use SideStore, AltStore, Sideloadly, ESign or
Xcode, and make sure the signing profile grants:

| Entitlement | Why |
|---|---|
| `get-task-allow` | lets StikDebug attach; without it JIT cannot be enabled |
| `com.apple.developer.kernel.increased-memory-limit` | Memory+ (larger jetsam limit) |
| `com.apple.developer.kernel.extended-virtual-addressing` | 512 GB address map; the guest window is far above the default 63 GB map |

Free Apple developer profiles usually include `get-task-allow` but may not
include the two kernel entitlements. The badges in the app read the actual
signature, so they show what the installed copy really has.

## Status

| Stage | State |
|---|---|
| iOS host, JIT, Memory+, CI IPA | implemented in this tree |
| FEXCore iOS static libraries | next |
| Guest memory map + loader | next |
| shadPS4 → Vulkan → MoltenVK | next |
| Audio/video, launcher | next |

The Android branch runs a Linux ARM64/glibc ELF; iOS cannot execute it. The
runtime is being rebuilt as a Mach-O framework inside this app, reusing
Madeira's iOS FEX build and its JIT/memory work.
