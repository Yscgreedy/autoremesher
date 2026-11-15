# AutoRemesher CLI protocol (v1)

The headless executable is `autoremesher.exe` (no Qt). It is the only component that
runs the remeshing core. The Go GUI is a separate executable that spawns this one and
talks the contract below.

## Command line

```
autoremesher --input MODEL.obj --output OUT.obj --target-quads 20000 [options]
```

| flag | default | meaning |
|---|---|---|
| `-i, --input PATH` | required | input OBJ (triangles and/or quads; polygons are triangulated) |
| `-o, --output PATH` | `<input-stem>_remeshed.obj` | output OBJ: quad faces with `vt` per corner and `vn` per vertex |
| `--deviation` | off | measure how far the output sits from the input surface |
| `--uv-mode corner\|vertex\|hybrid\|seam` | corner | which UV channel to write (see below); seam cuts the mesh at chart boundaries |
| `--no-uv` | off | skip the `vt` lines |
| `--no-normals` | off | skip the `vn` lines |
| `--symmetry x\|y\|z` | off | cut at that plane, weld the seam, mirror the positive half |
| `--symmetry-plane F` | 0 | coordinate of the mirror plane along the axis |
| `--symmetry-tolerance F` | 0.002 | seam tolerance as a fraction of the bbox diagonal |
| `-q, --target-quads N` | 20000 | requested quad count (the core receives 2*N triangles). The cover scale is solved from this budget, so the result lands within about **±5%**; `--legacy-target-quads` restores the old average-edge-length path, which lands around **0.7N..1.05N** |
| `--adaptivity F` | 0.5 | gradient adaptivity 0..1 |
| `--anisotropy F` | 0.0 | anisotropy 0..1 |
| `--sharp-edge DEG` | 60 | sharp edge angle threshold |
| `--smooth-normal DEG` | 0 | smooth normal angle threshold |
| `--edge-scaling F` | 1.0 | edge scale multiplier |
| `--preset organic\|hard-surface` | organic | fills every remeshing knob the command line did not name |
| `--model-type organic\|hard-surface` | organic | alias of `--preset`, kept for old scripts |
| `--exact-target-quads` | **on** | solve every island's cover scale from the quad budget; accepted for compatibility |
| `--legacy-target-quads` | off | take the scale from the average edge length instead (−30%..+5%) |
| `--relative-tolerance` / `--absolute-tolerance` | relative | cross point weld tolerance mode |
| `--keep-island-fraction F` | 0 | keep patches above this area fraction |
| `--keep-all-islands` | off | keep every extracted island |
| `--weld-patches` | off | weld coincident patch vertices |
| `--graded-holes` / `--legacy-holes` | graded | hole filling strategy |
| `--projected-caps` | off | seal large holes with projected caps |
| `--local-remesh` | off | energy guided topology pass |
| `--threads N` | 0 (all) | pin the TBB worker count |
| `--json` | off | JSON-lines protocol on **stdout** (see below) |
| `--progress` | off | human readable progress on **stderr** |
| `--verbose` | off | per-phase report |
| `--log-level quiet\|normal\|debug` | normal | `debug` adds the per-step and per-hole traces the core keeps for diagnosis |
| `--version`, `--help` | | |

Exit codes: `0` success, `1` usage/input error, `2` remeshing failed, `3` output write failed.

## JSON lines on stdout (`--json`)

One JSON object per line, flushed as it happens. The GUI reads stdout line by line.

