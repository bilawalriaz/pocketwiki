import Foundation

// ============================================================================
// Firmware dist enumeration.
//
// The repo keeps ready-to-flash artifacts in `firmware/dist/<chip-variant>/`
// (firmware.bin, bootloader.bin, partitions.bin/csv, content/*). This module
// locates that folder without user input and turns it into selectable
// variants the GUI shows with presence ticks, so flashing needs zero file
// picking.
// ============================================================================

/// A single named artifact a firmware variant may or may not ship with.
struct FirmwareArtifact: Identifiable, Equatable {
    let id: String
    let label: String
    let present: Bool
    let detail: String?
}

/// One chip variant in the dist folder (e.g. "esp32-s3").
struct FirmwareVariant: Identifiable, Equatable {
    let name: String
    let directory: URL
    let app: URL?
    let bootloader: URL?
    let partitionsBin: URL?
    let partitionsCSV: URL?
    let content: [URL]

    var id: String { name }

    var hasApp: Bool { app != nil }
    var hasBootloader: Bool { bootloader != nil }
    var hasPartitions: Bool { partitionsCSV != nil || partitionsBin != nil }
    var hasContent: Bool { !content.isEmpty }

    /// Ordered presence list for the tick row in the UI.
    var artifacts: [FirmwareArtifact] {
        [
            FirmwareArtifact(id: "app", label: "App",
                             present: hasApp,
                             detail: app.map(FirmwareDistScanner.sizeText)),
            FirmwareArtifact(id: "bootloader", label: "Bootloader",
                             present: hasBootloader,
                             detail: bootloader.map(FirmwareDistScanner.sizeText)),
            FirmwareArtifact(id: "partitions", label: "Partitions",
                             present: hasPartitions,
                             detail: partitionsCSV != nil ? "csv" : (partitionsBin != nil ? "bin" : nil)),
            FirmwareArtifact(id: "content", label: "Content",
                             present: hasContent,
                             detail: content.isEmpty ? nil : "\(content.count) file\(content.count == 1 ? "" : "s")"),
        ]
    }

    var appSizeText: String? { app.map(FirmwareDistScanner.sizeText) }
}

enum FirmwareDistScanner {
    /// The dist root, found without user input: walk up from the working
    /// directory looking for `firmware/dist`, then look for a checkout sitting
    /// directly in the home directory. No fixed project name is assumed, so a
    /// clone under any name is found.
    static func locateDefaultRoot() -> URL? {
        let fm = FileManager.default
        var cwd = URL(fileURLWithPath: fm.currentDirectoryPath)
        for _ in 0..<6 {
            let candidate = cwd.appendingPathComponent("firmware").appendingPathComponent("dist")
            if fm.fileExists(atPath: candidate.path) { return candidate }
            if cwd.path == "/" { break }
            cwd.deleteLastPathComponent()
        }
        let home = fm.homeDirectoryForCurrentUser
        let entries = (try? fm.contentsOfDirectory(
            at: home, includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles])) ?? []
        for entry in entries.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
            let candidate = entry.appendingPathComponent("firmware").appendingPathComponent("dist")
            if fm.fileExists(atPath: candidate.path) { return candidate }
        }
        return nil
    }

    /// Variants = subdirectories of the root that contain a `firmware.bin`.
    static func scan(root: URL) -> [FirmwareVariant] {
        let fm = FileManager.default
        guard let entries = try? fm.contentsOfDirectory(
            at: root,
            includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles]
        ) else { return [] }

        var out: [FirmwareVariant] = []
        for dir in entries.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
            let isDir = (try? dir.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) ?? false
            guard isDir else { continue }
            let app = dir.appendingPathComponent("firmware.bin")
            guard fm.fileExists(atPath: app.path) else { continue }

            let bootloader = dir.appendingPathComponent("bootloader.bin")
            let partitionsBin = dir.appendingPathComponent("partitions.bin")
            let partitionsCSV = dir.appendingPathComponent("partitions.csv")
            let contentDir = dir.appendingPathComponent("content")
            let contentNames = (try? fm.contentsOfDirectory(atPath: contentDir.path))?
                .filter { !$0.hasPrefix(".") }
                .sorted() ?? []
            let content = contentNames.map { contentDir.appendingPathComponent($0) }

            out.append(FirmwareVariant(
                name: dir.lastPathComponent,
                directory: dir,
                app: fm.fileExists(atPath: app.path) ? app : nil,
                bootloader: fm.fileExists(atPath: bootloader.path) ? bootloader : nil,
                partitionsBin: fm.fileExists(atPath: partitionsBin.path) ? partitionsBin : nil,
                partitionsCSV: fm.fileExists(atPath: partitionsCSV.path) ? partitionsCSV : nil,
                content: content
            ))
        }
        return out
    }

    /// Case/punctuation-insensitive chip-key comparison:
    /// "esp32-s3" == "esp32s3".
    static func normalize(_ name: String) -> String {
        name.lowercased().filter { $0.isLetter || $0.isNumber }
    }

    static func sizeText(_ url: URL) -> String {
        let attrs = try? url.resourceValues(forKeys: [.fileSizeKey])
        guard let size = attrs?.fileSize else { return "—" }
        let formatter = ByteCountFormatter()
        formatter.countStyle = .file
        return formatter.string(fromByteCount: Int64(size))
    }
}
