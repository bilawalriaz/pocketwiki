import Foundation

/// The ESP serial bootloader protocol. Command codes, framing, checksum,
/// timeouts and reset sequences mirror esptool v4.9's `loader.py`,
/// `cmds.py` and `reset.py`.
public final class ESPLoader {
    public enum Command: UInt8 {
        case flashBegin = 0x02
        case flashData = 0x03
        case flashEnd = 0x04
        case memBegin = 0x05
        case memEnd = 0x06
        case memData = 0x07
        case sync = 0x08
        case writeReg = 0x09
        case readReg = 0x0A
        case spiSetParams = 0x0B
        case spiAttach = 0x0D
        case readFlashSlow = 0x0E
        case changeBaudrate = 0x0F
        case spiFlashMD5 = 0x13
        case getSecurityInfo = 0x14
        case eraseFlash = 0xD0 // stub only
        case eraseRegion = 0xD1 // stub only
    }

    public static let checksumMagic: UInt8 = 0xEF
    public static let flashWriteSize = 0x400
    public static let stubFlashWriteSize = 0x4000
    public static let ramBlock = 0x1800
    public static let flashSectorSize = 0x1000
    public static let chipDetectMagicReg: UInt32 = 0x4000_1000
    public static let romInvalidRecvMsg: UInt8 = 0x05
    public static let stubHello = "OHAI"

    public static let usbJTAGSerialPID: UInt16 = 0x1001

    private let port: SerialPort
    private let slip = SLIPDecoder()

    /// Set by the detector once the chip is identified.
    public var family: ChipFamily = .unknown
    public private(set) var syncStubDetected = false
    public private(set) var spiAttached = false

    /// True after the software stub flasher is uploaded and running.
    public private(set) var isStub = false
    /// Stub replies use 2 status bytes; ROM replies use 4.
    public var statusBytesLength: Int { isStub ? 2 : family.statusBytesLength }
    public var flashWriteSize: Int { isStub ? Self.stubFlashWriteSize : Self.flashWriteSize }

    /// USB product id of the port (0x1001 = built-in USB-Serial/JTAG), used to
    /// pick the right reset sequence. Set by the detector.
    public var usbProductID: UInt16?

    /// Reset the chip back into download mode with a fresh serial session.
    /// Detection's SPI register commands leave the ROM in a state that
    /// otherwise rejects the stub upload, so we reset before loading it.
    public func resetIntoDownloadMode() throws {
        do {
            if usbProductID == Self.usbJTAGSerialPID {
                try usbJtagSerialReset()
            } else {
                try unixTightReset()
            }
        } catch {
            // ignore; reopen + sync below is the real recovery
        }
        port.close()
        Thread.sleep(forTimeInterval: 0.4)
        try port.open(baud: 115200, readTimeout: 0.1)
        port.flushInput()
        try sync()
        pendingPackets.removeAll()
        slip.reset()
        try disableWatchdogsIfNeeded()
    }

    /// On C3/S3 over USB-Serial/JTAG, the RTC/SWD watchdogs can reset the
    /// board mid-flash; disable them after (re)connecting.
    public func disableWatchdogsIfNeeded() throws {
        switch family {
        case .esp32c3:
            let uartNo = try readReg(0x3FCD_F07C) & 0xFF
            if uartNo == 3 {
                try writeReg(0x6000_80A8, 0x50D8_3AA1)
                try writeReg(0x6000_8090, 0)
                try writeReg(0x6000_80A8, 0)
                try writeReg(0x6000_80B0, 0x8F1D_312A)
                try writeReg(0x6000_80AC, try readReg(0x6000_80AC) | (1 << 31))
                try writeReg(0x6000_80B0, 0)
            }
        case .esp32s3:
            let uartNo = try readReg(0x3FCE_F14C) & 0xFF
            if uartNo == 4 {
                try writeReg(0x6000_80B0, 0x50D8_3AA1)
                try writeReg(0x6000_8098, 0)
                try writeReg(0x6000_80B0, 0)
                try writeReg(0x6000_80B8, 0x8F1D_312A)
                try writeReg(0x6000_80B4, try readReg(0x6000_80B4) | (1 << 31))
                try writeReg(0x6000_80B8, 0)
            }
        default:
            break
        }
    }

