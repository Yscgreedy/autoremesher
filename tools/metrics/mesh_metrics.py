#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""AutoRemesher 网格质量度量工具（numpy-only，不依赖 scipy）。

用法
----
单个网格:
    python mesh_metrics.py TARGET.obj [--ref REF.obj] [--json OUT.json] [--samples N]

基线对比:
    python mesh_metrics.py compare BASELINE.json CANDIDATE.json [--json OUT.json]
    python mesh_metrics.py                      # 等价于 compare baseline.json candidate.json

设计要点
--------
* 只使用 numpy；OBJ 解析、连通性（并查集）、曲面最近点查询（均匀网格哈希）全部自实现。
* 面用 CSR（face_starts + face_idx）存储，支持 n 边形、负索引、同文件多对象、
  形如 "f v/vt/vn" / "f v//vn" 的面，以及「首尾重复顶点」（v1 v2 v3 v4 v1）写法。
* 所有几何量均为双精度；退化判定与偏差同时给出相对量（除以包围盒对角线），便于跨尺度比较。

退出码: 0 正常 / 1 compare 检出回归 / 2 输入错误。
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
import time

import numpy as np

TOOL_NAME = "mesh_metrics"
TOOL_VERSION = 1

_SLASH_RE = re.compile(rb"/[^\s]*")


# ---------------------------------------------------------------------------
# 基础工具
# ---------------------------------------------------------------------------
def _log(msg, quiet=False):
    if not quiet:
        sys.stderr.write("[mesh_metrics] " + msg + "\n")
        sys.stderr.flush()


def _fmt(x, nd=6):
    """统一数值格式化：None / nan / inf 有稳定表现。"""
    if x is None:
        return "-"
    if isinstance(x, (int, np.integer)):
        return str(int(x))
    try:
        xf = float(x)
    except (TypeError, ValueError):
        return str(x)
    if math.isnan(xf):
        return "nan"
    if math.isinf(xf):
        return "inf" if xf > 0 else "-inf"
    if xf == 0:
        return "0"
    ax = abs(xf)
    if ax >= 1e6 or ax < 1e-4:
        return "%.4e" % xf
    return ("%%.%dg" % nd) % xf


# ---------------------------------------------------------------------------
# OBJ 解析
# ---------------------------------------------------------------------------
class Mesh(object):
    """CSR 形式的多边形网格。

    verts       (N,3) float64
    face_sizes  (F,)   int64      每个面的顶点数
    face_starts (F+1,) int64      CSR 偏移
    face_idx    (M,)   int64      展平的顶点下标
    """

    __slots__ = ("path", "verts", "face_sizes", "face_starts", "face_idx",
                 "n_objects", "n_vn", "n_vt", "warnings", "parse_seconds",
                 "raw_face_lines", "bytes")

    def __init__(self):
        self.path = ""
        self.verts = np.zeros((0, 3), dtype=np.float64)
        self.face_sizes = np.zeros(0, dtype=np.int64)
        self.face_starts = np.zeros(1, dtype=np.int64)
        self.face_idx = np.zeros(0, dtype=np.int64)
        self.n_objects = 0
        self.n_vn = 0
        self.n_vt = 0
        self.warnings = []
        self.parse_seconds = 0.0
        self.raw_face_lines = 0
        self.bytes = 0

    @property
    def n_verts(self):
        return int(self.verts.shape[0])

    @property
    def n_faces(self):
        return int(self.face_sizes.shape[0])

    def triangles(self):
        """多边形按扇形三角化，返回 (T,3) int64 顶点下标。"""
        sizes = self.face_sizes
        if sizes.size == 0:
            return np.zeros((0, 3), dtype=np.int64)
        k2 = np.clip(sizes - 2, 0, None)
        total = int(k2.sum())
        if total == 0:
            return np.zeros((0, 3), dtype=np.int64)
        face_of_tri = np.repeat(np.arange(sizes.size, dtype=np.int64), k2)
        seg = np.repeat(np.cumsum(k2) - k2, k2)
        local_t = np.arange(total, dtype=np.int64) - seg
        fs = self.face_starts[face_of_tri]
        v0 = self.face_idx[fs]
        v1 = self.face_idx[fs + local_t + 1]
        v2 = self.face_idx[fs + local_t + 2]
        return np.stack([v0, v1, v2], axis=1)


