#!/usr/bin/env python3
"""Contract smoke test for the autoremesher CLI.

The CLI is driven by scripts and by the Go GUI over the JSON envelope, so the
envelope is asserted here rather than described in prose: an input event, a
result with topology/holes/patches/phases, warnings for a shell that will keep a
boundary, a silent --quiet, and an --exact-target-quads that lands near the
requested count.

Usage:
    python tools/smoke_cli.py [path-to-autoremesher]

Exit codes: 0 all checks passed, 1 a check failed.
"""

import argparse
import json
import math
import os
import subprocess
import sys
import tempfile


def write_sphere(path, rings=48, segments=96):
    """A closed UV sphere: two unique poles and triangle fans, no coincident ring.

    Returns the number of faces in the top cap, which the open-shell case
    removes to leave exactly one boundary loop.
    """
    vertices = [(0.0, 0.0, 1.0)]
    for ring in range(1, rings):
        phi = math.pi * ring / rings
        for segment in range(segments):
            theta = 2.0 * math.pi * segment / segments
            vertices.append((math.sin(phi) * math.cos(theta),
                             math.sin(phi) * math.sin(theta),
                             math.cos(phi)))
    south = len(vertices) + 1
    vertices.append((0.0, 0.0, -1.0))

    def ring_vertex(ring, segment):
        # The north pole is vertex 1, so the first grid ring starts at 2.  Getting
        # this off by one is what makes a hand-rolled fixture silently non-manifold.
        return 2 + (ring - 1) * segments + segment % segments

    faces = []
    cap_faces = 0
    for segment in range(segments):
        faces.append((1, ring_vertex(1, segment + 1), ring_vertex(1, segment)))
        cap_faces += 1
    for ring in range(1, rings - 1):
        for segment in range(segments):
            first = ring_vertex(ring, segment)
            second = ring_vertex(ring, segment + 1)
            third = ring_vertex(ring + 1, segment + 1)
            fourth = ring_vertex(ring + 1, segment)
            faces.append((first, second, third))
            faces.append((first, third, fourth))
    for segment in range(segments):
        faces.append((south, ring_vertex(rings - 1, segment), ring_vertex(rings - 1, segment + 1)))

    with open(path, "w", encoding="utf-8") as handle:
        for vertex in vertices:
            handle.write("v %.6f %.6f %.6f\n" % vertex)
        for face in faces:
            handle.write("f %d %d %d\n" % face)
    return cap_faces


def run(binary, arguments):
    return subprocess.run([binary] + arguments, capture_output=True, text=True)


def parse_events(stdout):
    return [json.loads(line) for line in stdout.splitlines() if line.strip()]


def count_obj(path):
    counts = {"v": 0, "vt": 0, "vn": 0, "f": 0, "corners": 0, "slashed": 0}
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            if line.startswith("v "):
                counts["v"] += 1
            elif line.startswith("vt "):
                counts["vt"] += 1
            elif line.startswith("vn "):
                counts["vn"] += 1
            elif line.startswith("f "):
                counts["f"] += 1
                counts["corners"] += len(line.split()) - 1
                if "/" in line:
                    counts["slashed"] += 1
    return counts


def check(condition, message, failures):
    if not condition:
        failures.append(message)


