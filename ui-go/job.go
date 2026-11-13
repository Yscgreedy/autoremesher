// Job orchestration for the optional Go front end.
//
// One Job owns exactly one autoremesher_cli process. The CLI is a black box
// across a process boundary: we hand it flags, we read its stdout report and its
// stderr event stream. Two stderr dialects are supported, because the
// --events-json switch may not exist in the binary on disk:
//
//	structured  {"schema_version":1,"type":"progress","percent":12,"status":"..."}
//	text        12% done. Extracting connections
//
// execOnce() runs one attempt in one dialect. run() owns the fallback: when the
// CLI answers "unknown option" for --events-json the job is re-run once without
// it, and the service records that it is degraded from then on.
package main

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	statusQueued   = "queued"
	statusRunning  = "running"
	statusDone     = "done"
	statusFailed   = "failed"
	statusCanceled = "canceled"
)

// Limits that keep a runaway job from eating the browser tab or the process.
const (
	maxJobEvents    = 400 // ring buffer replayed to a late SSE subscriber
	maxTailLines    = 40  // stderr/stdout tail kept for error messages
	maxObjReadBytes = 64 << 20
)

// Params mirrors the CLI flags. Defaults match autoremesher_cli (see --help).
type Params struct {
	TargetQuads  int      `json:"target_quads"`
	EdgeScaling  float64  `json:"edge_scaling"`
	SharpEdge    float64  `json:"sharp_edge"`
	SmoothNormal float64  `json:"smooth_normal"`
	Adaptivity   float64  `json:"adaptivity"`
	Anisotropy   float64  `json:"anisotropy"`
	ExtraArgs    []string `json:"extra_args"`
}

// paramsRequest keeps "absent" distinguishable from an explicit zero, which
// matters for adaptivity/anisotropy where 0 is a meaningful value.
type paramsRequest struct {
	TargetQuads  *int     `json:"target_quads"`
	EdgeScaling  *float64 `json:"edge_scaling"`
	SharpEdge    *float64 `json:"sharp_edge"`
	SmoothNormal *float64 `json:"smooth_normal"`
	Adaptivity   *float64 `json:"adaptivity"`
	Anisotropy   *float64 `json:"anisotropy"`
	ExtraArgs    []string `json:"extra_args"`
}

func (r paramsRequest) normalize() (Params, error) {
	p := Params{
		TargetQuads:  50000,
		EdgeScaling:  1.0,
		SharpEdge:    90.0,
		SmoothNormal: 0.0,
		Adaptivity:   1.0,
		Anisotropy:   1.0,
	}
	if r.TargetQuads != nil {
		p.TargetQuads = *r.TargetQuads
	}
	if r.EdgeScaling != nil {
		p.EdgeScaling = *r.EdgeScaling
	}
	if r.SharpEdge != nil {
		p.SharpEdge = *r.SharpEdge
	}
	if r.SmoothNormal != nil {
		p.SmoothNormal = *r.SmoothNormal
	}
	if r.Adaptivity != nil {
		p.Adaptivity = *r.Adaptivity
	}
	if r.Anisotropy != nil {
		p.Anisotropy = *r.Anisotropy
	}
	p.ExtraArgs = sanitizeExtraArgs(r.ExtraArgs)

	// Ranges are the ones printed by `autoremesher_cli --help`.
	if p.TargetQuads < 1 || p.TargetQuads > 2000000 {
		return p, fmt.Errorf("target_quads 必须在 1..2000000 之间（收到 %d）", p.TargetQuads)
	}
	if p.EdgeScaling < 1.0 || p.EdgeScaling > 4.0 {
		return p, fmt.Errorf("edge_scaling 必须在 1.0..4.0 之间（收到 %g）", p.EdgeScaling)
	}
	if p.SharpEdge < 30.0 || p.SharpEdge > 180.0 {
		return p, fmt.Errorf("sharp_edge 必须在 30..180 之间（收到 %g）", p.SharpEdge)
	}
	if p.SmoothNormal < 0.0 || p.SmoothNormal > 180.0 {
		return p, fmt.Errorf("smooth_normal 必须在 0..180 之间（收到 %g）", p.SmoothNormal)
	}
	if p.Adaptivity < 0.0 || p.Adaptivity > 1.0 {
		return p, fmt.Errorf("adaptivity 必须在 0..1 之间（收到 %g）", p.Adaptivity)
	}
	if p.Anisotropy < 0.0 || p.Anisotropy > 1.0 {
		return p, fmt.Errorf("anisotropy 必须在 0..1 之间（收到 %g）", p.Anisotropy)
	}
	return p, nil
}

