import Foundation

/// Connects to a chip, identifies it (ESP32 / C3 / S3 / ...), reads eFuse
/// details and the physical flash size. Flow mirrors esptool v4.9:
/// reset -> sync -> get_security_info (C3/S3+) or magic read (ESP32/S2) ->
/// _post_connect (watchdog handling on USB-JTAG) -> SPI attach -> flash id.
public final class ChipDetector {
    private let port: SerialPort
    private let loader: ESPLoader
    private let usbProductID: UInt16?

    public var onLog: (String) -> Void = { _ in }

    public init(port: SerialPort, usbProductID: UInt16? = nil) {
        self.port = port
        self.loader = ESPLoader(port: port)
        self.usbProductID = usbProductID
        loader.usbProductID = usbProductID
    }

    public var loaderInstance: ESPLoader { loader }

    /// Release the serial port (e.g. before handing off to esptool).
    public func close() {
        port.close()
    }

    public func detect(connectAttempts: Int = 7) throws -> DeviceInfo {
        try port.open(baud: 115200, readTimeout: 0.1)
        try connect(attempts: connectAttempts)
        try detectChip()
        try postConnect()
        return try readDeviceInfo()
    }

    // MARK: - Connect

    private enum ResetKind: CustomStringConvertible {
        case usbJtag
        case unixTight(delay: TimeInterval)
        case classic(delay: TimeInterval)

        var description: String {
            switch self {
            case .usbJtag: return "usb-jtag reset"
            case .unixTight: return "tight reset"
            case .classic: return "classic reset"
            }
        }
    }

    private func resetStrategies() -> [ResetKind] {
        if usbProductID == ESPLoader.usbJTAGSerialPID {
            // Native USB-Serial/JTAG: DTR/RTS carry special semantics.
            return [.usbJtag, .usbJtag, .usbJtag, .usbJtag, .usbJtag, .usbJtag, .usbJtag]
        }
        return [
            .unixTight(delay: 0.05), .unixTight(delay: 0.55),
            .classic(delay: 0.05), .classic(delay: 0.55),
            .usbJtag, // defensive: some native-USB boards are missed by enumeration
        ]
    }

    private func runReset(_ kind: ResetKind) throws {
        switch kind {
        case .usbJtag: try loader.usbJtagSerialReset()
        case .unixTight(let delay): try loader.unixTightReset(delay: delay)
        case .classic(let delay): try loader.classicReset(delay: delay)
        }
    }

    private func connect(attempts: Int) throws {
        let strategies = resetStrategies()
        var lastError: Error?
        for attempt in 0..<max(1, attempts) {
            let strategy = strategies[attempt % strategies.count]
            do {
                port.flushInput()
                try runReset(strategy)
                checkBootLog()

                for _ in 0..<5 {
                    port.flushInput()
                    port.flushOutput()
                    do {
                        try loader.sync()
                        return
                    } catch {
                        lastError = error
                        Thread.sleep(forTimeInterval: 0.05)
                    }
                }
            } catch {
                lastError = error
            }
            // USB-Serial/JTAG boards drop off USB during reset and
            // re-enumerate; the old fd goes stale, so reopen for the next try.
            if attempt < max(1, attempts) - 1 {
                port.close()
                Thread.sleep(forTimeInterval: 0.2)
                _ = try? port.open(baud: 115200, readTimeout: 0.1)
            }
        }
        throw FlashError("failed to connect to \(port.path): \(lastError.map { "\($0)" } ?? "unknown error")")
    }

    /// Parse the ROM boot log for a "wrong boot mode" diagnosis.
    private func checkBootLog() {
        let data = port.read(port.inWaiting())
        guard !data.isEmpty, let text = String(data: data, encoding: .ascii) else { return }
        let bootMatch = text.range(of: "boot:0x[0-9a-fA-F]+", options: .regularExpression)
        if let bootMatch {
            let mode = text[bootMatch]
            if !text.contains("waiting for download") {
                onLog("note: chip booted to \(mode) (not download mode) — check the GPIO0 strap or press the BOOT button")
            } else {
                onLog("note: download mode detected from boot log (\(mode))")
            }
        }
    }

    // MARK: - Chip identification

