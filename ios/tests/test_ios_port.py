"""Structural checks for the iOS port.

The iOS target cannot be compiled on the machine that checks this repository in
(outside a GitHub Actions macOS runner), so these tests pin the pieces that are
easy to break silently and that the build/run story depends on:

  * the app is linked above the PS4 direct window;
  * the JIT layer is the reference protocol (BRK #0xf00d, _M, no-footprint),
    not a lookalike;
  * the Memory+ entitlements and the com.apple.ar.viewer memory host survive;
  * the session hands the pool to FEX and calls the stock loader;
  * the Linux compatibility shims exist for every Linux-only name the stock
    runtime uses.

Run with:  python -m unittest discover -s ios/tests
"""

import os
import plistlib
import re
import unittest

import yaml

IOS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(IOS)


def read(*parts):
    with open(os.path.join(IOS, *parts), encoding="utf-8") as handle:
        return handle.read()


class ProjectTests(unittest.TestCase):
    def setUp(self):
        self.project = yaml.safe_load(read("project.yml"))
        self.targets = self.project["targets"]

    def test_targets_exist(self):
        self.assertIn("SpartanPort", self.targets)
        self.assertIn("SpartanRuntime", self.targets)
        self.assertIn("SpartanPortMemoryHost", self.targets)

    def test_app_keeps_the_default_image_base(self):
        # The guest range starts at 64 GiB; the app image at 0x100000000 does
        # not overlap it, and moving the image would only add risk.
        flags = self.targets["SpartanPort"]["settings"]["base"]["OTHER_LDFLAGS"]
        self.assertNotIn("-image_base", flags)

    def test_runtime_target_uses_the_compat_header(self):
        base = self.targets["SpartanRuntime"]["settings"]["base"]
        # C sources get the Linux-name shims. The C++ FEX glue deliberately does
        # not: a `gettid`/`memfd_create` macro would rewrite names inside FEX's
        # own headers, so it includes spartanport_ios.h instead.
        self.assertIn("sp_compat.h", base["OTHER_CFLAGS"])
        self.assertNotIn("sp_compat.h", base["OTHER_CPLUSPLUSFLAGS"])

    def test_runtime_target_builds_the_stock_loader_and_hle(self):
        sources = self.targets["SpartanRuntime"]["sources"]
        paths = {entry["path"] if isinstance(entry, dict) else entry for entry in sources}
        self.assertIn("../src/runtime_memory.c", paths)
        self.assertIn("../src/runtime_thread.c", paths)
        self.assertIn("Runtime", paths)
        # probe.c is compiled through the wrapper that renames main.
        self.assertNotIn("../src/probe.c", paths)
        self.assertTrue(os.path.exists(os.path.join(IOS, "Runtime", "sp_probe_ios.c")))

    def test_app_links_fexcore_and_extension(self):
        app = self.targets["SpartanPort"]
        flags = app["settings"]["base"]["OTHER_LDFLAGS"]
        for library in ("-lFEXCore", "-lFEXCore_Base", "-lJemallocLibs", "-lxxhash"):
            self.assertIn(library, flags)
        deps = app["dependencies"]
        self.assertTrue(any(d.get("target") == "SpartanRuntime" for d in deps if isinstance(d, dict)))
        embedded = [d for d in deps if isinstance(d, dict) and d.get("target") == "SpartanPortMemoryHost"]
        self.assertTrue(embedded)
        self.assertTrue(embedded[0].get("embed"))

    def test_runtime_sources_listed_in_project_exist(self):
        for entry in self.targets["SpartanRuntime"]["sources"]:
            path = entry["path"] if isinstance(entry, dict) else entry
            if not path.startswith(".."):
                self.assertTrue(os.path.exists(os.path.join(IOS, path)), path)


class PlistTests(unittest.TestCase):
    def test_app_entitlements(self):
        with open(os.path.join(IOS, "SpartanPort", "Resources", "SpartanPort.entitlements"), "rb") as handle:
            entitlements = plistlib.load(handle)
        for key in (
            "com.apple.developer.kernel.increased-memory-limit",
            "com.apple.developer.kernel.extended-virtual-addressing",
            "get-task-allow",
        ):
            self.assertIs(entitlements.get(key), True, key)

    def test_app_queries_stikdebug(self):
        with open(os.path.join(IOS, "SpartanPort", "Resources", "Info.plist"), "rb") as handle:
            info = plistlib.load(handle)
        self.assertIn("stikdebug", info["LSApplicationQueriesSchemes"])
        self.assertTrue(info["UIFileSharingEnabled"])

    def test_memory_host_is_the_viewer_extension(self):
        with open(os.path.join(IOS, "SpartanPortMemoryHost", "Info.plist"), "rb") as handle:
            info = plistlib.load(handle)
        extension = info["NSExtension"]
        self.assertEqual(extension["NSExtensionPointIdentifier"], "com.apple.ar.viewer")
        self.assertEqual(extension["NSExtensionPrincipalClass"], "SpartanPortMemoryHostHandler")
        with open(os.path.join(IOS, "SpartanPortMemoryHost", "MemoryHost.entitlements"), "rb") as handle:
            self.assertIs(plistlib.load(handle).get("get-task-allow"), True)


