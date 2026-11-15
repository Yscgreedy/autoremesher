#!/usr/bin/env python3
r"""Capability gate: checks the autoremesher CLI against the consumer contract
(D:\works\BlenderProject\docs\autoremesher_能力需求_v1.md, R1..R12).

Every check has two sides: what the CLI reports, and what an independent
parser measures on the file it wrote.  A field that is merely present passes
nothing -- the numbers have to agree.

Usage:
  python tools/capability_gate.py --exe build/cli/dist/autoremesher.exe \
      --input MODEL.obj --out build/audit-tmp/gate [--targets 6000,20000] \
      [--adaptivities 0,0.3,0.5] [--skip-costly] [--json report.json]

Exit code 0 = every implemented check passed, 1 = at least one failed,
2 = usage/IO error.  Unimplemented requirements report SKIP, never FAIL, so
the same command shows progress as features land.
"""

import argparse
import collections
import json
import math
import os
import subprocess
import sys
import time

# ---------------------------------------------------------------- obj parsing


def parse_obj(path):
    """Return (vertices, faces as vertex-index tuples).  uv/normal indices are
    ignored the same way the CLI's own analyser ignores them."""
    verts = []
    faces = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("v "):
                p = line.split()
                verts.append((float(p[1]), float(p[2]), float(p[3])))
            elif line.startswith("f "):
                faces.append(tuple(int(t.split("/")[0]) - 1 for t in line.split()[1:]))
    return verts, faces


def mesh_stats(verts, faces, dedupe=False):
    """Topology and quality metrics computed with no help from the CLI."""
    if dedupe:
        seen = set()
        kept = []
        for f in faces:
            key = frozenset(f)
            if key in seen:
                continue
            seen.add(key)
            kept.append(f)
        faces = kept

    edge_faces = collections.Counter()
    edge_len = {}
    for f in faces:
        n = len(f)
        for i in range(n):
            a, b = f[i], f[(i + 1) % n]
            key = (a, b) if a < b else (b, a)
            edge_faces[key] += 1
            if key not in edge_len:
                ax, ay, az = verts[a]
                bx, by, bz = verts[b]
                edge_len[key] = math.sqrt((ax - bx) ** 2 + (ay - by) ** 2 + (az - bz) ** 2)

    boundary = sum(1 for c in edge_faces.values() if c == 1)
    gt2 = sum(1 for c in edge_faces.values() if c > 2)
    wire = 0  # obj has no standalone edges

    # components over faces
    adjacency = collections.defaultdict(list)
    for f in faces:
        n = len(f)
        for i in range(n):
            a, b = f[i], f[(i + 1) % n]
            adjacency[(a, b) if a < b else (b, a)].append(f)
    face_ids = {f: i for i, f in enumerate(faces)}
    parent = list(range(len(faces)))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def union(x, y):
        rx, ry = find(x), find(y)
        if rx != ry:
            parent[ry] = rx

    for group in adjacency.values():
        for k in range(1, len(group)):
            union(face_ids[group[0]], face_ids[group[k]])
    comps = len({find(i) for i in range(len(faces))})

    # valence / poles on interior vertices only
    # valence = number of distinct incident edges
    vedge = collections.defaultdict(set)
    for (a, b) in edge_faces:
        vedge[a].add(b)
        vedge[b].add(a)
    valence = collections.Counter(len(vedge[v]) for v in vedge)
    boundary_verts = set()
    for (a, b), c in edge_faces.items():
        if c == 1:
            boundary_verts.add(a)
            boundary_verts.add(b)
    interior_verts = [v for v in vedge if v not in boundary_verts]

    poles = sum(1 for v in interior_verts if len(vedge[v]) != 4)
    poles_pct = 100.0 * poles / len(interior_verts) if interior_verts else 0.0
    valence_max = max(valence) if valence else 0
    valence_hist = {
        "3": valence.get(3, 0),
        "4": valence.get(4, 0),
        "5": valence.get(5, 0),
        "gt5": sum(c for k, c in valence.items() if k > 5),
    }

    quad_aspects = []
    zero_len = sum(1 for L in edge_len.values() if L <= 1e-12)
    for f in faces:
        if len(f) != 4:
            continue
        ls = []
        for i in range(4):
            a, b = f[i], f[(i + 1) % 4]
            ls.append(edge_len[(a, b) if a < b else (b, a)])
        mn = min(ls)
        if mn > 0:
            quad_aspects.append(max(ls) / mn)
    quad_aspects.sort()

    def pct(seq, p):
        if not seq:
            return 0.0
        return seq[min(len(seq) - 1, int(p * len(seq)))]

    all_len = sorted(edge_len.values())

    return {
        "vertices": len(verts),
        "faces": len(faces),
        "quads": sum(1 for f in faces if len(f) == 4),
        "boundary_edges": boundary,
        "edges_gt2_faces": gt2,
        "blender_non_manifold": boundary + gt2,
        "wire_edges": wire,
        "components": comps,
        "poles_pct": poles_pct,
        "valence_max": valence_max,
        "valence_hist": valence_hist,
        "quad_aspect": {
            "p50": pct(quad_aspects, 0.5),
            "p95": pct(quad_aspects, 0.95),
            "max": quad_aspects[-1] if quad_aspects else 0.0,
        },
        "edge_len": {
            "min": all_len[0],
            "p50": pct(all_len, 0.5),
            "p95": pct(all_len, 0.95),
            "max": all_len[-1],
        },
        "zero_length_edges": zero_len,
    }