def load_obj(path, quiet=False):
    t0 = time.perf_counter()
    m = Mesh()
    m.path = os.path.abspath(path)
    with open(path, "rb") as fh:
        data = fh.read()
    m.bytes = len(data)
    if b"\r" in data:
        data = data.replace(b"\r\n", b"\n").replace(b"\r", b"\n")

    v_parts = []
    f_parts = []
    n_obj = 0
    n_vn = 0
    n_vt = 0
    for ln in data.split(b"\n"):
        if not ln:
            continue
        b0 = ln[0]
        b1 = ln[1:2]
        if b0 == 118:                                  # 'v'
            if b1 == b" " or b1 == b"\t":
                v_parts.append(ln[2:])
            elif b1 == b"n":
                n_vn += 1
            elif b1 == b"t":
                n_vt += 1
        elif b0 == 102 and (b1 == b" " or b1 == b"\t"):   # 'f'
            f_parts.append(ln[2:])
        elif (b0 == 111 or b0 == 103) and (b1 == b" " or b1 == b"\t"):  # 'o' / 'g'
            n_obj += 1
    del data

    # ---- 顶点 ------------------------------------------------------------
    nv = len(v_parts)
    verts = np.zeros((0, 3), dtype=np.float64)
    if nv:
        flat = b" ".join(v_parts)
        vals = np.fromstring(flat, dtype=np.float64, sep=" ")
        if vals.size == 3 * nv:
            verts = vals.reshape(nv, 3)
        elif vals.size == 4 * nv:
            verts = vals.reshape(nv, 4)[:, :3].copy()
        else:
            m.warnings.append("mixed vertex arity; per-line fallback parser used")
            rows = []
            for p in v_parts:
                t = p.split()
                rows.append((float(t[0]), float(t[1]), float(t[2]) if len(t) > 2 else 0.0))
            verts = np.array(rows, dtype=np.float64) if rows else np.zeros((0, 3))
    m.verts = verts

    # ---- 面 --------------------------------------------------------------
    f_sizes = np.zeros(0, dtype=np.int64)
    f_idx = np.zeros(0, dtype=np.int64)
    if f_parts:
        flat_f = b" ".join(f_parts)
        if b"/" in flat_f:
            flat_f = _SLASH_RE.sub(b"", flat_f)
            m.warnings.append("faces contain v/vt/vn references; only the vertex index is used")
        sizes = np.fromiter((len(p.split()) for p in f_parts), dtype=np.int64,
                            count=len(f_parts))
        toks = flat_f.split()
        if len(toks) != int(sizes.sum()):
            raise ValueError("OBJ face token count mismatch (%d vs %d)"
                             % (len(toks), int(sizes.sum())))
        idx = np.array(toks, dtype=np.int64)

        # OBJ 面索引是 1 基；负索引相对当前顶点数（这里按文件内总顶点数解析）
        if idx.size:
            neg = idx < 0
            if neg.any():
                m.warnings.append("negative (relative) face indices resolved against total vertex count")
            idx = idx.copy()
            idx[neg] += m.n_verts
            idx[~neg] -= 1
            if idx.min() < 0:
                raise ValueError("face index out of range (negative after resolution)")

        starts = np.zeros(sizes.size + 1, dtype=np.int64)
        np.cumsum(sizes, out=starts[1:])

        if sizes.size:
            first = idx[starts[:-1]]
            last = idx[starts[1:] - 1]
            dup = first == last
            if dup.any():
                keepm = np.ones(idx.size, dtype=bool)
                keepm[starts[1:][dup] - 1] = False
                idx = idx[keepm]
                sizes = sizes - dup.astype(np.int64)
                m.warnings.append("removed %d duplicated closing vertex indices" % int(dup.sum()))

        ok = sizes >= 3
        if not ok.all():
            n_bad = int((~ok).sum())
            keepm = np.repeat(ok, sizes)
            idx = idx[keepm]
            sizes = sizes[ok]
            m.warnings.append("dropped %d faces with fewer than 3 vertices" % n_bad)

        if idx.size:
            hi = int(idx.max())
            if hi >= m.n_verts:
                raise ValueError("face index %d out of range (vertices=%d)" % (hi, m.n_verts))
        f_sizes, f_idx = sizes, idx

    m.face_sizes = f_sizes
    m.face_idx = f_idx
    starts = np.zeros(f_sizes.size + 1, dtype=np.int64)
    np.cumsum(f_sizes, out=starts[1:])
    m.face_starts = starts
    m.n_objects = n_obj
    m.n_vn = n_vn
    m.n_vt = n_vt
    m.raw_face_lines = len(f_parts)
    m.parse_seconds = time.perf_counter() - t0
    _log("parsed %s: %d verts, %d faces in %.2fs"
         % (os.path.basename(path), m.n_verts, m.n_faces, m.parse_seconds), quiet)
    return m


# ---------------------------------------------------------------------------
# 并查集（向量化标签传播 + 指针跳跃）
# ---------------------------------------------------------------------------
def _compress(parent, iters=16):
    for _ in range(iters):
        p = parent[parent]
        if np.array_equal(p, parent):
            break
        parent = p
    return parent


def union_labels(n, pairs, max_iter=128):
    """n 个元素 + (K,2) 连接对 -> 每个元素的连通分量最小标签。"""
    parent = np.arange(n, dtype=np.int64)
    if pairs is None or len(pairs) == 0 or n == 0:
        return parent
    a = pairs[:, 0]
    b = pairs[:, 1]
    for _ in range(max_iter):
        pa = parent[a]
        pb = parent[b]
        lo = np.minimum(pa, pb)
        hi = np.maximum(pa, pb)
        m = lo != hi
        if not m.any():
            break
        np.minimum.at(parent, hi[m], lo[m])
        parent = _compress(parent)
    return _compress(parent)


# ---------------------------------------------------------------------------
# 网格拓扑 / 几何度量
# ---------------------------------------------------------------------------
def _face_edges(mesh):
    """每个面的每条有向边：(a, b, face_of_edge)。"""
    sizes = mesh.face_sizes
    if sizes.size == 0:
        z = np.zeros(0, dtype=np.int64)
        return z, z, z
    fs_rep = np.repeat(mesh.face_starts[:-1], sizes)
    sz_rep = np.repeat(sizes, sizes)
    local = np.arange(mesh.face_idx.size, dtype=np.int64) - fs_rep
    a = mesh.face_idx
    b = mesh.face_idx[fs_rep + (local + 1) % sz_rep]
    face_of_edge = np.repeat(np.arange(sizes.size, dtype=np.int64), sizes)
    return a, b, face_of_edge