// sanitizeExtraArgs drops the flags the server owns, so a caller cannot
// redirect the output file or desynchronise the event stream.
func sanitizeExtraArgs(in []string) []string {
	out := make([]string, 0, len(in))
	for i := 0; i < len(in); i++ {
		arg := strings.TrimSpace(in[i])
		if arg == "" {
			continue
		}
		name := arg
		inline := false
		if eq := strings.IndexByte(arg, '='); eq > 0 {
			name = arg[:eq]
			inline = true
		}
		switch name {
		case "-i", "--input", "-o", "--output", "--report", "--events-json":
			if !inline && (name == "-i" || name == "--input" || name == "-o" || name == "--output" || name == "--report") && i+1 < len(in) {
				i++
			}
			continue
		}
		out = append(out, arg)
	}
	return out
}

// Event is both the SSE payload and the stored replay entry.
type Event struct {
	Type string `json:"type"`
	Seq  int    `json:"seq"`
	Time string `json:"time"`
	// Pointers keep the wire format honest: a progress event always carries
	// "percent" (0 is a real value), while log/phase events never do.
	Percent   *int     `json:"percent,omitempty"`
	Status    string   `json:"status,omitempty"`
	Line      string   `json:"line,omitempty"`
	Lines     []string `json:"lines,omitempty"`
	Quads     *int     `json:"quads,omitempty"`
	NonQuads  *int     `json:"non_quads,omitempty"`
	Vertices  *int     `json:"vertices,omitempty"`
	Exit      *int     `json:"exit,omitempty"`
	ElapsedMS int64    `json:"elapsed_ms,omitempty"`
	Error     string   `json:"error,omitempty"`
}

// Job is the mutable state of one CLI run.
type Job struct {
	ID      string
	Input   string
	Output  string
	Params  Params
	Created time.Time

	mu       sync.Mutex
	status   string
	started  time.Time
	finished time.Time
	elapsed  time.Duration
	quads    int
	nonQuads int
	vertices int
	exit     int
	degraded bool
	errText  string
	events   []Event
	seq      int
	subs     map[chan Event]struct{}

	ctx    context.Context
	cancel context.CancelFunc
}

func newJob(id, input, output string, p Params) *Job {
	ctx, cancel := context.WithCancel(context.Background())
	return &Job{
		ID:      id,
		Input:   input,
		Output:  output,
		Params:  p,
		Created: time.Now(),
		status:  statusQueued,
		subs:    make(map[chan Event]struct{}),
		ctx:     ctx,
		cancel:  cancel,
	}
}

func (j *Job) emit(ev Event) {
	j.mu.Lock()
	j.seq++
	ev.Seq = j.seq
	ev.Time = time.Now().Format("15:04:05.000")
	j.events = append(j.events, ev)
	if len(j.events) > maxJobEvents {
		j.events = append([]Event(nil), j.events[len(j.events)-maxJobEvents:]...)
	}
	subs := make([]chan Event, 0, len(j.subs))
	for ch := range j.subs {
		subs = append(subs, ch)
	}
	j.mu.Unlock()

	for _, ch := range subs {
		// A slow browser must not stall the pipeline: drop for that subscriber
		// only, the replay buffer still holds the full history.
		select {
		case ch <- ev:
		default:
		}
	}
}

