import Foundation

/// Shared detection entry point for the CLI and the GUI.
///
/// - With `port` set: detect on exactly that device.
/// - Otherwise: scan serial ports (USB adapters first, up to 8), returning
///   the first device that answers the ESP sync.
public func detectDevice(port: String?, explicitAttempts: Int = 7, scanAttempts: Int = 3,
                         onLog: @escaping (String) -> Void = { _ in })
    throws -> (detector: ChipDetector, device: DeviceInfo, portInfo: SerialPortInfo?) {
    if let port {
        let sp = SerialPort(path: port)
        // Resolve USB metadata for the explicit path so native USB-Serial/JTAG
        // boards get the right reset sequence immediately.
        let matched = SerialPortScanner.all().first { $0.path == port }
        let detector = ChipDetector(port: sp, usbProductID: matched?.productID)
        detector.onLog = onLog
        let device = try detector.detect(connectAttempts: explicitAttempts)
        let info = matched ?? SerialPortInfo(path: port,
                                             name: URL(fileURLWithPath: port).lastPathComponent,
                                             vendorID: nil, productID: nil, serialNumber: nil, isUSB: false)
        return (detector, device, info)
    }

    var candidates = SerialPortScanner.all()
    candidates.sort { ($0.isUSB ? 0 : 1) < ($1.isUSB ? 0 : 1) }
    var errors: [String] = []
    for info in candidates.prefix(8) {
        let sp = SerialPort(path: info.path)
        let detector = ChipDetector(port: sp, usbProductID: info.productID)
        detector.onLog = onLog
        do {
            let device = try detector.detect(connectAttempts: scanAttempts)
            return (detector, device, info)
        } catch {
            errors.append("\(info.path): \(error)")
        }
    }
    if candidates.isEmpty {
        throw FlashError("no serial ports found — plug in the board")
    }
    throw FlashError("no ESP32 found on any port:\n" + errors.joined(separator: "\n"))
}

public func parseChipFamily(_ raw: String) throws -> ChipFamily {
    switch raw.lowercased() {
    case "esp32": return .esp32
    case "esp32s2", "s2": return .esp32s2
    case "esp32c3", "c3": return .esp32c3
    case "esp32s3", "s3": return .esp32s3
    case "esp32c2", "c2": return .esp32c2
    case "esp32c6", "c6": return .esp32c6
    default:
        throw FlashError("unknown chip '\(raw)' (expected esp32, esp32c3, esp32s3, ...)")
    }
}

public func parseFlashSize(_ raw: String) throws -> FlashSize {
    let upper = raw.uppercased()
    guard let size = FlashSize(rawValue: upper) else {
        throw FlashError("unknown flash size '\(raw)' (expected 1MB, 2MB, 4MB, 8MB, 16MB, ...)")
    }
    return size
}

public struct DetectOutput: Codable {
    public var port: SerialPortInfo?
    public var device: DeviceInfo

    public init(port: SerialPortInfo?, device: DeviceInfo) {
        self.port = port
        self.device = device
    }
}
