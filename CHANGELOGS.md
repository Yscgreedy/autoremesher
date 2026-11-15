Unreleased
- Diagnose the input: the core counts boundary edges/loops, non-manifold edges,
  components and degenerate faces before the run, and the CLI surfaces them as
  warnings (JSON `warning` events and stderr lines) instead of starting a run that
  was always going to keep a boundary
- Report what the pipeline decided: `--json` now carries an `input` event and a
  `result` with output topology, holes filled/left open, patches kept/dropped,
  island count, cores kept busy and the phase report as structured timings
- Stop the library from writing to stderr on its own: core diagnostics go through
  `AutoRemesher::Log` with a sink the application installs, so `--quiet` is quiet
  and `--verbose` prints the phase report exactly once (it used to print it twice)
- `--log-level quiet|normal|debug`; the per-step and per-hole traces that used to
  spray stderr on every run are now debug-only. The same treatment went to the
  remaining unconditional writers: parameterizer failures, the cross-field
  singularity summary, the chart/exact-target traces in quadparameterizer (still
  behind their env switches), the charge verifier, and the vendored
  isotropicremesher's "Found repeated halfedge" (now behind `AR_DEBUG_HALFEDGE`).
  `--quiet` prints nothing but real errors
- `--preset organic|hard-surface` actually changes the four knobs; `--model-type`
  is an alias. `ModelType`/`setModelType` are gone from the core API: the switch
  was stored and never read, so "hard surface" produced bit-identical output to
  "organic" while looking like it did something. The GUI preset now moves the
  sliders to the same table
- **Behaviour change:** `--target-quads` is now exact by default - the cover scale
  is solved from the budget, so the count lands within about ±5%. The old
  average-edge-length path (0.7N..1.05N) is still there as `--legacy-target-quads`;
  `--exact-target-quads` stays accepted so existing command lines keep parsing.
  The phase report says which path ran, and the CLI warns when the result misses
  the request by more than 10%
- The GUI passes `--exact-target-quads` by default, so a target slider in the UI
  lands where it says
- UV v2: the extractor keeps the cover's own UV per cross point and carries it through
  every cleanup pass, so QuadExtractor exposes `remeshedVertexUvs()`. `--uv-mode
  corner|vertex|hybrid` picks the channel: corner (default, unchanged), vertex
  (continuous across faces that share a vertex), hybrid (vertex + per-corner fallback
  where the two channels disagree on area). Measured: sphere 12k corner 0 degenerate
  UV faces / p99 3.7, vertex 9 / 62, hybrid 2 / 3.4; multi_part corner 38 degenerate,
  vertex 2577 - which is why corner stays the default
- UV seam splitting (`--uv-mode seam`): src/core/param/uvcharts.* unions faces into
  charts by comparing the per-face chart UVs on their shared edges (1% of the UV
  extent: inside a chart the independent evaluations agree to ~0.1%, across a seam the
  values are a whole cell apart) and duplicates the vertices on chart boundaries. One
  vertex cannot carry two UVs, so this is what lets per-vertex UVs describe a seam.
  Sphere 12k: 22 duplicated vertices, 0 degenerate UV faces, p99/p50 1.41 (corner 3.7);
  multi_part: 97 degenerate (corner 38).  Symmetry turns it off with a warning because
  the cut row would be re-welded into open edges.  Default stays corner
- Export UVs and normals: the output OBJ now carries `vt` (one per face corner) and
  `vn` (smooth, area weighted). The UVs are the island's own parameterization carried
  onto the quads through the chart under each face's centroid (`UvTransfer`, uniform
  grid point location), with islands laid out side by side and the set normalized into
  the unit square; a degenerate chart falls back to the face's planar frame.
  `--no-uv` / `--no-normals` skip either. Measured on the sphere fixture: 0 degenerate
  UV faces, UV-area p99/p50 = 3.7
- Move the topology analyzer to `src/core/geom/topologystats.cpp`, the UV transfer to
  `src/core/param/uvtransfer.cpp` and the symmetry pass to
  `src/core/pipeline/meshsymmetry.cpp`; the pipeline file is back to 1251 lines and its
  R8 exemption is gone
- Symmetry: `--symmetry x|y|z` cuts the output at the plane, welds the seam and
  mirrors the positive half, so the result is symmetric by construction.  Face
  clipping carries the face's own UVs; the cut row loses a corner (98.6% quads on
  the sphere fixture, 0 unmatched vertices, 3 seam edge(s) left open by the weld).
  `--symmetry-plane` moves the mirror plane off the coordinate plane (models that
  are not centred on the origin), `--symmetry-tolerance` sets what counts as the
  seam (0.002 of the bbox diagonal).  The seam weld now searches neighbouring grid
  cells, which closes the last sliver faces: the sphere fixture comes out with zero
  open edges and zero non-manifold edges
- Add `tools/smoke_cli.py`, wired into the gates workflow: it asserts the JSON
  envelope (input event, warning for an open shell, result topology/holes/patches/
  phases), that `--quiet` is silent, that vt/vn are written (and skipped on demand),
  and that the target count lands within 15%
- Regenerate `docs/regression-anchors.json` on the exact-by-default binary; the
  previous hashes are reproducible with `--legacy-target-quads`
- Remove the Qt dependency: UI, OpenGL preview, QtAwesome and QtWaitingSpinner are gone
- The main executable autoremesher is now a headless CLI and never starts a GUI
- Add a Qt-free CMake build (autoremesher_core + autoremesher) and build.bat
- Add the Go GUI in gui/: a separate process that drives the CLI over JSON lines
- Document the CLI/JSON contract in docs/cli-protocol.md

1.2.0
- Reduce triangles and irregular vertices in quad extraction
- Support touch pad pinch and scroll zooming