import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

private struct Point {
  let x: CGFloat
  let y: CGFloat
}

private let uprightURL = URL(string: "https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/K150-stickS3_main-products_07.webp")!
private let flatURL = URL(string: "https://shop.m5stack.com/cdn/shop/files/4_e5191b04-4dbf-4212-af94-1de732541a92_1200x1200.webp?v=1770176878")!

private func loadImage(from url: URL) throws -> CGImage {
  let data = try Data(contentsOf: url)
  guard
    let source = CGImageSourceCreateWithData(data as CFData, nil),
    let image = CGImageSourceCreateImageAtIndex(source, 0, nil)
  else {
    throw NSError(domain: "voxstick-doc-assets", code: 1, userInfo: [
      NSLocalizedDescriptionKey: "Could not decode \(url.absoluteString)"
    ])
  }
  return image
}

private func makeContext(width: Int, height: Int) throws -> CGContext {
  let colorSpace = CGColorSpaceCreateDeviceRGB()
  guard let context = CGContext(
    data: nil,
    width: width,
    height: height,
    bitsPerComponent: 8,
    bytesPerRow: width * 4,
    space: colorSpace,
    bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
  ) else {
    throw NSError(domain: "voxstick-doc-assets", code: 2, userInfo: [
      NSLocalizedDescriptionKey: "Could not create CGContext"
    ])
  }
  return context
}

private func imageByRemovingWhiteBackground(_ image: CGImage) throws -> CGImage {
  let width = image.width
  let height = image.height
  let bytesPerRow = width * 4
  let bitmapInfo = CGBitmapInfo.byteOrder32Big.rawValue | CGImageAlphaInfo.premultipliedLast.rawValue
  let colorSpace = CGColorSpaceCreateDeviceRGB()
  var data = Data(repeating: 0, count: bytesPerRow * height)

  try data.withUnsafeMutableBytes { rawBuffer in
    guard let baseAddress = rawBuffer.baseAddress else {
      throw NSError(domain: "voxstick-doc-assets", code: 5, userInfo: [
        NSLocalizedDescriptionKey: "Could not allocate image buffer"
      ])
    }
    guard let context = CGContext(
      data: baseAddress,
      width: width,
      height: height,
      bitsPerComponent: 8,
      bytesPerRow: bytesPerRow,
      space: colorSpace,
      bitmapInfo: bitmapInfo
    ) else {
      throw NSError(domain: "voxstick-doc-assets", code: 6, userInfo: [
        NSLocalizedDescriptionKey: "Could not create matte CGContext"
      ])
    }

    context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))

    let pixels = rawBuffer.bindMemory(to: UInt8.self)
    for offset in stride(from: 0, to: pixels.count, by: 4) {
      let red = Int(pixels[offset])
      let green = Int(pixels[offset + 1])
      let blue = Int(pixels[offset + 2])
      let maxChannel = max(red, green, blue)
      let minChannel = min(red, green, blue)
      let isNeutralWhite = maxChannel - minChannel < 16

      guard isNeutralWhite, minChannel > 228 else { continue }

      let opacity: CGFloat
      if minChannel >= 248 {
        opacity = 0
      } else {
        opacity = CGFloat(248 - minChannel) / 20
      }

      pixels[offset] = UInt8(CGFloat(red) * opacity)
      pixels[offset + 1] = UInt8(CGFloat(green) * opacity)
      pixels[offset + 2] = UInt8(CGFloat(blue) * opacity)
      pixels[offset + 3] = UInt8(CGFloat(pixels[offset + 3]) * opacity)
    }
  }

  guard
    let provider = CGDataProvider(data: data as CFData),
    let keyedImage = CGImage(
      width: width,
      height: height,
      bitsPerComponent: 8,
      bitsPerPixel: 32,
      bytesPerRow: bytesPerRow,
      space: colorSpace,
      bitmapInfo: CGBitmapInfo(rawValue: bitmapInfo),
      provider: provider,
      decode: nil,
      shouldInterpolate: true,
      intent: .defaultIntent
    )
  else {
    throw NSError(domain: "voxstick-doc-assets", code: 7, userInfo: [
      NSLocalizedDescriptionKey: "Could not create keyed image"
    ])
  }

  return keyedImage
}

