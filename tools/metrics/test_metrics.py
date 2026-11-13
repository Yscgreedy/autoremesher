#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""mesh_metrics 自检：合成网格上的已知答案（numpy-only，无 pytest 依赖）。

运行:  python tools/metrics/test_metrics.py
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import mesh_metrics as mm  # noqa: E402

_FAILED = []
_PASSED = []


def check(name, cond, detail=""):
    if cond:
        _PASSED.append(name)
        print("PASS  %s" % name)
    else:
        _FAILED.append(name)
        print("FAIL  %s  %s" % (name, detail))


def approx(a, b, tol=1e-9):
    if a is None or b is None:
        return False
    return abs(float(a) - float(b)) <= tol


# ---------------------------------------------------------------------------
# OBJ 写入辅助
# ---------------------------------------------------------------------------
def write_obj(path, verts, faces, extra_header=""):
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("# test mesh\n")
        if extra_header:
            fh.write(extra_header)
        for v in verts:
            fh.write("v %.17g %.17g %.17g\n" % (v[0], v[1], v[2]))
        for f in faces:
            fh.write("f " + " ".join(str(i) for i in f) + "\n")


def cube_obj(path):
    verts = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0),
             (0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)]
    faces = [[1, 4, 3, 2], [5, 6, 7, 8], [1, 2, 6, 5],
             [2, 3, 7, 6], [3, 4, 8, 7], [4, 1, 5, 8]]
    write_obj(path, verts, faces)


def grid_obj(path, n, size=1.0):
    verts = []
    for j in range(n + 1):
        for i in range(n + 1):
            verts.append((size * i / n, size * j / n, 0.0))
    faces = []
    for j in range(n):
        for i in range(n):
            a = j * (n + 1) + i + 1
            b = a + 1
            c = b + (n + 1)
            d = a + (n + 1)
            faces.append([a, b, c, d])
    write_obj(path, verts, faces)


def metrics_of(path):
    return mm.compute_metrics(mm.load_obj(path, quiet=True))


# ---------------------------------------------------------------------------
# 用例
# ---------------------------------------------------------------------------
def test_parse_features(tmp):
    """n 边形 / 负索引 / v//vn / 首尾重复 / 多对象 / 1 基索引。"""
    p = os.path.join(tmp, "parse.obj")
    with open(p, "w", encoding="utf-8") as fh:
        fh.write("o obj1\n")
        fh.write("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n")
        fh.write("vn 0 0 1\n")
        fh.write("f 1//1 2//1 3//1 4//1\n")        # quad with v//vn
        fh.write("f 1 2 3 4 1\n")                   # duplicate closing vertex
        fh.write("g obj2\n")
        fh.write("v 2 0 0\nv 3 0 0\nv 3 1 0\nv 2 1 0\nv 2 2 0\n")
        fh.write("f -5 -4 -3 -2\n")                 # quad, negative -> 4..7
        fh.write("f -5 -4 -3 -2 -1\n")              # pentagon, negative -> 4..8
        fh.write("f 2/1/1 3/1/1 4/1/1\n")           # v/vt/vn style
    m = mm.load_obj(p, quiet=True)
    check("parse: vertex count", m.n_verts == 9, "got %d" % m.n_verts)
    check("parse: face count", m.n_faces == 5, "got %d" % m.n_faces)
    check("parse: quad detected", int((m.face_sizes == 4).sum()) == 3,
          str(m.face_sizes.tolist()))
    check("parse: pentagon detected", int((m.face_sizes == 5).sum()) == 1,
          str(m.face_sizes.tolist()))
    check("parse: objects counted", m.n_objects == 2, str(m.n_objects))
    a, b = int(m.face_starts[2]), int(m.face_starts[3])
    check("parse: negative idx resolved",
          m.face_idx[a:b].tolist() == [4, 5, 6, 7],
          str(m.face_idx[a:b].tolist()))
    a, b = int(m.face_starts[3]), int(m.face_starts[4])
    check("parse: pentagon negative idx",
          m.face_idx[a:b].tolist() == [4, 5, 6, 7, 8],
          str(m.face_idx[a:b].tolist()))
    check("parse: 1-based converted", int(m.face_idx[0]) == 0, str(m.face_idx[:4].tolist()))
    check("parse: closing dup removed", int(m.face_sizes[1]) == 4, str(m.face_sizes.tolist()))
    check("parse: v//vn stripped", int(m.n_vn) == 1, str(m.n_vn))


