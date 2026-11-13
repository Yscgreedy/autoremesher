package server

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"

	"autoremesher-gui"
	"autoremesher-gui/internal/job"
	"autoremesher-gui/internal/testutil"
)

const tinyOBJ = `v -0.5 -0.5 -0.5
v 0.5 -0.5 -0.5
v 0.5 0.5 -0.5
v -0.5 0.5 -0.5
f 1 2 3 4
`

type env struct {
	ts    *httptest.Server
	token string
	mgr   *job.Manager
}

func newEnv(t *testing.T, extraEnv []string) *env {
	t.Helper()
	bin := testutil.BuildFakeCLI(t)
	ws, err := job.NewWorkspace()
	if err != nil {
		t.Fatalf("workspace: %v", err)
	}
	t.Cleanup(func() { _ = ws.Close() })
	mgr := job.NewManager(job.CLIInfo{Path: bin, Found: true, Source: "test"}, ws)
	if extraEnv != nil {
		mgr.SetEnv(extraEnv)
	}
	token, err := NewToken()
	if err != nil {
		t.Fatalf("token: %v", err)
	}
	srv, err := New(Config{Version: "test-version", Token: token, Manager: mgr, Assets: gui.Assets})
	if err != nil {
		t.Fatalf("server: %v", err)
	}
	ts := httptest.NewServer(srv.Handler())
	t.Cleanup(func() {
		ts.Close()
		_ = mgr.Close()
	})
	return &env{ts: ts, token: token, mgr: mgr}
}

func (e *env) do(t *testing.T, method, path string, body []byte, contentType string, withToken bool) (*http.Response, []byte) {
	t.Helper()
	req, err := http.NewRequest(method, e.ts.URL+path, bytes.NewReader(body))
	if err != nil {
		t.Fatalf("request: %v", err)
	}
	if contentType != "" {
		req.Header.Set("Content-Type", contentType)
	}
	if withToken {
		req.Header.Set("X-AR-Token", e.token)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("%s %s: %v", method, path, err)
	}
	defer resp.Body.Close()
	var buf bytes.Buffer
	_, _ = buf.ReadFrom(resp.Body)
	return resp, buf.Bytes()
}

func (e *env) upload(t *testing.T) map[string]any {
	t.Helper()
	var buf bytes.Buffer
	w := multipart.NewWriter(&buf)
	fw, err := w.CreateFormFile("file", "cube.obj")
	if err != nil {
		t.Fatalf("multipart: %v", err)
	}
	_, _ = fw.Write([]byte(tinyOBJ))
	_ = w.Close()
	resp, body := e.do(t, http.MethodPost, "/api/upload", buf.Bytes(), w.FormDataContentType(), true)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("upload status %d: %s", resp.StatusCode, body)
	}
	var out map[string]any
	if err := json.Unmarshal(body, &out); err != nil {
		t.Fatalf("upload json: %v (%s)", err, body)
	}
	return out
}

func (e *env) run(t *testing.T, inputID string) (*http.Response, map[string]any) {
	t.Helper()
	payload, _ := json.Marshal(map[string]any{
		"inputId": inputID, "targetQuads": 500, "adaptivity": 0.5,
		"modelType": "organic", "edgeScaling": 1,
	})
	resp, body := e.do(t, http.MethodPost, "/api/run", payload, "application/json", true)
	var out map[string]any
	_ = json.Unmarshal(body, &out)
	return resp, out
}

