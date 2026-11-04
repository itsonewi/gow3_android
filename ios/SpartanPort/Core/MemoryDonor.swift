import Foundation
import Darwin

/// Memory+ donation experiment, the iOS port of Madeira's MadeiraMemoryHost.
///
/// The extension is a `com.apple.ar.viewer` app extension: iOS 27's jetsam table
/// gives that extension point no memory limit (LiveContainer runs guest apps in
/// one). The extension creates memory objects and hands them to the app through
/// XPC; this side starts it and asks for its budget.
///
/// It is a diagnostic until the direct-memory pool is actually backed by a
/// donated memory entry: enable it with env `SPARTAN_MEMORY_HOST=1` (Xcode
/// scheme or an installer that sets it) and the log shows the extension's
/// footprint next to the app's.
@MainActor
enum MemoryDonor {
    static var isRequested: Bool {
        ProcessInfo.processInfo.environment["SPARTAN_MEMORY_HOST"] == "1"
    }

    static var helperIdentifier: String? {
        guard let url = Bundle.main.builtInPlugInsURL?.appendingPathComponent("SpartanPortMemoryHost.appex") else {
            return nil
        }
        return Bundle(url: url)?.bundleIdentifier
    }

    static var isAvailable: Bool { helperIdentifier != nil }

    /// Starts the extension and logs what its process may allocate.
    static func probe() {
        guard isRequested else { return }
        guard let identifier = helperIdentifier else {
            LogStore.shared.log("[memory+] the memory host extension is missing from this install",
                                level: .warning)
            return
        }
        if NSClassFromString("NSExtension") == nil {
            dlopen("/System/Library/Frameworks/ExtensionFoundation.framework/ExtensionFoundation", RTLD_NOW)
        }
        guard let extensionClass = NSClassFromString("NSExtension") else {
            LogStore.shared.log("[memory+] iOS did not provide the extension API", level: .warning)
            return
        }
        let factory = unsafeBitCast(extensionClass as AnyObject, to: NSExtensionClassShim.self)
        let found: AnyObject
        do {
            found = try factory.extension(withIdentifier: identifier)
        } catch {
            LogStore.shared.log("[memory+] cannot find \(identifier): \(error.localizedDescription)",
                                level: .warning)
            return
        }
        let shim = unsafeBitCast(found, to: NSExtensionShim.self)
        let listener = DonorListener()
        let request = DonorRequest(extension: found, shim: shim, listener: listener)
        request.begin()
        active.append(request)
        LogStore.shared.log("[memory+] started \(identifier)", level: .info)
    }

    fileprivate static var active: [DonorRequest] = []

    fileprivate static func finished(_ request: DonorRequest) {
        active.removeAll { $0 === request }
    }
}

@objc private protocol NSExtensionClassShim {
    @objc(extensionWithIdentifier:error:)
    func `extension`(withIdentifier identifier: String) throws -> AnyObject
}

@objc private protocol NSExtensionShim {
    @objc(beginExtensionRequestWithInputItems:completion:)
    func beginExtensionRequest(withInputItems items: [Any], completion: @escaping (NSUUID?) -> Void)
    @objc(setRequestCompletionBlock:)
    func setRequestCompletionBlock(_ block: @escaping (NSUUID?, [Any]?) -> Void)
    @objc(setRequestCancellationBlock:)
    func setRequestCancellationBlock(_ block: @escaping (NSUUID?, NSError?) -> Void)
}

@MainActor
private final class DonorRequest {
    private let `extension`: AnyObject
    private let shim: NSExtensionShim
    private let listener: DonorListener
    private var connection: NSXPCConnection?

    init(extension found: AnyObject, shim: NSExtensionShim, listener: DonorListener) {
        self.extension = found
        self.shim = shim
        self.listener = listener
    }

    func begin() {
        listener.onReady = { [weak self] connection, pid, available in
            Task { @MainActor in
                guard let self else { return }
                self.connection = connection
                LogStore.shared.log("[memory+] extension pid \(pid), it may allocate \(available >> 20) MB",
                                    level: .success)
                let server = connection.remoteObjectProxy as? MHMemoryServer
                server?.statusWithReply { _, footprint, available in
                    Task { @MainActor in
                        LogStore.shared.log("[memory+] extension footprint \(footprint >> 20) MB, "
                                            + "available \(available >> 20) MB", level: .info)
                        MemoryDonor.finished(self)
                    }
                }
            }
        }
        listener.resume()

        let item = NSExtensionItem()
        item.userInfo = ["endpoint": listener.endpoint]
        shim.setRequestCompletionBlock { _, _ in
            DispatchQueue.main.async { MainActor.assumeIsolated {
                LogStore.shared.log("[memory+] extension request finished", level: .info)
            } }
        }
        shim.setRequestCancellationBlock { _, error in
            DispatchQueue.main.async { MainActor.assumeIsolated {
                LogStore.shared.log("[memory+] extension cancelled: "
                                    + (error?.localizedDescription ?? "unknown"), level: .warning)
            } }
        }
        shim.beginExtensionRequest(withInputItems: [item]) { _ in }
    }
}

/// Accepts the extension's XPC connection and hands it to the request.
final class DonorListener: NSObject, NSXPCListenerDelegate {
    let listener = NSXPCListener.anonymous()
    var onReady: ((NSXPCConnection, Int, UInt64) -> Void)?

    var endpoint: NSXPCListenerEndpoint { listener.endpoint }

    override init() {
        super.init()
        listener.delegate = self
    }

    func resume() { listener.resume() }

    func listener(_ listener: NSXPCListener,
                  shouldAcceptNewConnection connection: NSXPCConnection) -> Bool {
        connection.exportedInterface = MHMemoryServerInterface()
        connection.exportedObject = DonorClient { [weak self] pid, available in
            self?.onReady?(connection, pid, available)
        }
        connection.remoteObjectInterface = NSXPCInterface(with: MHMemoryServer.self)
        connection.resume()
        return true
    }
}

private final class DonorClient: NSObject, MHMemoryClient {
    private let ready: (Int, UInt64) -> Void

    init(ready: @escaping (Int, UInt64) -> Void) {
        self.ready = ready
    }

    func extensionReady(_ pid: Int32, available: UInt64) {
        ready(Int(pid), available)
    }
}
