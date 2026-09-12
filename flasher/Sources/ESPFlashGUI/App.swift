import SwiftUI
import ESPFlashCore

// ============================================================================
// PocketWiki Flasher — native macOS SwiftUI surface.
//
// THESIS:        A calm field bench for a hardware tool — warm paper, one
//                forest-green signal, softly squared flat cards — so the
//                board, not the chrome, is what you look at.
// OWN-WORLD:     PocketWiki design system (DESIGN.md): paper canvas, Clean
//                Leaf cards with hairline borders, forest green as the only
//                accent, SF Symbols in tinted squares, native controls,
//                semantic colors that adapt to dark mode.
// COMPOSITION:   Brand header with live device pill -> Device card (waiting /
//                detecting / connected spec-grid screens) -> Firmware card
//                (auto-enumerated dist variants with presence ticks) ->
//                flash progress strip -> action bar (status left, Flash
//                right) -> console accordion (hidden by default).
// BEHAVIOR:      The model watches the serial bus — plugging a board in is
//                found and identified automatically, and the dist variant
//                matching the chip is auto-selected. Dark mode is first-class.
// ============================================================================

@main
struct ESPFlashGUIApp: App {
    var body: some Scene {
        WindowGroup("PocketWiki Flasher") {
            ContentView()
        }
        .defaultSize(width: 840, height: 880)
    }
}

// MARK: - Design tokens (PocketWiki system)

extension Color {
    /// Brand green — the one accent. Light: Pocket Forest. Dark: Night Mint.
    static let pocketAccent = Color(nsColor: NSColor(name: nil) { appearance in
        let dark = appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
        return dark
            ? NSColor(red: 0x79 / 255, green: 0xd4 / 255, blue: 0xae / 255, alpha: 1)
            : NSColor(red: 0x17 / 255, green: 0x6b / 255, blue: 0x4d / 255, alpha: 1)
    })
    static let pocketForest = Color(red: 0x17 / 255, green: 0x6b / 255, blue: 0x4d / 255)
    static let pocketForestDeep = Color(red: 0x0f / 255, green: 0x51 / 255, blue: 0x39 / 255)
    /// Mist Green — quiet hover fills; translucent forest on dark.
    static let pocketForestSoft = Color(nsColor: NSColor(name: nil) { appearance in
        let dark = appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
        return dark
            ? NSColor(red: 0x17 / 255, green: 0x6b / 255, blue: 0x4d / 255, alpha: 0.30)
            : NSColor(red: 0xe6 / 255, green: 0xf2 / 255, blue: 0xed / 255, alpha: 1)
    })
    /// Warm Paper — the window canvas.
    static let pocketPaper = Color(nsColor: NSColor(name: nil) { appearance in
        let dark = appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
        return dark
            ? NSColor(red: 0x10 / 255, green: 0x13 / 255, blue: 0x12 / 255, alpha: 1)
            : NSColor(red: 0xf5 / 255, green: 0xf7 / 255, blue: 0xf6 / 255, alpha: 1)
    })
    /// Clean Leaf — card fills.
    static let pocketSurface = Color(nsColor: NSColor(name: nil) { appearance in
        let dark = appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
        return dark
            ? NSColor(red: 0x1a / 255, green: 0x1e / 255, blue: 0x1c / 255, alpha: 1)
            : NSColor(red: 1.0, green: 1.0, blue: 1.0, alpha: 1)
    })
    /// Soft Rule — hairline card borders.
    static let pocketLine = Color(nsColor: NSColor(name: nil) { appearance in
        let dark = appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
        return dark
            ? NSColor(red: 0x2c / 255, green: 0x33 / 255, blue: 0x2f / 255, alpha: 1)
            : NSColor(red: 0xdb / 255, green: 0xe2 / 255, blue: 0xde / 255, alpha: 1)
    })
    /// Deep Ink — primary text.
    static let pocketInk = Color(nsColor: NSColor(name: nil) { appearance in
        let dark = appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
        return dark
            ? NSColor(red: 0xe2 / 255, green: 0xe9 / 255, blue: 0xe5 / 255, alpha: 1)
            : NSColor(red: 0x17 / 255, green: 0x20 / 255, blue: 0x1c / 255, alpha: 1)
    })
    /// Weathered Ink — explanations and metadata.
    static let pocketInkMuted = Color(nsColor: NSColor(name: nil) { appearance in
        let dark = appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
        return dark
            ? NSColor(red: 0xba / 255, green: 0xc5 / 255, blue: 0xbf / 255, alpha: 1)
            : NSColor(red: 0x65 / 255, green: 0x71 / 255, blue: 0x6b / 255, alpha: 1)
    })
    /// Success signal — forest in light, Night Mint on dark.
    static let pocketSuccess = Color(nsColor: NSColor(name: nil) { appearance in
        let dark = appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
        return dark
            ? NSColor(red: 0x79 / 255, green: 0xd4 / 255, blue: 0xae / 255, alpha: 1)
            : NSColor(red: 0x17 / 255, green: 0x6b / 255, blue: 0x4d / 255, alpha: 1)
    })
    /// Danger — muted red, distinct from the green system.
    static let pocketDanger = Color(nsColor: NSColor(name: nil) { appearance in
        let dark = appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
        return dark
            ? NSColor(red: 0xe0 / 255, green: 0x8d / 255, blue: 0x80 / 255, alpha: 1)
            : NSColor(red: 0x8a / 255, green: 0x2c / 255, blue: 0x21 / 255, alpha: 1)
    })
    /// Console — a dark terminal well in both appearances.
    static let consoleSurface = Color(red: 0x0a / 255, green: 0x0d / 255, blue: 0x0c / 255)
    static let consoleInk = Color(red: 0xd6 / 255, green: 0xde / 255, blue: 0xd9 / 255)
    static let consoleInkMuted = Color(red: 0x6f / 255, green: 0x7a / 255, blue: 0x74 / 255)
}

