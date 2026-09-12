import Foundation

/// Chip family identification. Grounded in esptool v4.9 (`targets/esp32.py`,
/// `esp32c3.py`, `esp32s3.py`).
public enum ChipFamily: String, Codable, CaseIterable, Equatable {
    case esp32 = "ESP32"
    case esp32s2 = "ESP32-S2"
    case esp32c3 = "ESP32-C3"
    case esp32s3 = "ESP32-S3"
    case esp32c2 = "ESP32-C2"
    case esp32c6 = "ESP32-C6"
    case unknown = "unknown"

    /// Match against the chip id returned by `get_security_info`.
    public static func fromImageChipID(_ id: UInt32) -> ChipFamily {
        switch id {
        case 0: return .esp32
        case 2: return .esp32s2
        case 5: return .esp32c3
        case 9: return .esp32s3
        case 12: return .esp32c2
        case 13: return .esp32c6
        default: return .unknown
        }
    }

    /// Match against the magic value at 0x40001000 (classic ESP32/S2 only;
    /// newer chips do not expose a stable magic and are matched by chip id).
    public static func fromMagic(_ magic: UInt32) -> ChipFamily {
        switch magic {
        case 0x00F01D83: return .esp32
        case 0x000007C6: return .esp32s2
        default: return .unknown
        }
    }

    public var imageChipID: UInt16? {
        switch self {
        case .esp32: return 0
        case .esp32s2: return 2
        case .esp32c3: return 5
        case .esp32s3: return 9
        case .esp32c2: return 12
        case .esp32c6: return 13
        case .unknown: return nil
        }
    }

    /// Where the 2nd-stage bootloader lives in flash.
    public var bootloaderFlashOffset: UInt32 {
        self == .esp32 ? 0x1000 : 0x0
    }

    /// Length of the status bytes in command replies. All ESP32-family ROM
    /// loaders use 4 (inherited from ESP32ROM in esptool); only the software
    /// stub uses 2, which we do not run.
    public var statusBytesLength: Int {
        4
    }

    /// ROM loaders of these chips take an extra encrypted-flash flag on
    /// `flash_begin`.
    public var supportsEncryptedROMFlash: Bool {
        self != .esp32 && self != .unknown
    }

    public var spi: SPIRegisterSet {
        switch self {
        case .esp32:
            return SPIRegisterSet(base: 0x3FF42000, usr: 0x1C, usr1: 0x20, usr2: 0x24,
                                  mosiDlen: 0x28, misoDlen: 0x2C, w0: 0x80, addrMSB: true)
        default:
            return SPIRegisterSet(base: 0x60002000, usr: 0x18, usr1: 0x1C, usr2: 0x20,
                                  mosiDlen: 0x24, misoDlen: 0x28, w0: 0x58, addrMSB: false)
        }
    }

    /// The chip id string esptool expects (`--chip`).
    public var esptoolName: String {
        switch self {
        case .esp32: return "esp32"
        case .esp32s2: return "esp32s2"
        case .esp32c3: return "esp32c3"
        case .esp32s3: return "esp32s3"
        case .esp32c2: return "esp32c2"
        case .esp32c6: return "esp32c6"
        case .unknown: return "auto"
        }
    }
}

/// Per-family SPI peripheral register offsets for `run_spiflash_command`.
public struct SPIRegisterSet {
    public let base: UInt32
    public let usr: UInt32
    public let usr1: UInt32
    public let usr2: UInt32
    public let mosiDlen: UInt32
    public let misoDlen: UInt32
    public let w0: UInt32
    /// Whether the SPI address register sends the address MSB-first.
    public let addrMSB: Bool
}

