package job

import (
	"bufio"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
)

// Input describes one uploaded model inside the session workspace.
type Input struct {
	ID        string `json:"id"`
	Name      string `json:"name"`
	Path      string `json:"path"`
	Size      int64  `json:"size"`
	Vertices  int    `json:"vertices"`
	Faces     int    `json:"faces"`
	Triangles int    `json:"triangles"`
}

// Workspace is the per-session scratch directory holding uploads and outputs.
type Workspace struct {
	dir    string
	mu     sync.Mutex
	inputs map[string]*Input
}

// NewWorkspace creates a private temp directory and removes it on Close.
func NewWorkspace() (*Workspace, error) {
	dir, err := os.MkdirTemp("", "autoremesher-gui-")
	if err != nil {
		return nil, fmt.Errorf("create workspace: %w", err)
	}
	return &Workspace{dir: dir, inputs: map[string]*Input{}}, nil
}

// Dir returns the workspace root.
func (w *Workspace) Dir() string { return w.dir }

// NewID returns a hex identifier.
func NewID() string {
	var b [8]byte
	if _, err := rand.Read(b[:]); err != nil {
		return fmt.Sprintf("%d", os.Getpid())
	}
	return hex.EncodeToString(b[:])
}

// Save stores an uploaded model and parses its OBJ header counts.
func (w *Workspace) Save(name string, r io.Reader) (*Input, error) {
	id := NewID()
	clean := sanitizeName(name)
	path := filepath.Join(w.dir, id+"-"+clean)
	f, err := os.Create(path)
	if err != nil {
		return nil, fmt.Errorf("store upload: %w", err)
	}
	n, copyErr := io.Copy(f, r)
	closeErr := f.Close()
	if copyErr != nil {
		os.Remove(path)
		return nil, fmt.Errorf("store upload: %w", copyErr)
	}
	if closeErr != nil {
		os.Remove(path)
		return nil, fmt.Errorf("store upload: %w", closeErr)
	}
	verts, faces, tris := scanOBJ(path)
	in := &Input{ID: id, Name: clean, Path: path, Size: n, Vertices: verts, Faces: faces, Triangles: tris}
	w.mu.Lock()
	w.inputs[id] = in
	w.mu.Unlock()
	return in, nil
}

// Get looks up a previously stored input.
func (w *Workspace) Get(id string) (*Input, bool) {
	w.mu.Lock()
	defer w.mu.Unlock()
	in, ok := w.inputs[id]
	return in, ok
}

// OutputPath returns the result path for a job.
func (w *Workspace) OutputPath(jobID string) string {
	return filepath.Join(w.dir, "out-"+jobID+".obj")
}

// RemoveOutput deletes the result OBJ of one job. It is the workspace-side
// cleanup used when a finished job is evicted; a missing file is not an error,
// because a failed or cancelled job never produced one.
func (w *Workspace) RemoveOutput(jobID string) error {
	if err := os.Remove(w.OutputPath(jobID)); err != nil && !os.IsNotExist(err) {
		return fmt.Errorf("remove output: %w", err)
	}
	return nil
}

// Close removes the workspace tree.
func (w *Workspace) Close() error {
	return os.RemoveAll(w.dir)
}

func sanitizeName(name string) string {
	name = filepath.Base(strings.TrimSpace(name))
	name = strings.Map(func(r rune) rune {
		switch {
		case r == 0:
			return -1
		case r < 32, r == 127, r == 34, r == 92, r == 47, r == 58, r == 42, r == 63, r == 60, r == 62, r == 124:
			return '_'
		}
		return r
	}, name)
	name = strings.TrimSpace(name)
	if name == "" || name == "." || name == ".." {
		name = "model.obj"
	}
	if len(name) > 100 {
		name = name[len(name)-100:]
	}
	return name
}

// scanOBJ counts vertices and triangles in an OBJ file (header pass only).
func scanOBJ(path string) (vertices, faces, triangles int) {
	f, err := os.Open(path)
	if err != nil {
		return 0, 0, 0
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 0, 64*1024), 1<<20)
	for sc.Scan() {
		line := sc.Text()
		if line == "" {
			continue
		}
		switch {
		case strings.HasPrefix(line, "v "):
			vertices++
		case strings.HasPrefix(line, "f "):
			faces++
			n := len(strings.Fields(line)) - 1
			if n >= 3 {
				triangles += n - 2
			}
		}
	}
	return vertices, faces, triangles
}
