package server

import (
	"encoding/json"
	"net/http"
	"time"
)

// handleEvents streams the job as Server-Sent Events. Every frame is
// "data: {json}\n\n" and the JSON always carries a "type" field; the stream
// ends with {"type":"end"} and the connection closes. A disconnected browser
// never cancels the job: only POST /api/cancel does.
func (s *Server) handleEvents(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeError(w, http.StatusMethodNotAllowed, "use GET")
		return
	}
	j := s.mgr.Get(r.URL.Query().Get("job"))
	if j == nil {
		writeError(w, http.StatusNotFound, "unknown job")
		return
	}
	flusher, ok := w.(http.Flusher)
	if !ok {
		writeError(w, http.StatusInternalServerError, "streaming unsupported")
		return
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("X-Accel-Buffering", "no")
	w.WriteHeader(http.StatusOK)
	flusher.Flush()

	events, stop := j.Subscribe()
	defer stop()

	send := func(v any) bool {
		b, err := json.Marshal(v)
		if err != nil {
			return true
		}
		if _, err := w.Write([]byte("data: ")); err != nil {
			return false
		}
		if _, err := w.Write(b); err != nil {
			return false
		}
		if _, err := w.Write([]byte("\n\n")); err != nil {
			return false
		}
		flusher.Flush()
		return true
	}

	ticker := time.NewTicker(15 * time.Second)
	defer ticker.Stop()
	for {
		select {
		case ev, ok := <-events:
			if !ok {
				send(map[string]any{"type": "end", "job": j.ID(), "state": j.Snapshot().State})
				return
			}
			if !send(ev) {
				return
			}
		case <-ticker.C:
			if _, err := w.Write([]byte(": ping\n\n")); err != nil {
				return
			}
			flusher.Flush()
		case <-r.Context().Done():
			return
		}
	}
}
