# autoremesher-gui

The Qt-free desktop GUI for AutoRemesher. A single pure-stdlib Go binary that
serves a local web UI on loopback and drives the headless `autoremesher.exe`
through the JSON-lines protocol documented in `docs/cli-protocol.md`.

```
autoremesher.exe      headless CLI (C++, no Qt)  — owns the remeshing core
autoremesher-gui.exe  this program (Go)          — spawns the CLI per job
```

## Build

Go 1.27, standard library only (no modules to download, builds offline):

```bash
cd gui
go build -o dist/autoremesher-gui.exe ./cmd/autoremesher-gui   # Windows
go build -o dist/autoremesher-gui     ./cmd/autoremesher-gui   # Linux/macOS
```

Quality gates:

```bash
gofmt -l .        # must print nothing
go vet ./...      # must be clean
go test ./...     # unit + manager + HTTP smoke tests
```

## Run

```bash
autoremesher-gui.exe --cli ..\dist\autoremesher.exe
```

| flag | default | meaning |
|---|---|---|
| `--cli PATH` | discovery | path to the headless `autoremesher` executable |
| `--port N` | `0` | listen port, `0` picks a free one |
| `--addr IP` | `127.0.0.1` | bind address; loopback only, anything else is rejected |
| `--no-open` | off | do not open the browser |
| `--version` | | print the GUI version and exit |

The console prints the session URL, which contains a random token:

```
url   http://127.0.0.1:18080/s/1f3c.../
```

## CLI discovery order

1. `--cli PATH`
2. next to the GUI executable (`autoremesher.exe`)
3. `<exe dir>/dist`, `<exe dir>/..`, `<exe dir>/../dist`, `<exe dir>/../..`
4. the current working directory and `<cwd>/dist`
5. `PATH` lookup for `autoremesher`

If nothing is found the UI still starts and shows "CLI not found"; `/api/info`
re-probes on every call, so dropping the CLI next to the GUI fixes it without a
restart.

## Session token

16 random bytes, hex encoded, generated at start. The UI is served under
`/s/<token>/`; every `/api/*` call must carry the token as

* `X-AR-Token: <token>` header, or
* `?token=<token>` (used by `EventSource`, which cannot set headers), or
* the `ar_token` cookie.

Missing or wrong token → `401`. A wrong token in the static path → `404`.

## HTTP API

| method | path | body / query | response |
|---|---|---|---|
| GET | `/s/<token>/` | | embedded UI |
| GET | `/api/info` | | `{cli:{path,found,version,source}, version, platform, cpus}` |
| POST | `/api/upload` | multipart `file` (also accepts `model`) | `{id,name,path,size,vertices,faces,triangles}` |
| POST | `/api/run` | JSON params | `{jobId, command}`; `409` if a job is running |
| GET | `/api/events?job=ID` | | SSE stream, `data: {json}\n\n` |
| POST | `/api/cancel` | `{jobId}` | `{cancelled:true}` |
| GET | `/api/model?job=ID` | | result OBJ, `application/octet-stream` |

SSE frames are plain `data:` lines whose JSON always has a `type`:
`start`, `progress`, `log`, `report`, `result`, `error`, and a final
`{"type":"end","state":"done|failed|cancelled"}` before the stream closes.
Reconnecting replays the retained tail (last 2000 events), so a refreshed tab
still shows the run.

Only one job runs at a time; a second `/api/run` gets `409`. Cancel kills the
whole process tree (`taskkill /T /F` on Windows, process group `SIGKILL`
elsewhere). Closing the browser or losing the SSE connection never kills a job.

## Parameters (server side validation)

| name | range | default |
|---|---|---|
| `targetQuads` | 100 … 2,000,000 | 20000 |
| `adaptivity` | 0 … 1 | 0.5 |
| `anisotropy` | 0 … 1 | 0 |
| `sharpEdge` | 0 … 180 | 60 |
| `smoothNormal` | 0 … 180 | 0 |
| `modelType` | `organic` / `hard-surface` | organic |
| `threads` | 0 … 256 (0 = all) | 0 |
| `keepIslandFraction` | 0 … 1 | 0 |
| `edgeScaling` | 0.01 … 100 | 1 |
| `exactTargetQuads`, `weldPatches`, `legacyHoles`, `projectedCaps`, `localRemesh` | booleans | off |

Out-of-range values are rejected with `400` and a message that names the field.

## Keybindings

| key | action |
|---|---|
| `Ctrl+Enter` | run |
| `Esc` | cancel the running job |
| `W` | cycle shaded / shaded+wire / wire |
| `O` | open the file picker |
| `D` | save the result OBJ |
| `L` | collapse / expand the log |
| `F` | re-fit the camera |
| drag | orbit |
| wheel | zoom |
| double click | reset the view |

## Layout

```
gui/
  go.mod
  assets.go                     go:embed of web/
  cmd/autoremesher-gui/main.go  flags, listener, browser launch
  internal/job/                 discovery, params, stream parser, manager, workspace
  internal/server/              HTTP handlers, SSE, token guard, static embed host
  internal/testutil/            builds the fake CLI for tests
  internal/testdata/fakecli/    protocol stub (progress / result / error paths)
  web/                          index.html, style.css, app.js, viewer.js
  dist/autoremesher-gui.exe     built binary
```

## Tests

```bash
cd gui
go test ./...            # all packages
go test ./internal/job -run TestThrottle -v
go test ./internal/server -run TestConflictAndCancel -v
```

`internal/job` covers the JSON-lines parser and the 25/s progress throttle
(`stream_test.go`) and the full manager lifecycle against the fake CLI
(`manager_test.go`: happy path, busy rejection, cancel, error path, validation,
argv construction). `internal/server` covers token rejection, static page
delivery, upload → run → SSE → model, 409 on a concurrent run, cancel and
validation errors.

## End-to-end smoke test (no Qt, no C++ needed)

```bash
cd gui
go build -o dist/autoremesher-gui.exe ./cmd/autoremesher-gui
go build -o $TEMP/fakecli.exe ./internal/testdata/fakecli
dist/autoremesher-gui.exe --no-open --port 18080 --cli $TEMP/fakecli.exe
```

then in another shell (TOKEN from the printed URL):

```bash
curl -s "http://127.0.0.1:18080/api/info?token=$TOKEN"
curl -s -F "file=@cube.obj" "http://127.0.0.1:18080/api/upload?token=$TOKEN"
curl -s -X POST -H "Content-Type: application/json" \
     -d '{"inputId":"<id>","targetQuads":500}' \
     "http://127.0.0.1:18080/api/run?token=$TOKEN"
curl -sN "http://127.0.0.1:18080/api/events?job=<jobId>&token=$TOKEN"
curl -s -o out.obj "http://127.0.0.1:18080/api/model?job=<jobId>&token=$TOKEN"
curl -s "http://127.0.0.1:18080/s/$TOKEN/" | head -5
```

## Contract notes

* The GUI always passes `--json --verbose`: `--json` is required for the
  stream, `--verbose` is required for `report` events (contract rule).
* `--output` is always set explicitly to a workspace path, so the GUI never
  depends on the `<stem>_remeshed.obj` default.
* `--graded-holes` is never passed (it is the default); `--legacy-holes` is
  passed only when the user enables it.
* `--relative-tolerance` is never passed (default mode).
* Kernel/exit-code mapping: exit `0` + `result` → done, exit `1/2/3` or an
  `error` event → failed, killed by `/api/cancel` → cancelled.
