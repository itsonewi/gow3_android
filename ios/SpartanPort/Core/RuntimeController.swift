import Foundation
import UIKit
import Darwin

/// Owns the JIT session (StikDebug + the pool) and the guest session.
///
/// The JIT flow is Madeira's: open StikDebug's `enable-jit` URL with
/// spartanport-jit.js, wait for CS_DEBUGGED, allocate the pool with
/// sp_pool_prepare (BRK #0xf00d, vm_remap RW alias, no-footprint), detach the
/// debugger and keep the pool for FEX. Everything the guest does afterwards
/// happens in the runtime library on a worker thread.
@MainActor
final class RuntimeController: ObservableObject {
    enum JITState: Equatable {
        case idle
        case enabling
        case attached
        case ready
        case failed(String)

        var label: String {
            switch self {
            case .idle: return "Not enabled"
            case .enabling: return "Waiting for StikDebug…"
            case .attached: return "Debugger attached"
            case .ready: return "JIT ready"
            case .failed(let message): return message
            }
        }
    }

    @Published private(set) var jitState: JITState = .idle
    @Published private(set) var pool: JITPool?
    @Published private(set) var entitlements = EntitlementStatus.check()
    @Published private(set) var signing = SigningStatus.current
    @Published private(set) var addressMap = AddressMap.current()
    @Published private(set) var debuggerAttached = false
    @Published private(set) var stikDebugInstalled = false
    @Published private(set) var availableMemory: UInt64 = 0
    @Published private(set) var appImage = AppImageRange.current()
    @Published private(set) var runtimeBuild = ""
    @Published var poolSizeMB: Int = JITPoolAllocator.suggestedSizeMB()
    @Published private(set) var busy = false

    /// The guest session, created by play(). ContentView observes it directly.
    @Published private(set) var session = GameSession()

    private var waiter: Timer?

    init() {
        jit_install_trap_handler()
        sp_set_log(spartanPortLogHook, nil)
        runtimeBuild = String(cString: sp_build_info())
        refresh()
        let device = UIDevice.current
        LogStore.shared.log(
            "[startup] \(device.model) \(device.systemName) "
            + "\(ProcessInfo.processInfo.operatingSystemVersionString) "
            + "available=\(ProcessInfo.processInfo.physicalMemory >> 20)MB",
            level: .info)
        LogStore.shared.log("[startup] address map \(addressMap.description)", level: .info)
        LogStore.shared.log("[startup] app image \(appImage.description)", level: .info)
        LogStore.shared.log("[startup] runtime: \(runtimeBuild)", level: .info)
        LogStore.shared.log(
            "[startup] entitlements: increased-memory=\(entitlements.increasedMemory ? 1 : 0) "
            + "extended-va=\(entitlements.extendedVA ? 1 : 0) "
            + "allow-jit=\(entitlements.jitAllowed ? 1 : 0)",
            level: entitlements.increasedMemory && entitlements.extendedVA ? .success : .warning)
        if !signing.debuggable {
            LogStore.shared.log("[startup] \(SigningStatus.notDebuggableMessage)", level: .error)
        }
        // The PS4 user window starts at 64 GiB; without extended virtual
        // addressing every guest mapping fails, so say so before a session.
        sp_check_guest_window()
        sp_log_address_space()

        // Memory+ experiment: with SPARTAN_MEMORY_HOST=1, start the extension
        // that iOS leaves without a jetsam limit and log its budget.
        MemoryDonor.probe()
    }

    func refresh() {
        entitlements = EntitlementStatus.check()
        signing = SigningStatus.current
        addressMap = AddressMap.current()
        debuggerAttached = isDebuggerAttached()
        stikDebugInstalled = Self.stikDebugURL != nil
        availableMemory = jit_available_memory()
        appImage = AppImageRange.current()
    }

