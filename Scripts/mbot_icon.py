"""Renders M-Bot's build-menu icon from the same geometry as the in-game meshes (pure Python, no deps).

A small z-buffered software rasterizer: orthographic three-quarter view from above, wings out as in flight,
written as an RGBA PNG with a soft blue glow behind the ship.
"""
import math
import struct
import zlib

import mbot_mesh

HULL = (0.16, 0.17, 0.20)
CANOPY = (0.20, 0.55, 0.85)
ENGINE = (0.35, 0.75, 1.00)


def _png(path, size, pixels):
    rows = []
    for j in range(size):
        row = bytearray([0])
        for i in range(size):
            r, g, b, a = pixels[j * size + i]
            row += bytes([max(0, min(255, int(c * 255 + 0.5))) for c in (r, g, b, a)])
        rows.append(bytes(row))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def _scene():
    """(triangle points, base colour, emissive) in Unreal space: X forward, Y right, Z up."""
    parts = mbot_mesh.build_mbot()
    out = []
    for name, colour, emissive in (("Hull", HULL, 0.0), ("Canopy", CANOPY, 0.35), ("Engine", ENGINE, 1.0)):
        for pts, _ in parts[name].tris:
            out.append((pts, colour, emissive))
    px, py, pz = mbot_mesh.WING_PIVOT
    for side in (1.0, -1.0):
        for pts, _ in parts["Wing"].tris:
            out.append((tuple((p[0] + px, (p[1] + py) * side, p[2] + pz) for p in pts), HULL, 0.0))
    return out


def render_icon(path, size=256, supersample=2, yaw_deg=-35.0, pitch_deg=50.0):
    tris = _scene()
    # Camera: turn the ship so its nose points to the upper right, then look down on it.
    cy, sy = math.cos(math.radians(yaw_deg)), math.sin(math.radians(yaw_deg))
    cp, sp = math.cos(math.radians(pitch_deg)), math.sin(math.radians(pitch_deg))

    def view(p):
        x, y, z = p
        # Yaw about Z.
        x, y = x * cy - y * sy, x * sy + y * cy
        # Screen: right = +Y, up = +X tilted by pitch; depth towards the camera.
        sx = y
        sup = x * sp + z * cp
        depth = -x * cp + z * sp
        return sx, sup, depth

    light = (0.35, -0.45, 0.82)  # in world space, from above and front-left
    ll = math.sqrt(sum(c * c for c in light))
    light = tuple(c / ll for c in light)
    cam = (-cp * cy, cp * sy, sp)  # world direction towards the camera (approx., for specular)

    projected = []
    for pts, colour, emissive in tris:
        a, b, c = pts
        u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        v = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
        n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        nl = math.sqrt(sum(k * k for k in n)) or 1.0
        n = tuple(k / nl for k in n)
        diff = abs(sum(n[i] * light[i] for i in range(3)))
        h = tuple(light[i] + cam[i] for i in range(3))
        hl = math.sqrt(sum(k * k for k in h)) or 1.0
        spec = abs(sum(n[i] * h[i] / hl for i in range(3))) ** 40
        shade = tuple(min(1.0, colour[i] * (0.35 + 0.9 * diff) + 0.45 * spec + emissive * colour[i]) for i in range(3))
        projected.append(([view(p) for p in pts], shade))

    xs = [p[0] for t, _ in projected for p in t]
    ys = [p[1] for t, _ in projected for p in t]
    span = max(max(xs) - min(xs), max(ys) - min(ys))
    big = size * supersample
    scale = big * 0.86 / span
    ox = big / 2 - (max(xs) + min(xs)) / 2 * scale
    oy = big / 2 + (max(ys) + min(ys)) / 2 * scale

    colour_buf = [None] * (big * big)
    depth_buf = [-1e30] * (big * big)
    for pts, shade in projected:
        sp_ = [(p[0] * scale + ox, oy - p[1] * scale, p[2]) for p in pts]
        (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = sp_
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if abs(area) < 1e-9:
            continue
        minx, maxx = max(0, int(min(x0, x1, x2))), min(big - 1, int(max(x0, x1, x2)) + 1)
        miny, maxy = max(0, int(min(y0, y1, y2))), min(big - 1, int(max(y0, y1, y2)) + 1)
        for j in range(miny, maxy + 1):
            py = j + 0.5
            for i in range(minx, maxx + 1):
                px = i + 0.5
                w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area
                w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area
                w2 = 1.0 - w0 - w1
                if w0 < 0 or w1 < 0 or w2 < 0:
                    continue
                z = w0 * z0 + w1 * z1 + w2 * z2
                k = j * big + i
                if z > depth_buf[k]:
                    depth_buf[k] = z
                    colour_buf[k] = shade

    pixels = []
    for j in range(size):
        for i in range(size):
            r = g = b = a = 0.0
            for dj in range(supersample):
                for di in range(supersample):
                    c = colour_buf[(j * supersample + dj) * big + i * supersample + di]
                    if c:
                        r += c[0]; g += c[1]; b += c[2]; a += 1.0
            n = supersample * supersample
            cover = a / n
            if cover > 0:
                r, g, b = r / a, g / a, b / a
            # Soft blue glow behind, like the Acclivity Stone icons, so the dark hull stands out in the menu.
            d = math.hypot((i + 0.5) / size - 0.5, (j + 0.5) / size - 0.5)
            glow = max(0.0, 1.0 - d / 0.5) ** 2 * 0.6
            gr, gg, gb = 0.25, 0.6, 1.0
            out_a = cover + glow * (1.0 - cover)
            if out_a > 0:
                r = (r * cover + gr * glow * (1.0 - cover)) / out_a
                g = (g * cover + gg * glow * (1.0 - cover)) / out_a
                b = (b * cover + gb * glow * (1.0 - cover)) / out_a
            pixels.append((r, g, b, out_a))
    _png(path, size, pixels)


if __name__ == "__main__":
    import sys
    render_icon(sys.argv[1] if len(sys.argv) > 1 else "Icon_MBot.png")
