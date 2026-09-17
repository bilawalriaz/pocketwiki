import Foundation
import ArgumentParser
import ESPFlashCore

@main
struct ESPFlashCLI: ParsableCommand {
    static let configuration = CommandConfiguration(
        commandName: "espflasher",
        abstract: "Detect, prepare and flash ESP32-family devices (ESP32, ESP32-C3, ESP32-S3).",
        version: "0.1.0",
        subcommands: [ListPorts.self, Detect.self, Flash.self, Image.self, Partitions.self]
    )
}

// MARK: - Shared helpers

func stderr(_ text: String) {
    FileHandle.standardError.write(Data(text.utf8))
}

func printJSON<T: Encodable>(_ value: T) throws {
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
    let data = try encoder.encode(value)
    print(String(decoding: data, as: UTF8.self))
}

final class ProgressPrinter {
    private var drawing = false

    func update(_ p: FlashProgress) {
        switch p.phase {
        case .writing(_, let done, let total):
            let pct = total > 0 ? Int(Double(done) * 100 / Double(total)) : 0
            stderr("\r[\(String(format: "%3d", pct))%] \(p.message)")
            drawing = true
        default:
            stderr((drawing ? "\n" : "") + p.message + "\n")
            drawing = false
        }
    }
}

func renderDevice(_ device: DeviceInfo, port: SerialPortInfo?) -> String {
    var lines: [String] = []
    if let port { lines.append("Port:      \(port.path)") }
    lines.append("Chip:      \(device.chip.displayName)")
    if let pkg = device.chip.package { lines.append("Package:   \(pkg)") }
    if let rev = device.chip.revisionText { lines.append("Revision:  \(rev)") }
    if let mac = device.chip.macAddress { lines.append("MAC:       \(mac)") }
    if let embedded = device.chip.efuseEmbeddedFlash { lines.append("eFuse:     embedded flash \(embedded)\(device.chip.flashVendorEfuse.map { " (\($0))" } ?? "")") }
    if let mode = device.chip.usbMode { lines.append("USB mode:  \(mode)") }
    if let flash = device.flashSizeText {
        lines.append("Flash:     \(flash) (via \(device.flashSizeSource ?? "?"))\(device.flashManufacturer.map { ", \($0)" } ?? "")")
    } else if let id = device.flashID {
        lines.append("Flash:     unknown size (id 0x\(String(format: "%06X", id)))")
    }
    if let id = device.flashID {
        lines.append("Flash ID:  0x\(String(format: "%06X", id))")
    }
    return lines.joined(separator: "\n")
}

// MARK: - list

struct ListPorts: ParsableCommand {
    static let configuration = CommandConfiguration(
        commandName: "list",
        abstract: "List serial ports with USB info where available."
    )

    @Flag(name: .shortAndLong, help: "Emit JSON")
    var json = false

    func run() throws {
        let ports = SerialPortScanner.all()
        if json {
            try printJSON(ports)
            return
        }
        for p in ports {
            var line = p.path.padding(toLength: 28, withPad: " ", startingAt: 0)
            line += p.name.padding(toLength: 24, withPad: " ", startingAt: 0)
            if let vid = p.vendorID, let pid = p.productID {
                line += String(format: " %04x:%04x", vid, pid)
                if let serial = p.serialNumber { line += "  \(serial)" }
            }
            print(line)
        }
    }
}

// MARK: - detect

struct Detect: ParsableCommand {
    static let configuration = CommandConfiguration(
        commandName: "detect",
        abstract: "Detect an ESP32, its variant (C3/S3/...), revision and flash size."
    )

    @Option(name: .shortAndLong, help: "Serial port path (default: scan all ports)")
    var port: String?

    @Option(name: .shortAndLong, help: "Connect attempts on an explicit port")
    var attempts = 7

    @Flag(name: .shortAndLong, help: "Emit JSON")
    var json = false

    func run() throws {
        let (detector, device, info) = try detectDevice(port: port, explicitAttempts: attempts, scanAttempts: 3, onLog: { stderr($0 + "\n") })
        _ = detector
        if json {
            try printJSON(DetectOutput(port: info, device: device))
        } else {
            print(renderDevice(device, port: info))
        }
    }
}

// MARK: - flash