func (j *Job) subscribe() (chan Event, []Event, string) {
	ch := make(chan Event, 128)
	j.mu.Lock()
	backlog := append([]Event(nil), j.events...)
	j.subs[ch] = struct{}{}
	status := j.status
	j.mu.Unlock()
	return ch, backlog, status
}

func (j *Job) unsubscribe(ch chan Event) {
	j.mu.Lock()
	delete(j.subs, ch)
	j.mu.Unlock()
}

func (j *Job) setStatus(s string) {
	j.mu.Lock()
	j.status = s
	if s == statusRunning {
		j.started = time.Now()
	}
	j.mu.Unlock()
}

func (j *Job) setDegraded(note string) {
	j.mu.Lock()
	j.degraded = true
	j.mu.Unlock()
	j.emit(Event{Type: "log", Line: note})
}

func (j *Job) isTerminal() bool {
	j.mu.Lock()
	defer j.mu.Unlock()
	return j.status == statusDone || j.status == statusFailed || j.status == statusCanceled
}

// jobView is the JSON shape returned by GET /api/jobs.
type jobView struct {
	ID           string   `json:"id"`
	Input        string   `json:"input"`
	Output       string   `json:"output"`
	Status       string   `json:"status"`
	Params       Params   `json:"params"`
	CreatedAt    string   `json:"created_at"`
	ElapsedMS    int64    `json:"elapsed_ms"`
	Quads        int      `json:"quads"`
	NonQuads     int      `json:"non_quads"`
	Vertices     int      `json:"vertices"`
	Exit         int      `json:"exit"`
	Degraded     bool     `json:"degraded"`
	Error        string   `json:"error,omitempty"`
	OutputExists bool     `json:"output_exists"`
	Percent      int      `json:"percent"`
	Extras       []string `json:"extra_args,omitempty"`
}

func (j *Job) view() jobView {
	j.mu.Lock()
	defer j.mu.Unlock()
	percent := 0
	if j.status == statusDone {
		percent = 100
	}
	for i := len(j.events) - 1; i >= 0; i-- {
		if j.events[i].Type == "progress" && j.events[i].Percent != nil {
			percent = *j.events[i].Percent
			break
		}
	}
	_, statErr := os.Stat(j.Output)
	return jobView{
		ID: j.ID, Input: j.Input, Output: j.Output, Status: j.status,
		Params: j.Params, CreatedAt: j.Created.Format(time.RFC3339),
		ElapsedMS: j.elapsed.Milliseconds(), Quads: j.quads, NonQuads: j.nonQuads,
		Vertices: j.vertices, Exit: j.exit, Degraded: j.degraded, Error: j.errText,
		OutputExists: statErr == nil, Percent: percent, Extras: j.Params.ExtraArgs,
	}
}

// execResult is what one CLI attempt reported.
type execResult struct {
	exit          int
	startErr      error
	unknownEvents bool // CLI rejected --events-json
	sawStructured bool // at least one NDJSON event parsed
	phaseLines    []string
	quads         int
	nonQuads      int
	vertices      int
	hasStats      bool
	stderrTail    []string
	stdoutTail    []string
	errMessage    string
}

// parseTextProgress reads the historical "<percent>% done. <status>" line the
// CLI prints when --events-json is unavailable.
func parseTextProgress(line string) (int, string, bool) {
	const marker = "% done."
	idx := strings.Index(line, marker)
	if idx < 1 || idx > 3 {
		return 0, "", false
	}
	for i := 0; i < idx; i++ {
		if line[i] < '0' || line[i] > '9' {
			return 0, "", false
		}
	}
	percent, err := strconv.Atoi(line[:idx])
	if err != nil || percent > 100 {
		return 0, "", false
	}
	return percent, strings.TrimSpace(line[idx+len(marker):]), true
}

// Manager owns every job and the CLI capability state.
type Manager struct {
	mu      sync.Mutex
	jobs    map[string]*Job
	order   []string
	cliPath string
	workDir string
	maxJobs int

	stateMu     sync.RWMutex
	eventsJSON  bool
	degradeNote string
	probeNote   string
}

