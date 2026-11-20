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
/// Image kernel written as source, compiled by the system when first used. Every case's scene is made ahead, one after
/// another in the background (prepare: the Launchers' home pages), and kept; the model is compiled by Core ML when the
/// Launcher starts, off the main thread.
@MainActor final class DepthScenes: ObservableObject {
    static let shared = DepthScenes()
    /// The picture and its depth (0 farthest ... 1 nearest), both `height` pixels high in the picture's own proportions
    /// (the official key art is 16:9: 910 x 512). On the Mac also the picture behind its near things, the depth then
    /// two values a pixel: the picture's, edges made sharp, and the depth behind (DepthLayers).
    /// `plain`, the Mac's: the depth as one layer had it, for the development switch that compares the two (user
    /// 2026-11-20: "mac 增加一个 开关 我可以对比 看效果").
    struct Scene { let picture: CIImage; let depth: CIImage; let height: CGFloat; var behind: CIImage? = nil; var plain: CIImage? = nil }
    static let side = 512   // the scene's height
    /// Every case's scene kept (two images of at most 2 MB each: about 35 MB for the ten games), all made ahead
    /// (prepare), so no case waits for its depth when it is turned to (user 2026-11-19: "每次 看一个封面 才开始准备
    /// 视差效果 还要等1秒 不能都提前准备好吗？"; until then only the last 3 were kept, each made when first leaned).
    static let kept = 16
    static let modelName = "DepthAnythingV2SmallF16P8"

    @Published private(set) var made: [String: Scene] = [:]
    private var order: [String] = []
    private var asked: Set<String> = []
    /// Pictures waiting for their scene, made one at a time (the model is not run on several at once).
    private var queue: [String] = []
    private var working = false

    private init() {
        Task.detached(priority: .utility) {   // compiled ahead of the first case
            let start = Date()
            let ready = Self.model != nil
            let ms = Date().timeIntervalSince(start) * 1000
            await MainActor.run { CoverFlowTrace.note(String(format: "depth model %@ in %.0f ms", ready ? "ready" : "missing", ms)) }
        }
    }

    /// The scene of a picture; nil until it is made, or when it cannot be. One asked for and not made yet goes to the
    /// front of the queue.
    func scene(of path: String) -> Scene? {
        if let known = made[path] { return known }
        if !asked.contains(path) {
            asked.insert(path)   // left in when it cannot be made: not asked again
            queue.insert(path, at: 0)
        } else if let i = queue.firstIndex(of: path), i > 0 {
            queue.insert(queue.remove(at: i), at: 0)
        }
        pump()
        return nil
    }

    /// Make the scenes of these pictures ahead, in this order (the middle case first, then outwards), one after
    /// another in the background; called when the cases are shown, and again when their pictures change.
    func prepare(_ paths: [String]) {
        for path in paths where !asked.contains(path) {
            asked.insert(path)
            queue.append(path)
        }
        pump()
    }

    /// The items from `middle` outwards: it, then one each side, two each side, ... (the order to prepare the cases).
    nonisolated static func outwards<T>(_ items: [T], from middle: Int) -> [T] {
        guard !items.isEmpty else { return [] }
        let m = min(max(middle, 0), items.count - 1)
        var out = [items[m]]
        for d in 1..<max(items.count, 1) {
            if m + d < items.count { out.append(items[m + d]) }
            if m - d >= 0 { out.append(items[m - d]) }
        }
        return out
    }

    private func pump() {
        guard !working, !queue.isEmpty else { return }
        working = true
        let path = queue.removeFirst()
        Task.detached(priority: .userInitiated) {
            let start = Date()
            let scene = Self.make(path)
            let ms = Date().timeIntervalSince(start) * 1000
            await MainActor.run {
                CoverFlowTrace.note(String(format: "depth scene %@ %@ in %.0f ms", scene == nil ? "failed" : "made",
                                           (path as NSString).lastPathComponent, ms))
                if let scene {
                    self.made[path] = scene
                    self.order.append(path)
                    while self.order.count > Self.kept {
                        let old = self.order.removeFirst()
                        self.made[old] = nil
                        self.asked.remove(old)
                    }
                }
                self.working = false
                self.pump()
            }
        }
    }

