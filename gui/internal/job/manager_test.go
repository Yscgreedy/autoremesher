package job

import (
	"errors"
	"os"
	"strings"
	"testing"
	"time"

	"autoremesher-gui/internal/testutil"
)

const tinyOBJ = `# cube
v -0.5 -0.5 -0.5
v 0.5 -0.5 -0.5
v 0.5 0.5 -0.5
v -0.5 0.5 -0.5
v -0.5 -0.5 0.5
v 0.5 -0.5 0.5
v 0.5 0.5 0.5
v -0.5 0.5 0.5
f 1 4 3 2
f 5 6 7 8
f 1 2 6 5
f 2 3 7 6
f 3 4 8 7
f 4 1 5 8
`

func newTestManager(t *testing.T, env []string) (*Manager, *Workspace) {
	t.Helper()
	bin := testutil.BuildFakeCLI(t)
	ws, err := NewWorkspace()
	if err != nil {
		t.Fatalf("workspace: %v", err)
	}
	t.Cleanup(func() { _ = ws.Close() })
	m := NewManager(CLIInfo{Path: bin, Found: true, Source: "test"}, ws)
	if env != nil {
		m.SetEnv(env)
	}
	t.Cleanup(func() { _ = m.Close() })
	return m, ws
}

func baseParams(inputID string) Params {
	p := DefaultParams()
	p.InputID = inputID
	p.TargetQuads = 400
	return p
}

func collect(t *testing.T, j *Job, timeout time.Duration) []Event {
	t.Helper()
	ch, stop := j.Subscribe()
	defer stop()
	deadline := time.NewTimer(timeout)
	defer deadline.Stop()
	var out []Event
	for {
		select {
		case ev, ok := <-ch:
			if !ok {
				return out
			}
			out = append(out, ev)
		case <-deadline.C:
			t.Fatalf("timed out after %s waiting for job %s (%d events)", timeout, j.ID(), len(out))
			return out
		}
	}
}

func TestManagerHappyPath(t *testing.T) {
	m, ws := newTestManager(t, nil)
	in, err := ws.Save("cube.obj", strings.NewReader(tinyOBJ))
	if err != nil {
		t.Fatalf("save: %v", err)
	}
	if in.Vertices != 8 || in.Triangles != 12 {
		t.Fatalf("OBJ header counts: got %d verts / %d tris, want 8 / 12", in.Vertices, in.Triangles)
	}

	j, err := m.Start(baseParams(in.ID))
	if err != nil {
		t.Fatalf("start: %v", err)
	}
	events := collect(t, j, 60*time.Second)

	var sawProgress, sawResult bool
	for _, ev := range events {
		switch ev.Type {
		case "progress":
			sawProgress = true
		case "result":
			sawResult = true
			if ev.Quads == 0 {
				t.Fatalf("result has no quads: %+v", ev)
			}
		}
	}
	if !sawProgress {
		t.Fatal("no progress events forwarded")
	}
	if !sawResult {
		t.Fatalf("no result event; events: %+v", events)
	}

	snap := j.Snapshot()
	if snap.State != StateDone {
		t.Fatalf("state = %s (%s), want done", snap.State, snap.Error)
	}
	if snap.Result == nil || snap.Result.Quads == 0 {
		t.Fatalf("snapshot result missing: %+v", snap.Result)
	}
	if snap.Progress != 1 {
		t.Fatalf("progress = %v, want 1", snap.Progress)
	}
	if _, err := os.Stat(j.OutputPath()); err != nil {
		t.Fatalf("output file missing: %v", err)
	}
	if len(snap.Command) < 3 || snap.Command[1] != "--input" {
		t.Fatalf("unexpected CLI argv: %v", snap.Command)
	}
}

func TestManagerSecondJobRejectedAndCancelled(t *testing.T) {
	env := append(os.Environ(), "FAKE_DELAY_MS=180")
	m, ws := newTestManager(t, env)
	in, err := ws.Save("cube.obj", strings.NewReader(tinyOBJ))
	if err != nil {
		t.Fatalf("save: %v", err)
	}
	params := baseParams(in.ID)
	j1, err := m.Start(params)
	if err != nil {
		t.Fatalf("first start: %v", err)
	}
	if _, err := m.Start(params); !errors.Is(err, ErrBusy) {
		t.Fatalf("second start error = %v, want ErrBusy", err)
	}
	time.Sleep(120 * time.Millisecond)
	if err := m.Cancel(j1.ID()); err != nil {
		t.Fatalf("cancel: %v", err)
	}
	select {
	case <-j1.Done():
	case <-time.After(30 * time.Second):
		t.Fatal("job did not stop after cancel")
	}
	snap := j1.Snapshot()
	if snap.State != StateCancelled {
		t.Fatalf("state = %s, want cancelled", snap.State)
	}

	// A new job must be accepted once the previous one finished.
	j2, err := m.Start(params)
	if err != nil {
		t.Fatalf("start after cancel: %v", err)
	}
	if err := j2.Cancel(); err != nil {
		t.Fatalf("cancel second: %v", err)
	}
	select {
	case <-j2.Done():
	case <-time.After(30 * time.Second):
		t.Fatal("second job did not stop")
	}
}

