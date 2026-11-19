import CoreGraphics
import CoreImage
import CoreML
import SwiftUI
import Vision
#if os(macOS)
import AppKit
#else
import UIKit
#endif

/// A case's picture in depth, as a phone's spatial scene (user 2026-11-19: "手机上 照片做墙纸 有一种 空间3d 特效",
/// "做一个 mac上 我看看", then "能想iphone 原生的 一样好吗？" "好 做"): the picture's depth is estimated once by Depth
/// Anything V2 Small (its Core ML conversion, Apache-2.0, carried by the app as it is: tools/deps/depth_model.py), and
/// the picture is cut into `count` layers by it: layer k holds every pixel nearer than k / count (layer 0 the whole
/// picture), its edge soft. CoverFront stacks them far to near, each moved by its depth as the case leans, so the
/// picture moves as a scene in depth; where a near part moves off, the layer under it holds the same part a little less
/// moved, so no hole shows (the phone's generates what was hidden; here the picture stretches a little).
///
/// No Metal of our own (a Launcher built with only the Command Line Tools has no Metal compiler): the layers are plain
/// images, moved by SwiftUI. Only the case under the pointer is made (CoverFront asks only while it leans), the last
/// `kept` kept; the model is compiled by Core ML when the Launcher starts, off the main thread.
@MainActor final class DepthScenes: ObservableObject {
    static let shared = DepthScenes()
    struct Scene { let layers: [PlatformImage]; let depths: [CGFloat] }
    // 48 layers, so the copies of a near thing at the steps between them close up into one surface, its side seen as
    // thickness (user 2026-11-19: "如何 让人的 这些 边缘 连贯 像是一个 有厚度的 东西？", "先做 1"; at 16 they showed as
    // separate rings), each 384 px: 48 x 384 x 384 x 4 bytes = 28 MB, kept for the case under the pointer only
    static let count = 48
    static let side = 384
    static let kept = 1
    static let modelName = "DepthAnythingV2SmallF16P8"

    @Published private(set) var made: [String: Scene] = [:]
    private var order: [String] = []
    private var asked: Set<String> = []

    private init() {
        Task.detached(priority: .utility) {   // compiled ahead of the first case
            let start = Date()
            let ready = Self.model != nil
            let ms = Date().timeIntervalSince(start) * 1000
            await MainActor.run { CoverFlowTrace.note(String(format: "depth model %@ in %.0f ms", ready ? "ready" : "missing", ms)) }
        }
    }

    /// The scene of a picture; nil until it is made (asked the first time), or when it cannot be.
    func scene(of path: String) -> Scene? {
        if let known = made[path] { return known }
        guard !asked.contains(path) else { return nil }
        asked.insert(path)   // left in when it cannot be made: not asked again
        Task.detached(priority: .userInitiated) {
            let start = Date()
            let parts = Self.make(path)
            let ms = Date().timeIntervalSince(start) * 1000
            await MainActor.run {
                CoverFlowTrace.note(String(format: "depth scene %@ %@ in %.0f ms", parts == nil ? "failed" : "made",
                                           (path as NSString).lastPathComponent, ms))
            }
            guard let parts else { return }
            await MainActor.run {
                self.made[path] = Scene(layers: parts.layers.map(depthLayerImage), depths: parts.depths)
                self.order.append(path)
                while self.order.count > Self.kept {
                    let old = self.order.removeFirst()
                    self.made[old] = nil
                    self.asked.remove(old)
                }
            }
        }
        return nil
    }

    /// The model, compiled by Core ML from the package the app carries (the Mac: Contents/Resources; the iPhone:
    /// Models/); nil without it.
    nonisolated(unsafe) static let model: VNCoreMLModel? = {
        let found = Bundle.main.url(forResource: modelName, withExtension: "mlpackage")
            ?? Bundle.main.resourceURL?.appendingPathComponent("Models/\(modelName).mlpackage")
        guard let url = found, FileManager.default.fileExists(atPath: url.path),
              let compiled = try? MLModel.compileModel(at: url),
              let ml = try? MLModel(contentsOf: compiled, configuration: MLModelConfiguration()) else { return nil }
        return try? VNCoreMLModel(for: ml)
    }()

