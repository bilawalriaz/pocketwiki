import Foundation

/// Little-endian packing helpers. Everything in the ESP ROM protocol and the
/// partition table is little-endian.
public enum LE {
    public static func u16(_ v: UInt16) -> Data {
        Data([UInt8(truncatingIfNeeded: v), UInt8(truncatingIfNeeded: v >> 8)])
    }

    public static func u32(_ v: UInt32) -> Data {
        Data([
            UInt8(truncatingIfNeeded: v),
            UInt8(truncatingIfNeeded: v >> 8),
            UInt8(truncatingIfNeeded: v >> 16),
            UInt8(truncatingIfNeeded: v >> 24),
        ])
    }

    public static func u16(_ data: Data, _ at: Int) -> UInt16 {
        guard at + 2 <= data.count else { return 0 }
        return UInt16(data[at]) | (UInt16(data[at + 1]) << 8)
    }

    public static func u32(_ data: Data, _ at: Int) -> UInt32 {
        guard at + 4 <= data.count else { return 0 }
        return UInt32(data[at])
            | (UInt32(data[at + 1]) << 8)
            | (UInt32(data[at + 2]) << 16)
            | (UInt32(data[at + 3]) << 24)
    }

    public static func u16BE(_ v: UInt16) -> Data {
        Data([UInt8(truncatingIfNeeded: v >> 8), UInt8(truncatingIfNeeded: v)])
    }

    public static func u32BE(_ v: UInt32) -> Data {
        Data([
            UInt8(truncatingIfNeeded: v >> 24),
            UInt8(truncatingIfNeeded: v >> 16),
            UInt8(truncatingIfNeeded: v >> 8),
            UInt8(truncatingIfNeeded: v),
        ])
    }
}

public extension Data {
    /// Pad to a multiple of `multiple`, filling with `fill` (0xFF by default).
    func padTo(_ multiple: Int, fill: UInt8 = 0xFF) -> Data {
        let rem = count % multiple
        guard rem != 0 else { return self }
        return self + Data(repeating: fill, count: multiple - rem)
    }

    var hex: String {
        map { String(format: "%02x", $0) }.joined()
    }

    /// Strip trailing 0x00/0xFF bytes (used when decoding partition labels).
    var trimmedNulls: Data {
        var out = self
        while let last = out.last, last == 0x00 || last == 0xFF {
            out.removeLast()
        }
        return out
    }
}

public func hex4(_ v: UInt32) -> String {
    String(format: "%08X", v)
}

public func hex(_ v: UInt32) -> String {
    String(format: "%x", v)
}
