import Foundation
import Security
import UIKit

private typealias SecTaskRef = OpaquePointer

@_silgen_name("SecTaskCopyValueForEntitlement")
private func _SecTaskCopyValueForEntitlement(
    _ task: SecTaskRef,
    _ entitlement: NSString,
    _ error: NSErrorPointer
) -> Unmanaged<CFTypeRef>?

@_silgen_name("SecTaskCreateFromSelf")
private func _SecTaskCreateFromSelf(
    _ allocator: CFAllocator?
) -> SecTaskRef?

/// Reads one entitlement from this process's own signature.
func checkAppEntitlement(_ ent: String) -> Bool {
    guard let task = _SecTaskCreateFromSelf(nil) else { return false }
    defer { Unmanaged<CFTypeRef>.fromOpaque(UnsafeRawPointer(task)).release() }

    guard let value = _SecTaskCopyValueForEntitlement(task, ent as NSString, nil)?.takeRetainedValue() else {
        return false
    }
    if let number = value as? NSNumber {
        return number.boolValue
    }
    return false
}

/// The three capabilities the runtime needs, read from the signed copy.
/// Memory+ is the Increased Memory Limit entitlement; it cannot be switched on
/// inside the app, it must be granted when the IPA is signed.
struct EntitlementStatus {
    let jitAllowed: Bool
    let increasedMemory: Bool
    let extendedVA: Bool

    static func check() -> EntitlementStatus {
        EntitlementStatus(
            jitAllowed: checkAppEntitlement("com.apple.security.cs.allow-jit"),
            increasedMemory: checkAppEntitlement("com.apple.developer.kernel.increased-memory-limit"),
            extendedVA: checkAppEntitlement("com.apple.developer.kernel.extended-virtual-addressing")
        )
    }
}

/// Is a debugger attached to this process right now? This is what StikDebug
/// rides on; CS_DEBUGGED is set while traced.
func isDebuggerAttached() -> Bool {
    var info = kinfo_proc()
    var size = MemoryLayout<kinfo_proc>.stride
    var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()]
    let ret = sysctl(&mib, UInt32(mib.count), &info, &size, nil, 0)
    guard ret == 0 else { return false }
    return (info.kp_proc.p_flag & P_TRACED) != 0
}

/// How this copy was signed, from the kernel's code-signing flags.
struct SigningStatus {
    let known: Bool
    let flags: UInt32

    /// CS_GET_TASK_ALLOW. Unknown flags count as debuggable so a failed query
    /// never blocks a copy that does work.
    var debuggable: Bool { !known || flags & 0x4 != 0 }
    var debugged: Bool { known && flags & 0x1000_0000 != 0 }

    static var current: SigningStatus {
        var flags: UInt32 = 0
        let known = jit_cs_status(&flags)
        return SigningStatus(known: known, flags: flags)
    }

    static let notDebuggableMessage =
        "This copy was signed without get-task-allow, so no debugger can attach and JIT cannot be enabled. "
        + "Reinstall with a development certificate (SideStore, AltStore, Sideloadly or Xcode)."
}

/// A one-line summary of the address map the process actually received.
struct AddressMap {
    let low: UInt64
    let high: UInt64
    let known: Bool

    static func current() -> AddressMap {
        var low: UInt64 = 0
        var high: UInt64 = 0
        let known = jit_task_map_range(&low, &high)
        return AddressMap(low: low, high: high, known: known)
    }

    var description: String {
        guard known else { return "unknown" }
        return String(format: "0x%llx – 0x%llx (%.0f GB)", low, high, Double(high) / 1_073_741_824.0)
    }

    /// 0xfc0000000 without the extended-VA entitlement, 0x8000000000 with it.
    var isExtended: Bool { known && high > 0xfc0000000 }
}
