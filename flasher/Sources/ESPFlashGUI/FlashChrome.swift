import SwiftUI
import ESPFlashCore

// ============================================================================
// Flash/board chrome: the state screens for the device card, the flash
// progress bar, the success banner and the collapsible console. Pure surface;
// all state lives in FlasherModel.
// ============================================================================

// MARK: - Waiting screen

/// Hero state before a board shows up: pulsing plug + hint, with a manual
/// USB-port fallback for UART-bridge boards that auto-detect cannot reach.
struct WaitingForBoardView: View {
    let usbPorts: [SerialPortInfo]
    @Binding var selectedPort: String?
    let onDetect: () -> Void
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var pulse = false
    @State private var showManual = false

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 12) {
                ZStack {
                    Circle()
                        .fill(Color.pocketForestSoft)
                        .frame(width: 48, height: 48)
                        .scaleEffect(pulse ? 1.18 : 0.9)
                        .opacity(pulse ? 0.85 : 1)
                    TintedIcon(systemName: "cable.connector", size: 34)
                }
                VStack(alignment: .leading, spacing: 2) {
                    Text("Waiting for board")
                        .font(.callout.weight(.semibold))
                        .foregroundStyle(Color.pocketInk)
                    Text("Plug in the ESP32 over USB-C — it is found and identified automatically.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            if !usbPorts.isEmpty {
                Button {
                    withAnimation(.easeOut(duration: 0.2)) { showManual.toggle() }
                } label: {
                    HStack(spacing: 4) {
                        Image(systemName: "chevron.right")
                            .font(.system(size: 9, weight: .semibold))
                            .rotationEffect(.degrees(showManual ? 90 : 0))
                        Text("Not auto-detected? Choose a USB port…")
                            .font(.caption)
                    }
                    .foregroundStyle(.secondary)
                }
                .buttonStyle(.plain)
                .help("Manually pick a serial port (e.g. a USB-UART bridge board)")

                if showManual {
                    HStack(spacing: 8) {
                        Picker("Port", selection: $selectedPort) {
                            ForEach(usbPorts) { port in
                                Text(port.name.isEmpty ? port.path : "\(port.path) (\(port.name))")
                                    .tag(Optional(port.path))
                            }
                        }
                        .labelsHidden()
                        .frame(maxWidth: 320)
                        .disabled(usbPorts.isEmpty)

                        Button("Detect", action: onDetect)
                            .buttonStyle(PocketGhostButtonStyle(compact: true))
                    }
                    .padding(.leading, 4)
                    .transition(.opacity.combined(with: .move(edge: .top)))
                }
            }
        }
        .padding(.vertical, 6)
        .onAppear {
            guard !reduceMotion else { return }
            withAnimation(.easeInOut(duration: 1.2).repeatForever(autoreverses: true)) {
                pulse = true
            }
        }
    }
}

// MARK: - Detecting screen

/// Shown while the detector is talking to a candidate port.
struct DetectingBoardView: View {
    let portName: String

    var body: some View {
        HStack(spacing: 12) {
            ZStack {
                Circle()
                    .fill(Color.pocketForestSoft)
                    .frame(width: 48, height: 48)
                ProgressView()
                    .controlSize(.small)
                    .tint(Color.pocketAccent)
            }
            VStack(alignment: .leading, spacing: 2) {
                Text("Identifying board…")
                    .font(.callout.weight(.semibold))
                    .foregroundStyle(Color.pocketInk)
                Text(portName)
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
        }
        .padding(.vertical, 6)
    }
}

// MARK: - Failed screen

/// No ESP answered on any candidate port: diagnosis + retry + manual picker.
struct BoardFailedView: View {
    let message: String
    let usbPorts: [SerialPortInfo]
    @Binding var selectedPort: String?
    let onRetry: () -> Void
    let onDetect: () -> Void
    @State private var showManual = false

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 12) {
                ZStack {
                    Circle()
                        .fill(Color.pocketDanger.opacity(0.12))
                        .frame(width: 48, height: 48)
                    Image(systemName: "exclamationmark.triangle.fill")
                        .font(.system(size: 16, weight: .medium))
                        .foregroundStyle(Color.pocketDanger)
                }
                VStack(alignment: .leading, spacing: 2) {
                    Text("No board found")
                        .font(.callout.weight(.semibold))
                        .foregroundStyle(Color.pocketInk)
                    Text(message)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Button("Try again", action: onRetry)
                    .buttonStyle(PocketGhostButtonStyle(compact: true))
                    .help("Rescan and re-detect")
            }

            if !usbPorts.isEmpty {
                Button {
                    withAnimation(.easeOut(duration: 0.2)) { showManual.toggle() }
                } label: {
                    HStack(spacing: 4) {
                        Image(systemName: "chevron.right")
                            .font(.system(size: 9, weight: .semibold))
                            .rotationEffect(.degrees(showManual ? 90 : 0))
                        Text("Choose a USB port manually…")
                            .font(.caption)
                    }
                    .foregroundStyle(.secondary)
                }
                .buttonStyle(.plain)

                if showManual {
                    HStack(spacing: 8) {
                        Picker("Port", selection: $selectedPort) {
                            ForEach(usbPorts) { port in
                                Text(port.name.isEmpty ? port.path : "\(port.path) (\(port.name))")
                                    .tag(Optional(port.path))
                            }
                        }
                        .labelsHidden()
                        .frame(maxWidth: 320)

                        Button("Detect", action: onDetect)
                            .buttonStyle(PocketGhostButtonStyle(compact: true))
                    }
                    .padding(.leading, 4)
                    .transition(.opacity.combined(with: .move(edge: .top)))
                }
            }
        }
        .padding(.vertical, 6)
    }
}