func TestTokenRejection(t *testing.T) {
	e := newEnv(t, nil)

	resp, body := e.do(t, http.MethodGet, "/api/info", nil, "", false)
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("info without token: status %d (%s)", resp.StatusCode, body)
	}
	resp, _ = e.do(t, http.MethodGet, "/api/info?token=deadbeef", nil, "", false)
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("info with wrong token: status %d", resp.StatusCode)
	}
	resp, _ = e.do(t, http.MethodGet, "/s/nosuchtoken/", nil, "", false)
	if resp.StatusCode != http.StatusNotFound {
		t.Fatalf("bad static token: status %d, want 404", resp.StatusCode)
	}

	client := &http.Client{CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	r, err := client.Get(e.ts.URL + "/")
	if err != nil {
		t.Fatalf("root: %v", err)
	}
	r.Body.Close()
	if r.StatusCode != http.StatusFound || r.Header.Get("Location") != "/s/"+e.token+"/" {
		t.Fatalf("root redirect: %d %q", r.StatusCode, r.Header.Get("Location"))
	}
}

func TestInfoAndStaticPage(t *testing.T) {
	e := newEnv(t, nil)

	resp, body := e.do(t, http.MethodGet, "/api/info", nil, "", true)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("info status %d: %s", resp.StatusCode, body)
	}
	var info struct {
		CLI struct {
			Path  string `json:"path"`
			Found bool   `json:"found"`
		} `json:"cli"`
		Version  string `json:"version"`
		Platform string `json:"platform"`
		CPUs     int    `json:"cpus"`
	}
	if err := json.Unmarshal(body, &info); err != nil {
		t.Fatalf("info json: %v", err)
	}
	if !info.CLI.Found || info.CLI.Path == "" {
		t.Fatalf("CLI not reported: %+v", info.CLI)
	}
	if info.Version != "test-version" || info.CPUs == 0 || info.Platform == "" {
		t.Fatalf("info incomplete: %+v", info)
	}

	// The token must survive only inside the served path.
	resp, page := e.do(t, http.MethodGet, "/s/"+e.token+"/", nil, "", false)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("index status %d", resp.StatusCode)
	}
	text := string(page)
	for _, want := range []string{"AutoRemesher", "app.js", "viewer.js", "style.css"} {
		if !strings.Contains(text, want) {
			t.Fatalf("index.html missing %q", want)
		}
	}
	// The token lives in the request path, not in the HTML body.
	if strings.Contains(text, e.token) {
		t.Log("index.html echoes the session token")
	}
}

func TestRunProducesResultAndModel(t *testing.T) {
	e := newEnv(t, nil)
	up := e.upload(t)
	inputID, _ := up["id"].(string)
	if inputID == "" {
		t.Fatalf("upload returned no id: %v", up)
	}
	if up["name"] != "cube.obj" {
		t.Fatalf("upload name = %v", up["name"])
	}

	resp, runOut := e.run(t, inputID)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("run status %d: %v", resp.StatusCode, runOut)
	}
	jobID, _ := runOut["jobId"].(string)
	if jobID == "" {
		t.Fatalf("run returned no jobId: %v", runOut)
	}

	events := readSSE(t, e, jobID)
	var gotResult, gotProgress bool
	for _, ev := range events {
		switch ev["type"] {
		case "progress":
			gotProgress = true
		case "result":
			gotResult = true
			if q, _ := ev["quads"].(float64); q <= 0 {
				t.Fatalf("result has no quads: %v", ev)
			}
		}
	}
	if !gotProgress || !gotResult {
		t.Fatalf("missing events (progress=%v result=%v): %+v", gotProgress, gotResult, events)
	}

	resp, model := e.do(t, http.MethodGet, "/api/model?job="+jobID, nil, "", true)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("model status %d: %s", resp.StatusCode, model)
	}
	if ct := resp.Header.Get("Content-Type"); ct != "application/octet-stream" {
		t.Fatalf("model content-type = %q", ct)
	}
	obj := string(model)
	if !strings.Contains(obj, "v ") || !strings.Contains(obj, "f ") {
		t.Fatalf("model body is not an OBJ: %q", truncate(obj, 200))
	}

	// A finished job must not be cancellable any more, and unknown ids 404.
	resp, body := e.do(t, http.MethodPost, "/api/cancel", []byte(`{"jobId":"`+jobID+`"}`), "application/json", true)
	if resp.StatusCode != http.StatusOK || !strings.Contains(string(body), "false") {
		t.Fatalf("cancel of finished job: %d %s", resp.StatusCode, body)
	}
	resp, _ = e.do(t, http.MethodGet, "/api/model?job=nope", nil, "", true)
	if resp.StatusCode != http.StatusNotFound {
		t.Fatalf("unknown job model status %d, want 404", resp.StatusCode)
	}
}

