import Foundation

/// Files discovered inside a firmware build directory (PlatformIO or
/// ESP-IDF layout), mirroring the repo's `tools/flash_all.py`.
public struct FirmwareDirContents {
    public var bootloader: URL?
    public var partitionTable: URL?
    public var app: URL?
    public var content: [URL]

    public var isEmpty: Bool {
        bootloader == nil && partitionTable == nil && app == nil && content.isEmpty
    }
}

public struct BuildOptions {
    public var app: URL?
    public var bootloader: URL?
    /// Raw CSV text of a partition layout.
    public var partitionsCSV: String?
    public var fwDir: URL?
    /// Extra named bins (e.g. content, index) written at their partition
    /// offsets when the partition table names them.
    public var extraBins: [(label: String, url: URL)]
    public var chipFamily: ChipFamily
    public var flashSizeBytes: UInt64
    /// Bootloader size-field encoding (high nibble of byte 3); nil = keep.
    public var flashSizeBits: UInt8?
    /// Skip bootloader and partition table; write only the app.
    public var appOnly: Bool

    public init(app: URL?, bootloader: URL?, partitionsCSV: String?, fwDir: URL?,
                extraBins: [(label: String, url: URL)] = [],
                chipFamily: ChipFamily, flashSizeBytes: UInt64, flashSizeBits: UInt8?,
                appOnly: Bool = false) {
        self.app = app
        self.bootloader = bootloader
        self.partitionsCSV = partitionsCSV
        self.fwDir = fwDir
        self.extraBins = extraBins
        self.chipFamily = chipFamily
        self.flashSizeBytes = flashSizeBytes
        self.flashSizeBits = flashSizeBits
        self.appOnly = appOnly
    }
}

public struct BuildResult {
    public var segments: [FlashSegmentData]
    public var partitions: [Partition]
    public var merged: Data
    public var endAddress: UInt64
    public var flashSizeBytes: UInt64
    public var warnings: [String]

    /// Largest flash offset touched + 1.
    public var fitsFlash: Bool { endAddress <= flashSizeBytes }
}

/// Turns a chip + flash size + firmware artifacts into the exact flash plan:
/// right bootloader offset, right partition table (generated or from CSV),
/// patched bootloader flash-size byte, and a merged image.
public enum ImageBuilder {
    public static func discoverFirmwareDir(_ url: URL) -> FirmwareDirContents {
        let fm = FileManager.default
        let pioBoot = url.appendingPathComponent("bootloader.bin")
        let pioParts = url.appendingPathComponent("partitions.bin")
        let pioApp = url.appendingPathComponent("firmware.bin")
        let idfBoot = url.appendingPathComponent("bootloader").appendingPathComponent("bootloader.bin")
        let idfParts = url.appendingPathComponent("partition_table").appendingPathComponent("partition-table.bin")
        let idfApp = url.appendingPathComponent("pocketwiki.bin")

        var bootloader: URL?
        var partitionTable: URL?
        var app: URL?
        if fm.fileExists(atPath: pioApp.path) {
            bootloader = pioBoot
            partitionTable = pioParts
            app = pioApp
        } else if fm.fileExists(atPath: idfApp.path) {
            bootloader = idfBoot
            partitionTable = idfParts
            app = idfApp
        }
        var content: [URL] = []
        for name in ["content.bin", "index.bin"] {
            let u = url.appendingPathComponent("content").appendingPathComponent(name)
            if fm.fileExists(atPath: u.path) { content.append(u) }
        }
        return FirmwareDirContents(bootloader: bootloader, partitionTable: partitionTable, app: app, content: content)
    }

