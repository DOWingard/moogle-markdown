"""Minimal z-buffered, textured software rasterizer for Wavefront OBJ models.

Renders a model to a transparent RGBA PNG. Used to turn the mascot 3D rips
into 2D sprites for the application UI.
"""
import argparse
import json
import math
import os
import sys
import time

import numpy as np
from PIL import Image


def log(level, msg, **kw):
    rec = {"timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"), "level": level,
           "context": "render_obj", "message": msg}
    rec.update(kw)
    print(json.dumps(rec), file=sys.stderr)


def load_mtl(path):
    mats, cur = {}, None
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            p = line.strip().split(None, 1)
            if not p:
                continue
            if p[0] == "newmtl":
                cur = p[1].strip()
                mats[cur] = {}
            elif p[0] == "map_Kd" and cur:
                mats[cur]["map_Kd"] = p[1].strip()
    return mats


def load_obj(path):
    base = os.path.dirname(path)
    V, VT, VN, faces = [], [], [], []
    mats, mat = {}, None
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            p = line.split()
            if not p:
                continue
            t = p[0]
            if t == "v":
                V.append([float(x) for x in p[1:4]])
            elif t == "vt":
                VT.append([float(x) for x in p[1:3]])
            elif t == "vn":
                VN.append([float(x) for x in p[1:4]])
            elif t == "mtllib":
                mats.update(load_mtl(os.path.join(base, line.split(None, 1)[1].strip())))
            elif t == "usemtl":
                mat = line.split(None, 1)[1].strip()
            elif t == "f":
                idx = []
                for c in p[1:]:
                    s = c.split("/")
                    vi = int(s[0])
                    ti = int(s[1]) if len(s) > 1 and s[1] else 0
                    vi = vi - 1 if vi > 0 else len(V) + vi
                    ti = (ti - 1 if ti > 0 else len(VT) + ti) if ti else -1
                    idx.append((vi, ti))
                for k in range(1, len(idx) - 1):  # fan triangulation
                    faces.append((idx[0], idx[k], idx[k + 1], mat))
    textures = {}
    for name, m in mats.items():
        if "map_Kd" in m:
            tp = os.path.join(base, m["map_Kd"])
            try:
                textures[name] = np.asarray(Image.open(tp).convert("RGBA"), dtype=np.float32) / 255.0
            except OSError as exc:
                log("error", "texture load failed", error=str(exc), texture=tp, material=name)
    log("info", "model loaded", path=path, vertices=len(V), uvs=len(VT), triangles=len(faces),
        materials=list(mats), textures=list(textures))
    return np.array(V, dtype=np.float64), np.array(VT, dtype=np.float64), faces, textures


def rot_y(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])


def rot_x(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])


