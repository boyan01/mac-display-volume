#!/usr/bin/env swift

import AppKit
import Foundation

guard CommandLine.arguments.count == 2 else {
    fputs("usage: generate-app-icon.swift <output.icns>\n", stderr)
    exit(2)
}

let outputURL = URL(fileURLWithPath: CommandLine.arguments[1])
let fileManager = FileManager.default
let iconsetURL = fileManager.temporaryDirectory
    .appendingPathComponent("MacDisplayVolumeIcon-\(UUID().uuidString)")
    .appendingPathExtension("iconset")

try fileManager.createDirectory(at: iconsetURL, withIntermediateDirectories: true)
defer {
    try? fileManager.removeItem(at: iconsetURL)
}

func drawIcon(size: CGFloat) -> NSImage {
    let image = NSImage(size: NSSize(width: size, height: size))
    image.lockFocus()
    defer { image.unlockFocus() }

    NSColor.clear.setFill()
    NSRect(x: 0, y: 0, width: size, height: size).fill()

    let scale = size / 1024.0
    func rect(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat) -> NSRect {
        NSRect(x: x * scale, y: y * scale, width: w * scale, height: h * scale)
    }
    func color(_ red: CGFloat, _ green: CGFloat, _ blue: CGFloat) -> NSColor {
        NSColor(red: red / 255.0, green: green / 255.0, blue: blue / 255.0, alpha: 1.0)
    }

    let shadow = NSShadow()
    shadow.shadowColor = NSColor.black.withAlphaComponent(0.22)
    shadow.shadowOffset = NSSize(width: 0, height: -20 * scale)
    shadow.shadowBlurRadius = 34 * scale

    NSGraphicsContext.saveGraphicsState()
    shadow.set()
    let background = NSBezierPath(roundedRect: rect(76, 76, 872, 872), xRadius: 210 * scale, yRadius: 210 * scale)
    color(61, 137, 255).setFill()
    background.fill()
    NSGraphicsContext.restoreGraphicsState()

    let topGlow = NSBezierPath(ovalIn: rect(168, 606, 688, 250))
    color(111, 205, 255).withAlphaComponent(0.50).setFill()
    topGlow.fill()

    let monitorShadow = NSShadow()
    monitorShadow.shadowColor = NSColor.black.withAlphaComponent(0.20)
    monitorShadow.shadowOffset = NSSize(width: 0, height: -12 * scale)
    monitorShadow.shadowBlurRadius = 24 * scale

    NSGraphicsContext.saveGraphicsState()
    monitorShadow.set()
    let monitor = NSBezierPath(roundedRect: rect(206, 308, 612, 390), xRadius: 54 * scale, yRadius: 54 * scale)
    color(28, 42, 72).setFill()
    monitor.fill()
    NSGraphicsContext.restoreGraphicsState()

    let screen = NSBezierPath(roundedRect: rect(252, 360, 520, 286), xRadius: 34 * scale, yRadius: 34 * scale)
    color(237, 249, 255).setFill()
    screen.fill()

    let screenGlow = NSBezierPath(roundedRect: rect(278, 536, 468, 74), xRadius: 30 * scale, yRadius: 30 * scale)
    color(183, 238, 255).setFill()
    screenGlow.fill()

    color(28, 42, 72).setFill()
    NSBezierPath(roundedRect: rect(462, 270, 100, 58), xRadius: 22 * scale, yRadius: 22 * scale).fill()
    NSBezierPath(roundedRect: rect(360, 228, 304, 58), xRadius: 28 * scale, yRadius: 28 * scale).fill()

    let speaker = NSBezierPath()
    speaker.move(to: NSPoint(x: 364 * scale, y: 454 * scale))
    speaker.line(to: NSPoint(x: 432 * scale, y: 454 * scale))
    speaker.line(to: NSPoint(x: 528 * scale, y: 530 * scale))
    speaker.line(to: NSPoint(x: 528 * scale, y: 374 * scale))
    speaker.line(to: NSPoint(x: 432 * scale, y: 454 * scale))
    speaker.line(to: NSPoint(x: 364 * scale, y: 454 * scale))
    speaker.close()
    color(255, 195, 66).setFill()
    speaker.fill()

    color(246, 136, 47).setStroke()
    let arcLineWidth = 28 * scale
    for (index, radius) in [94.0, 150.0].enumerated() {
        let arc = NSBezierPath()
        arc.lineWidth = arcLineWidth - CGFloat(index) * 4 * scale
        arc.lineCapStyle = .round
        arc.appendArc(
            withCenter: NSPoint(x: 548 * scale, y: 454 * scale),
            radius: CGFloat(radius) * scale,
            startAngle: -42,
            endAngle: 42,
            clockwise: false
        )
        arc.stroke()
    }

    let faceLine = NSBezierPath()
    faceLine.lineWidth = 11 * scale
    faceLine.lineCapStyle = .round
    color(28, 42, 72).setStroke()
    faceLine.move(to: NSPoint(x: 380 * scale, y: 560 * scale))
    faceLine.line(to: NSPoint(x: 380 * scale, y: 560 * scale))
    faceLine.move(to: NSPoint(x: 466 * scale, y: 560 * scale))
    faceLine.line(to: NSPoint(x: 466 * scale, y: 560 * scale))
    faceLine.stroke()

    let smile = NSBezierPath()
    smile.lineWidth = 12 * scale
    smile.lineCapStyle = .round
    smile.move(to: NSPoint(x: 366 * scale, y: 504 * scale))
    smile.curve(
        to: NSPoint(x: 480 * scale, y: 504 * scale),
        controlPoint1: NSPoint(x: 394 * scale, y: 476 * scale),
        controlPoint2: NSPoint(x: 452 * scale, y: 476 * scale)
    )
    smile.stroke()

    let knob = NSBezierPath(ovalIn: rect(650, 248, 144, 144))
    color(255, 222, 91).setFill()
    knob.fill()
    color(28, 42, 72).withAlphaComponent(0.12).setStroke()
    knob.lineWidth = 8 * scale
    knob.stroke()

    return image
}

func writePNG(size: Int, name: String) throws {
    let image = drawIcon(size: CGFloat(size))
    guard
        let tiff = image.tiffRepresentation,
        let bitmap = NSBitmapImageRep(data: tiff),
        let png = bitmap.representation(using: .png, properties: [:])
    else {
        throw NSError(domain: "Icon", code: 1)
    }
    try png.write(to: iconsetURL.appendingPathComponent(name))
}

let icons: [(Int, String)] = [
    (16, "icon_16x16.png"),
    (32, "icon_16x16@2x.png"),
    (32, "icon_32x32.png"),
    (64, "icon_32x32@2x.png"),
    (128, "icon_128x128.png"),
    (256, "icon_128x128@2x.png"),
    (256, "icon_256x256.png"),
    (512, "icon_256x256@2x.png"),
    (512, "icon_512x512.png"),
    (1024, "icon_512x512@2x.png"),
]

for icon in icons {
    try writePNG(size: icon.0, name: icon.1)
}

try? fileManager.removeItem(at: outputURL)

let process = Process()
process.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
process.arguments = ["-c", "icns", iconsetURL.path, "-o", outputURL.path]
try process.run()
process.waitUntilExit()

if process.terminationStatus != 0 {
    exit(process.terminationStatus)
}
