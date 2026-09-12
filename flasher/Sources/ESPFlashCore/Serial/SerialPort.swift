import Foundation

#if os(Linux)
import Glibc
#else
import Darwin
#endif

/// Minimal POSIX serial port over termios. Works identically on macOS and
/// Linux (the Windows port would replace just this file).
public final class SerialPort {
    public let path: String
    private var fd: Int32 = -1

    public private(set) var baudRate: Int32 = 0
    public private(set) var readTimeout: Double = 0.1

    public init(path: String) {
        self.path = path
    }

    public var isOpen: Bool { fd >= 0 }

    @discardableResult
    public func open(baud: Int32 = 115200, readTimeout: Double = 0.1) throws -> SerialPort {
        guard !isOpen else { return self }
        let h = path.withCString { Darwin.open($0, O_RDWR | O_NOCTTY | O_NONBLOCK) }
        guard h >= 0 else {
            throw FlashError("failed to open serial port \(path): \(String(cString: strerror(errno)))")
        }
        fd = h
        let flags = fcntl(fd, F_GETFL)
        _ = fcntl(fd, F_SETFL, flags & ~O_NONBLOCK)
        self.readTimeout = readTimeout
        try configure(baud: baud)
        return self
    }

    public func close() {
        if isOpen {
            _ = Darwin.close(fd)
            fd = -1
        }
    }

    deinit {
        close()
    }

    public func setBaud(_ baud: Int32) throws {
        try configure(baud: baud)
    }

    public func setReadTimeout(_ timeout: Double) {
        readTimeout = timeout
        guard isOpen else { return }
        var t = termios()
        if tcgetattr(fd, &t) == 0 {
            applyVTime(&t)
            _ = tcsetattr(fd, TCSANOW, &t)
        }
    }

    // MARK: - I/O

    @discardableResult
    public func read(_ maxLength: Int) -> Data {
        guard isOpen, maxLength > 0 else { return Data() }
        var buf = [UInt8](repeating: 0, count: maxLength)
        let n = buf.withUnsafeMutableBytes { raw -> Int in
            Darwin.read(fd, raw.baseAddress, maxLength)
        }
        guard n > 0 else { return Data() }
        return Data(buf[0..<n])
    }

    public func write(_ data: Data) throws {
        guard isOpen else { throw FlashError("port not open: \(path)") }
        // Blocking write, one syscall per packet, like pyserial. (Do NOT use
        // O_NONBLOCK here: splitting a SLIP frame across USB transfers with
        // sleeps corrupts the ESP's flash-write stream over USB-Serial/JTAG.)
        var written = 0
        while written < data.count {
            let n = data.withUnsafeBytes { raw -> Int in
                Darwin.write(fd, raw.baseAddress!.advanced(by: written), data.count - written)
            }
            if n < 0 {
                if errno == EINTR { continue }
                throw FlashError("write failed on \(path): \(String(cString: strerror(errno)))")
            }
            if n == 0 {
                throw FlashError("write returned 0 on \(path)")
            }
            written += n
        }
    }

    public func inWaiting() -> Int {
        guard isOpen else { return 0 }
        var n: Int32 = 0
        #if os(Linux)
        _ = ioctl(fd, UInt(FIONREAD), &n)
        #else
        // FIONREAD is not exposed by Swift's Darwin module; _IOR('f', 127, int).
        _ = ioctl(fd, 0x4004_667F, &n)
        #endif
        return Int(n)
    }

    public func flushInput() {
        if isOpen { _ = tcflush(fd, TCIFLUSH) }
    }

    public func flushOutput() {
        if isOpen { _ = tcflush(fd, TCOFLUSH) }
    }

    // MARK: - Control lines (reset / boot-mode straps)

    private var modemStatus: CInt {
        get {
            guard isOpen else { return 0 }
            var s: CInt = 0
            if ioctl(fd, UInt(TIOCMGET), &s) != 0 { return 0 }
            return s
        }
        set {
            guard isOpen else { return }
            var s = newValue
            _ = ioctl(fd, UInt(TIOCMSET), &s)
        }
    }

    public func setDTR(_ on: Bool) throws {
        var s = modemStatus
        if on { s |= CInt(TIOCM_DTR) } else { s &= ~CInt(TIOCM_DTR) }
        modemStatus = s
    }

    public func setRTS(_ on: Bool) throws {
        var s = modemStatus
        if on { s |= CInt(TIOCM_RTS) } else { s &= ~CInt(TIOCM_RTS) }
        modemStatus = s
    }

    /// Set both lines atomically (TIOCMSET), as esptool's "tight" reset needs.
    public func setDTRandRTS(dtr: Bool, rts: Bool) throws {
        var s = modemStatus
        if dtr { s |= CInt(TIOCM_DTR) } else { s &= ~CInt(TIOCM_DTR) }
        if rts { s |= CInt(TIOCM_RTS) } else { s &= ~CInt(TIOCM_RTS) }
        modemStatus = s
    }

    // MARK: - termios config

    private func configure(baud: Int32) throws {
        guard isOpen else { return }
        var t = termios()
        guard tcgetattr(fd, &t) == 0 else {
            throw FlashError("tcgetattr failed on \(path)")
        }
        cfmakeraw(&t)
        t.c_cflag |= UInt(CLOCAL | CREAD)
        t.c_cflag &= ~UInt(CRTSCTS)
        t.c_cflag &= ~UInt(CSIZE)
        t.c_cflag |= UInt(CS8)
        t.c_cflag &= ~UInt(PARENB)
        t.c_cflag &= ~UInt(CSTOPB)
        applyVTime(&t)
        guard let speed = baudConstant(baud) else {
            throw FlashError("unsupported baud rate \(baud)")
        }
        _ = cfsetispeed(&t, speed)
        _ = cfsetospeed(&t, speed)
        guard tcsetattr(fd, TCSANOW, &t) == 0 else {
            throw FlashError("tcsetattr failed on \(path)")
        }
        baudRate = baud
    }

    private func applyVTime(_ t: inout termios) {
        #if os(Linux)
        t.c_cc.6 = 0 // VMIN
        t.c_cc.5 = cc_t(max(0, min(255, Int(readTimeout * 10)))) // VTIME in 0.1s units
        #else
        t.c_cc.16 = 0 // VMIN
        t.c_cc.17 = cc_t(max(0, min(255, Int(readTimeout * 10)))) // VTIME
        #endif
    }

    private func baudConstant(_ baud: Int32) -> tcflag_t? {
        switch baud {
        case 9600: return tcflag_t(B9600)
        case 19200: return tcflag_t(B19200)
        case 38400: return tcflag_t(B38400)
        case 57600: return tcflag_t(B57600)
        case 115200: return tcflag_t(B115200)
        case 230400: return tcflag_t(B230400)
        #if os(Linux)
        case 460800: return tcflag_t(B460800)
        case 921600: return tcflag_t(B921600)
        #endif
        default: return nil
        }
    }
}
