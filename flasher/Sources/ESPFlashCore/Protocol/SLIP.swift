import Foundation

/// SLIP framing used by every ESP ROM/stub bootloader command exchange.
/// Packet: C0 ... payload (with C0 -> DB DC, DB -> DB DD escapes) ... C0.
public enum SLIP {
    public static let end: UInt8 = 0xC0
    public static let esc: UInt8 = 0xDB
    public static let escEnd: UInt8 = 0xDC
    public static let escEsc: UInt8 = 0xDD

    public static func encode(_ data: Data) -> Data {
        var out = Data()
        out.append(end)
        for b in data {
            switch b {
            case end:
                out.append(contentsOf: [esc, escEnd])
            case esc:
                out.append(contentsOf: [esc, escEsc])
            default:
                out.append(b)
            }
        }
        out.append(end)
        return out
    }

    /// Strip framing and unescape (used by tests and the packet decoder).
    public static func decode(_ data: Data) throws -> Data {
        var out = Data()
        var inEscape = false
        for b in data {
            if b == end { continue }
            if inEscape {
                switch b {
                case escEnd: out.append(end)
                case escEsc: out.append(esc)
                default: throw FlashError.protocolError("invalid SLIP escape (0xdb, 0x\(String(format: "%02x", b)))")
                }
                inEscape = false
            } else if b == esc {
                inEscape = true
            } else {
                out.append(b)
            }
        }
        return out
    }
}

/// Incremental SLIP stream decoder. Mirrors esptool's `slip_reader` state
/// machine: ignores bytes until the first C0, then assembles packets,
/// unescaping as it goes.
public final class SLIPDecoder {
    private var partial: [UInt8]?
    private var inEscape = false

    public init() {}

    public func reset() {
        partial = nil
        inEscape = false
    }

    /// Feed raw bytes; returns every complete packet found.
    public func feed(_ bytes: [UInt8]) throws -> [Data] {
        var packets: [Data] = []
        for b in bytes {
            if partial == nil {
                if b == SLIP.end {
                    partial = []
                }
                // Anything before the first C0 is boot-log noise; ignore it.
            } else if inEscape {
                inEscape = false
                if b == SLIP.escEnd {
                    partial!.append(SLIP.end)
                } else if b == SLIP.escEsc {
                    partial!.append(SLIP.esc)
                } else {
                    throw FlashError.protocolError("invalid SLIP escape (0xdb, 0x\(String(format: "%02x", b)))")
                }
            } else if b == SLIP.esc {
                inEscape = true
            } else if b == SLIP.end {
                packets.append(Data(partial!))
                partial = nil
            } else {
                partial!.append(b)
            }
        }
        return packets
    }
}
