import XCTest
@testable import ESPFlashCore

/// Firmware images are build output, so a test run only has them when someone
/// has already built the firmware. A missing artifact skips the test that needs
/// it instead of failing a checkout that has not been built.
func stagedFirmware<T>(_ value: T?, _ name: String) throws -> T {
    guard let value else {
        throw XCTSkip("firmware/\(name) is not built: run pio run -d firmware -e esp32-s3")
    }
    return value
}

/// Helper for locating repo firmware artifacts (the PocketWiki checkout).
enum Fixtures {
    static var repoRoot: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent() // ESPFlashCoreTests
            .deletingLastPathComponent() // Tests
            .deletingLastPathComponent() // flasher
            .deletingLastPathComponent() // repo root
    }

    static func fixture(_ path: String) -> URL? {
        let url = repoRoot.appendingPathComponent(path).standardizedFileURL
        return FileManager.default.fileExists(atPath: url.path) ? url : nil
    }

    static var partitionsCSV4MB: String? {
        fixture("firmware/partitions.csv").flatMap { try? String(contentsOf: $0, encoding: .utf8) }
    }

    static var partitionsCSV16MB: String? {
        fixture("firmware/partitions_16mb.csv").flatMap { try? String(contentsOf: $0, encoding: .utf8) }
    }

    static var s3PartitionsBin: Data? {
        fixture("firmware/.pio/build/esp32-s3/partitions.bin").flatMap { try? Data(contentsOf: $0) }
    }

    static var s3FirmwareBin: Data? {
        fixture("firmware/.pio/build/esp32-s3/firmware.bin").flatMap { try? Data(contentsOf: $0) }
    }

    static var s3BootloaderBin: Data? {
        fixture("firmware/.pio/build/esp32-s3/bootloader.bin").flatMap { try? Data(contentsOf: $0) }
    }

    static var s3ContentBin: Data? {
        fixture("firmware/.pio/build/esp32-s3/content/content.bin").flatMap { try? Data(contentsOf: $0) }
    }

    static var s3IndexBin: Data? {
        fixture("firmware/.pio/build/esp32-s3/content/index.bin").flatMap { try? Data(contentsOf: $0) }
    }
}

final class HashingTests: XCTestCase {
    func testMD5Vectors() {
        XCTAssertEqual(MD5.hex(Data()), "d41d8cd98f00b204e9800998ecf8427e")
        XCTAssertEqual(MD5.hex(Data("abc".utf8)), "900150983cd24fb0d6963f7d28e17f72")
    }

