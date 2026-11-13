package server

import (
	"errors"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"strings"

	"autoremesher-gui/internal/job"
)

const maxUploadBytes = 512 << 20

type infoResponse struct {
	CLI      job.CLIInfo `json:"cli"`
	Version  string      `json:"version"`
	Platform string      `json:"platform"`
	CPUs     int         `json:"cpus"`
	Token    bool        `json:"tokenOk"`
}

func (s *Server) handleInfo(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		writeError(w, http.StatusMethodNotAllowed, "use GET")
		return
	}
	cli := s.mgr.CLI()
	if !cli.Found {
		// A late build of the CLI next to the GUI should be picked up without
		// restarting the server.
		if again := s.mgr.Rediscover(s.explicitCLI); again.Found {
			cli = again
		}
	}
	writeJSON(w, http.StatusOK, infoResponse{
		CLI:      cli,
		Version:  s.version,
		Platform: runtime.GOOS + "/" + runtime.GOARCH,
		CPUs:     runtime.NumCPU(),
		Token:    true,
	})
}

func (s *Server) handleUpload(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeError(w, http.StatusMethodNotAllowed, "use POST")
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, maxUploadBytes)
	if err := r.ParseMultipartForm(32 << 20); err != nil {
		writeError(w, http.StatusBadRequest, "invalid multipart body: "+err.Error())
		return
	}
	defer func() {
		if r.MultipartForm != nil {
			_ = r.MultipartForm.RemoveAll()
		}
	}()
	file, header, err := r.FormFile("file")
	if err != nil {
		file, header, err = r.FormFile("model")
	}
	if err != nil {
		writeError(w, http.StatusBadRequest, "multipart field \"file\" is required")
		return
	}
	defer file.Close()
	if header.Size > maxUploadBytes {
		writeError(w, http.StatusRequestEntityTooLarge, "model is too large")
		return
	}
	in, err := s.mgr.Workspace().Save(header.Filename, file)
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, in)
}

func (s *Server) handleRun(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeError(w, http.StatusMethodNotAllowed, "use POST")
		return
	}
	params, err := job.DecodeParams(io.LimitReader(r.Body, 1<<20))
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	j, err := s.mgr.Start(params)
	switch {
	case err == nil:
	case errors.Is(err, job.ErrBusy):
		writeError(w, http.StatusConflict, err.Error())
		return
	case errors.Is(err, job.ErrNoCLI):
		writeError(w, http.StatusServiceUnavailable, err.Error())
		return
	case errors.Is(err, job.ErrUnknownInput):
		writeError(w, http.StatusBadRequest, err.Error())
		return
	default:
		var re *job.RangeError
		if errors.As(err, &re) {
			writeError(w, http.StatusBadRequest, err.Error())
			return
		}
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"jobId":   j.ID(),
		"command": j.Snapshot().Command,
	})
}

func (s *Server) handleCancel(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeError(w, http.StatusMethodNotAllowed, "use POST")
		return
	}
	var body struct {
		JobID string `json:"jobId"`
	}
	if err := decodeJSON(r, &body); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	j := s.mgr.Get(body.JobID)
	if j == nil {
		writeError(w, http.StatusNotFound, "unknown job")
		return
	}
	state := j.Snapshot().State
	if state != job.StateRunning {
		writeJSON(w, http.StatusOK, map[string]any{"cancelled": false, "state": state})
		return
	}
	if err := s.mgr.Cancel(body.JobID); err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"cancelled": true, "jobId": body.JobID})
}

func (s *Server) handleModel(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		writeError(w, http.StatusMethodNotAllowed, "use GET")
		return
	}
	j := s.mgr.Get(r.URL.Query().Get("job"))
	if j == nil {
		writeError(w, http.StatusNotFound, "unknown job")
		return
	}
	path := j.OutputPath()
	f, err := os.Open(path)
	if err != nil {
		writeError(w, http.StatusNotFound, "no result yet for this job")
		return
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	name := strings.TrimSuffix(filepath.Base(j.Snapshot().Input), filepath.Ext(j.Snapshot().Input)) + "_remeshed.obj"
	if name == "_remeshed.obj" {
		name = "remeshed.obj"
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Disposition", "attachment; filename=\""+name+"\"")
	w.Header().Set("Content-Length", itoa(st.Size()))
	w.WriteHeader(http.StatusOK)
	_, _ = io.Copy(w, f)
}

func itoa(n int64) string {
	if n == 0 {
		return "0"
	}
	var buf [20]byte
	i := len(buf)
	for n > 0 {
		i--
		buf[i] = byte('0' + n%10)
		n /= 10
	}
	return string(buf[i:])
}
