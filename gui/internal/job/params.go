package job

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"strconv"
	"strings"
)

// Model types understood by the CLI (docs/cli-protocol.md).
const (
	ModelOrganic     = "organic"
	ModelHardSurface = "hard-surface"
)

// Limits enforced server side before a job is allowed to start.
const (
	MinTargetQuads = 100
	MaxTargetQuads = 2000000
	MaxThreads     = 256
	MinEdgeScaling = 0.01
	MaxEdgeScaling = 100.0
)

// Params is the payload of POST /api/run. Field names follow the web UI and
// map 1:1 onto the CLI flags listed in docs/cli-protocol.md.
type Params struct {
	InputID            string  `json:"inputId"`
	TargetQuads        int     `json:"targetQuads"`
	Adaptivity         float64 `json:"adaptivity"`
	Anisotropy         float64 `json:"anisotropy"`
	SharpEdge          float64 `json:"sharpEdge"`
	SmoothNormal       float64 `json:"smoothNormal"`
	ModelType          string  `json:"modelType"`
	ExactTargetQuads   bool    `json:"exactTargetQuads"`
	Threads            int     `json:"threads"`
	KeepIslandFraction float64 `json:"keepIslandFraction"`
	WeldPatches        bool    `json:"weldPatches"`
	LegacyHoles        bool    `json:"legacyHoles"`
	ProjectedCaps      bool    `json:"projectedCaps"`
	LocalRemesh        bool    `json:"localRemesh"`
	EdgeScaling        float64 `json:"edgeScaling"`
}

// DefaultParams mirrors the defaults table of the CLI contract.
func DefaultParams() Params {
	return Params{
		TargetQuads:  20000,
		Adaptivity:   0.5,
		Anisotropy:   0.0,
		SharpEdge:    60,
		SmoothNormal: 0,
		ModelType:    ModelOrganic,
		Threads:      0,
		EdgeScaling:  1.0,
	}
}

// numeric accepts a JSON number, a numeric string (HTML number inputs produce
// strings when they are serialised naively) or null / empty.
type numeric float64

func (n *numeric) UnmarshalJSON(b []byte) error {
	s := strings.TrimSpace(string(b))
	if s == "" || s == "null" {
		return nil
	}
	if len(s) > 1 && s[0] == '"' {
		var str string
		if err := json.Unmarshal(b, &str); err != nil {
			return err
		}
		s = strings.TrimSpace(str)
		if s == "" {
			return nil
		}
	}
	f, err := strconv.ParseFloat(s, 64)
	if err != nil {
		return fmt.Errorf("expected a number, got %s", string(b))
	}
	*n = numeric(f)
	return nil
}

// boolean accepts true/false, 1/0 and "true"/"false".
type boolean bool

func (b *boolean) UnmarshalJSON(raw []byte) error {
	s := strings.TrimSpace(string(raw))
	if s == "" || s == "null" {
		return nil
	}
	switch strings.ToLower(strings.Trim(s, "\"")) {
	case "true", "1", "yes", "on":
		*b = true
	case "false", "0", "no", "off":
		*b = false
	default:
		return fmt.Errorf("expected a boolean, got %s", s)
	}
	return nil
}

type paramsWire struct {
	InputID            *string  `json:"inputId"`
	TargetQuads        *numeric `json:"targetQuads"`
	Adaptivity         *numeric `json:"adaptivity"`
	Anisotropy         *numeric `json:"anisotropy"`
	SharpEdge          *numeric `json:"sharpEdge"`
	SmoothNormal       *numeric `json:"smoothNormal"`
	ModelType          *string  `json:"modelType"`
	ExactTargetQuads   *boolean `json:"exactTargetQuads"`
	Threads            *numeric `json:"threads"`
	KeepIslandFraction *numeric `json:"keepIslandFraction"`
	WeldPatches        *boolean `json:"weldPatches"`
	LegacyHoles        *boolean `json:"legacyHoles"`
	ProjectedCaps      *boolean `json:"projectedCaps"`
	LocalRemesh        *boolean `json:"localRemesh"`
	EdgeScaling        *numeric `json:"edgeScaling"`
}

