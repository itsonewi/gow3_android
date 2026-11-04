import Foundation
import UIKit

/// Owns the JIT session and the memory probes shown on the main screen.
/// The actual BRK protocol lives in Native/JITAllocator.c; this class only
/// drives the StikDebug URL flow and keeps SwiftUI state.
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
            case .failed: return "Failed"
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
    @Published var poolSizeMB: Int = 512
    @Published private(set) var busy = false

    private var waiter: Timer?

    init() {
        jit_install_trap_handler()
        refresh()
        LogStore.shared.log(
            "[startup] \(UIDevice.current.model) \(UIDevice.current.systemName) "
            + "\(ProcessInfo.processInfo.operatingSystemVersionString) "
            + "available=\(ProcessInfo.processInfo.physicalMemory >> 20)MB",
            level: .info
        )
        LogStore.shared.log("[startup] address map \(addressMap.description)", level: .info)
        LogStore.shared.log(
            "[startup] entitlements: allow-jit=\(entitlements.jitAllowed ? 1 : 0) "
            + "increased-memory=\(entitlements.increasedMemory ? 1 : 0) "
            + "extended-va=\(entitlements.extendedVA ? 1 : 0)",
            level: entitlements.increasedMemory && entitlements.extendedVA ? .success : .warning
        )
        if !signing.debuggable {
            LogStore.shared.log("[startup] \(SigningStatus.notDebuggableMessage)", level: .error)
        }
    }

    func refresh() {
        entitlements = EntitlementStatus.check()
        signing = SigningStatus.current
        addressMap = AddressMap.current()
        debuggerAttached = isDebuggerAttached()
        stikDebugInstalled = Self.stikDebugURL != nil
        availableMemory = jit_available_memory()
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

    /// Open StikDebug with our bundled script, targeting this process. StikDebug
    /// attaches, runs the script and services the BRK requests. The user comes
    /// back to SpartanPort; `waitForDebugger` then observes CS_DEBUGGED.
    func enableJIT() {
        guard !busy else { return }
        refresh()

        guard signing.debuggable else {
            fail(SigningStatus.notDebuggableMessage)
            return
        }
        guard let base = Self.stikDebugURL else {
            fail("StikDebug is not installed (stikdebug:// scheme not found). "
                 + "Install StikDebug, then tap Enable JIT again.")
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
                if jit_check_debugged() {
                    timer.invalidate()
                    self.waiter = nil
                    self.debuggerAttached = isDebuggerAttached()
                    self.jitState = .attached
                    LogStore.shared.log("[jit] CS_DEBUGGED is set; debugger attached", level: .success)
                    self.preparePoolAutomatically()
                } else if Date() >= deadline {
                    timer.invalidate()
                    self.waiter = nil
                    self.fail("StikDebug did not attach within 90 seconds. Check its pairing file and LocalDevVPN, "
                              + "then try again.")
                }
            }
        }
        waiter = timer
        RunLoop.main.add(timer, forMode: .common)
    }

    private func preparePoolAutomatically() {
        guard pool == nil else { return }
        preparePool()
    }

    /// Allocate the dual-mapped pool, then detach the debugger. Everything that
    /// needs BRK (FEX code buffers) uses this pool; iOS does not let us create
    /// executable pages any other way.
    func preparePool() {
        guard !busy else { return }
        refresh()
        guard jit_check_debugged() || pool != nil else {
            fail("Enable JIT first: StikDebug must be attached to answer the pool request.")
            return
        }

        busy = true
        let megabytes = max(64, min(poolSizeMB, 1024))
        LogStore.shared.log("[jit] requesting \(megabytes) MB pool…", level: .info)

        DispatchQueue.global(qos: .userInitiated).async {
            do {
                let newPool = try JITMemory.allocate(size: megabytes << 20)
                JITMemory.detachDebugger()
                DispatchQueue.main.async {
                    self.pool = newPool
                    self.jitState = .ready
                    self.busy = false
                    self.refresh()
                    LogStore.shared.log(
                        "[jit] pool ready: RX \(newPool.rxDescription) RW \(newPool.rwDescription) "
                        + "\(newPool.sizeDescription)",
                        level: .success
                    )
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
        guard jit_check_debugged() else {
            LogStore.shared.log("[jit] no debugger session to detach", level: .warning)
            return
        }
        JITMemory.detachDebugger()
        refresh()
    }

    func testMapping() {
        LogStore.shared.log("[jit] running non-executing mapping probe…", level: .info)
        DispatchQueue.global(qos: .userInitiated).async {
            let ok = JITMemory.testMapping()
            DispatchQueue.main.async {
                LogStore.shared.log("[jit] mapping probe \(ok ? "passed" : "failed")",
                                    level: ok ? .success : .error)
            }
        }
    }

    private func fail(_ message: String) {
        jitState = .failed(message)
        LogStore.shared.log("[jit] \(message)", level: .error)
    }
}