    /// The layers (side x side, premultiplied RGBA) and the depth each moves by (0 farthest ... 1 nearest).
    nonisolated static func make(_ path: String) -> (layers: [CGImage], depths: [CGFloat])? {
        guard let model, let image = CoverImages.decode(path) else { return nil }
        let request = VNCoreMLRequest(model: model)
        request.imageCropAndScaleOption = .scaleFill
        guard (try? VNImageRequestHandler(cgImage: image).perform([request])) != nil,
              let buffer = (request.results?.first as? VNPixelBufferObservation)?.pixelBuffer else { return nil }
        let n = side, bounds = CGRect(x: 0, y: 0, width: side, height: side)
        // the depth at side x side, a little smoothed, 0 farthest ... 1 nearest (the model's relative inverse depth)
        let raw = CIImage(cvPixelBuffer: buffer)
        let depthImage = raw.transformed(by: CGAffineTransform(scaleX: CGFloat(n) / raw.extent.width,
                                                               y: CGFloat(n) / raw.extent.height))
            .clampedToExtent().applyingGaussianBlur(sigma: 1.5).cropped(to: bounds)
        var depth = [Float](repeating: 0, count: n * n)
        let linear = CIContext(options: [.workingColorSpace: NSNull()])
        depth.withUnsafeMutableBytes {
            linear.render(depthImage, toBitmap: $0.baseAddress!, rowBytes: n * 4, bounds: bounds, format: .Rf, colorSpace: nil)
        }
        guard let lo = depth.min(), let hi = depth.max(), hi > lo else { return nil }
        for i in depth.indices { depth[i] = (depth[i] - lo) / (hi - lo) }
        // the picture at the same size and the same row order
        guard let srgb = CGColorSpace(name: CGColorSpace.sRGB) else { return nil }
        let source = CIImage(cgImage: image)
        let picture = source.transformed(by: CGAffineTransform(scaleX: CGFloat(n) / source.extent.width,
                                                               y: CGFloat(n) / source.extent.height))
        var rgba = [UInt8](repeating: 0, count: n * n * 4)
        rgba.withUnsafeMutableBytes {
            CIContext().render(picture, toBitmap: $0.baseAddress!, rowBytes: n * 4, bounds: bounds, format: .RGBA8,
                               colorSpace: srgb)
        }
        // layer k: the pixels nearer than k / count, faded in over a band either side of it
        var layers: [CGImage] = [], depths: [CGFloat] = []
        let feather = 0.5 / Float(count)   // one layer wide (user 2026-11-19: "0.5 / 层数", after trying 0.1 and 1)
        for k in 0..<count {
            let t = Float(k) / Float(count)
            var pixels = rgba
            if k > 0 {
                for i in 0..<(n * n) {
                    let x = min(1, max(0, (depth[i] - (t - feather)) / (2 * feather)))
                    let a = x * x * (3 - 2 * x)
                    if a < 1 {
                        let o = i * 4
                        for c in 0..<4 { pixels[o + c] = UInt8(Float(pixels[o + c]) * a) }
                    }
                }
            }
            guard let layer = premultiplied(pixels, side: n, space: srgb) else { return nil }
            layers.append(layer)
            depths.append(CGFloat(t))
        }
        return (layers, depths)
    }

    private nonisolated static func premultiplied(_ pixels: [UInt8], side n: Int, space: CGColorSpace) -> CGImage? {
        guard let provider = CGDataProvider(data: Data(pixels) as CFData) else { return nil }
        return CGImage(width: n, height: n, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: n * 4, space: space,
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue), provider: provider,
                       decode: nil, shouldInterpolate: true, intent: .defaultIntent)
    }
}

/// A layer as the platform holds an image.
private func depthLayerImage(_ image: CGImage) -> PlatformImage {
    #if os(macOS)
    NSImage(cgImage: image, size: NSSize(width: image.width, height: image.height))
    #else
    UIImage(cgImage: image)
    #endif
}
