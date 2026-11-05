import Darwin
import Foundation
import Network

/// Picks the address the helper uses to reach this iPhone's `lockdownd`.
///
/// LocalDevVPN is normally needed because `lockdownd` is not reachable at
/// `10.7.0.1` until a packet-tunnel reflects the connection back into the
/// device. Some iOS builds also answer on loopback or on the device's own
/// Wi-Fi/cellular address; try those first so a session can be self-contained
/// where the OS allows it. If none answers, fall back to LocalDevVPN's address
/// and let StikJIT report the usual "connect LocalDevVPN" error.
enum SpartanPortEndpoint {
    static let lockdownPort: UInt16 = 62078
    static let localDevVPNAddress = "10.7.0.1"

    struct Choice {
        let host: String
        let note: String
    }

    static func choose() -> Choice {
        var candidates: [(String, String)] = [("127.0.0.1", "loopback")]
        for address in interfaceAddresses() where address != "127.0.0.1" {
            candidates.append((address, "device address \(address)"))
        }
        candidates.append((localDevVPNAddress, "LocalDevVPN"))

        for (host, note) in candidates {
            // 10.7.0.1 is the one that goes through the tunnel; give it a
            // little longer than the direct addresses.
            let timeout: TimeInterval = host == localDevVPNAddress ? 0.75 : 0.4
            if canConnect(host: host, port: lockdownPort, timeout: timeout) {
                NSLog("[SpartanPortJIT] lockdown reachable at %@ (%@)", host, note)
                return Choice(host: host, note: note)
            }
        }
        NSLog("[SpartanPortJIT] no lockdown endpoint answered; using LocalDevVPN address")
        return Choice(host: localDevVPNAddress,
                      note: "no direct endpoint answered; connect LocalDevVPN")
    }

    /// The device's own IPv4 interface addresses (Wi-Fi, cellular, ...),
    /// ignoring the loopback and the VPN address itself.
    static func interfaceAddresses() -> [String] {
        var head: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&head) == 0, let first = head else { return [] }
        defer { freeifaddrs(head) }

        var result: [String] = []
        var cursor: UnsafeMutablePointer<ifaddrs>? = first
        while let entry = cursor {
            defer { cursor = entry.pointee.ifa_next }
            let flags = Int32(entry.pointee.ifa_flags)
            guard flags & IFF_UP != 0, flags & IFF_LOOPBACK == 0,
                  let address = entry.pointee.ifa_addr,
                  address.pointee.sa_family == UInt8(AF_INET) else { continue }

            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            let resultCode = getnameinfo(address, socklen_t(address.pointee.sa_len),
                                         &host, socklen_t(host.count), nil, 0,
                                         NI_NUMERICHOST)
            guard resultCode == 0 else { continue }
            let text = String(cString: host)
            if text != localDevVPNAddress && !result.contains(text) {
                result.append(text)
            }
        }
        return result
    }

    static func canConnect(host: String, port: UInt16, timeout: TimeInterval) -> Bool {
        guard let nwPort = NWEndpoint.Port(rawValue: port) else { return false }
        let connection = NWConnection(host: NWEndpoint.Host(host), port: nwPort, using: .tcp)
        let done = DispatchSemaphore(value: 0)
        var reachable = false

        connection.stateUpdateHandler = { state in
            switch state {
            case .ready:
                reachable = true
                done.signal()
            case .failed, .cancelled:
                done.signal()
            default:
                break
            }
        }
        connection.start(queue: DispatchQueue.global(qos: .utility))
        let waitResult = done.wait(timeout: .now() + timeout)
        connection.cancel()
        return waitResult == .success && reachable
    }
}