```json
{"type":"start","version":"1.2.0","input":"C:/m/a.obj","output":"C:/m/a_remeshed.obj","targetQuads":20000,"vertices":51234,"triangles":102400,"preset":"organic","exactTargetQuads":true}
{"type":"input","topology":{"vertices":51234,"faces":102400,"quads":0,"nonQuads":102400,"triangles":102400,"boundaryEdges":5950,"boundaryLoops":412,"nonManifoldEdges":3,"components":7,"degenerateFaces":0}}
{"type":"warning","code":"input_open_boundary","value":5950,"message":"input has 5950 boundary edge(s) in 412 loop(s); remeshing does not stitch input boundaries, so the output keeps a boundary there"}
{"type":"progress","value":0.42,"status":"Quad extraction"}
{"type":"report","lines":["Analyze input topology: 12.1 ms","Islands: 7, input triangles: 102400","..."]}
{"type":"result","output":"C:/m/a_remeshed.obj","seconds":12.31,"vertices":18422,"quads":19977,"nonQuads":23,"triangles":102400,
 "requestedQuads":20000,"quadsError":-0.0011,"exactTargetQuads":true,"preset":"organic","islands":7,"coresBusy":6.8,
 "topology":{"input":{"...":"same fields as the input event"},"output":{"boundaryEdges":0,"boundaryLoops":0,"nonManifoldEdges":0,"components":7,"degenerateFaces":0}},
 "holes":{"filled":3,"ignored":1,"facesAdded":42},"patches":{"kept":7,"dropped":2},
 "uvs":true,"uvCorners":79840,"normals":true,
 "deviation":{"samples":18422,"unresolved":0,"mean":0.000119,"p50":0.000081,"p95":0.000375,"max":0.000856},
 "symmetry":{"axis":"x","seamVertices":168,"clippedFaces":176,"mirroredFaces":3227,"verticesBefore":6278,"verticesAfter":6446,"plane":0,"tolerance":0.0069},
 "phases":[{"name":"Analyze input topology","ms":12.1,"accumulated":false},{"name":"Solve frame field","ms":267.8,"accumulated":true}]}
{"type":"error","message":"failed to load input.obj"}
```

Rules:
- `progress.value` is 0..1 and never decreases; `status` is the current pipeline step name.
- Progress events are throttled to at most ~25 per second.
- Exactly one `result` or one `error` terminates the stream, then the process exits.
- `report` is emitted only when `--verbose` is also given.
- `input` arrives once, before the run: the topology of the mesh as handed in, so a
  caller can refuse a shell with thousands of boundary edges before paying for it.
- `warning` events may appear anywhere between `start` and the terminating event.
  Codes in use: `input_open_boundary`, `input_non_manifold`, `input_multiple_components`,
  `input_degenerate_faces`, `holes_left_open`, `target_quads_off`.
- `result` always carries the flat v1 keys (`output`, `seconds`, `vertices`, `quads`,
  `nonQuads`, `triangles`), so an existing reader keeps working; the nested blocks are
  additive. `holes.ignored` counts boundary loops longer than 65 edges that graded
  filling refuses to close without `--projected-caps`.
- `uvs` is true only when every face got a UV; `uvMode` says which channel was used
  (`corner` = one vt per face corner, `vertex` = one per vertex, `hybrid` = vertex
  plus corners for the faces the vertex UVs would stretch); `uvCorners` is the number
  of `vt` lines written. `normals` reports the `vn` lines.
- `phases[].accumulated` marks per-island sums: `accumulated / wall clock` is the
  parallelism the phase reached, which is what `coresBusy` reports for the whole run.
- Human diagnostics never go to stdout when `--json` is set (stderr only).

## Go GUI side

The GUI (`gui/`, module `autoremesher-gui`) is a single pure-stdlib binary:
local HTTP server + embedded web UI, opens the browser on start. It never links Qt.
It locates the CLI as: `--cli` flag, else next to its own executable, else `.`, else PATH.
## GUI behaviour worth knowing (as implemented in `gui/`)

- The GUI always passes `--json --verbose` and always sets `--output` to a path in
  its temp workspace, so it never depends on the `<stem>_remeshed.obj` default.
- It probes the CLI with `--version` (3 s timeout); the first stdout line becomes the
  version shown in the UI. A CLI that prints nothing there reports an empty version.
- Cancellation is a GUI concept only: the CLI process tree is killed and the SSE
  stream ends with `{"type":"end","state":"cancelled"}`. Exit codes are ignored for
  a cancelled job.
- `--graded-holes` and `--relative-tolerance` are defaults and are never passed;
  only `--legacy-holes` is passed when the user opts into legacy hole filling.
- The GUI sends `--exact-target-quads` explicitly and its checkbox starts checked; the
  CLI now defaults to the same path, so the two agree without the flag. Scripts that
  pinned the old output should pass `--legacy-target-quads`.
- Static assets are served under `/s/<token>/`; every `/api/*` call needs the token
  as `?token=`, `X-AR-Token` header or cookie. The page HTML never contains it.

