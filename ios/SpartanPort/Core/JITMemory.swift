import Foundation
import Darwin

/// A dual-mapped JIT pool: RX is executable (debugger-supplied), RW is the
/// writable alias of the same VM object. FEX writes generated blocks to RW and
/// executes them from RX.
struct JITPool {
    let rx: UnsafeMutableRawPointer
    let rw: UnsafeMutableRawPointer
    let size: Int

    var rxDescription: String { String(format: "0x%lx", UInt(bitPattern: rx)) }
    var rwDescription: String { String(format: "0x%lx", UInt(bitPattern: rw)) }
    var sizeDescription: String { "\(size / 1024 / 1024) MB" }
}

enum JITMemoryError: LocalizedError {
    case noDebugger
    case aliasFailed(kern_return_t)
    case protectFailed(kern_return_t)

    var errorDescription: String? {
        switch self {
        case .noDebugger:
            return "StikDebug did not answer the JIT request. Enable JIT first and keep StikDebug attached."
        case .aliasFailed(let kr):
            return "Could not map the writable JIT alias (vm_remap error \(kr)). Restart the app and retry."
        case .protectFailed(let kr):
            return "Could not make the JIT alias writable (vm_protect error \(kr))."
        }
    }
}

enum JITMemory {
    /// Ask the attached debugger for a run of RX pages, then map a RW alias of
    /// the same VM object and mark it jetsam-exempt.
    ///
    /// The BRK is answered by spartanport-jit.js, which allocates with `_M` and
    /// calls prepare_memory_region(). Without a debugger the SIGTRAP fallback
    /// returns x0 = 0 and this throws instead of crashing.
    static func allocate(size requestedSize: Int) throws -> JITPool {
        let page = 0x4000
        let size = (requestedSize + page - 1) & ~(page - 1)

        guard let rx = jit26_prepare_region(nil, size) else {
            throw JITMemoryError.noDebugger
        }

        var rwAddress: vm_address_t = 0
        var currentProtection: vm_prot_t = 0
        var maximumProtection: vm_prot_t = 0
        var kr = vm_remap(
            mach_task_self_,
            &rwAddress,
            vm_size_t(size),
            0,
            VM_FLAGS_ANYWHERE,
            mach_task_self_,
            vm_address_t(UInt(bitPattern: rx)),
            0,
            &currentProtection,
            &maximumProtection,
            VM_INHERIT_NONE
        )
        if kr != KERN_SUCCESS {
            // Some SDKs refuse a hint above the 63 GB map; retry without one.
            if kr == KERN_NO_SPACE {
                rwAddress = 0
                kr = vm_remap(
                    mach_task_self_,
                    &rwAddress,
                    vm_size_t(size),
                    0,
                    VM_FLAGS_ANYWHERE,
                    mach_task_self_,
                    vm_address_t(UInt(bitPattern: rx)),
                    0,
                    &currentProtection,
                    &maximumProtection,
                    VM_INHERIT_NONE
                )
            }
            guard kr == KERN_SUCCESS else {
                vm_deallocate(mach_task_self_, vm_address_t(UInt(bitPattern: rx)), vm_size_t(size))
                throw JITMemoryError.aliasFailed(kr)
            }
        }

        let protectResult = vm_protect(mach_task_self_, rwAddress, vm_size_t(size), 0,
                                       VM_PROT_READ | VM_PROT_WRITE)
        guard protectResult == KERN_SUCCESS else {
            vm_deallocate(mach_task_self_, rwAddress, vm_size_t(size))
            vm_deallocate(mach_task_self_, vm_address_t(UInt(bitPattern: rx)), vm_size_t(size))
            throw JITMemoryError.protectFailed(protectResult)
        }

        guard let rw = UnsafeMutableRawPointer(bitPattern: UInt(rwAddress)) else {
            throw JITMemoryError.aliasFailed(KERN_INVALID_ADDRESS)
        }

        let exempt = jit_make_region_no_footprint(rw, size, "spartanport-pool")
        LogStore.shared.log(
            "[jit] pool ready: RX=\(String(format: "0x%lx", UInt(bitPattern: rx))) "
            + "RW=\(String(format: "0x%lx", UInt(rwAddress))) size=\(size >> 20)MB "
            + "no-footprint=\(exempt ? "applied" : "refused")",
            level: exempt ? .success : .warning
        )

        return JITPool(rx: rx, rw: rw, size: size)
    }

    /// Detach StikDebug once the pool exists. FEX only needs the pool; it does
    /// not need the debugger to keep answering BRKs.
    static func detachDebugger() {
        guard jit_check_debugged() else {
            LogStore.shared.log("[jit] detach skipped: no debugger session", level: .warning)
            return
        }
        jit26_detach()
        LogStore.shared.log("[jit] detach requested", level: .info)
    }

    /// Non-executing diagnostic: dual map, coherence and RX permission.
    @discardableResult
    static func testMapping() -> Bool {
        jit_test_mapping()
    }
}