private func writePNG(_ image: CGImage, to url: URL) throws {
  guard
    let destination = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)
  else {
    throw NSError(domain: "voxstick-doc-assets", code: 3, userInfo: [
      NSLocalizedDescriptionKey: "Could not create PNG destination"
    ])
  }
  CGImageDestinationAddImage(destination, image, nil)
  if !CGImageDestinationFinalize(destination) {
    throw NSError(domain: "voxstick-doc-assets", code: 4, userInfo: [
      NSLocalizedDescriptionKey: "Could not write \(url.path)"
    ])
  }
}

private func fillRoundedRect(_ context: CGContext, rect: CGRect, radius: CGFloat, color: CGColor) {
  context.setFillColor(color)
  context.addPath(CGPath(roundedRect: rect, cornerWidth: radius, cornerHeight: radius, transform: nil))
  context.fillPath()
}

private func addPolygon(_ context: CGContext, _ points: [Point]) {
  guard let first = points.first else { return }
  context.beginPath()
  context.move(to: CGPoint(x: first.x, y: first.y))
  for point in points.dropFirst() {
    context.addLine(to: CGPoint(x: point.x, y: point.y))
  }
  context.closePath()
}

private func fillPolygon(_ context: CGContext, _ points: [Point], color: CGColor) {
  context.setFillColor(color)
  addPolygon(context, points)
  context.fillPath()
}

private func drawSceneBackground(_ context: CGContext, width: Int, height: Int, horizonY: CGFloat) {
  let canvas = CGRect(x: 0, y: 0, width: width, height: height)
  context.setFillColor(CGColor(red: 0.988, green: 0.992, blue: 0.996, alpha: 1))
  context.fill(canvas)

  context.saveGState()
  context.translateBy(x: 0, y: CGFloat(height))
  context.scaleBy(x: 1, y: -1)

  let floor = [
    Point(x: 70, y: horizonY),
    Point(x: CGFloat(width - 70), y: horizonY),
    Point(x: CGFloat(width), y: CGFloat(height)),
    Point(x: 0, y: CGFloat(height))
  ]
  fillPolygon(
    context,
    floor,
    color: CGColor(red: 0.944, green: 0.962, blue: 0.976, alpha: 1)
  )

  context.setStrokeColor(CGColor(red: 0.832, green: 0.872, blue: 0.910, alpha: 1))
  context.setLineWidth(2)
  context.move(to: CGPoint(x: 70, y: horizonY))
  context.addLine(to: CGPoint(x: CGFloat(width - 70), y: horizonY))
  context.strokePath()

  context.setStrokeColor(CGColor(red: 0.830, green: 0.875, blue: 0.910, alpha: 0.65))
  context.setLineWidth(1)
  let vanishingPoint = CGPoint(x: CGFloat(width) * 0.5, y: horizonY)
  for x in stride(from: -120, through: width + 120, by: 180) {
    context.move(to: vanishingPoint)
    context.addLine(to: CGPoint(x: x, y: height))
  }
  for y in stride(from: Int(horizonY) + 98, through: height + 20, by: 112) {
    let lineY = CGFloat(y)
    let inset = CGFloat(y - Int(horizonY)) * 0.22
    context.move(to: CGPoint(x: max(CGFloat(0), CGFloat(70) - inset), y: lineY))
    context.addLine(to: CGPoint(x: min(CGFloat(width), CGFloat(width - 70) + inset), y: lineY))
  }
  context.strokePath()

  context.restoreGState()
}

