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
- Export UVs and normals: the output OBJ now carries `vt` (one per face corner) and
  `vn` (smooth, area weighted). The UVs are the island's own parameterization carried
  onto the quads through the chart under each face's centroid (`UvTransfer`, uniform
  grid point location), with islands laid out side by side and the set normalized into
  the unit square; a degenerate chart falls back to the face's planar frame.
  `--no-uv` / `--no-normals` skip either. Measured on the sphere fixture: 0 degenerate
  UV faces, UV-area p99/p50 = 3.7
- Move the topology analyzer to `src/core/geom/topologystats.cpp` and the UV transfer to
  `src/core/param/uvtransfer.cpp`; the pipeline file no longer sits on its 1500 line
  budget
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