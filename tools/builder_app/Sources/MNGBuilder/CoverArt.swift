import CoreImage
import ImageIO
import SwiftUI
import Vision
#if os(macOS)
import AppKit
/// A game's picture as the platform holds it (the Mac: its app's icon; the iPhone / iPad Launcher: the PNG its install
/// put in, tools/ios_launcher/install.py). This file is shared by both Launchers.
typealias PlatformImage = NSImage
extension Image { init(platformImage: PlatformImage) { self.init(nsImage: platformImage) } }
private func platformImage(_ image: CGImage) -> PlatformImage {
    NSImage(cgImage: image, size: NSSize(width: image.width, height: image.height))
}
/// An image drawn at `scale` pixels per point, shown at its size in points.
private func platformImage(_ image: CGImage, points: CGSize, scale: CGFloat) -> PlatformImage {
    NSImage(cgImage: image, size: points)
}
#else
import UIKit
typealias PlatformImage = UIImage
extension Image { init(platformImage: PlatformImage) { self.init(uiImage: platformImage) } }
private func platformImage(_ image: CGImage) -> PlatformImage { UIImage(cgImage: image) }
private func platformImage(_ image: CGImage, points: CGSize, scale: CGFloat) -> PlatformImage {
    UIImage(cgImage: image, scale: scale, orientation: .up)
}
#endif

/// A picture blurred once at the size it is shown, instead of on every frame of an animation (user 2026-11-19: "封面
/// 移动的时候里面图片 也在一直 调整": the opened case blurred the picture live over the whole stage and across both
/// halves, twice with its reflection, on every frame; the frames came 6, 13 and 20 ms apart in turn, MNG_COVERFLOW_TRACE,
/// and the whole picture moved in uneven steps). Drawn by SwiftUI itself (ImageRenderer) with the very modifiers a live
/// blur had, so it looks the same; kept per picture, size and settings.
@MainActor
enum FrozenBlur {
    private static var made: [String: PlatformImage] = [:]

    /// `image` filling `width` x `height` (cut to fill), blurred by `radius` with opaque edges and its colour times
    /// `saturation`, drawn at `scale` pixels per point.
    static func image(_ image: PlatformImage, width: CGFloat, height: CGFloat, radius: CGFloat, saturation: Double,
                      scale: CGFloat) -> PlatformImage? {
        let w = width.rounded(), h = height.rounded()
        guard w > 0, h > 0 else { return nil }
        let key = "\(ObjectIdentifier(image).hashValue) \(w)x\(h) r\(radius) s\(saturation) @\(scale)"
        if let known = made[key] { return known }
        let renderer = ImageRenderer(content:
            Image(platformImage: image).resizable().aspectRatio(contentMode: .fill)
                .frame(width: w, height: h).clipped()
                .blur(radius: radius, opaque: true)
                .saturation(saturation))
        renderer.scale = scale
        guard let cg = renderer.cgImage else { return nil }
        let frozen = platformImage(cg, points: CGSize(width: w, height: h), scale: scale)
        if made.count >= 24 { made.removeAll() }   // a window resized many times: start again
        made[key] = frozen
        return frozen
    }
}

/// Each game's picture decoded once, at the size a case shows it, and its blurred backdrop made once; read again when
/// its file changes (a new build of the game). User 2026-11-19 "为什么 这么卡": every case read its picture from the
/// file, decoded it and blurred it live on every frame of a swipe; the iPhone stuttered, then ran smoothly without it,
/// and the Mac ("mac 也要这样优化吗" "改") did the same, reading the app's Info.plist as well.
@MainActor
final class CoverImages {
    static let shared = CoverImages()
    struct Cover { let picture: PlatformImage; let backdrop: PlatformImage; let path: String }

    private var cache: [String: (stamp: Date?, cover: Cover?)] = [:]
    private let context = CIContext()

    func cover(at path: String?) -> Cover? {
        guard let path else { return nil }
        let stamp = (try? FileManager.default.attributesOfItem(atPath: path))?[.modificationDate] as? Date
        if let known = cache[path], known.stamp == stamp { return known.cover }
        let made = load(path)
        cache[path] = (stamp, made)
        return made
    }