// MARK: - Flash progress bar

/// The flash progress surface: rounded track, gradient fill, percentage,
/// stage caption, and a shimmer for the indeterminate phase (detection /
/// image build / esptool connect) before byte offsets start streaming in.
struct FlashProgressBar: View {
    var progress: Double
    var indeterminate: Bool
    var caption: String
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    private var fillFraction: CGFloat {
        if indeterminate { return 0.42 }
        return CGFloat(max(0.0, min(progress, 1.0)))
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(alignment: .firstTextBaseline) {
                Text(caption)
                    .font(.caption.weight(.medium))
                    .foregroundStyle(Color.pocketInkMuted)
                Spacer()
                if !indeterminate {
                    Text("\(Int((progress * 100).rounded()))%")
                        .font(.system(size: 11, weight: .semibold, design: .monospaced))
                        .foregroundStyle(Color.pocketInk)
                        .monospacedDigit()
                        .contentTransition(.numericText())
                }
            }

            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    Capsule()
                        .fill(Color.pocketLine.opacity(0.5))

                    Capsule()
                        .fill(LinearGradient(
                            colors: [Color.pocketForest, Color.pocketSuccess],
                            startPoint: .leading, endPoint: .trailing
                        ))
                        .frame(width: max(geo.size.width * fillFraction, 10))
                        .animation(reduceMotion ? nil : .easeOut(duration: 0.25), value: progress)

                    if indeterminate && !reduceMotion {
                        TimelineView(.animation(minimumInterval: 1.0 / 30.0)) { timeline in
                            let phase = (timeline.date.timeIntervalSinceReferenceDate
                                .truncatingRemainder(dividingBy: 1.2)) / 1.2
                            Capsule()
                                .fill(Color.white.opacity(0.16))
                                .frame(width: geo.size.width * 0.3)
                                .offset(x: -geo.size.width * 0.3 + phase * geo.size.width * 1.2)
                        }
                    }
                }
            }
            .frame(height: 10)
            .clipped()
        }
    }
}

// MARK: - Success banner

/// Brief green confirmation after a completed flash, with a pop-in checkmark.
struct FlashSuccessBanner: View {
    let text: String
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var popped = false

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: "checkmark.circle.fill")
                .font(.system(size: 15, weight: .medium))
                .foregroundStyle(Color.pocketSuccess)
                .scaleEffect(popped ? 1 : 0.3)
                .opacity(popped ? 1 : 0)
            Text(text)
                .font(.callout)
                .foregroundStyle(Color.pocketSuccess)
            Spacer()
        }
        .padding(.vertical, 2)
        .onAppear {
            if reduceMotion {
                popped = true
            } else {
                withAnimation(.spring(response: 0.35, dampingFraction: 0.6)) { popped = true }
            }
        }
    }
}

// MARK: - Console accordion

/// Console log hidden by default; expands on demand. Shows an unread-line
/// badge while collapsed and auto-scrolls to the newest output when open.
struct ConsoleAccordion: View {
    @ObservedObject var model: FlasherModel
    @Binding var isExpanded: Bool
    var unreadLines: Int
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        VStack(spacing: 0) {
            header
            if isExpanded {
                logView
                    .transition(.opacity.combined(with: .move(edge: .top)))
            }
        }
        .padding(14)
        .background(Color.consoleSurface, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .strokeBorder(Color.pocketLine, lineWidth: 1)
        )
    }

    private var header: some View {
        HStack(spacing: 8) {
            Image(systemName: "chevron.right")
                .font(.system(size: 10, weight: .semibold))
                .rotationEffect(.degrees(isExpanded ? 90 : 0))
                .animation(.easeOut(duration: 0.18), value: isExpanded)
            Image(systemName: "terminal")
                .font(.system(size: 11))
            Text("Console")
                .font(.system(size: 12, weight: .semibold))
            if !isExpanded && unreadLines > 0 {
                Text("\(unreadLines) new")
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundStyle(Color.pocketAccent)
                    .padding(.horizontal, 6)
                    .padding(.vertical, 2)
                    .background(Color.pocketForestSoft, in: Capsule())
            }
            Spacer()
            if !model.logText.isEmpty {
                Button {
                    model.clearLog()
                } label: {
                    Label("Clear", systemImage: "trash")
                        .font(.system(size: 11, weight: .medium))
                }
                .buttonStyle(.borderless)
                .foregroundStyle(Color.consoleInkMuted)
                .help("Clear the console")
            }
        }
        .foregroundStyle(Color.consoleInkMuted)
        .contentShape(Rectangle())
        .onTapGesture {
            withAnimation(reduceMotion ? nil : .easeOut(duration: 0.2)) {
                isExpanded.toggle()
            }
        }
        .accessibilityAddTraits(.isButton)
        .accessibilityLabel(isExpanded ? "Hide console" : "Show console")
    }

    private var logView: some View {
        ScrollViewReader { proxy in
            ScrollView {
                if model.logText.isEmpty {
                    Text("esptool output appears here.")
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(Color.consoleInkMuted)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(8)
                } else {
                    Text(model.logText)
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(Color.consoleInk)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .textSelection(.enabled)
                        .padding(8)
                        .id("logEnd")
                }
            }
            .scrollIndicators(.automatic)
            .onAppear {
                if !model.logText.isEmpty {
                    proxy.scrollTo("logEnd", anchor: .bottom)
                }
            }
            .onChange(of: model.logText) { _ in
                withAnimation(reduceMotion ? nil : .easeOut(duration: 0.15)) {
                    proxy.scrollTo("logEnd", anchor: .bottom)
                }
            }
        }
        .padding(.top, 10)
    }
}
