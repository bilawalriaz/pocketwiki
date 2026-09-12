import Foundation

/// ESP32-family application/bootloader image header, used to validate that
/// an app matches the target chip and to patch the bootloader flash size
/// field. Layout per the IDF binary image format.
public struct FirmwareImage {
    /// Image magic byte (0xE9).
    public let magic: UInt8
    public let segmentCount: UInt8
    public let flashMode: UInt8
    /// High nibble = flash size bits, low nibble = frequency.
    public let flashSizeFreq: UInt8
    public let entryPoint: UInt32
    /// chip id from the extended header (offset 12).
    public let chipID: UInt16?
    /// Minimum chip revision (offset 15).
    public let minRev: UInt8?
    /// SHA-256 appended to the image (extended header byte 15).
    public let hashAppended: Bool
    /// Byte length of header + segments, excluding the SHA trailer.
    public let dataLength: Int

    public init?(data: Data) {
        guard data.count >= 8, data[0] == 0xE9 else { return nil }
        magic = data[0]
        segmentCount = data[1]
        flashMode = data[2]
        flashSizeFreq = data[3]
        entryPoint = LE.u32(data, 4)

        let hasExtended = data.count >= 24
        if hasExtended {
            chipID = LE.u16(data, 12)
            minRev = data[15]
            hashAppended = data[23] == 1
        } else {
            chipID = nil
            minRev = nil
            hashAppended = false
        }

        var pos = hasExtended ? 24 : 8
        var valid = true
        for _ in 0..<Int(segmentCount) {
            guard pos + 8 <= data.count else { valid = false; break }
            let segSize = Int(LE.u32(data, pos + 4))
            pos += 8 + segSize
            guard pos <= data.count else { valid = false; break }
        }
        dataLength = valid ? pos : -1
    }

    /// Validate that an app image targets `family`, throwing a useful error
    /// otherwise. Returns the parsed image.
    public static func validateApp(_ data: Data, for family: ChipFamily) throws -> FirmwareImage {
        guard let image = FirmwareImage(data: data) else {
            throw FlashError("file is not an ESP32-family image (bad magic byte; expected 0xE9)")
        }
        if let expected = family.imageChipID, let chipID = image.chipID, chipID != 0, chipID != expected {
            throw FlashError("image is built for chip id \(chipID), but the target is \(family.rawValue) (id \(expected))")
        }
        return image
    }

    /// Patch the flash-size field of a bootloader image (bytes 2-3 hold
    /// flash mode + size|freq). Only touches the size nibble; mode and
    /// frequency stay as compiled. Recomputes the appended SHA-256 when the
    /// image carries one.
    public static func patchBootloaderFlashSize(_ image: Data, sizeBits: UInt8?) -> Data {
        guard let sizeBits, image.count >= 4, image[0] == 0xE9 else { return image }
        let sizeFreq = image[3]
        let patched = (sizeFreq & 0x0F) | sizeBits
        guard patched != sizeFreq else { return image }

        var out = image
        out[3] = patched
        if let parsed = FirmwareImage(data: out),
           parsed.hashAppended,
           parsed.dataLength > 0,
           out.count >= parsed.dataLength + 33 {
            let digest = SHA256.digest(out.subdata(in: 0..<parsed.dataLength))
            out.replaceSubrange((parsed.dataLength + 1)..<(parsed.dataLength + 33), with: digest)
        }
        return out
    }
}
