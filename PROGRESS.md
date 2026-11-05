# SpartanPort iOS — living progress & handoff

> **Rule for every agent:** update this file whenever you change code, run a
> workflow, discover a new failure, or reach a new device/runtime state. Add a
> row to **Workflow log**, a bullet to **Change log**, and refresh **Current
> state**. Do not claim the game runs until a real device log proves it.

Last updated: **2026-11-05** · branch `arm` · HEAD `40e28fc` (code identical on
`79a012f`; `40e28fc` is docs only).

---

## 1. What this project is

`gow3_android` is **SpartanPort**, a native God of War III Remastered (PS4)
port. The guest is x86-64 PS4 code, run by **FEXCore**; a custom HLE implements
PS4 libraries in `src/runtime_*.c`, and the stock loader is `src/probe.c`
(prepared `boot.bin`, magic `G3PROBE2`). The GPU is **shadPS4's Vulkan video
core** with **MoltenVK** providing Vulkan on Metal.

This repository is the **iOS port**. The reference for iOS-specific solutions
is **[willfaust/Madeira](https://github.com/willfaust/Madeira)**; reuse its
approach instead of inventing a second protocol.

iOS specifics:

| Layer | Where | State |
|---|---|---|
| SwiftUI launcher, import, diagnostics | `ios/SpartanPort/` | implemented |
| JIT: StikDebug **or built-in StikJIT**, BRK pool | `ios/SpartanPort/Native/JITAllocator.c`, `Core/JITPool.swift`, `ios/SpartanPortJITHelper/` | implemented |
| Memory+: entitlements, guest window | `ios/SpartanPort/Resources/`, `ios/SpartanPortMemoryHost/` | implemented |
| PS4 memory/loader/kernel HLE | stock `src/*.c` + `src/probe.c` | compiled in |
| CPU: FEXCore on the JIT pool | `ios/Runtime/cpu/` | compiled in |
| GPU: shadPS4 → Vulkan → MoltenVK | `gpu/`, `out/gpu-ios/libspartanportgpu-all.a` | linked, **device run pending** |
| Audio/pad | SDL3 `runtime_pad.c` / `runtime_audio.c` in the GPU app | compiled in |
| ATRAC9 (libSceAjm) | vendored LibAtrac9 + `runtime_ajm.c` | compiled in |
| MP3 / movies | FFmpeg-free stubs | **gap** |
| Touch overlay | — | **not started** |

The two apps are separate targets so a CPU-only diagnostic build stays small:

- `SpartanPort` — CPU-only (`--cpu-only`), weak GPU stubs.
- `SpartanPortGPU` — links `libspartanportgpu-all.a`, runs without `--cpu-only`,
  so the real `spartanportgpu_*` symbols create the `CAMetalLayer`, MoltenVK
  device and presenter.

## 2. Current state (read this first)

**Both builds are green on GitHub Actions:**

- CPU IPA: run [42040743104](https://github.com/itsonewi/gow3_android/actions/runs/42040743104)
  (`Build iOS IPA`, artifact `SpartanPort-ipa-macos-15-intel` +
  `SpartanPort-dsym-macos-15-intel`).
- GPU IPA: run [42037347032](https://github.com/itsonewi/gow3_android/actions/runs/42037347032)
  (`Build iOS GPU app`, artifact `spartanport-ios-gpu`).
- Structural tests: `python -m unittest discover -s ios/tests` → **30 pass**.

**The current problem is not compilation. It is that no real device run has
confirmed the boot path.** The next person must get a device log from
`SpartanPortGPU.ipa` and continue from the first line that stops.

A device with **iOS 27** is available and the user will install the IPA. The
built-in StikJIT path is designed for iOS 26+; iOS 27 also supports in-app
pairing on Madeira, but that Rust helper is **not ported yet**, so a pairing
file is still required (see §4).

### What was proven on CI

- Full iOS app builds with Xcode 26.3 (the workflow picks the newest
  `/Applications/Xcode_2[6-9]*.app`; the default 16.4 cannot read the bundled
  StikJIT interface).
- `SpartanPortGPU.app` contains: the Metal surface, MoltenVK, shadPS4 shader
  recompiler, SDL3 pad/audio, LibAtrac9, `StikJIT.framework`, and
  `PlugIns/SpartanPortJITHelper.appex`.
- `StikJIT.framework` is embedded at `Payload/SpartanPortGPU.app/Frameworks/`;
  the helper has
  `LC_RPATH @executable_path/../../Frameworks`, so it resolves the framework.
- MoltenVK portability is enabled (`VK_KHR_portability_enumeration` +
  `VK_KHR_portability_subset`, static `vkGetInstanceProcAddr`). Without it
  `vkEnumeratePhysicalDevices` returns zero.

### Not proven

- Metal surface creation and MoltenVK device creation on hardware.
- FEXCore executing the guest on the JIT pool on hardware.
- Any frame presented; any audio heard; any controller/touch input.
- Jetsam behaviour with the guest pool + MoltenVK + 17 MB binary.

## 3. How to install and produce a useful log

1. Sign the IPA with a profile that keeps **app extensions and embedded
   frameworks** (SideStore, AltStore, Plume, Sideloadly, Xcode). The app, the
   JIT helper and `StikJIT.framework` must be signed together. Free-account
   signing works for 7 days.
2. Required entitlements (already in `ios/SpartanPort/Resources/SpartanPort.entitlements`):
   - `get-task-allow` (JIT / debugger attach),
   - `com.apple.developer.kernel.increased-memory-limit`,
   - `com.apple.developer.kernel.extended-virtual-addressing`.
3. Install `LocalDevVPN` and connect it. The JIT helper reaches this iPhone's
   `lockdownd` through `10.7.0.1:62078`.
4. Put this iPhone's **pairing file** into the app:
   - JIT card → **Pairing file** → choose `pairingFile.plist`, or
   - drop it at `Documents/StikJIT/pairingFile.plist` through Finder file
     sharing.
5. JIT card → Method:
   - **Automatic** uses StikDebug if installed, otherwise built-in.
   - **Built-in** forces the bundled helper (no StikDebug app needed).
6. Tap **Enable JIT**, wait for `CS_DEBUGGED`, then **Pool**.
7. Open the in-app **Log** and copy the `[jit]`, `[runtime]`, `GPU:` and
   `VideoOut:` lines. If the app dies, get the crash report from
   Settings → Privacy & Security → Analytics & Improvements.

The built-in helper's request only completes after the app prepares its pool
and sends the detach BRK; that is expected. `SpartanPortJITHelper` logs under
`[SpartanPortJIT]` in Console.app/`os_log`.

## 4. Known gaps / risks (ordered)

1. **No confirmed device boot.** Start here before adding features.
2. **Built-in JIT needs a pairing file.** No in-app pairing generation yet
   (Madeira's `libmadeira_rppairing.a` / `JITPairing.swift` is the reference on
   iOS 27). Built-in requires **iOS 26+**; older iOS must use StikDebug.
3. **Touch controls.** iPhone has no gamepad/keyboard; current SDL pad path
   supports MFI/Bluetooth controllers only. A touch overlay is not started.
4. **Vulkan feature assertions.** `vk_instance.cpp` asserts
   `robustBufferAccess2`, `robustImageAccess2`, `nullDescriptor`, push
   descriptors, vertex divisor and Vulkan 1.3. MoltenVK may not expose all of
   them; the first device log will say which. shadPS4 upstream has a fallback
   for missing `nullDescriptor` (PR #1001) that is not ported here.
5. **Movies/MP3.** `runtime_videodec.c` and `runtime_mp3.c` need FFmpeg; current
   stubs fail at the first movie/MP3 import. ATRAC9 is decoded.
6. **Signing.** Unsigned CI IPA has no entitlements until the sideloader adds
   them; a missing extended-VA entitlement makes every guest mapping fail.
7. **Xcode/Swift interface.** `StikJIT.framework`'s interface was emitted by
   Swift 6.4; the committed `.swiftinterface` files have module selectors
   (`Module::Type`) stripped and `-interface-compiler-version 6.3` so Xcode 26
   imports them. If the framework is replaced, re-apply
   `patch_framework_interface.py` (kept outside the repo at the workspace root;
   recreate it from the description if missing).
8. **Remote branch can move.** Before pushing, `git fetch origin arm`; another
   agent/session may have advanced it (this happened during development).

## 5. Workflow log

Add a row for every workflow dispatch/result. Keep failures; they document the
fix path.

| Date (UTC) | Run | Workflow | Commit | Result | What it proved / fixed |
|---|---|---|---|---|---|
| 2026-11-05 13:59 | [42040743104](https://github.com/itsonewi/gow3_android/actions/runs/42040743104) | Build iOS IPA | `40e28fc` | ✅ | CPU IPA + dSYM; built-in JIT bundled; 30 tests pass |
| 2026-11-05 13:37 | [42037347032](https://github.com/itsonewi/gow3_android/actions/runs/42037347032) | Build iOS GPU app | `79a012f` | ✅ | GPU IPA with helper + StikJIT + MoltenVK portability |
| 2026-11-05 13:34 | [42036574857](https://github.com/itsonewi/gow3_android/actions/runs/42036574857) | Build iOS IPA | `79a012f` | ✅ | First green CPU build with built-in helper |
| 2026-11-05 13:24 | [42035443606](https://github.com/itsonewi/gow3_android/actions/runs/42035443606) | Build iOS IPA | `e737971` | ❌ | StikJIT Swift 6.4 `Module::Type` interface unreadable by Xcode 26.3 → stripped selectors |
| 2026-11-05 13:18 | [42034676909](https://github.com/itsonewi/gow3_android/actions/runs/42034676909) | Build iOS IPA | `0c49f7f` | ❌ | App extension must be `APPLICATION_EXTENSION_API_ONLY=YES` |
| 2026-11-05 13:10 | [42033405326](https://github.com/itsonewi/gow3_android/actions/runs/42033405326) | Build iOS IPA | `b3e231f` | ✅ | dSYM artifact publishing |
| 2026-11-05 13:03 | [42032313154](https://github.com/itsonewi/gow3_android/actions/runs/42032313154) | Build iOS IPA | `2fc8dda` | ✅ | CPU after MoltenVK/pad/audio/ATRAC9 docs |
| 2026-11-05 12:?? | [42028897548](https://github.com/itsonewi/gow3_android/actions/runs/42028897548) | Build iOS GPU app | `b06d173` | ✅ | First GPU IPA with SDL3 pad/audio + LibAtrac9 |
| 2026-11-05 12:?? | [42024388638](https://github.com/itsonewi/gow3_android/actions/runs/42024388638) | Build iOS GPU app | `2d76d02` | ❌ | Darwin has no `clock_nanosleep`; audio sleep fixed |
| 2026-11-05 12:?? | [42022807764](https://github.com/itsonewi/gow3_android/actions/runs/42022807764) | Build iOS GPU app | `0285f0a` | ❌ | SDL3 headers live in `ios/.build/ios/include` |
| 2026-11-05 11:?? | [42014702784](https://github.com/itsonewi/gow3_android/actions/runs/42014702784) | Build iOS GPU app | `91b54b6` | ✅ | MoltenVK portability + static `vkGetInstanceProcAddr` |

Earlier history (before this table) is in `git log --oneline`.

## 6. Change log (newest first)

- **2026-11-05** — Built-in StikJIT: `SpartanPortJITHelper.appex`, JSON host,
  pairing store, Picker/import UI, framework embedded; Xcode 26 interface patch.
- **2026-11-05** — GPU app compiles real SDL3 `runtime_pad.c` /
  `runtime_audio.c`; MP3 stays weak FFmpeg stub; LibAtrac9 + `runtime_ajm.c`.
- **2026-11-05** — MoltenVK: portability enumeration/subset, static entry point;
  verified strings and device enumeration prerequisites.
- **2026-11-05** — GPU build: ATRAC9, pad/audio, Darwin shims (`clock_nanosleep`,
  `st_mtimespec`, `PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP`, SDL header path).
- **2026-11-05** — First green GPU IPA (`b06d173`), then built-in JIT.
- Before that — CPU IPA green, JIT pool/BRK layer, FEX iOS branch
  `ios-port-2607`, loader/HLE, Memory+ extension, JIT pool.

## 7. Commands

```sh
# tests
python -m unittest discover -s ios/tests

# CPU IPA
gh workflow run ios.yml --ref arm
# GPU IPA (manual; takes longer)
gh workflow run ios-gpu.yml --ref arm -f build_app=true

# watch / inspect
gh run list --limit 5
gh run view <id> --json status,conclusion,jobs
gh run download <id> -n spartanport-ios-gpu -D artifacts/<id>
```

Build locally (macOS only): `bash ios/scripts/build-gpu-ios.sh --app` after
`ios/scripts/fetch-fex.sh` and `ios/scripts/build-fex-ios.sh`; XcodeGen is
required. FEX must be the `ios-port-2607` branch, not `main`.

## 8. For the next agent

1. Read this file, then `ios/README.md`, `gpu/README-ios.md`, `RUNTIME.md`.
2. `git fetch origin arm`, check `git log --oneline -5`, never force-push.
3. Run the structural tests before pushing.
4. Update this file in the **same commit** as the change/workflow; keep the
   table chronological and the "Current state" truthful.
5. Do not claim the game runs from a green build. A green CI build means it
   compiles/links/packages; it does not mean MoltenVK created a device or the
   guest reached a frame.
6. When a device log arrives, add its first failing line to §2/§4 and make that
   the next task. Preserve the user's files and settings.
7. Prefer Madeira's implementation when a problem is already solved there; cite
   the file/commit in the change log.

## 9. Next steps (suggested order)

1. Device run of `SpartanPortGPU.ipa` on iOS 27; capture `[jit]`, `GPU:`,
   `VideoOut:` and any crash report.
2. Fix the first GPU/MoltenVK feature assertion if it aborts (log says which).
3. Add in-app pairing (Madeira's Rust `rppairing` + `JITPairing.swift`) so no
   computer/StikDebug is needed at all.
4. Touch overlay for iPhone (map to `runtime_pad` injected state or a new C
   setter).
5. FFmpeg/VideoToolbox for videodec + MP3 (movies and music).
6. Performance/frame pacing and jetsam tuning.