func NewManager(cliPath, workDir string, maxJobs int) *Manager {
	if maxJobs < 1 {
		maxJobs = 32
	}
	return &Manager{
		jobs:    make(map[string]*Job),
		cliPath: cliPath,
		workDir: workDir,
		maxJobs: maxJobs,
	}
}

func (m *Manager) EventsJSON() (bool, string, string) {
	m.stateMu.RLock()
	defer m.stateMu.RUnlock()
	return m.eventsJSON, m.degradeNote, m.probeNote
}

func (m *Manager) setEventsJSON(supported bool, note string) {
	m.stateMu.Lock()
	m.eventsJSON = supported
	m.degradeNote = note
	m.stateMu.Unlock()
}

func (m *Manager) setProbeNote(note string) {
	m.stateMu.Lock()
	m.probeNote = note
	m.stateMu.Unlock()
}

func (m *Manager) eventsJSONSupported() bool {
	m.stateMu.RLock()
	defer m.stateMu.RUnlock()
	return m.eventsJSON
}

// probeEventsJSON decides the dialect for the whole process lifetime.
//
// The probe is `--events-json --version`: a CLI that knows the switch parses
// it and then prints its version and exits 0; an older CLI hits the unknown
// option branch first and exits 1 with "unknown option" on stderr. Using
// --version keeps the probe from touching the remeshing pipeline.
func (m *Manager) probeEventsJSON() {
	if m.cliPath == "" {
		m.setEventsJSON(false, "")
		m.setProbeNote("未找到 autoremesher_cli，事件流不可用")
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, m.cliPath, "--events-json", "--version")
	cmd.Dir = m.workDir
	out, err := cmd.Output()
	if err == nil && strings.Contains(string(out), "AutoRemesher") {
		m.setEventsJSON(true, "")
		m.setProbeNote("CLI 支持 --events-json")
		return
	}
	msg := "CLI 不支持 --events-json，将解析文本进度"
	if err != nil {
		msg += "（探测失败: " + firstLine(err.Error()) + "）"
	}
	m.setEventsJSON(false, msg)
	m.setProbeNote(msg)
}

// Start validates nothing but the existence of the paths (the handler does
// that) and launches the job goroutine.
func (m *Manager) Start(input, output string, p Params) *Job {
	id := newJobID()
	j := newJob(id, input, output, p)

	m.mu.Lock()
	m.jobs[id] = j
	m.order = append(m.order, id)
	victims := m.pruneLocked()
	m.mu.Unlock()

	for _, v := range victims {
		v.cancel()
	}

	go m.run(j)
	return j
}

// pruneLocked drops the oldest finished jobs once the cap is exceeded.
func (m *Manager) pruneLocked() []*Job {
	var victims []*Job
	for len(m.order) > m.maxJobs {
		dropped := false
		for i, id := range m.order {
			j := m.jobs[id]
			if j == nil {
				m.order = append(m.order[:i], m.order[i+1:]...)
				dropped = true
				break
			}
			if j.isTerminal() {
				delete(m.jobs, id)
				m.order = append(m.order[:i], m.order[i+1:]...)
				victims = append(victims, j)
				dropped = true
				break
			}
		}
		if !dropped {
			break // everything is still running
		}
	}
	return victims
}

func (m *Manager) Get(id string) *Job {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.jobs[id]
}

func (m *Manager) List() []jobView {
	m.mu.Lock()
	ids := append([]string(nil), m.order...)
	jobs := make([]*Job, 0, len(ids))
	for _, id := range ids {
		if j := m.jobs[id]; j != nil {
			jobs = append(jobs, j)
		}
	}
	m.mu.Unlock()

	views := make([]jobView, 0, len(jobs))
	for i := len(jobs) - 1; i >= 0; i-- { // newest first
		views = append(views, jobs[i].view())
	}
	return views
}

func (m *Manager) Count() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return len(m.jobs)
}