/// Everything the detector learned about a connected chip.
public struct ChipInfo: Codable, Equatable {
    public var family: ChipFamily
    /// "chip-id" (get_security_info) or "magic" (0x40001000 read).
    public var detectionMethod: String
    /// The raw chip id or magic value used for detection.
    public var rawValue: UInt32?
    public var revisionMajor: Int?
    public var revisionMinor: Int?
    public var package: String?
    public var macAddress: String?
    /// Embedded flash size reported by eFuse (C3/S3), e.g. "4MB".
    public var efuseEmbeddedFlash: String?
    public var flashVendorEfuse: String?
    /// "USB-Serial/JTAG", "USB-OTG" or nil when a plain UART bridge is used.
    public var usbMode: String?

    public init(family: ChipFamily, detectionMethod: String, rawValue: UInt32?) {
        self.family = family
        self.detectionMethod = detectionMethod
        self.rawValue = rawValue
    }

    public var displayName: String {
        family == .unknown ? "Unknown ESP chip" : family.rawValue
    }

    public var revisionText: String? {
        guard let major = revisionMajor, let minor = revisionMinor else { return nil }
        return "v\(major).\(minor)"
    }
}

/// Full detection result: chip identity plus physical flash info.
public struct DeviceInfo: Codable, Equatable {
    public var chip: ChipInfo
    public var flashSizeBytes: UInt64?
    /// "spi-id" when read from the flash chip, "efuse" when read from eFuse.
    public var flashSizeSource: String?
    public var flashID: UInt32?
    public var flashManufacturer: String?

    public var flashSizeText: String? {
        guard let bytes = flashSizeBytes else { return nil }
        return "\(bytes / 1024 / 1024)MB"
    }
}

/// Flash size id (JEDEC capacity byte) -> bytes, from esptool cmds.py.
public let detectedFlashSizes: [UInt32: UInt64] = [
    0x12: 256 * 1024, 0x13: 512 * 1024, 0x14: 1 << 20, 0x15: 2 << 20,
    0x16: 4 << 20, 0x17: 8 << 20, 0x18: 16 << 20, 0x19: 32 << 20,
    0x1A: 64 << 20, 0x1B: 128 << 20, 0x1C: 256 << 20,
    0x20: 64 << 20, 0x21: 128 << 20, 0x22: 256 << 20,
    0x32: 256 * 1024, 0x33: 512 * 1024, 0x34: 1 << 20, 0x35: 2 << 20,
    0x36: 4 << 20, 0x37: 8 << 20, 0x38: 16 << 20, 0x39: 32 << 20,
    0x3A: 64 << 20,
]

/// Common SPI flash manufacturers by JEDEC manufacturer id.
public let flashManufacturers: [UInt32: String] = [
    0x20: "XMC", 0x37: "AMIC", 0x5E: "Zbit", 0x68: "BY",
    0x85: "Puya", 0x9D: "ISSI", 0xA1: "Fudan", 0xC2: "Macronix",
    0xC8: "GigaDevice", 0xEF: "Winbond", 0x1C: "EON",
]

/// Flash size <-> bootloader size-field encoding (shared by all ESP32-family).
public enum FlashSize: String, Codable, CaseIterable, Equatable {
    case m1 = "1MB"
    case m2 = "2MB"
    case m4 = "4MB"
    case m8 = "8MB"
    case m16 = "16MB"
    case m32 = "32MB"
    case m64 = "64MB"
    case m128 = "128MB"

    public var bytes: UInt64 {
        switch self {
        case .m1: return 1 << 20
        case .m2: return 2 << 20
        case .m4: return 4 << 20
        case .m8: return 8 << 20
        case .m16: return 16 << 20
        case .m32: return 32 << 20
        case .m64: return 64 << 20
        case .m128: return 128 << 20
        }
    }

    /// High nibble of the bootloader image flash-size/frequency byte.
    public var sizeBits: UInt8 {
        switch self {
        case .m1: return 0x00
        case .m2: return 0x10
        case .m4: return 0x20
        case .m8: return 0x30
        case .m16: return 0x40
        case .m32: return 0x50
        case .m64: return 0x60
        case .m128: return 0x70
        }
    }

    public static func fromBytes(_ bytes: UInt64) -> FlashSize? {
        allCases.first { $0.bytes == bytes }
    }
}
