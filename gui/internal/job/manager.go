package job

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"strings"
	"sync"
	"time"
)

// State is the lifecycle of one remesh job.
type State string

const (
	StateRunning   State = "running"
	StateDone      State = "done"
	StateFailed    State = "failed"
	StateCancelled State = "cancelled"
)

var (
	ErrBusy         = errors.New("another remesh job is already running")
	ErrUnknownJob   = errors.New("unknown job")
	ErrUnknownInput = errors.New("unknown inputId: upload the model again")
	ErrNoCLI        = errors.New("autoremesher CLI executable not found")
)

const (
	// MaxLogTail is how many events are kept for late/reconnecting browsers.
	MaxLogTail = 2000
	subBuffer  = 512
	scanBuffer = 1 << 20
)

// DefaultMaxJobs is how many finished jobs the manager keeps in memory. Every
// retained job also retains up to MaxLogTail events, so an unbounded map is an
// unbounded leak in a long-lived session. --max-jobs overrides it.
const DefaultMaxJobs = 32

// Result mirrors the CLI "result" event.
type Result struct {
	Output    string  `json:"output"`
	Seconds   float64 `json:"seconds"`
	Vertices  int     `json:"vertices"`
	Quads     int     `json:"quads"`
	NonQuads  int     `json:"nonQuads"`
	Triangles int     `json:"triangles"`
}

// Snapshot is a point in time view of a job.
type Snapshot struct {
	ID       string   `json:"id"`
	State    State    `json:"state"`
	Progress float64  `json:"progress"`
	Status   string   `json:"status"`
	Error    string   `json:"error,omitempty"`
	Result   *Result  `json:"result,omitempty"`
	Started  string   `json:"started"`
	Elapsed  float64  `json:"elapsedSeconds"`
	Command  []string `json:"command"`
	Input    string   `json:"input"`
	Output   string   `json:"output"`
}

// Job is a single CLI invocation.
type Job struct {
	mu         sync.Mutex
	id         string
	params     Params
	inputPath  string
	outputPath string
	command    []string
	cmd        *exec.Cmd
	state      State
	progress   float64
	status     string
	errMsg     string
	result     *Result
	started    time.Time
	finished   time.Time
	history    []Event
	subs       map[chan Event]struct{}
	cancelling bool
	done       chan struct{}
	clock      func() time.Time
}

func newJob(id string, p Params, inputPath, outputPath string, command []string, clock func() time.Time) *Job {
	return &Job{
		id:         id,
		params:     p,
		inputPath:  inputPath,
		outputPath: outputPath,
		command:    command,
		state:      StateRunning,
		status:     "starting",
		started:    clock(),
		subs:       map[chan Event]struct{}{},
		done:       make(chan struct{}),
		clock:      clock,
	}
}

// ID returns the job identifier.
func (j *Job) ID() string { return j.id }

// OutputPath returns where the CLI writes the remeshed OBJ.
func (j *Job) OutputPath() string { return j.outputPath }

// Done is closed when the job reaches a terminal state.
func (j *Job) Done() <-chan struct{} { return j.done }

// Snapshot reports the current state.
func (j *Job) Snapshot() Snapshot {
	j.mu.Lock()
	defer j.mu.Unlock()
	end := j.finished
	if j.state == StateRunning {
		end = j.clock()
	}
	s := Snapshot{
		ID:       j.id,
		State:    j.state,
		Progress: j.progress,
		Status:   j.status,
		Error:    j.errMsg,
		Result:   j.result,
		Started:  j.started.Format(time.RFC3339Nano),
		Command:  append([]string(nil), j.command...),
		Input:    j.inputPath,
		Output:   j.outputPath,
	}
	if !end.IsZero() {
		s.Elapsed = end.Sub(j.started).Seconds()
	}
	return s
}

// Subscribe replays the retained event tail and then streams live events.
// The returned stop function must always be called.
func (j *Job) Subscribe() (<-chan Event, func()) {
	j.mu.Lock()
	defer j.mu.Unlock()
	replay := append([]Event(nil), j.history...)
	ch := make(chan Event, len(replay)+subBuffer)
	for _, ev := range replay {
		ch <- ev
	}
	if j.state != StateRunning {
		close(ch)
		return ch, func() {}
	}
	j.subs[ch] = struct{}{}
	stop := func() {
		j.mu.Lock()
		defer j.mu.Unlock()
		if _, ok := j.subs[ch]; ok {
			delete(j.subs, ch)
			close(ch)
		}
	}
	return ch, stop
}