// run executes the job, applies the --events-json fallback exactly once, and
// publishes the terminal event.
func (m *Manager) run(j *Job) {
	defer j.cancel()

	j.setStatus(statusRunning)
	j.emit(Event{Type: "log", Line: "启动: " + filepath.Base(m.cliPath)})

	withEvents := m.eventsJSONSupported()
	if !withEvents {
		j.setDegraded("服务处于降级模式：使用文本进度解析（" + m.degradeReason() + "）")
	}

	res := m.execOnce(j, withEvents)
	if withEvents && res.unknownEvents {
		// The binary ignored our probe or changed between probe and run.
		m.setEventsJSON(false, "CLI 运行期拒绝 --events-json，已降级为文本进度解析")
		j.setDegraded("CLI 不认识 --events-json，自动降级并重跑一次（不带该开关）")
		res = m.execOnce(j, false)
	}

	quads, nonQuads, vertices := res.quads, res.nonQuads, res.vertices
	if !res.hasStats {
		if q, n, v, ok := countObjFaces(j.Output); ok {
			quads, nonQuads, vertices = q, n, v
			j.emit(Event{Type: "log", Line: fmt.Sprintf("统计取自输出 OBJ：quads=%d non_quads=%d vertices=%d", q, n, v)})
		}
	}

	status := statusDone
	errText := res.errMessage
	switch {
	case j.ctx.Err() != nil:
		status = statusCanceled
		errText = "已取消"
	case res.startErr != nil:
		status = statusFailed
		errText = "无法启动 CLI: " + res.startErr.Error()
	case res.exit != 0:
		status = statusFailed
		if errText == "" {
			errText = fmt.Sprintf("CLI 退出码 %d", res.exit)
			if len(res.stderrTail) > 0 {
				errText += "：" + strings.Join(res.stderrTail, " | ")
			}
		}
	default:
		if _, err := os.Stat(j.Output); err != nil {
			status = statusFailed
			errText = "CLI 正常退出但未生成输出文件: " + j.Output
		}
	}

	j.finish(status, quads, nonQuads, vertices, res.exit, errText, res.phaseLines)
}

func (m *Manager) degradeReason() string {
	m.stateMu.RLock()
	defer m.stateMu.RUnlock()
	return m.degradeNote
}

func (j *Job) finish(status string, quads, nonQuads, vertices, exit int, errText string, phaseLines []string) {
	if len(phaseLines) > 0 {
		j.emit(Event{Type: "phase", Lines: phaseLines})
	}

	j.mu.Lock()
	j.status = status
	j.finished = time.Now()
	if !j.started.IsZero() {
		j.elapsed = j.finished.Sub(j.started)
	}
	j.quads, j.nonQuads, j.vertices, j.exit = quads, nonQuads, vertices, exit
	j.errText = errText
	elapsed := j.elapsed
	j.mu.Unlock()

	if status == statusDone {
		j.emit(Event{Type: "progress", Percent: intPtr(100), Status: "完成"})
	}
	exitVal := exit
	j.emit(Event{
		Type: "done", Status: status,
		Quads: intPtr(quads), NonQuads: intPtr(nonQuads), Vertices: intPtr(vertices),
		Exit: &exitVal, ElapsedMS: elapsed.Milliseconds(), Error: errText,
	})
}

