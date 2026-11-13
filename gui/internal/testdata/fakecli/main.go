// Command fakecli is a stand-in for the headless autoremesher that speaks the
// JSON-lines contract in docs/cli-protocol.md. It is used by the Go tests and by
// the manual end-to-end smoke test.
package main

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

var boolFlags = map[string]bool{
	"json": true, "verbose": true, "progress": true, "version": true, "help": true,
	"exact-target-quads": true, "keep-all-islands": true, "weld-patches": true,
	"graded-holes": true, "legacy-holes": true, "projected-caps": true,
	"local-remesh": true, "relative-tolerance": true, "absolute-tolerance": true,
	"strict": true,
}

type options struct {
	values  map[string]string
	flags   map[string]bool
	unknown []string
}

func parse(args []string) options {
	o := options{values: map[string]string{}, flags: map[string]bool{}}
	for i := 0; i < len(args); i++ {
		a := args[i]
		if !strings.HasPrefix(a, "-") {
			o.unknown = append(o.unknown, a)
			continue
		}
		name := strings.TrimLeft(a, "-")
		if eq := strings.IndexByte(name, '='); eq >= 0 {
			o.values[name[:eq]] = name[eq+1:]
			continue
		}
		if boolFlags[name] {
			o.flags[name] = true
			continue
		}
		if i+1 < len(args) && !strings.HasPrefix(args[i+1], "-") {
			i++
			o.values[name] = args[i]
			continue
		}
		o.flags[name] = true
	}
	return o
}

func (o options) value(names ...string) string {
	for _, n := range names {
		if v, ok := o.values[n]; ok {
			return v
		}
	}
	return ""
}

func emit(v any) {
	b, err := json.Marshal(v)
	if err != nil {
		return
	}
	os.Stdout.Write(append(b, 10))
}

func env(name string) string { return strings.TrimSpace(os.Getenv(name)) }

func delay() time.Duration {
	if ms := env("FAKE_DELAY_MS"); ms != "" {
		if n, err := strconv.Atoi(ms); err == nil && n >= 0 {
			return time.Duration(n) * time.Millisecond
		}
	}
	return 12 * time.Millisecond
}

func main() {
	o := parse(os.Args[1:])
	if o.flags["version"] {
		fmt.Println("autoremesher 1.9.0-fake (fakecli)")
		return
	}
	if o.flags["help"] {
		fmt.Println("fakecli: staging stub for the AutoRemesher JSON-lines contract")
		return
	}
	if len(o.unknown) > 0 {
		fmt.Fprintln(os.Stderr, "fakecli: ignoring positional args:", strings.Join(o.unknown, " "))
	}
	jsonMode := o.flags["json"]
	input := o.value("input", "i")
	output := o.value("output", "o")
	target := 20000
	if q := o.value("target-quads", "q"); q != "" {
		if n, err := strconv.Atoi(q); err == nil && n > 0 {
			target = n
		}
	}
	if output == "" && input != "" {
		stem := strings.TrimSuffix(filepath.Base(input), filepath.Ext(input))
		output = filepath.Join(filepath.Dir(input), stem+"_remeshed.obj")
	}
	mode := env("FAKE_MODE")
	started := time.Now()

	if input == "" {
		emit(map[string]any{"type": "error", "message": "missing --input"})
		os.Exit(1)
	}
	inVerts, inTris := countOBJ(input)
	if inVerts == 0 {
		emit(map[string]any{"type": "error", "message": "failed to load " + input})
		os.Exit(1)
	}

	if jsonMode {
		emit(map[string]any{
			"type": "start", "version": "1.9.0-fake", "input": input, "output": output,
			"targetQuads": target, "vertices": inVerts, "triangles": inTris,
		})
	} else {
		fmt.Fprintf(os.Stderr, "fakecli: %d verts / %d tris -> %s\n", inVerts, inTris, output)
	}

	if mode == "error" {
		emit(map[string]any{"type": "error", "message": "fake remeshing failure"})
		os.Exit(2)
	}
	if mode == "hang" {
		time.Sleep(60 * time.Second)
	}

	steps := []string{
		"Preprocess", "Decimation", "Frame field", "Parametrization",
		"Quad extraction", "Weld", "Hole filling", "Optimize", "Write",
	}
	for i, step := range steps {
		value := float64(i+1) / float64(len(steps)+1)
		if jsonMode {
			emit(map[string]any{"type": "progress", "value": math.Round(value*1000) / 1000, "status": step})
			if i%3 == 0 {
				emit(map[string]any{"type": "log", "level": "info",
					"message": fmt.Sprintf("%s: %d %%", step, int(value*100))})
			}
		}
		time.Sleep(delay())
	}

	if jsonMode && o.flags["verbose"] {
		emit(map[string]any{"type": "report", "lines": []string{
			"decimation 0.31s",
			"frame field 1.20s",
			fmt.Sprintf("target %d quads", target),
		}})
	}

	verts, quads, err := writeGrid(output, target)
	if err != nil {
		emit(map[string]any{"type": "error", "message": "output write failed: " + err.Error()})
		os.Exit(3)
	}
	seconds := math.Round(time.Since(started).Seconds()*100) / 100
	emit(map[string]any{
		"type": "result", "output": output, "seconds": seconds,
		"vertices": verts, "quads": quads, "nonQuads": 0, "triangles": inTris,
	})
}

func countOBJ(path string) (verts, tris int) {
	b, err := os.ReadFile(path)
	if err != nil {
		return 0, 0
	}
	for _, line := range strings.Split(string(b), "\n") {
		switch {
		case strings.HasPrefix(line, "v "):
			verts++
		case strings.HasPrefix(line, "f "):
			if n := len(strings.Fields(line)) - 1; n >= 3 {
				tris += n - 2
			}
		}
	}
	return verts, tris
}

// writeGrid emits a quad grid of roughly the requested face count.
func writeGrid(path string, target int) (verts, quads int, err error) {
	n := int(math.Round(math.Sqrt(float64(target))))
	if n < 1 {
		n = 1
	}
	if n > 96 {
		n = 96
	}
	var b strings.Builder
	b.WriteString("# fakecli staging output\n")
	b.WriteString(fmt.Sprintf("# target quads %d\n", target))
	for y := 0; y <= n; y++ {
		for x := 0; x <= n; x++ {
			fx := float64(x) / float64(n)
			fy := float64(y) / float64(n)
			z := 0.16 * math.Sin(fx*math.Pi*2) * math.Cos(fy*math.Pi*2)
			b.WriteString(fmt.Sprintf("v %.6f %.6f %.6f\n", fx-0.5, fy-0.5, z))
		}
	}
	idx := func(x, y int) int { return y*(n+1) + x + 1 }
	for y := 0; y < n; y++ {
		for x := 0; x < n; x++ {
			b.WriteString(fmt.Sprintf("f %d %d %d %d\n", idx(x, y), idx(x+1, y), idx(x+1, y+1), idx(x, y+1)))
			quads++
		}
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return 0, 0, err
	}
	if err := os.WriteFile(path, []byte(b.String()), 0o644); err != nil {
		return 0, 0, err
	}
	return (n + 1) * (n + 1), quads, nil
}
