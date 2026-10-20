// Package player plays a track through an ffmpeg child process that decodes
// and writes straight to ALSA.
//
// One ffmpeg process plays one track. Pausing, seeking and volume changes kill
// the process and start a new one at the right offset, which also releases the
// audio device while paused so games can use it.
//
// Adapted from Audiobookshelf.pak's engine, which plays multi-file books; a
// Media here is normally one track, and the service moves through the queue.
package player

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"log"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"time"
)

type State string

const (
	StateIdle    State = "idle"
	StateLoading State = "loading"
	StatePlaying State = "playing"
	StatePaused  State = "paused"
	StateEnded   State = "ended"
	StateError   State = "error"
)

type Track struct {
	StartOffset float64
	Duration    float64
	Source      string // local file path or http URL
	// SourceAt, when set, gives a source that already starts offset seconds
	// into the track, for streams ffmpeg can't seek (server transcodes).
	SourceAt func(offset float64) string
}

type Chapter struct {
	Start float64 `json:"start"`
	End   float64 `json:"end"`
	Title string  `json:"title"`
}

type Media struct {
	ItemID    string
	EpisodeID string
	Title     string
	Author    string
	Duration  float64
	Tracks    []Track
	Chapters  []Chapter
}

type Config struct {
	FFmpeg string
	// Output is "alsa" (default) or "null" for testing without a sound card.
	Output string
	Device string
	// DeviceFunc, when set, picks the ALSA device each time playback starts.
	DeviceFunc func() string
	// SampleRate of the stream handed to ALSA.
	SampleRate int
}

type Event int

const (
	EventEnded Event = iota
	EventError
	EventStateChanged
)

type Engine struct {
	cfg     Config
	onEvent func(Event)

	mu       sync.Mutex
	media    *Media
	state    State
	pos      float64 // media position when not playing, or at process start
	speed    float64
	volume   float64 // software gain, 1 = unchanged
	gen      int     // bumped whenever the current process is abandoned
	proc     *exec.Cmd
	trackIdx int
	outTime  float64 // seconds of output the current process has produced
	started  time.Time
	retries  int
	lastErr  string
	// listened accumulates wall-clock seconds of actual playback; drained by
	// the sync loop.
	listened   float64
	listenMark time.Time
}

func New(cfg Config, onEvent func(Event)) *Engine {
	if cfg.Device == "" {
		cfg.Device = "default"
	}
	if cfg.SampleRate == 0 {
		cfg.SampleRate = 48000
	}
	if cfg.Output == "" {
		cfg.Output = "alsa"
	}
	return &Engine{cfg: cfg, onEvent: onEvent, state: StateIdle, speed: 1, volume: 1}
}

type Snapshot struct {
	State    State
	Media    *Media
	Position float64
	Speed    float64
	Error    string
}

func (e *Engine) Snapshot() Snapshot {
	e.mu.Lock()
	defer e.mu.Unlock()
	return Snapshot{State: e.state, Media: e.media, Position: e.positionLocked(), Speed: e.speed, Error: e.lastErr}
}

func (e *Engine) Position() float64 {
	e.mu.Lock()
	defer e.mu.Unlock()
	return e.positionLocked()
}

func (e *Engine) positionLocked() float64 {
	if e.media == nil {
		return 0
	}
	p := e.pos
	if e.state == StatePlaying {
		p += e.outTime * e.speed
		t := e.media.Tracks[e.trackIdx]
		if end := t.StartOffset + t.Duration; t.Duration > 0 && p > end {
			p = end
		}
	}
	if p > e.media.Duration && e.media.Duration > 0 {
		p = e.media.Duration
	}
	if p < 0 {
		p = 0
	}
	return p
}

// TakeListened returns and resets the seconds actually listened since the
// last call.
func (e *Engine) TakeListened() float64 {
	e.mu.Lock()
	defer e.mu.Unlock()
	e.markListenLocked()
	l := e.listened
	e.listened = 0
	return l
}

