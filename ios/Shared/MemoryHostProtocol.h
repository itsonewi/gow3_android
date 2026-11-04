// Memory+ donation protocol, shared by SpartanPort and SpartanPortMemoryHost.
//
// iOS 27's jetsam table gives the com.apple.ar.viewer extension point no memory
// limit (LiveContainer runs guest apps in such an extension). This is the same
// experiment Madeira runs in MadeiraMemoryHost: the extension creates memory
// objects (plain, ledger-tagged, purgeable, or a purgeable allocation donated
// through a memory entry) and hands them to the app, then both processes'
// phys_footprint are compared to see whose budget paid for the pages.
//
// The app uses this only with env SPARTAN_MEMORY_HOST=1; the default path backs
// the guest's direct memory with a sparse file and marks the JIT pool
// no-footprint instead.

#import <Foundation/Foundation.h>
#import <mach/mach.h>
#import <xpc/xpc.h>

// <mach/mach_vm.h> is "unsupported" on the iOS SDK, but libsystem_kernel
// exports these (JITAllocator.c declares its own the same way).
extern kern_return_t mach_vm_map(vm_map_t target, mach_vm_address_t *address, mach_vm_size_t size,
                                 mach_vm_offset_t mask, int flags, mem_entry_name_port_t object,
                                 memory_object_offset_t offset, boolean_t copy, vm_prot_t cur_protection,
                                 vm_prot_t max_protection, vm_inherit_t inheritance);
extern kern_return_t mach_vm_deallocate(vm_map_t target, mach_vm_address_t address, mach_vm_size_t size);
extern kern_return_t mach_vm_allocate(vm_map_t target, mach_vm_address_t *address, mach_vm_size_t size, int flags);

typedef NS_ENUM(int, MHRegionKind) {
    MHRegionPlain = 0,          // MAP_MEM_NAMED_CREATE: an object with no owner
    MHRegionLedgerTagged = 1,   // + MAP_MEM_LEDGER_TAGGED: owned by the extension
    MHRegionPurgeable = 2,      // + MAP_MEM_PURGABLE (non-volatile): owned by the extension
    MHRegionPurgeableAlloc = 3, // mach_vm_allocate(VM_FLAGS_PURGABLE) + a memory entry on it
};

@protocol MHMemoryServer
/// The extension's pid, phys_footprint and os_proc_available_memory().
- (void)statusWithReply:(void (^)(int pid, uint64_t footprint, uint64_t available))reply;
/// Creates a region of `bytes`; with `touch`, the extension maps it and writes
/// every page first (and keeps it mapped). The reply's XPC dictionary holds the
/// memory entry's send right under "entry".
- (void)createRegion:(uint64_t)bytes kind:(int)kind touch:(BOOL)touch
               reply:(void (^)(xpc_object_t handle, int kr, uint64_t footprint, uint64_t available))reply;
/// Replies, then exits the extension process shortly after.
- (void)exitWithReply:(void (^)(void))reply;
@end

/// The app's side of the connection. The extension calls this first: an
/// NSXPCConnection made from an endpoint reaches the listener only with its
/// first message, so without it the app never sees the connection.
@protocol MHMemoryClient
- (void)extensionReady:(int)pid available:(uint64_t)available;
@end

static inline NSXPCInterface *MHMemoryServerInterface(void) {
    NSXPCInterface *interface = [NSXPCInterface interfaceWithProtocol:@protocol(MHMemoryServer)];
    [interface setXPCType:XPC_TYPE_DICTIONARY forSelector:@selector(createRegion:kind:touch:reply:)
                 argumentIndex:0 ofReply:YES];
    return interface;
}
