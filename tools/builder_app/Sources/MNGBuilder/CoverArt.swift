import AppKit
import ImageIO
import SwiftUI
import Vision

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
    let picture: NSImage
    let center: CGFloat

    var body: some View {
        GeometryReader { geo in
            let side = geo.size.height
            let x = min(0, max(geo.size.width - side, geo.size.width / 2 - center * side))
            Image(nsImage: picture).resizable().frame(width: side, height: side)
                .offset(x: x)
                .frame(width: geo.size.width, height: side, alignment: .leading)
                .clipped()
        }
    }
}