    /// The model, compiled by Core ML from the package the app carries (the Mac: Contents/Resources; the iPhone:
    /// Models/) once and kept in the caches folder, read from there on the next starts (user 2026-11-19, the iPhone
    /// compiled it on every start: "depth model ready in 7487 ms" before any case had its depth); nil without it.
    nonisolated(unsafe) static let model: VNCoreMLModel? = {
        let found = Bundle.main.url(forResource: modelName, withExtension: "mlpackage")
            ?? Bundle.main.resourceURL?.appendingPathComponent("Models/\(modelName).mlpackage")
        guard let url = found, FileManager.default.fileExists(atPath: url.path) else { return nil }
        let fm = FileManager.default
        // the kept copy is named after the package's manifest (its size and date): a new model is compiled anew
        let manifest = (try? fm.attributesOfItem(atPath: url.appendingPathComponent("Manifest.json").path)) ?? [:]
        let stamp = "\((manifest[.size] as? Int) ?? 0)-\(Int(((manifest[.modificationDate] as? Date) ?? .distantPast).timeIntervalSince1970))"
        let folder = fm.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent(Bundle.main.bundleIdentifier ?? "org.macnativegames.launcher")
        let kept = folder.appendingPathComponent("\(modelName)-\(stamp).mlmodelc")
        var start = Date()
        if !fm.fileExists(atPath: kept.path) {
            guard let compiled = try? MLModel.compileModel(at: url) else { return nil }
            try? fm.createDirectory(at: folder, withIntermediateDirectories: true)
            for old in (try? fm.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? []
            where old.lastPathComponent.hasPrefix(modelName) { try? fm.removeItem(at: old) }
            do { try fm.moveItem(at: compiled, to: kept) } catch { try? fm.copyItem(at: compiled, to: kept) }
            CoverFlowTrace.note(String(format: "depth model compiled in %.0f ms", Date().timeIntervalSince(start) * 1000))
            start = Date()
        }
        guard let ml = try? MLModel(contentsOf: kept, configuration: MLModelConfiguration()) else {
            try? fm.removeItem(at: kept)   // unreadable: compiled again on the next start
            return nil
        }
        CoverFlowTrace.note(String(format: "depth model loaded in %.0f ms", Date().timeIntervalSince(start) * 1000))
        return try? VNCoreMLModel(for: ml)
    }()

    /// The picture `side` pixels high in its own proportions and its depth at the same size, normalised to 0 (farthest)
    /// ... 1 (nearest), a little smoothed; both rendered once into memory, so drawing from them costs no more model or
    /// decoding.
    nonisolated static func make(_ path: String) -> Scene? {
        guard let model, let image = CoverImages.decode(path) else { return nil }
        let request = VNCoreMLRequest(model: model)
        request.imageCropAndScaleOption = .scaleFill
        guard (try? VNImageRequestHandler(cgImage: image).perform([request])) != nil,
              let buffer = (request.results?.first as? VNPixelBufferObservation)?.pixelBuffer else { return nil }
        let h = side, w = max(1, Int((Double(side) * Double(image.width) / Double(max(1, image.height))).rounded()))
        let bounds = CGRect(x: 0, y: 0, width: w, height: h)
        let raw = CIImage(cvPixelBuffer: buffer)
        let smooth = raw.transformed(by: CGAffineTransform(scaleX: CGFloat(w) / raw.extent.width,
                                                           y: CGFloat(h) / raw.extent.height))
            .clampedToExtent().applyingGaussianBlur(sigma: 1.5).cropped(to: bounds)
        var depth = [Float](repeating: 0, count: w * h)
        let linear = CIContext(options: [.workingColorSpace: NSNull()])
        depth.withUnsafeMutableBytes {
            linear.render(smooth, toBitmap: $0.baseAddress!, rowBytes: w * 4, bounds: bounds, format: .Rf, colorSpace: nil)
        }
        guard let lo = depth.min(), let hi = depth.max(), hi > lo else { return nil }
        for i in depth.indices { depth[i] = (depth[i] - lo) / (hi - lo) }
        guard let srgb = CGColorSpace(name: CGColorSpace.sRGB) else { return nil }
        let source = CIImage(cgImage: image)
        let scaled = source.transformed(by: CGAffineTransform(scaleX: CGFloat(w) / source.extent.width,
                                                              y: CGFloat(h) / source.extent.height))
        var rgba = [UInt8](repeating: 0, count: w * h * 4)
        rgba.withUnsafeMutableBytes {
            CIContext().render(scaled, toBitmap: $0.baseAddress!, rowBytes: w * 4, bounds: bounds, format: .RGBA8,
                               colorSpace: srgb)
        }
        let size = CGSize(width: w, height: h)
        let picture = CIImage(bitmapData: Data(rgba), bytesPerRow: w * 4, size: size, format: .RGBA8, colorSpace: srgb)
        let plain = CIImage(bitmapData: depth.withUnsafeBufferPointer { Data(buffer: $0) }, bytesPerRow: w * 4,
                            size: size, format: .Lf, colorSpace: nil)
        guard DepthLayers.on else { return Scene(picture: picture, depth: plain, height: CGFloat(h)) }
        let start = Date()
        let layers = DepthLayers.make(depth: depth, rgba: rgba, width: w, height: h)
        CoverFlowTrace.note(String(format: "depth layers %@: %d filled behind, %d unreached, in %.0f ms",
                                   (path as NSString).lastPathComponent, layers.filled, layers.unreached,
                                   Date().timeIntervalSince(start) * 1000))
        let depthImage = CIImage(bitmapData: layers.depth.withUnsafeBufferPointer { Data(buffer: $0) }, bytesPerRow: w * 4,
                                 size: size, format: .RGh, colorSpace: nil)
        let behind = CIImage(bitmapData: Data(layers.behind), bytesPerRow: w * 4, size: size, format: .RGBA8, colorSpace: srgb)
        #if os(macOS)
        return Scene(picture: picture, depth: depthImage, height: CGFloat(h), behind: behind, plain: plain)
        #else
        return Scene(picture: picture, depth: depthImage, height: CGFloat(h), behind: behind)
        #endif
    }
}

/// The picture drawn anew from its depth for one lean (parallax occlusion mapping): for each pixel, the depths are
/// walked from the nearest to the farthest in `steps`, and the first surface found to land on this pixel gives its
/// colour; so a near thing stands in front, its side is its own edge drawn on (seen as thickness), and nothing tears.
/// A part at depth d moves by shift x (d - focus): the farthest a little against the lean, the nearest the most with
/// it. Drawn by the GPU into the screen (DepthSurface.swift).
@MainActor enum DepthWarp {
    // Any nearer surface counts, so a near thing's side is drawn on as thickness. Tried 2026-11-19: counting a surface
    // only at its own depth (a near thing floating, what is behind it showing, "浮在前面、露出背景"): a figure's own
    // depths vary, so much of it was passed over as "side" and broke into specks ("不好").
    nonisolated static let steps = 48   // the depths walked per pixel (user 2026-11-19: 24 tried, "用48")
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
    private static var told = false

