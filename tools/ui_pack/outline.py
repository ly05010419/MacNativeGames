"""Exact polygons of the union of occupied pixel squares (MIT, original implementation)."""
import numpy as np


def pixel_boundaries(mask):
    """Follow exposed directed grid edges, keeping foreground on their right.

    At a diagonal contact, take a right turn first: touching pixels keep separate
    rings instead of making a self-crossing polygon. Hole rings wind oppositely.
    """
    filled = np.pad(np.asarray(mask, dtype=bool), 1)
    edges = {}
    def edge(x, y, direction):
        edges.setdefault((int(x), int(y)), set()).add(direction)
    for y, x in np.argwhere(filled[1:-1, 1:-1] & ~filled[:-2, 1:-1]): edge(x, y, 0)
    for y, x in np.argwhere(filled[1:-1, 1:-1] & ~filled[1:-1, 2:]): edge(x + 1, y, 1)
    for y, x in np.argwhere(filled[1:-1, 1:-1] & ~filled[2:, 1:-1]): edge(x + 1, y + 1, 2)
    for y, x in np.argwhere(filled[1:-1, 1:-1] & ~filled[1:-1, :-2]): edge(x, y + 1, 3)
    step = ((1, 0), (0, 1), (-1, 0), (0, -1))
    rings = []
    while edges:
        start = position = next(iter(edges))
        direction = min(edges[start]); points = []
        while True:
            points.append(position)
            edges[position].remove(direction)
            if not edges[position]: del edges[position]
            dx, dy = step[direction]; position = (position[0] + dx, position[1] + dy)
            if position == start: break
            choices = edges[position]
            direction = next(d for d in ((direction+1)%4, direction, (direction+3)%4, (direction+2)%4) if d in choices)
        P = np.asarray(points, dtype=float)
        before, after = P - np.roll(P, 1, axis=0), np.roll(P, -1, axis=0) - P
        corners = before[:, 0] * after[:, 1] != before[:, 1] * after[:, 0]
        rings.append(P[corners])
    return rings