struct Flash: ParsableCommand {
    static let configuration = CommandConfiguration(
        commandName: "flash",
        abstract: "Prepare the right image (partitions, bootloader, offsets) and flash it."
    )

    @Option(name: .shortAndLong, help: "Serial port path (default: scan all ports)")
    var port: String?

    @Option(name: .customLong("app"), help: "App image (firmware.bin / pocketwiki.bin)")
    var app: String?

    @Option(name: .customLong("bootloader"), help: "Bootloader image (optional; discovered from --fw-dir)")
    var bootloader: String?

    @Option(name: .customLong("partitions"), help: "Partition layout CSV (optional; defaults to the fw-dir table or a generated layout)")
    var partitions: String?

    @Option(name: .customLong("fw-dir"), help: "Firmware build directory (firmware/dist/<env>, PlatformIO .pio/build/<env>, or ESP-IDF build)")
    var fwDir: String?

    @Option(name: .customLong("chip"), help: "Chip family: auto, esp32, esp32c3, esp32s3")
    var chip = "auto"

    @Option(name: .customLong("flash-size"), help: "Flash size: auto, 1MB, 2MB, 4MB, 8MB, 16MB")
    var flashSize = "auto"

    @Option(name: .customLong("baud"), help: "Line rate (115200 is the reliable ROM maximum)")
    var baud = 115200

    @Option(name: .customLong("out"), help: "Also write the merged image to this file")
    var out: String?

    @Flag(name: .customLong("app-only"), help: "Flash only the app partition (keep bootloader + partition table)")
    var appOnly = false

    @Flag(name: .customLong("no-verify"), help: "Skip esptool's verification after writing")
    var noVerify = false

    @Flag(name: .customLong("erase-all"), help: "Erase the whole flash before writing")
    var eraseAll = false

    func run() throws {
        let (detector, device, info) = try detectDevice(port: port, explicitAttempts: 7, scanAttempts: 3, onLog: { stderr($0 + "\n") })
        stderr(renderDevice(device, port: info) + "\n")

        var family = device.chip.family
        if chip != "auto" {
            let requested = try parseChipFamily(chip)
            guard family == .unknown || requested == family else {
                throw FlashError("chip mismatch: detected \(family.rawValue) but --chip \(requested.rawValue) was given")
            }
            family = requested
        }
        guard family != .unknown else {
            throw FlashError("could not identify the chip; pass --chip esp32/esp32c3/esp32s3 explicitly")
        }

        var sizeBytes = device.flashSizeBytes
        var explicitSize: FlashSize?
        if flashSize != "auto" {
            let requested = try parseFlashSize(flashSize)
            explicitSize = requested
            if let physical = sizeBytes, requested.bytes > physical {
                stderr("warning: --flash-size \(requested.rawValue) is larger than the detected \(physical / 1024 / 1024) MB flash\n")
            }
            sizeBytes = requested.bytes
        }
        if sizeBytes == nil {
            stderr("warning: flash size unknown, assuming 4MB\n")
            sizeBytes = 4 << 20
        }

        let csvText: String?
        if let partitions {
            guard let text = try? String(contentsOfFile: partitions, encoding: .utf8) else {
                throw FlashError("cannot read partitions CSV: \(partitions)")
            }
            csvText = text
        } else {
            csvText = nil
        }

        let options = BuildOptions(
            app: app.map { URL(fileURLWithPath: $0) },
            bootloader: bootloader.map { URL(fileURLWithPath: $0) },
            partitionsCSV: csvText,
            fwDir: fwDir.map { URL(fileURLWithPath: $0) },
            chipFamily: family,
            flashSizeBytes: sizeBytes!,
            // Patch the bootloader flash-size byte only when the user chose
            // an explicit size; "auto" keeps the compiled value (esptool
            // default), which is what the firmware was built for.
            flashSizeBits: explicitSize?.sizeBits,
            appOnly: appOnly
        )
        let result = try ImageBuilder.build(options)
        stderr(ImageBuilder.describe(result) + "\n")
        for warning in result.warnings { stderr("warning: \(warning)\n") }

        if let out {
            try result.merged.write(to: URL(fileURLWithPath: out))
            stderr("merged image: \(out) (\(result.merged.count) bytes)\n")
        }

        let printer = ProgressPrinter()
        let flasher = EsptoolFlasher()
        flasher.onLog = { printer.update(FlashProgress(phase: .resetting, message: $0)) }
        flasher.onProgress = { pct in
            printer.update(FlashProgress(phase: .writing(offset: 0, bytesDone: UInt64(pct * 100), bytesTotal: 100),
                                         message: String(format: "overall %3d%%", Int(pct * 100))))
        }
        detector.close() // release our fd; esptool opens its own connection
        try flasher.flash(chip: family, port: info?.path ?? port ?? "",
                          baud: baud, segments: result.segments, verify: !noVerify, eraseAll: eraseAll)
    }
}

