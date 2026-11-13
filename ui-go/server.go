// HTTP surface of the optional Go front end.
//
// Everything is served from one mux on 127.0.0.1:8787 by default: the embedded
// single page, a small JSON API, and one SSE stream per job. No third-party
// packages, no cgo, no external assets in the page.
package main

import (
	"bufio"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

type Server struct {
	mgr     *Manager
	roots   []string
	web     fs.FS
	started time.Time
	version string
}

func NewServer(mgr *Manager, web fs.FS, roots []string, version string) *Server {
	return &Server{mgr: mgr, web: web, roots: roots, started: time.Now(), version: version}
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	// Go 1.22 patterns: method and wildcard live in the route itself.
	mux.HandleFunc("GET /{$}", s.handleIndex)
	mux.HandleFunc("GET /api/health", s.handleHealth)
	mux.HandleFunc("GET /api/jobs", s.handleListJobs)
	mux.HandleFunc("POST /api/jobs", s.handleCreateJob)
	mux.HandleFunc("GET /api/jobs/{id}/events", s.handleEvents)
	mux.HandleFunc("POST /api/jobs/{id}/cancel", s.handleCancel)
	mux.HandleFunc("GET /api/jobs/{id}/obj", s.handleObj)
	mux.HandleFunc("GET /api/fs", s.handleFS)
	mux.HandleFunc("GET /favicon.ico", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	})
	return withLogging(mux)
}

func withLogging(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		started := time.Now()
		next.ServeHTTP(w, r)
		// The job list is polled every two seconds; keep it out of the log.
		if r.URL.Path != "/api/jobs" || r.Method != http.MethodGet {
			logf("%s %s (%s)", r.Method, r.URL.RequestURI(), time.Since(started).Round(time.Millisecond))
		}
	})
}

func (s *Server) handleIndex(w http.ResponseWriter, r *http.Request) {
	data, err := fs.ReadFile(s.web, "index.html")
	if err != nil {
		http.Error(w, "embedded page missing", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	if _, err := w.Write(data); err != nil {
		logf("write index: %v", err)
	}
}

type healthView struct {
	OK            bool     `json:"ok"`
	Version       string   `json:"version"`
	Name          string   `json:"name"`
	CLIPath       string   `json:"cli_path"`
	CLIFound      bool     `json:"cli_found"`
	EventsJSON    bool     `json:"events_json"`
	Degraded      bool     `json:"degraded"`
	DegradeNote   string   `json:"degrade_note,omitempty"`
	ProbeNote     string   `json:"probe_note,omitempty"`
	Jobs          int      `json:"jobs"`
	UptimeSeconds float64  `json:"uptime_seconds"`
	Roots         []string `json:"roots"`
}

func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	eventsJSON, degradeNote, probeNote := s.mgr.EventsJSON()
	cliPath := s.mgr.cliPath
	found := false
	if cliPath != "" {
		if _, err := os.Stat(cliPath); err == nil {
			found = true
		}
	}
	writeJSON(w, http.StatusOK, healthView{
		OK:            found,
		Version:       s.version,
		Name:          "autoremesher-ui-go",
		CLIPath:       cliPath,
		CLIFound:      found,
		EventsJSON:    eventsJSON,
		Degraded:      !eventsJSON,
		DegradeNote:   degradeNote,
		ProbeNote:     probeNote,
		Jobs:          s.mgr.Count(),
		UptimeSeconds: time.Since(s.started).Seconds(),
		Roots:         s.roots,
	})
}

func (s *Server) handleListJobs(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{
		"jobs":  s.mgr.List(),
		"count": s.mgr.Count(),
	})
}

type createJobRequest struct {
	Input  string `json:"input"`
	Output string `json:"output"`
	paramsRequest
}

