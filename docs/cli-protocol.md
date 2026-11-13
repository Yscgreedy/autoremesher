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
| `-o, --output PATH` | `<input-stem>_remeshed.obj` | output OBJ with quad faces |
| `-q, --target-quads N` | 20000 | requested quad count (the core receives 2*N triangles) |
| `--adaptivity F` | 0.5 | gradient adaptivity 0..1 |
| `--anisotropy F` | 0.0 | anisotropy 0..1 |
| `--sharp-edge DEG` | 60 | sharp edge angle threshold |
| `--smooth-normal DEG` | 0 | smooth normal angle threshold |
| `--edge-scaling F` | 1.0 | edge scale multiplier |
| `--model-type organic\|hard-surface` | organic | model type |
| `--exact-target-quads` | off | solve every island's cover scale from the quad budget |
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
| `--version`, `--help` | | |

Exit codes: `0` success, `1` usage/input error, `2` remeshing failed, `3` output write failed.

## JSON lines on stdout (`--json`)

One JSON object per line, flushed as it happens. The GUI reads stdout line by line.

```json
{"type":"start","version":"1.2.0","input":"C:/m/a.obj","output":"C:/m/a_remeshed.obj","targetQuads":20000,"vertices":51234,"triangles":102400}
{"type":"progress","value":0.42,"status":"Quad extraction"}
{"type":"log","level":"info","message":"..."}
{"type":"report","lines":["decimation 0.31s","frame field 1.20s"]}
{"type":"result","output":"C:/m/a_remeshed.obj","seconds":12.31,"vertices":18422,"quads":19977,"nonQuads":23,"triangles":102400}
{"type":"error","message":"failed to load input.obj"}
```

Rules:
- `progress.value` is 0..1 and never decreases; `status` is the current pipeline step name.
- Progress events are throttled to at most ~25 per second.
- Exactly one `result` or one `error` terminates the stream, then the process exits.
- `report` is emitted only when `--verbose` is also given.
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
- Static assets are served under `/s/<token>/`; every `/api/*` call needs the token
  as `?token=`, `X-AR-Token` header or cookie. The page HTML never contains it.