    /// When true, `command()` skips the per-call read-timeout reconfig
    /// (termios churn) and relies on the port's fixed timeout. Enabled for a
    /// large flash write so tcsetattr cannot disrupt the USB-JTAG stream.
    public var bulkWrite = false

    /// Set the port's read timeout directly (used to fix it for a bulk write).
    public func setReadTimeout(_ seconds: TimeInterval) {
        port.setReadTimeout(seconds)
    }

    /// Reopen the serial port (the chip stays in download mode) and re-sync,
    /// giving a clean ROM session for the next flash region.
    public func reconnect() throws {
        port.close()
        Thread.sleep(forTimeInterval: 0.2)
        try port.open(baud: 115200, readTimeout: 0.1)
        port.flushInput()
        try sync()
        pendingPackets.removeAll()
        slip.reset()
    }

    public init(port: SerialPort) {
        self.port = port
    }

    // MARK: - Checksum

    public static func checksum(_ data: Data, state: UInt8 = checksumMagic) -> UInt8 {
        var s = state
        for b in data { s ^= b }
        return s
    }

    // MARK: - Command layer

    /// Send a request (SLIP framed) and read the matching response.
    /// Returns the header `val` field and the response body (status bytes
    /// still included; `checkCommand` strips them).
    public func command(op: UInt8?, data: Data = Data(), chk: UInt32 = 0,
                        waitResponse: Bool = true, timeout: TimeInterval = 3.0)
        throws -> (val: UInt32, data: Data) {
        if let op {
            var header = Data([0x00, op])
            header += LE.u16(UInt16(data.count))
            header += LE.u32(chk)
            try port.write(SLIP.encode(header + data))
        }
        guard waitResponse else { return (0, Data()) }

        if !bulkWrite {
            let oldTimeout = port.readTimeout
            port.setReadTimeout(min(timeout, 240))
            defer { port.setReadTimeout(oldTimeout) }
        }

        for _ in 0..<100 {
            guard let packet = try readSLIPPacket() else { throw FlashError.noSerialData() }
            guard packet.count >= 8, packet[0] == 1 else { continue }
            let respOp = packet[1]
            let val = LE.u32(packet, 4)
            let body = packet.count > 8 ? packet.subdata(in: 8..<packet.count) : Data()
            if op == nil || respOp == op {
                return (val, body)
            }
            if body.count >= 2 && body[0] != 0 && body[1] == Self.romInvalidRecvMsg {
                port.flushInput()
                slip.reset()
                throw FlashError.unsupportedCommand(respOp)
            }
        }
        throw FlashError.protocolError("response doesn't match request (op 0x\(String(format: "%02x", op ?? 0)))")
    }

    /// Run a command and check the status bytes. Returns extra response data
    /// beyond the status (e.g. md5 result), or empty.
    @discardableResult
    public func checkCommand(op: UInt8, data: Data = Data(), chk: UInt32 = 0,
                             timeout: TimeInterval = 3.0, description: String) throws -> Data {
        let (_, resp) = try command(op: op, data: data, chk: chk, timeout: timeout)
        let statusLen = statusBytesLength
        guard resp.count >= statusLen else {
            throw FlashError.protocolError("failed to \(description): only \(resp.count) byte status response")
        }
        let status = resp.suffix(statusLen)
        guard status[status.startIndex] == 0 else {
            let hex = status.map { String(format: "%02x", $0) }.joined()
            throw FlashError.protocolError("failed to \(description) (status 0x\(hex))")
        }
        return resp.count > statusLen ? resp.dropLast(statusLen) : Data()
    }

    private var pendingPackets: [Data] = []

    private func readSLIPPacket() throws -> Data? {
        while true {
            if !pendingPackets.isEmpty {
                return pendingPackets.removeFirst()
            }
            let chunk = port.read(256)
            guard !chunk.isEmpty else { throw FlashError.noSerialData() }
            pendingPackets = try slip.feed(Array(chunk))
        }
    }

    // MARK: - Sync