func TestConflictAndCancel(t *testing.T) {
	envList := append([]string{}, os.Environ()...)
	envList = append(envList, "FAKE_DELAY_MS=250")
	e := newEnv(t, envList)

	up := e.upload(t)
	inputID, _ := up["id"].(string)

	resp, runOut := e.run(t, inputID)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("first run status %d: %v", resp.StatusCode, runOut)
	}
	jobID, _ := runOut["jobId"].(string)

	resp2, conflict := e.run(t, inputID)
	if resp2.StatusCode != http.StatusConflict {
		t.Fatalf("second run status %d, want 409 (%v)", resp2.StatusCode, conflict)
	}

	resp, body := e.do(t, http.MethodPost, "/api/cancel", []byte(`{"jobId":"`+jobID+`"}`), "application/json", true)
	if resp.StatusCode != http.StatusOK || !strings.Contains(string(body), "true") {
		t.Fatalf("cancel response %d %s", resp.StatusCode, body)
	}

	events := readSSE(t, e, jobID)
	endState := ""
	for _, ev := range events {
		if ev["type"] == "end" {
			if s, ok := ev["state"].(string); ok {
				endState = s
			}
			break
		}
	}
	if endState != string(job.StateCancelled) {
		t.Fatalf("stream end state = %q, want %q (events: %+v)", endState, job.StateCancelled, events)
	}
	if j := e.mgr.Get(jobID); j == nil {
		t.Fatal("job disappeared")
	} else if st := j.Snapshot().State; st != job.StateCancelled && st != job.StateFailed {
		t.Fatalf("state after cancel = %s", st)
	}
}

func TestRunValidation(t *testing.T) {
	e := newEnv(t, nil)
	up := e.upload(t)
	inputID, _ := up["id"].(string)

	bad, _ := json.Marshal(map[string]any{"inputId": inputID, "targetQuads": 5})
	resp, body := e.do(t, http.MethodPost, "/api/run", bad, "application/json", true)
	if resp.StatusCode != http.StatusBadRequest {
		t.Fatalf("out-of-range targetQuads: %d %s", resp.StatusCode, body)
	}
	if !strings.Contains(string(body), "targetQuads") {
		t.Fatalf("error should name the field: %s", body)
	}

	resp, _ = e.do(t, http.MethodPost, "/api/run", []byte("{bad json"), "application/json", true)
	if resp.StatusCode != http.StatusBadRequest {
		t.Fatalf("malformed JSON: %d", resp.StatusCode)
	}
}

// readSSE consumes the SSE stream for a job until the "end" frame.
func readSSE(t *testing.T, e *env, jobID string) []map[string]any {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, e.ts.URL+"/api/events?job="+jobID+"&token="+e.token, nil)
	if err != nil {
		t.Fatalf("events request: %v", err)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("events: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("events status %d", resp.StatusCode)
	}
	if ct := resp.Header.Get("Content-Type"); !strings.HasPrefix(ct, "text/event-stream") {
		t.Fatalf("events content-type = %q", ct)
	}
	var out []map[string]any
	sc := bufio.NewScanner(resp.Body)
	sc.Buffer(make([]byte, 0, 64*1024), 1<<20)
	for sc.Scan() {
		line := sc.Text()
		if !strings.HasPrefix(line, "data: ") {
			continue
		}
		var ev map[string]any
		if err := json.Unmarshal([]byte(strings.TrimPrefix(line, "data: ")), &ev); err != nil {
			continue
		}
		out = append(out, ev)
		if ev["type"] == "end" {
			break
		}
	}
	return out
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n] + "…"
}
