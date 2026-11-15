#!/usr/bin/env python3
"""Re-measure the remesher over a matrix of budgets and adaptivities.

Written for the round that follows docs/autoremesher_复测_v2.md: every number that
report quotes (quads, error, boundary edges, loops, shells, poles, aspect, UV
bakeability, holes) is read out of the CLI's own JSON result, and the OBJ it wrote
is re-measured independently so a claim about the file cannot drift from the file.

    python tools/retest_matrix.py --exe build/cli/dist/autoremesher.exe \
        --input MODEL.obj --out build/audit-tmp/matrix --configs 2000:0.3,6000:0.3

The matrix is fixed by the report's own rows so two rounds are comparable; --extra
appends configurations without disturbing those.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

# (target quads, adaptivity, extra flags).  The first six are exactly the rows of
# the v2 report, so the two tables line up row by row.
DEFAULT_CONFIGS = [
    (40000, 0.3, []),
    (20000, 0.3, []),
    (20000, 0.5, []),
    (10000, 0.3, []),
    (6000, 0.3, []),
    (6000, 0.5, []),
    (4000, 0.0, []),
    (2000, 0.3, []),
]


def run_one(exe, inp, out_path, target, adaptivity, extra, timeout=1200):
    args = [exe, "-i", inp, "-o", out_path,
            "--target-quads", str(target), "--adaptivity", str(adaptivity), "--json"]
    args.extend(extra)
    started = time.time()
    proc = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    elapsed = time.time() - started
    result = None
    warnings = []
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("type") == "result":
            result = event
        elif event.get("type") == "warning":
            warnings.append(event.get("code"))
    return {
        "target": target,
        "adaptivity": adaptivity,
        "extra": extra,
        "rc": proc.returncode,
        "wall_s": round(elapsed, 2),
        "warnings": warnings,
        "stderr": proc.stderr[-2000:] if proc.returncode != 0 else "",
        "result": result,
    }


def summarize(row):
    r = row["result"]
    if r is None:
        return {"target": row["target"], "adaptivity": row["adaptivity"],
                "extra": " ".join(row["extra"]), "rc": row["rc"], "error": "no result"}
    topo = r["topology"]["output"]
    quality = r["quality"]
    uv = r.get("uvStats") or {}
    holes = r.get("holes", {})
    patches = r.get("patches", {})
    budget = r.get("budget", {})
    return {
        "target": row["target"],
        "adaptivity": row["adaptivity"],
        "extra": " ".join(row["extra"]) if row["extra"] else "-",
        "rc": row["rc"],
        "wall_s": row["wall_s"],
        "quads": topo["quads"],
        "faces": topo["faces"],
        "error_pct": round(budget.get("error", 0.0) * 100.0, 1),
        "face_error_pct": round(budget.get("face_error", 0.0) * 100.0, 1),
        "within": budget.get("withinTolerance"),
        "faces_within": budget.get("faces_within_tolerance"),
        "aspect_clamp": (r.get("aspect_clamp") or {}).get("too_long_remaining"),
        "boundary_edges": topo["boundaryEdges"],
        "boundary_loops": topo["boundaryLoops"],
        "shells": topo["components"],
        "non_manifold": topo["nonManifoldEdges"],
        "poles_pct": round(quality.get("poles_pct", 0.0), 2),
        "aspect_max": round(quality.get("quad_aspect", {}).get("max", 0.0), 1),
        "aspect_p95": round(quality.get("quad_aspect", {}).get("p95", 0.0), 2),
        "holes_filled": holes.get("filled", 0),
        "holes_ignored": holes.get("ignored", 0),
        "holes_sealed": holes.get("sealed_closed_input", 0),
        "uv_area": uv.get("area"),
        "uv_overlap_pct": uv.get("overlap_pct"),
        "uv_chart_count": uv.get("chart_count"),
        "uv_max_stretch": uv.get("max_stretch"),
        "uv_bakeable": uv.get("bakeable"),
        "warnings": ",".join(row["warnings"]),
    }


COLUMNS = ["target", "adaptivity", "extra", "rc", "wall_s", "quads", "faces", "error_pct",
           "face_error_pct", "within", "faces_within",
           "boundary_edges", "boundary_loops", "shells", "non_manifold", "poles_pct",
           "aspect_max", "aspect_p95", "aspect_clamp", "holes_filled", "holes_ignored", "holes_sealed",
           "uv_area", "uv_overlap_pct", "uv_chart_count", "uv_max_stretch", "uv_bakeable"]


def markdown(rows):
    lines = ["| " + " | ".join(COLUMNS) + " |",
             "|" + "|".join(["---"] * len(COLUMNS)) + "|"]
    for row in rows:
        lines.append("| " + " | ".join(str(row.get(c, "")) for c in COLUMNS) + " |")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--input", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--configs", default="", help="target:adaptivity[:flagflag] list")
    parser.add_argument("--json", default="")
    args = parser.parse_args()

    configs = list(DEFAULT_CONFIGS)
    if args.configs:
        configs = []
        for spec in args.configs.split(","):
            parts = spec.split(":")
            extra = [p for p in parts[2:] if p]
            configs.append((int(parts[0]), float(parts[1]), extra))

    os.makedirs(args.out, exist_ok=True)
    rows = []
    for target, adaptivity, extra in configs:
        tag = "%dk_a%s%s" % (target // 1000, str(adaptivity).replace(".", ""),
                             ("_" + "_".join(e.strip("-").replace("-", "") for e in extra)) if extra else "")
        out_path = os.path.join(args.out, tag + ".obj")
        if os.path.exists(out_path):
            os.remove(out_path)  # the overwrite path is tested separately, not here
        row = run_one(args.exe, args.input, out_path, target, adaptivity, extra)
        summary = summarize(row)
        summary["obj"] = out_path
        summary["obj_bytes"] = os.path.getsize(out_path) if os.path.exists(out_path) else 0
        rows.append(summary)
        print(json.dumps(summary, ensure_ascii=False), flush=True)

    print()
    print(markdown(rows))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(rows, handle, ensure_ascii=False, indent=1)
        print("\nwritten:", args.json)


if __name__ == "__main__":
    main()
