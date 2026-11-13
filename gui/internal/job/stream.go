package job

import (
	"encoding/json"
	"strings"
	"sync"
	"time"
)

// Event is one normalised line of the CLI JSON-lines protocol plus the small
// number of synthetic events the GUI adds (stderr log lines, "end").
type Event struct {
	Type        string   `json:"type"`
	Value       float64  `json:"value,omitempty"`
	Status      string   `json:"status,omitempty"`
	Level       string   `json:"level,omitempty"`
	Message     string   `json:"message,omitempty"`
	Lines       []string `json:"lines,omitempty"`
	Output      string   `json:"output,omitempty"`
	Seconds     float64  `json:"seconds,omitempty"`
	Vertices    int      `json:"vertices,omitempty"`
	Quads       int      `json:"quads,omitempty"`
	NonQuads    int      `json:"nonQuads,omitempty"`
	Triangles   int      `json:"triangles,omitempty"`
	Version     string   `json:"version,omitempty"`
	Input       string   `json:"input,omitempty"`
	TargetQuads int      `json:"targetQuads,omitempty"`
	At          string   `json:"at,omitempty"`
}

// Terminal reports whether the event ends the stream.
func (e Event) Terminal() bool {
	return e.Type == "result" || e.Type == "error"
}

// Throttler rate limits progress events to at most 25 per second while keeping
// the most recent dropped value so nothing is lost at the end of the run.
type Throttler struct {
	interval time.Duration
	last     time.Time
	started  bool
	pending  *Event
	Received int
	Emitted  int
	Dropped  int
}

// ProgressPerSecond is the contract limit for progress forwarding.
const ProgressPerSecond = 25

// NewThrottler returns a throttler limited to ProgressPerSecond events.
func NewThrottler() *Throttler {
	return &Throttler{interval: time.Second / ProgressPerSecond}
}

// Progress offers a progress event; it returns the events to forward (zero or
// one). now is injected so tests are deterministic.
func (t *Throttler) Progress(now time.Time, ev Event) []Event {
	t.Received++
	if !t.started || now.Sub(t.last) >= t.interval {
		t.started = true
		t.last = now
		t.pending = nil
		t.Emitted++
		return []Event{ev}
	}
	if t.pending != nil {
		// The previously queued value is discarded; the new one takes its
		// place so the newest progress always survives the window.
		t.Dropped++
	}
	cp := ev
	t.pending = &cp
	return nil
}

// Flush emits the most recent throttled progress event, if any.
func (t *Throttler) Flush(now time.Time) []Event {
	if t.pending == nil {
		return nil
	}
	ev := *t.pending
	t.pending = nil
	t.last = now
	t.Emitted++
	return []Event{ev}
}

// Parser turns CLI stdout lines into events.
type Parser struct {
	mu           sync.Mutex
	th           *Throttler
	seenTerminal bool
	seenStart    bool
}

// NewParser returns a parser with a fresh progress throttler.
func NewParser() *Parser {
	return &Parser{th: NewThrottler()}
}

// Throttler exposes the progress throttler (statistics for tests).
func (p *Parser) Throttler() *Throttler { return p.th }

// TerminalSeen reports whether a result or error event was parsed.
func (p *Parser) TerminalSeen() bool {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.seenTerminal
}

// Line parses one stdout line and returns the events to forward.
func (p *Parser) Line(line string, now time.Time) []Event {
	p.mu.Lock()
	defer p.mu.Unlock()

	line = strings.TrimRight(line, "")
	if strings.TrimSpace(line) == "" {
		return nil
	}

	var ev Event
	if err := json.Unmarshal([]byte(line), &ev); err != nil || ev.Type == "" {
		return []Event{{Type: "log", Level: "raw", Message: line}}
	}
	if ev.Type == "start" {
		p.seenStart = true
	}
	switch ev.Type {
	case "progress":
		if ev.Value < 0 {
			ev.Value = 0
		}
		if ev.Value > 1 {
			ev.Value = 1
		}
		return p.th.Progress(now, ev)
	case "result", "error":
		p.seenTerminal = true
		out := p.th.Flush(now)
		return append(out, ev)
	case "log", "report", "start":
		return []Event{ev}
	default:
		// Unknown event kinds are preserved as raw log lines so the UI can
		// still show diagnostics from a newer CLI.
		return []Event{{Type: "log", Level: "raw", Message: line}}
	}
}

// Flush emits any throttled progress event; call it when stdout closes.
func (p *Parser) Flush(now time.Time) []Event {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.th.Flush(now)
}