// DecodeParams reads a JSON body and applies the protocol defaults for every
// field the caller omitted.
func DecodeParams(r io.Reader) (Params, error) {
	p := DefaultParams()
	dec := json.NewDecoder(io.LimitReader(r, 1<<20))
	var w paramsWire
	if err := dec.Decode(&w); err != nil {
		return p, fmt.Errorf("invalid JSON body: %w", err)
	}
	if w.InputID != nil {
		p.InputID = strings.TrimSpace(*w.InputID)
	}
	if w.TargetQuads != nil {
		p.TargetQuads = int(float64(*w.TargetQuads))
	}
	if w.Adaptivity != nil {
		p.Adaptivity = float64(*w.Adaptivity)
	}
	if w.Anisotropy != nil {
		p.Anisotropy = float64(*w.Anisotropy)
	}
	if w.SharpEdge != nil {
		p.SharpEdge = float64(*w.SharpEdge)
	}
	if w.SmoothNormal != nil {
		p.SmoothNormal = float64(*w.SmoothNormal)
	}
	if w.ModelType != nil {
		p.ModelType = strings.ToLower(strings.TrimSpace(*w.ModelType))
	}
	if w.ExactTargetQuads != nil {
		p.ExactTargetQuads = bool(*w.ExactTargetQuads)
	}
	if w.Threads != nil {
		p.Threads = int(float64(*w.Threads))
	}
	if w.KeepIslandFraction != nil {
		p.KeepIslandFraction = float64(*w.KeepIslandFraction)
	}
	if w.WeldPatches != nil {
		p.WeldPatches = bool(*w.WeldPatches)
	}
	if w.LegacyHoles != nil {
		p.LegacyHoles = bool(*w.LegacyHoles)
	}
	if w.ProjectedCaps != nil {
		p.ProjectedCaps = bool(*w.ProjectedCaps)
	}
	if w.LocalRemesh != nil {
		p.LocalRemesh = bool(*w.LocalRemesh)
	}
	if w.EdgeScaling != nil {
		p.EdgeScaling = float64(*w.EdgeScaling)
	}
	return p, nil
}

// RangeError reports a numeric parameter outside its accepted range.
type RangeError struct {
	Field string
	Min   float64
	Max   float64
	Value float64
}

func (e *RangeError) Error() string {
	return fmt.Sprintf("%s must be between %g and %g (got %g)", e.Field, e.Min, e.Max, e.Value)
}

func inRange(field string, v, lo, hi float64) error {
	if v < lo || v > hi {
		return &RangeError{Field: field, Min: lo, Max: hi, Value: v}
	}
	return nil
}

// Validate enforces the numeric ranges documented for the UI.
func (p Params) Validate() error {
	if p.InputID == "" {
		return errors.New("inputId is required (upload a model first)")
	}
	if err := inRange("targetQuads", float64(p.TargetQuads), MinTargetQuads, MaxTargetQuads); err != nil {
		return err
	}
	if err := inRange("adaptivity", p.Adaptivity, 0, 1); err != nil {
		return err
	}
	if err := inRange("anisotropy", p.Anisotropy, 0, 1); err != nil {
		return err
	}
	if err := inRange("sharpEdge", p.SharpEdge, 0, 180); err != nil {
		return err
	}
	if err := inRange("smoothNormal", p.SmoothNormal, 0, 180); err != nil {
		return err
	}
	if err := inRange("threads", float64(p.Threads), 0, MaxThreads); err != nil {
		return err
	}
	if err := inRange("keepIslandFraction", p.KeepIslandFraction, 0, 1); err != nil {
		return err
	}
	if err := inRange("edgeScaling", p.EdgeScaling, MinEdgeScaling, MaxEdgeScaling); err != nil {
		return err
	}
	switch p.ModelType {
	case ModelOrganic, ModelHardSurface:
	default:
		return fmt.Errorf("modelType must be %q or %q (got %q)", ModelOrganic, ModelHardSurface, p.ModelType)
	}
	return nil
}

func formatFloat(v float64) string {
	return strconv.FormatFloat(v, 'g', -1, 64)
}

// Args builds the CLI argv (without argv[0]) for a validated parameter set.
func (p Params) Args(inputPath, outputPath string) []string {
	args := []string{
		"--input", inputPath,
		"--output", outputPath,
		"--json",
		"--verbose",
		"--target-quads", strconv.Itoa(p.TargetQuads),
		"--adaptivity", formatFloat(p.Adaptivity),
		"--anisotropy", formatFloat(p.Anisotropy),
		"--sharp-edge", formatFloat(p.SharpEdge),
		"--smooth-normal", formatFloat(p.SmoothNormal),
		"--edge-scaling", formatFloat(p.EdgeScaling),
		"--keep-island-fraction", formatFloat(p.KeepIslandFraction),
		"--model-type", p.ModelType,
		"--threads", strconv.Itoa(p.Threads),
	}
	if p.ExactTargetQuads {
		args = append(args, "--exact-target-quads")
	}
	if p.WeldPatches {
		args = append(args, "--weld-patches")
	}
	if p.LegacyHoles {
		args = append(args, "--legacy-holes")
	}
	if p.ProjectedCaps {
		args = append(args, "--projected-caps")
	}
	if p.LocalRemesh {
		args = append(args, "--local-remesh")
	}
	return args
}
