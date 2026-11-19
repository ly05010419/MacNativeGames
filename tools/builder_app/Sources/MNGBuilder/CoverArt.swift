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
    /// 700 px high). Backdrop: a quarter of that, blurred as CoverFront's live .blur(radius: 10) looks on a case (10 pt on
    /// an insert about 230 pt high: 4.3% of the picture's side).
    private func load(_ path: String) -> Cover? {
        guard let image = Self.decode(path) else { return nil }
        let small = CIImage(cgImage: image).transformed(by: CGAffineTransform(scaleX: 0.25, y: 0.25))
        let blurred = small.clampedToExtent().applyingGaussianBlur(sigma: Double(small.extent.width) * 0.043)
            .cropped(to: small.extent)
        guard let backdrop = context.createCGImage(blurred, from: small.extent) else { return nil }
        return Cover(picture: platformImage(image), backdrop: platformImage(backdrop), path: path)
    }

    /// The file's largest image (an icns holds several sizes) scaled to 768 px.
    nonisolated static func decode(_ path: String) -> CGImage? {
        guard let source = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil) else { return nil }
        let largest = (0..<CGImageSourceGetCount(source)).max { a, b in
            func width(_ i: Int) -> Int {
                (CGImageSourceCopyPropertiesAtIndex(source, i, nil) as? [CFString: Any])?[kCGImagePropertyPixelWidth] as? Int ?? 0
            }
            return width(a) < width(b)
        } ?? 0
        let options = [kCGImageSourceCreateThumbnailFromImageAlways: true, kCGImageSourceThumbnailMaxPixelSize: 768,
                       kCGImageSourceShouldCacheImmediately: true] as CFDictionary
        return CGImageSourceCreateThumbnailAtIndex(source, largest, options)
    }
}

/// A picture parted in two by depth, as a spatial scene on a phone's lock screen (user 2026-11-19: "手机上 照片做墙纸 有一种
/// 空间3d 特效", "做一个 mac上 我看看"): its subject (what Vision finds in front: a figure, a shield, the title) on its
/// own with clear round it, and the scenery behind, where the subject's place is filled with the picture heavily
/// blurred, so a little of it may show as the subject moves off it. Made once per picture, off the main thread; until
/// then (or when Vision finds no subject) the picture is one layer.
@MainActor final class SpatialLayers: ObservableObject {
    static let shared = SpatialLayers()
    struct Layers { let subject: PlatformImage; let scenery: PlatformImage }
    @Published private(set) var made: [String: Layers] = [:]
    private var asked: Set<String> = []

    func layers(of path: String) -> Layers? {
        if let known = made[path] { return known }
        if !asked.contains(path) {
            asked.insert(path)
            Task.detached(priority: .utility) {
                guard let parts = Self.part(path) else { return }
                await MainActor.run {
                    self.made[path] = Layers(subject: platformImage(parts.subject), scenery: platformImage(parts.scenery))
                }
            }
        }
        return nil
    }

    nonisolated static func part(_ path: String) -> (subject: CGImage, scenery: CGImage)? {
        guard let image = CoverImages.decode(path) else { return nil }
        let request = VNGenerateForegroundInstanceMaskRequest()
        let handler = VNImageRequestHandler(cgImage: image)
        guard (try? handler.perform([request])) != nil, let found = request.results?.first,
              let buffer = try? found.generateScaledMaskForImage(forInstances: found.allInstances, from: handler)
        else { return nil }
        let picture = CIImage(cgImage: image)
        let extent = picture.extent
        let mask = CIImage(cvPixelBuffer: buffer)
        let clear = CIImage(color: .clear).cropped(to: extent)
        // the subject: the picture where the mask is, clear elsewhere
        let subject = picture.applyingFilter("CIBlendWithMask", parameters: [kCIInputBackgroundImageKey: clear,
                                                                            kCIInputMaskImageKey: mask])
        // the scenery: the subject's place (a little wider, soft-edged) filled with the picture heavily blurred
        let wider = mask.applyingFilter("CIMorphologyMaximum", parameters: [kCIInputRadiusKey: extent.width * 0.012])
            .applyingGaussianBlur(sigma: extent.width * 0.006).cropped(to: extent)
        let smudged = picture.clampedToExtent().applyingGaussianBlur(sigma: extent.width * 0.03).cropped(to: extent)
        let scenery = smudged.applyingFilter("CIBlendWithMask", parameters: [kCIInputBackgroundImageKey: picture,
                                                                            kCIInputMaskImageKey: wider])
        let context = CIContext()
        guard let a = context.createCGImage(subject, from: extent), let b = context.createCGImage(scenery, from: extent)
        else { return nil }
        return (a, b)
    }
}

/// Where to cut a game's square picture into a case's tall insert (user 2026-11-17: "图片 重新裁剪 要合适的 长方形"):
/// across the middle of what matters in it, its title's words and what draws the eye, as Vision finds them. The pictures'
/// titles run across 0.75-0.9 of their width, so the insert keeps 88% of it and loses only empty sides. Worked out once
/// per picture, off the main thread; until then the cut is in the middle.
@MainActor final class CoverCrops: ObservableObject {
    static let shared = CoverCrops()
    /// The share of the square picture's width a tall insert keeps.
    static let keep: CGFloat = 0.88
    @Published private(set) var centers: [String: CGFloat] = [:]
    private var asked: Set<String> = []

