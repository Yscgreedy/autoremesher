package job

import (
	"strconv"
	"testing"
	"time"
)

func progressLine(v float64) string {
	return `{"type":"progress","value":` + strconv.FormatFloat(v, 'f', 4, 64) + `,"status":"Quad extraction"}`
}

func TestThrottleCapsProgressRate(t *testing.T) {
	p := NewParser()
	start := time.Unix(1700000000, 0)
	const total = 250
	var emitted []Event
	for i := 0; i < total; i++ {
		now := start.Add(time.Duration(i) * 4 * time.Millisecond) // spans one second
		emitted = append(emitted, p.Line(progressLine(float64(i)/total), now)...)
	}
	emitted = append(emitted, p.Flush(start.Add(time.Second))...)

	if len(emitted) == 0 {
		t.Fatal("expected progress events to be forwarded")
	}
	// 25 per second plus one trailing flush.
	if len(emitted) > (ProgressPerSecond + 2) {
		t.Fatalf("forwarded %d progress events in ~1s, want <= %d", len(emitted), ProgressPerSecond+2)
	}
	if len(emitted) < 15 {
		t.Fatalf("forwarded only %d progress events, throttle is too aggressive", len(emitted))
	}

	th := p.Throttler()
	if th.Received != total {
		t.Fatalf("throttler saw %d events, want %d", th.Received, total)
	}
	if th.Emitted != len(emitted) {
		t.Fatalf("throttler emitted %d, parser forwarded %d", th.Emitted, len(emitted))
	}
	if th.Emitted+th.Dropped > total {
		t.Fatalf("emitted(%d)+dropped(%d) exceeds %d inputs", th.Emitted, th.Dropped, total)
	}
	if th.Dropped == 0 {
		t.Fatal("expected throttling to drop intermediate progress events")
	}

	last := -1.0
	for _, ev := range emitted {
		if ev.Type != "progress" {
			t.Fatalf("unexpected event type %q", ev.Type)
		}
		if ev.Value < last {
			t.Fatalf("progress went backwards: %v after %v", ev.Value, last)
		}
		last = ev.Value
	}
	if last < 0.9 {
		t.Fatalf("final forwarded progress %v, want the last value to survive throttling", last)
	}
}

func TestParserClampsProgress(t *testing.T) {
	p := NewParser()
	now := time.Unix(0, 0)
	high := p.Line(`{"type":"progress","value":2.5,"status":"x"}`, now)
	if len(high) != 1 || high[0].Value != 1 {
		t.Fatalf("progress above 1 not clamped: %+v", high)
	}
	low := p.Line(`{"type":"progress","value":-3,"status":"x"}`, now.Add(time.Second))
	if len(low) != 1 || low[0].Value != 0 {
		t.Fatalf("progress below 0 not clamped: %+v", low)
	}
}

func TestParserFlushesPendingProgressOnResult(t *testing.T) {
	p := NewParser()
	t0 := time.Unix(0, 0)
	if evs := p.Line(progressLine(0.10), t0); len(evs) != 1 || evs[0].Value != 0.10 {
		t.Fatalf("first progress should pass: %+v", evs)
	}
	if evs := p.Line(progressLine(0.90), t0.Add(time.Millisecond)); len(evs) != 0 {
		t.Fatalf("second progress inside the window should be throttled: %+v", evs)
	}
	evs := p.Line(`{"type":"result","output":"o.obj","seconds":1.5,"vertices":10,"quads":8,"nonQuads":0,"triangles":4}`, t0.Add(2*time.Millisecond))
	if len(evs) != 2 {
		t.Fatalf("expected pending progress + result, got %d: %+v", len(evs), evs)
	}
	if evs[0].Type != "progress" || evs[0].Value != 0.90 {
		t.Fatalf("pending progress not flushed: %+v", evs[0])
	}
	if evs[1].Type != "result" || evs[1].Quads != 8 {
		t.Fatalf("result not parsed: %+v", evs[1])
	}
	if !p.TerminalSeen() {
		t.Fatal("TerminalSeen should be true after a result")
	}
}

func TestParserHandlesMalformedAndUnknownLines(t *testing.T) {
	p := NewParser()
	now := time.Unix(0, 0)
	evs := p.Line("this is not json", now)
	if len(evs) != 1 || evs[0].Type != "log" || evs[0].Level != "raw" {
		t.Fatalf("malformed line should become a raw log: %+v", evs)
	}
	evs = p.Line(`{"type":"brandnew","payload":1}`, now)
	if len(evs) != 1 || evs[0].Type != "log" {
		t.Fatalf("unknown event type should become a log line: %+v", evs)
	}
	if evs := p.Line("   ", now); len(evs) != 0 {
		t.Fatalf("blank line should be ignored: %+v", evs)
	}
	evs = p.Line(`{"type":"report","lines":["a","b"]}`, now)
	if len(evs) != 1 || len(evs[0].Lines) != 2 {
		t.Fatalf("report not parsed: %+v", evs)
	}
}
