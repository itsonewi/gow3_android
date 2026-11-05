import Foundation

/// The pairing file that lets the JIT helper reach this iPhone's lockdownd
/// through LocalDevVPN. It is a credential for the device itself, so it lives
/// in Application Support, not in Documents (which the Files app exposes).
enum JITPairingStore {
    static var directory: URL? {
        guard let base = FileManager.default.urls(for: .applicationSupportDirectory,
                                                  in: .userDomainMask).first else { return nil }
        let url = base.appendingPathComponent("SpartanPort", isDirectory: true)
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    static var fileURL: URL? {
        directory?.appendingPathComponent("pairingFile.plist")
    }

    /// A pairing file imported earlier, or one an installer left in the
    /// StikDebug-compatible Documents/StikJIT location.
    static func data() -> Data? {
        if let url = fileURL, let data = try? Data(contentsOf: url), !data.isEmpty {
            return data
        }
        if let documents = FileManager.default.urls(for: .documentDirectory,
                                                    in: .userDomainMask).first {
            let legacy = documents.appendingPathComponent("StikJIT/pairingFile.plist")
            if let data = try? Data(contentsOf: legacy), !data.isEmpty {
                try? data.write(to: fileURL ?? legacy, options: .atomic)
                return data
            }
        }
        return nil
    }

    static var hasPairing: Bool { data() != nil }

    static func `import`(from source: URL) throws {
        guard let destination = fileURL else {
            throw NSError(domain: "SpartanPortJIT", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "There is no writable app support folder."])
        }
        let accessed = source.startAccessingSecurityScopedResource()
        defer { if accessed { source.stopAccessingSecurityScopedResource() } }
        let data = try Data(contentsOf: source)
        guard !data.isEmpty else {
            throw NSError(domain: "SpartanPortJIT", code: 2,
                          userInfo: [NSLocalizedDescriptionKey: "The pairing file is empty."])
        }
        try data.write(to: destination, options: .atomic)
        try? FileManager.default.setAttributes([.posixPermissions: 0o600],
                                               ofItemAtPath: destination.path)
    }
}
