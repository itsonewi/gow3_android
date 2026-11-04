import Foundation

/// C log hook shared by every session. C function pointers cannot capture, so
/// the destination is the app-wide LogStore.
func spartanPortLogHook(_ level: Int32, _ message: UnsafePointer<CChar>?, _ user: UnsafeMutableRawPointer?) {
    guard let message else { return }
    let text = String(cString: message)
    let logLevel: LogLevel
    switch level {
    case SP_LOG_ERROR: logLevel = .error
    case SP_LOG_WARNING: logLevel = .warning
    default: logLevel = .info
    }
    LogStore.shared.log("[runtime] \(text)", level: logLevel)
}

/// One run of the ported runtime.
///
/// `sp_session_start` launches the stock loader on a worker thread: it maps the
/// prepared boot image, starts FEXCore on the StikDebug pool and enters the
/// game's entry point. This class only mirrors that state for SwiftUI and
/// validates the paths before the C side sees them.
@MainActor
final class GameSession: ObservableObject {
    enum State: Equatable {
        case idle
        case starting
        case running
        case exited

        var label: String {
            switch self {
            case .idle: return "Not running"
            case .starting: return "Starting…"
            case .running: return "Running"
            case .exited: return "Stopped"
            }
        }
    }

    @Published private(set) var state: State = .idle
    @Published private(set) var lastError: String?
    @Published private(set) var build = String(cString: sp_build_info())
    @Published private(set) var startedAt: Date?

    private var poll: Timer?

    var isRunning: Bool { state == .running || state == .starting }

    func start(gameDirectory: URL, app0: URL?, userDirectory: URL, pool: JITPool?, cpuOnly: Bool) {
        guard !isRunning else { return }
        lastError = nil
        state = .starting

        var config = sp_session_config()
        var result: Int32 = -1

        // sp_session_start only reads these strings during the call.
        gameDirectory.path.withCString { game in
            let app0Path = app0?.path ?? ""
            app0Path.withCString { dump in
                userDirectory.path.withCString { user in
                    config.game_dir = game
                    config.app0_dir = dump
                    config.user_dir = user
                    config.cpu_only = cpuOnly ? 1 : 0
                    config.jit_rx = pool?.rx
                    config.jit_rw = pool?.rw
                    config.jit_size = size_t(pool?.size ?? 0)
                    result = sp_session_start(&config, spartanPortLogHook, nil)
                }
            }
        }

        if result != 0 {
            let message = String(cString: sp_session_last_error())
            lastError = message.isEmpty ? "the runtime refused to start (code \(result))" : message
            state = .idle
            LogStore.shared.log("[session] \(lastError ?? "start failed")", level: .error)
            return
        }

        startedAt = Date()
        state = .running
        LogStore.shared.log("[session] runtime started from \(gameDirectory.lastPathComponent)", level: .success)
        startPolling()
    }

    func stop() {
        if sp_session_stop() == 0 {
            state = .exited
            poll?.invalidate()
            poll = nil
        }
    }

    /// Called by the timer and by RuntimeController.stop().
    func refresh() {
        guard state == .running else { return }
        if sp_session_running() == 0 {
            state = .exited
            poll?.invalidate()
            poll = nil
            LogStore.shared.log("[session] the runtime thread ended", level: .warning)
        }
    }

    private func startPolling() {
        poll?.invalidate()
        let timer = Timer(timeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
        poll = timer
        RunLoop.main.add(timer, forMode: .common)
    }
}