func (e *Engine) markListenLocked() {
	now := time.Now()
	if e.state == StatePlaying && !e.listenMark.IsZero() {
		e.listened += now.Sub(e.listenMark).Seconds()
	}
	e.listenMark = now
}

// Load replaces the current media without starting playback.
func (e *Engine) Load(m *Media, pos float64) {
	e.mu.Lock()
	defer e.mu.Unlock()
	e.stopProcLocked()
	e.markListenLocked()
	e.media = m
	e.pos = clamp(pos, 0, m.Duration)
	e.state = StatePaused
	e.lastErr = ""
	e.retries = 0
}

// Unload stops playback and forgets the media. Returns the final position.
func (e *Engine) Unload() float64 {
	e.mu.Lock()
	defer e.mu.Unlock()
	p := e.positionLocked()
	e.markListenLocked()
	e.stopProcLocked()
	e.media = nil
	e.state = StateIdle
	e.pos = 0
	return p
}

func (e *Engine) Play() error {
	e.mu.Lock()
	defer e.mu.Unlock()
	if e.media == nil {
		return errors.New("nothing loaded")
	}
	if e.state == StatePlaying || e.state == StateLoading {
		return nil
	}
	if e.state == StateEnded {
		e.pos = 0
	}
	e.retries = 0
	return e.startLocked()
}

func (e *Engine) Pause() {
	e.mu.Lock()
	defer e.mu.Unlock()
	if e.state != StatePlaying && e.state != StateLoading {
		return
	}
	e.pos = e.positionLocked()
	e.markListenLocked()
	e.stopProcLocked()
	e.state = StatePaused
	e.emitLocked(EventStateChanged)
}

func (e *Engine) Seek(pos float64) error {
	e.mu.Lock()
	defer e.mu.Unlock()
	if e.media == nil {
		return errors.New("nothing loaded")
	}
	wasPlaying := e.state == StatePlaying || e.state == StateLoading
	e.markListenLocked()
	e.stopProcLocked()
	e.pos = clamp(pos, 0, e.media.Duration)
	if e.state == StateEnded || e.state == StateError {
		e.state = StatePaused
	}
	if wasPlaying {
		return e.startLocked()
	}
	e.state = StatePaused
	e.emitLocked(EventStateChanged)
	return nil
}

func (e *Engine) SetSpeed(s float64) error {
	s = clamp(s, 0.5, 4)
	e.mu.Lock()
	if e.speed == s {
		e.mu.Unlock()
		return nil
	}
	playing := e.state == StatePlaying || e.state == StateLoading
	pos := e.positionLocked()
	e.speed = s
	e.mu.Unlock()
	if playing {
		return e.Seek(pos)
	}
	return nil
}

// SetVolume sets the software gain (0..1). Only needed for outputs whose
// level NextUI can't set (USB DACs, Bluetooth); playing audio restarts at the
// same position to apply it.
func (e *Engine) SetVolume(v float64) error {
	v = clamp(v, 0, 1)
	e.mu.Lock()
	if e.volume == v {
		e.mu.Unlock()
		return nil
	}
	playing := e.state == StatePlaying || e.state == StateLoading
	pos := e.positionLocked()
	e.volume = v
	e.mu.Unlock()
	if playing {
		return e.Seek(pos)
	}
	return nil
}

func (e *Engine) Speed() float64 {
	e.mu.Lock()
	defer e.mu.Unlock()
	return e.speed
}

func (e *Engine) emitLocked(ev Event) {
	if e.onEvent != nil {
		go e.onEvent(ev)
	}
}

func (e *Engine) stopProcLocked() {
	e.gen++
	if e.proc != nil && e.proc.Process != nil {
		_ = e.proc.Process.Kill()
	}
	e.proc = nil
	e.outTime = 0
}

