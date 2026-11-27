"""
Procedural low-poly meshes for the other Skyward ships, in the same style and units as mbot_mesh.py
(cm, Unreal axes: X forward, Y right, Z up, origin at the vehicle's origin).

  Poco: the DDF's basic fighter, "a glorified pencil with wings": a long needle nose, swept wings
        with pods on the tips, small canards and a light-lance turret underneath.
  Krell interceptor: an egg-shaped head ringed with frills and spikes, one dark eye at the front,
        and tentacles trailing behind.
"""
import math

from mbot_mesh import Mesh, loft, prism, _norm


def tube(mesh, path, radii, sides=10):
    """Round tube along a 3D path (points), radius per point; capped at the thin end."""
    rings = []
    for i, p in enumerate(path):
        a = path[max(i - 1, 0)]
        b = path[min(i + 1, len(path) - 1)]
        t = _norm((b[0] - a[0], b[1] - a[1], b[2] - a[2]))
        up = (0.0, 0.0, 1.0) if abs(t[2]) < 0.9 else (0.0, 1.0, 0.0)
        u = _norm((t[1] * up[2] - t[2] * up[1], t[2] * up[0] - t[0] * up[2], t[0] * up[1] - t[1] * up[0]))
        v = (t[1] * u[2] - t[2] * u[1], t[2] * u[0] - t[0] * u[2], t[0] * u[1] - t[1] * u[0])
        ring = []
        for k in range(sides):
            ang = 2 * math.pi * k / sides
            c, s = math.cos(ang), math.sin(ang)
            n = (u[0] * c + v[0] * s, u[1] * c + v[1] * s, u[2] * c + v[2] * s)
            ring.append(((p[0] + n[0] * radii[i], p[1] + n[1] * radii[i], p[2] + n[2] * radii[i]), n))
        rings.append(ring)
    for r in range(len(rings) - 1):
        inside = tuple((path[r][j] + path[r + 1][j]) / 2 for j in range(3))
        for k in range(sides):
            k2 = (k + 1) % sides
            (a, na), (b, nb) = rings[r][k], rings[r][k2]
            (c, nc), (d, nd) = rings[r + 1][k2], rings[r + 1][k]
            mesh.tri(a, b, c, inside, (na, nb, nc))
            mesh.tri(a, c, d, inside, (na, nc, nd))
    end = path[-1]
    before = path[-2]
    for k in range(sides):
        mesh.tri(end, rings[-1][k][0], rings[-1][(k + 1) % sides][0], before)


# ---------------------------------------------------------------------------------------------
POCO_LENGTH = 800.0


def build_poco():
    """Returns {"Hull", "Canopy", "Engine"}. About 8 m long and 4.4 m across the wing pods."""
    z = 120.0
    hull = Mesh()
    # Fuselage: engine block at the back, the cockpit section, then the long needle nose.
    loft(hull, [
        (-400, z, 40, 34),
        (-330, z, 58, 44),
        (-150, z, 62, 46),
        (-20, z, 50, 40),
        (80, z, 30, 26),
        (220, z - 2, 16, 14),
        (400, z - 4, 3, 3),
    ], power=2.4)
    for side in (1, -1):
        # Main wings: swept back, drooping a little towards the pods.
        root_y, tip_y, droop = 40.0, 200.0, -22.0
        prism(hull, [(-40, root_y * side, z), (-190, tip_y * side, z + droop), (-305, tip_y * side, z + droop), (-300, root_y * side, z)], (0, 0, 1), 12)
        # Wing-tip pods (the guns), with a fin on top.
        pod_y = 212.0 * side
        pod = Mesh()
        loft(pod, [(-340, 0, 16, 16), (-300, 0, 22, 22), (-150, 0, 20, 20), (-100, 0, 4, 4)], sides=12, power=2.0)
        for (a, b, c), ns in pod.tris:
            hull.tris.append((tuple((p[0], p[1] + pod_y, p[2] + z + droop) for p in (a, b, c)), ns))
        prism(hull, [(-320, pod_y, z + droop + 15), (-250, pod_y, z + droop + 15), (-300, pod_y, z + droop + 60)], (0, 1, 0), 6)
        # Small canards ahead of the wings.
        prism(hull, [(90, 26 * side, z), (10, 110 * side, z - 4), (-30, 110 * side, z - 4), (0, 26 * side, z)], (0, 0, 1), 8)
        # Tail stubs either side of the engine.
        prism(hull, [(-300, 50 * side, z + 10), (-420, 80 * side, z + 10), (-420, 60 * side, z + 10), (-330, 40 * side, z + 10)], (0, 0, 1), 10)
    # Light-lance turret underneath.
    loft(hull, [(-60, z - 52, 4, 4), (-90, z - 56, 20, 14), (-150, z - 54, 20, 14), (-170, z - 50, 4, 4)], sides=12, power=2.0)
    canopy = Mesh()
    loft(canopy, [(40, z + 36, 4, 3), (10, z + 46, 26, 18), (-80, z + 50, 32, 20), (-140, z + 44, 4, 3)])
    engine = Mesh()
    loft(engine, [(-396, z, 32, 26), (-412, z, 30, 24)], sides=20)
    return {"Hull": hull, "Canopy": canopy, "Engine": engine}


