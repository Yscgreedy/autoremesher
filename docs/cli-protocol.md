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
| `--no-uv` | off | skip the `vt` lines |
| `--no-normals` | off | skip the `vn` lines |
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
- `uvs` is true only when every face got a UV; `uvCorners` is the number of `vt`
  lines written (one per face corner, not per vertex). `normals` reports the `vn` lines.
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