    func testSHA256Vectors() {
        XCTAssertEqual(SHA256.hex(Data()), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
        XCTAssertEqual(SHA256.hex(Data("abc".utf8)), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
    }
}

final class SLIPTests: XCTestCase {
    func testEncodeRoundTrip() {
        let payload = Data([0xC0, 0xDB, 0x01, 0x02, 0xC0, 0xDB])
        let framed = SLIP.encode(payload)
        XCTAssertEqual(Array(framed.prefix(1)), [0xC0])
        XCTAssertEqual(Array(framed.suffix(1)), [0xC0])
        XCTAssertEqual(try SLIP.decode(framed), payload)
    }

    func testDecoderIncremental() {
        let decoder = SLIPDecoder()
        let payload = Data([0x01, 0x02, 0xDB, 0xC0])
        let framed = SLIP.encode(payload)
        var packets: [Data] = []
        for byte in framed {
            packets += try! decoder.feed([byte])
        }
        XCTAssertEqual(packets, [payload])
    }

    func testDecoderIgnoresBootNoise() throws {
        let decoder = SLIPDecoder()
        let packets = try decoder.feed(Array("boot:0xf000c1 (2)\n".utf8))
        XCTAssertTrue(packets.isEmpty)
    }
}

final class ChecksumTests: XCTestCase {
    func testROMChecksum() {
        XCTAssertEqual(ESPLoader.checksum(Data()), 0xEF)
        XCTAssertEqual(ESPLoader.checksum(Data([0x01, 0x02, 0x03])), 0xEF ^ 0x01 ^ 0x02 ^ 0x03)
    }
}

final class ByteTests: XCTestCase {
    func testLE() {
        XCTAssertEqual(LE.u16(0x1234), Data([0x34, 0x12]))
        XCTAssertEqual(LE.u32(0xDEADBEEF), Data([0xEF, 0xBE, 0xAD, 0xDE]))
        XCTAssertEqual(LE.u32(Data([0xEF, 0xBE, 0xAD, 0xDE]), 0), 0xDEADBEEF)
        XCTAssertEqual(LE.u16(Data([0x34, 0x12]), 0), 0x1234)
    }
}

final class PartitionTableTests: XCTestCase {
    func testParseRepoCSV4MB() throws {
        let csv = try XCTUnwrap(Fixtures.partitionsCSV4MB)
        let parts = try PartitionTable.parseCSV(csv)
        XCTAssertEqual(parts.count, 6)
        let factory = try XCTUnwrap(parts.first { $0.name == "factory" })
        XCTAssertEqual(factory.type, 0x00)
        XCTAssertEqual(factory.subtype, 0x00)
        XCTAssertEqual(factory.offset, 0x10000)
        XCTAssertEqual(factory.size, 0x180000)
        let content = try XCTUnwrap(parts.first { $0.name == "content" })
        XCTAssertEqual(content.type, 0x01)
        XCTAssertEqual(content.subtype, 0x40)
        XCTAssertEqual(content.offset, 0x190000)
        let packs = try XCTUnwrap(parts.first { $0.name == "packs" })
        XCTAssertEqual(packs.subtype, 0x81) // FAT + wear levelling
    }

    func testParseRepoCSV16MB() throws {
        let csv = try XCTUnwrap(Fixtures.partitionsCSV16MB)
        let parts = try PartitionTable.parseCSV(csv)
        XCTAssertEqual(parts.count, 6)
        let factory = try XCTUnwrap(parts.first { $0.name == "factory" })
        XCTAssertEqual(factory.size, 0x180000) // 1.5 MB app
        let content = try XCTUnwrap(parts.first { $0.name == "content" })
        XCTAssertEqual(content.offset, 0x190000)
        let packs = try XCTUnwrap(parts.first { $0.name == "packs" })
        XCTAssertEqual(packs.offset, 0x220000)
        XCTAssertEqual(packs.size, 0xDE0000) // ~14 MB FAT store
    }

    /// The encoder must reproduce PlatformIO's partitions.bin byte for byte.
    func testEncodeMatchesPlatformIOBinary() throws {
        let csv = try XCTUnwrap(Fixtures.partitionsCSV16MB)
        let real = try stagedFirmware(Fixtures.s3PartitionsBin, ".pio/build/esp32-s3/partitions.bin")
        let parts = try PartitionTable.parseCSV(csv)
        let encoded = PartitionTable.encode(parts)
        XCTAssertEqual(encoded, real, "generated table must match gen_esp32part output")
    }

    func testDecodeRoundTrip() throws {
        let csv = try XCTUnwrap(Fixtures.partitionsCSV16MB)
        let parts = try PartitionTable.parseCSV(csv)
        let decoded = try PartitionTable.decode(PartitionTable.encode(parts))
        XCTAssertEqual(decoded, parts)
    }

    func testDecodePlatformIOBinary() throws {
        let csv = try XCTUnwrap(Fixtures.partitionsCSV16MB)
        let real = try stagedFirmware(Fixtures.s3PartitionsBin, ".pio/build/esp32-s3/partitions.bin")
        let parts = try PartitionTable.parseCSV(csv)
        let decoded = try PartitionTable.decode(real)
        XCTAssertEqual(decoded, parts)
    }

    func testGenerateDefault() throws {
        let parts = try PartitionTable.generateDefault(flashSize: 4 << 20, appSize: 0x100000)
        XCTAssertEqual(parts.count, 3)
        XCTAssertEqual(parts[0].name, "nvs")
        XCTAssertEqual(parts[0].offset, 0x9000)
        XCTAssertEqual(parts[0].size, 0x6000)
        XCTAssertEqual(parts[1].name, "phy_init")
        XCTAssertEqual(parts[2].name, "factory")
        XCTAssertEqual(parts[2].offset, 0x10000)
        XCTAssertEqual(parts[2].size, 4 << 20 - 0x10000)
    }

    func testGenerateDefaultAppTooBig() {
        XCTAssertThrowsError(try PartitionTable.generateDefault(flashSize: 1 << 20, appSize: 1 << 20))
    }
}

final class FirmwareImageTests: XCTestCase {
    func testParseRealS3Firmware() throws {
        let data = try stagedFirmware(Fixtures.s3FirmwareBin, ".pio/build/esp32-s3/firmware.bin")
        let image = try XCTUnwrap(FirmwareImage(data: data))
        XCTAssertEqual(image.chipID, 9) // ESP32-S3
        XCTAssertEqual(image.magic, 0xE9)
        XCTAssertTrue(image.segmentCount > 0)
    }

    func testValidateAppChipMismatch() throws {
        let data = try stagedFirmware(Fixtures.s3FirmwareBin, ".pio/build/esp32-s3/firmware.bin")
        XCTAssertThrowsError(try FirmwareImage.validateApp(data, for: .esp32c3)) { error in
            XCTAssertTrue("\(error)".contains("chip id"))
        }
        _ = try FirmwareImage.validateApp(data, for: .esp32s3)
    }

    func testPatchBootloaderFlashSize() throws {
        let data = try stagedFirmware(Fixtures.s3BootloaderBin, ".pio/build/esp32-s3/bootloader.bin")
        let original = data[3]
        let patched = FirmwareImage.patchBootloaderFlashSize(data, sizeBits: 0x30) // 8MB
        XCTAssertEqual(patched.count, data.count)
        XCTAssertEqual(patched[0], 0xE9)
        XCTAssertEqual(patched[3] & 0xF0, 0x30)
        XCTAssertEqual(patched[3] & 0x0F, original & 0x0F) // frequency untouched
        // Header bytes and segments stay untouched; only byte 3 and, when the
        // image carries an appended SHA-256, that recomputed trailer may change.
        let parsed = try XCTUnwrap(FirmwareImage(data: data))
        let shaStart = parsed.dataLength + 1
        XCTAssertEqual(Array(patched[4..<shaStart]), Array(data[4..<shaStart]))
        if parsed.hashAppended {
            XCTAssertNotEqual(Array(patched[shaStart...]), Array(data[shaStart...]),
                              "patching the header must recompute the appended SHA-256")
        } else {
            XCTAssertEqual(Array(patched[4...]), Array(data[4...]))
        }
        // Same size: unchanged
        XCTAssertEqual(FirmwareImage.patchBootloaderFlashSize(data, sizeBits: original & 0xF0), data)
    }

    func testPatchSyntheticSHAImage() {
        // 24-byte header + one 8-byte segment header + 8 data bytes + SHA trailer.
        var image = Data()
        image += Data([0xE9, 1, 0, 0x20])                    // magic, 1 segment, mode, size|freq
        image += LE.u32(0x40000000)                          // entry
        image += Data(repeating: 0, count: 16)               // extended header
        image[23] = 1                                        // sha appended flag
        image += LE.u32(0x3FC88000) + LE.u32(8) + Data([0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01, 0x02])
        image += Data([0xEB]) + Data(repeating: 0x00, count: 32) + Data([0x00]) // trailer
        let patched = FirmwareImage.patchBootloaderFlashSize(image, sizeBits: 0x30)
        XCTAssertEqual(patched[3] & 0xF0, 0x30)
        // SHA over header+ext+segments (dataLength = 24+8+8 = 40) must be recomputed.
        let expected = SHA256.digest(patched.subdata(in: 0..<40))
        XCTAssertEqual(Array(patched[41..<73]), Array(expected))
    }
}

final class ImageBuilderTests: XCTestCase {
    func testBuildPlanFromRealS3Build() throws {
        let fwDir = try stagedFirmware(Fixtures.fixture("firmware/.pio/build/esp32-s3"), ".pio/build/esp32-s3")
        let csv = try XCTUnwrap(Fixtures.partitionsCSV16MB)
        let result = try ImageBuilder.build(BuildOptions(
            app: nil, bootloader: nil, partitionsCSV: csv, fwDir: fwDir,
            chipFamily: .esp32s3, flashSizeBytes: 16 << 20, flashSizeBits: 0x40
        ))
        XCTAssertTrue(result.fitsFlash)
        XCTAssertEqual(result.segments.map { $0.label }, ["bootloader", "partition-table", "factory", "content", "index"])
        XCTAssertEqual(result.segments[0].offset, 0x0) // S3 bootloader
        XCTAssertEqual(result.segments[1].offset, 0x8000)
        XCTAssertEqual(result.segments[2].offset, 0x10000)
        XCTAssertEqual(result.segments[3].offset, 0x190000) // content
        XCTAssertEqual(result.segments[4].offset, 0x210000) // index
        // Merge: no gaps contain zeros, first bytes are the bootloader image.
        XCTAssertEqual(result.merged[0], 0xE9)
        XCTAssertEqual(result.merged.count, Int(result.endAddress))
    }

    func testRejectsPartitionLayoutLargerThanDetectedFlash() throws {
        let fwDir = try stagedFirmware(Fixtures.fixture("firmware/.pio/build/esp32-s3"), ".pio/build/esp32-s3")
        let csv = try XCTUnwrap(Fixtures.partitionsCSV16MB)
        XCTAssertThrowsError(try ImageBuilder.build(BuildOptions(
            app: nil, bootloader: nil, partitionsCSV: csv, fwDir: fwDir,
            chipFamily: .esp32s3, flashSizeBytes: 8 << 20, flashSizeBits: 0x20
        ))) { error in
            XCTAssertTrue(String(describing: error).contains("detected flash is only 8 MB"))
        }
    }

    func testAppOnlyPlan() throws {
        let fwDir = try stagedFirmware(Fixtures.fixture("firmware/.pio/build/esp32-s3"), ".pio/build/esp32-s3")
        let csv = try XCTUnwrap(Fixtures.partitionsCSV16MB)
        let result = try ImageBuilder.build(BuildOptions(
            app: nil, bootloader: nil, partitionsCSV: csv, fwDir: fwDir,
            chipFamily: .esp32s3, flashSizeBytes: 16 << 20, flashSizeBits: 0x40, appOnly: true
        ))
        XCTAssertEqual(result.segments.map { $0.label }, ["factory"])
        XCTAssertEqual(result.segments[0].offset, 0x10000)
    }

    func testAppTooLargeForPartition() throws {
        let fwDir = try stagedFirmware(Fixtures.fixture("firmware/.pio/build/esp32-s3"), ".pio/build/esp32-s3")
        // 4MB layout against the 16MB build's 4MB app: factory is only 1.5MB there.
        let csv = try XCTUnwrap(Fixtures.partitionsCSV4MB)
        XCTAssertThrowsError(try ImageBuilder.build(BuildOptions(
            app: nil, bootloader: nil, partitionsCSV: csv, fwDir: fwDir,
            chipFamily: .esp32c3, flashSizeBytes: 4 << 20, flashSizeBits: 0x20
        )))
    }
}