func (j *Job) publish(ev Event) {
	if ev.Type == "log" || ev.Type == "progress" || ev.Type == "report" {
		ev.At = j.clock().Format("15:04:05.000")
	}
	j.mu.Lock()
	j.history = append(j.history, ev)
	if len(j.history) > MaxLogTail {
		trimmed := make([]Event, MaxLogTail)
		copy(trimmed, j.history[len(j.history)-MaxLogTail:])
		j.history = trimmed
	}
	switch ev.Type {
	case "progress":
		if ev.Value > j.progress {
			j.progress = ev.Value
		}
		if ev.Status != "" {
			j.status = ev.Status
		}
	case "start":
		if ev.Status != "" {
			j.status = ev.Status
		}
	case "result":
		j.progress = 1
		j.status = "done"
		j.result = &Result{
			Output:    ev.Output,
			Seconds:   ev.Seconds,
			Vertices:  ev.Vertices,
			Quads:     ev.Quads,
			NonQuads:  ev.NonQuads,
			Triangles: ev.Triangles,
		}
	case "error":
		j.errMsg = ev.Message
		j.status = "failed"
	}
	subs := make([]chan Event, 0, len(j.subs))
	for ch := range j.subs {
		subs = append(subs, ch)
	}
	j.mu.Unlock()
	for _, ch := range subs {
		select {
		case ch <- ev:
		default:
		}
	}
}

func (j *Job) finish(state State, errMsg string) {
	j.mu.Lock()
	if j.state != StateRunning {
		j.mu.Unlock()
		return
	}
	j.state = state
	j.finished = j.clock()
	if errMsg != "" && j.errMsg == "" {
		j.errMsg = errMsg
	}
	if state == StateFailed && j.errMsg == "" {
		j.errMsg = errMsg
	}
	if state == StateCancelled {
		j.errMsg = "cancelled"
		j.status = "cancelled"
	}
	if state == StateFailed && j.status == "running" {
		j.status = "failed"
	}
	subs := make([]chan Event, 0, len(j.subs))
	for ch := range j.subs {
		subs = append(subs, ch)
	}
	j.subs = map[chan Event]struct{}{}
	close(j.done)
	j.mu.Unlock()
	for _, ch := range subs {
		close(ch)
	}
}

func (j *Job) isRunning() bool {
	j.mu.Lock()
	defer j.mu.Unlock()
	return j.state == StateRunning
}

// Cancel kills the process tree. It is idempotent.
func (j *Job) Cancel() error {
	j.mu.Lock()
	if j.state != StateRunning {
		j.mu.Unlock()
		return nil
	}
	j.cancelling = true
	j.status = "cancelling"
	cmd := j.cmd
	j.mu.Unlock()
	if cmd == nil || cmd.Process == nil {
		return nil
	}
	return killTree(cmd)
}

func (j *Job) cancelled() bool {
	j.mu.Lock()
	defer j.mu.Unlock()
	return j.cancelling
}

func (j *Job) setCmd(cmd *exec.Cmd) {
	j.mu.Lock()
	j.cmd = cmd
	j.mu.Unlock()
}

// release drops the retained event history of a job that left the manager. It
// is the memory half of eviction: the Job object can still be read by a handler
// that holds it, but it no longer pins up to MaxLogTail events.
func (j *Job) release() {
	j.mu.Lock()
	j.history = nil
	j.subs = map[chan Event]struct{}{}
	j.mu.Unlock()
}

// Manager owns the CLI discovery, the temp workspace and at most one running job.
type Manager struct {
	mu      sync.Mutex
	cli     CLIInfo
	ws      *Workspace
	env     []string
	jobs    map[string]*Job
	order   []string // job ids in creation order, used by the retention cap
	current *Job
	clock   func() time.Time
	maxJobs int
}

