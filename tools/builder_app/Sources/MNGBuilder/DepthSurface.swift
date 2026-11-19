import CoreImage
import Metal
import QuartzCore
import SwiftUI
#if os(macOS)
import AppKit
#else
import UIKit
#endif

/// The leaning case's picture in depth, drawn by the GPU straight into a Metal layer (user 2026-11-19: "ab 都做"). It was
/// drawn by Core Image into a new CGImage each frame: the GPU's result copied back into memory and uploaded again as a
/// new texture, some 50 times a second while the case leaned, and the phone ran hot ("thermal serious", a frame in five
/// dropped). Now the kernel's output goes from the GPU to the screen, and nothing is drawn when the shift has not moved
/// (DepthWarpSurface.show). No shader of our own: Core Image compiles the kernel source (DepthWarp) at run time.
struct DepthWarpView {
    let scene: DepthScenes.Scene
    /// In the picture's pixels (x right, y down), already rounded (CoverFront).
    let shift: CGPoint
    let focus: CGFloat
}

#if os(macOS)
extension DepthWarpView: NSViewRepresentable {
    func makeNSView(context: Context) -> DepthWarpSurface { DepthWarpSurface() }
    func updateNSView(_ view: DepthWarpSurface, context: Context) { view.show(scene, shift, focus) }
}
#else
extension DepthWarpView: UIViewRepresentable {
    func makeUIView(context: Context) -> DepthWarpSurface { DepthWarpSurface() }
    func updateUIView(_ view: DepthWarpSurface, context: Context) { view.show(scene, shift, focus) }
}
#endif

#if os(macOS)
typealias DepthPlatformView = NSView
#else
typealias DepthPlatformView = UIView
#endif

@MainActor
final class DepthWarpSurface: DepthPlatformView {
    nonisolated(unsafe) static let device = MTLCreateSystemDefaultDevice()
    nonisolated(unsafe) static let queue = device?.makeCommandQueue()
    nonisolated(unsafe) static let context: CIContext? = queue.map {
        CIContext(mtlCommandQueue: $0, options: [.cacheIntermediates: false,
                                                 .workingColorSpace: CGColorSpace(name: CGColorSpace.sRGB) as Any])
    }

    private var input: (scene: DepthScenes.Scene, shift: CGPoint, focus: CGFloat)?
    /// What the layer shows now: a draw with the same is skipped.
    private var shown: (picture: ObjectIdentifier, shift: CGPoint, focus: CGFloat, size: CGSize)?

    private var metal: CAMetalLayer { layer as! CAMetalLayer }

    #if os(macOS)
    init() {
        super.init(frame: .zero)
        wantsLayer = true
        setUp()
    }
    override func makeBackingLayer() -> CALayer { CAMetalLayer() }
    override var isFlipped: Bool { true }
    override func layout() { super.layout(); draw() }
    override func viewDidChangeBackingProperties() { super.viewDidChangeBackingProperties(); draw() }
    private var scale: CGFloat { window?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 2 }
    #else
    override class var layerClass: AnyClass { CAMetalLayer.self }
    init() {
        super.init(frame: .zero)
        isUserInteractionEnabled = false
        setUp()
    }
    override func layoutSubviews() { super.layoutSubviews(); draw() }
    override func didMoveToWindow() { super.didMoveToWindow(); draw() }
    private var scale: CGFloat { window?.screen.scale ?? UIScreen.main.scale }
    #endif

    required init?(coder: NSCoder) { fatalError("not from a nib") }

    private func setUp() {
        metal.device = Self.device
        metal.pixelFormat = .bgra8Unorm
        metal.framebufferOnly = false   // Core Image writes the drawable's texture
        metal.isOpaque = false
        metal.colorspace = CGColorSpace(name: CGColorSpace.sRGB)
    }

    func show(_ scene: DepthScenes.Scene, _ shift: CGPoint, _ focus: CGFloat) {
        input = (scene, shift, focus)
        draw()
    }

    private func draw() {
        guard let input, let context = Self.context, let queue = Self.queue else { return }
        // drawn at no more than the scene's own pixels (its depth has no finer detail), the layer stretching it over the
        // view: at the screen's pixels (3x on the iPhone) each frame walked 48 depths for nearly three times as many,
        // and a frame in five was dropped while the case leaned (trace 2026-11-19)
        let extent = input.scene.picture.extent
        let fit = min(1, extent.height / max(1, bounds.height * scale))
        let size = CGSize(width: (bounds.width * scale * fit).rounded(), height: (bounds.height * scale * fit).rounded())
        guard size.width >= 1, size.height >= 1 else { return }
        let key = (ObjectIdentifier(input.scene.picture), input.shift, input.focus, size)
        if let shown, shown.picture == key.0, shown.shift == key.1, shown.focus == key.2, shown.size == key.3 { return }
        guard let warped = DepthWarp.warped(input.scene, shift: input.shift, focus: input.focus) else { return }
        metal.contentsScale = scale
        metal.drawableSize = size
        guard let drawable = metal.nextDrawable(), let buffer = queue.makeCommandBuffer() else { return }
        let start = Date()
        // the scene's pixels stretched over the drawable (the view is the picture's size, in its proportions)
        let image = warped.transformed(by: CGAffineTransform(scaleX: size.width / extent.width, y: size.height / extent.height))
        let destination = CIRenderDestination(mtlTexture: drawable.texture, commandBuffer: buffer)
        destination.isFlipped = true   // the texture's rows run down, Core Image's up
        destination.colorSpace = CGColorSpace(name: CGColorSpace.sRGB)
        _ = try? context.startTask(toRender: image, from: CGRect(origin: .zero, size: size), to: destination, at: .zero)
        buffer.present(drawable)
        buffer.commit()
        shown = key
        if CoverFlowTrace.on { CoverFlowTrace.note(String(format: "depth warp %.1f ms (gpu)", Date().timeIntervalSince(start) * 1000)) }
    }
}