    private static var stikDebugURL: URL? {
        let candidates = ["stikdebug://enable-jit", "stikjit://enable-jit"]
        for text in candidates {
            if let url = URL(string: text), UIApplication.shared.canOpenURL(url) {
                return url
            }
        }
        return nil
    }

    private var scriptData: Data? {
        guard let url = Bundle.main.url(forResource: "spartanport-jit", withExtension: "js") else {
            return nil
        }
        return try? Data(contentsOf: url)
    }

    /// Open StikDebug with our bundled script, targeting this process.
    func enableJIT() {
        guard !busy else { return }
        refresh()

        guard signing.debuggable else {
            fail(SigningStatus.notDebuggableMessage)
            return
        }
        guard let base = Self.stikDebugURL else {
            fail("StikDebug is not installed (stikdebug:// scheme not found). Install StikDebug, "
                 + "then tap Enable JIT again.")
            return
        }
        guard let bundleID = Bundle.main.bundleIdentifier, let scriptData else {
            fail("spartanport-jit.js is missing from the app bundle.")
            return
        }

        var components = URLComponents(url: base, resolvingAgainstBaseURL: false)
        components?.queryItems = [
            URLQueryItem(name: "bundle-id", value: bundleID),
            URLQueryItem(name: "pid", value: String(getpid())),
            URLQueryItem(name: "script-data", value: scriptData.base64EncodedString()),
        ]
        guard let url = components?.url else {
            fail("Could not build the StikDebug request.")
            return
        }

        jitState = .enabling
        LogStore.shared.log("[jit] opening StikDebug for pid \(getpid())", level: .info)
        UIApplication.shared.open(url, options: [:]) { [weak self] opened in
            guard let self else { return }
            Task { @MainActor in
                if opened {
                    self.waitForDebugger()
                } else {
                    self.fail("iOS refused to open StikDebug. Is it installed and allowed to open URLs?")
                }
            }
        }
    }

    private func waitForDebugger() {
        waiter?.invalidate()
        let deadline = Date().addingTimeInterval(90)
        LogStore.shared.log("[jit] waiting for CS_DEBUGGED (timeout 90s)…", level: .info)

        let timer = Timer(timeInterval: 0.5, repeats: true) { [weak self] timer in
            Task { @MainActor in
                guard let self else {
                    timer.invalidate()
                    return
                }
                if self.csDebugged() && isDebuggerAttached() {
                    timer.invalidate()
                    self.waiter = nil
                    self.debuggerAttached = isDebuggerAttached()
                    self.jitState = .attached
                    LogStore.shared.log("[jit] CS_DEBUGGED is set; debugger attached", level: .success)
                    self.preparePool()
                } else if Date() >= deadline {
                    timer.invalidate()
                    self.waiter = nil
                    self.fail("StikDebug did not attach within 90 seconds. Check its pairing file and "
                              + "LocalDevVPN, then try again.")
                }
            }
        }
        waiter = timer
        RunLoop.main.add(timer, forMode: .common)
    }

    /// Allocate the pool, then detach. Everything FEX executes afterwards comes
    /// from these pages; iOS offers no other route to executable memory.
    func preparePool() {
        guard !busy else { return }
        refresh()
        guard csDebugged() || pool != nil else {
            fail("Enable JIT first: StikDebug must be attached to answer the pool request.")
            return
        }
        guard pool == nil else { return }

        busy = true
        let megabytes = max(64, min(poolSizeMB, 1024))
        LogStore.shared.log("[jit] requesting \(megabytes) MB pool…", level: .info)

        DispatchQueue.global(qos: .userInitiated).async {
            do {
                let newPool = try JITPoolAllocator.allocate(sizeMB: megabytes)
                JITPoolAllocator.detach()
                DispatchQueue.main.async {
                    self.pool = newPool
                    self.jitState = .ready
                    self.busy = false
                    self.refresh()
                    LogStore.shared.log(
                        "[jit] pool ready: RX \(newPool.rxDescription) RW \(newPool.rwDescription) "
                        + "\(newPool.sizeDescription)", level: .success)
                }
            } catch {
                DispatchQueue.main.async {
                    self.busy = false
                    self.fail(error.localizedDescription)
                }
            }
        }
    }