// NewManager builds a manager over an existing workspace.
func NewManager(cli CLIInfo, ws *Workspace) *Manager {
	return &Manager{
		cli:     cli,
		ws:      ws,
		jobs:    map[string]*Job{},
		order:   []string{},
		maxJobs: DefaultMaxJobs,
		clock:   time.Now,
	}
}

// Workspace exposes the scratch area (uploads, output paths).
func (m *Manager) Workspace() *Workspace { return m.ws }

// CLI returns the current CLI info.
func (m *Manager) CLI() CLIInfo {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.cli
}

// SetCLI replaces the CLI info (used after a late discovery / in tests).
func (m *Manager) SetCLI(info CLIInfo) {
	m.mu.Lock()
	m.cli = info
	m.mu.Unlock()
}

// Rediscover re-runs discovery, keeping the previously explicit path.
func (m *Manager) Rediscover(explicit string) CLIInfo {
	info := Discover(explicit)
	if info.Found {
		m.SetCLI(info)
	}
	return info
}

// SetEnv overrides the environment for spawned CLI processes (tests).
func (m *Manager) SetEnv(env []string) {
	m.mu.Lock()
	m.env = env
	m.mu.Unlock()
}

// SetClock overrides time.Now (tests).
func (m *Manager) SetClock(clock func() time.Time) {
	m.mu.Lock()
	m.clock = clock
	m.mu.Unlock()
}

// MaxJobs reports the retention cap.
func (m *Manager) MaxJobs() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.maxJobs
}

// SetMaxJobs changes the retention cap and prunes immediately. A value below 1
// falls back to DefaultMaxJobs. Running jobs are never dropped, so the live
// count can briefly exceed the cap while a run is in flight.
func (m *Manager) SetMaxJobs(n int) {
	if n < 1 {
		n = DefaultMaxJobs
	}
	m.mu.Lock()
	m.maxJobs = n
	victims := m.pruneLocked()
	m.mu.Unlock()
	m.evict(victims)
}

// pruneLocked drops the oldest finished jobs until the retained count fits the
// cap. A job that is still running is never a candidate, and an id whose job is
// already gone is forgotten instead of counted. The caller holds m.mu and must
// run evict on the result after unlocking, because freeing a job touches the
// filesystem.
func (m *Manager) pruneLocked() []*Job {
	var victims []*Job
	limit := m.maxJobs
	if limit < 1 {
		limit = DefaultMaxJobs
	}
	for len(m.order) > limit {
		idx := -1
		for i, id := range m.order {
			j := m.jobs[id]
			if j == nil || !j.isRunning() {
				idx = i
				break
			}
		}
		if idx < 0 {
			return victims // every retained job is still running
		}
		id := m.order[idx]
		m.order = append(m.order[:idx], m.order[idx+1:]...)
		j := m.jobs[id]
		if j == nil {
			continue
		}
		delete(m.jobs, id)
		if m.current == j {
			m.current = nil
		}
		victims = append(victims, j)
	}
	return victims
}

// evict releases the memory an evicted job held and removes its result file
// through the workspace, the same path Close uses. Errors are ignored on
// purpose: the file may already be gone, and eviction must never fail a request.
func (m *Manager) evict(victims []*Job) {
	for _, v := range victims {
		v.release()
		if m.ws != nil {
			_ = m.ws.RemoveOutput(v.ID())
		}
	}
}

// Get returns a job by id.
func (m *Manager) Get(id string) *Job {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.jobs[id]
}

// Current returns the most recent job, if any.
func (m *Manager) Current() *Job {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.current
}

// Start validates params, allocates the workspace output and launches the CLI
// in the background. It returns ErrBusy when another job is still running.
func (m *Manager) Start(p Params) (*Job, error) {
	if err := p.Validate(); err != nil {
		return nil, err
	}
	in, ok := m.ws.Get(p.InputID)
	if !ok {
		return nil, ErrUnknownInput
	}
	m.mu.Lock()
	if m.current != nil && m.current.isRunning() {
		m.mu.Unlock()
		return nil, ErrBusy
	}
	if !m.cli.Found {
		m.mu.Unlock()
		return nil, ErrNoCLI
	}
	id := NewID()
	out := m.ws.OutputPath(id)
	cmdline := append([]string{m.cli.Path}, p.Args(in.Path, out)...)
	j := newJob(id, p, in.Path, out, cmdline, m.clock)
	m.jobs[id] = j
	m.order = append(m.order, id)
	m.current = j
	victims := m.pruneLocked()
	m.mu.Unlock()

	// Freeing a victim touches the filesystem, so it happens outside the lock
	// and before the new job starts producing events.
	m.evict(victims)
	go m.run(j)
	return j, nil
}

