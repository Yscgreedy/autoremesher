package job

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

// CLIInfo describes the discovered headless CLI.
type CLIInfo struct {
	Path    string `json:"path"`
	Found   bool   `json:"found"`
	Version string `json:"version,omitempty"`
	Source  string `json:"source,omitempty"`
}

// CLIName is the executable name of the headless remesher.
func CLIName() string {
	if runtime.GOOS == "windows" {
		return "autoremesher.exe"
	}
	return "autoremesher"
}

// candidate is one place the CLI may live.
type candidate struct {
	dir    string
	source string
}

// discoveryOrder documents (and implements) the CLI lookup order:
//  1. the --cli flag
//  2. next to the GUI executable
//  3. ../dist and .. relative to the GUI executable
//  4. the current working directory and ./dist
//  5. PATH lookup for "autoremesher"
func discoveryOrder(explicit string) []candidate {
	var out []candidate
	if strings.TrimSpace(explicit) != "" {
		out = append(out, candidate{dir: filepath.Dir(explicit), source: "flag"})
	}
	if exe, err := os.Executable(); err == nil {
		dir := filepath.Dir(exe)
		out = append(out,
			candidate{dir: dir, source: "exe-dir"},
			candidate{dir: filepath.Join(dir, "dist"), source: "exe-dir/dist"},
			candidate{dir: filepath.Join(dir, ".."), source: "parent"},
			candidate{dir: filepath.Join(dir, "..", "dist"), source: "parent/dist"},
			candidate{dir: filepath.Join(dir, "..", ".."), source: "grandparent"},
		)
	}
	if wd, err := os.Getwd(); err == nil {
		out = append(out,
			candidate{dir: wd, source: "cwd"},
			candidate{dir: filepath.Join(wd, "dist"), source: "cwd/dist"},
		)
	}
	return out
}

// Discover locates the CLI and probes its version. It never fails: a missing
// CLI yields Found=false so /api/info can report the problem to the UI.
func Discover(explicit string) CLIInfo {
	if p := strings.TrimSpace(explicit); p != "" {
		if abs, err := filepath.Abs(p); err == nil {
			p = abs
		}
		if isExecutable(p) {
			return CLIInfo{Path: p, Found: true, Source: "flag", Version: probeVersion(p)}
		}
	}
	names := []string{CLIName(), "autoremesher"}
	seen := map[string]bool{}
	for _, c := range discoveryOrder(explicit) {
		for _, name := range names {
			p := filepath.Clean(filepath.Join(c.dir, name))
			if seen[p] {
				continue
			}
			seen[p] = true
			if isExecutable(p) {
				return CLIInfo{Path: p, Found: true, Source: c.source, Version: probeVersion(p)}
			}
		}
	}
	if p, err := exec.LookPath("autoremesher"); err == nil {
		if abs, err := filepath.Abs(p); err == nil {
			p = abs
		}
		return CLIInfo{Path: p, Found: true, Source: "path", Version: probeVersion(p)}
	}
	if strings.TrimSpace(explicit) != "" {
		return CLIInfo{Path: explicit, Found: false, Source: "flag"}
	}
	return CLIInfo{Found: false, Source: "none"}
}

func isExecutable(path string) bool {
	st, err := os.Stat(path)
	if err != nil || st.IsDir() {
		return false
	}
	return true
}

// probeVersion runs "autoremesher --version" with a short timeout.
func probeVersion(path string) string {
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, path, "--version")
	setProcAttr(cmd)
	out, err := cmd.Output()
	if err != nil && len(out) == 0 {
		return ""
	}
	line := strings.TrimSpace(string(out))
	if i := strings.IndexByte(line, 10); i >= 0 {
		line = strings.TrimSpace(line[:i])
	}
	return line
}
