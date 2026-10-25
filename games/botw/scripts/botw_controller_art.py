"""Controller pictures drawn by our own code for the controller tips of Breath of the Wild (botw_prompts.py).

The game shows the Switch in handheld mode on its controller tips page (layout PaAllControllerTipsNN_00) as two pictures, a front view
(Nt_NNillust_00, 576x288) and the top edge (Nt_NNillust_01, 576x100), and labelled lines (callouts) end on its buttons. This
module draws an Xbox controller (style 'xbox') or a DualSense (style 'playstation') in the same two pictures. Nothing of the game is copied: the pictures are made of curves, discs,
rounded rectangles and the letters of the Switch system font (the `Letters` of botw_prompts.py), and the only game facts used are the
places of the buttons, so the game's callout lines still end on a button of the same kind:
  front view   the left stick, the d-pad cross, the right stick, the four face buttons, and View and Menu in the places of the
               Switch's - and +; the face buttons keep the Xbox layout (Y top, X left, B right, A bottom) whatever the controls.txt;
  top edge     the LT, LB, RT and RB shoulder buttons in the places of the Switch's ZL, L, ZR and R.
Each body has its controller's own outline round those places: two grips that hang below the d-pad and the right stick and
the bumpers standing above the top corners (the Xbox controller's grips short and round, the DualSense's longer, with a flat top,
the touchpad and a dark middle). The pictures keep the Switch's places because the tips page draws its lines and its rings round
the d-pad and the face buttons as panes of the layout, at fixed spots.
The DualSense keeps those places too: its touchpad, PS button and mute button sit between the sticks, the d-pad is four arrow
buttons whose middles are the Switch's four d-pad buttons, the face buttons carry the Cross, Circle, Square and Triangle
symbols, Options and Create are small upright buttons in the places of + and -, and the shoulders read L2 L1 R2 R1.
The in-game hint boxes (PaMessageTipsDrcImageNN_00) show four 228x228 pieces of the Switch and ring the button of a hint: a left
and a right shoulder view (Nt_GamePadIllustSnn_00, _01) and the right and left front of the handheld (_02: face buttons, right
stick; _03: left stick, d-pad). The two front pieces are the front view enlarged 1.2 times (as the Switch's are), the shoulder
views the corner of the controller seen from above.
The grey of each pixel and its alpha are the two channels of the game's R8G8 textures.
Every draw function takes `scale` (an integer 1 to 4): the same picture drawn `scale` times as large and as sharp, (width * scale)
x (height * scale) pixels. All the numbers in this module are pixels of the game's size (scale 1); the Canvas multiplies them.
"""
import functools
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import botw_formats as bf       # noqa: E402

FRONT, TOP = (576, 288), (576, 100)         # the size of the two pictures
# Where the Switch pictures have a button, in pixels of the picture: the game's callout lines end there.
FRONT_STICKS = {'lstick': (58.5, 88.5), 'rstick': (517.5, 151.5)}
FRONT_DPAD = (59.0, 152.0)                  # the four d-pad buttons sit at (59, 135) (41, 152) (59, 169) (76.5, 152): a cross here
FRONT_FACE = {'x': (517.0, 71.5), 'y': (499.5, 88.5), 'a': (535.0, 88.5), 'b': (517.0, 105.5)}     # Switch button -> place
FRONT_MENUS = {'minus': (81.5, 53.5), 'plus': (494.5, 53.5)}
TOP_SHOULDERS = {'zl': (9, 10, 70, 34), 'l': (9, 38, 70, 58), 'zr': (505, 10, 566, 34), 'r': (505, 38, 566, 58)}   # x0, y0, x1, y1

# The face buttons keep the controller's own layout wherever the four are drawn together: the pad button that sits in each
# Switch button's place (Xbox Y top, X left, B right, A bottom; Triangle, Square, Circle, Cross). A controls.txt changes only
# which action a button does, so it changes the prompts, not these pictures.
FACE_LAYOUT = {'x': 'y', 'y': 'x', 'a': 'b', 'b': 'a'}