// execOnce runs the CLI once. Exactly one dialect is used per attempt.
func (m *Manager) execOnce(j *Job, withEventsJSON bool) execResult {
	args := []string{
		"--input", j.Input,
		"--output", j.Output,
		"--target-quads", strconv.Itoa(j.Params.TargetQuads),
		"--edge-scaling", formatFloat(j.Params.EdgeScaling),
		"--sharp-edge", formatFloat(j.Params.SharpEdge),
		"--smooth-normal", formatFloat(j.Params.SmoothNormal),
		"--adaptivity", formatFloat(j.Params.Adaptivity),
		"--anisotropy", formatFloat(j.Params.Anisotropy),
	}
	args = append(args, j.Params.ExtraArgs...)
	if withEventsJSON {
		args = append(args, "--events-json")
	}

	cmd := exec.CommandContext(j.ctx, m.cliPath, args...)
	cmd.Dir = m.workDir
	stderrPipe, err := cmd.StderrPipe()
	if err != nil {
		return execResult{exit: -1, startErr: err}
	}
	stdoutPipe, err := cmd.StdoutPipe()
	if err != nil {
		return execResult{exit: -1, startErr: err}
	}
	if err := cmd.Start(); err != nil {
		return execResult{exit: -1, startErr: err}
	}

	var stderrRes, stdoutRes execResult
	var wg sync.WaitGroup
	wg.Add(2)
	go func() { defer wg.Done(); stderrRes = scanStderr(j, stderrPipe, withEventsJSON) }()
	go func() { defer wg.Done(); stdoutRes = scanStdout(j, stdoutPipe) }()
	// Wait only after every read has returned, otherwise Wait closes the pipes
	// underneath the scanners (documented os/exec rule).
	wg.Wait()
	waitErr := cmd.Wait()

	res := stderrRes
	res.stdoutTail = stdoutRes.stdoutTail
	if !res.hasStats && stdoutRes.hasStats {
		res.hasStats = true
		res.quads, res.nonQuads, res.vertices = stdoutRes.quads, stdoutRes.nonQuads, stdoutRes.vertices
	}
	if cmd.ProcessState != nil {
		res.exit = cmd.ProcessState.ExitCode()
	} else if waitErr != nil {
		res.exit = -1
	}
	if waitErr != nil && res.exit == 0 {
		res.exit = -1
	}
	return res
}

// scanStderr consumes one dialect of the CLI's stderr.
func scanStderr(j *Job, r io.Reader, withEventsJSON bool) execResult {
	res := execResult{}
	scanner := bufio.NewScanner(r)
	// A single phase event carries every report line in one JSON object, so the
	// default 64 KiB token limit is not enough.
	scanner.Buffer(make([]byte, 64*1024), 4*1024*1024)

	var indented []string
	lastPercent, lastStatus := -1, ""
	for scanner.Scan() {
		line := strings.TrimRight(scanner.Text(), "")
		if line == "" {
			continue
		}
		if withEventsJSON {
			if ev, ok := parseEventLine(line); ok {
				res.sawStructured = true
				switch ev.Type {
				case "progress":
					percent := 0
					if ev.Percent != nil {
						percent = *ev.Percent
					}
					if percent == lastPercent && ev.Status == lastStatus {
						continue
					}
					lastPercent, lastStatus = percent, ev.Status
					j.emit(Event{Type: "progress", Percent: intPtr(percent), Status: ev.Status})
				case "phase":
					res.phaseLines = append(res.phaseLines, ev.Lines...)
				case "done":
					// Stats are published by run() after the status is final, so
					// the terminal SSE event and GET /api/jobs never disagree.
					if ev.Quads != nil {
						res.quads = *ev.Quads
						res.nonQuads = derefInt(ev.NonQuads)
						res.vertices = derefInt(ev.Vertices)
						res.hasStats = true
					}
				case "error":
					res.errMessage = ev.Message
					j.emit(Event{Type: "log", Line: "CLI: " + ev.Message})
				default:
					j.emit(Event{Type: "log", Line: line})
				}
				continue
			}
			// Mixed output: the CLI may still print plain diagnostics.
			if strings.Contains(line, "unknown option") && strings.Contains(line, "--events-json") {
				res.unknownEvents = true
			}
			pushTail(&res.stderrTail, line)
			j.emit(Event{Type: "log", Line: line})
			continue
		}

		// ---- plain text dialect ----
		if percent, status, ok := parseTextProgress(line); ok {
			if percent != lastPercent || status != lastStatus {
				lastPercent, lastStatus = percent, status
				j.emit(Event{Type: "progress", Percent: intPtr(percent), Status: status})
			}
			continue
		}
		if strings.Contains(line, "unknown option") && strings.Contains(line, "--events-json") {
			res.unknownEvents = true
		}
		if strings.HasPrefix(line, "  ") {
			// Indented lines are the per-stage phase report.
			indented = append(indented, strings.TrimRight(line, " "))
			continue
		}
		pushTail(&res.stderrTail, line)
		j.emit(Event{Type: "log", Line: line})
	}
	if len(indented) > 0 {
		res.phaseLines = append(res.phaseLines, indented...)
	}
	return res
}

