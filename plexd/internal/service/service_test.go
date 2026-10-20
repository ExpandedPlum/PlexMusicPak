package service

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/ExpandedPlum/PlexMusicPak/plexd/internal/plex"
)

// fakePlex is a Plex Media Server stand-in: one track file, a transcoder
// that serves an Opus file, and the timeline and scrobble endpoints.
type fakePlex struct {
	*httptest.Server
	mu        sync.Mutex
	timelines []string // "ratingKey state"
	scrobbles []string
	offsets   []string // transcode start offsets
	tokens    []string
	parts     []string // track file requests
}

func (f *fakePlex) count(list *[]string, v string) int {
	f.mu.Lock()
	defer f.mu.Unlock()
	n := 0
	for _, x := range *list {
		if x == v {
			n++
		}
	}
	return n
}

func (f *fakePlex) record(list *[]string, v string) {
	f.mu.Lock()
	defer f.mu.Unlock()
	*list = append(*list, v)
}

func (f *fakePlex) has(list *[]string, v string) bool {
	f.mu.Lock()
	defer f.mu.Unlock()
	for _, x := range *list {
		if x == v {
			return true
		}
	}
	return false
}

func newFakePlex(t *testing.T, mp3, opus string) *fakePlex {
	f := &fakePlex{}
	mux := http.NewServeMux()
	mux.HandleFunc("/identity", func(w http.ResponseWriter, r *http.Request) { _, _ = w.Write([]byte("{}")) })
	mux.HandleFunc("/library/parts/", func(w http.ResponseWriter, r *http.Request) {
		f.record(&f.tokens, r.Header.Get("X-Plex-Token"))
		f.record(&f.parts, r.URL.Path)
		http.ServeFile(w, r, mp3)
	})
	mux.HandleFunc("/audio/:/transcode/universal/start", func(w http.ResponseWriter, r *http.Request) {
		f.record(&f.offsets, r.URL.Query().Get("offset"))
		http.ServeFile(w, r, opus)
	})
	mux.HandleFunc("/:/timeline", func(w http.ResponseWriter, r *http.Request) {
		q := r.URL.Query()
		f.record(&f.timelines, q.Get("ratingKey")+" "+q.Get("state"))
	})
	mux.HandleFunc("/:/scrobble", func(w http.ResponseWriter, r *http.Request) {
		f.record(&f.scrobbles, r.URL.Query().Get("key"))
	})
	f.Server = httptest.NewServer(mux)
	t.Cleanup(f.Close)
	return f
}

func fixture(t *testing.T, dir, name string, args ...string) string {
	out := filepath.Join(dir, name)
	cmd := exec.Command("ffmpeg", append([]string{"-hide_banner", "-loglevel", "error", "-y",
		"-f", "lavfi", "-i", "sine=frequency=440:duration=2"}, append(args, out)...)...)
	if b, err := cmd.CombinedOutput(); err != nil {
		t.Skipf("ffmpeg can't make test audio: %v %s", err, b)
	}
	return out
}

func writeConfig(t *testing.T, path string, cfg map[string]any) {
	b, _ := json.Marshal(cfg)
	// A distinct mtime each write, so Reload notices.
	if err := os.WriteFile(path, b, 0o644); err != nil {
		t.Fatal(err)
	}
	later := time.Now().Add(time.Duration(len(b)) * time.Millisecond)
	_ = os.Chtimes(path, later, later)
}

func waitFor(t *testing.T, what string, d time.Duration, ok func() bool) {
	t.Helper()
	deadline := time.Now().Add(d)
	for time.Now().Before(deadline) {
		if ok() {
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

func newTestService(t *testing.T) (*Service, *fakePlex, string) {
	if _, err := exec.LookPath("ffmpeg"); err != nil {
		t.Skip("ffmpeg not installed")
	}
	dir := t.TempDir()
	f := newFakePlex(t, fixture(t, dir, "t.mp3", "-c:a", "libmp3lame"),
		fixture(t, dir, "t.opus", "-c:a", "libopus"))
	cfgPath := filepath.Join(dir, "config.json")
	writeConfig(t, cfgPath, map[string]any{
		"token":     "tok",
		"client_id": "plexmusic-test",
		"connections": []map[string]string{
			{"uri": "http://127.0.0.1:1", "kind": "local"}, // nothing listens here
			{"uri": f.URL, "kind": "remote"},
		},
	})
	s := New(Options{ConfigPath: cfgPath, SettingsPath: filepath.Join(dir, "plexd.json"),
		CacheDir: filepath.Join(dir, "cache"), FFmpeg: "ffmpeg", Output: "null"})
	if err := s.StartProxy(); err != nil {
		t.Fatal(err)
	}
	go s.Run()
	t.Cleanup(func() { s.Shutdown(); s.Quit() })
	return s, f, cfgPath
}

func TestQueuePlaysThroughAndReports(t *testing.T) {
	s, f, _ := newTestService(t)
	tracks := []Track{
		{RatingKey: 1, DurationMs: 2000, Title: "One", MediaKey: "/library/parts/1/one.mp3"},
		{RatingKey: 2, DurationMs: 2000, Title: "Two", MediaKey: "/library/parts/2/two.mp3"},
	}
	if err := s.SetQueue(tracks, 0, false); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "first track playing", 5*time.Second, func() bool {
		st := s.Status()
		return st.Player.State == "playing" && st.Player.Track.RatingKey == 1
	})
	if s.Status().Connection != plex.KindRemote {
		t.Fatalf("connection: %q (the dead local address should be skipped)", s.Status().Connection)
	}
	waitFor(t, "second track playing", 8*time.Second, func() bool {
		st := s.Status()
		return st.Player.Track.RatingKey == 2 && st.Player.State == "playing"
	})
	waitFor(t, "end of queue", 8*time.Second, func() bool { return s.Status().Player.State == "ended" })
	waitFor(t, "reports", 3*time.Second, func() bool {
		return f.has(&f.timelines, "1 stopped") && f.has(&f.scrobbles, "1") && f.has(&f.scrobbles, "2")
	})
	if !f.has(&f.tokens, "tok") {
		t.Fatal("proxy should add the token")
	}
	st := s.Status().Player
	if st.HasNext || !st.HasPrev || st.Index != 1 {
		t.Fatalf("queue position at end: %+v", st)
	}
}

func TestTranscodeSeeksOnTheServer(t *testing.T) {
	s, f, cfgPath := newTestService(t)
	writeConfig(t, cfgPath, map[string]any{
		"token": "tok", "stream_bitrate_kbps": 96,
		"connections": []map[string]string{{"uri": f.URL, "kind": "local"}},
	})
	if err := s.SetQueue([]Track{{RatingKey: 7, DurationMs: 2000}}, 0, false); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "playing", 5*time.Second, func() bool { return s.Status().Player.State == "playing" })
	if st := s.Status().Player; st.TranscodeKbps != 96 {
		t.Fatalf("transcode kbps %d", st.TranscodeKbps)
	}
	if err := s.Seek(1000); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "seek on the transcoder", 3*time.Second, func() bool { return f.has(&f.offsets, "1.0") })
}