def test_cube(tmp):
    p = os.path.join(tmp, "cube.obj")
    cube_obj(p)
    m = metrics_of(p)
    check("cube: verts", m["vertices"] == 8, str(m["vertices"]))
    check("cube: faces", m["faces"] == 6, str(m["faces"]))
    check("cube: quad ratio 100", approx(m["quad_ratio_pct"], 100.0), str(m["quad_ratio_pct"]))
    check("cube: closed (no boundary)", m["edges_boundary"] == 0, str(m["edges_boundary"]))
    check("cube: no non-manifold", m["edges_non_manifold"] == 0, str(m["edges_non_manifold"]))
    check("cube: 1 component", m["components"] == 1, str(m["components"]))
    check("cube: 0 boundary loops", m["boundary_loops"] == 0, str(m["boundary_loops"]))
    check("cube: 12 edges", m["edges_unique"] == 12, str(m["edges_unique"]))
    check("cube: area 6", approx(m["area_total"], 6.0, 1e-12), str(m["area_total"]))
    check("cube: valence all 3", m["valence_histogram"] == {"3": 8}, str(m["valence_histogram"]))
    check("cube: no degenerate", m["degenerate_faces"] == 0, str(m["degenerate_faces"]))
    check("cube: normals consistent", m["normal_inconsistent_edges"] == 0,
          "%d (max dihedral %.2f)" % (m["normal_inconsistent_edges"], m["max_dihedral_deg"]))
    check("cube: edge median 1", approx(m["edge_length"]["median"], 1.0), str(m["edge_length"]["median"]))
    check("cube: bbox diag sqrt3", approx(m["bbox"]["diagonal"], 3 ** 0.5, 1e-12),
          str(m["bbox"]["diagonal"]))


def test_grid(tmp):
    n = 4
    p = os.path.join(tmp, "grid.obj")
    grid_obj(p, n)
    m = metrics_of(p)
    check("grid: faces n^2", m["faces"] == n * n, str(m["faces"]))
    check("grid: verts (n+1)^2", m["vertices"] == (n + 1) ** 2, str(m["vertices"]))
    check("grid: edges 2n(n+1)", m["edges_unique"] == 2 * n * (n + 1), str(m["edges_unique"]))
    check("grid: boundary edges 4n", m["edges_boundary"] == 4 * n, str(m["edges_boundary"]))
    check("grid: interior edges 2n(n-1)", m["edges_interior"] == 2 * n * (n - 1),
          str(m["edges_interior"]))
    check("grid: 1 boundary loop", m["boundary_loops"] == 1, str(m["boundary_loops"]))
    check("grid: 1 component", m["components"] == 1, str(m["components"]))
    check("grid: area 1", approx(m["area_total"], 1.0, 1e-12), str(m["area_total"]))
    check("grid: interior valence 4",
          m["valence_histogram"].get("4") == (n - 1) ** 2, str(m["valence_histogram"]))


def test_two_islands(tmp):
    p = os.path.join(tmp, "islands.obj")
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (5, 0, 0), (6, 0, 0), (5, 1, 0)]
    faces = [[1, 2, 3], [4, 5, 6]]
    write_obj(p, verts, faces)
    m = metrics_of(p)
    check("islands: 2 components", m["components"] == 2, str(m["components"]))
    check("islands: 2 boundary loops", m["boundary_loops"] == 2, str(m["boundary_loops"]))
    check("islands: 6 boundary edges", m["edges_boundary"] == 6, str(m["edges_boundary"]))
    check("islands: 0 interior edges", m["edges_interior"] == 0, str(m["edges_interior"]))


def test_nonmanifold(tmp):
    p = os.path.join(tmp, "nonmanifold.obj")
    verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1)]
    faces = [[1, 2, 3], [2, 1, 4], [1, 2, 5]]
    write_obj(p, verts, faces)
    m = metrics_of(p)
    check("nonmanifold: 1 bad edge", m["edges_non_manifold"] == 1, str(m["edges_non_manifold"]))
    check("nonmanifold: multiplicity 3", m["edge_multiplicity_max"] == 3, str(m["edge_multiplicity_max"]))
    check("nonmanifold: 1 component", m["components"] == 1, str(m["components"]))


def test_degenerate(tmp):
    p = os.path.join(tmp, "degen.obj")
    verts = [(0, 0, 0), (1, 0, 0), (2, 0, 0), (0, 1, 0)]
    faces = [[1, 2, 3], [1, 2, 2, 4]]   # collinear + repeated vertex
    write_obj(p, verts, faces)
    m = metrics_of(p)
    check("degen: 2 degenerate faces", m["degenerate_faces"] == 2, str(m["degenerate_faces"]))


def test_normal_flip(tmp):
    p = os.path.join(tmp, "flip.obj")
    verts = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
    faces = [[1, 2, 3], [1, 3, 4], [1, 3, 2]]   # 第三个面与第二个反向
    write_obj(p, verts, faces)
    m = metrics_of(p)
    check("flip: detects reversed face", m["normal_inconsistent_edges"] >= 1,
          str(m["normal_inconsistent_edges"]))
    check("flip: max dihedral 180", approx(m["max_dihedral_deg"], 180.0, 1e-6),
          str(m["max_dihedral_deg"]))


