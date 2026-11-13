package server

import (
	"errors"
	"os"
	"path/filepath"
	"testing"
)

func TestIsWithin(t *testing.T) {
	root := filepath.Join(string(filepath.Separator), "roots", "a")
	cases := []struct {
		name string
		path string
		want bool
	}{
		{"root itself", root, true},
		{"direct child", filepath.Join(root, "m.obj"), true},
		{"nested child", filepath.Join(root, "x", "y", "m.obj"), true},
		{"parent", filepath.Dir(root), false},
		{"sibling with a shared prefix", root + "bc", false},
		{"escape through dotdot", filepath.Join(root, "..", "b", "m.obj"), false},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if got := isWithin(c.path, root); got != c.want {
				t.Fatalf("isWithin(%q, %q) = %v, want %v", c.path, root, got, c.want)
			}
		})
	}
}

func TestResolveDirIsConfinedToRoots(t *testing.T) {
	base := t.TempDir()
	root := filepath.Join(base, "root")
	outside := filepath.Join(base, "outside")
	for _, dir := range []string{root, outside} {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			t.Fatalf("mkdir %s: %v", dir, err)
		}
	}
	model := filepath.Join(root, "m.obj")
	if err := os.WriteFile(model, []byte("v 0 0 0\n"), 0o644); err != nil {
		t.Fatalf("write model: %v", err)
	}

	got, err := resolveDir(root, []string{root})
	if err != nil {
		t.Fatalf("resolveDir(root): %v", err)
	}
	want, err := filepath.EvalSymlinks(root)
	if err != nil {
		t.Fatalf("EvalSymlinks(root): %v", err)
	}
	if got != want {
		t.Fatalf("resolveDir = %q, want %q", got, want)
	}

	if _, err := resolveDir(outside, []string{root}); !errors.Is(err, errForbiddenPath) {
		t.Fatalf("outside directory error = %v, want errForbiddenPath", err)
	}
	if _, err := resolveDir(root, nil); !errors.Is(err, errForbiddenPath) {
		t.Fatalf("no roots error = %v, want errForbiddenPath", err)
	}
	if _, err := resolveDir(model, []string{root}); !errors.Is(err, errNotDirectory) {
		t.Fatalf("file error = %v, want errNotDirectory", err)
	}
	if _, err := resolveDir(filepath.Join(root, "missing"), []string{root}); err == nil {
		t.Fatal("a missing directory must be an error")
	}
}

func TestResolveDirRejectsSymlinkEscape(t *testing.T) {
	base := t.TempDir()
	root := filepath.Join(base, "root")
	outside := filepath.Join(base, "outside")
	for _, dir := range []string{root, outside} {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			t.Fatalf("mkdir %s: %v", dir, err)
		}
	}
	link := filepath.Join(root, "sneaky")
	if err := os.Symlink(outside, link); err != nil {
		t.Skipf("symlinks are unavailable here: %v", err)
	}
	if _, err := resolveDir(link, []string{root}); !errors.Is(err, errForbiddenPath) {
		t.Fatalf("symlink escape error = %v, want errForbiddenPath", err)
	}
}
