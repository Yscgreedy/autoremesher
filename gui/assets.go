// Package gui embeds the static web UI.
package gui

import "embed"

// Assets is the embedded web tree; internal/server serves "web" under
// /s/<token>/.
//
//go:embed web
var Assets embed.FS