func (s *Server) handleCreateJob(w http.ResponseWriter, r *http.Request) {
	var req createJobRequest
	if err := decodeJSON(r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	params, err := req.paramsRequest.normalize()
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	input, err := resolveInput(req.Input)
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	output, err := resolveOutput(req.Output, input)
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if samePath(input, output) {
		writeError(w, http.StatusBadRequest, "input 与 output 不能是同一个文件")
		return
	}
	if _, err := os.Stat(s.mgr.cliPath); err != nil {
		writeError(w, http.StatusServiceUnavailable, "找不到 autoremesher_cli："+s.mgr.cliPath+
			"（设置 AUTOREMESHER_CLI 或用 -cli 指定，或先构建 build/core）")
		return
	}

	job := s.mgr.Start(input, output, params)
	logf("job %s started: %s -> %s (target_quads=%d)", job.ID, input, output, params.TargetQuads)
	writeJSON(w, http.StatusAccepted, map[string]any{
		"id": job.ID, "status": statusRunning, "input": input, "output": output,
	})
}

// handleEvents streams one job as SSE. A late subscriber gets the ring buffer
// first, so a page refresh in the middle of a run still shows the whole history.
func (s *Server) handleEvents(w http.ResponseWriter, r *http.Request) {
	job := s.mgr.Get(r.PathValue("id"))
	if job == nil {
		writeError(w, http.StatusNotFound, "job 不存在")
		return
	}
	flusher, ok := w.(http.Flusher)
	if !ok {
		writeError(w, http.StatusInternalServerError, "当前 ResponseWriter 不支持 flush")
		return
	}

	w.Header().Set("Content-Type", "text/event-stream; charset=utf-8")
	w.Header().Set("Cache-Control", "no-cache, no-transform")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("X-Accel-Buffering", "no")
	w.WriteHeader(http.StatusOK)

	ch, backlog, status := job.subscribe()
	defer job.unsubscribe(ch)

	if err := writeSSE(w, flusher, Event{Type: "state", Status: status}); err != nil {
		return
	}
	for _, ev := range backlog {
		if err := writeSSE(w, flusher, ev); err != nil {
			return
		}
	}
	if job.isTerminal() {
		return
	}

	keepAlive := time.NewTicker(15 * time.Second)
	defer keepAlive.Stop()
	ctx := r.Context()
	for {
		select {
		case <-ctx.Done():
			return
		case <-keepAlive.C:
			if _, err := fmt.Fprint(w, ": ping\n\n"); err != nil {
				return
			}
			flusher.Flush()
		case ev, ok := <-ch:
			if !ok {
				return
			}
			if err := writeSSE(w, flusher, ev); err != nil {
				return
			}
			if ev.Type == "done" {
				return // the terminal event has been delivered
			}
		}
	}
}

func (s *Server) handleCancel(w http.ResponseWriter, r *http.Request) {
	job := s.mgr.Get(r.PathValue("id"))
	if job == nil {
		writeError(w, http.StatusNotFound, "job 不存在")
		return
	}
	if job.isTerminal() {
		writeJSON(w, http.StatusOK, map[string]any{"id": job.ID, "status": "already-finished"})
		return
	}
	job.cancel()
	logf("job %s cancel requested", job.ID)
	writeJSON(w, http.StatusOK, map[string]any{"id": job.ID, "status": "canceling"})
}

func (s *Server) handleObj(w http.ResponseWriter, r *http.Request) {
	job := s.mgr.Get(r.PathValue("id"))
	if job == nil {
		writeError(w, http.StatusNotFound, "job 不存在")
		return
	}
	file, err := os.Open(job.Output)
	if err != nil {
		writeError(w, http.StatusNotFound, "输出文件不存在："+job.Output)
		return
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	if info.Size() > maxObjReadBytes {
		writeError(w, http.StatusRequestEntityTooLarge,
			fmt.Sprintf("OBJ 太大（%d 字节，上限 %d）", info.Size(), maxObjReadBytes))
		return
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.Header().Set("Content-Disposition", "attachment; filename="+strconv.Quote(filepath.Base(job.Output)))
	http.ServeContent(w, r, filepath.Base(job.Output), info.ModTime(), file)
}

type fsEntry struct {
	Name  string `json:"name"`
	Path  string `json:"path"`
	Size  int64  `json:"size"`
	MTime string `json:"mtime"`
}

type fsView struct {
	Dir       string    `json:"dir"`
	Parent    string    `json:"parent,omitempty"`
	Entries   []fsEntry `json:"entries"`
	Roots     []string  `json:"roots"`
	Truncated bool      `json:"truncated"`
}

// handleFS lists .obj files in one directory. It is deliberately restricted to
// the configured roots so the page cannot become a filesystem oracle for the
// whole disk. Job submission is separate and takes an explicit path.
func (s *Server) handleFS(w http.ResponseWriter, r *http.Request) {
	dir := r.URL.Query().Get("dir")
	if strings.TrimSpace(dir) == "" {
		if len(s.roots) == 0 {
			writeError(w, http.StatusInternalServerError, "没有可浏览的根目录")
			return
		}
		dir = s.roots[0]
	}
	abs, err := safeDir(dir, s.roots)
	if err != nil {
		status := http.StatusBadRequest
		if errors.Is(err, errForbiddenPath) {
			status = http.StatusForbidden
		}
		writeError(w, status, err.Error())
		return
	}

	dirents, err := os.ReadDir(abs)
	if err != nil {
		writeError(w, http.StatusBadRequest, "读取目录失败："+err.Error())
		return
	}
	entries := make([]fsEntry, 0, len(dirents))
	truncated := false
	for _, de := range dirents {
		if de.IsDir() || !strings.EqualFold(filepath.Ext(de.Name()), ".obj") {
			continue
		}
		info, err := de.Info()
		if err != nil {
			continue
		}
		entries = append(entries, fsEntry{
			Name:  de.Name(),
			Path:  filepath.Join(abs, de.Name()),
			Size:  info.Size(),
			MTime: info.ModTime().Format(time.RFC3339),
		})
		if len(entries) >= 2000 {
			truncated = true
			break
		}
	}
	sort.Slice(entries, func(i, k int) bool { return entries[i].Name < entries[k].Name })

	view := fsView{Dir: abs, Entries: entries, Roots: s.roots, Truncated: truncated}
	if parent := filepath.Dir(abs); parent != abs && withinRoots(parent, s.roots) {
		view.Parent = parent
	}
	writeJSON(w, http.StatusOK, view)
}

var (
	errForbiddenPath = errors.New("路径不在允许浏览的根目录内")
	errNotDir        = errors.New("不是目录")
)

func safeDir(p string, roots []string) (string, error) {
	if strings.ContainsRune(p, 0) {
		return "", errForbiddenPath
	}
	abs, err := filepath.Abs(p)
	if err != nil {
		return "", errForbiddenPath
	}
	// Resolve symlinks so a link inside a root cannot point outside it.
	real, err := filepath.EvalSymlinks(abs)
	if err != nil {
		return "", fmt.Errorf("目录不可访问：%s", p)
	}
	info, err := os.Stat(real)
	if err != nil {
		return "", fmt.Errorf("目录不可访问：%s", p)
	}
	if !info.IsDir() {
		return "", errNotDir
	}
	if !withinRoots(real, roots) {
		return "", errForbiddenPath
	}
	return real, nil
}

func withinRoots(p string, roots []string) bool {
	for _, root := range roots {
		realRoot, err := filepath.EvalSymlinks(root)
		if err != nil {
			realRoot = root
		}
		if isWithin(p, realRoot) {
			return true
		}
	}
	return false
}

func isWithin(p, root string) bool {
	rel, err := filepath.Rel(root, p)
	if err != nil {
		return false
	}
	if rel == "." {
		return true
	}
	if rel == ".." || strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
		return false
	}
	return !filepath.IsAbs(rel)
}

func resolveInput(p string) (string, error) {
	if strings.TrimSpace(p) == "" {
		return "", errors.New("input 不能为空")
	}
	abs, err := filepath.Abs(p)
	if err != nil {
		return "", fmt.Errorf("input 路径非法：%v", err)
	}
	info, err := os.Stat(abs)
	if err != nil {
		return "", fmt.Errorf("input 不存在：%s", abs)
	}
	if info.IsDir() {
		return "", fmt.Errorf("input 是目录：%s", abs)
	}
	if !strings.EqualFold(filepath.Ext(abs), ".obj") {
		return "", fmt.Errorf("只支持 .obj 输入：%s", abs)
	}
	return abs, nil
}

// resolveOutput defaults to "<input stem>-remesh.obj" next to the input.
func resolveOutput(p, input string) (string, error) {
	if strings.TrimSpace(p) == "" {
		ext := filepath.Ext(input)
		stem := strings.TrimSuffix(filepath.Base(input), ext)
		return filepath.Join(filepath.Dir(input), stem+"-remesh.obj"), nil
	}
	abs, err := filepath.Abs(p)
	if err != nil {
		return "", fmt.Errorf("output 路径非法：%v", err)
	}
	if !strings.EqualFold(filepath.Ext(abs), ".obj") {
		return "", fmt.Errorf("output 必须是 .obj：%s", abs)
	}
	dir := filepath.Dir(abs)
	info, err := os.Stat(dir)
	if err != nil || !info.IsDir() {
		return "", fmt.Errorf("output 目录不存在：%s", dir)
	}
	return abs, nil
}

func samePath(a, b string) bool {
	aa, err1 := filepath.Abs(a)
	bb, err2 := filepath.Abs(b)
	if err1 != nil || err2 != nil {
		return a == b
	}
	return strings.EqualFold(aa, bb)
}

func newJobID() string {
	var buf [6]byte
	if _, err := rand.Read(buf[:]); err != nil {
		return fmt.Sprintf("job-%d", time.Now().UnixNano())
	}
	return fmt.Sprintf("%s-%s", time.Now().Format("150405"), hex.EncodeToString(buf[:]))
}

// injectUnknownField is a diagnostic switch (UI_GO_INJECT_X_FUTURE=1) that adds
// a field this server does not model to every forwarded event. It exists to
// prove forward compatibility in both directions: clients must ignore unknown
// fields, and the server must keep streaming after emitting one.
var injectUnknownField bool

// SetInjectUnknownField turns the diagnostic on. Called once at startup.
func SetInjectUnknownField(on bool) { injectUnknownField = on }

func writeSSE(w io.Writer, f http.Flusher, ev Event) error {
	payload, err := json.Marshal(ev)
	if err != nil {
		return err
	}
	if injectUnknownField {
		var fields map[string]any
		if err := json.Unmarshal(payload, &fields); err == nil {
			fields["x_future"] = 1
			if merged, err := json.Marshal(fields); err == nil {
				payload = merged
			}
		}
	}
	if _, err := fmt.Fprintf(w, "data: %s\n\n", payload); err != nil {
		return err
	}
	f.Flush()
	return nil
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(code)
	enc := json.NewEncoder(w)
	enc.SetEscapeHTML(false)
	_ = enc.Encode(v)
}

func writeError(w http.ResponseWriter, code int, msg string) {
	writeJSON(w, code, map[string]string{"error": msg})
}

func decodeJSON(r *http.Request, dst any) error {
	body, err := io.ReadAll(io.LimitReader(r.Body, 1<<20))
	if err != nil {
		return fmt.Errorf("读取请求体失败：%v", err)
	}
	if len(strings.TrimSpace(string(body))) == 0 {
		return errors.New("请求体为空")
	}
	dec := json.NewDecoder(bufio.NewReader(strings.NewReader(string(body))))
	dec.DisallowUnknownFields()
	if err := dec.Decode(dst); err != nil {
		return fmt.Errorf("请求体不是合法 JSON：%v", err)
	}
	return nil
}