// MARK: - Model

/// Device-card screens: which board-acquisition state the UI is in.
enum BoardPhase: Equatable {
    case waiting
    case detecting(port: String)
    case connected
    case failed(String)
}

@MainActor
final class FlasherModel: ObservableObject {
    @Published var ports: [SerialPortInfo] = []
    @Published var selectedPort: String?
    @Published var device: DeviceInfo?
    @Published var busy = false
    /// True while writing flash; distinguishes flashing progress from detection.
    @Published private(set) var isFlashing = false
    @Published var progress = 0.0
    @Published var status = ""

    @Published var appPath: String?
    @Published var bootloaderPath: String?
    @Published var partitionsPath: String?
    @Published var fwDir: String?

    /// Board acquisition state machine: waiting → detecting → connected / failed.
    @Published var phase: BoardPhase = .waiting

    /// Firmware dist enumeration (firmware/dist/<chip-variant>/).
    @Published var distRoot: String?
    @Published var variants: [FirmwareVariant] = []
    @Published var selectedVariantName: String?

    private var watchTimer: Timer?
    private var lastPortSet = Set<String>()
    private var connectedMisses = 0
    private var scanTick = 0
    private var hasAttemptedAutoDetect = false

    private var logBuffer = ""
    @Published var logText = ""

    /// Thread-safe log append: safe to call from background flash/detect
    /// threads; hops to the main actor internally.
    nonisolated func log(_ line: String) {
        Task { @MainActor [weak self] in
            self?.logText += line + "\n"
        }
    }

    func clearLog() {
        logText = ""
    }

    // MARK: Firmware dist

    /// USB serial ports only, ESP-priority order — the picker's and the
    /// auto-detect's shared candidate list (no Bluetooth noise).
    var usbPorts: [SerialPortInfo] {
        ports
            .filter(\.isUSB)
            .sorted { Self.portPriority($0) < Self.portPriority($1) }
    }

    var selectedVariant: FirmwareVariant? {
        variants.first { $0.name == selectedVariantName }
    }

    var canFlash: Bool {
        !busy && selectedVariant?.hasApp == true && selectedPort != nil
    }

    func variantMatchesBoard(_ variant: FirmwareVariant) -> Bool {
        guard let family = device?.chip.family else { return false }
        return FirmwareDistScanner.normalize(variant.name) == family.esptoolName
    }

    /// Enumerate firmware/dist (locating it automatically on first use).
    func scanDist() {
        if distRoot == nil {
            distRoot = FirmwareDistScanner.locateDefaultRoot()?.path
        }
        guard let root = distRoot else { return }
        let found = FirmwareDistScanner.scan(root: URL(fileURLWithPath: root))
        let namesChanged = found.map(\.name) != variants.map(\.name)
        variants = found
        if namesChanged {
            if let family = device?.chip.family {
                selectVariant(matching: family)
            } else {
                selectVariant(found.first?.name)
            }
        }
    }

    /// Point the dist root at a user-chosen folder and re-enumerate.
    func setDistRoot(_ path: String?) {
        distRoot = path
        variants = []
        selectedVariantName = nil
        appPath = nil
        bootloaderPath = nil
        partitionsPath = nil
        fwDir = nil
        scanDist()
    }

    /// Pick the variant whose normalized name matches the connected chip
    /// ("esp32-s3" ↔ ESP32-S3); falls back to keeping the current selection.
    func selectVariant(matching family: ChipFamily) {
        if let match = variants.first(where: {
            FirmwareDistScanner.normalize($0.name) == family.esptoolName
        }) {
            selectVariant(match.name)
        } else if device != nil, !variants.isEmpty {
            status = "No firmware for \(family.rawValue) — pick a variant below"
        }
    }

