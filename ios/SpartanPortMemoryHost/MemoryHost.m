// SpartanPortMemoryHost: the Memory+ donation experiment.
//
// iOS 27's jetsam table gives the com.apple.ar.viewer extension point no memory
// limit. This extension is started by SpartanPort through NSExtension; it opens
// an XPC connection back to the app and creates memory objects on request. Both
// processes' phys_footprint and os_proc_available_memory() are reported so the
// app can see whose budget paid for the pages (the same experiment as Madeira's
// MadeiraMemoryHost, implemented independently here).
//
// The interest is whether objects created in a process without a jetsam limit,
// then mapped by the app, are charged to the app at all. If they are not, the
// guest's 5 GiB direct pool could be donated instead of charged to SpartanPort.

#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <mach/mach.h>
#import <os/proc.h>
#import <stdlib.h>
#import <string.h>

#import "../Shared/MemoryHostProtocol.h"

static uint64_t host_footprint(void) {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return 0;
    return info.phys_footprint;
}

@interface SpartanMemoryServer : NSObject <MHMemoryServer>
@end

@implementation SpartanMemoryServer {
    // Every entry is kept alive for the lifetime of the extension, otherwise the
    // memory object would be reclaimed as soon as the reply is sent.
    NSMutableArray<NSNumber *> *_ports;
}

- (instancetype)init {
    if ((self = [super init])) _ports = [NSMutableArray array];
    return self;
}

- (void)statusWithReply:(void (^)(int, uint64_t, uint64_t))reply {
    reply(getpid(), host_footprint(), os_proc_available_memory());
}

- (void)createRegion:(uint64_t)bytes kind:(int)kind touch:(BOOL)touch
               reply:(void (^)(xpc_object_t, int, uint64_t, uint64_t))reply {
    memory_object_size_t size = bytes;
    mach_port_t entry = MACH_PORT_NULL;
    kern_return_t kr = KERN_FAILURE;

    if (kind == MHRegionPurgeableAlloc) {
        // Allocate purgeable pages here, then name them with a memory entry.
        // The entry survives this process and keeps the pages.
        mach_vm_address_t range = 0;
        kr = mach_vm_allocate(mach_task_self(), &range, size, VM_FLAGS_ANYWHERE | VM_FLAGS_PURGABLE);
        if (kr == KERN_SUCCESS) {
            kr = mach_make_memory_entry_64(mach_task_self(), &size, range, VM_PROT_READ | VM_PROT_WRITE,
                                           &entry, MACH_PORT_NULL);
        }
    } else {
        vm_prot_t options = VM_PROT_READ | VM_PROT_WRITE | MAP_MEM_NAMED_CREATE;
        if (kind == MHRegionLedgerTagged) options |= MAP_MEM_LEDGER_TAGGED;
        if (kind == MHRegionPurgeable) options |= MAP_MEM_PURGABLE;
        kr = mach_make_memory_entry_64(mach_task_self(), &size, 0, options, &entry, MACH_PORT_NULL);
    }

    if (kr == KERN_SUCCESS && touch) {
        // Write every page first, so the pages are resident in this process
        // (and its footprint is the one that pays for them).
        mach_vm_address_t mapped = 0;
        kr = mach_vm_map(mach_task_self(), &mapped, size, 0, VM_FLAGS_ANYWHERE, entry, 0, FALSE,
                         VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
        if (kr == KERN_SUCCESS) memset((void *)mapped, 0x5a, (size_t)size);
    }

    xpc_object_t replyObject = xpc_dictionary_create(NULL, NULL, 0);
    if (kr == KERN_SUCCESS) {
        xpc_dictionary_set_mach_send(replyObject, "entry", entry);
        [_ports addObject:@(entry)];
    }
    NSLog(@"[memoryhost] %llu MB kind=%d touch=%d kr=%d footprint=%llu MB", bytes >> 20, kind, touch, kr,
          host_footprint() >> 20);
    reply(replyObject, kr, host_footprint(), os_proc_available_memory());
}

- (void)exitWithReply:(void (^)(void))reply {
    reply();
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        exit(0);
    });
}

@end

@interface SpartanPortMemoryHostHandler : NSObject <NSExtensionRequestHandling>
@end

static NSXPCConnection *gConnection;
static SpartanMemoryServer *gServer;

@implementation SpartanPortMemoryHostHandler

- (void)beginRequestWithExtensionContext:(NSExtensionContext *)context {
    NSXPCListenerEndpoint *endpoint = [context.inputItems.firstObject userInfo][@"endpoint"];
    if (![endpoint isKindOfClass:NSXPCListenerEndpoint.class]) {
        NSLog(@"[memoryhost] request has no endpoint: %@", [context.inputItems.firstObject userInfo]);
        return;
    }
    gServer = [SpartanMemoryServer new];
    gConnection = [[NSXPCConnection alloc] initWithListenerEndpoint:endpoint];
    gConnection.exportedInterface = MHMemoryServerInterface();
    gConnection.exportedObject = gServer;
    gConnection.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(MHMemoryClient)];
    [gConnection resume];
    [(id<MHMemoryClient>)gConnection.remoteObjectProxy extensionReady:getpid()
                                                          available:os_proc_available_memory()];
    NSLog(@"[memoryhost] connected, pid %d, available %zu MB", getpid(), os_proc_available_memory() >> 20);
}

@end

// NSExtensionRequestHandling decodes the input items with only property-list
// classes allowed, and an NSXPCListenerEndpoint is not one. LiveContainer lifts
// the check the same way for LiveProcess; do it before the request is decoded.
static BOOL allow_any_class(id self, SEL _cmd, Class cls, id key, BOOL allowing) { return YES; }

__attribute__((constructor)) static void allow_endpoint_decoding(void) {
    Method method = class_getInstanceMethod(NSClassFromString(@"NSXPCDecoder"),
                                            NSSelectorFromString(@"_validateAllowedClass:forKey:allowingInvocations:"));
    if (method) method_setImplementation(method, (IMP)allow_any_class);
}
