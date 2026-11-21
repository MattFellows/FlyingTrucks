"""
Procedural low-poly meshes for M-Bot (a sleek black starfighter), written as OBJ files.

Pure Python (no Unreal needed), so it can also be previewed outside the editor.
Units are cm in Unreal's axes: X forward, Y right, Z up, origin at the vehicle's origin.
Triangles are wound so that cross(b - a, c - a) points outwards (the same convention as the ring).
"""
import math


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _norm(a):
    l = math.sqrt(_dot(a, a)) or 1.0
    return (a[0] / l, a[1] / l, a[2] / l)


class Mesh:
    def __init__(self):
        self.tris = []  # ((p0, p1, p2), (n0, n1, n2))

    def tri(self, a, b, c, inside, normals=None):
        """Adds a triangle facing away from the point `inside`."""
        n = _cross(_sub(b, a), _sub(c, a))
        centre = ((a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3, (a[2] + b[2] + c[2]) / 3)
        if _dot(n, _sub(centre, inside)) < 0:
            b, c = c, b
            if normals:
                normals = (normals[0], normals[2], normals[1])
            n = (-n[0], -n[1], -n[2])
        if _dot(n, n) < 1e-10:
            return
        self.tris.append(((a, b, c), normals or (_norm(n),) * 3))

    def write_obj(self, path, comment):
        lines = ["# " + comment]
        for i, (pts, ns) in enumerate(self.tris):
            for p in pts:
                lines.append("v %.3f %.3f %.3f" % p)
            for n in ns:
                lines.append("vn %.5f %.5f %.5f" % n)
        lines.append("vt 0 0")
        for i in range(len(self.tris)):
            b = 3 * i
            lines.append("f %d/1/%d %d/1/%d %d/1/%d" % (b + 1, b + 1, b + 2, b + 2, b + 3, b + 3))
        with open(path, "w") as f:
            f.write("\n".join(lines) + "\n")


def _section(x, cz, w, h, n, p):
    pts = []
    for k in range(n):
        t = 2 * math.pi * k / n
        c, s = math.cos(t), math.sin(t)
        y = w * math.copysign(abs(c) ** (2.0 / p), c)
        z = cz + h * math.copysign(abs(s) ** (2.0 / p), s)
        pts.append((x, y, z))
    return pts


def loft(mesh, sections, sides=20, power=2.6, cap_start=True, cap_end=True):
    """Smooth-shaded body through cross-sections (x, centre z, half width, half height), front to back."""
    rings = [_section(x, cz, w, h, sides, power) for x, cz, w, h in sections]
    centres = [(x, 0.0, cz) for x, cz, _, _ in sections]
    for r in range(len(rings) - 1):
        r0, r1 = rings[r], rings[r + 1]
        c0, c1 = centres[r], centres[r + 1]
        inside = ((c0[0] + c1[0]) / 2, 0.0, (c0[2] + c1[2]) / 2)
        for k in range(sides):
            k2 = (k + 1) % sides
            quad = (r0[k], r0[k2], r1[k2], r1[k])
            norms = tuple(_norm((0.0, p[1], (p[2] - (c0 if p in r0 else c1)[2]) * 1.0)) for p in quad)
            mesh.tri(quad[0], quad[1], quad[2], inside, (norms[0], norms[1], norms[2]))
            mesh.tri(quad[0], quad[2], quad[3], inside, (norms[0], norms[2], norms[3]))
    for do_cap, idx, step in ((cap_start, 0, 1), (cap_end, len(rings) - 1, -1)):
        if not do_cap:
            continue
        ring, centre = rings[idx], centres[idx]
        inside = centres[idx + step] if 0 <= idx + step < len(centres) else centre
        for k in range(sides):
            mesh.tri(centre, ring[k], ring[(k + 1) % sides], inside)


def prism(mesh, outline, thickness_dir, thickness):
    """Flat-shaded slab from a convex planar outline (3D points), `thickness` thick along `thickness_dir`."""
    d = _norm(thickness_dir)
    h = thickness / 2.0
    top = [(p[0] + d[0] * h, p[1] + d[1] * h, p[2] + d[2] * h) for p in outline]
    bot = [(p[0] - d[0] * h, p[1] - d[1] * h, p[2] - d[2] * h) for p in outline]
    n = len(outline)
    inside = tuple(sum(p[i] for p in outline) / n for i in range(3))
    for i in range(1, n - 1):
        mesh.tri(top[0], top[i], top[i + 1], inside)
        mesh.tri(bot[0], bot[i], bot[i + 1], inside)
    for i in range(n):
        j = (i + 1) % n
        mesh.tri(top[i], top[j], bot[j], inside)
        mesh.tri(top[i], bot[j], bot[i], inside)


WING_PIVOT = (10.0, 200.0, 125.0)  # right wing's pivot on the body (front corner of the wing root), before scaling
WING_SWING_DEGREES = 34.0  # wings swing out by this much in flight (the faint outline in the reference drawing)


def _rot_yaw(p, deg):
    a = math.radians(deg)
    return (p[0] * math.cos(a) - p[1] * math.sin(a), p[0] * math.sin(a) + p[1] * math.cos(a), p[2])


def build_mbot():
    """Returns {"Hull", "Canopy", "Engine", "Wing"} meshes. Nose along +X.

    Hull: central body with a rounded front pod, two big forward prongs either side, an engine block between
    two thin rear spikes, and small canted fins. Wing: the right main wing in its *extended* (flight) pose,
    pivoting on its root's front corner at the origin; the game swings it back WING_SWING_DEGREES on the ground.
    """
    hull = Mesh()
    # Central body: engine block at the back, a narrower neck, then the rounded front pod.
    loft(hull, [
        (-310, 120, 58, 38),
        (-200, 124, 86, 52),
        (20, 127, 90, 56),
        (80, 128, 62, 46),
        (120, 129, 52, 42),
        (150, 130, 76, 54),
        (215, 130, 80, 56),
        (245, 130, 66, 47),
        (262, 130, 36, 27),
        (267, 130, 6, 5),
    ], power=3.0)
    for side in (1, -1):
        # Forward prongs: chunky angular blades that reach past the pod to sharp points.
        prong = [(380, 146 * side, 128), (140, 214 * side, 128), (-60, 180 * side, 128), (-60, 90 * side, 128), (150, 76 * side, 128)]
        prism(hull, prong, (0, 0, 1), 42)
        # Thin spikes either side of the engine.
        prism(hull, [(-195, 62 * side, 122), (-195, 38 * side, 122), (-385, 50 * side, 122)], (0, 0, 1), 14)
        # Small canted fins behind the wing roots.
        lean = 0.45
        fin = [(-150, 100 * side, 160), (-235, 100 * side, 160), (-268, (100 + 70 * lean) * side, 225), (-222, (100 + 70 * lean) * side, 225)]
        prism(hull, fin, (0, 1, -lean * side), 8)
    canopy = Mesh()
    loft(canopy, [
        (258, 148, 5, 4),
        (238, 158, 48, 30),
        (185, 166, 60, 38),
        (140, 160, 44, 28),
        (118, 150, 6, 4),
    ])
    engine = Mesh()
    loft(engine, [(-302, 120, 48, 30), (-330, 120, 44, 27)], sides=24)

    # Right main wing (+Y), folded (ground) pose relative to its pivot, then swung out to the flight pose.
    folded = [(0.0, 0.0, 0.0), (-390.0, 120.0, 0.0), (-200.0, -134.0, 0.0)]
    extended = [_rot_yaw(p, -WING_SWING_DEGREES) for p in folded]
    wing = Mesh()
    prism(wing, extended, (0, 0, 1), 14)

    # Fill the gap between the body and the extended wing: a thin plate from the wing's two root corners
    # (in the flight pose) straight in to the body. Swept back on the ground, the wing covers it.
    px, py, pz = WING_PIVOT
    root_front, root_rear = extended[0], extended[2]
    for side in (1, -1):
        outer_front = (px + root_front[0], (py + root_front[1]) * side, pz)
        outer_rear = (px + root_rear[0], (py + root_rear[1]) * side, pz)
        inner_rear = (outer_rear[0], 70 * side, pz)
        inner_front = (outer_front[0], 70 * side, pz)
        prism(hull, [outer_front, outer_rear, inner_rear, inner_front], (0, 0, 1), 12)
    return {"Hull": hull, "Canopy": canopy, "Engine": engine, "Wing": wing}


