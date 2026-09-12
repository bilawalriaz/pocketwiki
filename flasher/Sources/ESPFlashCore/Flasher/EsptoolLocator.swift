import Foundation

/// Finds a Python interpreter with the `esptool` module installed, mirroring
/// the repo's `tools/flash_all.py::find_idf_python`. We delegate the actual
/// flash write to esptool (battle-tested for USB-Serial/JTAG), keeping this
/// tool's own protocol code for detection and image preparation.
public enum EsptoolLocator {
    /// Path to a Python that can `import esptool`, or nil.
    public static func findPython() -> String? {
        var candidates: [String] = []
        if let env = ProcessInfo.processInfo.environment["IDF_PYTHON_ENV_PATH"] {
            candidates.append("\(env)/bin/python")
        }
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        if let espressif = try? FileManager.default.contentsOfDirectory(atPath: "\(home)/.espressif/python_env") {
            candidates += espressif.sorted().map { "\(home)/.espressif/python_env/\($0)/bin/python" }
        }
        candidates.append("\(home)/.platformio/penv/bin/python")
        candidates.append("python3")

        for candidate in candidates {
            guard FileManager.default.fileExists(atPath: candidate) || candidate == "python3" else { continue }
            if hasEsptool(candidate) {
                return candidate
            }
        }
        return nil
    }

    private static func hasEsptool(_ python: String) -> Bool {
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: python)
        proc.arguments = ["-c", "import esptool"]
        proc.standardOutput = Pipe()
        proc.standardError = Pipe()
        do {
            try proc.run()
            proc.waitUntilExit()
            return proc.terminationStatus == 0
        } catch {
            return false
        }
    }
}