PIECE = (228, 228)                          # the four hint pieces
PIECE_RIGHT = {'face': {'x': (112.5, 79.5), 'y': (91.0, 100.0), 'a': (134.0, 100.0), 'b': (112.5, 120.0)}, 'stick': (112.5, 175.5),
               'menu': ('plus', (85.5, 57.5))}      # Nt_GamePadIllustSnn_02: where the Switch picture has these buttons
PIECE_LEFT = {'stick': (114.5, 100.0), 'dpad': (114.5, 175.5), 'menu': ('minus', (141.5, 58.0))}                      # _03
PIECE_SHOULDERS = {'zl': (63, 69, 135, 104), 'l': (54, 110, 135, 131)}      # _00: the left shoulder (_01 is its mirror image)

BODY, RIM, DARK, EDGE, LIGHT = 193, 135, 55, 13, 175      # grey levels (the game's pictures use 193 for the light plastic and
#                                                           55 inside dark buttons, 13 around them)


_DETAIL = 1     # how many times finer the curves are cut into pieces: the scale of the picture being drawn (see `scaled`)


def scaled(draw):
    """A draw function that takes `scale` (an integer 1 to 4) and cuts its curves into `scale` times as many pieces."""
    @functools.wraps(draw)
    def wrapper(*args, scale=1, **kwargs):
        global _DETAIL
        if scale not in (1, 2, 3, 4):
            raise ValueError('scale must be 1, 2, 3 or 4, not %r' % (scale,))
        before, _DETAIL = _DETAIL, scale
        try:
            return draw(*args, scale=scale, **kwargs)
        finally:
            _DETAIL = before
    return wrapper


def circle(cx, cy, r):
    n = max(24, int(r * 4 * _DETAIL))
    return [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)]


def rounded_rect(x0, y0, x1, y1, r):
    r = min(r, (x1 - x0) / 2, (y1 - y0) / 2)
    points, steps = [], 8 * _DETAIL
    for cx, cy, start in ((x1 - r, y0 + r, -90), (x1 - r, y1 - r, 0), (x0 + r, y1 - r, 90), (x0 + r, y0 + r, 180)):
        for i in range(steps + 1):
            a = math.radians(start + 90 * i / steps)
            points.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return points