func (e *Engine) trackForLocked(pos float64) int {
	tr := e.media.Tracks
	for i := len(tr) - 1; i >= 0; i-- {
		if pos >= tr[i].StartOffset-0.001 {
			return i
		}
	}
	return 0
}

func (e *Engine) args(t Track, local float64) []string {
	a := []string{"-nostdin", "-hide_banner", "-loglevel", "error", "-nostats",
		"-progress", "pipe:1", "-stats_period", "0.5"}
	if e.cfg.Output == "null" {
		// Pace like a real sound card would: output in real time.
		a = append(a, "-readrate", strconv.FormatFloat(e.speed, 'f', 2, 64))
	}
	src := t.Source
	if t.SourceAt != nil {
		src = t.SourceAt(local)
		local = 0
	}
	if strings.HasPrefix(src, "http://") || strings.HasPrefix(src, "https://") {
		a = append(a, "-reconnect", "1", "-reconnect_on_network_error", "1", "-reconnect_delay_max", "10")
	}
	if local > 0.01 {
		a = append(a, "-ss", strconv.FormatFloat(local, 'f', 3, 64))
	}
	a = append(a, "-i", src, "-vn", "-sn", "-dn")
	filters := []string{}
	if e.speed != 1 {
		filters = append(filters, "atempo="+strconv.FormatFloat(e.speed, 'f', 2, 64))
	}
	if e.volume != 1 {
		filters = append(filters, "volume="+strconv.FormatFloat(e.volume, 'f', 3, 64))
	}
	filters = append(filters, "aresample="+strconv.Itoa(e.cfg.SampleRate))
	a = append(a, "-af", strings.Join(filters, ","), "-ac", "2", "-ar", strconv.Itoa(e.cfg.SampleRate))
	if e.cfg.Output == "null" {
		a = append(a, "-f", "null", "-")
	} else {
		dev := e.cfg.Device
		if e.cfg.DeviceFunc != nil {
			dev = e.cfg.DeviceFunc()
		}
		a = append(a, "-f", "alsa", dev)
	}
	return a
}

func (e *Engine) startLocked() error {
	e.stopProcLocked()
	idx := e.trackForLocked(e.pos)
	t := e.media.Tracks[idx]
	local := e.pos - t.StartOffset
	if t.Duration > 0 && local >= t.Duration-0.05 {
		// Position is at (or past) the end of this track.
		if idx+1 >= len(e.media.Tracks) {
			e.state = StateEnded
			e.pos = e.media.Duration
			e.emitLocked(EventEnded)
			return nil
		}
		idx++
		t = e.media.Tracks[idx]
		e.pos = t.StartOffset
		local = 0
	}
	if local < 0 {
		local = 0
	}
	e.pos = t.StartOffset + local
	e.trackIdx = idx

	cmd := exec.Command(e.cfg.FFmpeg, e.args(t, local)...)
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return err
	}
	var stderr strings.Builder
	cmd.Stderr = &limitedWriter{w: &stderr, n: 4096}
	if err := cmd.Start(); err != nil {
		e.state = StateError
		e.lastErr = "cannot start ffmpeg: " + err.Error()
		e.emitLocked(EventError)
		return err
	}
	e.proc = cmd
	e.state = StateLoading
	e.started = time.Now()
	e.lastErr = ""
	e.outTime = 0
	gen := e.gen
	e.emitLocked(EventStateChanged)

	go e.readProgress(gen, stdout)
	go e.wait(gen, cmd, &stderr)
	return nil
}

func (e *Engine) readProgress(gen int, r io.Reader) {
	sc := bufio.NewScanner(r)
	for sc.Scan() {
		line := sc.Text()
		k, v, ok := strings.Cut(line, "=")
		if !ok || (k != "out_time_us" && k != "out_time_ms") {
			continue
		}
		us, err := strconv.ParseInt(strings.TrimSpace(v), 10, 64)
		if err != nil || us < 0 {
			continue
		}
		e.mu.Lock()
		if e.gen == gen {
			e.outTime = float64(us) / 1e6
			if e.state == StateLoading && us > 0 {
				e.state = StatePlaying
				e.listenMark = time.Now()
				e.retries = 0
				e.emitLocked(EventStateChanged)
			}
		}
		e.mu.Unlock()
	}
}