    public func sync() throws {
        let (val, _) = try command(op: Command.sync.rawValue,
                                   data: Data([0x07, 0x07, 0x12, 0x20]) + Data(repeating: 0x55, count: 32),
                                   timeout: 0.1)        // ROM loaders answer sync with a non-zero val; the flasher stub
        // answers 0 (and means the chip was not actually reset).
        var stub = val == 0
        for _ in 0..<7 {
            let (v, _) = try command(op: nil, data: Data(), timeout: 0.1)
            stub = stub && v == 0
        }
        syncStubDetected = stub
    }

    // MARK: - Registers

    public func readReg(_ addr: UInt32, timeout: TimeInterval = 3.0) throws -> UInt32 {
        let (val, data) = try command(op: Command.readReg.rawValue, data: LE.u32(addr), timeout: timeout)
        if data.count >= 1 && data[0] != 0 {
            throw FlashError.protocolError("failed to read register address \(hex4(addr))")
        }
        return val
    }

    public func writeReg(_ addr: UInt32, _ value: UInt32, mask: UInt32 = 0xFFFF_FFFF, delayUs: UInt32 = 0) throws {
        _ = try checkCommand(op: Command.writeReg.rawValue,
                             data: LE.u32(addr) + LE.u32(value) + LE.u32(mask) + LE.u32(delayUs),
                             description: "write target memory")
    }

    // MARK: - Chip info commands

    public struct SecurityInfo {
        public var flashCryptCnt: UInt8
        public var chipID: UInt32?
    }

    public func getSecurityInfo() throws -> SecurityInfo {
        let data = try checkCommand(op: Command.getSecurityInfo.rawValue, description: "get security info")
        guard data.count >= 12 else {
            throw FlashError.protocolError("get security info returned \(data.count) bytes")
        }
        let chipID = data.count >= 16 ? LE.u32(data, 12) : nil
        return SecurityInfo(flashCryptCnt: data[4], chipID: chipID)
    }

    // MARK: - RAM (stub) loading

    public func memBegin(size: UInt32, blocks: Int, blocksize: Int, offset: UInt32) throws {
        _ = try checkCommand(op: Command.memBegin.rawValue,
                             data: LE.u32(size) + LE.u32(UInt32(blocks)) + LE.u32(UInt32(blocksize)) + LE.u32(offset),
                             description: "enter RAM download mode")
    }

    public func memBlock(_ data: Data, seq: Int) throws {
        _ = try checkCommand(op: Command.memData.rawValue,
                             data: LE.u32(UInt32(data.count)) + LE.u32(UInt32(seq)) + LE.u32(0) + LE.u32(0) + data,
                             chk: UInt32(Self.checksum(data)),
                             description: "write to target RAM")
    }

    public func memFinish(entrypoint: UInt32) throws {
        _ = try checkCommand(op: Command.memEnd.rawValue,
                             data: LE.u32(entrypoint == 0 ? 1 : 0) + LE.u32(entrypoint),
                             timeout: 0.2, description: "leave RAM download mode")
    }

    /// A precompiled stub flasher image (from the esptool package).
    public struct StubImage {
        public let text: Data
        public let textStart: UInt32
        public let entry: UInt32
        public let data: Data
        public let dataStart: UInt32
        public let bssStart: UInt32?

        public init?(json: Data) {
            guard let obj = try? JSONSerialization.jsonObject(with: json) as? [String: Any] else { return nil }
            guard let textB64 = obj["text"] as? String,
                  let textStart = obj["text_start"] as? NSNumber,
                  let entry = obj["entry"] as? NSNumber,
                  let dataB64 = obj["data"] as? String,
                  let dataStart = obj["data_start"] as? NSNumber,
                  let text = Data(base64Encoded: textB64),
                  let data = Data(base64Encoded: dataB64) else { return nil }
            self.text = text
            self.textStart = textStart.uint32Value
            self.entry = entry.uint32Value
            self.data = data
            self.dataStart = dataStart.uint32Value
            self.bssStart = (obj["bss_start"] as? NSNumber)?.uint32Value
        }
    }

