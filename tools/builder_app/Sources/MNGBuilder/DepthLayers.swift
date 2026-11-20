import Accelerate
import CoreImage

/// A case's picture in two layers, the idea of 3D Photo Inpainting (Shih, Su, Kopf, Huang, CVPR 2020; MIT; its idea only,
/// none of its code or networks) made light (user 2026-11-20: "直接做2 可以照 3D Photo Inpainting 的思路，自己写一个轻量的
/// 两层版本（前景加补好的背景） 做mac"). With one layer a near thing's side was drawn on as it leaned: its edge pixels
/// stretched over the gap they left (DepthWarp). Now:
/// - the depth's edges are made sharp: a pixel on an edge is the near depth or the far one, not a slope between (the
///   model's depth, smoothed, ran from one to the other over some 8 pixels);
/// - behind every near thing, as far in as a lean can uncover, a second picture and depth are filled in from its farther
///   side, ring by ring;
/// - as the case leans, each pixel is walked through both; where the picture's first surface is a side (a step in
///   depth, not a surface), what is behind shows instead.
/// Made only where it can be shown: the Mac's development build, behind its switch ("两层景深", off at first). The
/// release and the iPhone draw one layer as 1.1.0 (two compared worse: "打开 更差"; "只能我测试显示 发布版本 不显示").
enum DepthLayers {
    #if os(macOS)
    static let on = !Paths.isRelease
    #else
    static let on = false
    #endif
    /// A depth step of at least this within `edgeReach` pixels is an edge, made sharp.
    static let edge: Float = 0.1
    static let edgeReach = 4
    /// A pixel nearer by this than the farthest within `reach` may have something uncovered behind it.
    static let gap: Float = 0.08
    /// In the 512-high scene's pixels: a full lean moves a part by at most about 24 at depthReach 0.05 of the front's
    /// width (across and down at once), 72 at the development build's strongest, 0.15 ("景深强度"); what a lean can
    /// uncover behind a near thing (that times the step in depth) is no wider.
    static let reach = 64
    /// Where the picture's first surface is a step in depth of more than this (a side), what is behind starts to show,
    /// all of it at twice this (a surface is found within one step of the walk, 1/47).
    static let side: Float = 0.04

    struct Layers {
        /// The picture's depth with its edges made sharp, and the depth behind its near things, two halves a pixel.
        let depth: [Float16]
        /// The picture behind its near things, RGBA8.
        let behind: [UInt8]
        /// Pixels filled in behind near things, and those a lean could uncover but no farther side reached.
        let filled: Int
        let unreached: Int
    }

    /// Pixels just outside a near thing's sharp depth edge that go with it: the model's edge and the picture's are a
    /// pixel or two apart, and these pixels, left at the far depth, stayed behind as the thing's ghost outline
    /// (probe 2026-11-20, BotW's shield); and the pixels from which nothing behind is taken, one more.
    static let outline = 2

    /// The two layers from the picture (RGBA8) and its depth (0 farthest ... 1 nearest), both w x h.
    static func make(depth raw: [Float], rgba: [UInt8], width w: Int, height h: Int) -> Layers {
        let depth = sharpened(raw, w, h)
        let far = extreme(depth, w, h, reach, min: true)
        let hole = depth.indices.map { depth[$0] - far[$0] > gap ? Float(1) : 0 }
        let outlined = extreme(hole, w, h, outline, min: false)
        let unclean = extreme(hole, w, h, outline + 1, min: false)
        let nearest = extreme(depth, w, h, outline, min: false)
        // the picture's own depth: its near things' outlines taken with them
        let front = depth.indices.map { outlined[$0] > 0 && hole[$0] == 0 ? nearest[$0] : depth[$0] }
        let (seed, unreached) = nearestFarther(depth, far, unclean, w, h)
        var behindDepth = depth
        var colour = rgba.map(Float.init)
        var filled: [Int] = []
        for p in depth.indices where unclean[p] > 0 {
            let s = seed[p]
            guard s >= 0 else { continue }
            // the far side's picture carried on past its edge as in a mirror (its own texture, not one colour drawn
            // out); its edge pixel where the mirror falls outside it
            let mx = 2 * (s % w) - p % w, my = 2 * (s / w) - p / w
            var from = s
            if mx >= 0, mx < w, my >= 0, my < h {
                let m = my * w + mx
                if unclean[m] == 0, depth[m] <= far[p] + gap { from = m }
            }
            behindDepth[p] = depth[s]
            colour[p * 4] = Float(rgba[from * 4])
            colour[p * 4 + 1] = Float(rgba[from * 4 + 1])
            colour[p * 4 + 2] = Float(rgba[from * 4 + 2])
            filled.append(p)
        }
        smooth(&colour, filled, behindDepth, far, w, h)
        var packed = [Float16](repeating: 0, count: depth.count * 2)
        for i in depth.indices {
            packed[i * 2] = Float16(front[i])
            packed[i * 2 + 1] = Float16(behindDepth[i])
        }
        let behind = colour.map { UInt8(max(0, min(255, $0.rounded()))) }
        return Layers(depth: packed, behind: behind, filled: filled.count, unreached: unreached)
    }

