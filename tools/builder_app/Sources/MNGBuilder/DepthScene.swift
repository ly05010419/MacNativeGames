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
/// "做一个 mac上 我看看", "能想iphone 原生的 一样好吗？" "好 做"): the picture's depth is estimated once by Depth Anything
/// V2 Small (its Core ML conversion, Apache-2.0, carried by the app as it is: tools/deps/depth_model.py). As the case
/// leans, DepthWarp draws the picture anew pixel by pixel from it, each part moved by its own depth (user 2026-11-19:
/// "开始做2", after 48 layers stacked: their steps rang round a figure, and 48 images a frame dropped every fifth one).
///
/// No Metal of our own (a Launcher built with only the Command Line Tools has no Metal compiler): the warp is a Core
/// Image kernel written as source, compiled by the system when first used. Only the case under the pointer is made
/// (CoverFront asks only while it leans), the last `kept` kept; the model is compiled by Core ML when the Launcher
/// starts, off the main thread.
@MainActor final class DepthScenes: ObservableObject {
    static let shared = DepthScenes()
    /// The picture and its depth (0 farthest ... 1 nearest), both side x side.
    struct Scene { let picture: CIImage; let depth: CIImage; let side: CGFloat }
    static let side = 512
    static let kept = 3   // two 1 MB images each
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
            let scene = Self.make(path)
            let ms = Date().timeIntervalSince(start) * 1000
            await MainActor.run {
                CoverFlowTrace.note(String(format: "depth scene %@ %@ in %.0f ms", scene == nil ? "failed" : "made",
                                           (path as NSString).lastPathComponent, ms))
                guard let scene else { return }
                self.made[path] = scene
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

    /// The picture at side x side and its depth, normalised to 0 (farthest) ... 1 (nearest), a little smoothed; both
    /// rendered once into memory, so drawing from them costs no more model or decoding.
    nonisolated static func make(_ path: String) -> Scene? {
        guard let model, let image = CoverImages.decode(path) else { return nil }
        let request = VNCoreMLRequest(model: model)
        request.imageCropAndScaleOption = .scaleFill
        guard (try? VNImageRequestHandler(cgImage: image).perform([request])) != nil,
              let buffer = (request.results?.first as? VNPixelBufferObservation)?.pixelBuffer else { return nil }
        let n = side, bounds = CGRect(x: 0, y: 0, width: side, height: side)
        let raw = CIImage(cvPixelBuffer: buffer)
        let smooth = raw.transformed(by: CGAffineTransform(scaleX: CGFloat(n) / raw.extent.width,
                                                           y: CGFloat(n) / raw.extent.height))
            .clampedToExtent().applyingGaussianBlur(sigma: 1.5).cropped(to: bounds)
        var depth = [Float](repeating: 0, count: n * n)
        let linear = CIContext(options: [.workingColorSpace: NSNull()])
        depth.withUnsafeMutableBytes {
            linear.render(smooth, toBitmap: $0.baseAddress!, rowBytes: n * 4, bounds: bounds, format: .Rf, colorSpace: nil)
        }
        guard let lo = depth.min(), let hi = depth.max(), hi > lo else { return nil }
        for i in depth.indices { depth[i] = (depth[i] - lo) / (hi - lo) }
        let depthImage = CIImage(bitmapData: depth.withUnsafeBufferPointer { Data(buffer: $0) }, bytesPerRow: n * 4,
                                 size: CGSize(width: n, height: n), format: .Lf, colorSpace: nil)
        guard let srgb = CGColorSpace(name: CGColorSpace.sRGB) else { return nil }
        let source = CIImage(cgImage: image)
        let scaled = source.transformed(by: CGAffineTransform(scaleX: CGFloat(n) / source.extent.width,
                                                              y: CGFloat(n) / source.extent.height))
        var rgba = [UInt8](repeating: 0, count: n * n * 4)
        rgba.withUnsafeMutableBytes {
            CIContext().render(scaled, toBitmap: $0.baseAddress!, rowBytes: n * 4, bounds: bounds, format: .RGBA8,
                               colorSpace: srgb)
        }
        let picture = CIImage(bitmapData: Data(rgba), bytesPerRow: n * 4, size: CGSize(width: n, height: n),
                              format: .RGBA8, colorSpace: srgb)
        return Scene(picture: picture, depth: depthImage, side: CGFloat(n))
    }
}

/// The picture drawn anew from its depth for one lean (parallax occlusion mapping): for each pixel, the depths are
/// walked from the nearest to the farthest in `steps`, and the first surface found to land on this pixel gives its
/// colour; so a near thing stands in front, its side is its own edge drawn on (seen as thickness), and nothing tears.
/// A part at depth d moves by shift x (d - focus): the farthest a little against the lean, the nearest the most with
/// it. The last frame is kept: the case's reflection asks for the same one.
@MainActor enum DepthWarp {
    // Any nearer surface counts, so a near thing's side is drawn on as thickness. Tried 2026-11-19: counting a surface
    // only at its own depth (a near thing floating, what is behind it showing, "浮在前面、露出背景"): a figure's own
    // depths vary, so much of it was passed over as "side" and broke into specks ("不好").
    static let steps = 48
    private static let source = """
    kernel vec4 depthParallax(sampler image, sampler depth, vec2 shift, float focus) {
        vec2 p = destCoord();
        vec4 colour = sample(image, samplerTransform(image, p + shift * focus));
        float found = 0.0;
        for (int i = 0; i < \(steps); i++) {
            float z = 1.0 - float(i) / \(steps - 1).0;
            vec2 q = p - shift * (z - focus);
            float d = sample(depth, samplerTransform(depth, q)).r;
            float hit = step(z, d) * (1.0 - found);
            colour = mix(colour, sample(image, samplerTransform(image, q)), hit);
            found = max(found, hit);
        }
        return colour;
    }
    """
    /// nil where the system will not compile it: CoverFront then moves the picture whole.
    static let kernel: CIKernel? = CIKernel(source: source)
    private static let context = CIContext(options: [.cacheIntermediates: false])
    private static var last: (key: String, image: PlatformImage)?
    private static var told = false

    /// The picture for a shift in the picture's pixels (x right, y down, as SwiftUI's), or nil.
    static func image(_ scene: DepthScenes.Scene, path: String, shift: CGPoint, focus: CGFloat) -> PlatformImage? {
        let key = "\(path) \((shift.x * 4).rounded()) \((shift.y * 4).rounded())"
        if let last, last.key == key { return last.image }
        if !told {
            told = true
            CoverFlowTrace.note("depth warp kernel " + (kernel == nil ? "missing: the picture moves whole" : "ready"))
        }
        guard let kernel else { return nil }
        let start = Date()
        defer { if CoverFlowTrace.on { CoverFlowTrace.note(String(format: "depth warp %.1f ms", Date().timeIntervalSince(start) * 1000)) } }
        let margin = abs(shift.x) + abs(shift.y) + 2
        let extent = scene.picture.extent
        // Core Image's y runs up: a shift down on the screen is a negative one here
        guard let out = kernel.apply(extent: extent, roiCallback: { _, rect in rect.insetBy(dx: -margin, dy: -margin) },
                                     arguments: [scene.picture.clampedToExtent(), scene.depth.clampedToExtent(),
                                                 CIVector(x: shift.x, y: -shift.y), focus]),
              let cg = context.createCGImage(out, from: extent) else { return nil }
        #if os(macOS)
        let made = NSImage(cgImage: cg, size: NSSize(width: cg.width, height: cg.height))
        #else
        let made = UIImage(cgImage: cg)
        #endif
        last = (key, made)
        return made
    }
}