    private func detectChip() throws {
        do {
            let info = try loader.getSecurityInfo()
            guard let chipID = info.chipID else {
                throw FlashError("get security info returned no chip id")
            }
            let family = ChipFamily.fromImageChipID(chipID)
            guard family != .unknown else {
                throw FlashError("unsupported chip id \(chipID)")
            }
            loader.family = family
            _ = family
            return
        } catch let error as FlashError where error.isUnsupportedCommand {
            // Classic ESP32 / S2: identify via the magic value register.
            let magic = try loader.readReg(ESPLoader.chipDetectMagicReg)
            let family = ChipFamily.fromMagic(magic)
            guard family != .unknown else {
                throw FlashError("unsupported chip magic value 0x\(String(format: "%08X", magic))")
            }
            loader.family = family
            return
        }
    }

    /// Per-chip connection hooks: on C3/S3 the RTC/SWD watchdogs can reset
    /// the board mid-flash when talking over USB-Serial/JTAG, so disable them.
    private func postConnect() throws {
        switch loader.family {
        case .esp32c3, .esp32s3:
            try loader.disableWatchdogsIfNeeded()
        default:
            break
        }
    }

    // MARK: - Device details

    private func readDeviceInfo() throws -> DeviceInfo {
        var chip = ChipInfo(family: loader.family, detectionMethod: "chip-id", rawValue: loader.family.imageChipID.map { UInt32($0) })
        if loader.family == .esp32 || loader.family == .esp32s2 {
            chip.detectionMethod = "magic"
        }

        switch loader.family {
        case .esp32c3:
            let b1: UInt32 = 0x6000_8844 // EFUSE_BLOCK1
            let w3 = try loader.readReg(b1 + 12)
            let w5 = try loader.readReg(b1 + 20)
            chip.package = c3Package((w3 >> 21) & 0x07)
            chip.revisionMajor = Int((w5 >> 24) & 0x03)
            chip.revisionMinor = Int((((w5 >> 23) & 0x01) << 3) | ((w3 >> 18) & 0x07))
            chip.efuseEmbeddedFlash = c3FlashCap((w3 >> 27) & 0x07)
            chip.flashVendorEfuse = c3FlashVendor(try loader.readReg(b1 + 16) & 0x07)
            chip.macAddress = macFromRegs(hi: try loader.readReg(b1 + 4), lo: try loader.readReg(b1))
        case .esp32s3:
            let b1: UInt32 = 0x6000_7044 // EFUSE_BLOCK1
            let w3 = try loader.readReg(b1 + 12)
            let w5 = try loader.readReg(b1 + 20)
            chip.package = s3Package((w3 >> 21) & 0x07)
            chip.revisionMajor = Int((w5 >> 24) & 0x03)
            chip.revisionMinor = Int((((w5 >> 23) & 0x01) << 3) | ((w3 >> 18) & 0x07))
            chip.efuseEmbeddedFlash = s3FlashCap((w3 >> 27) & 0x07)
            chip.flashVendorEfuse = s3FlashVendor(try loader.readReg(b1 + 16) & 0x07)
            chip.macAddress = macFromRegs(hi: try loader.readReg(b1 + 4), lo: try loader.readReg(b1))
        case .esp32:
            let efuseBase: UInt32 = 0x3FF5_A000
            let w3 = try loader.readReg(efuseBase + 12)
            let w5 = try loader.readReg(efuseBase + 20)
            let pkg = ((w3 >> 9) & 0x07) | (((w3 >> 2) & 0x01) << 3)
            chip.package = esp32Package(pkg, singleCore: (w3 & 1) != 0)
            let revBits = (((w3 >> 15) & 1) << 0) | (((w5 >> 20) & 1) << 1) | (((try loader.readReg(0x3FF6_607C) >> 31) & 1) << 2)
            let revMap: [UInt32: Int] = [0: 0, 1: 1, 3: 2, 7: 3]
            chip.revisionMajor = revMap[revBits] ?? 0
            chip.revisionMinor = Int((w5 >> 24) & 0x03)
            chip.macAddress = macFromRegs(hi: try loader.readReg(efuseBase + 8), lo: try loader.readReg(efuseBase + 4))
        default:
            break
        }

        var device = DeviceInfo(chip: chip)
        device = try readFlashInfo(into: device)
        return device
    }

