import Foundation
import os

enum LogLevel: String {
    case info
    case success
    case warning
    case error
}

struct LogEntry: Identifiable {
    let id = UUID()
    let date: Date
    let level: LogLevel
    let text: String
}

/// In-app diagnostic log. It also goes to the unified log, so a crash report or
/// Console.app shows the same JIT/Memory+ decisions the UI does.
final class LogStore: ObservableObject {
    static let shared = LogStore()

    @Published private(set) var entries: [LogEntry] = []

    private let lock = NSLock()
    private let logger = Logger(subsystem: "com.onewi.spartanport", category: "runtime")
    private let formatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SSS"
        return f
    }()

    private init() {}

    func log(_ message: String, level: LogLevel = .info) {
        logger.log(level: level == .error ? .error : .default, "\(message, privacy: .public)")

        let entry = LogEntry(date: Date(), level: level, text: message)
        DispatchQueue.main.async {
            self.entries.append(entry)
            if self.entries.count > 400 {
                self.entries.removeFirst(self.entries.count - 400)
            }
        }
    }

    func text(for entry: LogEntry) -> String {
        "\(formatter.string(from: entry.date))  \(entry.text)"
    }

    func clear() {
        entries.removeAll()
    }
}