func TestRelayCapsOriginalQuality(t *testing.T) {
	s, f, cfgPath := newTestService(t)
	writeConfig(t, cfgPath, map[string]any{
		"token":       "tok",
		"connections": []map[string]string{{"uri": f.URL, "kind": "relay"}},
	})
	if err := s.SetQueue([]Track{{RatingKey: 3, DurationMs: 2000, MediaKey: "/library/parts/3/x.flac"}}, 0, false); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "playing", 5*time.Second, func() bool { return s.Status().Player.State == "playing" })
	if st := s.Status(); st.Connection != plex.KindRelay || st.Player.TranscodeKbps != plex.RelayStreamKbps {
		t.Fatalf("relay should stream Opus: %+v", st)
	}
}

func TestLocalFileAndControls(t *testing.T) {
	s, _, _ := newTestService(t)
	dir := t.TempDir()
	local := fixture(t, dir, "local.mp3", "-c:a", "libmp3lame")
	tracks := []Track{
		{RatingKey: 10, DurationMs: 2000, LocalPath: local},
		{RatingKey: 11, DurationMs: 2000, LocalPath: local},
		{RatingKey: 12, DurationMs: 2000, LocalPath: local},
	}
	if err := s.SetQueue(tracks, 1, false); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "playing", 5*time.Second, func() bool { return s.Status().Player.State == "playing" })
	if st := s.Status().Player; !st.Local || st.Index != 1 {
		t.Fatalf("local playback: %+v", st)
	}
	if err := s.Toggle(); err != nil || s.Status().Player.State != "paused" {
		t.Fatalf("toggle to pause: %v %s", err, s.Status().Player.State)
	}
	if err := s.Next(); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "next track", 5*time.Second, func() bool {
		st := s.Status().Player
		return st.Index == 2 && st.State == "playing"
	})
	s.CycleRepeat()
	if s.Status().Player.Repeat != RepeatAll || !s.Status().Player.HasNext {
		t.Fatal("repeat all")
	}
	if err := s.Prev(); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "previous track", 5*time.Second, func() bool { return s.Status().Player.Index == 1 })
	s.Stop()
	if st := s.Status().Player; st.State != "idle" || st.Track != nil {
		t.Fatalf("stop: %+v", st)
	}
	if !strings.Contains(s.Status().Version, "dev") {
		t.Fatal("version")
	}
}

func TestNextTrackIsPrefetched(t *testing.T) {
	s, f, _ := newTestService(t)
	tracks := []Track{
		{RatingKey: 1, DurationMs: 2000, MediaKey: "/library/parts/1/one.mp3"},
		{RatingKey: 2, DurationMs: 2000, MediaKey: "/library/parts/2/two.mp3"},
	}
	if err := s.SetQueue(tracks, 0, false); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "second track playing", 8*time.Second, func() bool {
		st := s.Status().Player
		return st.Track.RatingKey == 2 && st.State == "playing"
	})
	if !s.Status().Player.Prefetched {
		t.Fatal("the second track should play from its prefetched copy")
	}
	// Fetched once ahead of time; the player read the local copy.
	if n := f.count(&f.parts, "/library/parts/2/two.mp3"); n != 1 {
		t.Fatalf("track 2 requested %d times", n)
	}
	s.Stop()
	left, _ := filepath.Glob(filepath.Join(s.cacheDir, "*"))
	if len(left) != 0 {
		t.Fatalf("stop should delete prefetched files: %v", left)
	}
}
