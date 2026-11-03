"""Draw the game's button icons for the player's layout (E6 S6.3, C1-C2). Run by the builder with its icons venv's
Python (`<venv>/bin/python -I icons_generate.py <romfs out> <app> <version> <plan.json>`), as a process: fontTools
lives there, not in the launcher's Python.

It adds a 'layout' style to games/totk/scripts/make_button_icons.py (imported, nothing copied) whose glyphs come from
the plan icons_plan.plan() wrote (code point, kind, args), and runs its main() into <romfs out>: the four font
archives and the resource size table, grown on top of a table already in that folder (the builder seeds the other
enabled mods' table there first, S6.3 B1). Shapes are drawn with the script's own helpers: a circle with a letter,
LB/RB and LT/RT shapes, Menu (three bars) and View (two squares), a d-pad with lit arms, a crossed circle for an
input no button drives, and labels side by side for a combined d-pad glyph whose inputs are elsewhere."""
import json
from pathlib import Path
import sys

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'games/totk/scripts'))
import make_button_icons as icons  # noqa: E402

ADVANCE = {'face': 640, 'bumper': 820, 'trigger': 760, 'menu': 640, 'view': 640, 'dpad': 640, 'none': 640}


def _hole(pen):
    return icons.ReverseContourPen(pen)


def drawer(g, kind, *args):
    """(advance, draw(pen)) for one plan entry."""
    mid = 320
    if kind == 'face':
        label = args[0]
        return 640, lambda pen: (icons.circle(pen, mid, icons.CY, icons.R),
                                 g.hole(pen, label, mid, icons.CY, icons.R * (0.95 if len(label) == 1 else 0.7),
                                        icons.R * 1.3))
    if kind == 'bumper':
        return 820, lambda pen: (icons.rounded(pen, 40, icons.Y0 + 70, 780, icons.Y1 - 70, 70, 150),
                                 g.hole(pen, args[0], 410, icons.CY, 230, 620))
    if kind == 'trigger':
        return 760, lambda pen: (icons.rounded(pen, 60, icons.Y0, 700, icons.Y1, 60, 230),
                                 g.hole(pen, args[0], 380, icons.CY - 20, 230, 540))
    if kind == 'menu':
        def draw(pen):
            icons.circle(pen, mid, icons.CY, icons.R)
            for dy in (-115, 0, 115):
                icons.rounded(_hole(pen), mid - 150, icons.CY + dy - 38, mid + 150, icons.CY + dy + 38, 38)
        return 640, draw
    if kind == 'view':
        def draw(pen):
            icons.circle(pen, mid, icons.CY, icons.R)
            icons.rounded(_hole(pen), mid - 165, icons.CY + 15, mid - 15, icons.CY + 165, 16)
            icons.rounded(_hole(pen), mid + 15, icons.CY - 165, mid + 165, icons.CY - 15, 16)
        return 640, draw
    if kind == 'dpad':
        return 640, lambda pen: _dpad(pen, mid, args[0])
    if kind == 'labels':
        labels = args[0]
        width = 330 * len(labels) + 80
        def draw(pen):
            step = (width - 80) / len(labels)
            for i, label in enumerate(labels):
                icons.key(g, pen, 40 + i * step + 12, icons.Y0 + 60, 40 + (i + 1) * step - 12, icons.Y1 - 60, label)
        return width, draw
    def draw(pen):                                      # 'none': a crossed circle
        icons.circle(pen, mid, icons.CY, icons.R)
        bar = icons.R * 0.16
        icons.polygon(_hole(pen), [(mid - icons.R * 0.62 - bar, icons.CY - icons.R * 0.62 + bar),
                                   (mid - icons.R * 0.62 + bar, icons.CY - icons.R * 0.62 - bar),
                                   (mid + icons.R * 0.62 + bar, icons.CY + icons.R * 0.62 - bar),
                                   (mid + icons.R * 0.62 - bar, icons.CY + icons.R * 0.62 + bar)])
    return 640, draw


def _dpad(pen, cx, lit):
    cy, length, arm = icons.CY, icons.R, icons.R * 0.40
    icons.polygon(pen, [(cx + length, cy - arm), (cx + length, cy + arm), (cx + arm, cy + arm), (cx + arm, cy + length),
                        (cx - arm, cy + length), (cx - arm, cy + arm), (cx - length, cy + arm), (cx - length, cy - arm),
                        (cx - arm, cy - arm), (cx - arm, cy - length), (cx + arm, cy - length), (cx + arm, cy - arm)])
    c, h, t = (length + arm) / 2, arm * 0.72, arm * 0.80          # arm centre, half base and height of the arrow
    heads = {'up': [(cx + h, cy + c - t / 2), (cx, cy + c + t / 2), (cx - h, cy + c - t / 2)],
             'down': [(cx - h, cy - c + t / 2), (cx, cy - c - t / 2), (cx + h, cy - c + t / 2)],
             'left': [(cx - c + t / 2, cy + h), (cx - c - t / 2, cy), (cx - c + t / 2, cy - h)],
             'right': [(cx + c - t / 2, cy - h), (cx + c + t / 2, cy), (cx + c - t / 2, cy + h)]}
    for direction in lit:
        icons.polygon(_hole(pen), heads[direction])


def style(entries):
    return lambda g: {cp: drawer(g, kind, *args) for cp, kind, *args in entries}


def main(argv):
    out, app, version, plan_file = argv
    icons.STYLES['layout'] = style(json.loads(Path(plan_file).read_text()))
    sys.argv = ['make_button_icons.py', out, '--app', app, '--version', version, '--style', 'layout']
    icons.main()


if __name__ == '__main__':
    main(sys.argv[1:])
