import Foundation

/// One ESP-IDF partition.
public struct Partition: Codable, Equatable {
    public var name: String
    public var type: UInt8
    public var subtype: UInt8
    public var offset: UInt32
    public var size: UInt32
    public var flags: UInt32

    public init(name: String, type: UInt8, subtype: UInt8, offset: UInt32, size: UInt32, flags: UInt32 = 0) {
        self.name = name
        self.type = type
        self.subtype = subtype
        self.offset = offset
        self.size = size
        self.flags = flags
    }

    public var isApp: Bool { type == 0x00 }
    public var isData: Bool { type == 0x01 }

    public var typeText: String {
        switch type {
        case 0x00: return "app"
        case 0x01: return "data"
        case 0x02: return "bootloader"
        case 0x03: return "partition_table"
        default: return String(format: "0x%02x", type)
        }
    }

    public var subtypeText: String {
        switch (type, subtype) {
        case (0x00, 0x00): return "factory"
        case (0x00, 0x10..<0x20): return "ota_\(subtype - 0x10)"
        case (0x00, 0x20): return "test"
        case (0x01, 0x00): return "ota"
        case (0x01, 0x01): return "phy"
        case (0x01, 0x02): return "nvs"
        case (0x01, 0x03): return "coredump"
        case (0x01, 0x04): return "nvs_keys"
        case (0x01, 0x05): return "efuse"
        case (0x01, 0x06): return "undefined"
        case (0x01, 0x80): return "esphttpd"
        case (0x01, 0x81): return "fat"
        case (0x01, 0x82): return "spiffs"
        case (0x01, 0x83): return "littlefs"
        default: return String(format: "0x%02x", subtype)
        }
    }
}

/// ESP-IDF partition table binary format (gen_esp32part.py, esp-idf master):
/// 32-byte entries followed by `0xEB 0xEB 0xFF*14` + md5(entries), padded to
/// 0xC00 with 0xFF. The first entry is the first partition — there is no
/// separate zero entry.
public enum PartitionTable {
    public static let maxLength = 0xC00
    public static let md5Begin: [UInt8] = [0xEB, 0xEB] + Array(repeating: 0xFF, count: 14)
    public static let magic: UInt16 = 0x50AA

    // MARK: - CSV parsing (mirrors gen_esp32part semantics)

    public static func parseCSV(_ csv: String) throws -> [Partition] {
        var partitions: [Partition] = []
        var lastEnd: UInt64 = 0x8000 + UInt64(maxLength) // partition table occupies 0x8000..0x8C00
        for (lineNo, rawLine) in csv.split(separator: "\n").enumerated() {
            let line = rawLine.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !line.isEmpty, !line.hasPrefix("#") else { continue }
            let fields = line.split(separator: ",", omittingEmptySubsequences: false)
                .map { $0.trimmingCharacters(in: .whitespaces) }
            guard fields.count >= 5 else {
                throw FlashError("partition CSV line \(lineNo + 1): expected 5+ fields, got \(fields.count)")
            }
            let name = fields[0]
            let type = try parseType(fields[1], line: lineNo + 1)
            let subtype = try parseSubtype(type: type, raw: fields[2], line: lineNo + 1)
            let offsetRaw = fields[3]
            let sizeRaw = fields[4]
            let flags = fields.count >= 6 && !fields[5].isEmpty ? try parseFlags(fields[5]) : 0

            var offset: UInt32?
            if !offsetRaw.isEmpty {
                offset = try parseInt(offsetRaw, line: lineNo + 1, field: "offset")
                if offset! < lastEnd {
                    throw FlashError("partition CSV line \(lineNo + 1): partition \(name) offset 0x\(hex4(offset!)) overlaps previous content ending at 0x\(String(format: "%X", lastEnd))")
                }
            }

            var size: UInt64
            if sizeRaw.hasPrefix("-") {
                // Negative size means "fill to end of flash": -size - offset.
                let neg = try parseInt(sizeRaw, line: lineNo + 1, field: "size")
                guard let offset else {
                    throw FlashError("partition CSV line \(lineNo + 1): negative size requires an explicit offset")
                }
                size = UInt64(neg) &- UInt64(offset)
            } else {
                size = UInt64(try parseInt(sizeRaw, line: lineNo + 1, field: "size"))
            }

            let resolvedOffset = offset ?? UInt32(alignUp(lastEnd, to: type == 0x00 ? 0x10000 : 0x1000))
            if offset == nil { lastEnd = alignUp(lastEnd, to: type == 0x00 ? 0x10000 : 0x1000) }
            lastEnd = UInt64(resolvedOffset) + size

            partitions.append(Partition(name: name, type: type, subtype: subtype,
                                        offset: resolvedOffset, size: UInt32(truncatingIfNeeded: size), flags: flags))
        }
        return partitions
    }

    private static func alignUp(_ v: UInt64, to alignment: UInt64) -> UInt64 {
        (v + alignment - 1) / alignment * alignment
    }

    private static func parseType(_ raw: String, line: Int) throws -> UInt8 {
        switch raw {
        case "app": return 0x00
        case "data": return 0x01
        case "bootloader": return 0x02
        case "partition_table": return 0x03
        default:
            return UInt8(try parseInt(raw, line: line, field: "type"))
        }
    }