POCO_MUZZLES = [(-90, 212, 98), (-90, -212, 98)]


# ---------------------------------------------------------------------------------------------
KRELL_LENGTH = 720.0


def build_krell():
    """Returns {"Hull", "Eye", "Tentacles"}. A 3.2 m head ~4 m across, tentacles trailing ~4 m behind."""
    z = 125.0
    hull = Mesh()
    head = [
        (175, z - 4, 8, 6),
        (155, z, 95, 42),
        (100, z, 175, 64),
        (20, z, 200, 72),
        (-60, z + 2, 170, 66),
        (-115, z + 2, 105, 50),
        (-145, z, 50, 34),
        (-150, z, 10, 8),
    ]
    loft(hull, head, sides=28, power=2.1)
    # Frills and spikes around the rim of the head (wider at the sides, as in the front view).
    count = 16
    for i in range(count):
        ang = 2 * math.pi * (i + 0.5) / count
        c, s = math.cos(ang), math.sin(ang)
        if s < -0.5:
            continue  # none underneath: they'd dig into the ground
        for x0, w, h, length in ((70, 175, 64, 60), (-40, 175, 66, 75)):
            base_y, base_z = w * 0.95 * c, z + h * 0.95 * s
            out = _norm((0.0, c * 1.4, s))
            tip = (x0 - 35, base_y + out[1] * length, base_z + out[2] * length)
            prism(hull, [(x0 + 28, base_y, base_z), (x0 - 28, base_y, base_z), tip], (0.0, -s, c), 6)
    # Crest along the top, like the side view's ridge.
    prism(hull, [(110, 0, z + 60), (-120, 0, z + 54), (-60, 0, z + 100)], (0, 1, 0), 8)

    eye = Mesh()
    # One big eye in the middle of the face (front view), bulging out of the nose.
    loft(eye, [(192, z, 6, 4), (182, z, 40, 24), (160, z, 50, 30), (135, z, 46, 28)], sides=20, power=2.0)

    tentacles = Mesh()
    # (start y, start z, length, wave amplitude, phase): spread round the back of the head.
    for i, (y0, z0, length, amp, phase) in enumerate((
            (0, 30, 520, 45, 0.0), (55, 12, 480, 55, 1.2), (-55, 12, 480, 55, 2.1),
            (95, -14, 430, 60, 0.6), (-95, -14, 430, 60, 2.8), (40, -32, 400, 50, 1.8), (-40, -32, 400, 50, 0.3))):
        path, radii = [], []
        steps = 14
        for k in range(steps + 1):
            t = k / steps
            x = -130 - length * t
            spread = 1.0 + 0.8 * t
            y = y0 * spread + amp * t * math.sin(phase + t * 7.0)
            zz = z + z0 * spread + amp * 0.5 * t * math.cos(phase + t * 6.0) - 25 * t
            path.append((x, y, zz))
            radii.append(17.0 * (1.0 - t) + 3.0)
        tube(tentacles, path, radii, sides=8)
    return {"Hull": hull, "Eye": eye, "Tentacles": tentacles}


KRELL_MUZZLES = [(150, 60, 120), (150, -60, 120)]