### Flags added in 1.3 (additive; every old flag keeps its meaning)

| flag | default | meaning |
|---|---|---|
| `--check-only` | off | analyse the input and exit: area, volume, bbox, unit guess, shell and boundary counts, degenerate edges, plus a parameter `advice` block. Writes nothing; `result` carries `"checkOnly": true` and `"output": null`. |
| `--preset-file PATH` | — | flat JSON object of knobs, applied **after** `--preset` and **before** the command line: a flag the caller passed always wins. An unknown or malformed key warns instead of being ignored silently. |
| `--symmetry check\|snap\|mirror-half` | mirror | what the pass does: `check` only measures, `snap` moves the near-plane vertices without cutting, `mirror-half` is the historical cut-weld-mirror. |
| `--symmetry-mode M` | mirror | the same choice spelled out, for scripts that pass the axis separately. |
| `--symmetry-keep positive\|negative` | positive | which side `mirror-half` keeps. |
| `--clean-degenerate` / `--no-clean-degenerate` | on | weld zero length edges and drop degenerate / duplicate faces at the end of extraction. Off restores the pre-1.3 file, for an A/B run. |
| `--max-aspect F` | 0 | reserved for the aspect clamp; 0 removes only the degenerate cells. |
| `--unit-hint mm\|cm\|m` | guess | only changes the unit `unit_guess` reports, never a threshold. |

### `result` blocks added in 1.3 (all additive)

```json
{"type":"result","quads":19697,"quadsError":-0.0152,
 "topology":{
   "input":{ "…":"unchanged" },
   "internal":{ "vertices":20170,"faces":20169,"quads":19662,"boundaryEdges":37,
                "edgesWithMoreThanTwoFaces":0,"nonManifoldEdgesBlender":37,
                "components":15,"degenerateFaces":0 },
   "exported":{ "…":"the same fields, measured by re-reading the written OBJ" },
   "same":true,
   "output":{ "…":"alias of internal, kept for v1 readers; removed in v2" } },
 "quality":{"poles_pct":9.87,"pole_count":1991,"interior_vertices":20168,
            "valence_max":14,"valence_hist":{"3":819,"4":18180,"5":902,"gt5":144},
            "edge_len":{"min":0.0,"p50":0.0027,"p95":0.00393,"max":0.00832},
            "quad_aspect":{"p50":1.34,"p95":3.57,"max":268.4},
            "quad_aspect_area":{"max":128.4},
            "zero_length_edges":0,"degenerate_faces":0},
 "budget":{"requested":20000,"actual":19697,"error":-0.0152,
           "withinTolerance":true,"mode":"solved","non_quads":507},
 "unit_guess":"m","unit_scale_to_mm":1000,
 "patches":{"kept":16,"dropped":0,"welded_vertices":812,"remaining_shells":15,
            "reason":"gap_exceeds_tolerance"},
 "uvStats":{"mode":"corner","area":1.2255,"overlap_pct":67.5,"chart_count":0,
            "max_stretch":4.1,"median_stretch":1.8,"stray_triangles":0,"bakeable":false},
 "symmetry":{"axis":"x","mode":"check","keep_positive":true,"match_pct":1.8,
             "max_deviation":0.00062,"mean_deviation":0.0002,"plane_estimate":-0.0091,
             "matched_vertices":136,"considered_vertices":7487,
             "snapped_verts":0,"max_snap":0.0,"snap_tolerance":0.0},
 "effective_params":{"targetQuads":20000,"adaptivity":0.3,"…":"every knob, after preset and preset-file"}}
```

Conventions worth knowing:

