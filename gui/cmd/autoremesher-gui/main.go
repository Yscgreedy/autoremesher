package main

import (
	"context"
	"flag"
	"fmt"
	"net"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"time"

	"autoremesher-gui"
	"autoremesher-gui/internal/job"
	"autoremesher-gui/internal/server"
)

const appVersion = "1.0.0"

func main() {
	cliFlag := flag.String("cli", "", "path to the headless autoremesher executable")
	noOpen := flag.Bool("no-open", false, "do not open the default browser")
	port := flag.Int("port", 0, "listen port (0 picks a free port)")
	addr := flag.String("addr", "127.0.0.1", "loopback address to bind")
	showVersion := flag.Bool("version", false, "print version and exit")
	maxJobs := flag.Int("max-jobs", job.DefaultMaxJobs,
		"how many finished jobs to keep in memory; the oldest finished job is dropped (with its output file) beyond that")
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "autoremesher-gui %s\n\nusage: autoremesher-gui [flags]\n\n", appVersion)
		flag.PrintDefaults()
	}
	flag.Parse()

	if *showVersion {
		fmt.Printf("autoremesher-gui %s (%s/%s)\n", appVersion, runtime.GOOS, runtime.GOARCH)
		return
	}
	if err := run(*cliFlag, *noOpen, *port, *addr, *maxJobs); err != nil {
		fmt.Fprintln(os.Stderr, "autoremesher-gui:", err)
		os.Exit(1)
	}
}

func run(cliFlag string, noOpen bool, port int, addr string, maxJobs int) error {
	if port < 0 || port > 65535 {
		return fmt.Errorf("--port must be between 0 and 65535 (got %d)", port)
	}
	if maxJobs < 1 {
		return fmt.Errorf("--max-jobs must be at least 1 (got %d)", maxJobs)
	}
	addr = strings.TrimSpace(addr)
	if addr == "" {
		addr = "127.0.0.1"
	}
	if err := requireLoopback(addr); err != nil {
		return err
	}

	ws, err := job.NewWorkspace()
	if err != nil {
		return err
	}
	cliInfo := job.Discover(cliFlag)
	mgr := job.NewManager(cliInfo, ws)
	mgr.SetMaxJobs(maxJobs)
	defer func() { _ = mgr.Close() }()

	token, err := server.NewToken()
	if err != nil {
		return err
	}
	srv, err := server.New(server.Config{
		Version:     appVersion,
		Token:       token,
		Manager:     mgr,
		Assets:      gui.Assets,
		ExplicitCLI: cliFlag,
	})
	if err != nil {
		return err
	}

	ln, err := net.Listen("tcp", net.JoinHostPort(addr, strconv.Itoa(port)))
	if err != nil {
		return err
	}
	url := fmt.Sprintf("http://%s/s/%s/", ln.Addr().String(), token)

	fmt.Printf("AutoRemesher GUI %s (%s/%s)\n", appVersion, runtime.GOOS, runtime.GOARCH)
	fmt.Printf("  url   %s\n", url)
	if cliInfo.Found {
		ver := cliInfo.Version
		if ver == "" {
			ver = "version unknown"
		}
		fmt.Printf("  cli   %s (%s, via %s)\n", cliInfo.Path, ver, cliInfo.Source)
	} else {
		fmt.Println("  cli   NOT FOUND — pass --cli PATH to the headless autoremesher")
	}
	fmt.Printf("  work  %s\n", ws.Dir())
	fmt.Printf("  jobs  keep at most %d finished jobs\n", mgr.MaxJobs())
	fmt.Println("  stop  Ctrl+C")

	httpSrv := &http.Server{
		Handler:           srv.Handler(),
		ReadHeaderTimeout: 15 * time.Second,
	}
	errc := make(chan error, 1)
	go func() { errc <- httpSrv.Serve(ln) }()

	if !noOpen {
		go func() {
			time.Sleep(120 * time.Millisecond)
			if err := openBrowser(url); err != nil {
				fmt.Fprintf(os.Stderr, "could not open browser: %v\n", err)
			}
		}()
	}

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)
	select {
	case err := <-errc:
		return err
	case <-stop:
		fmt.Println("shutting down")
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = httpSrv.Shutdown(ctx)
		return nil
	}
}

// requireLoopback keeps the GUI bound to the local machine.
func requireLoopback(addr string) error {
	if strings.EqualFold(addr, "localhost") {
		return nil
	}
	ip := net.ParseIP(addr)
	if ip == nil || !ip.IsLoopback() {
		return fmt.Errorf("--addr must be a loopback address such as 127.0.0.1 or ::1 (got %q)", addr)
	}
	return nil
}

func openBrowser(url string) error {
	switch runtime.GOOS {
	case "windows":
		return exec.Command("rundll32", "url.dll,FileProtocolHandler", url).Start()
	case "darwin":
		return exec.Command("open", url).Start()
	default:
		if _, err := exec.LookPath("xdg-open"); err != nil {
			return err
		}
		return exec.Command("xdg-open", url).Start()
	}
}
