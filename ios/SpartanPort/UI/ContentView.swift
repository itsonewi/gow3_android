import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @EnvironmentObject private var runtime: RuntimeController
    @EnvironmentObject private var library: GameLibrary
    @ObservedObject private var log = LogStore.shared
    @Environment(\.scenePhase) private var scenePhase

    @State private var showFolderPicker = false
    @State private var showLog = true

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    header
                    capabilityGrid
                    jitCard
                    memoryCard
                    gameCard
                    runtimeCard
                    logCard
                }
                .padding(.horizontal, 18)
                .padding(.vertical, 20)
            }
            .background(background)
            .toolbar(.hidden, for: .navigationBar)
            .fileImporter(isPresented: $showFolderPicker,
                          allowedContentTypes: [.folder],
                          allowsMultipleSelection: false) { result in
                switch result {
                case .success(let urls):
                    if let url = urls.first {
                        library.setFolder(url)
                    }
                case .failure(let error):
                    LogStore.shared.log("[library] folder picker failed: \(error.localizedDescription)",
                                        level: .error)
                }
            }
            .onChange(of: scenePhase) { _, phase in
                if phase == .active {
                    runtime.refresh()
                }
            }
        }
    }

    // MARK: - Header

    private var header: some View {
        HStack(spacing: 14) {
            ZStack {
                RoundedRectangle(cornerRadius: 16, style: .continuous)
                    .fill(LinearGradient(colors: [.red.opacity(0.9), .orange.opacity(0.7)],
                                         startPoint: .topLeading, endPoint: .bottomTrailing))
                Text("SP")
                    .font(.system(size: 26, weight: .black, design: .rounded))
                    .foregroundStyle(.white)
            }
            .frame(width: 58, height: 58)
            .overlay(
                RoundedRectangle(cornerRadius: 16, style: .continuous)
                    .stroke(.white.opacity(0.16), lineWidth: 1)
            )

            VStack(alignment: .leading, spacing: 3) {
                Text("SPARTANPORT")
                    .font(.system(size: 22, weight: .heavy, design: .rounded))
                    .tracking(1.2)
                Text("God of War III · iOS runtime")
                    .font(.system(size: 13, weight: .medium))
                    .foregroundStyle(.secondary)
            }
            Spacer()
            Button {
                runtime.refresh()
            } label: {
                Image(systemName: "arrow.clockwise")
                    .font(.system(size: 15, weight: .semibold))
                    .padding(10)
                    .background(.white.opacity(0.08), in: Circle())
            }
            .buttonStyle(.plain)
        }
    }

    // MARK: - Capability cards

    private var capabilityGrid: some View {
        VStack(spacing: 10) {
            capability(
                title: "JIT",
                ok: runtime.debuggerAttached || runtime.jitState == .ready,
                icon: "bolt.fill",
                detail: runtime.jitState == .ready
                    ? "Pool allocated · debugger detached"
                    : (runtime.debuggerAttached ? "Debugger attached · CS_DEBUGGED set"
                                                : "StikDebug attaches on demand")
            )
            capability(
                title: "Memory+",
                ok: runtime.entitlements.increasedMemory,
                icon: "memorychip.fill",
                detail: runtime.entitlements.increasedMemory
                    ? "Increased Memory Limit entitlement granted"
                    : "Entitlement missing from this signature"
            )
            capability(
                title: "Extended virtual addressing",
                ok: runtime.addressMap.isExtended,
                icon: "square.stack.3d.up.fill",
                detail: runtime.addressMap.description
            )
        }
    }

    private func capability(title: String, ok: Bool, icon: String, detail: String) -> some View {
        HStack(spacing: 12) {
            Image(systemName: icon)
                .font(.system(size: 16, weight: .semibold))
                .foregroundStyle(ok ? .green : .orange)
                .frame(width: 34, height: 34)
                .background((ok ? Color.green : Color.orange).opacity(0.12), in: RoundedRectangle(cornerRadius: 10))

            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.system(size: 14, weight: .semibold))
                Text(detail)
                    .font(.system(size: 11.5))
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
            }
            Spacer()
            Image(systemName: ok ? "checkmark.circle.fill" : "exclamationmark.circle.fill")
                .foregroundStyle(ok ? .green : .orange)
        }
        .padding(12)
        .background(.white.opacity(0.055), in: RoundedRectangle(cornerRadius: 14, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .stroke(.white.opacity(0.06), lineWidth: 1)
        )
    }

    // MARK: - JIT controls

    private var jitCard: some View {
        card(title: "JIT layer", subtitle: "Madeira-compatible StikDebug / BRK #0xf00d") {
            HStack(spacing: 10) {
                Button {
                    runtime.enableJIT()
                } label: {
                    Label(jitButtonTitle, systemImage: "bolt.fill")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(PrimaryButtonStyle())
                .disabled(runtime.busy || runtime.jitState == .enabling)

                Button {
                    runtime.preparePool()
                } label: {
                    Label(runtime.busy ? "Working" : "Pool", systemImage: "memorychip")
                }
                .buttonStyle(SecondaryButtonStyle())
                .disabled(runtime.busy || runtime.pool != nil)
            }

            HStack {
                Text("Pool size")
                    .font(.system(size: 12.5, weight: .medium))
                    .foregroundStyle(.secondary)
                Spacer()
                Picker("Pool size", selection: $runtime.poolSizeMB) {
                    Text("128 MB").tag(128)
                    Text("256 MB").tag(256)
                    Text("512 MB").tag(512)
                    Text("896 MB").tag(896)
                    Text("1024 MB").tag(1024)
                }
                .pickerStyle(.menu)
                .tint(.orange)
                .disabled(runtime.pool != nil)
            }

            if let pool = runtime.pool {
                VStack(alignment: .leading, spacing: 4) {
                    row("RX (exec)", pool.rxDescription)
                    row("RW (write)", pool.rwDescription)
                    row("Size", pool.sizeDescription)
                }
                .padding(10)
                .background(.black.opacity(0.25), in: RoundedRectangle(cornerRadius: 10))
            }

            if case .failed(let message) = runtime.jitState {
                Text(message)
                    .font(.system(size: 11.5))
                    .foregroundStyle(.red.opacity(0.9))
                    .fixedSize(horizontal: false, vertical: true)
            }

            HStack(spacing: 14) {
                tinyStatus("CS_DEBUGGED", runtime.signing.debugged)
                tinyStatus("Debugger", runtime.debuggerAttached)
                tinyStatus("StikDebug", runtime.stikDebugInstalled)
            }

            Button("Run mapping probe") {
                runtime.testMapping()
            }
            .font(.system(size: 12, weight: .medium))
            .foregroundStyle(.orange)
            .disabled(runtime.pool != nil)
        }
    }

    private var jitButtonTitle: String {
        switch runtime.jitState {
        case .idle: return "Enable JIT"
        case .enabling: return "Enabling…"
        case .attached: return "Prepare pool"
        case .ready: return "JIT ready"
        case .failed: return "Retry JIT"
        }
    }

    // MARK: - Memory card

    private var memoryCard: some View {
        card(title: "Memory layer", subtitle: "Memory+ entitlements and live limits") {
            row("Physical RAM", "\(ProcessInfo.processInfo.physicalMemory >> 20) MB")
            row("Available to app", "\(runtime.availableMemory >> 20) MB")
            row("Address map", runtime.addressMap.description)
            row("Increased memory", runtime.entitlements.increasedMemory ? "granted" : "not granted")
            row("Extended VA", runtime.entitlements.extendedVA ? "granted" : "not granted")
            row("get-task-allow", runtime.signing.debuggable ? "granted" : "not granted")
            row("allow-jit", runtime.entitlements.jitAllowed ? "granted" : "not granted (iOS ignores it)")

            Text("Memory+ cannot be enabled by a switch. The signing certificate or sideloading tool must grant "
                 + "com.apple.developer.kernel.increased-memory-limit and "
                 + "com.apple.developer.kernel.extended-virtual-addressing.")
                .font(.system(size: 11))
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    // MARK: - Game folder

    private var gameCard: some View {
        card(title: "Game", subtitle: "Folder with eboot.bin, sce_module and sce_sys") {
            HStack(spacing: 12) {
                Image(systemName: library.hasGame ? "checkmark.seal.fill" : "folder.badge.plus")
                    .font(.system(size: 20, weight: .semibold))
                    .foregroundStyle(library.hasGame ? .green : .orange)
                    .frame(width: 40, height: 40)
                    .background(.white.opacity(0.06), in: RoundedRectangle(cornerRadius: 11))

                VStack(alignment: .leading, spacing: 3) {
                    Text(library.displayName)
                        .font(.system(size: 13.5, weight: .semibold))
                        .lineLimit(1)
                    Text(library.hasGame ? "eboot.bin found" : "No valid game folder imported")
                        .font(.system(size: 11.5))
                        .foregroundStyle(.secondary)
                }
                Spacer()
            }

            HStack(spacing: 10) {
                Button {
                    showFolderPicker = true
                } label: {
                    Label("Import folder", systemImage: "folder")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(SecondaryButtonStyle())

                if library.folder != nil {
                    Button("Clear") { library.clear() }
                        .buttonStyle(SecondaryButtonStyle())
                }
            }

            if let error = library.importError {
                Text(error)
                    .font(.system(size: 11.5))
                    .foregroundStyle(.red.opacity(0.9))
            }
        }
    }

    // MARK: - Runtime roadmap

    private var runtimeCard: some View {
        card(title: "Runtime", subtitle: "Bringing the native ARM64 runtime to iOS") {
            stage("1", "iOS host + JIT + Memory+", done: true,
                  detail: "This build: SwiftUI shell, StikDebug JIT, entitlements.")
            stage("2", "FEXCore iOS static libraries", done: false,
                  detail: "willfaust/FEX ios-port-2607, linked from CI.")
            stage("3", "Memory map + ELF/PS4 loader", done: false,
                  detail: "0x0800000000–0x0f00000000 guest window via mach_vm_map.")
            stage("4", "GPU: shadPS4 → Vulkan → MoltenVK", done: false,
                  detail: "Metal surface, MoltenVK, SDL3 window on UIKit.")
            stage("5", "Audio/video + launcher", done: false,
                  detail: "FFmpeg, Atrac9, save data and controller input.")

            Text("The Android branch's runtime is Linux/glibc ELF and cannot run on iOS as-is. "
                 + "The code is being ported piece by piece; this screen reports the real state of each layer.")
                .font(.system(size: 11))
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private func stage(_ number: String, _ title: String, done: Bool, detail: String) -> some View {
        HStack(alignment: .top, spacing: 10) {
            ZStack {
                Circle()
                    .fill(done ? Color.green.opacity(0.16) : Color.white.opacity(0.06))
                if done {
                    Image(systemName: "checkmark")
                        .font(.system(size: 10, weight: .bold))
                        .foregroundStyle(.green)
                } else {
                    Text(number)
                        .font(.system(size: 10, weight: .bold))
                        .foregroundStyle(.secondary)
                }
            }
            .frame(width: 22, height: 22)

            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.system(size: 12.5, weight: .semibold))
                    .foregroundStyle(done ? .primary : .secondary)
                Text(detail)
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary.opacity(0.9))
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
        }
    }

    // MARK: - Log

    private var logCard: some View {
        card(title: "Log", subtitle: "Also written to Console.app") {
            HStack {
                Text("\(log.entries.count) lines")
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
                Spacer()
                Button(showLog ? "Hide" : "Show") { showLog.toggle() }
                    .font(.system(size: 11, weight: .medium))
                    .foregroundStyle(.orange)
                Button("Clear") { LogStore.shared.clear() }
                    .font(.system(size: 11, weight: .medium))
                    .foregroundStyle(.orange)
            }

            if showLog {
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 3) {
                        ForEach(log.entries.suffix(120)) { entry in
                            Text(log.text(for: entry))
                                .font(.system(size: 9.5, design: .monospaced))
                                .foregroundStyle(color(for: entry.level))
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                    }
                }
                .frame(maxHeight: 210)
                .padding(9)
                .background(.black.opacity(0.35), in: RoundedRectangle(cornerRadius: 10))
            }
        }
    }

    // MARK: - Pieces

    private func card<Content: View>(title: String, subtitle: String,
                                     @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 11) {
            VStack(alignment: .leading, spacing: 1) {
                Text(title)
                    .font(.system(size: 15, weight: .bold))
                Text(subtitle)
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
            }
            content()
        }
        .padding(14)
        .background(.white.opacity(0.045), in: RoundedRectangle(cornerRadius: 18, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 18, style: .continuous)
                .stroke(.white.opacity(0.06), lineWidth: 1)
        )
    }

    private func row(_ key: String, _ value: String) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(key)
                .font(.system(size: 11.5))
                .foregroundStyle(.secondary)
            Spacer()
            Text(value)
                .font(.system(size: 11.5, weight: .medium, design: .monospaced))
                .multilineTextAlignment(.trailing)
        }
    }

    private func tinyStatus(_ label: String, _ ok: Bool) -> some View {
        HStack(spacing: 5) {
            Circle()
                .fill(ok ? Color.green : Color.orange.opacity(0.8))
                .frame(width: 6, height: 6)
            Text(label)
                .font(.system(size: 10.5, weight: .medium))
                .foregroundStyle(.secondary)
        }
    }

    private func color(for level: LogLevel) -> Color {
        switch level {
        case .info: return .white.opacity(0.75)
        case .success: return .green.opacity(0.9)
        case .warning: return .orange.opacity(0.9)
        case .error: return .red.opacity(0.9)
        }
    }

    private var background: some View {
        LinearGradient(
            colors: [Color(red: 0.06, green: 0.07, blue: 0.09),
                     Color(red: 0.10, green: 0.06, blue: 0.05),
                     Color(red: 0.04, green: 0.04, blue: 0.06)],
            startPoint: .top,
            endPoint: .bottom
        )
        .ignoresSafeArea()
    }
}

// MARK: - Button styles

struct PrimaryButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 13, weight: .semibold))
            .foregroundStyle(.white)
            .padding(.vertical, 11)
            .padding(.horizontal, 14)
            .background(
                LinearGradient(colors: [.orange, .red.opacity(0.85)],
                               startPoint: .topLeading, endPoint: .bottomTrailing),
                in: RoundedRectangle(cornerRadius: 11, style: .continuous)
            )
            .opacity(configuration.isPressed ? 0.75 : 1)
    }
}

struct SecondaryButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 13, weight: .semibold))
            .foregroundStyle(.primary)
            .padding(.vertical, 11)
            .padding(.horizontal, 14)
            .background(.white.opacity(0.08), in: RoundedRectangle(cornerRadius: 11, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: 11, style: .continuous)
                    .stroke(.white.opacity(0.08), lineWidth: 1)
            )
            .opacity(configuration.isPressed ? 0.65 : 1)
    }
}