    private static func parseSubtype(type: UInt8, raw: String, line: Int) throws -> UInt8 {
        switch (type, raw) {
        case (0x00, "factory"): return 0x00
        case (0x00, "test"): return 0x20
        case (0x01, "ota"): return 0x00
        case (0x01, "phy"): return 0x01
        case (0x01, "nvs"): return 0x02
        case (0x01, "coredump"): return 0x03
        case (0x01, "nvs_keys"): return 0x04
        case (0x01, "efuse"): return 0x05
        case (0x01, "undefined"): return 0x06
        case (0x01, "esphttpd"): return 0x80
        case (0x01, "fat"): return 0x81
        case (0x01, "spiffs"): return 0x82
        case (0x01, "littlefs"): return 0x83
        default:
            if raw.hasPrefix("ota_"), let n = UInt8(raw.dropFirst(4)), n < 16 {
                return 0x10 + n
            }
            return UInt8(try parseInt(raw, line: line, field: "subtype"))
        }
    }

    private static func parseFlags(_ raw: String) throws -> UInt32 {
        var flags: UInt32 = 0
        for token in raw.split(separator: " ").map({ String($0) }) {
            switch token {
            case "encrypted": flags |= 1 << 0
            case "readonly": flags |= 1 << 1
            default:
                flags |= UInt32(try parseInt(token, line: 0, field: "flags"))
            }
        }
        return flags
    }

    private static func parseInt(_ raw: String, line: Int, field: String) throws -> UInt32 {
        if raw.hasPrefix("0x") || raw.hasPrefix("0X") {
            guard let v = UInt32(raw.dropFirst(2), radix: 16) else {
                throw FlashError("partition CSV line \(line): invalid \(field) value '\(raw)'")
            }
            return v
        }
        guard let v = UInt32(raw, radix: 10) else {
            throw FlashError("partition CSV line \(line): invalid \(field) value '\(raw)'")
        }
        return v
    }

    // MARK: - Binary encode / decode

    /// Encode partitions into the canonical binary table (entries + md5,
    /// padded to 0xC00).
    public static func encode(_ partitions: [Partition], includeMD5: Bool = true) -> Data {
        var entries = Data()
        for p in partitions {
            entries += LE.u16(magic)
            entries += Data([p.type, p.subtype])
            entries += LE.u32(p.offset)
            entries += LE.u32(p.size)
            var label = Data(p.name.utf8.prefix(16))
            if label.count < 16 {
                label.append(contentsOf: repeatElement(0, count: 16 - label.count))
            }
            entries += label
            entries += LE.u32(p.flags)
        }
        var out = entries
        if includeMD5 {
            out += Data(md5Begin) + MD5.digest(entries)
        }
        if out.count < maxLength {
            out += Data(repeating: 0xFF, count: maxLength - out.count)
        }
        return out
    }

    /// Decode a binary table (as written by gen_esp32part / PlatformIO).
    public static func decode(_ data: Data) throws -> [Partition] {
        var out: [Partition] = []
        var i = 0
        while i + 32 <= data.count {
            let m = LE.u16(data, i)
            if m != magic { break }
            let type = data[i + 2]
            let subtype = data[i + 3]
            let offset = LE.u32(data, i + 4)
            let size = LE.u32(data, i + 8)
            let labelBytes = data.subdata(in: (i + 12)..<(i + 28)).trimmedNulls
            let label = String(decoding: labelBytes, as: UTF8.self)
            let flags = LE.u32(data, i + 28)
            out.append(Partition(name: label, type: type, subtype: subtype, offset: offset, size: size, flags: flags))
            i += 32
        }
        return out
    }

    /// Format partitions back to CSV text.
    public static func formatCSV(_ partitions: [Partition]) -> String {
        var lines: [String] = ["# Name,   Type, SubType, Offset,   Size,      Flags"]
        for p in partitions {
            let name = p.name.padding(toLength: 8, withPad: " ", startingAt: 0)
            let type = p.typeText.padding(toLength: 10, withPad: " ", startingAt: 0)
            let sub = p.subtypeText.padding(toLength: 9, withPad: " ", startingAt: 0)
            lines.append("\(name) \(type) \(sub) 0x\(String(format: "%06X", p.offset)),  0x\(String(format: "%06X", p.size)),")
        }
        return lines.joined(separator: "\n")
    }

    // MARK: - Default layout generation

    /// Standard single-app layout for a bare flash of `flashSize` bytes:
    /// nvs + phy_init + factory app filling the rest.
    public static func generateDefault(flashSize: UInt64, appSize: UInt64 = 0) throws -> [Partition] {
        let factoryOffset: UInt64 = 0x10000
        let reserved: UInt64 = 0x9000 // nvs 0x6000 + phy 0x1000 + 0x1000 gap? no: nvs at 0x9000..0xF000, phy at 0xF000..0x10000
        guard flashSize >= factoryOffset + 0x9000 + 0x1000 else {
            throw FlashError("flash is too small for a usable layout (need at least 128 KB, have \(flashSize) bytes)")
        }
        let factorySize = flashSize - factoryOffset
        guard appSize == 0 || appSize <= factorySize else {
            throw FlashError("app image (\(appSize) bytes) does not fit the generated factory partition (\(factorySize) bytes)")
        }
        return [
            Partition(name: "nvs", type: 0x01, subtype: 0x02, offset: 0x9000, size: 0x6000),
            Partition(name: "phy_init", type: 0x01, subtype: 0x01, offset: 0xF000, size: 0x1000),
            Partition(name: "factory", type: 0x00, subtype: 0x00, offset: 0x10000, size: UInt32(truncatingIfNeeded: factorySize)),
        ]
    }

    public static func findApp(_ partitions: [Partition]) -> Partition? {
        partitions.first { $0.isApp && $0.subtype == 0x00 }
    }

    public static func find(_ partitions: [Partition], name: String) -> Partition? {
        partitions.first { $0.name == name }
    }
}
