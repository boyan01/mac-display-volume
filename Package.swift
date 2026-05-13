// swift-tools-version: 6.2

import PackageDescription

let package = Package(
    name: "mac-display-volume",
    defaultLocalization: "en",
    platforms: [
        .macOS(.v15),
    ],
    products: [
        .executable(
            name: "DisplayVolume",
            targets: ["DisplayVolumeApp"]
        ),
        .executable(
            name: "DriverProbe",
            targets: ["DriverProbe"]
        ),
    ],
    targets: [
        .executableTarget(
            name: "DisplayVolumeApp",
            dependencies: ["DisplayVolumeCore"],
            path: "Sources/DisplayVolumeApp",
            resources: [
                .process("Resources"),
            ]
        ),
        .target(
            name: "DisplayVolumeCore",
            path: "Sources/DisplayVolumeCore"
        ),
        .executableTarget(
            name: "DriverProbe",
            dependencies: ["DisplayVolumeCore"],
            path: "Tools/DriverProbe"
        ),
        .testTarget(
            name: "DisplayVolumeCoreTests",
            dependencies: ["DisplayVolumeCore"],
            path: "Tests/DisplayVolumeCoreTests"
        ),
    ]
)