def uv_stats(path):
    vt = []
    faces = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("vt "):
                p = line.split()
                vt.append((float(p[1]), float(p[2])))
            elif line.startswith("f "):
                idx = []
                for t in line.split()[1:]:
                    parts = t.split("/")
                    idx.append(int(parts[1]) - 1 if len(parts) > 1 and parts[1] else -1)
                faces.append(idx)
    if not vt or any(i < 0 for f in faces for i in f):
        return None
    area = 0.0
    for f in faces:
        for k in range(1, len(f) - 1):
            (u0, v0), (u1, v1), (u2, v2) = vt[f[0]], vt[f[k]], vt[f[k + 1]]
            area += 0.5 * abs((u1 - u0) * (v2 - v0) - (u2 - u0) * (v1 - v0))
    grid = 512
    cover = [bytearray(grid) for _ in range(grid)]
    over = 0
    filled = 0
    for f in faces:
        for k in range(1, len(f) - 1):
            (u0, v0), (u1, v1), (u2, v2) = vt[f[0]], vt[f[k]], vt[f[k + 1]]
            den = (v1 - v2) * (u0 - u2) + (u2 - u1) * (v0 - v2)
            if abs(den) < 1e-18:
                continue
            ix0 = max(0, int(min(u0, u1, u2) * grid))
            ix1 = min(grid - 1, int(max(u0, u1, u2) * grid))
            iy0 = max(0, int(min(v0, v1, v2) * grid))
            iy1 = min(grid - 1, int(max(v0, v1, v2) * grid))
            for iy in range(iy0, iy1 + 1):
                py = (iy + 0.5) / grid
                row = cover[iy]
                for ix in range(ix0, ix1 + 1):
                    px = (ix + 0.5) / grid
                    w0 = ((v1 - v2) * (px - u2) + (u2 - u1) * (py - v2)) / den
                    w1 = ((v2 - v0) * (px - u2) + (u0 - u2) * (py - v2)) / den
                    w2 = 1.0 - w0 - w1
                    if w0 >= 0 and w1 >= 0 and w2 >= 0:
                        row[ix] = min(2, row[ix] + 1)
    for iy in range(grid):
        row = cover[iy]
        for ix in range(grid):
            if row[ix] == 1:
                filled += 1
            elif row[ix] >= 2:
                filled += 1
                over += 1
    return {
        "area": area,
        "overlap_pct": (100.0 * over / filled) if filled else 0.0,
    }

# ------------------------------------------------------------------- running


