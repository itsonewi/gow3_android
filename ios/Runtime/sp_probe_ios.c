/*
 * The stock loader, built for iOS.
 *
 * src/probe.c defines main(); iOS apps have their own entry point (the SwiftUI
 * @main), so the session compiles the loader under a different symbol and calls
 * it on a worker thread. A separate translation unit keeps the rename out of
 * the shared source and out of every other platform's build.
 */
#include "sp_compat.h"
#define main spartanport_loader_main
#include "../../src/probe.c"