    /// Apply a variant: populate the flash pipeline's file paths from the
    /// dist folder. Partitions come from the CSV only — the binary table is
    /// left for ImageBuilder to pick up from `fwDir`.
    func selectVariant(_ name: String?) {
        guard let name, let variant = variants.first(where: { $0.name == name }) else { return }
        selectedVariantName = name
        appPath = variant.app?.path
        bootloaderPath = variant.bootloader?.path
        partitionsPath = variant.partitionsCSV?.path
        fwDir = variant.directory.path
        status = "Firmware ready: \(name)"
    }

    func refreshPorts() {
        ports = SerialPortScanner.all()
        if selectedPort == nil || !ports.contains(where: { $0.path == selectedPort }) {
            selectedPort = ports.first?.path
        }
    }

    // MARK: Board watch (auto-detect)

    /// Start the once-a-second serial watcher. Call from the surface's
    /// onAppear; the watcher drives waiting → detecting → connected.
    func startWatching() {
        stopWatching()
        let timer = Timer(timeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in
                self?.watchTick()
            }
        }
        RunLoop.main.add(timer, forMode: .common)
        watchTimer = timer
        watchTick()
    }

    func stopWatching() {
        watchTimer?.invalidate()
        watchTimer = nil
    }

    /// One watch beat. Looks for a newly attached USB port, keeps the
    /// connected board stable across its USB re-enumeration, and retries a
    /// failed detection when the port set changes (or every ~10s).
    private func watchTick() {
        scanTick += 1
        let all = SerialPortScanner.all()
        if all != ports { ports = all }
        let currentSet = Set(all.map(\.path))
        let candidates = all
            .filter(\.isUSB) // earbuds / Bluetooth / plain /dev/cu.* ports are not USB
            .sorted { Self.portPriority($0) < Self.portPriority($1) }

        guard !busy else {
            lastPortSet = currentSet
            return
        }

        switch phase {
        case .connected:
            guard let port = selectedPort, currentSet.contains(port) else {
                connectedMisses += 1
                // USB-Serial/JTAG boards vanish for a beat during reset; only
                // give up after the port has been gone for 3 consecutive beats.
                if connectedMisses >= 3 {
                    device = nil
                    selectedPort = nil
                    phase = .waiting
                    connectedMisses = 0
                    hasAttemptedAutoDetect = false
                    status = "Board disconnected"
                    log("board disconnected — waiting for a new one")
                }
                return
            }
            connectedMisses = 0
        case .detecting:
            lastPortSet = currentSet
        case .waiting, .failed:
            let portsChanged = currentSet != lastPortSet
            lastPortSet = currentSet
            guard !candidates.isEmpty else { return }
            // On failure: retry immediately when the port set changes, else
            // on a slow cadence in case the board needed a moment.
            if case .failed = phase, !portsChanged, scanTick % 10 != 0 { return }
            if !portsChanged && hasAttemptedAutoDetect { return }
            attemptAutoDetect(candidates)
        }
    }

    /// Candidate ordering: native USB-Serial/JTAG first, then any Espressif
    /// VID, then the rest of the USB serial ports.
    private static func portPriority(_ port: SerialPortInfo) -> Int {
        if port.isUSBJTAGSerial { return 0 }
        if port.vendorID == Self.espressifVendorID { return 1 }
        return 2
    }

    private static let espressifVendorID: UInt16 = 0x303A

    /// Try each USB candidate in priority order until one answers the ESP
    /// sync; updates `phase` as it walks the list.
    private func attemptAutoDetect(_ candidates: [SerialPortInfo]) {
        guard let first = candidates.first else { return }
        hasAttemptedAutoDetect = true
        busy = true
        phase = .detecting(port: first.path)
        status = "Found \(first.name) — identifying…"
        log("auto-detect: \(first.path) (\(first.name))")

        let remaining = Array(candidates.dropFirst())
        Task.detached(priority: .userInitiated) { [weak self] in
            guard let self else { return }
            var candidatesToTry = [first] + remaining
            while let candidate = candidatesToTry.first {
                candidatesToTry.removeFirst()
                await MainActor.run {
                    self.phase = .detecting(port: candidate.path)
                    self.status = "Found \(candidate.name) — identifying…"
                }
                do {
                    let (detector, device, info) = try detectDevice(
                        port: candidate.path, explicitAttempts: 7, scanAttempts: 3
                    ) { self.log($0) }
                    detector.close()
                    await MainActor.run {
                        self.install(device: device, port: info ?? candidate, auto: true)
                    }
                    return
                } catch {
                    self.log("auto-detect: \(candidate.path) did not answer (\(error))")
                }
            }
            await MainActor.run {
                self.busy = false
                self.phase = .failed("No ESP32 answered on any USB port")
                self.status = "No board responding — check the USB-C cable and BOOT mode"
                self.log("auto-detect: no ESP32 found")
            }
        }
    }

    /// Promote a successful detection into the connected state and pick the
    /// firmware variant that matches the chip.
    private func install(device: DeviceInfo, port: SerialPortInfo, auto: Bool) {
        self.device = device
        selectedPort = port.path
        connectedMisses = 0
        busy = false
        phase = .connected
        status = "\(device.chip.displayName) connected"
        log("\(auto ? "auto-" : "")detected \(device.chip.displayName) on \(port.path)")
        if variants.isEmpty { scanDist() }
        selectVariant(matching: device.chip.family)
    }

    /// Manual detect on the picker's port — the fallback for UART-bridge
    /// boards that auto-detect cannot reach.
    func detect() {
        guard !busy else { return }
        guard let portPath = selectedPort else {
            status = "Pick a USB port first"
            return
        }
        busy = true
        isFlashing = false
        progress = 0
        device = nil
        phase = .detecting(port: portPath)
        status = "Detecting…"
        Task.detached(priority: .userInitiated) { [weak self] in
            guard let self else { return }
            do {
                let (detector, device, _) = try detectDevice(
                    port: portPath, explicitAttempts: 7, scanAttempts: 3
                ) { self.log($0) }
                detector.close()
                let matched = await MainActor.run { self.ports.first { $0.path == portPath } }
                await MainActor.run {
                    self.install(device: device, port: matched ?? SerialPortInfo(
                        path: portPath, name: URL(fileURLWithPath: portPath).lastPathComponent,
                        vendorID: nil, productID: nil, serialNumber: nil, isUSB: false
                    ), auto: false)
                }
            } catch {
                await MainActor.run {
                    self.busy = false
                    self.phase = .failed("\(error)")
                    self.status = "Detection failed: \(error)"
                }
            }
        }
    }

    /// Force a fresh auto-detect pass (the failed screen's "Try again").
    func retryAutoDetect() {
        hasAttemptedAutoDetect = false
        lastPortSet = []
        let candidates = SerialPortScanner.all()
            .filter(\.isUSB)
            .sorted { Self.portPriority($0) < Self.portPriority($1) }
        guard !candidates.isEmpty else {
            status = "No USB ports found"
            return
        }
        attemptAutoDetect(candidates)
    }

    func flash() {
        guard !busy else { return }
        guard selectedVariant != nil else {
            status = "Pick a firmware image first"
            return
        }
        guard let appPath else {
            status = "No app image in the selected firmware"
            return
        }
        busy = true
        isFlashing = true
        progress = 0
        status = "Flashing…"
        guard let portPath = selectedPort else {
            status = "No port selected"
            busy = false
            isFlashing = false
            return
        }
        let appURL = URL(fileURLWithPath: appPath)
        let bootloaderURL = bootloaderPath.map { URL(fileURLWithPath: $0) }
        let fwDirURL = fwDir.map { URL(fileURLWithPath: $0) }
        let csvText = partitionsPath.flatMap { try? String(contentsOfFile: $0, encoding: .utf8) }

        Task.detached(priority: .userInitiated) { [weak self] in
            guard let self else { return }
            do {
                let (detector, device, _) = try detectDevice(port: portPath, explicitAttempts: 7, scanAttempts: 3) { self.log($0) }
                let family = device.chip.family
                guard family != .unknown else {
                    throw FlashError("could not identify the chip; check the device or pick one explicitly")
                }
                let sizeBytes = device.flashSizeBytes ?? (4 << 20)
                let options = BuildOptions(
                    app: appURL,
                    bootloader: bootloaderURL,
                    partitionsCSV: csvText,
                    fwDir: fwDirURL,
                    chipFamily: family,
                    flashSizeBytes: sizeBytes,
                    flashSizeBits: nil // keep the bootloader's compiled flash-size byte
                )
                let result = try ImageBuilder.build(options)
                self.log(ImageBuilder.describe(result))
                detector.close() // release our fd; esptool opens its own connection
                let flasher = EsptoolFlasher()
                flasher.onLog = { self.log($0) }
                flasher.onProgress = { f in
                    Task { @MainActor in self.progress = f }
                }
                try flasher.flash(chip: family, port: portPath, baud: 115200,
                                  segments: result.segments, verify: true)
                await MainActor.run {
                    self.progress = 1
                    self.status = "Flash complete"
                    self.busy = false
                    self.isFlashing = false
                }
            } catch {
                await MainActor.run {
                    self.status = "Flash failed: \(error)"
                    self.busy = false
                    self.isFlashing = false
                }
            }
        }
    }
}