    /// Upload and run the software stub flasher. On success `isStub` is set
    /// and the larger/faster stub flash protocol is used.
    public func runStub(_ stub: StubImage) throws {
        for (payload, start) in [(stub.text, stub.textStart), (stub.data, stub.dataStart)] {
            guard !payload.isEmpty else { continue }
            let blocks = (payload.count + Self.ramBlock - 1) / Self.ramBlock
            try memBegin(size: UInt32(payload.count), blocks: blocks, blocksize: Self.ramBlock, offset: start)
            for seq in 0..<blocks {
                let lo = seq * Self.ramBlock
                let hi = min(lo + Self.ramBlock, payload.count)
                try memBlock(payload.subdata(in: lo..<hi), seq: seq)
            }
        }
        try memFinish(entrypoint: stub.entry)

        // The stub prints "OHAI" (raw) once running.
        let oldTimeout = port.readTimeout
        port.setReadTimeout(2.0)
        defer { port.setReadTimeout(oldTimeout) }
        var buf = Data()
        let deadline = Date().addingTimeInterval(3.0)
        var gotHello = false
        while Date() < deadline {
            buf += port.read(64)
            if buf.range(of: Data(Self.stubHello.utf8), options: []) != nil {
                gotHello = true
                break
            }
        }
        guard gotHello else {
            throw FlashError("failed to start stub flasher (no OHAI response)")
        }
        port.flushInput()
        pendingPackets.removeAll()
        slip.reset()
        isStub = true
    }

    // MARK: - SPI flash setup

    /// Enable the SPI flash pins. The classic ESP32 needs its eFuse-programmed
    /// pin mux passed here; C3/S3 accept 0.
    public func flashSpiAttach(hspiArg: UInt32 = 0) throws {
        var arg = LE.u32(hspiArg)
        arg += Data([0, 0, 0, 0]) // ROM-only "is_legacy" + reserved bytes
        _ = try checkCommand(op: Command.spiAttach.rawValue, data: arg, description: "configure SPI flash pins")
        spiAttached = true
    }

    public func flashSetParameters(size: UInt32) throws {
        _ = try checkCommand(op: Command.spiSetParams.rawValue,
                             data: LE.u32(0) + LE.u32(size) + LE.u32(64 * 1024) + LE.u32(4 * 1024) + LE.u32(256) + LE.u32(0xFFFF),
                             description: "set SPI params")
    }

    // MARK: - Arbitrary SPI flash commands (flash id, status)

    public func runSPIFlashCommand(cmd: UInt8, data: Data = Data(), readBits: Int = 0,
                                   addr: UInt32? = nil, addrLen: Int = 0, dummyLen: Int = 0) throws -> UInt32 {
        let spi = family.spi
        let usrCommand: UInt32 = 1 << 31
        let usrAddr: UInt32 = 1 << 30
        let usrDummy: UInt32 = 1 << 29
        let usrMiso: UInt32 = 1 << 28
        let usrMosi: UInt32 = 1 << 27
        let usrAddrLenShift: UInt32 = 26
        let usr2CommandLenShift: UInt32 = 28
        let cmdUsr: UInt32 = 1 << 18

        guard readBits <= 32 else { throw FlashError.protocolError("reading more than 32 bits from SPI is unsupported") }
        guard data.count <= 64 else { throw FlashError.protocolError("writing more than 64 bytes with one SPI command is unsupported") }

        func setDataLengths(mosiBits: Int, misoBits: Int) throws {
            if mosiBits > 0 { try writeReg(spi.base + spi.mosiDlen, UInt32(mosiBits - 1)) }
            if misoBits > 0 { try writeReg(spi.base + spi.misoDlen, UInt32(misoBits - 1)) }
            var flags: UInt32 = 0
            if dummyLen > 0 { flags |= UInt32(dummyLen - 1) }
            if addrLen > 0 { flags |= UInt32(addrLen - 1) << usrAddrLenShift }
            if flags != 0 { try writeReg(spi.base + spi.usr1, flags) }
        }

        let dataBits = data.count * 8
        let oldUsr = try readReg(spi.base + spi.usr)
        let oldUsr2 = try readReg(spi.base + spi.usr2)

        var flags = usrCommand
        if readBits > 0 { flags |= usrMiso }
        if dataBits > 0 { flags |= usrMosi }
        if addrLen > 0 { flags |= usrAddr }
        if dummyLen > 0 { flags |= usrDummy }

        try setDataLengths(mosiBits: dataBits, misoBits: readBits)
        try writeReg(spi.base + spi.usr, flags)
        try writeReg(spi.base + spi.usr2, (7 << usr2CommandLenShift) | UInt32(cmd))

        if addrLen > 0, let addr {
            var shifted = addr
            if spi.addrMSB { shifted = addr << UInt32(32 - addrLen) }
            try writeReg(spi.base + 0x04, shifted)
        }

        if dataBits == 0 {
            try writeReg(spi.base + spi.w0, 0)
        } else {
            let padded = data.padTo(4, fill: 0x00)
            var next = spi.base + spi.w0
            for i in stride(from: 0, to: padded.count, by: 4) {
                try writeReg(next, LE.u32(padded, i))
                next += 4
            }
        }

        try writeReg(spi.base + 0x00, cmdUsr)
        var done = false
        for _ in 0..<10 {
            if (try readReg(spi.base + 0x00) & cmdUsr) == 0 { done = true; break }
        }
        guard done else { throw FlashError.protocolError("SPI command did not complete in time") }

        let status = try readReg(spi.base + spi.w0)
        try writeReg(spi.base + spi.usr, oldUsr)
        try writeReg(spi.base + spi.usr2, oldUsr2)
        return status
    }