private func drawSoftEllipse(
  _ context: CGContext,
  center: CGPoint,
  size: CGSize,
  angle: CGFloat,
  alpha: CGFloat
) {
  context.saveGState()
  context.translateBy(x: center.x, y: center.y)
  context.rotate(by: angle)
  for index in 0..<6 {
    let progress = CGFloat(index) / 6
    let rect = CGRect(
      x: -size.width * (1 + progress * 0.20) / 2,
      y: -size.height * (1 + progress * 0.75) / 2,
      width: size.width * (1 + progress * 0.20),
      height: size.height * (1 + progress * 0.75)
    )
    context.setFillColor(CGColor(red: 0.102, green: 0.137, blue: 0.180, alpha: alpha * (1 - progress) * 0.24))
    context.fillEllipse(in: rect)
  }
  context.restoreGState()
}

private func eraseFlatProductAnnotations(_ context: CGContext) {
  context.saveGState()
  context.translateBy(x: 0, y: 1200)
  context.scaleBy(x: 1, y: -1)

  let white = CGColor(red: 1, green: 1, blue: 1, alpha: 1)
  context.setFillColor(white)
  context.setStrokeColor(white)
  context.setLineWidth(52)
  context.setLineCap(.round)
  context.setLineJoin(.round)

  let textMasks = [
    CGRect(x: 100, y: 740, width: 280, height: 140),
    CGRect(x: 910, y: 650, width: 260, height: 190),
    CGRect(x: 750, y: 850, width: 300, height: 190)
  ]
  for mask in textMasks {
    context.fill(mask)
  }

  context.move(to: CGPoint(x: 132, y: 520))
  context.addLine(to: CGPoint(x: 540, y: 1010))
  context.move(to: CGPoint(x: 942, y: 682))
  context.addLine(to: CGPoint(x: 942, y: 804))
  context.move(to: CGPoint(x: 650, y: 1012))
  context.addLine(to: CGPoint(x: 944, y: 834))
  context.strokePath()

  let arrowMasks = [
    CGRect(x: 96, y: 488, width: 74, height: 78),
    CGRect(x: 506, y: 970, width: 76, height: 78),
    CGRect(x: 902, y: 650, width: 78, height: 64),
    CGRect(x: 902, y: 778, width: 78, height: 64),
    CGRect(x: 612, y: 974, width: 78, height: 74),
    CGRect(x: 902, y: 796, width: 78, height: 78)
  ]
  for mask in arrowMasks {
    context.fill(mask)
  }

  context.restoreGState()
}

private func drawRects(_ context: CGContext, rects: [CGRect]) {
  for rect in rects {
    context.fill(rect)
  }
}

private func drawMicGlyph(_ context: CGContext, color: CGColor, muted: Bool) {
  context.setFillColor(color)

  let rects = [
    CGRect(x: 57, y: 68, width: 20, height: 4),
    CGRect(x: 52, y: 72, width: 30, height: 4),
    CGRect(x: 49, y: 76, width: 4, height: 46),
    CGRect(x: 81, y: 76, width: 4, height: 46),
    CGRect(x: 52, y: 122, width: 30, height: 4),
    CGRect(x: 57, y: 126, width: 20, height: 4),
    CGRect(x: 37, y: 102, width: 4, height: 22),
    CGRect(x: 93, y: 102, width: 4, height: 22),
    CGRect(x: 41, y: 130, width: 52, height: 4),
    CGRect(x: 65, y: 134, width: 4, height: 24),
    CGRect(x: 47, y: 158, width: 40, height: 4)
  ]
  drawRects(context, rects: rects)

  context.setFillColor(CGColor(red: 0.063, green: 0.141, blue: 0.176, alpha: 1))
  context.fill(CGRect(x: 37, y: 178, width: 61, height: 4))

  if muted {
    context.setStrokeColor(CGColor(red: 0.937, green: 0.267, blue: 0.267, alpha: 1))
    context.setLineWidth(5)
    context.setLineCap(.round)
    context.move(to: CGPoint(x: 39, y: 154))
    context.addLine(to: CGPoint(x: 95, y: 66))
    context.strokePath()
  } else {
    context.setFillColor(CGColor(red: 0.086, green: 0.306, blue: 0.388, alpha: 1))
    context.fill(CGRect(x: 37, y: 178, width: 24, height: 4))
  }
}