class JITLayerTests(unittest.TestCase):
    def setUp(self):
        self.allocator = read("SpartanPort", "Native", "JITAllocator.c")
        self.header = read("SpartanPort", "Native", "JITAllocator.h")
        self.script = read("SpartanPort", "Resources", "spartanport-jit.js")

    def test_reference_protocol(self):
        # x16 = 1 prepare (x0 addr, x1 len), x16 = 0 detach.
        self.assertIn("mov x16, #1", self.allocator)
        self.assertIn("mov x16, #0", self.allocator)
        self.assertIn("0xd43e01a0", self.allocator)  # brk #0xf00d encoding
        self.assertIn("sp_pool_prepare", self.allocator)

    def test_no_footprint_ladder(self):
        for marker in ("plain/TASK_NULL", "plain/self", "share/self", "share/TASK_NULL"):
            self.assertIn(marker, self.allocator)
        self.assertIn("VM_LEDGER_FLAG_NO_FOOTPRINT", self.allocator)
        self.assertIn("mach_memory_entry_ownership", self.allocator)

    def test_pool_is_16k_aligned(self):
        self.assertIn("0x4000", self.allocator)
        self.assertIn("JIT_PAGE_MASK", self.allocator)

    def test_debugger_script(self):
        self.assertIn("0xf00d", self.script)
        self.assertIn("_M", self.script)                       # debugger allocates RX
        self.assertIn("prepare_memory_region", self.script)    # authorizes it
        self.assertIn("vAttach", self.script)
        self.assertNotIn("Madeira JIT", self.script)

    def test_pool_policy_matches_the_guest_window(self):
        swift = read("SpartanPort", "Core", "JITPool.swift")
        self.assertIn("0x1_0000_0000", swift)
        self.assertIn("0xfc_0000_0000", swift)
        self.assertIn("SP_JIT_NO_DEBUGGER", swift)


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.compat = read("Runtime", "platform", "sp_compat.h")
        self.session = read("Runtime", "sp_session.c")
        self.mmap = read("Runtime", "platform", "sp_jit_mmap.c")

    def test_linux_names_have_darwin_shims(self):
        for name in ("memfd_create", "fallocate", "gettid"):
            self.assertIn(f"#define {name} sp_{name}", self.compat)
        self.assertIn("sp_memfd_create", self.compat)

    def test_session_hands_the_pool_to_fex(self):
        self.assertIn("sp_jit_pool_set", self.session)
        self.assertIn("spartanport_loader_main", self.session)
        self.assertIn("--cpu-only", self.session)
        # The loader's stdio goes to the app log.
        self.assertIn("dup2", self.session)

    def test_executable_mappings_come_from_the_pool(self):
        self.assertIn("PROT_EXEC", self.mmap)
        self.assertIn("MAP_ANON", self.mmap)
        self.assertIn("pool_alloc_locked", self.mmap)

    def test_cpu_only_is_honest_about_the_gpu(self):
        stub = read("Runtime", "gpu", "sp_gpu_stub.c")
        self.assertIn("not part of the CPU-only bring-up", stub)

    def test_guest_window_contract(self):
        platform = read("Runtime", "platform", "sp_platform.c")
        self.assertIn("SP_PS4_USER_MIN", platform)
        self.assertIn("0x1000000000", platform)
        self.assertIn("sp_check_guest_window", platform)


class SwiftBridgeTests(unittest.TestCase):
    def test_swift_only_calls_declared_c_functions(self):
        headers = read("Runtime", "include", "spartanport_ios.h") + read(
            "SpartanPort", "Native", "JITAllocator.h")
        declared = set(re.findall(r"\b((?:sp|jit|spartanport)_[a-z0-9_]+)\s*\(", headers))
        # Types and enum constants also match the prefix; only calls are checked.
        used = set()
        for name in ("RuntimeController.swift", "GameSession.swift", "JITPool.swift",
                     "EntitlementChecker.swift"):
            source = read("SpartanPort", "Core", name)
            used |= set(re.findall(r"\b((?:sp|jit)_[a-z0-9_]+)\s*\(", source))
        missing = sorted(used - declared)
        missing = [name for name in missing if name not in {"sp_session_config"}]
        self.assertEqual(missing, [], f"Swift calls undeclared C functions: {missing}")

    def test_no_removed_api_is_still_called(self):
        for name in ("RuntimeController.swift", "GameSession.swift", "JITPool.swift"):
            source = read("SpartanPort", "Core", name)
            self.assertNotIn("jit_region_create", source)
            self.assertNotIn("JITMemory", source)


if __name__ == "__main__":
    unittest.main()