    /// Whether the warp can be drawn (the kernel compiled); said once in the trace.
    static var ready: Bool {
        if !told {
            told = true
            CoverFlowTrace.note("depth warp kernel " + (kernel == nil ? "missing: the picture moves whole" : "ready"))
        }
        return kernel != nil
    }

    /// The picture for a shift in the picture's pixels (x right, y down, as SwiftUI's), as a Core Image recipe the GPU
    /// draws straight to the screen (DepthWarpSurface); nil without the kernel.
    /// `layered`: from the scene's two layers where it has them; off (the Mac's development switch, to compare), the
    /// one-layer walk over the picture's own depth (the first of the two the layered depth holds).
    static func warped(_ scene: DepthScenes.Scene, shift: CGPoint, focus: CGFloat, layered: Bool = true) -> CIImage? {
        // Core Image's y runs up: a shift down on the screen is a negative one here
        if layered, let behind = scene.behind {
            return DepthLayers.warped(picture: scene.picture, behind: behind, depth: scene.depth,
                                      shift: CIVector(x: shift.x, y: -shift.y), focus: focus)
        }
        guard let kernel else { return nil }
        let margin = abs(shift.x) + abs(shift.y) + 2
        let extent = scene.picture.extent
        return kernel.apply(extent: extent, roiCallback: { _, rect in rect.insetBy(dx: -margin, dy: -margin) },
                            arguments: [scene.picture.clampedToExtent(), (scene.plain ?? scene.depth).clampedToExtent(),
                                        CIVector(x: shift.x, y: -shift.y), focus])
    }
}