    /// Read the JEDEC flash id: 3 bytes (manufacturer, type, capacity).
    public func flashID() throws -> UInt32 {
        try runSPIFlashCommand(cmd: 0x9F, readBits: 24) & 0xFF_FFFF
    }

    // MARK: - Flash programming

    /// Enter flash download mode for `size` bytes at `offset`. The ROM
    /// performs the erase up front (so this is where the scaled timeout
    /// lives); the stub erases as it writes. Returns the number of blocks.
    public func flashBegin(size: UInt32, offset: UInt32) throws -> Int {
        let blockSize = flashWriteSize
        let numBlocks = Int((size + UInt32(blockSize - 1)) / UInt32(blockSize))
        let timeout = isStub ? 3.0 : max(3.0, 30.0 * Double(size) / 1_000_000)
        var params = LE.u32(size) + LE.u32(UInt32(numBlocks)) + LE.u32(UInt32(blockSize)) + LE.u32(offset)
        if !isStub && family.supportsEncryptedROMFlash {
            params += LE.u32(0) // encrypted flag: 0 = plaintext
        }
        _ = try checkCommand(op: Command.flashBegin.rawValue, data: params,
                             timeout: timeout, description: "enter flash download mode")
        return numBlocks
    }

    /// Write one 0x400-byte block, retrying up to 3 times like esptool.
    public func flashBlock(_ block: Data, seq: Int) throws {
        for attempt in 0..<3 {
            do {
                _ = try checkCommand(op: Command.flashData.rawValue,
                                     data: LE.u32(UInt32(block.count)) + LE.u32(UInt32(seq)) + LE.u32(0) + LE.u32(0) + block,
                                     chk: UInt32(Self.checksum(block)),
                                     description: "write to target flash after seq \(seq)")
                return
            } catch {
                if attempt == 2 { throw error }
            }
        }
    }

    /// Leave flash mode. For the ROM, `reboot: false` keeps the bootloader
    /// resident; the stub exits on flash_end.
    public func flashFinish(reboot: Bool) throws {
        _ = try checkCommand(op: Command.flashEnd.rawValue,
                             data: LE.u32(reboot ? 0 : 1),
                             timeout: isStub ? 3.0 : 3.0,
                             description: "leave flash mode")
    }

