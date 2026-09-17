import Foundation

/// Single error type for the whole core. Keeps CLI and GUI error handling
/// trivial and makes JSON/UI mapping easy.
public struct FlashError: Error, CustomStringConvertible, LocalizedError, Equatable {
    public let message: String
    /// True when the ROM rejected a command as unknown (used to detect
    /// chips that do not support `get_security_info`, e.g. classic ESP32).
    public let isUnsupportedCommand: Bool

    public init(_ message: String, isUnsupportedCommand: Bool = false) {
        self.message = message
        self.isUnsupportedCommand = isUnsupportedCommand
    }

    public var description: String { message }
    public var errorDescription: String? { message }

    public static func unsupportedCommand(_ op: UInt8) -> FlashError {
        FlashError("unsupported command 0x\(String(format: "%02x", op))", isUnsupportedCommand: true)
    }

    public static func protocolError(_ message: String) -> FlashError {
        FlashError(message)
    }

    public static func noSerialData() -> FlashError {
        FlashError("no serial data received — is the board connected and in download mode?")
    }
}