// MARK: - Surface

struct ContentView: View {
    @StateObject private var model = FlasherModel()
    @State private var showAppPicker = false
    @State private var showBootloaderPicker = false
    @State private var showPartitionsPicker = false
    @State private var showFwDirPicker = false
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            header
            deviceCard
            firmwareCard
            progressStrip
                .animation(.easeOut(duration: 0.2), value: model.busy)
            actionBar
            consoleAccordion
        }
        .padding(20)
        .background(Color.pocketPaper)
        .frame(minWidth: 720, minHeight: 860, maxHeight: .infinity, alignment: .top)
        .onAppear { bootstrap() }
        .onDisappear { model.stopWatching() }
        .onChange(of: model.logText) { _ in trackLog() }
        .onChange(of: model.status) { newStatus in
            if newStatus.hasPrefix("Flash failed")
                || newStatus.hasPrefix("Detection failed")
                || newStatus.hasPrefix("No board") {
                withAnimation(.easeOut(duration: 0.2)) { consoleExpanded = true }
            }
        }
        .onChange(of: consoleExpanded) { expanded in
            if expanded {
                seenLineCount = lineCount
                unreadLines = 0
            }
        }
    }

    private func bootstrap() {
        #if DEBUG
        if ProcessInfo.processInfo.arguments.contains("--demo") {
            model.applyDemoDataIfRequested()
            return
        }
        #endif
        model.scanDist()
        model.startWatching()
        seenLineCount = lineCount
    }

    // MARK: Header

    private var header: some View {
        HStack(spacing: 12) {
            BrandMark()
            VStack(alignment: .leading, spacing: 1) {
                Text("PocketWiki Flasher")
                    .font(.system(size: 16, weight: .semibold))
                Text("Flash firmware to ESP32 boards")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            devicePill
        }
    }

    private var devicePill: some View {
        HStack(spacing: 7) {
            Circle()
                .fill(pillColor)
                .frame(width: 8, height: 8)
            Text(pillText)
                .font(.system(size: 12, weight: .medium))
                .foregroundStyle(.secondary)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 5)
        .background(Color.pocketSurface, in: RoundedRectangle(cornerRadius: 8, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .strokeBorder(Color.pocketLine, lineWidth: 1)
        )
        .animation(.easeOut(duration: 0.25), value: model.phase)
    }

    private var pillColor: Color {
        switch model.phase {
        case .connected: return Color.pocketSuccess
        case .detecting: return Color.pocketAccent
        case .failed: return Color.pocketDanger
        case .waiting: return Color.secondary.opacity(0.55)
        }
    }

    private var pillText: String {
        switch model.phase {
        case .connected: return "\(model.device?.chip.displayName ?? "Board") connected"
        case .detecting: return "Identifying…"
        case .failed: return "No board found"
        case .waiting: return "Waiting for board"
        }
    }

    // MARK: Device

    private var deviceCard: some View {
        SectionCard("Device") {
            HStack(spacing: 16) {
                boardArea
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .layoutPriority(1)

                Divider()
                    .overlay(Color.pocketLine)
                    .frame(height: 142)

                BoardModelView(reduceMotion: reduceMotion)
                    .frame(width: 220, height: 150)
                    .accessibilityElement(children: .ignore)
                    .accessibilityLabel("ESP32-S3 development board 3D preview")
                    .help("ESP32-S3 development board CAD preview — drag to rotate")
            }
        }
        .animation(.easeOut(duration: 0.3), value: model.phase)
    }

    @ViewBuilder
    private var boardArea: some View {
        switch model.phase {
        case .waiting:
            WaitingForBoardView(
                usbPorts: model.usbPorts,
                selectedPort: $model.selectedPort,
                onDetect: { model.detect() }
            )
            .transition(.opacity.combined(with: .move(edge: .top)))
        case .detecting(let port):
            DetectingBoardView(portName: port)
                .transition(.opacity)
        case .connected:
            if let device = model.device {
                if !reduceMotion {
                    deviceSummary(device)
                        .transition(.opacity.combined(with: .move(edge: .top)))
                } else {
                    deviceSummary(device)
                }
            }
        case .failed(let message):
            BoardFailedView(
                message: message,
                usbPorts: model.usbPorts,
                selectedPort: $model.selectedPort,
                onRetry: { model.retryAutoDetect() },
                onDetect: { model.detect() }
            )
            .transition(.opacity.combined(with: .move(edge: .top)))
        }
    }

    private func deviceSummary(_ device: DeviceInfo) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 10) {
                TintedIcon(systemName: "cpu", size: 30)
                VStack(alignment: .leading, spacing: 2) {
                    HStack(spacing: 8) {
                        Text(device.chip.displayName)
                            .font(.system(size: 15, weight: .semibold))
                            .foregroundStyle(Color.pocketInk)
                        if let flash = device.flashSizeText {
                            Text(flash)
                                .font(.callout)
                                .monospacedDigit()
                                .foregroundStyle(.secondary)
                        }
                    }
                    Text(identityLine(device))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Text("Connected")
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundStyle(Color.pocketSuccess)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 3)
                    .background(Color.pocketForestSoft, in: RoundedRectangle(cornerRadius: 6, style: .continuous))
            }

            Divider()
                .overlay(Color.pocketLine)

            specGrid(device)
        }
        .padding(.top, 2)
    }

    private func identityLine(_ device: DeviceInfo) -> String {
        [device.chip.revisionText, device.chip.usbMode].compactMap { $0 }.joined(separator: " · ")
    }

    private func specRows(_ device: DeviceInfo) -> [[(label: String, value: String)]] {
        var items: [(label: String, value: String)] = []
        if let pkg = device.chip.package { items.append(("Package", pkg)) }
        if let rev = device.chip.revisionText { items.append(("Revision", rev)) }
        if let mac = device.chip.macAddress { items.append(("MAC", mac)) }
        if let embedded = device.chip.efuseEmbeddedFlash { items.append(("eFuse", "embedded \(embedded)")) }
        if let mode = device.chip.usbMode { items.append(("USB", mode)) }
        if let flash = device.flashSizeText {
            let source = device.flashSizeSource ?? "?"
            let maker = device.flashManufacturer.map { ", \($0)" } ?? ""
            items.append(("Flash", "\(flash) (\(source))\(maker)"))
        }
        return stride(from: 0, to: items.count, by: 2).map {
            Array(items[$0..<min($0 + 2, items.count)])
        }
    }

    private func specGrid(_ device: DeviceInfo) -> some View {
        Grid(alignment: .leading, horizontalSpacing: 32, verticalSpacing: 6) {
            ForEach(Array(specRows(device).enumerated()), id: \.offset) { _, row in
                GridRow {
                    specCell(row[0])
                    if row.count > 1 {
                        specCell(row[1])
                    } else {
                        Color.clear.gridCellUnsizedAxes([.horizontal, .vertical])
                    }
                }
            }
        }
    }

    private func specCell(_ item: (label: String, value: String)) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(item.label)
                .font(.caption)
                .foregroundStyle(.secondary)
            Text(item.value)
                .font(.callout)
                .monospacedDigit()
                .foregroundStyle(Color.pocketInk)
                .lineLimit(1)
                .truncationMode(.middle)
                .textSelection(.enabled)
        }
    }

    // MARK: Firmware

    private var firmwareCard: some View {
        SectionCard("Firmware") {
            distHeader
            if model.variants.isEmpty {
                Text("No firmware found in the dist folder.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                VStack(spacing: 6) {
                    ForEach(model.variants) { variant in
                        variantRow(variant)
                    }
                }
            }
        }
        .fileImporter(isPresented: $showDistPicker, allowedContentTypes: [.folder]) { result in
            if case .success(let url) = result {
                model.setDistRoot(url.path)
            }
        }
    }

    private var distHeader: some View {
        HStack(spacing: 10) {
            TintedIcon(systemName: "shippingbox", size: 28)
            VStack(alignment: .leading, spacing: 1) {
                Text("Firmware dist")
                    .font(.callout)
                    .fontWeight(.medium)
                    .foregroundStyle(Color.pocketInk)
                Text(model.distRoot ?? "not found — choose a folder")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            Spacer()
            Button(model.distRoot == nil ? "Choose…" : "Change…") {
                showDistPicker = true
            }
            .buttonStyle(PocketGhostButtonStyle(compact: true))
            .help("Pick the folder containing firmware/dist/<chip>/ layouts")
            .disabled(model.busy)
        }
    }

    private func variantRow(_ variant: FirmwareVariant) -> some View {
        let isSelected = model.selectedVariantName == variant.name
        let matchesBoard = model.variantMatchesBoard(variant)
        return HStack(spacing: 10) {
            Image(systemName: isSelected ? "checkmark.circle.fill" : "circle")
                .font(.system(size: 14, weight: .medium))
                .foregroundStyle(isSelected ? Color.pocketAccent : Color.secondary.opacity(0.4))
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 8) {
                    Text(variant.name)
                        .font(.callout.weight(.semibold))
                        .foregroundStyle(Color.pocketInk)
                    if isSelected && matchesBoard {
                        Text("for this board")
                            .font(.system(size: 10, weight: .medium))
                            .foregroundStyle(Color.pocketAccent)
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(Color.pocketForestSoft, in: Capsule())
                    }
                    Spacer()
                    if let size = variant.appSizeText {
                        Text(size)
                            .font(.caption)
                            .monospacedDigit()
                            .foregroundStyle(.secondary)
                    }
                }
                HStack(spacing: 14) {
                    ForEach(variant.artifacts) { artifact in
                        HStack(spacing: 4) {
                            Image(systemName: artifact.present ? "checkmark.circle.fill" : "minus.circle")
                                .font(.system(size: 10))
                                .foregroundStyle(artifact.present ? Color.pocketSuccess : Color.secondary.opacity(0.45))
                            Text(artifact.label)
                                .font(.system(size: 11))
                                .foregroundStyle(artifact.present ? Color.pocketInkMuted : Color.secondary.opacity(0.6))
                            if let detail = artifact.detail {
                                Text(detail)
                                    .font(.system(size: 10))
                                    .monospacedDigit()
                                    .foregroundStyle(.secondary.opacity(0.85))
                            }
                        }
                        .help(artifact.present
                            ? "\(artifact.label): \(artifact.detail ?? "present")"
                            : "\(artifact.label): missing")
                    }
                }
            }
            Spacer()
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .background(
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .fill(isSelected ? Color.pocketForestSoft : Color.clear)
        )
        .overlay(
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .strokeBorder(isSelected ? Color.pocketAccent.opacity(0.4) : Color.clear, lineWidth: 1)
        )
        .contentShape(Rectangle())
        .onTapGesture {
            withAnimation(.easeOut(duration: 0.15)) { model.selectVariant(variant.name) }
        }
        .help("Use the \(variant.name) firmware for flashing")
    }

    // MARK: Progress

    @ViewBuilder
    private var progressStrip: some View {
        if model.busy {
            FlashProgressBar(
                progress: model.isFlashing ? model.progress : 0,
                indeterminate: !model.isFlashing || model.progress <= 0,
                caption: progressCaption
            )
            .transition(.opacity)
        } else if model.status.hasPrefix("Flash complete") {
            FlashSuccessBanner(text: model.status)
                .transition(.opacity)
        }
    }

    private var progressCaption: String {
        if !model.isFlashing { return "Preparing image…" }
        if model.progress <= 0 { return "Connecting to board…" }
        if model.progress >= 1 { return "Verifying flash…" }
        return "Writing firmware…"
    }

    // MARK: Action bar

    private var actionBar: some View {
        HStack(spacing: 14) {
            statusView
            Spacer()
            Button {
                model.flash()
            } label: {
                Label("Flash", systemImage: "arrow.down.circle")
            }
            .buttonStyle(PocketPrimaryButtonStyle())
            .keyboardShortcut(.defaultAction)
            .help("Build the image and write it to the board")
            .disabled(!model.canFlash)
        }
        .padding(.top, 2)
    }

    @ViewBuilder
    private var statusView: some View {
        if !model.status.isEmpty && !model.status.hasPrefix("Flash complete") {
            HStack(spacing: 6) {
                if model.busy {
                    ProgressView()
                        .controlSize(.small)
                        .tint(statusColor)
                } else if isSuccess {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(Color.pocketSuccess)
                } else if isFailure {
                    Image(systemName: "xmark.circle.fill")
                        .foregroundStyle(Color.pocketDanger)
                } else {
                    Circle()
                        .fill(Color.secondary)
                        .frame(width: 5, height: 5)
                }
                Text(model.status)
                    .font(.callout)
                    .foregroundStyle(statusColor)
                    .lineLimit(1)
            }
        }
    }

    private var isSuccess: Bool {
        model.status.hasPrefix("Flash complete") || model.status.hasPrefix("Firmware ready")
    }

    private var isFailure: Bool {
        model.status.hasPrefix("Flash failed") || model.status.hasPrefix("Detection failed")
            || model.status.hasPrefix("No board")
    }

    private var statusColor: Color {
        if model.busy { return Color.pocketAccent }
        if isSuccess { return Color.pocketSuccess }
        if isFailure { return Color.pocketDanger }
        return .secondary
    }

    // MARK: Console

    private var consoleAccordion: some View {
        ConsoleAccordion(model: model, isExpanded: $consoleExpanded, unreadLines: unreadLines)
            .frame(maxHeight: consoleExpanded ? CGFloat.infinity : nil, alignment: .top)
    }

    @State private var showDistPicker = false
    @State private var consoleExpanded = false
    @State private var seenLineCount = 0
    @State private var unreadLines = 0

    private var lineCount: Int { model.logText.split(separator: "\n").count }

    private func trackLog() {
        if consoleExpanded {
            seenLineCount = lineCount
        } else {
            unreadLines = max(0, lineCount - seenLineCount)
        }
    }
}