    func detach() {
        guard csDebugged() else {
            LogStore.shared.log("[jit] no debugger session to detach", level: .warning)
            return
        }
        JITPoolAllocator.detach()
        refresh()
    }

    func testMapping() {
        LogStore.shared.log("[jit] running non-executing mapping probe…", level: .info)
        DispatchQueue.global(qos: .userInitiated).async {
            let ok = JITPoolAllocator.testMapping()
            DispatchQueue.main.async {
                LogStore.shared.log("[jit] mapping probe \(ok ? "passed" : "failed")",
                                    level: ok ? .success : .error)
            }
        }
    }

    // MARK: - Playing the port

    /// Starts the guest. The prepared folder is the one holding boot.bin (made
    /// on a computer by scripts/prepare.py from the user's dump).
    func play(game: GameLibrary, cpuOnly: Bool) {
        guard !busy else { return }
        refresh()
        guard entitlements.extendedVA else {
            fail("This copy is missing com.apple.developer.kernel.extended-virtual-addressing. The "
                 + "guest maps its image and memory above 64 GiB, outside the default 63 GiB map, so "
                 + "the first mapping would fail. Reinstall with a profile that grants it.")
            return
        }
        guard let prepared = game.preparedFolder else {
            fail("Import a prepared runtime folder (the one with boot.bin). The game dump alone "
                 + "cannot be prepared on iOS: run scripts/prepare.py on a computer first.")
            return
        }
        guard pool != nil else {
            fail("Enable JIT first: the guest's CPU is FEX's JIT and needs the pool.")
            return
        }
        guard let userDir = Self.userDirectory() else { return }
#if SPARTANPORT_GPU
        // The GPU build (SpartanPortGPU target) links shadPS4 + MoltenVK; the
        // weak stubs in Runtime/gpu are overridden, so the loader may create the
        // Metal surface and the Vulkan device.
        _ = cpuOnly
        session.start(gameDirectory: prepared, app0: game.dumpFolder, userDirectory: userDir,
                      pool: pool, cpuOnly: false)
#else
        session.start(gameDirectory: prepared, app0: game.dumpFolder, userDirectory: userDir,
                      pool: pool, cpuOnly: cpuOnly)
#endif
    }

    func stop() {
        _ = sp_session_stop()
        session.refresh()
    }

    static func userDirectory() -> URL? {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
        guard let base else { return nil }
        let url = base.appendingPathComponent("SpartanPort", isDirectory: true)
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    /// Reads CS_DEBUGGED without logging, for the 0.5 s poll.
    private func csDebugged() -> Bool {
        var flags: UInt32 = 0
        return jit_cs_status(&flags) && (flags & 0x1000_0000) != 0
    }

    private func fail(_ message: String) {
        jitState = .failed(message)
        LogStore.shared.log("[jit] \(message)", level: .error)
    }
}

/// Where the app's own Mach-O image landed, from dyld image 0.
struct AppImageRange {
    let base: UInt64
    let end: UInt64
    let known: Bool

    static func current() -> AppImageRange {
        var base: UInt = 0
        var end: UInt = 0
        jit_app_image_range(&base, &end)
        return AppImageRange(base: UInt64(base), end: UInt64(end), known: end > base)
    }

    var description: String {
        guard known else { return "unknown" }
        return String(format: "0x%llx – 0x%llx", base, end)
    }

    /// The guest maps its image, direct-memory views and handles in
    /// [0x1000000000, 0xfc00000000); an app image inside that range would
    /// collide with them.
    var overlapsGuestWindow: Bool {
        known && end > 0x1_0000_0000 && base < 0xfc_0000_0000
    }
}