    public static func build(_ options: BuildOptions) throws -> BuildResult {
        var warnings: [String] = []
        let fm = FileManager.default
        let discovered = options.fwDir.map(discoverFirmwareDir) ?? FirmwareDirContents(bootloader: nil, partitionTable: nil, app: nil, content: [])

        func read(_ url: URL?, purpose: String) throws -> Data? {
            guard let url else { return nil }
            guard fm.fileExists(atPath: url.path) else {
                throw FlashError("missing \(purpose) file: \(url.path)")
            }
            return try Data(contentsOf: url)
        }

        // App image.
        let appURL = options.app ?? discovered.app
        let appData = try read(appURL, purpose: "app image")
        guard let appData else {
            throw FlashError("no app image given (--app) and none found in --fw-dir")
        }
        let appImage = try FirmwareImage.validateApp(appData, for: options.chipFamily)
        if let minRev = appImage.minRev, minRev > 0 {
            warnings.append("app image requires chip revision \(minRev)+ (target-specific check applies at flash time)")
        }

        // Partition layout.
        var partitions: [Partition]
        var partitionTableData: Data?
        if let csv = options.partitionsCSV {
            partitions = try PartitionTable.parseCSV(csv)
            partitionTableData = PartitionTable.encode(partitions)
        } else if let partBin = try read(discovered.partitionTable, purpose: "partition table") {
            partitionTableData = partBin
            partitions = try PartitionTable.decode(partBin)
        } else {
            partitions = try PartitionTable.generateDefault(flashSize: options.flashSizeBytes, appSize: UInt64(appData.count))
            partitionTableData = PartitionTable.encode(partitions)
        }

        // The partition table itself may contain entries beyond the physical
        // flash even when the small set of files we are about to write does
        // not. Reject that combination before esptool writes an image that
        // can boot-loop while trying to validate the table.
        for partition in partitions {
            let end = UInt64(partition.offset) + UInt64(partition.size)
            guard end <= options.flashSizeBytes else {
                throw FlashError("partition '\(partition.name)' ends at 0x\(String(format: "%X", end)) "
                                 + "but the detected flash is only \(options.flashSizeBytes / (1 << 20)) MB")
            }
        }

        guard let factory = PartitionTable.findApp(partitions) else {
            throw FlashError("partition layout has no factory app partition")
        }
        guard UInt64(appData.count) <= UInt64(factory.size) else {
            throw FlashError("app image (\(appData.count) bytes) exceeds factory partition size (\(factory.size) bytes); shrink the app or repartition")
        }

        var segments: [FlashSegmentData] = []

        if !options.appOnly {
            // Bootloader at the chip's offset, flash-size byte patched.
            if let bootURL = options.bootloader ?? discovered.bootloader {
                let bootData = try read(bootURL, purpose: "bootloader")
                let patched = FirmwareImage.patchBootloaderFlashSize(bootData!, sizeBits: options.flashSizeBits)
                if patched != bootData {
                    warnings.append("bootloader flash-size byte patched to match \(options.flashSizeBytes / 1024 / 1024) MB flash")
                }
                let end = UInt64(options.chipFamily.bootloaderFlashOffset) + UInt64(patched.count)
                guard end <= 0x8000 else {
                    throw FlashError("bootloader (\(patched.count) bytes) does not fit below the partition table (0x8000)")
                }
                segments.append(FlashSegmentData(label: "bootloader", offset: options.chipFamily.bootloaderFlashOffset, data: patched))
            } else {
                warnings.append("no bootloader found; partition table + app will be flashed assuming a compatible bootloader already exists")
            }

            // Partition table at 0x8000.
            if let partitionTableData {
                segments.append(FlashSegmentData(label: "partition-table", offset: 0x8000, data: partitionTableData))
            }
        } else {
            warnings.append("app-only mode: bootloader and partition table left untouched")
        }

        // App at the factory offset.
        segments.append(FlashSegmentData(label: "factory", offset: factory.offset, data: appData))

        // Extra bins (content, index, ...) at their partition offsets.
        // Skipped in app-only mode, which touches nothing but the app.
        if !options.appOnly {
            let extraURLs = options.extraBins + discovered.content.map { (label: $0.lastPathComponent.replacingOccurrences(of: ".bin", with: ""), url: $0) }
            for (label, url) in extraURLs {
                guard let part = PartitionTable.find(partitions, name: label) else {
                    warnings.append("skipping \(url.path): no partition named '\(label)' in the layout")
                    continue
                }
                let data = try read(url, purpose: "\(label) image")
                guard let data else { continue }
                guard UInt64(data.count) <= UInt64(part.size) else {
                    throw FlashError("\(label).bin (\(data.count) bytes) exceeds its partition size (\(part.size) bytes)")
                }
                segments.append(FlashSegmentData(label: label, offset: part.offset, data: data))
            }
        }

        // Sorted, overlap-checked; merge with 0xFF fill.
        let sorted = segments.sorted { $0.offset < $1.offset }
        for i in 1..<sorted.count {
            let prevEnd = UInt64(sorted[i - 1].offset) + UInt64(sorted[i - 1].data.count)
            guard prevEnd <= UInt64(sorted[i].offset) else {
                throw FlashError("flash plan overlaps: \(sorted[i - 1].label) ends at 0x\(String(format: "%X", prevEnd)), \(sorted[i].label) starts at 0x\(hex4(sorted[i].offset))")
            }
        }

        let endAddress = sorted.reduce(UInt64(0)) { max($0, UInt64($1.offset) + UInt64($1.data.count)) }
        var merged = Data(repeating: 0xFF, count: Int(endAddress))
        for seg in sorted {
            let range = Int(seg.offset)..<(Int(seg.offset) + seg.data.count)
            merged.replaceSubrange(range, with: seg.data)
        }

        let result = BuildResult(segments: sorted, partitions: partitions, merged: merged,
                                 endAddress: endAddress, flashSizeBytes: options.flashSizeBytes, warnings: warnings)
        if !result.fitsFlash {
            throw FlashError("flash plan ends at 0x\(String(format: "%X", endAddress)) but the flash is only \(options.flashSizeBytes) bytes — pick a smaller layout or larger flash")
        }
        return result
    }

    /// Render a plan as aligned text lines (for CLI/GUI display).
    public static func describe(_ result: BuildResult) -> String {
        var lines: [String] = []
        lines.append("Flash plan (\(result.flashSizeBytes / 1024 / 1024) MB flash, ends at 0x\(String(format: "%X", result.endAddress))):")
        lines.append("  offset     size      label")
        for seg in result.segments {
            lines.append(String(format: "  0x%08X  %8d  %@", seg.offset, seg.data.count, seg.label))
        }
        if !result.warnings.isEmpty {
            lines.append("Warnings:")
            for w in result.warnings { lines.append("  - \(w)") }
        }
        return lines.joined(separator: "\n")
    }
}