    /// The middle of the cut, as a share of the picture's width.
    func center(of path: String) -> CGFloat {
        if let c = centers[path] { return c }
        if !asked.contains(path) {
            asked.insert(path)
            Task.detached(priority: .utility) {
                let c = Self.find(path)
                await MainActor.run { self.centers[path] = c }
            }
        }
        return 0.5
    }

    /// The middle of the span the title's words and the eye-catching parts take up; 0.5 if Vision finds neither.
    nonisolated static func find(_ path: String) -> CGFloat {
        guard let source = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil),
              let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else { return 0.5 }
        let words = VNRecognizeTextRequest()
        let eye = VNGenerateAttentionBasedSaliencyImageRequest()
        do { try VNImageRequestHandler(cgImage: image).perform([words, eye]) } catch { return 0.5 }
        let boxes = (words.results ?? []).map(\.boundingBox)
            + ((eye.results ?? []).first?.salientObjects ?? []).map(\.boundingBox)
        guard let low = boxes.map(\.minX).min(), let high = boxes.map(\.maxX).max() else { return 0.5 }
        return (low + high) / 2
    }
}

/// A game's square picture cut to a tall frame: the frame's height is the picture's, its middle at `center`.
struct CroppedPicture: View {
    let picture: PlatformImage
    let center: CGFloat

    var body: some View {
        GeometryReader { geo in
            let side = geo.size.height
            let x = min(0, max(geo.size.width - side, geo.size.width / 2 - center * side))
            Image(platformImage: picture).resizable().frame(width: side, height: side)
                .offset(x: x)
                .frame(width: geo.size.width, height: side, alignment: .leading)
                .clipped()
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
    /// The picture already blurred, made once (the iPhone / iPad Launcher: a live blur of the whole picture on every
    /// case, every frame, made its swipe stutter, user 2026-11-19 "为什么 这么卡"); nil: blurred live (the Mac).
    var backdrop: PlatformImage? = nil
    @ViewBuilder let empty: Empty

    @ObservedObject private var crops = CoverCrops.shared
    @Environment(\.caseParallax) private var parallax
    @ObservedObject private var spatial = SpatialLayers.shared

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

    /// The insert under the band: the game's square picture cut to a tall rectangle round its title (CoverCrops), its
    /// lower edge fading into the same picture lightly blurred, which carries on to the bottom with the game's name.
    private var art: some View {
        ZStack {
            if let picture, let picturePath {
                GeometryReader { geo in
                    let tall = min(geo.size.height, geo.size.width / CoverCrops.keep)
                    // the parallax (caseParallax): the layers moved apart, each only moved, never laid out again
                    let w = geo.size.width
                    let back = CGSize(width: -parallax.x * w * 0.025, height: -parallax.y * w * 0.025)
                    let middle = CGSize(width: parallax.x * w * 0.035, height: parallax.y * w * 0.035)
                    let front = CGSize(width: parallax.x * w * 0.06, height: parallax.y * w * 0.06)
                    ZStack(alignment: .top) {
                        Group {
                            if let backdrop {
                                Image(platformImage: backdrop).resizable().scaledToFill()
                                    .frame(width: geo.size.width, height: geo.size.height)
                            } else {
                                Image(platformImage: picture).resizable().scaledToFill()
                                    .frame(width: geo.size.width, height: geo.size.height).blur(radius: 10)
                            }
                        }
                        .scaleEffect(1.08)   // farthest back, moved the other way: larger, so its edges never show
                        .offset(back)
                        .clipped()
                        .overlay(Color.black.opacity(0.25))
                        // the picture in depth (SpatialLayers): its scenery moved a little the other way, its
                        // subject towards the pointer and a touch nearer; one layer until they are made. The frame
                        // and its fading lower edge stay where they are.
                        let center = crops.center(of: picturePath)
                        ZStack {
                            if let parts = spatial.layers(of: picturePath) {
                                CroppedPicture(picture: parts.scenery, center: center)
                                    .offset(CGSize(width: -parallax.x * w * 0.012, height: -parallax.y * w * 0.012))
                                CroppedPicture(picture: parts.subject, center: center)
                                    .scaleEffect(1 + 0.03 * min(1, hypot(parallax.x, parallax.y)))
                                    .offset(middle)
                            } else {
                                CroppedPicture(picture: picture, center: center).offset(middle)
                            }
                        }
                        .frame(width: geo.size.width, height: tall)
                        .mask(LinearGradient(stops: [.init(color: .black, location: 0.88), .init(color: .clear, location: 1)],
                                             startPoint: .top, endPoint: .bottom))
                        VStack {
                            Spacer(minLength: 0)
                            Text(T.gameName(game)).font(.system(size: geo.size.width * 0.075, weight: .bold))
                                .foregroundStyle(.white).multilineTextAlignment(.center).lineLimit(2).minimumScaleFactor(0.6)
                                .padding(.horizontal, geo.size.width * 0.06).shadow(color: .black.opacity(0.6), radius: 3)
                                .frame(height: max(0, geo.size.height - tall * 0.96))
                        }
                        .offset(front)   // nearest: moved the most
                    }
                }
            } else {
                LinearGradient(colors: T.gameColors(game), startPoint: .topLeading, endPoint: .bottomTrailing)
                empty
            }
        }
    }
}