private func drawScreenContent(_ context: CGContext, muted: Bool) {
  let glyphColor = muted
    ? CGColor(red: 0.80, green: 0.84, blue: 0.89, alpha: 1)
    : CGColor(red: 0.22, green: 0.85, blue: 1.00, alpha: 1)
  drawMicGlyph(context, color: glyphColor, muted: muted)
}

private func drawUprightAsset(to output: URL) throws {
  let base = try imageByRemovingWhiteBackground(loadImage(from: uprightURL))
  let width = base.width
  let height = base.height
  let context = try makeContext(width: width, height: height)

  drawSceneBackground(context, width: width, height: height, horizonY: 835)
  context.saveGState()
  context.translateBy(x: 0, y: CGFloat(height))
  context.scaleBy(x: 1, y: -1)
  drawSoftEllipse(
    context,
    center: CGPoint(x: 600, y: 1014),
    size: CGSize(width: 390, height: 54),
    angle: 0,
    alpha: 1
  )
  context.restoreGState()

  context.draw(base, in: CGRect(x: 0, y: 0, width: width, height: height))

  context.saveGState()
  context.translateBy(x: 0, y: CGFloat(height))
  context.scaleBy(x: 1, y: -1)

  let screen = CGRect(x: 480, y: 276, width: 248, height: 420)
  fillRoundedRect(
    context,
    rect: screen,
    radius: 9,
    color: CGColor(red: 0.008, green: 0.024, blue: 0.090, alpha: 1)
  )

  context.saveGState()
  context.translateBy(x: screen.minX, y: screen.minY)
  context.scaleBy(x: screen.width / 135, y: screen.height / 240)
  drawScreenContent(context, muted: false)
  context.restoreGState()
  context.restoreGState()

  guard let image = context.makeImage() else { return }
  try writePNG(image, to: output)
}

private func drawFlatAsset(to output: URL) throws {
  let base = try loadImage(from: flatURL)
  let width = base.width
  let height = base.height
  let context = try makeContext(width: width, height: height)

  // Use the official three-quarter photo as-is so the natural tabletop shadow
  // stays intact. Only the LCD content is replaced with the VoxStick state.
  context.draw(base, in: CGRect(x: 0, y: 0, width: width, height: height))
  eraseFlatProductAnnotations(context)

  context.saveGState()
  context.translateBy(x: 0, y: CGFloat(height))
  context.scaleBy(x: 1, y: -1)

  let topLeft = Point(x: 280, y: 378)
  let topRight = Point(x: 490, y: 264)
  let bottomRight = Point(x: 680, y: 536)
  let bottomLeft = Point(x: 465, y: 653)
  let screenPolygon = [topLeft, topRight, bottomRight, bottomLeft]

  context.saveGState()
  addPolygon(context, screenPolygon)
  context.clip()

  let xAxis = CGVector(dx: topRight.x - topLeft.x, dy: topRight.y - topLeft.y)
  let yAxis = CGVector(dx: bottomLeft.x - topLeft.x, dy: bottomLeft.y - topLeft.y)
  context.concatenate(CGAffineTransform(
    a: xAxis.dx / 135,
    b: xAxis.dy / 135,
    c: yAxis.dx / 240,
    d: yAxis.dy / 240,
    tx: topLeft.x,
    ty: topLeft.y
  ))
  drawScreenContent(context, muted: true)
  context.restoreGState()
  context.restoreGState()

  guard let image = context.makeImage() else { return }
  try writePNG(image, to: output)
}

let outputDirectory = URL(fileURLWithPath: CommandLine.arguments.dropFirst().first ?? "docs/assets")
try FileManager.default.createDirectory(at: outputDirectory, withIntermediateDirectories: true)
try drawUprightAsset(to: outputDirectory.appendingPathComponent("voxstick-upright-live.png"))
try drawFlatAsset(to: outputDirectory.appendingPathComponent("voxstick-flat-muted.png"))
