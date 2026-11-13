// Package testutil builds the fake CLI used by the Go tests.
package testutil

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sync"
	"testing"
)

var (
	once sync.Once
	bin  string
	err  error
)

// ModuleRoot returns the gui/ directory holding go.mod.
func ModuleRoot() string {
	_, file, _, ok := runtime.Caller(0)
	if !ok {
		return "."
	}
	return filepath.Clean(filepath.Join(filepath.Dir(file), "..", ".."))
}

// BuildFakeCLI compiles internal/testdata/fakecli once per test binary and
// returns the path of the resulting executable.
func BuildFakeCLI(tb testing.TB) string {
	tb.Helper()
	once.Do(func() {
		dir, e := os.MkdirTemp("", "fakecli-bin-")
		if e != nil {
			err = e
			return
		}
		name := "fakecli"
		if runtime.GOOS == "windows" {
			name += ".exe"
		}
		out := filepath.Join(dir, name)
		cmd := exec.Command("go", "build", "-o", out, "./internal/testdata/fakecli")
		cmd.Dir = ModuleRoot()
		cmd.Env = append(os.Environ(), "GO111MODULE=on", "GOPROXY=off", "GOFLAGS=")
		if b, e := cmd.CombinedOutput(); e != nil {
			err = fmt.Errorf("build fakecli: %v: %s", e, string(b))
			return
		}
		bin = out
	})
	if err != nil {
		tb.Fatalf("%v", err)
	}
	return bin
}