// MARK: - Components

/// The square green `P` brand mark — the sole graphic signature.
struct BrandMark: View {
    var size: CGFloat = 36
    var body: some View {
        RoundedRectangle(cornerRadius: size * 0.28, style: .continuous)
            .fill(Color.pocketForest)
            .frame(width: size, height: size)
            .overlay(
                Text("P")
                    .font(.system(size: size * 0.56, weight: .heavy, design: .monospaced))
                    .foregroundStyle(.white)
            )
            .accessibilityLabel("PocketWiki")
    }
}

/// A quiet paper card with a hairline border — the system's container.
struct SectionCard<Content: View, Trailing: View>: View {
    let title: String
    var fill: Color = .pocketSurface
    var titleColor: Color = .pocketInk
    let trailing: Trailing
    let content: Content

    init(_ title: String, fill: Color = .pocketSurface, titleColor: Color = .pocketInk,
         @ViewBuilder content: () -> Content,
         @ViewBuilder trailing: () -> Trailing = { EmptyView() }) {
        self.title = title
        self.fill = fill
        self.titleColor = titleColor
        self.content = content()
        self.trailing = trailing()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .firstTextBaseline) {
                Text(title)
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(titleColor)
                Spacer()
                trailing
            }
            content
        }
        .padding(14)
        .background(fill, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .strokeBorder(Color.pocketLine, lineWidth: 1)
        )
    }
}