func scanStdout(j *Job, r io.Reader) execResult {
	res := execResult{}
	scanner := bufio.NewScanner(r)
	scanner.Buffer(make([]byte, 64*1024), 1*1024*1024)
	for scanner.Scan() {
		line := strings.TrimRight(scanner.Text(), "")
		if line == "" {
			continue
		}
		switch {
		case strings.HasPrefix(line, "Quads: "):
			res.quads, _ = strconv.Atoi(strings.TrimSpace(strings.TrimPrefix(line, "Quads: ")))
			res.hasStats = true
		case strings.HasPrefix(line, "Non-quads: "):
			res.nonQuads, _ = strconv.Atoi(strings.TrimSpace(strings.TrimPrefix(line, "Non-quads: ")))
		case strings.HasPrefix(line, "Vertices: "):
			res.vertices, _ = strconv.Atoi(strings.TrimSpace(strings.TrimPrefix(line, "Vertices: ")))
		}
		pushTail(&res.stdoutTail, line)
		if !strings.HasPrefix(line, "===") {
			j.emit(Event{Type: "log", Line: "stdout: " + line})
		}
	}
	return res
}

// eventLine is the NDJSON schema emitted by autoremesher_cli --events-json.
type eventLine struct {
	SchemaVersion int      `json:"schema_version"`
	Type          string   `json:"type"`
	Percent       *int     `json:"percent"`
	Status        string   `json:"status"`
	Lines         []string `json:"lines"`
	Quads         *int     `json:"quads"`
	NonQuads      *int     `json:"non_quads"`
	Vertices      *int     `json:"vertices"`
	Exit          *int     `json:"exit"`
	Message       string   `json:"message"`
}

func parseEventLine(line string) (eventLine, bool) {
	trimmed := strings.TrimSpace(line)
	if !strings.HasPrefix(trimmed, "{") {
		return eventLine{}, false
	}
	var ev eventLine
	if err := json.Unmarshal([]byte(trimmed), &ev); err != nil {
		return eventLine{}, false
	}
	if ev.Type == "" {
		return eventLine{}, false
	}
	return ev, true
}

func intPtr(v int) *int { return &v }

func derefInt(p *int) int {
	if p == nil {
		return 0
	}
	return *p
}

func pushTail(tail *[]string, line string) {
	*tail = append(*tail, line)
	if len(*tail) > maxTailLines {
		*tail = (*tail)[len(*tail)-maxTailLines:]
	}
}

// countObjFaces is the last-resort stats source: when neither the NDJSON done
// event nor the stdout report is available, count the OBJ we just wrote.
func countObjFaces(path string) (quads, nonQuads, vertices int, ok bool) {
	f, err := os.Open(path)
	if err != nil {
		return 0, 0, 0, false
	}
	defer f.Close()
	if fi, err := f.Stat(); err == nil && fi.Size() > maxObjReadBytes {
		return 0, 0, 0, false
	}
	scanner := bufio.NewScanner(f)
	scanner.Buffer(make([]byte, 64*1024), 4*1024*1024)
	for scanner.Scan() {
		line := scanner.Text()
		switch {
		case strings.HasPrefix(line, "v "):
			vertices++
		case strings.HasPrefix(line, "f "):
			if strings.Count(strings.TrimSpace(line), " ") >= 4 {
				quads++
			} else {
				nonQuads++
			}
		}
	}
	return quads, nonQuads, vertices, vertices > 0
}

func formatFloat(v float64) string {
	return strconv.FormatFloat(v, 'f', -1, 64)
}

func firstLine(s string) string {
	if i := strings.IndexByte(s, byte(10)); i >= 0 {
		return s[:i]
	}
	return s
}