def main():
    parser = argparse.ArgumentParser(description="CLI JSON contract smoke test.")
    parser.add_argument("binary", nargs="?",
                        default=os.path.join("build", "cli", "dist", "autoremesher.exe"))
    options = parser.parse_args()
    binary = options.binary
    if not os.path.isfile(binary):
        print("smoke: binary not found: %s" % binary, file=sys.stderr)
        return 1

    failures = []
    with tempfile.TemporaryDirectory() as workspace:
        model = os.path.join(workspace, "sphere.obj")
        write_sphere(model)

        proc = run(binary, ["--input", model, "--output", os.path.join(workspace, "out.obj"),
                            "--target-quads", "2000", "--json", "--quiet"])
        check(proc.returncode == 0, "exit code %d" % proc.returncode, failures)
        check(proc.stderr.strip() == "", "--quiet printed to stderr: %r" % proc.stderr[:200], failures)

        events = parse_events(proc.stdout)
        types = [event.get("type") for event in events]
        check(bool(events), "no JSON events", failures)
        check(types[-1] == "result",
              "stream ended with %r, not result" % (types[-1] if types else None), failures)
        check("input" in types, "no input event", failures)
        input_codes = [event.get("code") for event in events
                       if event.get("type") == "warning"
                       and str(event.get("code", "")).startswith("input_")]
        check(not input_codes, "closed sphere flagged as unhealthy: %r" % input_codes, failures)

        result = events[-1] if events else {}
        for key in ("output", "seconds", "vertices", "quads", "nonQuads", "triangles",
                    "requestedQuads", "quadsError", "exactTargetQuads", "preset",
                    "islands", "coresBusy", "topology", "holes", "patches", "phases"):
            check(key in result, "result is missing %r" % key, failures)
        topology = result.get("topology", {})
        for side in ("input", "output"):
            block = topology.get(side, {})
            for field in ("vertices", "faces", "quads", "nonQuads", "triangles",
                          "boundaryEdges", "boundaryLoops", "nonManifoldEdges",
                          "components", "degenerateFaces"):
                check(field in block, "topology.%s is missing %r" % (side, field), failures)
        for field in ("filled", "ignored", "facesAdded"):
            check(field in result.get("holes", {}), "holes is missing %r" % field, failures)
        for field in ("kept", "dropped"):
            check(field in result.get("patches", {}), "patches is missing %r" % field, failures)
        phases = result.get("phases", [])
        check(bool(phases), "phases is empty", failures)
        for phase in phases:
            if not all(key in phase for key in ("name", "ms", "accumulated")):
                failures.append("phase entry is missing a field: %r" % phase)
                break
        check(topology.get("output", {}).get("boundaryLoops", 1) == 0,
              "closed sphere came back open", failures)

        # UVs and normals are what makes the output usable downstream: a quad mesh
        # without vt has to be re-unwrapped before it can be textured.
        obj = count_obj(os.path.join(workspace, "out.obj"))
        check(result.get("uvs") is True, "result does not report uvs", failures)
        check(obj["vt"] == obj["corners"],
              "vt lines %d for %d face corners" % (obj["vt"], obj["corners"]), failures)
        check(obj["vn"] == obj["v"], "vn lines %d for %d vertices" % (obj["vn"], obj["v"]), failures)
        check(obj["slashed"] == obj["f"], "faces are missing the vt/vn references", failures)

        proc = run(binary, ["--input", model, "--output", os.path.join(workspace, "bare.obj"),
                            "--target-quads", "500", "--no-uv", "--no-normals", "--quiet"])
        bare = count_obj(os.path.join(workspace, "bare.obj"))
        check(proc.returncode == 0 and bare["vt"] == 0 and bare["vn"] == 0,
              "--no-uv/--no-normals still wrote vt=%d vn=%d" % (bare["vt"], bare["vn"]), failures)

        proc = run(binary, ["--input", model, "--output", os.path.join(workspace, "exact.obj"),
                            "--target-quads", "2000", "--exact-target-quads", "--json", "--quiet"])
        check(proc.returncode == 0, "exact run exit code %d" % proc.returncode, failures)
        exact = parse_events(proc.stdout)[-1] if proc.stdout.strip() else {}
        quads = exact.get("quads", 0)
        error = abs(quads - 2000) / 2000.0
        check(error <= 0.15,
              "exact solve landed %d quads for 2000 (%.1f%%)" % (quads, error * 100.0), failures)

        shell = os.path.join(workspace, "shell.obj")
        kept = []
        with open(model, "r", encoding="utf-8") as handle:
            for line in handle:
                if line.startswith("f "):
                    indices = [int(token) for token in line.split()[1:]]
                    if any(index < 50 for index in indices):
                        continue
                kept.append(line)
        with open(shell, "w", encoding="utf-8") as handle:
            handle.writelines(kept)
        proc = run(binary, ["--input", shell, "--output", os.path.join(workspace, "shell_out.obj"),
                            "--target-quads", "800", "--json", "--quiet"])
        codes = [event.get("code") for event in parse_events(proc.stdout)
                 if event.get("type") == "warning"]
        check("input_open_boundary" in codes,
              "open shell did not warn about its boundary (codes: %r)" % codes, failures)

        # Symmetry cuts and mirrors: the result has to say what it cut and welded.
        proc = run(binary, ["--input", model, "--output", os.path.join(workspace, "sym.obj"),
                            "--target-quads", "800", "--symmetry", "x", "--json", "--quiet"])
        check(proc.returncode == 0, "symmetry run exit code %d" % proc.returncode, failures)
        sym_events = parse_events(proc.stdout)
        sym_result = sym_events[-1] if sym_events else {}
        symmetry = sym_result.get("symmetry", {})
        check(symmetry.get("axis") == "x", "symmetry axis missing: %r" % symmetry, failures)
        check(symmetry.get("seamVertices", 0) > 0, "symmetry welded no seam: %r" % symmetry, failures)
        check(symmetry.get("mirroredFaces", 0) > 0, "symmetry mirrored no faces: %r" % symmetry, failures)
        mirror = count_obj(os.path.join(workspace, "sym.obj"))
        check(mirror["v"] == symmetry.get("verticesAfter"),
              "symmetry vertex count %d vs reported %r" % (mirror["v"], symmetry.get("verticesAfter")),
              failures)

    if failures:
        print("smoke: FAIL")
        for failure in failures:
            print("  - " + failure)
        return 1
    print("smoke: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
