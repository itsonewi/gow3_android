import Foundation
import Darwin

/// A dual-mapped JIT pool: `rx` is the view FEX executes code from (the
/// debugger's RWX run, or its RWX alias when TXM kept the run RX-only), `rw` is
/// the writable alias of the same pages. Both are made by sp_pool_prepare().
struct JITPool {
    let rx: UnsafeMutableRawPointer
    let rw: UnsafeMutableRawPointer
    let size: Int
    let noFootprint: Bool
    /// The C handle, kept so the pool can be released with the right addresses.
    fileprivate let handle: SPPool

    var rxDescription: String { String(format: "0x%lx", UInt(bitPattern: rx)) }
    var rwDescription: String { String(format: "0x%lx", UInt(bitPattern: rw)) }
    var sizeDescription: String { "\(size >> 20) MB" }
}

enum JITPoolError: LocalizedError {
    case prepare(Int32, String)

    var errorDescription: String? {
        switch self {
        case .prepare(_, let message): return message
        }
    }
}

/// The pool policy and the C layer around it.
///
/// Madeira sizes its pool to the largest free run the process has (a census of
/// the low gap), shrinks it when it does not fit and can split it around a hole.
/// The iOS port keeps the same shape: a suggested size from device RAM, a
/// shrink-and-retry loop, and a hard check that the debugger put the pool
/// outside the guest user window the game will map.
enum JITPoolAllocator {
    static let pageSize = 0x4000

    /// The PS4 user window the guest maps itself (USER_MIN/USER_MAX in
    /// src/runtime_memory.c). The pool must not land inside it, or the guest's
    /// image, direct-memory views and handles would collide with FEX code.
    static let guestUserMin: UInt64 = 0x1_0000_0000
    static let guestUserMax: UInt64 = 0xfc_0000_0000

    static func suggestedSizeMB() -> Int {
        let physical = ProcessInfo.processInfo.physicalMemory >> 20
        let available = jit_available_memory() >> 20
        var wanted = max(128, min(768, Int(physical / 6)))
        if available > 0 && UInt64(wanted) > available / 2 {
            wanted = max(64, Int(available / 2))
        }
        // Snap to the launcher's picker so the menu never shows a blank value.
        let options = [128, 256, 512, 896, 1024]
        return options.last(where: { $0 <= wanted }) ?? 128
    }

    static func allocate(sizeMB: Int) throws -> JITPool {
        var attempt = max(64, sizeMB)
        var lastMessage = "the debugger did not answer the pool request"
        for _ in 0..<4 {
            var out = SPPool()
            let code = sp_pool_prepare(nil, size_t(attempt << 20), &out)
            if code == SP_JIT_OK, let exec = out.exec, let rw = out.rw, out.size > 0 {
                let size = out.size
                guard !overlapsGuestWindow(exec, size) else {
                    var pool = out
                    sp_pool_release(&pool)
                    throw JITPoolError.prepare(code, "The debugger put the JIT pool inside the PS4 "
                        + "user window. Restart the app and try again.")
                }
                LogStore.shared.log("[jit] pool prepared at \(String(format: "0x%lx", UInt(bitPattern: exec))) "
                                    + "(\(size >> 20) MB, RWX=\(out.rx_writable), "
                                    + "no-footprint=\(out.no_footprint == 1 ? "yes" : "no"))",
                                    level: .success)
                return JITPool(rx: exec, rw: rw, size: size,
                               noFootprint: out.no_footprint == 1, handle: out)
            }
            if code == SP_JIT_NO_DEBUGGER {
                lastMessage = String(cString: sp_jit_last_error())
                break
            }
            lastMessage = String(cString: sp_jit_last_error())
            // The reference shrinks to the largest pool that fits instead of
            // failing the launch; do the same once per step.
            attempt = attempt / 2
            if attempt < 64 { break }
            LogStore.shared.log("[jit] \(attempt * 2) MB pool refused (\(lastMessage)); retrying with \(attempt) MB",
                                level: .warning)
        }
        throw JITPoolError.prepare(SP_JIT_ALLOC_FAILED, lastMessage)
    }

    static func overlapsGuestWindow(_ rx: UnsafeMutableRawPointer, _ size: Int) -> Bool {
        let base = UInt64(UInt(bitPattern: rx))
        return base < guestUserMax && guestUserMin < base + UInt64(size)
    }

    static func detach() {
        sp_pool_detach()
    }

    /// Non-executing diagnostic: dual map, coherence and RX permission.
    @discardableResult
    static func testMapping() -> Bool {
        jit_test_mapping()
    }
}