def render(V, VT, faces, textures, size, yaw, pitch, ss, flip_v, light):
    R = rot_x(math.radians(pitch)) @ rot_y(math.radians(yaw))
    P = V @ R.T
    lo, hi = P.min(0), P.max(0)
    centre = (lo + hi) / 2
    extent = max(hi[0] - lo[0], hi[1] - lo[1])
    W = size * ss
    margin = 0.04
    scale = W * (1 - 2 * margin) / extent
    sx = (P[:, 0] - centre[0]) * scale + W / 2
    sy = W / 2 - (P[:, 1] - centre[1]) * scale
    sz = P[:, 2]  # larger z = closer to the viewer
    key = np.round(P / max(extent, 1e-9) * 2000).astype(np.int64)
    _, inv = np.unique(key, axis=0, return_inverse=True)
    inv = inv.reshape(-1)
    acc = np.zeros((inv.max() + 1, 3))
    for (a, b, c, _m) in faces:
        n = np.cross(P[b[0]] - P[a[0]], P[c[0]] - P[a[0]])
        # orient toward the viewer so inconsistent winding does not cancel out
        if n[2] < 0:
            n = -n
        for i in (a[0], b[0], c[0]):
            acc[inv[i]] += n
    nrm = acc / np.maximum(np.linalg.norm(acc, axis=1, keepdims=True), 1e-12)
    VN = nrm[inv]
    color = np.zeros((W, W, 4), dtype=np.float32)
    zbuf = np.full((W, W), -np.inf)
    L = np.array(light, dtype=np.float64)
    L /= np.linalg.norm(L)
    skipped = 0
    for (a, b, c, mat) in faces:
        ia, ib, ic = a[0], b[0], c[0]
        x0, y0, x1, y1, x2, y2 = sx[ia], sy[ia], sx[ib], sy[ib], sx[ic], sy[ic]
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if abs(area) < 1e-9:
            continue
        minx, maxx = int(max(0, math.floor(min(x0, x1, x2)))), int(min(W - 1, math.ceil(max(x0, x1, x2))))
        miny, maxy = int(max(0, math.floor(min(y0, y1, y2)))), int(min(W - 1, math.ceil(max(y0, y1, y2))))
        if minx > maxx or miny > maxy:
            continue
        xs, ys = np.meshgrid(np.arange(minx, maxx + 1) + 0.5, np.arange(miny, maxy + 1) + 0.5)
        w0 = ((x1 - xs) * (y2 - ys) - (x2 - xs) * (y1 - ys)) / area
        w1 = ((x2 - xs) * (y0 - ys) - (x0 - xs) * (y2 - ys)) / area
        w2 = 1 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not inside.any():
            continue
        z = w0 * sz[ia] + w1 * sz[ib] + w2 * sz[ic]
        sub_z = zbuf[miny:maxy + 1, minx:maxx + 1]
        vis = inside & (z > sub_z)
        if not vis.any():
            continue
        # Smooth two-sided Lambert shading; rips frequently have inconsistent winding.
        ns = w0[..., None] * VN[ia] + w1[..., None] * VN[ib] + w2[..., None] * VN[ic]
        ns /= np.maximum(np.linalg.norm(ns, axis=-1, keepdims=True), 1e-12)
        shade = (0.58 + 0.42 * np.abs(ns @ L))[..., None]
        tex = textures.get(mat)
        if tex is not None and a[1] >= 0 and b[1] >= 0 and c[1] >= 0:
            uv = w0[..., None] * VT[a[1]] + w1[..., None] * VT[b[1]] + w2[..., None] * VT[c[1]]
            th, tw = tex.shape[:2]
            u = np.mod(uv[..., 0], 1.0)
            v = np.mod(uv[..., 1], 1.0)
            if flip_v:
                v = 1.0 - v
            tx = np.clip((u * tw).astype(int), 0, tw - 1)
            ty = np.clip((v * th).astype(int), 0, th - 1)
            px = tex[ty, tx]
        else:
            skipped += 1
            px = np.broadcast_to(np.array([0.8, 0.8, 0.8, 1.0], dtype=np.float32), w0.shape + (4,))
        vis &= px[..., 3] > 0.5  # alpha test for wing/hair cut-outs
        if not vis.any():
            continue
        rgb = np.clip(px[..., :3] * shade, 0, 1)
        sub_c = color[miny:maxy + 1, minx:maxx + 1]
        sub_c[vis, :3] = rgb[vis]
        sub_c[vis, 3] = 1.0
        sub_z[vis] = z[vis]
    if skipped:
        log("warn", "faces rendered without texture", count=skipped)
    img = Image.fromarray((color * 255).astype(np.uint8), "RGBA")
    # Premultiplied downsample so transparent edge pixels do not bleed black.
    return downsample(img, size)


def downsample(img, size):
    a = np.asarray(img, dtype=np.float32) / 255.0
    pm = a.copy()
    pm[..., :3] *= pm[..., 3:4]
    k = a.shape[0] // size
    pm = pm.reshape(size, k, size, k, 4).mean(axis=(1, 3))
    out = pm.copy()
    nz = out[..., 3] > 0
    out[nz, :3] /= out[nz, 3:4]
    return Image.fromarray(np.clip(out * 255 + 0.5, 0, 255).astype(np.uint8), "RGBA")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("obj")
    ap.add_argument("out")
    ap.add_argument("--size", type=int, default=256)
    ap.add_argument("--yaw", type=float, default=0.0)
    ap.add_argument("--pitch", type=float, default=0.0)
    ap.add_argument("--ss", type=int, default=4)
    ap.add_argument("--flip-v", action="store_true")
    ap.add_argument("--light", type=float, nargs=3, default=[0.4, 0.6, 1.0])
    args = ap.parse_args()
    t0 = time.time()
    try:
        V, VT, faces, tex = load_obj(args.obj)
        img = render(V, VT, faces, tex, args.size, args.yaw, args.pitch, args.ss, args.flip_v, args.light)
        bbox = img.getbbox()
        img.save(args.out)
        log("info", "render complete", out=args.out, seconds=round(time.time() - t0, 2), bbox=bbox)
    except Exception as exc:  # noqa: BLE001 - top-level CLI guard
        import traceback
        log("error", "render failed", error=str(exc), stack=traceback.format_exc(), obj=args.obj)
        sys.exit(1)


if __name__ == "__main__":
    main()