    /// Picture: the file's largest image (an icns holds several sizes) scaled to 768 px (a case's insert is at most about
    /// 700 px high). Backdrop: a quarter of that, blurred about as a live 10 pt blur looks on a case (10 pt on
    /// an insert about 230 pt high: 4.3% of the picture's side).
    private func load(_ path: String) -> Cover? {
        guard let image = Self.decode(path) else { return nil }
        let small = CIImage(cgImage: image).transformed(by: CGAffineTransform(scaleX: 0.25, y: 0.25))
        let blurred = small.clampedToExtent().applyingGaussianBlur(sigma: Double(small.extent.width) * 0.043)
            .cropped(to: small.extent)
        guard let backdrop = context.createCGImage(blurred, from: small.extent) else { return nil }
        return Cover(picture: platformImage(image), backdrop: platformImage(backdrop), path: path)
    }

    /// Every picture this many pixels high (or its own height, if less): its height is what fills the insert (about
    /// 730 px on a Retina screen), so a 1024 x 1024 square and a 1920 x 1080 key art come out equally sharp (user
    /// 2026-11-19: "像素都一致 不要有些高清 有些模糊"; a cap on the longer side had left the key art 720 high).
    nonisolated static let height = 1024

    /// The file's largest image (an icns holds several sizes) scaled to `height` px high, its width in proportion.
    nonisolated static func decode(_ path: String) -> CGImage? {
        guard let source = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil) else { return nil }
        func size(_ i: Int) -> (w: Int, h: Int) {
            let p = CGImageSourceCopyPropertiesAtIndex(source, i, nil) as? [CFString: Any]
            return (p?[kCGImagePropertyPixelWidth] as? Int ?? 0, p?[kCGImagePropertyPixelHeight] as? Int ?? 0)
        }
        let largest = (0..<CGImageSourceGetCount(source)).max { size($0).w < size($1).w } ?? 0
        let (w, h) = size(largest)
        let longest = h > 0 && w > 0 ? Int((Double(min(h, height)) * Double(max(w, h)) / Double(h)).rounded()) : height
        let options = [kCGImageSourceCreateThumbnailFromImageAlways: true, kCGImageSourceThumbnailMaxPixelSize: longest,
                       kCGImageSourceShouldCacheImmediately: true] as CFDictionary
        return CGImageSourceCreateThumbnailAtIndex(source, largest, options)
    }
}


/// A game's picture (the official key art, 16:9, or an app's square icon) cut to a tall frame: drawn `margin` times the
/// frame's height, in its own proportions, its middle at `center` across it; the extra is spread round the frame, top
/// and bottom alike and, where the picture is wide enough, left and right, so the picture can move inside the frame
/// without its edge coming in (user 2026-11-19: "原始图片 裁剪 大一圈" — the frame cut from a larger picture, not the
/// picture enlarged).
struct CroppedPicture<Content: View>: View {
    /// The picture's width over its height.
    let aspect: CGFloat
    let center: CGFloat
    var margin: CGFloat = 1
    /// The picture, drawn at the size it is given (an image, or the leaning case's picture in depth on the GPU).
    @ViewBuilder let content: Content

    var body: some View {
        GeometryReader { geo in
            let height = geo.size.height * margin
            let width = height * aspect
            let spare = (height - geo.size.height) / 2
            // as near the centre as the picture allows, keeping `spare` beyond the frame on both sides where it can
            let lowest = min(0, geo.size.width - width + spare), highest = max(lowest, -spare)
            let x = min(highest, max(lowest, geo.size.width / 2 - center * width))
            content.frame(width: width, height: height)
                .offset(x: x, y: -spare)
                .frame(width: geo.size.width, height: geo.size.height, alignment: .topLeading)
                .clipped()
        }
    }
}

extension CroppedPicture where Content == Image {
    init(picture: PlatformImage, center: CGFloat, margin: CGFloat = 1) {
        self.init(aspect: picture.size.width / max(1, picture.size.height), center: center, margin: margin) {
            Image(platformImage: picture).resizable()
        }
    }
}

