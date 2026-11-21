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


def build_mbot():
    """Returns {"Hull": Mesh, "Canopy": Mesh, "Engine": Mesh}."""
    hull = Mesh()
    # Long, low, pointed fuselage that swells over the middle and narrows to the engine.
    loft(hull, [
        (400, 112, 3, 2),
        (355, 113, 28, 18),
        (280, 117, 66, 40),
        (160, 123, 96, 56),
        (20, 127, 110, 64),
        (-130, 130, 104, 60),
        (-250, 132, 84, 50),
        (-330, 134, 60, 40),
    ], cap_end=True)
    # Swept wings, a little below the middle of the hull.
    for side in (1, -1):
        prism(hull, [(90, 92 * side, 116), (-245, 395 * side, 112), (-310, 395 * side, 112), (-235, 92 * side, 116)],
              (0, 0, 1), 16)
        # Twin tail fins, canted outwards.
        lean = 0.35
        fin = [(-175, 50 * side, 168), (-322, 50 * side, 172), (-350, (50 + 110 * lean) * side, 285), (-292, (50 + 110 * lean) * side, 285)]
        prism(hull, fin, (0, 1, -lean * side), 10)
    canopy = Mesh()
    loft(canopy, [
        (165, 166, 4, 3),
        (125, 172, 40, 30),
        (45, 180, 56, 42),
        (-35, 178, 52, 38),
        (-100, 168, 26, 22),
    ])
    engine = Mesh()
    loft(engine, [(-322, 134, 50, 33), (-352, 134, 46, 30)], sides=24)
    return {"Hull": hull, "Canopy": canopy, "Engine": engine}
