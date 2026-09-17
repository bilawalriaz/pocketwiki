// swift-tools-version:5.10
import PackageDescription

// ESPFlasher — detect, prepare and flash ESP32-family devices.
//
// Core (ESPFlashCore) is pure Swift, portable to macOS and Linux (and the
// foundation for a Windows port). The `espflasher` CLI and the `ESPFlashGUI`
// macOS app are thin wrappers over the core.
//
// On Linux build the CLI only:  swift build --product espflasher
let package = Package(
    name: "espflasher",
    platforms: [.macOS(.v13)],
    products: [
        .library(name: "ESPFlashCore", targets: ["ESPFlashCore"]),
        .executable(name: "espflasher", targets: ["espflasher"]),
        .executable(name: "ESPFlashGUI", targets: ["ESPFlashGUI"]),
    ],
    dependencies: [
        .package(url: "https://github.com/apple/swift-argument-parser.git", from: "1.3.0"),
    ],
    targets: [
        .target(
            name: "ESPFlashCore"
        ),
        .executableTarget(
            name: "espflasher",
            dependencies: [
                "ESPFlashCore",
                .product(name: "ArgumentParser", package: "swift-argument-parser"),
            ]
        ),
        .executableTarget(
            name: "ESPFlashGUI",
            dependencies: ["ESPFlashCore"],
            path: "Sources/ESPFlashGUI",
            resources: [.process("Resources")]
        ),
        .testTarget(
            name: "ESPFlashCoreTests",
            dependencies: ["ESPFlashCore"]
        ),
    ]
)
