import Foundation

/// The user's game folder. The runtime needs the folder containing eboot.bin,
/// sce_module/ and sce_sys/; on iOS it is imported through the Files picker and
/// kept as a security-scoped bookmark between launches.
@MainActor
final class GameLibrary: ObservableObject {
    @Published private(set) var folder: URL?
    @Published private(set) var importError: String?

    private let bookmarkKey = "spartanport.gameFolderBookmark"
    private var accessing: URL?

    init() {
        restore()
    }

    var ebootURL: URL? {
        folder?.appendingPathComponent("eboot.bin")
    }

    var hasGame: Bool {
        guard let eboot = ebootURL else { return false }
        return FileManager.default.fileExists(atPath: eboot.path)
    }

    /// The prepared runtime folder: the one that holds boot.bin. A folder with
    /// the game dump is not enough; scripts/prepare.py (which needs Python) has
    /// to run on a computer first, and the result is imported here.
    var preparedFolder: URL? {
        guard let folder else { return nil }
        let manager = FileManager.default
        if manager.fileExists(atPath: folder.appendingPathComponent("boot.bin").path) { return folder }
        // The picker often lands on the parent (e.g. the "dist" folder that
        // holds the prepared runtime next to the dump).
        if let children = try? manager.contentsOfDirectory(at: folder, includingPropertiesForKeys: [.isDirectoryKey],
                                                           options: [.skipsHiddenFiles]) {
            for child in children {
                let isDirectory = (try? child.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) ?? false
                if isDirectory, manager.fileExists(atPath: child.appendingPathComponent("boot.bin").path) {
                    return child
                }
            }
        }
        return nil
    }

    var isPrepared: Bool { preparedFolder != nil }

    /// The game dump, when the imported folder is (or contains) it; used for
    /// param.sfo, trophy data and the pause-menu WAD.
    var dumpFolder: URL? {
        guard let folder else { return nil }
        if FileManager.default.fileExists(atPath: folder.appendingPathComponent("sce_sys").path) {
            return folder
        }
        if let prepared = preparedFolder, prepared != folder { return folder }
        return nil
    }

    var displayName: String {
        folder?.lastPathComponent ?? "No game folder"
    }

    func setFolder(_ url: URL, bookmark: Data? = nil) {
        importError = nil
        stopAccessing()
        folder = url
        accessing = url
        _ = url.startAccessingSecurityScopedResource()

        do {
            let data = try bookmark ?? url.bookmarkData(options: .minimalBookmark,
                                                        includingResourceValuesForKeys: nil,
                                                        relativeTo: nil)
            UserDefaults.standard.set(data, forKey: bookmarkKey)
            LogStore.shared.log("[library] game folder: \(url.path)", level: .success)
            if !hasGame {
                LogStore.shared.log("[library] eboot.bin not found in the selected folder", level: .warning)
            } else {
                LogStore.shared.log("[library] eboot.bin found", level: .success)
            }
        } catch {
            importError = "Could not save access to this folder: \(error.localizedDescription)"
            LogStore.shared.log("[library] bookmark failed: \(error.localizedDescription)", level: .error)
        }
    }

    func clear() {
        stopAccessing()
        folder = nil
        UserDefaults.standard.removeObject(forKey: bookmarkKey)
        LogStore.shared.log("[library] game folder cleared", level: .info)
    }

    private func restore() {
        guard let data = UserDefaults.standard.data(forKey: bookmarkKey) else { return }
        var stale = false
        do {
            let url = try URL(resolvingBookmarkData: data,
                              options: [],
                              relativeTo: nil,
                              bookmarkDataIsStale: &stale)
            folder = url
            accessing = url
            _ = url.startAccessingSecurityScopedResource()
            if stale {
                LogStore.shared.log("[library] game folder bookmark is stale; re-import it", level: .warning)
            } else {
                LogStore.shared.log("[library] restored game folder: \(url.path)", level: .info)
            }
        } catch {
            LogStore.shared.log("[library] could not restore game folder: \(error.localizedDescription)",
                                level: .warning)
        }
    }

    private func stopAccessing() {
        accessing?.stopAccessingSecurityScopedResource()
        accessing = nil
    }
}
