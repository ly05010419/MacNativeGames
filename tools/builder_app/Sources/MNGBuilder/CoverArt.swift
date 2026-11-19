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
#else
import UIKit
typealias PlatformImage = UIImage
extension Image { init(platformImage: PlatformImage) { self.init(uiImage: platformImage) } }
private func platformImage(_ image: CGImage) -> PlatformImage { UIImage(cgImage: image) }
#endif

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
        guard let source = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil) else { return nil }
        let largest = (0..<CGImageSourceGetCount(source)).max { a, b in
            func width(_ i: Int) -> Int {
                (CGImageSourceCopyPropertiesAtIndex(source, i, nil) as? [CFString: Any])?[kCGImagePropertyPixelWidth] as? Int ?? 0
            }
            return width(a) < width(b)
        } ?? 0
        let options = [kCGImageSourceCreateThumbnailFromImageAlways: true, kCGImageSourceThumbnailMaxPixelSize: 768,
                       kCGImageSourceShouldCacheImmediately: true] as CFDictionary
        guard let image = CGImageSourceCreateThumbnailAtIndex(source, largest, options) else { return nil }
        let small = CIImage(cgImage: image).transformed(by: CGAffineTransform(scaleX: 0.25, y: 0.25))
        let blurred = small.clampedToExtent().applyingGaussianBlur(sigma: Double(small.extent.width) * 0.043)
            .cropped(to: small.extent)
        guard let backdrop = context.createCGImage(blurred, from: small.extent) else { return nil }
        return Cover(picture: platformImage(image), backdrop: platformImage(backdrop), path: path)
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
                        .clipped()
                        .overlay(Color.black.opacity(0.25))
                        CroppedPicture(picture: picture, center: crops.center(of: picturePath))
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
                    }
                }
            } else {
                LinearGradient(colors: T.gameColors(game), startPoint: .topLeading, endPoint: .bottomTrailing)
                empty
            }
        }
    }
}