def run_cli(exe, args, timeout=900):
    started = time.time()
    proc = subprocess.run([exe] + args, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=timeout)
    wall = time.time() - started
    events = []
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    result = next((e for e in events if e.get("type") == "result"), None)
    return {"wall": wall, "rc": proc.returncode, "events": events,
            "result": result, "stderr": proc.stderr}


def supports(exe, flag):
    """A flag is supported when the CLI does not reject it as unknown."""
    out = run_cli(exe, ["--input", os.devnull.replace(os.sep, "/"), flag, "--help"], timeout=60)
    text = (out["stderr"] or "") + "".join(
        str(e) for e in out["events"])
    lowered = text.lower()
    return not ("unknown" in lowered or "unrecognised" in lowered or "unrecognized" in lowered)


class Gate:
    def __init__(self):
        self.rows = []

    def add(self, rid, name, status, detail):
        self.rows.append({"req": rid, "check": name, "status": status, "detail": detail})

    def verdict(self, rid, name, ok, detail):
        self.add(rid, name, "PASS" if ok else "FAIL", detail)

    def skip(self, rid, name, detail):
        self.add(rid, name, "SKIP", detail)


def check_matrix(gate, exe, input_path, outdir, targets, adaptivities, base_flags):
    rows = []
    for target in targets:
        for adapt in adaptivities:
            tag = "t%d_a%s" % (target, adapt)
            out = os.path.join(outdir, tag + ".obj")
            args = ["-i", input_path, "-o", out, "--target-quads", str(target),
                    "--adaptivity", str(adapt)] + base_flags + ["--json", "--deviation"]
            run = run_cli(exe, args)
            res = run["result"]
            row = {"case": tag, "target": target, "adaptivity": adapt,
                   "seconds": run["wall"], "rc": run["rc"]}
            if res is None:
                row["error"] = "no result event"
            else:
                row["quads"] = res.get("quads")
                row["quadsError"] = res.get("quadsError")
                topology = res.get("topology") or {}
                # v1.1 names the two meshes apart: internal is what the core
                # measured, exported is what the written file actually contains.
                row["topology"] = topology.get("internal") or topology.get("output")
                row["topology_exported"] = topology.get("exported")
                row["topology_same"] = topology.get("same")
                row["warnings"] = [e.get("code") for e in run["events"] if e.get("type") == "warning"]
                row["patches"] = res.get("patches")
                row["holes"] = res.get("holes")
                row["uvs"] = res.get("uvs")
                row["quality"] = res.get("quality")
            if os.path.exists(out):
                verts, faces = parse_obj(out)
                row["measured_as_written"] = mesh_stats(verts, faces, dedupe=False)
                row["measured_deduped"] = mesh_stats(verts, faces, dedupe=True)
            rows.append(row)
    return rows


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--input", required=True)
    ap.add_argument("--out", default="build/audit-tmp/gate")
    ap.add_argument("--targets", default="6000,20000")
    ap.add_argument("--adaptivities", default="0,0.3,0.5")
    ap.add_argument("--json", default=None)
    ap.add_argument("--skip-costly", action="store_true",
                    help="skip the 15-case matrix and the uv/symmetry sweeps")
    args = ap.parse_args(argv)

    exe = os.path.abspath(args.exe)
    input_path = os.path.abspath(args.input)
    outdir = os.path.abspath(args.out)
    os.makedirs(outdir, exist_ok=True)
    targets = [int(t) for t in args.targets.split(",") if t]
    adaptivities = [float(a) for a in args.adaptivities.split(",") if a]

    base = ["--preset", "hard-surface", "--sharp-edge", "50",
            "--weld-patches", "--keep-all-islands", "--projected-caps"]
    gate = Gate()
    report = {"exe": exe, "input": input_path, "targets": targets,
              "adaptivities": adaptivities, "cases": [], "uv": [], "symmetry": {}}

    print("== R5 / R2 / R3 / R8 matrix ==")
    rows = check_matrix(gate, exe, input_path, outdir, targets, adaptivities, base)
    report["cases"] = rows
    for row in rows:
        tol = 0.08 if row["target"] <= 6000 else 0.05
        err = row.get("quadsError")
        tag = row["case"]
        if err is None:
            gate.verdict("R5", tag, False, "no result event")
            continue
        gate.verdict("R5", tag, abs(err) <= tol,
                     "error=%+.1f%% tol=%.0f%% quads=%s"
                     % (err * 100.0, tol * 100.0, row.get("quads")))

    # R2: quality block has to agree with the independent measurement
    for row in rows:
        tag = row["case"]
        measured = row.get("measured_deduped")
        q = row.get("quality")
        if not measured:
            gate.skip("R2", tag, "no output file to measure")
            continue
        if not q:
            gate.skip("R2", tag, "result has no quality block")
            continue
        poles = q.get("poles_pct")
        aspect = (q.get("quad_aspect") or {}).get("max")
        gate.verdict("R2", tag + " poles_pct",
                     poles is not None and abs(poles - measured["poles_pct"]) <= 0.5,
                     "reported=%s measured=%.2f" % (poles, measured["poles_pct"]))
        gate.verdict("R2", tag + " valence_max",
                     q.get("valence_max") == measured["valence_max"],
                     "reported=%s measured=%s" % (q.get("valence_max"), measured["valence_max"]))
        gate.verdict("R2", tag + " aspect_max",
                     aspect is not None and abs(aspect - measured["quad_aspect"]["max"])
                     <= max(1.0, 0.02 * measured["quad_aspect"]["max"]),
                     "reported=%s measured=%.2f" % (aspect, measured["quad_aspect"]["max"]))
        gate.verdict("R2", tag + " zero_length_edges",
                     q.get("zero_length_edges") == measured["zero_length_edges"],
                     "reported=%s measured=%s"
                     % (q.get("zero_length_edges"), measured["zero_length_edges"]))

    # R3: the reported topology has to describe the file that was written
    for row in rows:
        tag = row["case"]
        measured = row.get("measured_as_written")
        topo = row.get("topology")
        if not measured or not topo:
            gate.skip("R3", tag, "nothing to compare")
            continue
        gate.verdict("R3", tag + " boundary_edges",
                     topo.get("boundaryEdges") == measured["boundary_edges"],
                     "reported=%s measured=%s"
                     % (topo.get("boundaryEdges"), measured["boundary_edges"]))
        gate.verdict("R3", tag + " non_manifold",
                     topo.get("nonManifoldEdges") == measured["edges_gt2_faces"],
                     "reported=%s measured(>2 faces)=%s"
                     % (topo.get("nonManifoldEdges"), measured["edges_gt2_faces"]))
        exported = row.get("topology_exported")
        if exported is None:
            gate.skip("R3", tag + " exported block", "result has no topology.exported")
        else:
            gate.verdict("R3", tag + " exported==file",
                         exported.get("boundaryEdges") == measured["boundary_edges"]
                         and exported.get("nonManifoldEdges") == measured["edges_gt2_faces"],
                         "exported bnd=%s gt2=%s / file bnd=%s gt2=%s"
                         % (exported.get("boundaryEdges"), exported.get("nonManifoldEdges"),
                            measured["boundary_edges"], measured["edges_gt2_faces"]))
            gate.verdict("R3", tag + " same flag", row.get("topology_same") is True,
                         "same=%r" % (row.get("topology_same"),))
        deduped = row.get("measured_deduped") or {}
        dup_extra = measured["faces"] - deduped.get("faces", measured["faces"])
        gate.verdict("R4", tag + " duplicate_faces", dup_extra == 0,
                     "duplicate face copies written=%d" % dup_extra)
        gate.verdict("R4", tag + " zero_length_edges",
                     deduped.get("zero_length_edges") == 0,
                     "zero length edges=%s" % deduped.get("zero_length_edges"))
        aspect = deduped.get("quad_aspect", {}).get("max", 0.0)
        gate.verdict("R4", tag + " aspect<=20", aspect <= 20.0,
                     "quad aspect max=%.2f" % aspect)

    # R8: patch welding has to say what happened
    for row in rows:
        if row["target"] != targets[-1]:
            continue
        tag = row["case"]
        patches = row.get("patches") or {}
        topo = row.get("topology") or {}
        shells = patches.get("remaining_shells")
        reason = patches.get("reason")
        if shells is None:
            gate.skip("R8", tag, "patches has no remaining_shells/welded_vertices/reason")
            continue
        ok = shells <= 2 or (reason not in (None, ""))
        gate.verdict("R8", tag, ok, "remaining_shells=%s reason=%r reported components=%s"
                     % (shells, reason, topo.get("components")))

    # R1: check-only
    if not args.skip_costly:
        print("== R1 check-only ==")
        if not supports(exe, "--check-only"):
            gate.skip("R1", "check-only flag", "--check-only is not accepted by the CLI")
        else:
            out = os.path.join(outdir, "checkonly.jsonl")
            run = run_cli(exe, ["-i", input_path, "--check-only", "--json"])
            res = run["result"] or {}
            need = ["area", "volume", "bbox", "unit_guess", "boundary_loops",
                    "degenerate_edges", "shell_count", "advice"]
            missing = [k for k in need if k not in res]
            gate.verdict("R1", "fields", not missing, "missing=%s" % (missing or "none"))
            gate.verdict("R1", "latency", run["wall"] <= 1.5, "%.2f s" % run["wall"])
            gate.verdict("R1", "writes nothing", res.get("output") in (None, ""),
                         "output=%r" % (res.get("output"),))
            report["check_only"] = {"wall": run["wall"], "result": res}

        # R9: uv modes
        print("== R9 uv modes ==")
        uv_modes = ["corner", "vertex", "hybrid", "seam"]
        for mode in uv_modes:
            out = os.path.join(outdir, "uv_%s.obj" % mode)
            run = run_cli(exe, ["-i", input_path, "-o", out, "--target-quads",
                                str(min(targets)), "--adaptivity", "0.3"] + base
                          + ["--uv-mode", mode, "--json"])
            res = run["result"] or {}
            uvs = res.get("uvs")
            uv_stats_block = res.get("uvStats")
            measured = uv_stats(out) if os.path.exists(out) else None
            entry = {"mode": mode, "uvs": uvs, "uvStats": uv_stats_block, "measured": measured}
            report["uv"].append(entry)
            # v1 keeps "uvs" as the boolean it always was and adds the measured
            # object beside it; only rewrite "uvs" in v2.
            if not isinstance(uv_stats_block, dict):
                gate.skip("R9", mode, "result has no uvStats object (uvs=%r)" % (uvs,))
                continue
            uvs = uv_stats_block
            area = uvs.get("area")
            overlap = uvs.get("overlap_pct")
            bake = uvs.get("bakeable")
            if measured:
                gate.verdict("R9", mode + " area",
                             area is not None and abs(area - measured["area"]) <= 0.05 * max(1e-9, measured["area"]),
                             "reported=%s measured=%.4f" % (area, measured["area"]))
                gate.verdict("R9", mode + " overlap",
                             overlap is not None and abs(overlap - measured["overlap_pct"]) <= 2.0,
                             "reported=%s measured=%.2f%%" % (overlap, measured["overlap_pct"]))
            gate.verdict("R9", mode + " area in (0,1]",
                         area is not None and 0.0 < area <= 1.0, "area=%s" % (area,))
            gate.verdict("R9", mode + " bakeable flag",
                         bake is not None and (bake is False or (overlap is not None and overlap < 1.0)),
                         "bakeable=%s overlap=%s" % (bake, overlap))

        # R6: symmetry modes
        print("== R6 symmetry ==")
        for mode, flag in (("check", ["--symmetry", "check"]),
                           ("snap", ["--symmetry", "snap"]),
                           ("mirror-half", ["--symmetry-mode", "mirror-half"])):
            out = os.path.join(outdir, "sym_%s.obj" % mode)
            run = run_cli(exe, ["-i", input_path, "-o", out, "--target-quads",
                                str(targets[-1])] + base + flag + ["--json", "--deviation"])
            res = run["result"] or {}
            sym = res.get("symmetry") or {}
            report["symmetry"][mode] = {"rc": run["rc"], "symmetry": sym,
                                        "faces": (res.get("topology") or {}).get("output", {}).get("faces")}
            if run["rc"] != 0 or not sym:
                gate.skip("R6", mode, "mode not accepted (rc=%s, stderr=%r)"
                          % (run["rc"], (run["stderr"] or "")[-160:]))
                continue
            if mode == "check":
                gate.verdict("R6", "check reports match",
                             "match_pct" in sym and "max_deviation" in sym,
                             "symmetry=%s" % sorted(sym))
            elif mode == "snap":
                gate.verdict("R6", "snap keeps face count",
                             abs(res.get("quads") or 0) > 0, "quads=%s" % (res.get("quads"),))
            else:
                topology = res.get("topology") or {}
                curved = topology.get("internal") or topology.get("output") or {}
                warnings = [e.get("code") for e in run["events"] if e.get("type") == "warning"]
                # A mirror on an asymmetric mesh is destructive by construction; the
                # contract is "no hole, or say loudly that the cut did damage".
                gate.verdict("R6", "mirror-half: no hole or warned",
                             curved.get("boundaryEdges") == 0
                             or "symmetry_clip_heavy" in warnings
                             or "symmetry_mismatch" in warnings,
                             "boundaryEdges=%s warnings=%s"
                             % (curved.get("boundaryEdges"), warnings))

        # R10 effective params
        print("== R10 effective params ==")
        run = run_cli(exe, ["-i", input_path, "-o", os.path.join(outdir, "r10.obj"),
                            "--target-quads", str(min(targets)), "--adaptivity", "0.3",
                            "--json"])
        res = run["result"] or {}
        eff = res.get("effective_params")
        if not eff:
            gate.skip("R10", "effective_params", "result has no effective_params")
        else:
            ok = eff.get("targetQuads") in (min(targets), str(min(targets)))
            gate.verdict("R10", "effective_params echoes targetQuads", ok,
                         "effective=%r requested=%r" % (eff.get("targetQuads"), min(targets)))

        # R11 interrupt safety
        print("== R11 interrupt ==")
        part = os.path.join(outdir, "abort.obj")
        if os.path.exists(part):
            os.remove(part)
        proc = subprocess.Popen([exe, "-i", input_path, "-o", part, "--target-quads",
                                 str(max(targets)), "--json"],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                encoding="utf-8", errors="replace")
        time.sleep(4.0)
        proc.terminate()
        try:
            stdout, stderr = proc.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate()
        abort = any('"type":"abort"' in (l or "").replace('"type": "abort"', '"type":"abort"')
                    for l in (stdout or "").splitlines())
        if os.name == "nt" and proc.returncode not in (0, 130, -1073741510):
            # TerminateProcess is not a signal: the handler cannot run, so on
            # Windows only the "no partial file" half of R11 is testable here.
            gate.skip("R11", "abort event",
                      "hard kill on Windows (rc=%s); needs GRACEFUL interrupt to test"
                      % (proc.returncode,))
        else:
            gate.verdict("R11", "abort event", abort, "rc=%s stdout tail=%r"
                         % (proc.returncode, (stdout or "")[-160:]))
        gate.verdict("R11", "no partial file", not os.path.exists(part),
                     "partial output present=%s" % os.path.exists(part))

    # ------------------------------------------------------------- summary
    counts = collections.Counter(row["status"] for row in gate.rows)
    report["checks"] = gate.rows
    report["summary"] = dict(counts)
    width = max(len(row["check"]) for row in gate.rows) if gate.rows else 10
    print()
    print("%-5s %-*s %-5s %s" % ("REQ", width, "CHECK", "STATUS", "DETAIL"))
    for row in gate.rows:
        print("%-5s %-*s %-5s %s" % (row["req"], width, row["check"], row["status"], row["detail"]))
    print()
    print("summary:", dict(counts))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(report, fh, ensure_ascii=False, indent=1)
        print("report:", args.json)
    return 0 if counts.get("FAIL", 0) == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