- **`topology.internal` vs `topology.exported`**: the first is the mesh the core
  measured, the second is what the written file contains, re-read and re-analysed
  with the same rules; `same` is true when every field agrees. They differed in
  1.2: the cleanup passes left duplicated faces in the file, which turned boundary
  edges into three-face edges, and an outside tool that drops duplicate polygons
  (Blender's importer does) then described a different mesh than the JSON did.
- **Non-manifold means two things.** `nonManifoldEdges` (historical) counts edges
  used by more than two faces; `nonManifoldEdgesBlender` also counts boundary
  edges, which is what Blender's Non Manifold select picks up.
- **`quality.poles_pct`** counts vertices away from the border whose valence is
  not 4, over the interior vertex count. `valence_hist` is over interior vertices,
  `valence_max` over all of them.
- **`quad_aspect`** is `max_edge / min_edge`; `quad_aspect_area` is
  `max_edge^2 / (4*area)`, the area-relative convention Blender-based tooling uses.
- **`uvStats.area`** is the summed UV area of the layout as written; above 1 the
  layout overlaps itself and a bake smears two faces onto the same texels.
  `bakeable` is true only when `area` is in (0,1] and `overlap_pct < 1`.
- **`uvs` stays a boolean** in v1; `uvStats` is the measured object beside it.
- **`patches.reason`** is empty when the shell count is the design and names the
  cause otherwise: `no_weld_requested`, `open_holes`, `gap_exceeds_tolerance`.
- Warning codes added: `output_reload_failed`, `symmetry_mismatch`,
  `symmetry_clip_heavy`.

### `result` / flags added in 1.4 (all additive)

| flag | default | meaning |
|---|---|---|
| `--uv-pack-items chart\|face` | chart | how the UV layout is packed. `chart` keeps the cover's charts whole (a seam only where the cover changes chart, but intra-chart extrapolation error shows up as overlap: 4.9-14.7% of the covered cells). `face` gives every face its own box: overlap 0.10-0.68% and `bakeable: true`, at the price of a seam on every edge and a lower `area` ceiling. |
| `--no-seal-closed-holes` | off (sealing is on) | keep the historical 8/65 edge hole thresholds even when the input had no boundary at all. On a closed input every loop is an extraction artifact, so by default they are all capped; this switch restores the old behaviour for an A/B run. |
| `--max-aspect F` | 20 | **best effort**, and now documented as such: quads only (triangles and n-gons are untouched), one pass at the end of extraction, and a quad whose short side cannot be collapsed without giving some edge a third face is left as it is. |

New `result` fields:

```json
"aspect_clamp":{"threshold":20.0,"collapses":34,"faces":34,"rejected":0,"too_long_remaining":0},
"budget":{"requested":6000,"actual":5188,"error":-0.1353,"withinTolerance":false,
          "faces":5613,"face_error":-0.0645,"faces_within_tolerance":false,
          "mode":"solved","non_quads":425,
          "suggestion":"the face count is within tolerance; the gap is the non-quad faces ..."},
"holes":{"filled":213,"ignored":0,"facesAdded":1723,"sealed_closed_input":95},
"uvStats":{"mode":"corner","area":0.205,"overlap_pct":0.579,"chart_count":5950,
           "max_stretch":95.6,"median_stretch":1.03,"stray_triangles":0,"bakeable":true,
           "packed":true,"packed_charts":5950,"pack_fill":0.41}
```

- **`budget.faces` / `face_error` / `faces_within_tolerance`**: a target of 20000 is a **face** budget, and a run can hit it while the quad count is 6-13% below, because the cleanup passes leave triangles and pentagons. `actual` / `error` / `withinTolerance` keep their v1 quad meaning; the face fields sit beside them and `suggestion` says which of the two missed.
- **`aspect_clamp`**: `rejected` counts faces the clamp had to leave alone, `too_long_remaining` counts quads still over the threshold when it gave up. `threshold: 0` means the clamp never ran (`--max-aspect 0` or `--no-clean-degenerate`).
- **`holes.sealed_closed_input`**: loops sealed only because the input was closed. They are also counted in `filled`; this field separates "the thresholds capped it" from "the closed-input rule capped it anyway".
- **`uvStats.chart_count`** is now the number of packed items (equal to `packed_charts`), i.e. what a baker sees. It used to come from `AutoRemesher::uvChartCount()`, which only the seam-splitting path fills, so every other mode reported 0 next to a non-zero `packed_charts`.
- **`--check-only`** now also reports symmetry: `symmetry.axes[]` with `match_pct` per axis (all three unless `--symmetry AXIS` names one), plus `verdict`: `mirror-half` at >=80% matched, `snap` at >=50%, `off` below. Measured cost: +46 ms on a 59,750-vertex mesh.
- Warning codes added: `closed_input_left_open` (a closed input still has boundary edges after sealing - a pinched loop that cannot be capped); `holes_left_open` now also mentions the closed-input rule.