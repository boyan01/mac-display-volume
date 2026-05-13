// swift-tools-version: 6.2

import PackageDescription

let package = Package(
    name: "mac-display-volume",
    platforms: [
        .macOS(.v15),
    ],
    products: [
        .executable(
            name: "DisplayVolume",
            targets: ["DisplayVolumeApp"]
        ),
    ],
    targets: [
        .executableTarget(
            name: "DisplayVolumeApp",
            dependencies: ["DisplayVolumeCore"],
            path: "Sources/DisplayVolumeApp"
        ),
        .target(
            name: "DisplayVolumeCore",
            path: "Sources/DisplayVolumeCore"
        ),
        .testTarget(
            name: "DisplayVolumeCoreTests",
            dependencies: ["DisplayVolumeCore"],
            path: "Tests/DisplayVolumeCoreTests"
        ),
    ]
)