    /// Ask the ROM to hash `size` bytes at `addr` and return the hex md5.
    public func flashMD5(addr: UInt32, size: UInt32) throws -> String {
        let timeout = max(3.0, 8.0 * Double(size) / 1_000_000)
        let data = try checkCommand(op: Command.spiFlashMD5.rawValue,
                                    data: LE.u32(addr) + LE.u32(size) + LE.u32(0) + LE.u32(0),
                                    timeout: timeout, description: "calculate md5sum")
        // The ROM returns 32 ASCII hex chars, sometimes with trailing NUL
        // padding (observed on ESP32-S3); older ROMs return 16 raw bytes.
        var trimmed = data
        while let last = trimmed.last, last == 0 || last == 0xFF || last == 0x20 {
            trimmed.removeLast()
        }
        if trimmed.count == 32, let s = String(data: trimmed, encoding: .utf8), s.allSatisfy({ $0.isHexDigit }) {
            return s.lowercased()
        }
        if trimmed.count == 16 {
            return trimmed.hex
        }
        throw FlashError.protocolError("md5sum command returned unexpected result (\(data.count) bytes)")
    }

    // MARK: - Baud rate

    /// Raise the line rate after sync. Only C3/S3-class ROMs handle this
    /// directly; classic ESP32 needs a crystal-drift workaround, so we keep
    /// 115200 there.
    public func changeBaud(_ baud: Int32) throws {
        guard family == .esp32c3 || family == .esp32s3 || family == .esp32c2 || family == .esp32c6 else {
            throw FlashError("baud rate change is not supported on \(family.rawValue) ROM loader; staying at 115200")
        }
        _ = try command(op: Command.changeBaudrate.rawValue, data: LE.u32(UInt32(bitPattern: baud)) + LE.u32(0))
        try port.setBaud(baud)
        Thread.sleep(forTimeInterval: 0.05)
        port.flushInput()
    }

    // MARK: - Reset sequences (esptool reset.py)

    /// DTR=IO0, RTS=EN; the classic ESP32/C3/S3 dev-kit strap dance.
    public func classicReset(delay: TimeInterval = 0.05) throws {
        try port.setDTR(false) // IO0 = HIGH
        try port.setRTS(true)  // EN = LOW, chip in reset
        Thread.sleep(forTimeInterval: 0.1)
        try port.setDTR(true)  // IO0 = LOW
        try port.setRTS(false) // EN = HIGH, chip out of reset
        Thread.sleep(forTimeInterval: delay)
        try port.setDTR(false) // IO0 = HIGH, done
    }

    /// Sets DTR/RTS simultaneously (TIOCMSET), as required by some adapters.
    public func unixTightReset(delay: TimeInterval = 0.05) throws {
        try port.setDTRandRTS(dtr: false, rts: false)
        try port.setDTRandRTS(dtr: true, rts: true)
        try port.setDTRandRTS(dtr: false, rts: true) // IO0=HIGH & EN=LOW, in reset
        Thread.sleep(forTimeInterval: 0.1)
        try port.setDTRandRTS(dtr: true, rts: false) // IO0=LOW & EN=HIGH, out of reset
        Thread.sleep(forTimeInterval: delay)
        try port.setDTRandRTS(dtr: false, rts: false) // IO0=HIGH, done
        try port.setDTR(false)
    }

    /// Required when talking to the built-in USB-Serial/JTAG peripheral
    /// (PID 0x1001), where DTR/RTS semantics differ.
    public func usbJtagSerialReset() throws {
        try port.setRTS(false)
        try port.setDTR(false) // idle
        Thread.sleep(forTimeInterval: 0.1)
        try port.setDTR(true) // set IO0
        try port.setRTS(false)
        Thread.sleep(forTimeInterval: 0.1)
        try port.setRTS(true) // reset
        try port.setDTR(false)
        try port.setRTS(true)
        Thread.sleep(forTimeInterval: 0.1)
        try port.setDTR(false)
        try port.setRTS(false) // chip out of reset
    }

    /// Hard reset into the app: EN low pulse via RTS.
    public func hardReset(usesUSB: Bool = false) throws {
        try port.setRTS(true) // EN = LOW
        Thread.sleep(forTimeInterval: usesUSB ? 0.2 : 0.1)
        try port.setRTS(false)
        if usesUSB { Thread.sleep(forTimeInterval: 0.2) }
    }
}
