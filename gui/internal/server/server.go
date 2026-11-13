// Package server implements the loopback HTTP API and the embedded web UI.
package server

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"io"
	"io/fs"
	"net/http"
	"strings"

	"autoremesher-gui/internal/job"
)

// Config wires the HTTP layer to the job manager.
type Config struct {
	Version     string
	Token       string
	Manager     *job.Manager
	Assets      fs.FS // tree containing the "web" directory
	ExplicitCLI string
}

// Server is the loopback GUI server.
type Server struct {
	version     string
	token       string
	mgr         *job.Manager
	assets      fs.FS
	explicitCLI string
	handler     http.Handler
}

// NewToken returns a 16 byte hex session token.
func NewToken() (string, error) {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		return "", err
	}
	return hex.EncodeToString(b[:]), nil
}

// New builds the router.
func New(cfg Config) (*Server, error) {
	s := &Server{
		version:     cfg.Version,
		token:       cfg.Token,
		mgr:         cfg.Manager,
		assets:      cfg.Assets,
		explicitCLI: cfg.ExplicitCLI,
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/", s.handleRoot)
	mux.Handle("/s/"+s.token+"/", s.handleStatic())
	mux.HandleFunc("/api/info", s.guard(s.handleInfo))
	mux.HandleFunc("/api/upload", s.guard(s.handleUpload))
	mux.HandleFunc("/api/run", s.guard(s.handleRun))
	mux.HandleFunc("/api/events", s.guard(s.handleEvents))
	mux.HandleFunc("/api/cancel", s.guard(s.handleCancel))
	mux.HandleFunc("/api/model", s.guard(s.handleModel))
	s.handler = mux
	return s, nil
}

// Token returns the session token.
func (s *Server) Token() string { return s.token }

// Handler returns the root handler.
func (s *Server) Handler() http.Handler { return s.handler }

// tokenOK accepts the token as a header, a query parameter or a cookie.
func (s *Server) tokenOK(r *http.Request) bool {
	if s.token == "" {
		return false
	}
	if v := r.Header.Get("X-AR-Token"); v != "" && subtle.ConstantTimeCompare([]byte(v), []byte(s.token)) == 1 {
		return true
	}
	for _, key := range []string{"token", "t"} {
		if v := r.URL.Query().Get(key); v != "" && subtle.ConstantTimeCompare([]byte(v), []byte(s.token)) == 1 {
			return true
		}
	}
	if c, err := r.Cookie("ar_token"); err == nil {
		if subtle.ConstantTimeCompare([]byte(c.Value), []byte(s.token)) == 1 {
			return true
		}
	}
	return false
}

// guard rejects /api/* calls without a valid token.
func (s *Server) guard(h http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !s.tokenOK(r) {
			w.Header().Set("Cache-Control", "no-store")
			writeError(w, http.StatusUnauthorized, "invalid or missing session token")
			return
		}
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("X-Content-Type-Options", "nosniff")
		h(w, r)
	}
}

// handleRoot serves "/" as a redirect into the token scoped UI and 404s the rest.
func (s *Server) handleRoot(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	http.Redirect(w, r, "/s/"+s.token+"/", http.StatusFound)
}

// handleStatic serves the embedded web assets under /s/<token>/.
func (s *Server) handleStatic() http.Handler {
	sub, err := fs.Sub(s.assets, "web")
	if err != nil {
		panic(err)
	}
	files := http.FileServer(http.FS(sub))
	prefix := "/s/" + s.token
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		rel := strings.TrimPrefix(r.URL.Path, prefix)
		if rel == "" {
			rel = "/"
		}
		if strings.Contains(rel, "..") {
			http.NotFound(w, r)
			return
		}
		w.Header().Set("Cache-Control", "no-store")
		clone := r.Clone(r.Context())
		clone.URL.Path = rel
		files.ServeHTTP(w, clone)
	})
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	enc := json.NewEncoder(w)
	enc.SetEscapeHTML(false)
	_ = enc.Encode(v)
}

func decodeJSON(r *http.Request, v any) error {
	dec := json.NewDecoder(io.LimitReader(r.Body, 1<<20))
	return dec.Decode(v)
}

func writeError(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]any{"error": msg})
}