func TestManagerErrorPath(t *testing.T) {
	env := append(os.Environ(), "FAKE_MODE=error")
	m, ws := newTestManager(t, env)
	in, err := ws.Save("cube.obj", strings.NewReader(tinyOBJ))
	if err != nil {
		t.Fatalf("save: %v", err)
	}
	j, err := m.Start(baseParams(in.ID))
	if err != nil {
		t.Fatalf("start: %v", err)
	}
	events := collect(t, j, 60*time.Second)
	var sawError bool
	for _, ev := range events {
		if ev.Type == "error" {
			sawError = true
		}
	}
	if !sawError {
		t.Fatalf("expected an error event, got %+v", events)
	}
	if snap := j.Snapshot(); snap.State != StateFailed {
		t.Fatalf("state = %s, want failed", snap.State)
	}
}

func TestManagerUnknownInput(t *testing.T) {
	m, _ := newTestManager(t, nil)
	if _, err := m.Start(baseParams("does-not-exist")); !errors.Is(err, ErrUnknownInput) {
		t.Fatalf("err = %v, want ErrUnknownInput", err)
	}
}

func TestParamsValidate(t *testing.T) {
	cases := []struct {
		name string
		mut  func(*Params)
		ok   bool
	}{
		{"default", func(p *Params) {}, true},
		{"target too small", func(p *Params) { p.TargetQuads = 99 }, false},
		{"target too large", func(p *Params) { p.TargetQuads = 2000001 }, false},
		{"target at max", func(p *Params) { p.TargetQuads = 2000000 }, true},
		{"adaptivity high", func(p *Params) { p.Adaptivity = 1.01 }, false},
		{"anisotropy low", func(p *Params) { p.Anisotropy = -0.1 }, false},
		{"sharp edge high", func(p *Params) { p.SharpEdge = 181 }, false},
		{"smooth normal ok", func(p *Params) { p.SmoothNormal = 180 }, true},
		{"threads high", func(p *Params) { p.Threads = 257 }, false},
		{"threads ok", func(p *Params) { p.Threads = 256 }, true},
		{"keep fraction high", func(p *Params) { p.KeepIslandFraction = 2 }, false},
		{"edge scaling low", func(p *Params) { p.EdgeScaling = 0 }, false},
		{"bad model type", func(p *Params) { p.ModelType = "voxel" }, false},
		{"missing input", func(p *Params) { p.InputID = "" }, false},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			p := baseParams("in")
			c.mut(&p)
			err := p.Validate()
			if c.ok && err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			if !c.ok && err == nil {
				t.Fatal("expected a validation error")
			}
		})
	}
}

func TestParamsArgs(t *testing.T) {
	p := DefaultParams()
	p.LocalRemesh = true
	p.WeldPatches = true
	p.ModelType = ModelHardSurface
	p.Threads = 8
	joined := strings.Join(p.Args("/in/a.obj", "/out/a.obj"), " ")
	for _, want := range []string{
		"--json", "--verbose",
		"--input /in/a.obj", "--output /out/a.obj",
		"--target-quads 20000",
		"--adaptivity 0.5",
		"--sharp-edge 60",
		"--threads 8",
		"--model-type hard-surface",
		"--local-remesh", "--weld-patches",
	} {
		if !strings.Contains(joined, want) {
			t.Fatalf("argv missing %q: %s", want, joined)
		}
	}
	if strings.Contains(joined, "--legacy-holes") {
		t.Fatalf("legacy holes should only be passed when enabled: %s", joined)
	}
}

func TestDecodeParamsAppliesDefaults(t *testing.T) {
	p, err := DecodeParams(strings.NewReader(`{"inputId":"abc","targetQuads":1000}`))
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if p.TargetQuads != 1000 || p.Adaptivity != 0.5 || p.SharpEdge != 60 || p.ModelType != ModelOrganic || p.EdgeScaling != 1 {
		t.Fatalf("defaults not applied: %+v", p)
	}
	// numeric strings from an HTML input are accepted too
	p2, err := DecodeParams(strings.NewReader(`{"inputId":"abc","adaptivity":"0.25","threads":"4","localRemesh":"true"}`))
	if err != nil {
		t.Fatalf("decode strings: %v", err)
	}
	if p2.Adaptivity != 0.25 || p2.Threads != 4 || !p2.LocalRemesh {
		t.Fatalf("string coercion failed: %+v", p2)
	}
}