/// How far the case under the pointer leans, -1...1 each way (CoverFlowGeometry.lean; the iPhone, the phone's turn): the
/// layers of its front move apart by it, as a parallax picture's (user 2026-11-19: "mac 试一试 3d 视差效果"). 0 for every
/// other case.
struct CaseParallaxKey: EnvironmentKey { static let defaultValue = CGPoint.zero }
extension EnvironmentValues {
    var caseParallax: CGPoint {
        get { self[CaseParallaxKey.self] }
        set { self[CaseParallaxKey.self] = newValue }
    }
}

/// A case's front in the Cover Flow, as a real case (user 2026-11-17, a Switch 2 case): the red band across the top, the
/// insert's picture under it (the name is written under the middle case). `empty` is drawn on the game's colours when
/// there is no picture (the Mac: a game not built yet; the iPhone: a game whose picture its install did not find).
struct CoverFront<Empty: View>: View {
    let game: String
    let picture: PlatformImage?
    let picturePath: String?
    /// Where across the picture the front is cut, 0 left ... 1 right (covers.json `center`; the middle by default).
    var center: CGFloat = 0.5
    @ViewBuilder let empty: Empty

    @Environment(\.caseParallax) private var parallax
    @ObservedObject private var depth = DepthScenes.shared
    /// A part of the picture at depth d moves by depthReach x (d - depthFocus) of the front's width at a full lean
    /// (DepthWarp): the farthest (0) a little the other way, the nearest (1) the most towards the pointer.
    static var depthReach: CGFloat { 0.05 }   // half the thickness (user 2026-11-19: "我希望 厚度 小一半", from 0.10)
    /// The square picture is drawn this much taller than the insert, centred (CroppedPicture): the insert shows the
    /// middle 90% of its height (user 2026-11-19: "改成 90%", from 80%), 5% to spare above and below, more at the sides;
    /// the largest move is about 8 pt on the Mac and 11 pt at the iPhone's fuller lean, under the 15 pt or so to spare.
    static var margin: CGFloat { 1 / 0.9 }
    static var depthFocus: CGFloat { 0.35 }

    var body: some View {
        GeometryReader { geo in
            let band = geo.size.height * CaseHeader.share
            VStack(spacing: 0) {
                CaseHeader(game: game).frame(height: band)
                art.frame(width: geo.size.width, height: geo.size.height - band).clipped()
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)   // the case's front (CoverFlow sizes it)
        .clipped()
    }

    /// The insert under the band: the picture alone, filling the whole tall rectangle (user 2026-11-19: "去掉 这个 只显示
    /// 图片，不用 正方形了 用 长方形": no name nor blurred ground under it; the name is written under the middle case).
    private var art: some View {
        ZStack {
            if let picture, let picturePath {
                GeometryReader { geo in
                    let w = geo.size.width, tall = geo.size.height
                    // the picture in depth while the case leans (DepthScenes): each pixel moved by its depth, the
                    // farthest the other way; moved whole until the depth is made (and at rest, where it lies
                    // exactly on it)
                    let middle = CGSize(width: parallax.x * w * 0.035, height: parallax.y * w * 0.035)
                    ZStack {
                        // the shift at a full lean in the picture's pixels: depthReach of the front's width, the
                        // picture shown `tall` x margin points high
                        let margin = Self.margin
                        let perPoint = 1 / (tall * margin)
                        if parallax != .zero, DepthWarp.ready, let scene = depth.scene(of: picturePath) {
                            // drawn by the GPU straight to the screen (DepthSurface.swift), the shift rounded to half a
                            // pixel of the picture: a turn too small to move it draws nothing new ("ab 都做", A)
                            let pixels = Self.depthReach * w * scene.height * perPoint
                            let half = { (v: CGFloat) in (v * 2).rounded() / 2 }
                            CroppedPicture(aspect: scene.picture.extent.width / max(1, scene.picture.extent.height),
                                           center: center, margin: margin) {
                                DepthWarpView(scene: scene, shift: CGPoint(x: half(parallax.x * pixels), y: half(parallax.y * pixels)),
                                              focus: Self.depthFocus)
                            }
                        } else {
                            CroppedPicture(picture: picture, center: center, margin: margin).offset(middle)
                        }
                    }
                    .frame(width: w, height: tall)
                }
            } else {
                LinearGradient(colors: T.gameColors(game), startPoint: .topLeading, endPoint: .bottomTrailing)
                empty
            }
        }
    }
}