    private func readFlashInfo(into device: DeviceInfo) throws -> DeviceInfo {
        var out = device
        // Attach SPI flash first; the ROM does not enable it by itself.
        if !loader.spiAttached {
            if loader.family == .esp32 {
                let efuseBase: UInt32 = 0x3FF5_A000
                let w5 = try loader.readReg(efuseBase + 0x14)
                let w3 = try loader.readReg(efuseBase + 0x0C)
                let clk = (w5 >> 0) & 0x1F, q = (w5 >> 5) & 0x1F, d = (w5 >> 10) & 0x1F, cs = (w5 >> 15) & 0x1F
                let hd = (w3 >> 4) & 0x1F
                if clk == 0 && q == 0 && d == 0 && hd == 0 && cs == 0 {
                    try loader.flashSpiAttach(hspiArg: 0)
                } else {
                    try loader.flashSpiAttach(hspiArg: (hd << 24) | (cs << 18) | (d << 12) | (q << 6) | clk)
                }
            } else {
                try loader.flashSpiAttach(hspiArg: 0)
            }
        }
        let flashID = try loader.flashID()
        out.flashID = flashID
        // JEDEC id arrives byte-reversed in the SPI register: the first byte
        // (manufacturer) lands in the low byte, capacity in bits 16-23.
        out.flashManufacturer = flashManufacturers[flashID & 0xFF] ?? String(format: "0x%02X", flashID & 0xFF)
        let sizeID = (flashID >> 16) & 0xFF
        if let bytes = detectedFlashSizes[sizeID] {
            out.flashSizeBytes = bytes
            out.flashSizeSource = "spi-id"
        } else if let text = out.chip.efuseEmbeddedFlash,
                  let bytes = FlashSize(rawValue: text)?.bytes {
            out.flashSizeBytes = bytes
            out.flashSizeSource = "efuse"
            onLog("could not map flash id 0x\(String(format: "%06X", flashID)) to a size; using eFuse embedded flash (\(text))")
        }
        return out
    }

    // MARK: - eFuse decoders

    private func c3Package(_ v: UInt32) -> String {
        switch v {
        case 0: return "ESP32-C3 (QFN32)"
        case 1: return "ESP8685 (QFN28)"
        case 2: return "ESP32-C3 AZ (QFN32)"
        case 3: return "ESP8686 (QFN24)"
        default: return "unknown ESP32-C3 package"
        }
    }

    private func s3Package(_ v: UInt32) -> String {
        switch v {
        case 0: return "ESP32-S3 (QFN56)"
        case 1: return "ESP32-S3-PICO-1 (LGA56)"
        default: return "unknown ESP32-S3 package"
        }
    }

    private func esp32Package(_ v: UInt32, singleCore: Bool) -> String {
        switch v {
        case 0: return singleCore ? "ESP32-S0WDQ6" : "ESP32-D0WDQ6"
        case 1: return singleCore ? "ESP32-S0WD" : "ESP32-D0WD"
        case 2: return "ESP32-D2WD"
        case 4: return "ESP32-U4WDH"
        case 5: return "ESP32-PICO-D4"
        case 6: return "ESP32-PICO-V3-02"
        case 7: return "ESP32-D0WDR2-V3"
        default: return "unknown ESP32 package"
        }
    }

    private func c3FlashCap(_ v: UInt32) -> String? {
        switch v {
        case 1: return "4MB"
        case 2: return "2MB"
        case 3: return "1MB"
        case 4: return "8MB"
        default: return nil
        }
    }

    private func s3FlashCap(_ v: UInt32) -> String? {
        switch v {
        case 1: return "8MB"
        case 2: return "4MB"
        default: return nil
        }
    }

    private func c3FlashVendor(_ v: UInt32) -> String? {
        switch v {
        case 1: return "XMC"
        case 2: return "GD"
        case 3: return "FM"
        case 4: return "TT"
        case 5: return "ZBIT"
        default: return nil
        }
    }

    private func s3FlashVendor(_ v: UInt32) -> String? {
        v == 1 ? "BY" : nil
    }

    /// MAC from the two eFuse words: esptool packs both big-endian and keeps
    /// the last 6 bytes.
    private func macFromRegs(hi: UInt32, lo: UInt32) -> String {
        var bytes = Data()
        bytes += LE.u16BE(UInt16(truncatingIfNeeded: hi))
        bytes += LE.u32BE(lo)
        return bytes.map { String(format: "%02X", $0) }.joined(separator: ":")
    }
}