func (m *Manager) envSnapshot() []string {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.env == nil {
		return os.Environ()
	}
	return m.env
}

func (m *Manager) run(j *Job) {
	cmd := exec.Command(j.command[0], j.command[1:]...)
	cmd.Env = m.envSnapshot()
	setProcAttr(cmd)

	stdout, err := cmd.StdoutPipe()
	if err != nil {
		j.publish(Event{Type: "error", Message: fmt.Sprintf("stdout pipe: %v", err)})
		j.finish(StateFailed, fmt.Sprintf("stdout pipe: %v", err))
		return
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		j.publish(Event{Type: "error", Message: fmt.Sprintf("stderr pipe: %v", err)})
		j.finish(StateFailed, fmt.Sprintf("stderr pipe: %v", err))
		return
	}

	j.setCmd(cmd)
	if err := cmd.Start(); err != nil {
		msg := fmt.Sprintf("failed to start %s: %v", j.command[0], err)
		j.publish(Event{Type: "error", Message: msg})
		j.finish(StateFailed, msg)
		return
	}
	j.publish(Event{Type: "log", Level: "info", Message: strings.Join(j.command, " ")})

	parser := NewParser()
	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		sc := bufio.NewScanner(stderr)
		sc.Buffer(make([]byte, 0, 64*1024), scanBuffer)
		for sc.Scan() {
			line := strings.TrimRight(sc.Text(), "")
			if strings.TrimSpace(line) == "" {
				continue
			}
			j.publish(Event{Type: "log", Level: "stderr", Message: line})
		}
	}()

	sc := bufio.NewScanner(stdout)
	sc.Buffer(make([]byte, 0, 64*1024), scanBuffer)
	for sc.Scan() {
		for _, ev := range parser.Line(sc.Text(), j.clock()) {
			j.publish(ev)
		}
		if j.cancelled() {
			break
		}
	}
	scanErr := sc.Err()
	for _, ev := range parser.Flush(j.clock()) {
		j.publish(ev)
	}
	wg.Wait()
	waitErr := cmd.Wait()

	if j.cancelled() {
		j.publish(Event{Type: "log", Level: "warn", Message: "job cancelled by user"})
		j.finish(StateCancelled, "cancelled")
		return
	}
	snap := j.Snapshot()
	switch {
	case scanErr != nil:
		msg := fmt.Sprintf("reading CLI stdout: %v", scanErr)
		j.publish(Event{Type: "error", Message: msg})
		j.finish(StateFailed, msg)
	case snap.Result != nil:
		j.finish(StateDone, "")
	case waitErr != nil:
		code := -1
		var ee *exec.ExitError
		if errors.As(waitErr, &ee) {
			code = ee.ExitCode()
		}
		msg := fmt.Sprintf("CLI exited with status %d", code)
		if snap.Error != "" {
			msg = snap.Error
		}
		j.publish(Event{Type: "error", Message: msg})
		j.finish(StateFailed, msg)
	case snap.Error != "":
		j.finish(StateFailed, snap.Error)
	case !parser.TerminalSeen():
		msg := "CLI stream ended without a result event"
		j.publish(Event{Type: "error", Message: msg})
		j.finish(StateFailed, msg)
	default:
		j.finish(StateFailed, "remeshing failed")
	}
}

// Cancel kills a job's process tree.
func (m *Manager) Cancel(id string) error {
	j := m.Get(id)
	if j == nil {
		return ErrUnknownJob
	}
	return j.Cancel()
}

// Close terminates a running job and removes the workspace.
func (m *Manager) Close() error {
	m.mu.Lock()
	current := m.current
	ws := m.ws
	m.mu.Unlock()
	if current != nil && current.isRunning() {
		_ = current.Cancel()
		select {
		case <-current.Done():
		case <-time.After(5 * time.Second):
		}
	}
	if ws != nil {
		return ws.Close()
	}
	return nil
}
