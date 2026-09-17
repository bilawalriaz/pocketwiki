import Foundation

/// One discovered serial device.
public struct SerialPortInfo: Codable, Equatable, Identifiable {
    public var path: String
    public var name: String
    public var vendorID: UInt16?
    public var productID: UInt16?
    public var serialNumber: String?
    public var isUSB: Bool

    public init(path: String, name: String, vendorID: UInt16?, productID: UInt16?, serialNumber: String?, isUSB: Bool) {
        self.path = path
        self.name = name
        self.vendorID = vendorID
        self.productID = productID
        self.serialNumber = serialNumber
        self.isUSB = isUSB
    }

    public var id: String { path }

    /// The USB-Serial/JTAG peripheral built into ESP32-C3/S3 ships with this PID.
    public var isUSBJTAGSerial: Bool { productID == 0x1001 }
}

/// Platform-specific enumeration strategy.
public protocol PortEnumerator {
    func enumerate() -> [SerialPortInfo]
}

/// Merges all platform enumerators, de-duplicating by device path.
public enum SerialPortScanner {
    public static func all() -> [SerialPortInfo] {
        var seen = Set<String>()
        var out: [SerialPortInfo] = []
        for enumerator in platformEnumerators() {
            for p in enumerator.enumerate() where !seen.contains(p.path) {
                seen.insert(p.path)
                out.append(p)
            }
        }
        return out
    }

    private static func platformEnumerators() -> [PortEnumerator] {
        #if os(Linux)
        return [LinuxSysfsPortEnumerator(), FallbackPortEnumerator()]
        #else
        return [IOKitPortEnumerator(), FallbackPortEnumerator()]
        #endif
    }
}

#if os(macOS)
import IOKit

// IOSerialKeys values (not exported by Swift's IOKit module on all SDKs).
private let kIOSerialBSDServiceType = "IOSerialBSDClient" as CFString
private let kIOCalloutDeviceKey = "IOCalloutDevice" as CFString
private let kIOTTYBaseNameKey = "IOTTYBaseName" as CFString
private let kIOTTYDeviceKey = "IOTTYDevice" as CFString

/// macOS enumeration via IOKit: gives device paths plus USB vendor/product
/// IDs, serial numbers and product names from the USB parent.
public final class IOKitPortEnumerator: PortEnumerator {
    public init() {}

    public func enumerate() -> [SerialPortInfo] {
        var out: [SerialPortInfo] = []
        guard let match = IOServiceMatching("IOSerialBSDClient") else { return out }
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, match, &iterator) == KERN_SUCCESS else { return out }
        defer { IOObjectRelease(iterator) }

        while true {
            let service = IOIteratorNext(iterator)
            guard service != 0 else { break }
            defer { IOObjectRelease(service) }

            guard let devicePath = stringProperty(service, kIOCalloutDeviceKey as CFString) else { continue }
            let baseName = stringProperty(service, kIOTTYBaseNameKey as CFString)
                ?? stringProperty(service, kIOTTYDeviceKey as CFString)
            let usb = findUSBInfo(from: service)

            out.append(SerialPortInfo(
                path: devicePath,
                name: usb?.name ?? baseName ?? devicePath,
                vendorID: usb?.vendorID,
                productID: usb?.productID,
                serialNumber: usb?.serialNumber,
                isUSB: usb != nil
            ))
        }
        return out
    }

    private struct USBInfo {
        var vendorID: UInt16
        var productID: UInt16
        var serialNumber: String?
        var name: String?
    }

    private func findUSBInfo(from service: io_registry_entry_t) -> USBInfo? {
        // pyserial-proven approach: recursively search the IOService plane
        // (up through parents) for USB properties. Walking parents manually
        // misses the IOUSBHostDevice node on Apple Silicon.
        let flags = IOOptionBits(kIORegistryIterateRecursively | kIORegistryIterateParents)
        func search(_ key: String) -> CFTypeRef? {
            IORegistryEntrySearchCFProperty(service, kIOServicePlane, key as CFString,
                                            kCFAllocatorDefault, flags)
        }
        let vid = (search("USB Vendor ID") ?? search("idVendor")) as? NSNumber
        let pid = (search("USB Product ID") ?? search("idProduct")) as? NSNumber
        guard let vid, let pid else { return nil }
        return USBInfo(
            vendorID: vid.uint16Value,
            productID: pid.uint16Value,
            serialNumber: search("USB Serial Number") as? String,
            name: search("USB Product Name") as? String
        )
    }

    private func stringProperty(_ service: io_registry_entry_t, _ key: CFString) -> String? {
        guard let cf = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0)?.takeRetainedValue() else {
            return nil
        }
        return cf as? String
    }
}
#endif

#if os(Linux)
/// Linux enumeration via /sys/class/tty: USB serial adapters expose
/// idVendor/idProduct/serial/product in the device directory.
public final class LinuxSysfsPortEnumerator: PortEnumerator {
    public init() {}

    public func enumerate() -> [SerialPortInfo] {
        var out: [SerialPortInfo] = []
        guard let entries = try? FileManager.default.contentsOfDirectory(atPath: "/sys/class/tty") else { return out }
        for name in entries.sorted() {
            guard name.hasPrefix("ttyUSB") || name.hasPrefix("ttyACM") else { continue }
            var dir = "/sys/class/tty/\(name)/device"
            var vid: String?
            var pid: String?
            var serial: String?
            var product: String?
            for _ in 0..<10 {
                if let v = try? String(contentsOfFile: dir + "/idVendor", encoding: .utf8).trimmingCharacters(in: .whitespacesAndNewlines), !v.isEmpty {
                    vid = v
                }
                if let p = try? String(contentsOfFile: dir + "/idProduct", encoding: .utf8).trimmingCharacters(in: .whitespacesAndNewlines), !p.isEmpty {
                    pid = p
                }
                if let s = try? String(contentsOfFile: dir + "/serial", encoding: .utf8).trimmingCharacters(in: .whitespacesAndNewlines), !s.isEmpty {
                    serial = s
                }
                if let pr = try? String(contentsOfFile: dir + "/product", encoding: .utf8).trimmingCharacters(in: .whitespacesAndNewlines), !pr.isEmpty {
                    product = pr
                }
                if vid != nil { break }
                dir = (dir as NSString).deletingLastPathComponent
            }
            out.append(SerialPortInfo(
                path: "/dev/\(name)",
                name: product ?? name,
                vendorID: vid.flatMap { UInt16($0, radix: 16) },
                productID: pid.flatMap { UInt16($0, radix: 16) },
                serialNumber: serial,
                isUSB: vid != nil
            ))
        }
        return out
    }
}
#endif

/// Generic /dev scan used when the platform enumerator misses something.
public final class FallbackPortEnumerator: PortEnumerator {
    public init() {}

    public func enumerate() -> [SerialPortInfo] {
        guard let entries = try? FileManager.default.contentsOfDirectory(atPath: "/dev") else { return [] }
        #if os(Linux)
        let prefixes = ["ttyUSB", "ttyACM"]
        #else
        let prefixes = ["cu."]
        #endif
        return entries
            .filter { prefixes.contains(where: $0.hasPrefix) }
            .sorted()
            .map { SerialPortInfo(path: "/dev/\($0)", name: $0, vendorID: nil, productID: nil, serialNumber: nil, isUSB: false) }
    }
}
