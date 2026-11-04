import SwiftUI

@main
struct SpartanPortApp: App {
    @StateObject private var runtime = RuntimeController()
    @StateObject private var library = GameLibrary()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(runtime)
                .environmentObject(library)
                .preferredColorScheme(.dark)
        }
    }
}