/// Small square icon chip — the system's icon container.
struct TintedIcon: View {
    let systemName: String
    var size: CGFloat = 28
    var tint: Color = .pocketAccent

    var body: some View {
        Image(systemName: systemName)
            .font(.system(size: size * 0.46, weight: .medium))
            .foregroundStyle(tint)
            .frame(width: size, height: size)
            .background(tint.opacity(0.12), in: RoundedRectangle(cornerRadius: size * 0.26, style: .continuous))
    }
}

/// Primary action: forest fill, deepens on hover/press, dims when disabled.
struct PocketPrimaryButtonStyle: ButtonStyle {
    @Environment(\.isEnabled) private var isEnabled
    @State private var hovering = false

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 14, weight: .semibold))
            .padding(.horizontal, 18)
            .padding(.vertical, 9)
            .foregroundStyle(.white)
            .background(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(configuration.isPressed || hovering ? Color.pocketForestDeep : Color.pocketForest)
            )
            .opacity(isEnabled ? 1 : 0.4)
            .scaleEffect(configuration.isPressed ? 0.98 : 1)
            .animation(.easeOut(duration: 0.12), value: hovering)
            .onHover { hovering = $0 }
    }
}

/// Secondary action: hairline border, Mist Green hover wash.
struct PocketGhostButtonStyle: ButtonStyle {
    var compact = false
    @Environment(\.isEnabled) private var isEnabled
    @State private var hovering = false

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 13, weight: .medium))
            .foregroundStyle(isEnabled ? Color.pocketInk : Color.secondary)
            .padding(.horizontal, compact ? 10 : 12)
            .padding(.vertical, compact ? 5 : 6)
            .background(
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .fill(hovering ? Color.pocketForestSoft : Color.clear)
            )
            .overlay(
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .strokeBorder(isEnabled ? Color.pocketLine : Color.secondary.opacity(0.35), lineWidth: 1)
            )
            .opacity(isEnabled ? 1 : 0.45)
            .animation(.easeOut(duration: 0.12), value: hovering)
            .onHover { hovering = $0 }
    }
}

