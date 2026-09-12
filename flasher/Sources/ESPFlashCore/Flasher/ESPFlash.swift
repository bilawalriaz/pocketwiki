import Foundation

/// One blob to write into flash.
public struct FlashSegmentData {
    public var label: String
    public var offset: UInt32
    public var data: Data

    public init(label: String, offset: UInt32, data: Data) {
        self.label = label
        self.offset = offset
        self.data = data
    }
}

public enum FlashPhase: Equatable {
    case opening
    case connecting
    case detecting
    case erasing(offset: UInt32, size: UInt32)
    case writing(offset: UInt32, bytesDone: UInt64, bytesTotal: UInt64)
    case verifying(offset: UInt32)
    case resetting
    case done
}

public struct FlashProgress {
    public var phase: FlashPhase
    public var message: String

    public init(phase: FlashPhase, message: String) {
        self.phase = phase
        self.message = message
    }

    /// 0...1 overall completion, or nil for indeterminate phases.
    public var fraction: Double? {
        switch phase {
        case .writing(_, let done, let total):
            return total > 0 ? Double(done) / Double(total) : 0
        default:
            return nil
        }
    }
}

