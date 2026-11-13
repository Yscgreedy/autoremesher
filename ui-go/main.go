// autoremesher-ui-go is the optional browser front end for AutoRemesher.
//
// It never links against the remeshing core. Everything it does goes through
// the autoremesher_cli process: flags in, stdout report out, stderr event
// stream out. That keeps the front end free of Qt, cgo and third-party modules.
//
//	go build ./...
//	go run . -addr 127.0.0.1:8787
package main

import (
	"context"
	"embed"
	"errors"
	"flag"
	"fmt"
	"io/fs"
	"log"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"
	"time"
)

const version = "0.1.0"

//go:embed web/index.html
var webAssets embed.FS

func main() {
	var (
		addr    = flag.String("addr", "127.0.0.1:8787", "监听地址（默认只绑本机，无鉴权）")
		cliFlag = flag.String("cli", "", "autoremesher_cli 路径，覆盖 AUTOREMESHER_CLI 与自动查找")
		maxJobs = flag.Int("max-jobs", 32, "保留的作业数上限，超出后淘汰最旧的已完成作业")
	)
	var roots stringList
	flag.Var(&roots, "root", "允许 /api/fs 浏览的根目录，可重复；默认 当前目录 + 临时目录")
	flag.Parse()

	log.SetFlags(log.Ltime | log.Lmicroseconds)
	log.SetPrefix("[ui-go] ")

	workDir, err := os.Getwd()
	if err != nil {
		log.Fatalf("无法读取当前目录: %v", err)
	}
	if len(roots) == 0 {
		roots = append(roots, workDir, os.TempDir())
	}
	for i, r := range roots {
		if abs, err := filepath.Abs(r); err == nil {
			roots[i] = abs
		}
	}

	cliPath := resolveCLI(*cliFlag, workDir)
	mgr := NewManager(cliPath, workDir, *maxJobs)
	mgr.probeEventsJSON()

	eventsJSON, degradeNote, probeNote := mgr.EventsJSON()
	if cliPath == "" {
		log.Printf("警告: 未找到 autoremesher_cli（-cli / AUTOREMESHER_CLI / build/core）")
	} else {
		log.Printf("CLI: %s", cliPath)
	}
	log.Printf("事件方言: %s", dialectName(eventsJSON, probeNote))
	if degradeNote != "" {
		log.Printf("降级: %s", degradeNote)
	}

	// Diagnostic for forward-compatibility testing: every SSE event gets an
	// extra "x_future":1 field that no client knows about.
	injectField := envFlag("UI_GO_INJECT_X_FUTURE")
	SetInjectUnknownField(injectField)
	if injectField {
		log.Printf("诊断: 每个 SSE 事件将附带未知字段 x_future=1")
	}

	web, err := fs.Sub(webAssets, "web")
	if err != nil {
		log.Fatalf("内嵌页面损坏: %v", err)
	}

	srv := NewServer(mgr, web, roots, version)
	httpServer := &http.Server{
		Addr:              *addr,
		Handler:           srv.Handler(),
		ReadHeaderTimeout: 10 * time.Second,
		// No WriteTimeout: SSE connections are long lived by design.
	}

	url := "http://" + *addr + "/"
	if strings.HasPrefix(*addr, "0.0.0.0:") {
		url = "http://127.0.0.1:" + strings.TrimPrefix(*addr, "0.0.0.0:")
	}
	log.Printf("AutoRemesher ui-go %s (%s/%s) 开始监听 %s", version, runtime.GOOS, runtime.GOARCH, url)

	errCh := make(chan error, 1)
	go func() {
		if err := httpServer.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			errCh <- err
		}
	}()

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)

	select {
	case err := <-errCh:
		log.Fatalf("HTTP 服务失败: %v", err)
	case sig := <-stop:
		log.Printf("收到 %v，正在退出", sig)
	}

	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if err := httpServer.Shutdown(ctx); err != nil {
		log.Printf("关闭超时: %v", err)
	}
	log.Printf("已停止")
}

// envFlag reads a boolean-ish environment variable.
func envFlag(name string) bool {
	switch strings.ToLower(strings.TrimSpace(os.Getenv(name))) {
	case "1", "true", "yes", "on":
		return true
	}
	return false
}

func dialectName(eventsJSON bool, probeNote string) string {
	if eventsJSON {
		return "NDJSON (--events-json)"
	}
	if probeNote == "" {
		return "文本进度（未探测）"
	}
	return "文本进度（降级）"
}

// resolveCLI applies the documented precedence: flag, environment, then the
// build directories of this checkout, then PATH.
func resolveCLI(flagValue, workDir string) string {
	candidates := []string{}
	if flagValue != "" {
		candidates = append(candidates, flagValue)
	}
	if env := os.Getenv("AUTOREMESHER_CLI"); env != "" {
		candidates = append(candidates, env)
	}
	for _, dir := range []string{".", "..", "../.."} {
		base := filepath.Join(workDir, dir, "build", "core")
		candidates = append(candidates,
			filepath.Join(base, exeName("autoremesher_cli")),
			filepath.Join(base, "autoremesher_cli"))
	}
	for _, candidate := range candidates {
		abs, err := filepath.Abs(candidate)
		if err != nil {
			continue
		}
		if info, err := os.Stat(abs); err == nil && !info.IsDir() {
			return abs
		}
	}
	if path, err := exec.LookPath("autoremesher_cli"); err == nil {
		if abs, err := filepath.Abs(path); err == nil {
			return abs
		}
	}
	return ""
}

func exeName(base string) string {
	if runtime.GOOS == "windows" {
		return base + ".exe"
	}
	return base
}

// stringList is the repeatable -root flag.
type stringList []string

func (s *stringList) String() string { return strings.Join(*s, string(os.PathListSeparator)) }

func (s *stringList) Set(value string) error {
	if strings.TrimSpace(value) == "" {
		return fmt.Errorf("空的根目录")
	}
	*s = append(*s, value)
	return nil
}

func logf(format string, args ...any) {
	log.Printf(format, args...)
}
