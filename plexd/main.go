// plexd is PlexMusic's playback daemon for NextUI. It keeps music playing
// after the UI exits, in the NextUI menu and inside games, and makes the UI's
// network requests for it (/tmp/plexd-net.sock) with verified TLS.
//
//	plexd serve             run the daemon (normally started by launch.sh)
//	plexd ctl <cmd> [json]  send a command to the running daemon, e.g.
//	                        plexd ctl toggle, plexd ctl presleep, plexd ctl status
package main

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"github.com/ExpandedPlum/PlexMusicPak/plexd/internal/hotkeys"
	"github.com/ExpandedPlum/PlexMusicPak/plexd/internal/player"
	"github.com/ExpandedPlum/PlexMusicPak/plexd/internal/plex"
	"github.com/ExpandedPlum/PlexMusicPak/plexd/internal/service"
)

func env(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func socketPath() string { return env("PLEXD_SOCKET", "/tmp/plexd.sock") }

// netSocketPath is where the UI sends its own HTTP requests for plexd to make
// (see plex.Client.ForwardProxy).
func netSocketPath() string { return env("PLEXD_NET_SOCKET", "/tmp/plexd-net.sock") }

// serveNet runs the UI's request forwarder until done is closed.
func serveNet(path string, h http.Handler, done <-chan struct{}) error {
	_ = os.Remove(path)
	ln, err := net.Listen("unix", path)
	if err != nil {
		return err
	}
	_ = os.Chmod(path, 0o666)
	srv := &http.Server{Handler: h, ReadHeaderTimeout: 10 * time.Second}
	go func() {
		<-done
		_ = srv.Close()
		_ = os.Remove(path)
	}()
	if err := srv.Serve(ln); err != nil && err != http.ErrServerClosed {
		return err
	}
	return nil
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: plexd serve | plexd ctl <command> [json]")
		os.Exit(2)
	}
	switch os.Args[1] {
	case "serve":
		serve()
	case "ctl":
		os.Exit(ctl(os.Args[2:]))
	case "version":
		fmt.Println(service.Version)
	default:
		fmt.Fprintf(os.Stderr, "unknown command %q\n", os.Args[1])
		os.Exit(2)
	}
}

func unixClient(timeout time.Duration) *http.Client {
	return &http.Client{
		Timeout: timeout,
		Transport: &http.Transport{DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
			var d net.Dialer
			return d.DialContext(ctx, "unix", socketPath())
		}},
	}
}

func ctl(args []string) int {
	if len(args) == 0 {
		fmt.Fprintln(os.Stderr, "usage: plexd ctl <command> [json-body]")
		return 2
	}
	path := "/" + strings.TrimPrefix(args[0], "/")
	var body io.Reader = bytes.NewReader(nil)
	if len(args) > 1 {
		body = strings.NewReader(args[1])
	}
	resp, err := unixClient(15*time.Second).Post("http://plexd"+path, "application/json", body)
	if err != nil {
		fmt.Fprintln(os.Stderr, "plexd not running:", err)
		return 1
	}
	defer resp.Body.Close()
	out, _ := io.ReadAll(resp.Body)
	fmt.Println(string(out))
	if resp.StatusCode != http.StatusOK {
		return 1
	}
	return 0
}

func alreadyRunning() bool {
	resp, err := unixClient(2 * time.Second).Get("http://plexd/status")
	if err != nil {
		return false
	}
	resp.Body.Close()
	return true
}

func defaultFFmpeg() string {
	if exe, err := os.Executable(); err == nil {
		p := filepath.Join(filepath.Dir(exe), "ffmpeg")
		if _, err := os.Stat(p); err == nil {
			return p
		}
	}
	return "ffmpeg"
}

func serve() {
	if alreadyRunning() {
		fmt.Println("plexd already running")
		return
	}
	// Only one daemon may own the socket and the audio device.
	lock, err := os.OpenFile(env("PLEXD_LOCK", "/tmp/plexd.lock"), os.O_CREATE|os.O_RDWR, 0o644)
	if err == nil {
		defer lock.Close() // keeps the lock held (and the file reachable) until exit
		if err := syscall.Flock(int(lock.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
			fmt.Println("plexd already starting/running (lock held)")
			return
		}
	}
	if lp := os.Getenv("PLEXD_LOG"); lp != "" {
		if f, err := os.OpenFile(lp, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o644); err == nil {
			log.SetOutput(f)
		}
	}
	log.SetFlags(log.LstdFlags | log.Lmicroseconds)
	signal.Ignore(syscall.SIGHUP, syscall.SIGPIPE)

	cfgPath := env("PLEXD_CONFIG", plex.ConfigPath())
	userData := env("PLEXD_USERDATA", env("USERDATA_PATH", os.Getenv("HOME")))
	opts := service.Options{
		CacheDir:     env("PLEXD_CACHE", filepath.Join(userData, "plexmusic", "tmp")),
		ConfigPath:   cfgPath,
		SettingsPath: env("PLEXD_SETTINGS", filepath.Join(filepath.Dir(cfgPath), "plexd.json")),
		FFmpeg:       env("PLEXD_FFMPEG", defaultFFmpeg()),
		Output:       env("PLEXD_AUDIO_OUT", "alsa"),
		// NextUI sets HOME to the userdata dir; ALSA reads $HOME/.asoundrc.
		UserData: userData,
	}
	log.Printf("plexd %s starting (config=%s ffmpeg=%s out=%s)", service.Version, opts.ConfigPath, opts.FFmpeg, opts.Output)
	if err := player.Probe(context.Background(), opts.FFmpeg); err != nil {
		log.Printf("warning: %v", err)
	}
	svc := service.New(opts)
	if err := svc.StartProxy(); err != nil {
		log.Fatalf("proxy: %v", err)
	}

	hk := hotkeys.New(svc.HotkeysActive, svc.Hotkey)
	applyHotkeys := func(set service.Settings) {
		if err := hk.SetBindings(set.Hotkeys.Specs()); err != nil {
			log.Printf("hotkeys: %v", err)
		}
	}
	applyHotkeys(svc.Settings())
	svc.OnSettings(applyHotkeys)
	if os.Getenv("PLEXD_NO_HOTKEYS") == "" {
		go hk.Run()
	}

	go svc.Run()
	go func() {
		if err := svc.ServeControl(socketPath()); err != nil {
			log.Fatalf("control socket: %v", err)
		}
	}()
	go func() {
		// Not fatal: the UI connects directly when this socket is missing.
		if err := serveNet(netSocketPath(), svc.Plex.ForwardProxy(), svc.Done()); err != nil {
			log.Printf("network socket: %v", err)
		}
	}()

	idleLimit := 3 * time.Minute
	if v, err := time.ParseDuration(os.Getenv("PLEXD_IDLE_EXIT")); err == nil {
		idleLimit = v
	}

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGTERM, syscall.SIGINT)
	check := time.NewTicker(15 * time.Second)
	defer check.Stop()
	for {
		select {
		case s := <-sig:
			log.Printf("received %v, exiting", s)
			svc.Shutdown()
			svc.Quit()
		case <-svc.Done():
			_ = os.Remove(socketPath())
			log.Printf("bye")
			return
		case <-check.C:
			if idleLimit > 0 && svc.IdleFor() > idleLimit {
				log.Printf("idle for %v, exiting", idleLimit)
				svc.Quit()
			}
		}
	}
}
