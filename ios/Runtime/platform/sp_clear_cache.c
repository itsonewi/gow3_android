/*
 * clang lowers __builtin___clear_cache() to a call to ___clear_cache, which
 * libgcc/compiler-rt provide on Linux but iOS's libSystem does not. FEXCore's
 * CodeCache uses the builtin when it writes generated code; route it to
 * Darwin's instruction-cache maintenance call.
 */

#include <libkern/OSCacheControl.h>
#include <stddef.h>

void __clear_cache(void *begin, void *end) {
    if (!begin || !end || end <= begin) return;
    sys_icache_invalidate(begin, (size_t)((char *)end - (char *)begin));
}