def compute_metrics(mesh, quiet=False):
    """计算单个网格的全部指标，返回 JSON 友好 dict。"""
    t0 = time.perf_counter()
    V = mesh.verts
    sizes = mesh.face_sizes
    F = int(sizes.size)
    nv = int(V.shape[0])
    out = {
        "file": mesh.path,
        "tool": TOOL_NAME,
        "version": TOOL_VERSION,
        "bytes": int(mesh.bytes),
        "parse_seconds": round(mesh.parse_seconds, 3),
        "objects": int(mesh.n_objects),
        "vertices": nv,
        "faces": F,
        "warnings": list(mesh.warnings),
    }

    # ---- 多边形类型 ------------------------------------------------------
    quads = int((sizes == 4).sum())
    tris = int((sizes == 3).sum())
    gt4 = int((sizes > 4).sum())
    out["quads"] = quads
    out["tri_faces"] = tris
    out["ngon_faces"] = gt4
    out["non_quad_faces"] = int(F - quads)
    out["quad_ratio"] = (float(quads) / F) if F else 0.0
    out["quad_ratio_pct"] = 100.0 * out["quad_ratio"]
    if F:
        uk, uc = np.unique(sizes, return_counts=True)
        out["face_size_histogram"] = {str(int(k)): int(c) for k, c in zip(uk, uc)}
    else:
        out["face_size_histogram"] = {}

    # ---- 顶点使用情况 / 包围盒 -------------------------------------------
    used = np.zeros(nv, dtype=bool)
    if mesh.face_idx.size:
        used[np.unique(mesh.face_idx)] = True
    n_used = int(used.sum())
    out["used_vertices"] = n_used
    out["unreferenced_vertices"] = int(nv - n_used)
    if n_used:
        Vu = V[used]
        bmin = Vu.min(axis=0)
        bmax = Vu.max(axis=0)
    else:
        bmin = np.zeros(3)
        bmax = np.zeros(3)
    bsize = bmax - bmin
    diag = float(np.linalg.norm(bsize))
    out["bbox"] = {
        "min": [float(x) for x in bmin],
        "max": [float(x) for x in bmax],
        "size": [float(x) for x in bsize],
        "diagonal": diag,
    }
    diag2 = diag * diag if diag > 0 else 1.0

    # ---- 边 / 面法向 / 面积 ----------------------------------------------
    a, b, face_of_edge = _face_edges(mesh)
    has_edges = a.size > 0
    area_face = np.zeros(F, dtype=np.float64)
    face_norm = np.zeros((F, 3), dtype=np.float64)
    if has_edges:
        cr = np.cross(V[a], V[b])
        nrm = np.add.reduceat(cr, mesh.face_starts[:-1], axis=0)
        nlen = np.linalg.norm(nrm, axis=1)
        area_face = 0.5 * nlen
        nz = nlen > 0
        face_norm[nz] = nrm[nz] / nlen[nz, None]

    total_area = float(area_face.sum())
    out["area_total"] = total_area
    out["area_by_diag2"] = (total_area / diag2) if diag2 > 0 else None

    if has_edges:
        lo = np.minimum(a, b)
        hi = np.maximum(a, b)
        keep = lo != hi
        key = (lo.astype(np.int64) * max(nv, 1) + hi.astype(np.int64))[keep]
        order = np.argsort(key, kind="stable")
        ks = key[order]
        uniq, ustart, ucount = np.unique(ks, return_index=True, return_counts=True)
        fsort = face_of_edge[keep][order]
        u = (uniq // max(nv, 1)).astype(np.int64)
        w = (uniq % max(nv, 1)).astype(np.int64)
        elen = np.linalg.norm(V[u] - V[w], axis=1) if uniq.size else np.zeros(0)

        out["edges_unique"] = int(uniq.size)
        out["edges_boundary"] = int((ucount == 1).sum())
        out["edges_interior"] = int((ucount == 2).sum())
        out["edges_non_manifold"] = int((ucount > 2).sum())
        out["edge_multiplicity_max"] = int(ucount.max()) if ucount.size else 0

        if elen.size:
            out["edge_length"] = {
                "min": float(elen.min()),
                "p05": float(np.percentile(elen, 5)),
                "median": float(np.median(elen)),
                "mean": float(elen.mean()),
                "p95": float(np.percentile(elen, 95)),
                "max": float(elen.max()),
                "std": float(elen.std()),
            }
            out["edge_length_median_by_diag"] = (out["edge_length"]["median"] / diag) if diag > 0 else None
        else:
            out["edge_length"] = {}

        val = (np.bincount(np.concatenate([u, w]), minlength=nv)
               if uniq.size else np.zeros(nv, dtype=np.int64))
        used_val = val[used] if n_used else np.zeros(0, dtype=np.int64)
        vh = {}
        if used_val.size:
            vk, vc = np.unique(used_val, return_counts=True)
            vh = {str(int(k)): int(c) for k, c in zip(vk, vc)}
        out["valence_histogram"] = vh
        out["valence_mean"] = float(used_val.mean()) if used_val.size else None
        out["valence_max"] = int(used_val.max()) if used_val.size else None
        n_v4 = int(vh.get("4", 0))
        out["valence4_ratio"] = (float(n_v4) / n_used) if n_used else 0.0
        out["irregular_vertices"] = int(n_used - n_v4)

        # ---- 法向一致性：相邻面法向夹角 > 90 度 ---------------------------
        bad_pairs = 0
        max_ang = 0.0
        two = ucount == 2
        if two.any():
            f1 = fsort[ustart[two]]
            f2 = fsort[ustart[two] + 1]
            n1 = face_norm[f1]
            n2 = face_norm[f2]
            valid = (np.linalg.norm(n1, axis=1) > 0) & (np.linalg.norm(n2, axis=1) > 0)
            if valid.any():
                dot = np.clip(np.einsum("ij,ij->i", n1[valid], n2[valid]), -1.0, 1.0)
                ang = np.degrees(np.arccos(dot))
                bad_pairs += int((ang > 90.0).sum())
                max_ang = max(max_ang, float(ang.max()))
        nm = ucount > 2
        if nm.any():
            for g in np.nonzero(nm)[0]:
                st = int(ustart[g])
                cnt = int(ucount[g])
                nn = face_norm[fsort[st:st + cnt]]
                gd = np.clip(nn @ nn.T, -1.0, 1.0)
                gd = gd[np.triu_indices(cnt, 1)]
                if gd.size:
                    ang = np.degrees(np.arccos(gd))
                    bad_pairs += int((ang > 90.0).sum())
                    max_ang = max(max_ang, float(ang.max()))
        out["normal_inconsistent_edges"] = int(bad_pairs)
        out["normal_inconsistent_ratio"] = (float(bad_pairs) / out["edges_unique"]
                                            if out["edges_unique"] else 0.0)
        out["max_dihedral_deg"] = float(max_ang)

        # ---- 连通分量：面经共享边相连 ------------------------------------
        multi = ucount >= 2
        if multi.any():
            cnt = ucount[multi]
            st = ustart[multi]
            n_each = cnt - 1
            total_p = int(n_each.sum())
            seg = np.repeat(np.cumsum(n_each) - n_each, n_each)
            member_pos = np.repeat(st + 1, n_each) + (np.arange(total_p, dtype=np.int64) - seg)
            pairs = np.stack([np.repeat(fsort[st], n_each), fsort[member_pos]], axis=1)
        else:
            pairs = np.zeros((0, 2), dtype=np.int64)
        lab = union_labels(F, pairs)
        if F:
            ulab, fcounts = np.unique(lab, return_counts=True)
            out["components"] = int(ulab.size)
            desc = np.sort(fcounts)[::-1]
            out["components_largest_face_ratio"] = float(desc[0]) / float(F)
            out["components_top2_face_ratio"] = float(desc[:2].sum()) / float(F)
            out["components_top3_face_ratio"] = float(desc[:3].sum()) / float(F)
            out["components_face_histogram"] = [int(x) for x in desc[:10]]
            out["components_small_face_count"] = int((desc <= 3).sum())
        else:
            out["components"] = 0
            out["components_largest_face_ratio"] = 0.0
            out["components_top2_face_ratio"] = 0.0
            out["components_top3_face_ratio"] = 0.0
            out["components_face_histogram"] = []
            out["components_small_face_count"] = 0

        # ---- 边界环数 ----------------------------------------------------
        bmask = ucount == 1
        if bmask.any():
            bu, bw = u[bmask], w[bmask]
            blab = union_labels(nv, np.stack([bu, bw], axis=1))
            bverts = np.unique(np.concatenate([bu, bw]))
            out["boundary_loops"] = int(np.unique(blab[bverts]).size)
            out["boundary_vertices"] = int(bverts.size)
        else:
            out["boundary_loops"] = 0
            out["boundary_vertices"] = 0
    else:
        out.update({
            "edges_unique": 0, "edges_boundary": 0, "edges_interior": 0,
            "edges_non_manifold": 0, "edge_multiplicity_max": 0,
            "edge_length": {}, "valence_histogram": {},
            "normal_inconsistent_edges": 0, "components": 0,
            "components_largest_face_ratio": 0.0,
            "components_top2_face_ratio": 0.0, "components_top3_face_ratio": 0.0,
            "components_face_histogram": [], "components_small_face_count": 0,
            "boundary_loops": 0, "boundary_vertices": 0,
            "valence_mean": None, "valence_max": None,
            "valence4_ratio": 0.0, "irregular_vertices": 0,
            "max_dihedral_deg": 0.0, "normal_inconsistent_ratio": 0.0,
        })

    # ---- 退化面 ----------------------------------------------------------
    degen = area_face <= (1e-12 * diag2)
    if has_edges:
        rep = a == b
        if rep.any():
            rep_face = np.zeros(F, dtype=bool)
            rep_face[face_of_edge[rep]] = True
            degen |= rep_face
    out["degenerate_faces"] = int(degen.sum())
    out["degenerate_ratio"] = (float(degen.sum()) / F) if F else 0.0

    out["triangles_after_fan"] = int(mesh.triangles().shape[0])
    out["compute_seconds"] = round(time.perf_counter() - t0, 3)
    return out


# ---------------------------------------------------------------------------
# 均匀网格哈希：点到三角面最近距离
# ---------------------------------------------------------------------------
def _seg_dist(p, a, b):
    ab = b - a
    ap = p - a
    den = np.einsum("ij,ij->i", ab, ab)
    t = np.zeros(p.shape[0], dtype=np.float64)
    ok = den > 1e-30
    t[ok] = np.einsum("ij,ij->i", ap[ok], ab[ok]) / den[ok]
    np.clip(t, 0.0, 1.0, out=t)
    return np.linalg.norm(p - (a + t[:, None] * ab), axis=1)


def _point_tri_dist(p, a, b, c):
    """点到三角形最近距离（平面投影 + 三条边段，向量化）。"""
    ab = b - a
    ac = c - a
    ap = p - a
    n = np.cross(ab, ac)
    nn = np.einsum("ij,ij->i", n, n)

    d_plane = np.full(p.shape[0], np.inf, dtype=np.float64)
    ok = nn > 1e-30
    if ok.any():
        n_ok = n[ok]
        ab_ok = ab[ok]
        ac_ok = ac[ok]
        ap_ok = ap[ok]
        t = np.einsum("ij,ij->i", n_ok, ap_ok) / nn[ok]
        q = ap_ok - t[:, None] * n_ok
        d00 = np.einsum("ij,ij->i", ab_ok, ab_ok)
        d01 = np.einsum("ij,ij->i", ab_ok, ac_ok)
        d11 = np.einsum("ij,ij->i", ac_ok, ac_ok)
        d20 = np.einsum("ij,ij->i", q, ab_ok)
        d21 = np.einsum("ij,ij->i", q, ac_ok)
        denom = d00 * d11 - d01 * d01
        safe = denom > 1e-30
        v = np.zeros_like(denom)
        w = np.zeros_like(denom)
        v[safe] = (d11[safe] * d20[safe] - d01[safe] * d21[safe]) / denom[safe]
        w[safe] = (d00[safe] * d21[safe] - d01[safe] * d20[safe]) / denom[safe]
        inside = safe & (v >= 0) & (w >= 0) & (v + w <= 1)
        d_plane[ok] = np.where(inside, np.abs(t), np.inf)

    dseg = np.minimum(np.minimum(_seg_dist(p, a, b), _seg_dist(p, b, c)),
                      _seg_dist(p, c, a))
    return np.minimum(d_plane, dseg)


class TriGrid(object):
    """均匀网格哈希；三角形按包围盒覆盖的 cell 登记，查询时按 Chebyshev 环扩展。"""

    def __init__(self, verts, tris, cell, quiet=False):
        self.cell = float(cell)
        self.empty = True
        self.keys = np.zeros(0, dtype=np.int64)
        if tris is None or len(tris) == 0:
            return
        self.empty = False
        tv = verts[tris]
        self.a = tv[:, 0]
        self.b = tv[:, 1]
        self.c = tv[:, 2]
        guard = 0
        while True:
            cmin = np.floor(tv.min(axis=1) / self.cell).astype(np.int64)
            cmax = np.floor(tv.max(axis=1) / self.cell).astype(np.int64)
            span = cmax - cmin + 1
            total = int(np.prod(span, axis=1, dtype=np.int64).sum())
            if total <= 40_000_000 or guard >= 8:
                break
            self.cell *= 1.5
            guard += 1
        T = int(tris.shape[0])
        cnt = np.prod(span, axis=1, dtype=np.int64)
        total = int(cnt.sum())
        tri_id = np.repeat(np.arange(T, dtype=np.int64), cnt)
        seg = np.repeat(np.cumsum(cnt) - cnt, cnt)
        lin = np.arange(total, dtype=np.int64) - seg
        sz = span[tri_id, 2]
        sy = span[tri_id, 1]
        iz = lin % sz
        rem = lin // sz
        iy = rem % sy
        ix = rem // sy
        keys = self._pack(cmin[tri_id, 0] + ix, cmin[tri_id, 1] + iy, cmin[tri_id, 2] + iz)
        order = np.argsort(keys, kind="stable")
        ks = keys[order]
        uniq, ustart, ucount = np.unique(ks, return_index=True, return_counts=True)
        self.keys = uniq
        self.ustart = ustart
        self.ucount = ucount
        self.tri_of = tri_id[order]
        self.n_tris = T
        _log("grid: %d tris -> %d cells (cell=%.6g, rescaled %d)"
             % (T, uniq.size, self.cell, guard), quiet)

    @staticmethod
    def _pack(gx, gy, gz):
        off = 1 << 20
        x = (gx + off) & 0x1FFFFF
        y = (gy + off) & 0x1FFFFF
        z = (gz + off) & 0x1FFFFF
        return (x << 42) | (y << 21) | z

    def _ring_offsets(self, r):
        rng = np.arange(-r, r + 1, dtype=np.int64)
        gx, gy, gz = np.meshgrid(rng, rng, rng, indexing="ij")
        offs = np.stack([gx.ravel(), gy.ravel(), gz.ravel()], axis=1)
        if r > 0:
            offs = offs[np.abs(offs).max(axis=1) == r]
        return offs

    def nearest(self, pts, max_ring=8, margin=1, block=20000, quiet=False):
        """每个点到最近三角面的距离；无候选时为 inf。

        首个命中环 r 之后仍继续扫描 r+margin 环，避免大三角形跨 cell 造成的误判。
        """
        P = int(pts.shape[0])
        best = np.full(P, np.inf, dtype=np.float64)
        if self.empty or P == 0:
            return best
        pc = np.floor(pts / self.cell).astype(np.int64)
        hit = np.full(P, -1, dtype=np.int32)
        for r in range(max_ring + 1):
            active = (hit < 0) | (hit >= r - margin)
            if not active.any():
                break
            offs = self._ring_offsets(r)
            C = offs.shape[0]
            blk = max(512, min(block, int(1_500_000 // max(C, 1))))
            for s in range(0, P, blk):
                e = min(P, s + blk)
                loc = np.nonzero(active[s:e])[0]
                if loc.size == 0:
                    continue
                idx = loc + s
                cells = pc[idx][:, None, :] + offs[None, :, :]
                qk = self._pack(cells[:, :, 0].ravel(), cells[:, :, 1].ravel(),
                                cells[:, :, 2].ravel())
                pos = np.searchsorted(self.keys, qk)
                vpos = pos < self.keys.size
                vpos[vpos] = self.keys[pos[vpos]] == qk[vpos]
                if not vpos.any():
                    continue
                pid = np.repeat(idx, C)[vpos]
                tstart = self.ustart[pos[vpos]]
                tcnt = self.ucount[pos[vpos]]
                tot = int(tcnt.sum())
                pidx = np.repeat(pid, tcnt)
                seg = np.repeat(np.cumsum(tcnt) - tcnt, tcnt)
                tid = np.repeat(tstart, tcnt) + (np.arange(tot, dtype=np.int64) - seg)
                tid = self.tri_of[tid]
                d = _point_tri_dist(pts[pidx], self.a[tid], self.b[tid], self.c[tid])
                np.minimum.at(best, pidx, d)
            newly = (hit < 0) & np.isfinite(best)
            if newly.any():
                hit[newly] = r
        return best


# ---------------------------------------------------------------------------
# 参考网格对比
# ---------------------------------------------------------------------------
def _avg_edge_length(mesh):
    a, b, _ = _face_edges(mesh)
    if a.size == 0:
        return 0.0, 0.0
    lo = np.minimum(a, b).astype(np.int64)
    hi = np.maximum(a, b).astype(np.int64)
    sel = lo != hi
    key = np.unique(lo[sel] * max(mesh.n_verts, 1) + hi[sel])
    u = (key // max(mesh.n_verts, 1)).astype(np.int64)
    w = (key % max(mesh.n_verts, 1)).astype(np.int64)
    el = np.linalg.norm(mesh.verts[u] - mesh.verts[w], axis=1)
    if el.size == 0:
        return 0.0, 0.0
    return float(el.mean()), float(np.median(el))


def _sample_indices(n, limit):
    if limit is None or limit <= 0 or limit >= n:
        return np.arange(n, dtype=np.int64), False
    return np.unique(np.linspace(0, n - 1, int(limit)).round().astype(np.int64)), True


def compare_ref(target, ref, samples=None, ref_samples=None, max_ring=8, quiet=False):
    t_all = time.perf_counter()
    res = {"file": ref.path}
    ref_tris = ref.triangles()
    avg_e, med_e = _avg_edge_length(ref)
    if avg_e <= 0:
        avg_e = 1.0
    cell = 2.0 * avg_e
    res["ref_stats"] = {
        "vertices": ref.n_verts,
        "faces": ref.n_faces,
        "triangles": int(ref_tris.shape[0]),
        "avg_edge": avg_e,
        "median_edge": med_e,
        "cell_size": cell,
    }
    _log("ref avg edge=%.6g cell=%.6g" % (avg_e, cell), quiet)

    # ---- 正向：输出顶点 -> 参考表面 --------------------------------------
    t0 = time.perf_counter()
    used = np.zeros(target.n_verts, dtype=bool)
    if target.face_idx.size:
        used[np.unique(target.face_idx)] = True
    pts_idx = np.nonzero(used)[0]
    sub, truncated = _sample_indices(pts_idx.size, samples)
    pts = target.verts[pts_idx[sub]]
    grid = TriGrid(ref.verts, ref_tris, cell, quiet=quiet)
    d = grid.nearest(pts, max_ring=max_ring, quiet=quiet)
    fin = np.isfinite(d)
    dv = d[fin]
    dev_seconds = time.perf_counter() - t0

    if used.any():
        Vu = target.verts[used]
        diag_t = float(np.linalg.norm(Vu.max(axis=0) - Vu.min(axis=0)))
    else:
        diag_t = 1.0
    if dv.size:
        p95 = float(np.percentile(dv, 95))
        res["deviation"] = {
            "samples": int(dv.size),
            "total_points": int(pts_idx.size),
            "subsampled": bool(truncated),
            "unresolved": int(pts.shape[0] - dv.size),
            "mean": float(dv.mean()),
            "p50": float(np.percentile(dv, 50)),
            "p95": p95,
            "max": float(dv.max()),
            "mean_rel": (float(dv.mean()) / diag_t) if diag_t > 0 else None,
            "p95_rel": (p95 / diag_t) if diag_t > 0 else None,
            "max_rel": (float(dv.max()) / diag_t) if diag_t > 0 else None,
        }
    else:
        res["deviation"] = {"samples": 0, "mean": None, "p95": None, "max": None}
    res["deviation_seconds"] = round(dev_seconds, 3)

    # ---- 反向：参考顶点被输出覆盖的比例 ----------------------------------
    t0 = time.perf_counter()
    if ref.n_verts and target.n_faces:
        r_used = np.zeros(ref.n_verts, dtype=bool)
        if ref.face_idx.size:
            r_used[np.unique(ref.face_idx)] = True
        ridx = np.nonzero(r_used)[0]
        rsub, rtrunc = _sample_indices(ridx.size,
                                       ref_samples if ref_samples is not None else samples)
        rpts = ref.verts[ridx[rsub]]
        tgrid = TriGrid(target.verts, target.triangles(), cell, quiet=quiet)
        rd = tgrid.nearest(rpts, max_ring=max_ring, quiet=quiet)
        rfin = np.isfinite(rd)
        rdv = rd[rfin]
        thr = avg_e
        res["coverage"] = {
            "samples": int(rdv.size),
            "total_points": int(ridx.size),
            "subsampled": bool(rtrunc),
            "threshold": float(thr),
            "covered_ratio": (float((rdv <= thr).sum()) / float(rdv.size)) if rdv.size else None,
            "unresolved": int(rpts.shape[0] - rdv.size),
            "ref_dev_mean": float(rdv.mean()) if rdv.size else None,
            "ref_dev_p95": float(np.percentile(rdv, 95)) if rdv.size else None,
            "ref_dev_max": float(rdv.max()) if rdv.size else None,
        }
    else:
        res["coverage"] = {"covered_ratio": None, "note": "empty ref or target"}
    res["coverage_seconds"] = round(time.perf_counter() - t0, 3)
    res["total_seconds"] = round(time.perf_counter() - t_all, 3)
    return res


# ---------------------------------------------------------------------------
# 文本报告
# ---------------------------------------------------------------------------
def report_text(m, refres=None):
    L = []
    p = L.append
    p("Mesh metrics")
    p("============")
    p("")
    p("File: %s" % m["file"])
    p("Parse: %.2fs   Compute: %.2fs" % (m.get("parse_seconds", 0), m.get("compute_seconds", 0)))
    p("")
    p("Elements")
    p("  vertices ................. %d  (used %d, unreferenced %d)"
      % (m["vertices"], m.get("used_vertices", 0), m.get("unreferenced_vertices", 0)))
    p("  faces .................... %d  (fan triangles %d)"
      % (m["faces"], m.get("triangles_after_fan", 0)))
    p("  quads .................... %d" % m["quads"])
    p("  quad ratio ............... %.2f%%" % m["quad_ratio_pct"])
    p("  triangles ................ %d" % m["tri_faces"])
    p("  n-gons (>4) .............. %d" % m["ngon_faces"])
    p("  non-quad faces ........... %d" % m["non_quad_faces"])
    fh = m.get("face_size_histogram") or {}
    if fh:
        p("  face size histogram ...... %s"
          % ", ".join("%s->%d" % (k, v) for k, v in sorted(fh.items(), key=lambda kv: int(kv[0]))))
    p("")
    p("Topology")
    p("  edges .................... %d" % m.get("edges_unique", 0))
    p("  interior edges (2 faces) . %d" % m.get("edges_interior", 0))
    p("  boundary edges (1 face) .. %d" % m.get("edges_boundary", 0))
    p("  non-manifold edges (>2) .. %d  (max multiplicity %d)"
      % (m.get("edges_non_manifold", 0), m.get("edge_multiplicity_max", 0)))
    p("  boundary loops ........... %d  (boundary vertices %d)"
      % (m.get("boundary_loops", 0), m.get("boundary_vertices", 0)))
    p("  connected components ..... %d  (edge-connected; largest %.2f%% of faces, <=3-face shells %d)"
      % (m.get("components", 0), 100.0 * (m.get("components_largest_face_ratio") or 0.0),
         m.get("components_small_face_count", 0)))
    p("  component face share ..... top2 %.2f%%  top3 %.2f%%"
      % (100.0 * (m.get("components_top2_face_ratio") or 0.0),
         100.0 * (m.get("components_top3_face_ratio") or 0.0)))
    ch = m.get("components_face_histogram") or []
    if ch:
        p("  component faces (top10) .. [%s]" % ", ".join(str(x) for x in ch))
    p("  degenerate faces ......... %d  (%.4f%%)"
      % (m.get("degenerate_faces", 0), 100.0 * (m.get("degenerate_ratio") or 0)))
    p("  normal-inconsistent edges  %d  (max dihedral %.1f deg)"
      % (m.get("normal_inconsistent_edges", 0), m.get("max_dihedral_deg") or 0))
    p("")
    p("Vertex valence")
    vh = m.get("valence_histogram") or {}
    if vh:
        p("  mean %.3f   max %s   valence-4 ratio %.2f%%   irregular %s"
          % (m.get("valence_mean") or 0, m.get("valence_max"),
             100.0 * (m.get("valence4_ratio") or 0), m.get("irregular_vertices")))
        line = "  "
        for k, v in sorted(vh.items(), key=lambda kv: int(kv[0])):
            t = "%s:%d  " % (k, v)
            if len(line) + len(t) > 92:
                p(line.rstrip())
                line = "  "
            line += t
        if line.strip():
            p(line.rstrip())
    else:
        p("  (none)")
    p("")
    p("Geometry")
    bb = m["bbox"]
    p("  bbox min ................. [%s]" % ", ".join(_fmt(x) for x in bb["min"]))
    p("  bbox max ................. [%s]" % ", ".join(_fmt(x) for x in bb["max"]))
    p("  bbox size ................ [%s]  diag %s"
      % (", ".join(_fmt(x) for x in bb["size"]), _fmt(bb["diagonal"])))
    p("  surface area ............. %s  (area/diag^2 %s)"
      % (_fmt(m.get("area_total")), _fmt(m.get("area_by_diag2"))))
    el = m.get("edge_length") or {}
    if el:
        p("  edge length min .......... %s" % _fmt(el["min"]))
        p("  edge length median ....... %s" % _fmt(el["median"]))
        p("  edge length mean ......... %s" % _fmt(el["mean"]))
        p("  edge length p95 .......... %s" % _fmt(el["p95"]))
        p("  edge length max .......... %s" % _fmt(el["max"]))
        if el["min"] > 0:
            p("  edge max/min ratio ....... %s" % _fmt(el["max"] / el["min"], 4))
    else:
        p("  edge length .............. (none)")
    if m.get("warnings"):
        p("")
        p("Warnings")
        for w in m["warnings"]:
            p("  - %s" % w)
    if refres:
        p("")
        p("Reference comparison: %s" % refres.get("file"))
        rs = refres.get("ref_stats", {})
        p("  ref geometry ............. %d verts / %d faces / %d tris, avg edge %s"
          % (rs.get("vertices", 0), rs.get("faces", 0), rs.get("triangles", 0),
             _fmt(rs.get("avg_edge"))))
        p("  grid cell size ........... %s (2 x avg edge)" % _fmt(rs.get("cell_size")))
        dv = refres.get("deviation") or {}
        if dv.get("samples"):
            p("  out->ref distance ........ mean %s  p95 %s  max %s   (n=%d%s)"
              % (_fmt(dv.get("mean")), _fmt(dv.get("p95")), _fmt(dv.get("max")),
                 dv["samples"], ", subsampled" if dv.get("subsampled") else ""))
            p("  normalized by target diag  mean %s  p95 %s  max %s"
              % (_fmt(dv.get("mean_rel")), _fmt(dv.get("p95_rel")), _fmt(dv.get("max_rel"))))
            if dv.get("unresolved"):
                p("  unresolved points ........ %d" % dv["unresolved"])
        else:
            p("  out->ref distance ........ n/a")
        cv = refres.get("coverage") or {}
        if cv.get("covered_ratio") is not None:
            p("  ref coverage (thr %s) .... %.2f%%  (n=%d, ref->out mean %s, p95 %s, max %s)"
              % (_fmt(cv.get("threshold")), 100.0 * cv["covered_ratio"], cv.get("samples", 0),
                 _fmt(cv.get("ref_dev_mean")), _fmt(cv.get("ref_dev_p95")),
                 _fmt(cv.get("ref_dev_max"))))
        p("  timing ................... deviation %.2fs, coverage %.2fs, total %.2fs"
          % (refres.get("deviation_seconds", 0), refres.get("coverage_seconds", 0),
             refres.get("total_seconds", 0)))
    return "\n".join(L)


# ---------------------------------------------------------------------------
# compare
# ---------------------------------------------------------------------------
_COMPARE_ROWS = [
    ("vertices", "vertices", 0, None),
    ("faces", "faces", 0, None),
    ("quads", "quads", 0, True),
    ("quad_ratio_pct", "quad ratio (%)", 3, True),
    ("tri_faces", "tri faces", 0, False),
    ("ngon_faces", "n-gons (>4)", 0, False),
    ("edges_non_manifold", "non-manifold edges", 0, False),
    ("edges_boundary", "boundary edges", 0, False),
    ("boundary_loops", "boundary loops", 0, False),
    ("components", "components", 0, None),
    ("components_largest_face_ratio", "largest component ratio", 4, True),
    ("components_top2_face_ratio", "top-2 component ratio", 4, True),
    ("components_top3_face_ratio", "top-3 component ratio", 4, True),
    ("components_small_face_count", "small shells (<=3 faces)", 0, False),
    ("degenerate_faces", "degenerate faces", 0, False),
    ("normal_inconsistent_edges", "normal-inconsistent edges", 0, False),
    ("irregular_vertices", "irregular vertices", 0, False),
    ("valence4_ratio", "valence-4 ratio", 4, True),
    ("area_total", "surface area", 6, None),
    ("edge_length.median", "edge len median", 6, None),
    ("edge_length.p95", "edge len p95", 6, None),
    ("edge_length.max", "edge len max", 6, None),
    ("ref.deviation.mean", "deviation mean", 6, False),
    ("ref.deviation.p95", "deviation p95", 6, False),
    ("ref.deviation.max", "deviation max", 6, False),
    ("ref.coverage.covered_ratio", "ref coverage", 4, True),
    ("ref.total_seconds", "ref compare seconds", 3, False),
    ("total_seconds", "metrics total seconds", 3, False),
]


def _get(d, path):
    cur = d
    for part in path.split("."):
        if not isinstance(cur, dict) or part not in cur:
            return None
        cur = cur[part]
    return cur


def compare_json(base, cand, quad_tol=1.0, dev_tol=0.02, shell_tol=0.02):
    rows = []
    regressions = []
    warnings = []

    def add(lst, metric, msg):
        lst.append({"metric": metric, "message": msg})

    for path, label, nd, better in _COMPARE_ROWS:
        b = _get(base, path)
        c = _get(cand, path)
        if b is None and c is None:
            continue
        delta = None
        if isinstance(b, (int, float)) and isinstance(c, (int, float)):
            delta = c - b
        rows.append({"metric": path, "label": label, "baseline": b, "candidate": c,
                     "delta": delta, "digits": nd, "better": better})

    bq, cq = _get(base, "quad_ratio_pct"), _get(cand, "quad_ratio_pct")
    if bq is not None and cq is not None and (bq - cq) > quad_tol:
        add(regressions, "quad_ratio_pct",
            "quad ratio drop %.2f pp (> %.2f pp): %.2f%% -> %.2f%%"
            % (bq - cq, quad_tol, bq, cq))

    bnm, cnm = _get(base, "edges_non_manifold"), _get(cand, "edges_non_manifold")
    if bnm is not None and cnm is not None and cnm > bnm:
        add(regressions, "edges_non_manifold",
            "non-manifold edges increased: %d -> %d" % (bnm, cnm))

    bp, cp = _get(base, "ref.deviation.p95"), _get(cand, "ref.deviation.p95")
    if bp and cp:
        rel = (cp - bp) / bp
        if rel > dev_tol:
            add(regressions, "ref.deviation.p95",
                "deviation p95 increased %.2f%% (> %.2f%%): %s -> %s"
                % (100 * rel, 100 * dev_tol, _fmt(bp), _fmt(cp)))
    elif bp and cp is None:
        add(warnings, "ref.deviation.p95", "candidate has no deviation p95 (run with --ref)")

    bc, cc = _get(base, "components"), _get(cand, "components")
    if bc is not None and cc is not None and bc != cc:
        if cc > bc:
            add(regressions, "components", "connected components increased: %d -> %d" % (bc, cc))
        else:
            add(warnings, "components",
                "connected components changed (decreased): %d -> %d" % (bc, cc))

    for key, label in (("boundary_loops", "boundary loops"),
                       ("degenerate_faces", "degenerate faces"),
                       ("ngon_faces", "n-gons (>4)"),
                       ("normal_inconsistent_edges", "normal-inconsistent edges")):
        b = _get(base, key)
        c = _get(cand, key)
        if b is not None and c is not None and c > b:
            add(warnings, key, "%s increased: %d -> %d" % (label, b, c))

    blr, clr = _get(base, "components_largest_face_ratio"), _get(cand, "components_largest_face_ratio")
    if blr is not None and clr is not None and (blr - clr) > shell_tol:
        add(warnings, "components_largest_face_ratio",
            "largest component ratio dropped %.2f pp (> %.2f pp): %.4f -> %.4f"
            % (100 * (blr - clr), 100 * shell_tol, blr, clr))

    b3, c3 = _get(base, "components_top3_face_ratio"), _get(cand, "components_top3_face_ratio")
    if b3 is not None and c3 is not None and (b3 - c3) > shell_tol:
        add(warnings, "components_top3_face_ratio",
            "top-3 component ratio dropped %.2f pp (> %.2f pp): %.4f -> %.4f"
            % (100 * (b3 - c3), 100 * shell_tol, b3, c3))

    ba, ca = _get(base, "area_total"), _get(cand, "area_total")
    if ba and ca:
        rel = (ca - ba) / ba
        if abs(rel) > 0.01:
            add(warnings, "area_total",
                "surface area changed %.2f%% (> 1%%): %s -> %s" % (100 * rel, _fmt(ba), _fmt(ca)))

    return {"rows": rows, "regressions": regressions, "warnings": warnings,
            "verdict": "REGRESSION" if regressions else "OK",
            "baseline_file": base.get("file"), "candidate_file": cand.get("file"),
            "thresholds": {"quad_ratio_pp": quad_tol, "deviation_p95_rel": dev_tol,
                           "largest_component_ratio": shell_tol}}


def compare_text(cmp):
    L = []
    p = L.append
    p("Mesh metrics comparison")
    p("=======================")
    p("")
    p("baseline : %s" % cmp.get("baseline_file"))
    p("candidate: %s" % cmp.get("candidate_file"))
    p("")
    w1, w2 = 28, 14
    p("%-*s %*s %*s %*s  %s" % (w1, "metric", w2, "baseline", w2, "candidate", w2, "delta", "flag"))
    p("-" * (w1 + 3 * (w2 + 1) + 8))
    for r in cmp["rows"]:
        b, c, d = r["baseline"], r["candidate"], r["delta"]
        flag = ""
        if r["better"] is True and b is not None and c is not None:
            flag = "better" if c > b else ("worse" if c < b else "")
        elif r["better"] is False and b is not None and c is not None:
            flag = "better" if c < b else ("worse" if c > b else "")
        p("%-*s %*s %*s %*s  %s" % (w1, r["label"], w2, _fmt(b), w2, _fmt(c),
                                    w2, _fmt(d) if d is not None else "-", flag))
    p("")
    p("Verdict: %s" % cmp["verdict"])
    if cmp["regressions"]:
        p("")
        p("Regressions")
        for r in cmp["regressions"]:
            p("  [X] %s" % r["message"])
    if cmp["warnings"]:
        p("")
        p("Warnings")
        for r in cmp["warnings"]:
            p("  [!] %s" % r["message"])
    if not cmp["regressions"] and not cmp["warnings"]:
        p("")
        p("No regression detected.")
    return "\n".join(L)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def _default_json_path(target):
    root, _ = os.path.splitext(target)
    return root + ".metrics.json"


def _write_json(path, obj, quiet=False):
    path = os.path.abspath(path)
    try:
        d = os.path.dirname(path)
        if d:
            os.makedirs(d, exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(obj, fh, ensure_ascii=False, indent=2)
    except OSError as exc:
        sys.stderr.write("cannot write json %s: %s\n" % (path, exc))
        return False
    _log("json written to %s" % path, quiet)
    return True


def cmd_measure(args):
    t0 = time.perf_counter()
    try:
        mesh = load_obj(args.target, quiet=args.quiet)
    except (OSError, ValueError) as exc:
        sys.stderr.write("cannot read mesh %s: %s\n" % (args.target, exc))
        return 2
    if mesh.n_faces == 0:
        sys.stderr.write("no faces parsed from %s (not an OBJ polygon mesh?)\n" % args.target)
        return 2
    metrics = compute_metrics(mesh, quiet=args.quiet)
    refres = None
    if args.ref:
        try:
            ref = load_obj(args.ref, quiet=args.quiet)
        except (OSError, ValueError) as exc:
            sys.stderr.write("cannot read reference mesh %s: %s\n" % (args.ref, exc))
            return 2
        if ref.n_faces == 0:
            sys.stderr.write("no faces parsed from reference %s\n" % args.ref)
            return 2
        refres = compare_ref(mesh, ref, samples=args.samples,
                             ref_samples=args.ref_samples,
                             max_ring=args.max_ring, quiet=args.quiet)
        metrics["ref"] = refres
    metrics["total_seconds"] = round(time.perf_counter() - t0, 3)
    if not args.quiet:
        print(report_text(metrics, refres))
    if args.json:
        out = args.json if isinstance(args.json, str) else _default_json_path(args.target)
        if not _write_json(out, metrics, args.quiet):
            return 2
    return 0


def cmd_compare(args):
    bp = args.baseline or "baseline.json"
    cp = args.candidate or "candidate.json"
    for f in (bp, cp):
        if not os.path.exists(f):
            sys.stderr.write("missing json: %s\n" % f)
            return 2
    with open(bp, "r", encoding="utf-8") as fh:
        base = json.load(fh)
    with open(cp, "r", encoding="utf-8") as fh:
        cand = json.load(fh)
    cmp = compare_json(base, cand, quad_tol=args.quad_tol, dev_tol=args.dev_tol,
                       shell_tol=args.shell_tol)
    if not args.quiet:
        print(compare_text(cmp))
    if args.json:
        out = args.json if isinstance(args.json, str) else "comparison.json"
        if not _write_json(out, cmp, args.quiet):
            return 2
    return 1 if cmp["regressions"] else 0


def build_parser():
    ap = argparse.ArgumentParser(
        prog="mesh_metrics.py",
        description="AutoRemesher 网格质量度量（numpy-only）：拓扑/几何指标 + 参考网格偏差与覆盖率 + 基线对比。",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="examples:\n"
               "  mesh_metrics.py out.obj\n"
               "  mesh_metrics.py out.obj --ref clean.obj --json out.json\n"
               "  mesh_metrics.py out.obj --ref clean.obj --samples 20000\n"
               "  mesh_metrics.py compare baseline.json candidate.json\n"
               "  mesh_metrics.py            # compare ./baseline.json vs ./candidate.json\n")
    sub = ap.add_subparsers(dest="cmd")

    m = sub.add_parser("measure", help="measure a single mesh")
    m.add_argument("target")
    m.add_argument("--ref", default=None, help="reference (input) mesh for deviation/coverage")
    m.add_argument("--json", nargs="?", const=True, default=None,
                   help="write metrics JSON (bare flag -> <target>.metrics.json)")
    m.add_argument("--samples", type=int, default=None,
                   help="max sampled output vertices (default: all)")
    m.add_argument("--ref-samples", type=int, default=None,
                   help="max sampled reference vertices (default: same as --samples)")
    m.add_argument("--max-ring", type=int, default=8, help="grid ring expansion limit (default 8)")
    m.add_argument("--quiet", action="store_true")

    c = sub.add_parser("compare", help="compare two metrics JSON files")
    c.add_argument("baseline", nargs="?", default=None)
    c.add_argument("candidate", nargs="?", default=None)
    c.add_argument("--json", nargs="?", const=True, default=None)
    c.add_argument("--quad-tol", type=float, default=1.0,
                   help="allowed quad-ratio drop in percentage points (default 1.0)")
    c.add_argument("--dev-tol", type=float, default=0.02,
                   help="allowed relative increase of deviation p95 (default 0.02)")
    c.add_argument("--shell-tol", type=float, default=0.02,
                   help="allowed drop of components_largest_face_ratio (default 0.02)")
    c.add_argument("--quiet", action="store_true")
    return ap


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv:
        argv = ["compare"]
    elif argv[0] in ("measure", "compare", "-h", "--help"):
        pass
    elif argv[0].startswith("-"):
        argv = ["compare"] + argv          # 只有选项 -> 默认 compare 子命令
    else:
        argv = ["measure"] + argv          # 第一个是路径 -> measure
    args = build_parser().parse_args(argv)
    if args.cmd == "measure":
        return cmd_measure(args)
    return cmd_compare(args)


if __name__ == "__main__":
    sys.exit(main())