// runOneJob starts one job and waits until it reaches a terminal state.
func runOneJob(t *testing.T, m *Manager, inputID string) *Job {
	t.Helper()
	j, err := m.Start(baseParams(inputID))
	if err != nil {
		t.Fatalf("start: %v", err)
	}
	select {
	case <-j.Done():
	case <-time.After(60 * time.Second):
		t.Fatalf("job %s did not finish", j.ID())
	}
	if snap := j.Snapshot(); snap.State != StateDone {
		t.Fatalf("job %s state = %s (%s), want done", j.ID(), snap.State, snap.Error)
	}
	return j
}

func retainedJobs(m *Manager) int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return len(m.jobs)
}

func retainedEvents(j *Job) int {
	j.mu.Lock()
	defer j.mu.Unlock()
	return len(j.history)
}

func TestManagerRetentionCap(t *testing.T) {
	m, ws := newTestManager(t, nil)
	in, err := ws.Save("cube.obj", strings.NewReader(tinyOBJ))
	if err != nil {
		t.Fatalf("save: %v", err)
	}
	if got := m.MaxJobs(); got != DefaultMaxJobs {
		t.Fatalf("default cap = %d, want %d", got, DefaultMaxJobs)
	}
	m.SetMaxJobs(2)

	var jobs []*Job
	for i := 0; i < 3; i++ {
		jobs = append(jobs, runOneJob(t, m, in.ID))
	}

	if got := retainedJobs(m); got != 2 {
		t.Fatalf("retained %d jobs, want 2", got)
	}
	if m.Get(jobs[0].ID()) != nil {
		t.Fatal("the oldest finished job should have been evicted")
	}
	for _, keep := range jobs[1:] {
		if m.Get(keep.ID()) == nil {
			t.Fatalf("job %s was evicted but should have been kept", keep.ID())
		}
	}

	// Eviction must release both halves of the memory: the retained events and
	// the result file inside the workspace.
	if n := retainedEvents(jobs[0]); n != 0 {
		t.Fatalf("evicted job still retains %d events", n)
	}
	if _, err := os.Stat(jobs[0].OutputPath()); !os.IsNotExist(err) {
		t.Fatalf("evicted job left its output behind (stat err = %v)", err)
	}
	if _, err := os.Stat(jobs[2].OutputPath()); err != nil {
		t.Fatalf("surviving job lost its output: %v", err)
	}
}

func TestManagerNeverEvictsRunningJob(t *testing.T) {
	env := append(os.Environ(), "FAKE_DELAY_MS=400")
	m, ws := newTestManager(t, env)
	in, err := ws.Save("cube.obj", strings.NewReader(tinyOBJ))
	if err != nil {
		t.Fatalf("save: %v", err)
	}
	m.SetMaxJobs(1)

	finished := runOneJob(t, m, in.ID)
	running, err := m.Start(baseParams(in.ID))
	if err != nil {
		t.Fatalf("start: %v", err)
	}
	// The cap is exceeded by exactly one job, and the only candidate is the
	// finished one.
	if m.Get(finished.ID()) != nil {
		t.Fatal("the finished job should have been evicted to make room")
	}
	if m.Get(running.ID()) == nil {
		t.Fatal("the running job must never be evicted")
	}

	_ = running.Cancel()
	select {
	case <-running.Done():
	case <-time.After(30 * time.Second):
		t.Fatal("running job did not stop")
	}
}

func TestPruneLockedKeepsRunningJobs(t *testing.T) {
	m := &Manager{
		jobs:    map[string]*Job{},
		maxJobs: 1,
		clock:   time.Now,
	}
	p := DefaultParams()
	for _, id := range []string{"a", "b", "c"} {
		m.jobs[id] = newJob(id, p, "/in.obj", "/out.obj", []string{"autoremesher"}, time.Now)
		m.order = append(m.order, id)
	}

	if victims := m.pruneLocked(); len(victims) != 0 {
		t.Fatalf("evicted %d running jobs, want 0", len(victims))
	}
	if len(m.jobs) != 3 || len(m.order) != 3 {
		t.Fatalf("running jobs were dropped: jobs=%d order=%d", len(m.jobs), len(m.order))
	}

	// A finished job becomes a candidate; an id whose job is already gone is
	// forgotten instead of counting against the cap forever.
	m.jobs["b"].finish(StateDone, "")
	m.order = append([]string{"stale"}, m.order...)
	victims := m.pruneLocked()
	if len(victims) != 1 || victims[0].ID() != "b" {
		t.Fatalf("victims = %v, want exactly b", victims)
	}
	if _, ok := m.jobs["b"]; ok {
		t.Fatal("job b should have been deleted from the map")
	}
	if len(m.order) != 2 {
		t.Fatalf("order = %v, want the two running jobs", m.order)
	}
	for _, id := range m.order {
		if id == "stale" {
			t.Fatal("stale id was not forgotten")
		}
	}
}
