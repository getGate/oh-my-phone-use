import Foundation

final class StatusProtocol: URLProtocol {
    static var code = 200
    static var body = ""
    static var request: URLRequest?
    static var hold = false
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading() {
        Self.request = request
        if Self.hold { return }
        client?.urlProtocol(self, didReceive: HTTPURLResponse(url: request.url!, statusCode: Self.code,
                            httpVersion: "HTTP/1.1", headerFields: nil)!, cacheStoragePolicy: .notAllowed)
        client?.urlProtocol(self, didLoad: Data(Self.body.utf8))
        client?.urlProtocolDidFinishLoading(self)
    }
    override func stopLoading() {}
}

@main struct SetupStatusHarness {
    @MainActor static func main() async {
        let failedSocket = URLSession.shared.webSocketTask(with: URL(string: "wss://control.example/native")!)
        let diagnostic = ConnectionDiagnostics.describe(NSError(domain: NSURLErrorDomain, code: -1011,
            userInfo: [NSLocalizedDescriptionKey: "Bad response", "detail": "handshake rejected"]), task: failedSocket,
            endpoint: URL(string: "wss://control.example/native"))
        precondition(diagnostic.contains("wss://control.example/native"))
        precondition(diagnostic.contains("-1011") && diagnostic.contains("handshake rejected"))
        precondition(diagnostic.contains("HTTP response: not provided by URLSession"))
        failedSocket.cancel(with: .goingAway, reason: nil)
        for address in ["", "old-invalid-address", "wss://control.example/device"] {
            let stored = try! JSONSerialization.data(withJSONObject: [
                "address": address, "sessionID": "stale-session", "deviceID": "saved-device"
            ])
            let revoked = try! SharedConfiguration.revokingSession(in: stored)
            let value = try! JSONSerialization.jsonObject(with: revoked) as! [String: String]
            precondition(value["sessionID"] == nil)
            precondition(value["address"] == address && value["deviceID"] == "saved-device")
            let twice = try! SharedConfiguration.revokingSession(in: revoked)
            precondition((try! JSONSerialization.jsonObject(with: twice) as! [String: String]) == value)
        }
        let tool = try! JSONDecoder().decode(DongleStatus.self, from: Data(
            "{\"device\":\"XIAO Input Tool\",\"running\":false,\"state\":\"idle\",\"hidReady\":true}".utf8))
        precondition(tool.device == "XIAO Input Tool" && tool.hidReady)
        let unidentified = try! JSONDecoder().decode(DongleStatus.self, from: Data(
            "{\"running\":false,\"state\":\"idle\",\"hidReady\":true}".utf8))
        precondition(unidentified.device == nil)
        for name in ["XIAO Net Echo", "XIAO Input Tool", "Future display name"] {
            let bytes = try! JSONSerialization.data(withJSONObject: [
                "device": name, "running": false, "state": "idle", "hidReady": true
            ])
            let status = try! JSONDecoder().decode(DongleStatus.self, from: bytes)
            precondition(status.hidReady)
        }
        precondition((try? JSONDecoder().decode(DongleStatus.self,
            from: Data("{\"device\":\"XIAO Input Tool\"}".utf8))) == nil)
        // Input tool address: USB XIAO default versus a Wi-Fi ESP32-PICO-KIT address.
        precondition(try! DongleClient.baseURL(from: nil).absoluteString == "http://172.31.254.1")
        precondition(try! DongleClient.baseURL(from: "  ").absoluteString == "http://172.31.254.1")
        precondition(try! DongleClient.baseURL(from: "http://phoneuse.local").absoluteString == "http://phoneuse.local")
        precondition(try! DongleClient.baseURL(from: " HTTP://192.168.1.50:8080/ ").host(percentEncoded: false) == "192.168.1.50")
        for bad in ["phoneuse.local", "ws://host/device", "http://user:pw@host", "http://host/status", "http://host?x=1", "http://"] {
            precondition((try? DongleClient.baseURL(from: bad)) == nil, bad)
        }
        precondition(DongleClient.configured("nonsense").baseURL == DongleClient.defaultBaseURL)
        precondition(DongleClient.configured("http://phoneuse.local").host == "phoneuse.local")
        precondition(DongleClient().host == "172.31.254.1")
        let esp = try! JSONDecoder().decode(DongleStatus.self, from: Data(
            "{\"device\":\"PhoneUse ESP32\",\"transport\":\"Wi-Fi + BLE\",\"running\":false,\"state\":\"idle\",\"hidReady\":false,\"reports\":3,\"bleConnected\":false,\"hint\":\"Pair the tool\"}".utf8))
        precondition(esp.transport == "Wi-Fi + BLE" && esp.hint == "Pair the tool" && !esp.hidReady)
        precondition(SetupStatus.describe(esp) == "Pair the tool")
        let espReady = try! JSONDecoder().decode(DongleStatus.self, from: Data(
            "{\"device\":\"PhoneUse ESP32\",\"transport\":\"Wi-Fi + BLE\",\"running\":false,\"state\":\"done\",\"hidReady\":true}".utf8))
        precondition(SetupStatus.describe(espReady) == "PhoneUse ESP32 · Wi-Fi + BLE")
        precondition(SetupStatus.describe(tool) == "XIAO RP2040 · USB")
        precondition(SetupStatus.describe(unidentified) == "Input tool · USB")
        let espBusy = try! JSONDecoder().decode(DongleStatus.self, from: Data(
            "{\"device\":\"PhoneUse ESP32\",\"transport\":\"Wi-Fi + BLE\",\"running\":true,\"state\":\"typing\",\"hidReady\":true}".utf8))
        precondition(SetupStatus.describe(espBusy) == "Input tool is sending input")
        let storedTool = try! JSONSerialization.data(withJSONObject: [
            "address": "wss://control.example/device", "sessionID": "s", "deviceID": "d", "inputToolAddress": "http://phoneuse.local"
        ])
        let keptTool = try! JSONSerialization.jsonObject(with: try! SharedConfiguration.revokingSession(in: storedTool)) as! [String: String]
        precondition(keptTool["inputToolAddress"] == "http://phoneuse.local" && keptTool["sessionID"] == nil)
        let decodedTool = try! JSONDecoder().decode(SharedConfiguration.self, from: storedTool)
        precondition(decodedTool.inputToolAddress == "http://phoneuse.local" && decodedTool.inputToolURL.absoluteString == "http://phoneuse.local")
        let legacy = try! JSONDecoder().decode(SharedConfiguration.self, from: Data("{\"address\":\"wss://control.example/device\"}".utf8))
        precondition(legacy.inputToolAddress == nil && legacy.inputToolURL == DongleClient.defaultBaseURL)
        let encoded = try! JSONSerialization.jsonObject(with: try! JSONEncoder().encode(
            SharedConfiguration(address: "wss://control.example/device", inputToolAddress: " http://192.168.1.9 "))) as! [String: Any]
        precondition(encoded["inputToolAddress"] as? String == "http://192.168.1.9")
        precondition((try! JSONSerialization.jsonObject(with: try! JSONEncoder().encode(
            SharedConfiguration(address: "wss://control.example/device", inputToolAddress: ""))) as! [String: Any])["inputToolAddress"] == nil)
        let configuration = URLSessionConfiguration.ephemeral
        let broadcastError = RelayError(message: "Start a new session in the iPad app") as NSError
        precondition(broadcastError.domain == "iPadComputerUse")
        precondition(broadcastError.userInfo[NSLocalizedDescriptionKey] as? String == "Start a new session in the iPad app")
        configuration.protocolClasses = [StatusProtocol.self]
        let state = SetupStatus(session: URLSession(configuration: configuration))
        precondition(!state.hasCheckedServer && !state.isChecking)
        let address = "wss://control.example/device"
        StatusProtocol.body = "{\"connected\":true,\"pointerCalibrated\":true,\"calibrating\":false,\"screenBroadcast\":true}"
        await state.refresh(address: address, checkInput: false)
        precondition(state.serverConnected && state.calibrated == true && state.screenBroadcast)
        precondition(state.hasCheckedServer && !state.isChecking && state.retryDelay == 2)
        precondition(!state.allReady && state.readyCount == 2)
        precondition(StatusProtocol.request?.url?.scheme == "https")
        precondition(StatusProtocol.request?.url?.path == "/device-status/" + SharedConfiguration.localDeviceID())
        precondition(StatusProtocol.request?.value(forHTTPHeaderField: "Authorization") == nil)
        state.inputReady = true
        precondition(state.allReady)
        StatusProtocol.body = "{\"connected\":false,\"pointerCalibrated\":true,\"calibrating\":false,\"screenBroadcast\":false}"
        await state.refresh(address: address, checkInput: false)
        precondition(!state.serverConnected && state.calibrated == true && !state.allReady)
        StatusProtocol.code = 503
        await state.refresh(address: address, checkInput: false)
        precondition(!state.serverConnected && state.calibrated == nil && !state.screenBroadcast)
        precondition(state.serverDetail == "Server status unavailable")
        precondition(state.retryDelay == 4 && !state.isChecking)
        for _ in 0..<5 { await state.refresh(address: address, checkInput: false) }
        precondition(state.retryDelay == 30)
        state.nextCheck = Date().addingTimeInterval(30)
        state.retryNow()
        precondition(state.retryDelay == 2 && state.nextCheck == nil)
        StatusProtocol.code = 200; StatusProtocol.body = "invalid"
        await state.refresh(address: address, checkInput: false)
        precondition(!state.serverConnected && state.calibrated == nil)
        await state.refresh(address: "", checkInput: false)
        precondition(state.serverDetail == "Connection details needed")
        state.reset()
        precondition(!state.inputCompatible)
        precondition(state.inputReady == nil && state.readyCount == 0)
        precondition(!state.isChecking && !state.hasCheckedServer && state.retryDelay == 2)
        StatusProtocol.hold = true
        let checking = Task { await state.refresh(address: address, checkInput: false) }
        while !state.isChecking { await Task.yield() }
        precondition(state.checkingServer && !state.checkingInput)
        checking.cancel()
        await checking.value
        precondition(!state.isChecking && !state.hasCheckedServer && state.retryDelay == 2)
        StatusProtocol.hold = false
        StatusProtocol.body = "{\"connected\":true,\"pointerCalibrated\":true,\"calibrating\":false,\"screenBroadcast\":true}"
        await state.refresh(address: address, checkInput: false)
        precondition(state.serverReachable && state.hasCheckedServer && !state.isChecking)
        state.inputReady = true
        state.sessionID = nil
        StatusProtocol.request = nil
        await state.refresh(address: address, background: true)
        precondition(StatusProtocol.request == nil && state.inputReady == true && state.calibrated == true)
        state.calibrated = false
        StatusProtocol.hold = true
        let pointerCheck = Task { await state.refresh(address: address, background: true) }
        while !state.isChecking { await Task.yield() }
        precondition(state.checkingPointer && !state.checkingServer && !state.checkingInput)
        pointerCheck.cancel()
        await pointerCheck.value
        StatusProtocol.hold = false
        await state.refresh(address: address, background: true)
        precondition(state.calibrated == true && !state.isChecking)
        state.sessionID = UUID().uuidString
        StatusProtocol.request = nil
        await state.refresh(address: address, background: true)
        precondition(StatusProtocol.request != nil && !state.isChecking && state.inputReady == true)
        state.serverReachable = false
        StatusProtocol.hold = true
        let serverCheck = Task { await state.refresh(address: address, background: true) }
        while !state.isChecking { await Task.yield() }
        precondition(state.checkingServer && !state.checkingPointer && !state.checkingInput)
        serverCheck.cancel()
        await serverCheck.value
        StatusProtocol.hold = false
        state.retryNow()
        StatusProtocol.request = nil
        await state.refresh(address: address, checkInput: false)
        precondition(StatusProtocol.request != nil && state.serverReachable && state.calibrated == true)
        state.inputReady = true; state.inputCompatible = true
        state.useInputTool(address: "http://phoneuse.local")
        precondition(state.inputReady == nil && !state.inputCompatible && state.inputDetail == "Checking input tool")
        state.inputReady = true
        state.useInputTool(address: " http://phoneuse.local ")
        precondition(state.inputReady == true)
        print("Input tool address handling passed: USB default, Wi-Fi ESP32 address validation, status decoding for both firmwares.")
        print("Selective refresh passed: healthy idle skips requests; pointer-only and server-only retries; active-session telemetry; manual refresh recovery.")
        print("Setup status tests passed: connections, loading, cancellation, retry backoff, manual retry, recovery, and reset.")
    }
}