    /// For each pixel to fill (`fill` > 0), the nearest pixel outside it on the far side (as far as its own farthest
    /// within reach, not the near thing's own pixels), found outwards ring by ring from the pixels around; -1 where none
    /// is reached, and how many.
    static func nearestFarther(_ depth: [Float], _ far: [Float], _ fill: [Float], _ w: Int, _ h: Int) -> ([Int], Int) {
        var seed = [Int](repeating: -1, count: depth.count)
        var queue: [Int] = []
        queue.reserveCapacity(depth.count / 4)
        for p in depth.indices where fill[p] == 0 {
            seed[p] = p
            let x = p % w, y = p / w
            let edge = (x > 0 && fill[p - 1] > 0) || (x + 1 < w && fill[p + 1] > 0)
                || (y > 0 && fill[p - w] > 0) || (y + 1 < h && fill[p + w] > 0)
            if edge { queue.append(p) }
        }
        var head = 0
        while head < queue.count {
            let u = queue[head], s = seed[u]
            head += 1
            let x = u % w, y = u / w
            for dy in -1...1 where y + dy >= 0 && y + dy < h {
                for dx in -1...1 where x + dx >= 0 && x + dx < w && (dx != 0 || dy != 0) {
                    let v = u + dy * w + dx
                    guard fill[v] > 0, seed[v] < 0, depth[s] <= far[v] + gap else { continue }
                    seed[v] = s
                    queue.append(v)
                }
            }
        }
        let unreached = depth.indices.reduce(0) { $0 + (fill[$1] > 0 && seed[$1] < 0 ? 1 : 0) }
        return (seed, unreached)
    }

    /// Edges made sharp (3D Photo Inpainting sharpens its depth's edges before it cuts layers): where the depth around a
    /// pixel spans an edge, the pixel takes the nearer of the farthest and the nearest depth there.
    static func sharpened(_ depth: [Float], _ w: Int, _ h: Int) -> [Float] {
        let lo = extreme(depth, w, h, edgeReach, min: true)
        let hi = extreme(depth, w, h, edgeReach, min: false)
        return depth.indices.map { i in
            hi[i] - lo[i] < edge ? depth[i] : (depth[i] - lo[i] < hi[i] - depth[i] ? lo[i] : hi[i])
        }
    }

    /// The least (or greatest) value within `radius` pixels (a square), by vImage.
    static func extreme(_ values: [Float], _ w: Int, _ h: Int, _ radius: Int, min: Bool) -> [Float] {
        var out = [Float](repeating: 0, count: values.count)
        let side = vImagePixelCount(radius * 2 + 1)
        var source = values
        source.withUnsafeMutableBytes { src in
            out.withUnsafeMutableBytes { dst in
                var a = vImage_Buffer(data: src.baseAddress, height: vImagePixelCount(h), width: vImagePixelCount(w),
                                      rowBytes: w * 4)
                var b = vImage_Buffer(data: dst.baseAddress, height: vImagePixelCount(h), width: vImagePixelCount(w),
                                      rowBytes: w * 4)
                _ = min ? vImageMin_PlanarF(&a, &b, nil, 0, 0, side, side, vImage_Flags(kvImageNoFlags))
                        : vImageMax_PlanarF(&a, &b, nil, 0, 0, side, side, vImage_Flags(kvImageNoFlags))
            }
        }
        return out
    }