// MARK: - Demo preview (DEBUG only — never in release)

#if DEBUG
extension FlasherModel {
    /// Launch with `--demo` to preview the populated surface without hardware.
    func applyDemoDataIfRequested() {
        guard ProcessInfo.processInfo.arguments.contains("--demo") else { return }
        ports = [
            SerialPortInfo(path: "/dev/cu.usbmodem14201", name: "USB JTAG/serial debug unit",
                           vendorID: 0x303A, productID: 0x1001, serialNumber: "7C:DF:A1:02:34:56", isUSB: true)
        ]
        selectedPort = ports.first?.path
        // DeviceInfo has no cross-module memberwise init; decode a fixture
        // through its Codable conformance instead.
        let fixture = """
        {"chip":{"family":"ESP32-S3","detectionMethod":"demo","rawValue":9,
        "revisionMajor":0,"revisionMinor":1,"package":"QFN56",
        "macAddress":"7C:DF:A1:02:34:56","efuseEmbeddedFlash":"16MB",
        "flashVendorEfuse":"Winbond","usbMode":"USB-Serial/JTAG"},
        "flashSizeBytes":16777216,"flashSizeSource":"spi-id",
        "flashID":1589256,"flashManufacturer":"Winbond"}
        """
        device = try? JSONDecoder().decode(DeviceInfo.self, from: Data(fixture.utf8))
        phase = .connected
        connectedMisses = 0
        // Enumerate the real dist folder; fall back to the canonical layout
        // if the scanner cannot find it from this working directory.
        let root = FirmwareDistScanner.locateDefaultRoot()?.path ?? "firmware/dist"
        distRoot = root
        variants = FirmwareDistScanner.scan(root: URL(fileURLWithPath: root))
        selectVariant(matching: .esp32s3)
        if selectedVariantName == nil, let first = variants.first {
            selectVariant(first.name)
        }
        if appPath == nil {
            appPath = "\(root)/esp32-s3/firmware.bin"
            bootloaderPath = "\(root)/esp32-s3/bootloader.bin"
            partitionsPath = "\(root)/esp32-s3/partitions.csv"
            fwDir = "\(root)/esp32-s3"
        }
        logText = "Detecting device on /dev/cu.usbmodem14201…\n"
            + "  chip: ESP32-S3 (revision v0.1), 16MB flash (spi-id, Winbond)\n"
            + "  building image: bootloader + partition table + app + content\n"
            + "  writing 4 regions, verifying after write…\n"
            + "Flash complete — 16.00 MB written and verified."
        status = "Flash complete"
        busy = false
        isFlashing = false
        progress = 1
    }
}
#endif
