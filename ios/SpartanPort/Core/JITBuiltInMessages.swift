import Foundation

/// The JSON messages between the app and its JIT helper extension.
///
/// The helper runs in a separate process because a process cannot synchronously
/// debug itself: it uses the bundled StikJIT framework to attach to this app,
/// prepare the Developer Disk Image, and run `spartanport-jit.js`.
struct SpartanPortJITRequest: Codable, Sendable {
    static let itemKey = "spartanport-jit-request"

    enum Operation: String, Codable, Sendable {
        case prepare
        case enable
        case resetDDI
    }

    let operation: Operation
    let targetPID: Int32?
    let pairingData: Data?
    let scriptBase64: String?

    static func prepare(pairingData: Data) -> SpartanPortJITRequest {
        SpartanPortJITRequest(operation: .prepare, targetPID: nil,
                              pairingData: pairingData, scriptBase64: nil)
    }

    static func enable(targetPID: Int32, pairingData: Data,
                       scriptBase64: String) -> SpartanPortJITRequest {
        SpartanPortJITRequest(operation: .enable, targetPID: targetPID,
                              pairingData: pairingData, scriptBase64: scriptBase64)
    }

    static var resetDDI: SpartanPortJITRequest {
        SpartanPortJITRequest(operation: .resetDDI, targetPID: nil,
                              pairingData: nil, scriptBase64: nil)
    }

    struct Response: Codable, Sendable {
        static let itemKey = "spartanport-jit-response"
        let success: Bool
        let message: String
        let txmPresent: Bool?
    }
}