def outline(start, *segments, steps=None):
    """A contour from a start point and segments: a point is a straight line to it, three points are a cubic curve."""
    steps = steps or 14 * _DETAIL
    points, last = [start], start
    for seg in segments:
        if isinstance(seg[0], (int, float)):
            points.append(seg); last = seg
            continue
        p1, p2, p3 = seg
        for i in range(1, steps + 1):
            t = i / steps; u = 1 - t
            points.append((u**3 * last[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t**3 * p3[0],
                           u**3 * last[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t**3 * p3[1]))
        last = p3
    return points


def mirror(points, width):
    """The shape of `points` flipped left to right in a picture `width` wide (the point order is kept winding-correct)."""
    return [(width - x, y) for x, y in reversed(points)]


def inset(points, d):
    """The contour moved `d` pixels inwards (a negative `d` outwards), corners mitred."""
    n = len(points)
    area = sum(points[i][0] * points[(i + 1) % n][1] - points[(i + 1) % n][0] * points[i][1] for i in range(n))
    sign = 1 if area > 0 else -1

    def normal(a, b):
        dx, dy = b[0] - a[0], b[1] - a[1]; length = math.hypot(dx, dy) or 1.0
        return (-dy * sign / length, dx * sign / length)
    out = []
    for i in range(n):
        n1, n2 = normal(points[i - 1], points[i]), normal(points[i], points[(i + 1) % n])
        k = d / max(0.25, 1 + n1[0] * n2[0] + n1[1] * n2[1])
        out.append((points[i][0] + (n1[0] + n2[0]) * k, points[i][1] + (n1[1] + n2[1]) * k))
    return out


class Canvas:
    """A picture of grey levels and alpha, drawn on by painting shapes one over another (alpha blending, edges anti-aliased).
    `width` x `height` is the picture at scale 1; the shapes are in those pixels too, and the Canvas multiplies them by `scale`.
    `view` (zoom, dx, dy) first moves every shape point p to p * zoom + (dx, dy): a hint piece is the front view enlarged."""
    def __init__(self, width, height, scale=1, view=(1, 0, 0)):
        self.scale, self.view = scale, view
        self.width, self.height = width * scale, height * scale
        self.grey = [[255.0] * self.width for _ in range(self.height)]
        self.alpha = [[0.0] * self.width for _ in range(self.height)]

    def place(self, shapes):
        zoom, dx, dy = self.view
        if (zoom * self.scale, dx, dy) == (1, 0, 0):
            return shapes
        return [[((x * zoom + dx) * self.scale, (y * zoom + dy) * self.scale) for x, y in shape] for shape in shapes]

    def paint(self, shapes, grey, opacity=1.0, clip=None):
        """Paint the union of `shapes` (contours, all wound the same way) in `grey`; with `clip` (contours) only inside them."""
        shapes = self.place(shapes)
        points = [p for shape in shapes for p in shape]
        x0, x1 = max(0, math.floor(min(p[0] for p in points))), min(self.width, math.ceil(max(p[0] for p in points)))
        y0, y1 = max(0, math.floor(min(p[1] for p in points))), min(self.height, math.ceil(max(p[1] for p in points)))
        if x1 <= x0 or y1 <= y0:
            return
        cover = bf.coverage_rect([[(x - x0, y - y0) for x, y in shape] for shape in shapes], x1 - x0, y1 - y0)
        if clip:
            mask = bf.coverage_rect([[(x - x0, y - y0) for x, y in shape] for shape in self.place(clip)], x1 - x0, y1 - y0)
            cover = [[c * m // 255 for c, m in zip(row, mrow)] for row, mrow in zip(cover, mask)]
        for dy, row in enumerate(cover):
            grey_row, alpha_row = self.grey[y0 + dy], self.alpha[y0 + dy]
            for dx, c in enumerate(row):
                if not c:
                    continue
                a = c / 255 * opacity
                below = alpha_row[x0 + dx] * (1 - a)
                alpha_row[x0 + dx] = a + below
                grey_row[x0 + dx] = (grey * a + grey_row[x0 + dx] * below) / (a + below)

    def rows(self):
        """(grey rows, alpha rows) as bytes, the grey of a fully transparent pixel 255 like the game's pictures."""
        alpha = [bytes(min(255, round(a * 255)) for a in row) for row in self.alpha]
        grey = [bytes(min(255, round(g)) if a else 255 for g, a in zip(grow, arow)) for grow, arow in zip(self.grey, alpha)]
        return grey, alpha


def label_shape(letters, label, cx, cy, cap, max_width):
    return letters.text(label, cap, cx, cy, max_width)


SYMBOLS = ('Cross', 'Circle', 'Square', 'Triangle')     # the PlayStation face buttons' names (botw_prompts.PLAYSTATION_TEXT)


def symbol(canvas, name, cx, cy, s):
    """The PlayStation symbol `name` (one of SYMBOLS) in light grey on a dark button, `s` pixels from its middle to its edge."""
    if name == 'Circle':
        canvas.paint([circle(cx, cy, s * 0.95)], LIGHT)
        canvas.paint([circle(cx, cy, s * 0.62)], DARK)
    elif name == 'Square':
        canvas.paint([rounded_rect(cx - s * 0.85, cy - s * 0.85, cx + s * 0.85, cy + s * 0.85, s * 0.08)], LIGHT)
        canvas.paint([rounded_rect(cx - s * 0.52, cy - s * 0.52, cx + s * 0.52, cy + s * 0.52, s * 0.04)], DARK)
    elif name == 'Triangle':
        corners = [(cx, cy - s * 1.05), (cx + s * 1.05, cy + s * 0.75), (cx - s * 1.05, cy + s * 0.75)]
        canvas.paint([corners], LIGHT)
        canvas.paint([inset(corners, s * 0.32)], DARK)
    else:       # Cross: two bars across each other
        w = s * 0.17
        bars = []
        for a in (math.pi / 4, -math.pi / 4):
            ux, uy, vx, vy = math.cos(a) * s, math.sin(a) * s, -math.sin(a) * w, math.cos(a) * w
            bars.append([(cx - ux - vx, cy - uy - vy), (cx + ux - vx, cy + uy - vy), (cx + ux + vx, cy + uy + vy), (cx - ux + vx, cy - uy + vy)])
        canvas.paint(bars, LIGHT)


def dark_button(canvas, cx, cy, r, letters=None, label=None, cap=None):
    """A round dark button (the game's look: a black ring round a dark disc) with the letters `label` in light grey, or the
    PlayStation symbol when `label` is one of SYMBOLS."""
    canvas.paint([circle(cx, cy, r)], EDGE)
    canvas.paint([circle(cx, cy, r - 1.7)], DARK)
    if label in SYMBOLS:
        symbol(canvas, label, cx, cy, r * 0.55)
    elif label and letters:
        canvas.paint(label_shape(letters, label, cx, cy, cap or r * 0.8, 2 * r - 6), LIGHT)


def menu_glyph(canvas, cx, cy, size):
    """Three bars, as on the Xbox Menu button."""
    for dy in (-size * 0.32, 0, size * 0.32):
        canvas.paint([rounded_rect(cx - size * 0.45, cy + dy - size * 0.07, cx + size * 0.45, cy + dy + size * 0.07, size * 0.07)], LIGHT)


def view_glyph(canvas, cx, cy, size):
    """Two overlapping squares, as on the Xbox View button."""
    canvas.paint([rounded_rect(cx - size * 0.42, cy - size * 0.2, cx + size * 0.08, cy + size * 0.38, size * 0.05)], LIGHT)
    canvas.paint([rounded_rect(cx - size * 0.28, cy - size * 0.38, cx + size * 0.42, cy + size * 0.22, size * 0.05)], EDGE)
    canvas.paint([rounded_rect(cx - size * 0.22, cy - size * 0.32, cx + size * 0.36, cy + size * 0.16, size * 0.04)], LIGHT)


def upright_button(canvas, cx, cy, kind, k=1.0):
    """The DualSense's Options (`kind` 'menu': three bars) or Create (`kind` 'view': three upright strokes) button: a small
    upright dark pill."""
    pill = rounded_rect(cx - 5.5 * k, cy - 9.5 * k, cx + 5.5 * k, cy + 9.5 * k, 5.5 * k)
    canvas.paint([pill], EDGE)
    canvas.paint([inset(pill, 1.5)], DARK)
    if kind == 'menu':
        for dy in (-3.4 * k, 0, 3.4 * k):
            canvas.paint([rounded_rect(cx - 3 * k, cy + dy - 0.8 * k, cx + 3 * k, cy + dy + 0.8 * k, 0.8 * k)], LIGHT)
    else:
        for dx in (-2.6 * k, 0, 2.6 * k):
            canvas.paint([rounded_rect(cx + dx - 0.75 * k, cy - 4.5 * k, cx + dx + 0.75 * k, cy + 4.5 * k, 0.75 * k)], LIGHT)


def center_button(canvas, cx, cy, kind, letters, text, k=1.0, style='xbox'):
    """A small dark button for View, Menu or any other pad button (`kind`, a key of prompt_layout.prompt_controls); with the
    playstation style Menu and View are the upright Options and Create buttons."""
    if style == 'playstation' and kind in ('menu', 'view'):
        upright_button(canvas, cx, cy, kind, k)
        return
    dark_button(canvas, cx, cy, 8.5 * k)
    if kind == 'menu':
        menu_glyph(canvas, cx, cy, 13 * k)
    elif kind == 'view':
        view_glyph(canvas, cx, cy, 13 * k)
    elif text.get(kind):
        canvas.paint(label_shape(letters, text[kind], cx, cy, 6.0 * k, 13 * k), LIGHT)


def stick(canvas, cx, cy, k=1.0):
    """A thumbstick (`k` times the size of the front view's): a shallow dish in the body, a black ring, a dark cap with a ring."""
    canvas.paint([circle(cx, cy, 27 * k)], RIM)
    canvas.paint([circle(cx, cy, 25.6 * k)], 178)
    canvas.paint([circle(cx, cy, 19 * k)], EDGE)
    canvas.paint([circle(cx, cy, 17.2 * k)], DARK)
    canvas.paint([circle(cx, cy, 12.5 * k)], 36)
    canvas.paint([circle(cx, cy, 11.2 * k)], DARK)


def dpad(canvas, cx, cy, k=1.0):
    """The d-pad cross (`k` times the size of the front view's); its arms end where the Switch's four d-pad buttons are."""
    canvas.paint([circle(cx, cy, 31 * k)], RIM)
    canvas.paint([circle(cx, cy, 29.6 * k)], 178)

    def cross(grow):
        return [rounded_rect(cx - 27 * k - grow, cy - 9.5 * k - grow, cx + 27.5 * k + grow, cy + 9.5 * k + grow, 4 * k + grow),
                rounded_rect(cx - 9.5 * k - grow, cy - 27 * k - grow, cx + 9.5 * k + grow, cy + 27 * k + grow, 4 * k + grow)]
    canvas.paint(cross(1.7 * k), EDGE)
    canvas.paint(cross(0), DARK)
    canvas.paint([circle(cx, cy, 6.5 * k)], 38)


def arrow_pad(canvas, cx, cy, k=1.0):
    """The DualSense d-pad (`k` times the size of the front view's): four arrow buttons pointing at the middle, the middle of
    each where the Switch has a d-pad button (17 pixels from the centre at k = 1)."""
    canvas.paint([circle(cx, cy, 31 * k)], RIM)
    canvas.paint([circle(cx, cy, 29.6 * k)], 178)
    for dx, dy in ((0, -1), (1, 0), (0, 1), (-1, 0)):
        def at(u, v):       # u away from the centre, v across
            return (cx + (u * dx - v * dy) * k, cy + (u * dy + v * dx) * k)
        arrow = [at(7.5, 0), at(13, -7.8), at(27, -7.8), at(27, 7.8), at(13, 7.8)]
        if (arrow[1][0] - arrow[0][0]) * (arrow[2][1] - arrow[0][1]) - (arrow[1][1] - arrow[0][1]) * (arrow[2][0] - arrow[0][0]) < 0:
            arrow.reverse()     # every arrow wound the same way, so inset() grows them all outwards
        canvas.paint([inset(arrow, -1.7 * k)], EDGE)
        canvas.paint([arrow], DARK)


def both_halves(half, width=FRONT[0]):
    """A body outline from its left half (from the top middle round to the bottom middle) and that half's mirror image."""
    return half + mirror(half, width)[1:-1]


# The Xbox controller: a top that dips beside the guide button and rises to the bumpers, short round grips that hang below the
# d-pad and the right stick, and a round arch between them.
FRONT_BODY = both_halves(outline((288, 33), ((250, 33), (226, 36), (196, 36)), ((162, 36), (140, 27), (108, 27)),
                                 ((64, 27), (30, 46), (22, 84)), ((16, 114), (9, 160), (10, 198)), ((11, 248), (34, 283), (72, 283)),
                                 ((106, 283), (126, 262), (148, 236)), ((172, 208), (210, 191), (248, 191)), (288, 191)))
# The DualSense: a flat top the width of the touchpad, longer grips that end rounder and lower, a higher, wider arch.
FRONT_BODY_PS = both_halves(outline((288, 30), (150, 30), ((114, 30), (102, 24), (80, 24)), ((44, 24), (24, 44), (19, 80)),
                                    ((15, 112), (8, 150), (8, 196)), ((8, 244), (22, 285), (58, 285)), ((92, 285), (110, 264), (126, 238)),
                                    ((148, 204), (180, 194), (228, 194)), (288, 194)))
# The DualSense's touchpad (narrower at the bottom), Options and Create at its top corners in the places of + and -.
TOUCHPAD = outline((126, 33), (450, 33), ((459, 33), (464, 39), (463, 47)), (447, 118), ((445, 126), (439, 129), (430, 129)), (146, 129),
                   ((137, 129), (131, 126), (129, 118)), (113, 47), ((112, 39), (117, 33), (126, 33)))
PANEL = [(124, 126), (452, 126), (436, 290), (140, 290)]       # the DualSense's dark middle under the touchpad (cut by the body)


def bumper(body, lift, reach, width=FRONT[0]):
    """The left and right bumper of the front view: crescents `lift` pixels high at their middle that follow the body's top
    corners as far as `reach` (x0 of the top run, y1 of the side run); they are painted before the body, which hides their
    lower edge."""
    out = inset(body, -1)
    run = [i for i in range(len(body) // 2) if body[i][0] < reach[0] and body[i][1] < reach[1]]
    shapes = []
    for rim in (0, 1.7):
        outer, inner = [], []
        for j, i in enumerate(run):
            (x, y), (ox, oy) = body[i], out[i]
            k = max(0.0, lift * math.sin(math.pi * j / (len(run) - 1)) ** 0.6 - rim)
            outer.append((x + (ox - x) * k, y + (oy - y) * k))
            inner.append((x - (ox - x) * 6, y - (oy - y) * 6))
        left = outer + inner[::-1]
        shapes.append([left, mirror(left, width)])
    return shapes


def xbox_logo(canvas, cx, cy, r):
    """The guide button: a light button with a dark sphere cut by a light X (two bars that widen to their ends)."""
    canvas.paint([circle(cx, cy, r + 1.7)], RIM)
    canvas.paint([circle(cx, cy, r)], 226)
    sphere = circle(cx, cy, r * 0.68)
    canvas.paint([sphere], 96)
    for a in (math.pi / 4, -math.pi / 4):
        ux, uy, vx, vy = math.cos(a) * r, math.sin(a) * r, -math.sin(a) * r, math.cos(a) * r
        bar = [(cx - ux - vx * 0.2, cy - uy - vy * 0.2), (cx - vx * 0.05, cy - vy * 0.05), (cx + ux - vx * 0.2, cy + uy - vy * 0.2),
               (cx + ux + vx * 0.2, cy + uy + vy * 0.2), (cx + vx * 0.05, cy + vy * 0.05), (cx - ux + vx * 0.2, cy - uy + vy * 0.2)]
        canvas.paint([bar], 226, clip=[sphere])


def draw_front(canvas, controls, letters, text, style):
    """The front of the controller on `canvas` (the front view, or a hint piece that shows part of it enlarged)."""
    playstation = style == 'playstation'
    body = FRONT_BODY_PS if playstation else FRONT_BODY
    edge, dark = bumper(body, 7, (172, 80)) if playstation else bumper(body, 9, (186, 96))
    canvas.paint(edge, EDGE)
    canvas.paint(dark, DARK)
    canvas.paint([body], RIM)
    face = inset(body, 3.2)
    canvas.paint([face], BODY)
    if playstation:
        canvas.paint([PANEL], RIM, clip=[face])
        canvas.paint([inset(PANEL, 1.5)], 118, clip=[face])
        canvas.paint([TOUCHPAD], RIM)
        canvas.paint([inset(TOUCHPAD, 2)], 214)
        for side in ([(110, 50), (113.5, 50), (131.5, 127), (128, 127)], [(466, 50), (462.5, 50), (444.5, 127), (448, 127)]):
            canvas.paint([side], 236)                                     # the light bars beside the touchpad
        canvas.paint([circle(288, 152, 10.5)], LIGHT)                     # the PS button
        canvas.paint([circle(288, 152, 9.3)], EDGE)
        canvas.paint([circle(288, 152, 7.8)], DARK)
        canvas.paint([rounded_rect(278, 170, 298, 177, 3.5)], LIGHT)       # the mute button
        canvas.paint([rounded_rect(279.3, 171.3, 296.7, 175.7, 2.2)], DARK)
    else:
        xbox_logo(canvas, 288, 70, 14.5)
        canvas.paint([rounded_rect(280, 101, 296, 111, 4)], EDGE)         # the Share button
        canvas.paint([rounded_rect(281.7, 102.7, 294.3, 109.3, 3)], DARK)
    for cx, cy in FRONT_STICKS.values():
        stick(canvas, cx, cy)
    (arrow_pad if playstation else dpad)(canvas, *FRONT_DPAD)
    for slot, (cx, cy) in FRONT_FACE.items():
        dark_button(canvas, cx, cy, 10, letters, text.get(FACE_LAYOUT[slot]))
    for slot, (cx, cy) in FRONT_MENUS.items():
        center_button(canvas, cx, cy, controls.get(slot), letters, text, style=style)


@scaled
def front_view(controls, letters, text, style='xbox', scale=1):
    """The front view of an Xbox controller (or with style 'playstation' a DualSense) as (grey rows, alpha rows), 576x288 pixels.
    `controls` is prompt_layout.prompt_controls(); `text` names the pad buttons (botw_prompts.XBOX_TEXT or PLAYSTATION_TEXT);
    `letters` is a botw_prompts.Letters. With `scale` the picture is (576 * scale) x (288 * scale)."""
    canvas = Canvas(*FRONT, scale)
    draw_front(canvas, controls, letters, text, style)
    return canvas.rows()


def shoulder(left, x0, y0, x1, y1, trigger, width=TOP[0]):
    """The outline of a trigger (curved on its outer top corner) or a bumper (a rounded bar) in the box x0 y0 x1 y1."""
    if not trigger:
        return rounded_rect(x0, y0, x1, y1, 9)
    if left:
        return outline((x0, y1), ((x0 + 5, y1 - 13), (x0 + 15, y0), (x0 + 32, y0)), (x1 - 12, y0), ((x1 - 4, y0), (x1, y0 + 8), (x1, y0 + 16)), (x1, y1))
    return mirror(shoulder(True, width - x1, y0, width - x0, y1, True, width), width)


# The top edge seen from above, the back up: the back runs straight between the shoulders and the grips stand out towards the
# player at both ends (the DualSense's further and narrower).
TOP_BODY = both_halves(outline((288, 47), (100, 46), ((40, 46), (12, 52), (10, 68)), ((9, 84), (24, 97), (52, 97)),
                               ((96, 97), (124, 92), (160, 87)), ((200, 82), (240, 81), (288, 81))), TOP[0])
TOP_BODY_PS = both_halves(outline((288, 45), (100, 45), ((40, 45), (12, 50), (10, 66)), ((9, 86), (20, 98), (44, 98)),
                                  ((80, 98), (104, 93), (138, 86)), ((180, 79), (236, 78), (288, 78))), TOP[0])


@scaled
def top_view(controls, letters, text, style='xbox', scale=1):
    """The top edge of an Xbox controller (or a DualSense) as (grey rows, alpha rows), 576x100 pixels: the triggers and bumpers
    in the places of the Switch's ZL, L, ZR and R."""
    canvas = Canvas(*TOP, scale)
    playstation = style == 'playstation'
    body = TOP_BODY_PS if playstation else TOP_BODY
    canvas.paint([body], RIM)
    canvas.paint([inset(body, 3)], BODY)
    if playstation:
        canvas.paint([rounded_rect(116, 48, 460, 55, 3)], 214)            # the top edge of the touchpad
    else:
        canvas.paint([rounded_rect(100, 50, 476, 53, 1.5)], 208)          # a light edge along the back
    canvas.paint([rounded_rect(262, 58, 314, 69, 5)], EDGE)             # the USB-C port
    canvas.paint([rounded_rect(264.5, 60.5, 311.5, 66.5, 3)], DARK)
    if not playstation:
        canvas.paint([circle(352, 63, 5)], EDGE)                         # the pairing button (the DualSense has none on top)
        canvas.paint([circle(352, 63, 3.4)], DARK)
    for slot, (x0, y0, x1, y1) in TOP_SHOULDERS.items():
        trigger = slot in ('zl', 'zr')
        shape = shoulder(x0 < TOP[0] / 2, x0, y0, x1, y1, trigger)
        canvas.paint([shape], EDGE)
        canvas.paint([inset(shape, 1.7)], DARK)
        label = text.get(controls.get(slot))
        if label:
            canvas.paint(label_shape(letters, label, (x0 + x1) / 2, (y0 + y1) / 2 + (2 if trigger else 0), 9.0 if trigger else 8.0, x1 - x0 - 14), LIGHT)
    return canvas.rows()


# The hint pieces _02 and _03 are the front view enlarged 1.2 times: (zoom, dx, dy) that put the right stick (left stick) where
# the Switch piece has it.
WING_VIEW = {False: (1.2, PIECE_RIGHT['stick'][0] - 1.2 * FRONT_STICKS['rstick'][0], PIECE_RIGHT['stick'][1] - 1.2 * FRONT_STICKS['rstick'][1]),
             True: (1.2, PIECE_LEFT['stick'][0] - 1.2 * FRONT_STICKS['lstick'][0], PIECE_LEFT['stick'][1] - 1.2 * FRONT_STICKS['lstick'][1])}


@scaled
def wing(left, controls, letters, text, style='xbox', scale=1):
    """Nt_GamePadIllustSnn_02 (the right front: face buttons and right stick) or _03 (the left front: left stick and d-pad) as
    (grey rows, alpha rows), 228x228 pixels: that part of the front view, 1.2 times as large."""
    canvas = Canvas(*PIECE, scale, WING_VIEW[left])
    draw_front(canvas, controls, letters, text, style)
    return canvas.rows()


@scaled
def shoulders(left, controls, letters, text, style='xbox', scale=1):
    """Nt_GamePadIllustSnn_00 (the left shoulder: LT and LB) or _01 (the right: RT and RB) as (grey rows, alpha rows), 228x228
    pixels: the shoulder buttons over the corner of the controller seen from above, its grip standing out towards the player,
    the body cut at the picture's inner edge."""
    canvas = Canvas(*PIECE, scale)
    width = PIECE[0]
    band = outline((60, 124), (248, 124), (248, 168), (156, 171), ((122, 172), (110, 198), (82, 199)), ((60, 200), (49, 178), (49, 150)),
                   ((49, 134), (53, 124), (60, 124)))
    band = band if left else mirror(band, width)
    canvas.paint([band], RIM)
    canvas.paint([inset(band, 3)], BODY)
    port = 168 if left else width - 168
    canvas.paint([rounded_rect(port - 21, 140, port + 21, 150, 5)], EDGE)            # the USB-C port
    canvas.paint([rounded_rect(port - 18.5, 142.5, port + 18.5, 147.5, 3)], DARK)
    for slot, (x0, y0, x1, y1) in PIECE_SHOULDERS.items():
        if not left:
            x0, x1 = width - x1, width - x0
        trigger = slot == 'zl'
        shape = shoulder(left, x0, y0, x1, y1, trigger)
        canvas.paint([shape], EDGE)
        canvas.paint([inset(shape, 2.2)], DARK)
        label = text.get(controls.get(slot if left else {'zl': 'zr', 'l': 'r'}[slot]))
        if label:
            canvas.paint(label_shape(letters, label, (x0 + x1) / 2, (y0 + y1) / 2 + (3 if trigger else 0), 11.0 if trigger else 10.0, x1 - x0 - 20), LIGHT)
    return canvas.rows()


# texture name -> ((width, height), draw(controls, letters, text, style='xbox', scale=1)): the pictures that replace the game's, in
# Common.sblarc's BNTX. (width, height) is the game's size; draw(..., scale=n) returns a picture n times that size in each direction.
PICTURES = {'Nt_NNillust_00^f': (FRONT, front_view), 'Nt_NNillust_01^f': (TOP, top_view),
            'Nt_GamePadIllustSnn_00^f': (PIECE, lambda *a, **k: shoulders(True, *a, **k)),
            'Nt_GamePadIllustSnn_01^f': (PIECE, lambda *a, **k: shoulders(False, *a, **k)),
            'Nt_GamePadIllustSnn_02^f': (PIECE, lambda *a, **k: wing(False, *a, **k)), 'Nt_GamePadIllustSnn_03^f': (PIECE, lambda *a, **k: wing(True, *a, **k))}
