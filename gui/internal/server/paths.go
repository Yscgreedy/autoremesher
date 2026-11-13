package server

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// Directory containment helpers for a future read-only file browser.
//
// The GUI has no /api/fs route today: uploads land in the job workspace and the
// page never needs to look at the filesystem, so nothing calls these functions
// yet. They are the tested building blocks for that route (the same rules ui-go
// uses in safeDir/withinRoots), kept here so the policy is settled before
// anything is exposed:
//
//   - the target must be a directory, an empty or NUL-containing path is refused;
//   - symlinks are resolved before the containment test, so a link inside an
//     allowed root cannot point outside it;
//   - containment is decided by path component, not by string prefix, so /a/bc
//     is not treated as being inside /a/b.
var (
	// errForbiddenPath reports a directory outside every allowed root.
	errForbiddenPath = errors.New("path is outside the browsable roots")
	// errNotDirectory reports a path that exists but is not a directory.
	errNotDirectory = errors.New("path is not a directory")
)

// resolveDir makes dir absolute, resolves symlinks and proves it is a directory
// inside one of roots. A caller maps errForbiddenPath to 403 and every other
// error to 400.
func resolveDir(dir string, roots []string) (string, error) {
	if strings.ContainsRune(dir, 0) {
		return "", errForbiddenPath
	}
	abs, err := filepath.Abs(dir)
	if err != nil {
		return "", errForbiddenPath
	}
	real, err := filepath.EvalSymlinks(abs)
	if err != nil {
		return "", fmt.Errorf("cannot access %s: %w", dir, err)
	}
	info, err := os.Stat(real)
	if err != nil {
		return "", fmt.Errorf("cannot access %s: %w", dir, err)
	}
	if !info.IsDir() {
		return "", errNotDirectory
	}
	if !withinRoots(real, roots) {
		return "", errForbiddenPath
	}
	return real, nil
}

// withinRoots reports whether path is inside at least one of roots. A root that
// cannot be resolved is compared as given.
func withinRoots(path string, roots []string) bool {
	for _, root := range roots {
		realRoot, err := filepath.EvalSymlinks(root)
		if err != nil {
			realRoot = root
		}
		if isWithin(path, realRoot) {
			return true
		}
	}
	return false
}

// isWithin reports whether path is root itself or a descendant of it.
func isWithin(path, root string) bool {
	rel, err := filepath.Rel(root, path)
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
