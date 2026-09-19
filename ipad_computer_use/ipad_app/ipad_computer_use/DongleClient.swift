import Foundation

enum ConnectionDiagnostics {
    static func describe(_ error: Error, task: URLSessionWebSocketTask, endpoint: URL? = nil) -> String {
        let failure = error as NSError
        var lines = [
            "URL: \((endpoint ?? task.originalRequest?.url ?? task.currentRequest?.url)?.absoluteString ?? "unavailable")",
            "Error domain: \(failure.domain)",
            "Error code: \(failure.code)",
            "Description: \(failure.localizedDescription)",
            "Error details: \(String(reflecting: failure.userInfo))",
            "WebSocket close code: \(task.closeCode.rawValue)",
            "WebSocket close reason: \(task.closeReason.map { String(decoding: $0, as: UTF8.self) } ?? "not supplied")"
        ]
        if let response = task.response as? HTTPURLResponse {
            lines.append("HTTP status: \(response.statusCode)")
            lines.append("HTTP headers: \(String(reflecting: response.allHeaderFields))")
        } else {
            lines.append("HTTP response: not provided by URLSession")
        }
        return lines.joined(separator: "\n")
    }
}

struct RelayError: LocalizedError, CustomNSError {
    let message: String
    var errorDescription: String? { message }
    static var errorDomain: String { "iPadComputerUse" }
    var errorCode: Int { 1 }
    var errorUserInfo: [String: Any] { [NSLocalizedDescriptionKey: message] }
}

struct DongleStatus: Decodable {
    let device: String?
    let running: Bool
    let state: String
    let hidReady: Bool
    /// "Wi-Fi + BLE" on the ESP32-PICO-KIT firmware; absent on the USB XIAO RP2040 firmware.
    let transport: String?
    /// Firmware guidance while the tool cannot accept input (pair over Bluetooth, set the secret).
    let hint: String?
    let secretConfigured: Bool?
}

actor DongleClient {
    /// USB NCM address of the XIAO RP2040 input tool. Wi-Fi tools (ESP32) use a saved address.
    static let defaultAddress = "http://172.31.254.1"
    static let defaultBaseURL = URL(string: defaultAddress)!

    nonisolated let baseURL: URL
    private let session: URLSession

    /// Parses a saved input-tool address: empty means the USB default, otherwise http(s)://host[:port].
    static func baseURL(from address: String?) throws -> URL {
        let trimmed = (address ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty { return defaultBaseURL }
        guard let components = URLComponents(string: trimmed), let scheme = components.scheme?.lowercased(),
              ["http", "https"].contains(scheme), let host = components.host, !host.isEmpty,
              components.user == nil, components.password == nil, components.query == nil,
              components.fragment == nil, components.path.isEmpty || components.path == "/",
              let url = components.url else {
            throw RelayError(message: "Enter the input tool address as http://host or http://host:port")
        }
        return url
    }

    /// A client for the saved address; an unusable value falls back to the USB default.
    static func configured(_ address: String?) -> DongleClient {
        DongleClient(baseURL: (try? baseURL(from: address)) ?? defaultBaseURL)
    }

    nonisolated var host: String { baseURL.host(percentEncoded: false) ?? baseURL.absoluteString }

    init(baseURL: URL = DongleClient.defaultBaseURL) {
        self.baseURL = baseURL
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 3
        config.timeoutIntervalForResource = 5
        config.requestCachePolicy = .reloadIgnoringLocalCacheData
        session = URLSession(configuration: config)
    }

    private func request(_ path: String, secret: String? = nil, body: String? = nil) async throws -> Data {
        var request = URLRequest(url: baseURL.appendingPathComponent(path))
        if let secret {
            request.httpMethod = "POST"
            request.setValue(secret, forHTTPHeaderField: "X-Input-Device-Secret")
            request.setValue("text/plain", forHTTPHeaderField: "Content-Type")
            request.httpBody = (body ?? "").data(using: .utf8)
        }
        let (data, response) = try await session.data(for: request)
        guard let response = response as? HTTPURLResponse, (200..<300).contains(response.statusCode) else {
            throw RelayError(message: "Input tool rejected the request: \(String(data: data, encoding: .utf8) ?? "unknown error")")
        }
        return data
    }

    func status() async throws -> DongleStatus {
        try JSONDecoder().decode(DongleStatus.self, from: await request("status"))
    }

    func run(hex: String, secret: String) async throws {
        guard !hex.isEmpty, hex.count <= 5120, hex.count.isMultiple(of: 10),
              hex.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }) else {
            throw RelayError(message: "Invalid input records")
        }
        _ = try await request("input", secret: secret, body: hex)
    }

    func stop(secret: String) async throws { _ = try await request("stop", secret: secret) }
}
