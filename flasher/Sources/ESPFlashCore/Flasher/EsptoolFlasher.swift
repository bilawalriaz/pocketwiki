import Foundation

/// Flashes prepared segments by invoking the standard `esptool` (the same
/// tool the repo's `flash_all.py` uses). esptool handles reset, stub upload,
/// the USB-Serial/JTAG path and verification, which is far more reliable than
/// a hand-rolled ROM writer. We keep the protocol code only for detection.
public final class EsptoolFlasher {
    public var onLog: (String) -> Void = { _ in }
    public var onProgress: (Double) -> Void = { _ in }

    public init() {}

    public func flash(chip: ChipFamily, port: String, baud: Int = 115200,
                      segments: [FlashSegmentData], verify: Bool = true,
                      eraseAll: Bool = false) throws {
        guard let python = EsptoolLocator.findPython() else {
            throw FlashError("esptool not found — install it (`pip install esptool`) or set IDF_PYTHON_ENV_PATH")
        }
        guard !segments.isEmpty else { throw FlashError("nothing to flash") }
        guard chip != .unknown else { throw FlashError("unknown chip; pass --chip explicitly") }

        let fm = FileManager.default
        let dir = fm.temporaryDirectory.appendingPathComponent("espflasher-\(UUID().uuidString)")
        try fm.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: dir) }

        var args = ["-m", "esptool", "--chip", chip.esptoolName, "--port", port, "--baud", String(baud)]
        args += ["write_flash"]
        if eraseAll { args += ["--erase-all"] }
        if verify { args += ["--verify"] }

        let sorted = segments.sorted { $0.offset < $1.offset }
        for (i, seg) in sorted.enumerated() {
            let file = dir.appendingPathComponent("seg\(i).bin")
            try seg.data.write(to: file)
            args += [String(format: "0x%x", seg.offset), file.path]
            onLog("  will flash \(seg.label) (0x\(hex4(seg.offset)), \(seg.data.count) bytes)")
        }

        onLog("running: \(python) -m esptool ... write_flash")
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: python)
        proc.arguments = args
        let pipe = Pipe()
        proc.standardOutput = pipe
        proc.standardError = pipe

        let readGroup = DispatchGroup()
        readGroup.enter()
        let readHandle = pipe.fileHandleForReading
        readHandle.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard let self else { return }
            if data.isEmpty {
                handle.readabilityHandler = nil
                readGroup.leave()
                return
            }
            self.consume(data, segments: sorted)
        }

        do {
            try proc.run()
        } catch {
            throw FlashError("failed to launch esptool: \(error)")
        }
        proc.waitUntilExit()
        readGroup.wait()
        readHandle.closeFile()

        guard proc.terminationStatus == 0 else {
            throw FlashError("esptool failed (exit code \(proc.terminationStatus))")
        }
        onLog("esptool completed successfully")
    }

    private func consume(_ data: Data, segments: [FlashSegmentData]) {
        guard let text = String(data: data, encoding: .utf8) else { return }
        for line in text.components(separatedBy: CharacterSet(charactersIn: "\r\n")) {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.isEmpty { continue }
            // esptool progress: "\rWriting at 0x00124450... (42%)"
            if let progress = parseProgress(trimmed, segments: segments) {
                onProgress(progress)
                continue
            }
            // Swallow the noisy per-block "Writing at 0x..." spam but surface
            // real status lines.
            if trimmed.contains("Writing at 0x") { continue }
            onLog(trimmed)
        }
    }

    private func parseProgress(_ line: String, segments: [FlashSegmentData]) -> Double? {
        guard let range = line.range(of: "Writing at 0x") else { return nil }
        let rest = line[range.upperBound...]
        guard let hexPart = rest.split(separator: " ").first,
              let offset = UInt32(hexPart, radix: 16),
              let pctOpen = rest.range(of: "(") else { return nil }
        let pctStr = rest[pctOpen.upperBound...].prefix { $0.isNumber }
        guard let pct = Int(pctStr), pct >= 0, pct <= 100, !segments.isEmpty else { return nil }
        guard let idx = segments.firstIndex(where: { $0.offset == offset }) else { return nil }
        return (Double(idx) + Double(pct) / 100.0) / Double(segments.count)
    }
}
