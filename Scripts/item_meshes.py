"""
Procedural belt meshes for the Acclivity Stone items (pure Python, written as OBJ like mbot_mesh).
Units cm, Z up, resting on Z = 0 (they ride on the belt surface).
"""
import math

from mbot_mesh import Mesh, _norm, _cross


def _ring(centre, axis, radius, sides, phase=0.0):
    """Points of a regular polygon around `axis` through `centre`."""
    a = _norm(axis)
    helper = (1.0, 0.0, 0.0) if abs(a[0]) < 0.9 else (0.0, 1.0, 0.0)
    u = _norm(_cross(a, helper))
    v = _cross(a, u)
    pts = []
    for k in range(sides):
        t = phase + 2 * math.pi * k / sides
        c, s = math.cos(t) * radius, math.sin(t) * radius
        pts.append((centre[0] + u[0] * c + v[0] * s, centre[1] + u[1] * c + v[1] * s, centre[2] + u[2] * c + v[2] * s))
    return pts


def crystal(mesh, base, axis, radius, length, tip, sides=6):
    """A hexagonal crystal: prism from `base` along `axis`, ending in a pyramid point."""
    a = _norm(axis)
    top = (base[0] + a[0] * length, base[1] + a[1] * length, base[2] + a[2] * length)
    point = (top[0] + a[0] * tip, top[1] + a[1] * tip, top[2] + a[2] * tip)
    r0, r1 = _ring(base, a, radius, sides), _ring(top, a, radius, sides)
    middle = ((base[0] + top[0]) / 2, (base[1] + top[1]) / 2, (base[2] + top[2]) / 2)
    for k in range(sides):
        k2 = (k + 1) % sides
        mesh.tri(r0[k], r0[k2], r1[k2], middle)
        mesh.tri(r0[k], r1[k2], r1[k], middle)
        mesh.tri(r1[k], r1[k2], point, middle)
        mesh.tri(r0[k], r0[k2], base, top)


def build_stone():
    """Three-crystal cluster like the Acclivity Stone icon, about 30 cm across."""
    m = Mesh()
    crystal(m, (0, 0, 0), (0.0, 0.0, 1.0), 6.5, 20, 9)
    crystal(m, (-4, -3, 0), (-0.45, -0.25, 1.0), 4.8, 13, 6)
    crystal(m, (4, 3, 0), (0.5, 0.2, 1.0), 4.8, 14, 6)
    crystal(m, (2, -5, 0), (0.15, -0.6, 1.0), 3.6, 9, 5)
    return m


def build_gem():
    """A brilliant-cut gem resting on its point: octagonal girdle, flat table on top, about 24 cm across."""
    m = Mesh()
    sides = 8
    apex = (0.0, 0.0, 0.0)
    girdle = _ring((0, 0, 11), (0, 0, 1), 12.0, sides)
    table = _ring((0, 0, 17), (0, 0, 1), 7.0, sides, phase=math.pi / sides)
    table_centre = (0.0, 0.0, 17.0)
    inside = (0.0, 0.0, 10.0)
    for k in range(sides):
        k2 = (k + 1) % sides
        m.tri(apex, girdle[k], girdle[k2], inside)        # pavilion
        m.tri(girdle[k], girdle[k2], table[k], inside)    # crown: kite facets between girdle and table
        m.tri(table[k - 1], girdle[k], table[k], inside)
        m.tri(table_centre, table[k - 1], table[k], inside)  # table
    return m


BELT_SCALE = 2.0  # the first versions (about 30 cm) looked small next to the game's ores


def scaled(mesh, factor=BELT_SCALE):
    """Uniformly scales a mesh about the origin (normals are unchanged by uniform scaling)."""
    out = Mesh()
    out.tris = [(tuple((p[0] * factor, p[1] * factor, p[2] * factor) for p in pts), ns) for pts, ns in mesh.tris]
    return out