    /// The filled pixels softened once over their 3 x 3 neighbours as far as each one's farthest (neighbours mirrored
    /// from different edge pixels meet in seams).
    private static func smooth(_ colour: inout [Float], _ filled: [Int], _ depth: [Float], _ far: [Float],
                               _ w: Int, _ h: Int) {
        for _ in 0..<1 {
            let before = colour
            for p in filled {
                let x = p % w, y = p / w, limit = far[p] + gap
                var n: Float = 0, r: Float = 0, g: Float = 0, b: Float = 0
                for dy in -1...1 where y + dy >= 0 && y + dy < h {
                    for dx in -1...1 where x + dx >= 0 && x + dx < w {
                        let q = p + dy * w + dx
                        guard depth[q] <= limit else { continue }
                        n += 1; r += before[q * 4]; g += before[q * 4 + 1]; b += before[q * 4 + 2]
                    }
                }
                if n > 0 { colour[p * 4] = r / n; colour[p * 4 + 1] = g / n; colour[p * 4 + 2] = b / n }
            }
        }
    }

    /// Each pixel walked through both layers from the nearest depth to the farthest, as DepthWarp's one layer: the
    /// picture's first surface, or, where that is a side, the first surface behind. A side is where the depth jumps
    /// between one step of the walk and the next (the depth read pixel by pixel, not blended: blended, the jump at an
    /// edge was spread over the steps, and columns of a near thing's edge alternated between side and surface, probe
    /// 2026-11-20). What is behind shows only where it is what the walk passed just before the side: behind a thing in
    /// front of another (a shield over its bearer) the second layer holds the far background, so there the side is
    /// drawn on as before. The colours are read once, at the end (the one-layer kernel read the picture at every step).
    /// Where a surface is found, the depth is read blended (`depth`), so an edge moves smoothly with the lean; read
    /// pixel by pixel there too, edges moved a whole pixel at a time and went jagged (user 2026-11-20, Mario's face and
    /// logo: "打开 更差"). Only the jump is read pixel by pixel (`edges`, the same depth).
    static let source = """
    kernel vec4 depthLayers(sampler image, sampler behind, sampler depth, sampler edges, vec2 shift, float focus,
                            float side) {
        vec2 p = destCoord();
        vec2 front = p + shift * focus;
        vec2 back = front;
        float prev = sample(edges, samplerTransform(edges, p - shift * (1.0 - focus))).r;
        float jump = 0.0;
        float before = 0.0;
        float behindAt = 0.0;
        float frontFound = 0.0;
        float backFound = 0.0;
        for (int i = 0; i < \(DepthWarp.steps); i++) {
            float z = 1.0 - float(i) / \(DepthWarp.steps - 1).0;
            vec2 q = p - shift * (z - focus);
            vec2 d = sample(depth, samplerTransform(depth, q)).rg;
            float e = sample(edges, samplerTransform(edges, q)).r;
            float hit = step(z, d.r) * (1.0 - frontFound);
            front = mix(front, q, hit);
            jump = mix(jump, e - prev, hit);
            before = mix(before, prev, hit);
            frontFound = max(frontFound, hit);
            prev = e;
            float hitBehind = step(z, d.g) * (1.0 - backFound);
            back = mix(back, q, hitBehind);
            behindAt = mix(behindAt, d.g, hitBehind);
            backFound = max(backFound, hitBehind);
        }
        vec4 near = sample(image, samplerTransform(image, front));
        vec4 far = sample(behind, samplerTransform(behind, back));
        float through = smoothstep(side, 2.0 * side, jump) * (1.0 - smoothstep(side, 2.0 * side, abs(behindAt - before)));
        return mix(near, far, through);
    }
    """
    static let kernel: CIKernel? = CIKernel(source: source)

    /// The picture for a shift in the picture's pixels (Core Image's y, up), from its two layers; nil without the kernel.
    static func warped(picture: CIImage, behind: CIImage, depth: CIImage, shift: CIVector, focus: CGFloat) -> CIImage? {
        guard let kernel else { return nil }
        let margin = abs(shift.x) + abs(shift.y) + 2
        return kernel.apply(extent: picture.extent, roiCallback: { _, rect in rect.insetBy(dx: -margin, dy: -margin) },
                            arguments: [picture.clampedToExtent(), behind.clampedToExtent(), depth.clampedToExtent(),
                                        depth.samplingNearest().clampedToExtent(),
                                        shift, focus, CGFloat(side)])
    }
}