func (e *Engine) wait(gen int, cmd *exec.Cmd, stderr *strings.Builder) {
	err := cmd.Wait()
	e.mu.Lock()
	defer e.mu.Unlock()
	if e.gen != gen {
		return // we killed it on purpose
	}
	e.markListenLocked()
	t := e.media.Tracks[e.trackIdx]
	reached := e.pos + e.outTime*e.speed
	trackEnd := t.StartOffset + t.Duration
	nearEnd := t.Duration <= 0 || reached >= trackEnd-3

	if err == nil && nearEnd {
		e.proc = nil
		e.outTime = 0
		if e.trackIdx+1 < len(e.media.Tracks) {
			e.pos = e.media.Tracks[e.trackIdx+1].StartOffset
			if serr := e.startLocked(); serr != nil {
				log.Printf("player: next track: %v", serr)
			}
			return
		}
		e.pos = e.media.Duration
		e.state = StateEnded
		e.emitLocked(EventEnded)
		return
	}

	// Unexpected exit: network hiccup, device busy, decode error...
	msg := strings.TrimSpace(stderr.String())
	if msg == "" && err != nil {
		msg = err.Error()
	}
	if msg == "" {
		msg = "playback stopped unexpectedly"
	}
	log.Printf("player: ffmpeg exited at %.1fs (track %d): %s", reached, e.trackIdx, msg)
	e.pos = clamp(reached, 0, e.media.Duration)
	e.proc = nil
	e.outTime = 0
	if e.retries < 3 {
		e.retries++
		delay := time.Duration(e.retries*2) * time.Second
		e.state = StateLoading
		e.gen++
		retryGen := e.gen
		go func() {
			time.Sleep(delay)
			e.mu.Lock()
			defer e.mu.Unlock()
			if e.gen == retryGen && e.state == StateLoading {
				if err := e.startLocked(); err != nil {
					log.Printf("player: retry failed: %v", err)
				}
			}
		}()
		return
	}
	e.state = StateError
	e.lastErr = friendlyError(msg)
	e.emitLocked(EventError)
}

func friendlyError(msg string) string {
	l := strings.ToLower(msg)
	switch {
	case strings.Contains(l, "device or resource busy"):
		return "Audio device is busy"
	case strings.Contains(l, "connection refused"), strings.Contains(l, "timed out"),
		strings.Contains(l, "network"), strings.Contains(l, "i/o error"):
		return "Lost connection to server"
	case strings.Contains(l, "no such file"):
		return "Audio file missing"
	}
	if len(msg) > 120 {
		msg = msg[:120]
	}
	return msg
}

func clamp(v, lo, hi float64) float64 {
	if hi > lo && v > hi {
		return hi
	}
	if v < lo {
		return lo
	}
	return v
}

type limitedWriter struct {
	w io.Writer
	n int
}

func (l *limitedWriter) Write(p []byte) (int, error) {
	n := len(p)
	if l.n > 0 {
		q := p
		if len(q) > l.n {
			q = q[:l.n]
		}
		l.n -= len(q)
		_, _ = l.w.Write(q)
	}
	return n, nil
}

// Probe checks that ffmpeg can be executed, for a clear error at startup.
func Probe(ctx context.Context, ffmpeg string) error {
	cmd := exec.CommandContext(ctx, ffmpeg, "-hide_banner", "-version")
	if out, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("ffmpeg not usable (%s): %v %s", ffmpeg, err, strings.TrimSpace(string(out)))
	}
	return nil
}