def test_deviation_identical(tmp):
    p = os.path.join(tmp, "same.obj")
    cube_obj(p)
    a = mm.load_obj(p, quiet=True)
    b = mm.load_obj(p, quiet=True)
    r = mm.compare_ref(a, b, quiet=True)
    check("deviation: identical mesh mean ~ 0", r["deviation"]["mean"] < 1e-12,
          str(r["deviation"]["mean"]))
    check("deviation: identical mesh max ~ 0", r["deviation"]["max"] < 1e-12,
          str(r["deviation"]["max"]))
    check("coverage: identical mesh 100%", approx(r["coverage"]["covered_ratio"], 1.0),
          str(r["coverage"]["covered_ratio"]))


def test_deviation_offset(tmp):
    """平面整体沿 z 平移 h，点到参考距离应精确等于 h。"""
    p0 = os.path.join(tmp, "plane0.obj")
    p1 = os.path.join(tmp, "plane1.obj")
    grid_obj(p0, 8, size=1.0)
    h = 0.002
    verts = []
    for j in range(9):
        for i in range(9):
            verts.append((i / 8.0, j / 8.0, h))
    faces = []
    for j in range(8):
        for i in range(8):
            a = j * 9 + i + 1
            faces.append([a, a + 1, a + 1 + 9, a + 9])
    write_obj(p1, verts, faces)
    t = mm.load_obj(p1, quiet=True)
    r = mm.load_obj(p0, quiet=True)
    res = mm.compare_ref(t, r, quiet=True, max_ring=12)
    dev = res["deviation"]
    check("deviation: offset mean == h", approx(dev["mean"], h, 1e-9), str(dev["mean"]))
    check("deviation: offset max == h", approx(dev["max"], h, 1e-9), str(dev["max"]))
    check("coverage: small offset fully covered",
          approx(res["coverage"]["covered_ratio"], 1.0), str(res["coverage"]["covered_ratio"]))


def test_compare_modes(tmp):
    p = os.path.join(tmp, "m.obj")
    cube_obj(p)
    base = metrics_of(p)
    cand = json.loads(json.dumps(base))
    cand["quad_ratio_pct"] = base["quad_ratio_pct"] - 5.0
    cand["edges_non_manifold"] = 3
    cand["components"] = 2
    cmp = mm.compare_json(base, cand)
    check("compare: verdict REGRESSION", cmp["verdict"] == "REGRESSION", cmp["verdict"])
    keys = {r["metric"] for r in cmp["regressions"]}
    check("compare: quad drop flagged", "quad_ratio_pct" in keys, str(keys))
    check("compare: non-manifold flagged", "edges_non_manifold" in keys, str(keys))
    check("compare: components flagged", "components" in keys, str(keys))

    cmp2 = mm.compare_json(base, json.loads(json.dumps(base)))
    check("compare: identical -> OK", cmp2["verdict"] == "OK", cmp2["verdict"])

    small = json.loads(json.dumps(base))
    small["quad_ratio_pct"] = base["quad_ratio_pct"] - 0.5
    check("compare: quad drop within tolerance",
          mm.compare_json(base, small)["verdict"] == "OK")


def test_cli(tmp):
    p = os.path.join(tmp, "cli.obj")
    cube_obj(p)
    out = os.path.join(tmp, "out", "cli.json")
    rc = mm.main(["measure", p, "--json", out, "--quiet"])
    check("cli: measure rc", rc == 0, str(rc))
    check("cli: json exists", os.path.exists(out), out)
    with open(out, "r", encoding="utf-8") as fh:
        d = json.load(fh)
    check("cli: json has faces", d.get("faces") == 6, str(d.get("faces")))

    base = os.path.join(tmp, "base.json")
    cand = os.path.join(tmp, "cand.json")
    d2 = dict(d)
    d2["quad_ratio_pct"] = d["quad_ratio_pct"] - 10.0
    with open(base, "w", encoding="utf-8") as fh:
        json.dump(d, fh)
    with open(cand, "w", encoding="utf-8") as fh:
        json.dump(d2, fh)
    rc = mm.main(["compare", base, cand, "--quiet"])
    check("cli: compare returns 1 on regression", rc == 1, str(rc))

    # 无参模式（当前目录有 baseline.json / candidate.json）
    old = os.getcwd()
    try:
        os.chdir(tmp)
        os.replace(base, os.path.join(tmp, "baseline.json"))
        os.replace(cand, os.path.join(tmp, "candidate.json"))
        rc = mm.main(["--quiet"])
        check("cli: bare invocation compares default files", rc == 1, str(rc))
    finally:
        os.chdir(old)


def main():
    tmpdir = tempfile.mkdtemp(prefix="mesh_metrics_test_")
    tests = [test_parse_features, test_cube, test_grid, test_two_islands,
             test_nonmanifold, test_degenerate, test_normal_flip,
             test_deviation_identical, test_deviation_offset,
             test_compare_modes, test_cli]
    for t in tests:
        try:
            t(tmpdir)
        except Exception as exc:  # noqa: BLE001
            check(t.__name__, False, "exception: %r" % (exc,))
    print("")
    print("passed %d / %d" % (len(_PASSED), len(_PASSED) + len(_FAILED)))
    if _FAILED:
        print("failed: %s" % ", ".join(_FAILED))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