// MARK: - image

struct Image: ParsableCommand {
    static let configuration = CommandConfiguration(
        commandName: "image",
        abstract: "Build a merged flash image offline (no device required)."
    )

    @Option(name: .customLong("chip"), help: "Chip family: esp32, esp32c3, esp32s3")
    var chip: String

    @Option(name: .customLong("flash-size"), help: "Flash size: 1MB, 2MB, 4MB, 8MB, 16MB")
    var flashSize: String

    @Option(name: .customLong("app"), help: "App image (firmware.bin / pocketwiki.bin)")
    var app: String?

    @Option(name: .customLong("bootloader"), help: "Bootloader image (optional; discovered from --fw-dir)")
    var bootloader: String?

    @Option(name: .customLong("partitions"), help: "Partition layout CSV (optional)")
    var partitions: String?

    @Option(name: .customLong("fw-dir"), help: "Firmware build directory (discovery for app/bootloader/partition table/content)")
    var fwDir: String?

    @Option(name: .customLong("out"), help: "Write the merged image here")
    var out: String

    @Flag(name: .customLong("app-only"), help: "Only the app partition")
    var appOnly = false

    func run() throws {
        let family = try parseChipFamily(chip)
        let size = try parseFlashSize(flashSize)
        let csvText: String?
        if let partitions {
            guard let text = try? String(contentsOfFile: partitions, encoding: .utf8) else {
                throw FlashError("cannot read partitions CSV: \(partitions)")
            }
            csvText = text
        } else {
            csvText = nil
        }
        let options = BuildOptions(
            app: app.map { URL(fileURLWithPath: $0) },
            bootloader: bootloader.map { URL(fileURLWithPath: $0) },
            partitionsCSV: csvText,
            fwDir: fwDir.map { URL(fileURLWithPath: $0) },
            chipFamily: family,
            flashSizeBytes: size.bytes,
            flashSizeBits: size.sizeBits,
            appOnly: appOnly
        )
        let result = try ImageBuilder.build(options)
        stderr(ImageBuilder.describe(result) + "\n")
        try result.merged.write(to: URL(fileURLWithPath: out))
        print("Wrote \(result.merged.count) bytes to \(out)")
    }
}

// MARK: - partitions

struct Partitions: ParsableCommand {
    static let configuration = CommandConfiguration(
        commandName: "partitions",
        abstract: "Generate or convert a partition table."
    )

    @Option(name: .customLong("chip"), help: "Chip family (default layout only): esp32, esp32c3, esp32s3")
    var chip = "esp32c3"

    @Option(name: .customLong("flash-size"), help: "Flash size for the generated layout (default 4MB)")
    var flashSize = "4MB"

    @Option(name: .customLong("csv"), help: "Convert this CSV to binary (optional)")
    var csv: String?

    @Option(name: .customLong("out"), help: "Write the binary table here (else print CSV)")
    var out: String?

    func run() throws {
        _ = try parseChipFamily(chip)
        let size = try parseFlashSize(flashSize)
        var partitions: [Partition]
        if let csv {
            guard let text = try? String(contentsOfFile: csv, encoding: .utf8) else {
                throw FlashError("cannot read partitions CSV: \(csv)")
            }
            partitions = try PartitionTable.parseCSV(text)
        } else {
            partitions = try PartitionTable.generateDefault(flashSize: size.bytes)
        }
        if let out {
            let data = PartitionTable.encode(partitions)
            try data.write(to: URL(fileURLWithPath: out))
            print("Wrote partition table (\(data.count) bytes, \(partitions.count) partitions) to \(out)")
        } else {
            print(PartitionTable.formatCSV(partitions))
        }
    }
}
