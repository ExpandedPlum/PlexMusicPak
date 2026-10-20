// Package service is plexd's brain: it owns the play queue and the player,
// reports playback to Plex, keeps games quiet while music plays, and answers
// the UI.
//
// The structure follows Audiobookshelf.pak's daemon (absd), which does the
// same for audiobooks on the same devices.
package service

import (
	"context"
	"errors"
	"fmt"
	"log"
	"math/rand"
	"net"
	"net/http"
	"os"
	"strconv"
	"sync"
	"time"

	"github.com/ExpandedPlum/PlexMusicPak/plexd/internal/gameaudio"
	"github.com/ExpandedPlum/PlexMusicPak/plexd/internal/player"
	"github.com/ExpandedPlum/PlexMusicPak/plexd/internal/plex"
)

// Version is set at build time (-ldflags -X …service.Version=…).
var Version = "dev"

const (
	timelineInterval = 10 * time.Second
	scrobbleAt       = 0.90 // fraction played that counts as a play
	maxFailedTracks  = 3    // consecutive failures before giving up on the queue
)

type Options struct {
	CacheDir     string // where the next track is fetched ahead; "" = don't
	ConfigPath   string // the UI's config.json
	SettingsPath string // plexd.json
	UserData     string // NextUI's per-platform userdata dir (where .asoundrc lives)
	FFmpeg       string
	Output       string // "alsa" or "null" (tests)
}

type Service struct {
	Plex *plex.Client
	Eng  *player.Engine
	set  *settingsStore

	proxyBase string
	rng       *rand.Rand
	cacheDir  string

	mu           sync.Mutex
	q            *Queue
	queueID      int
	playing      int // RatingKey loaded in the engine, 0 = none
	scrobbled    bool
	lastTimeline time.Time
	failed       int
	errMsg       string
	uiSeen       time.Time
	lastBusy     time.Time
	trans        *time.Timer // pending resume after an app switch
	volTimer     *time.Timer
	pf           *prefetchJob // next track, downloading or downloaded
	pfPlaying    *prefetchJob // prefetched file the current track plays from

	quit       chan struct{}
	onSettings func(Settings)

	game        *gameaudio.Router
	gameMu      sync.Mutex
	gameErr     string
	gameMutedAt time.Time

	sinkMu      sync.Mutex
	sinkSig     string // audio routing when the music's output was opened
	sinkChecked time.Time
}

func New(opts Options) *Service {
	s := &Service{
		Plex:     plex.New(opts.ConfigPath, Version),
		set:      openSettings(opts.SettingsPath),
		rng:      rand.New(rand.NewSource(time.Now().UnixNano())),
		quit:     make(chan struct{}),
		lastBusy: time.Now(),
		cacheDir: opts.CacheDir,
	}
	s.clearCache()
	if opts.UserData != "" {
		s.game = gameaudio.New(opts.UserData)
		if conf := os.Getenv("PLEXD_ASOUND_CONF"); conf != "" { // tests
			s.game.SystemConf = conf
		}
	}
	s.Eng = player.New(player.Config{FFmpeg: opts.FFmpeg, Output: opts.Output,
		DeviceFunc: s.musicDevice}, s.onPlayerEvent)
	return s
}

// OnSettings registers a callback run after settings change.
func (s *Service) OnSettings(fn func(Settings)) { s.onSettings = fn }

func (s *Service) Settings() Settings { return s.set.Get() }

func (s *Service) SaveSettings(set Settings) error {
	if err := s.set.Save(set); err != nil {
		return err
	}
	if s.onSettings != nil {
		s.onSettings(set)
	}
	return nil
}

// StartProxy serves audio from the server to ffmpeg over loopback HTTP.
func (s *Service) StartProxy() error {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return err
	}
	s.proxyBase = "http://" + ln.Addr().String()
	srv := &http.Server{Handler: s.Plex.ProxyHandler()}
	go func() { log.Printf("proxy stopped: %v", srv.Serve(ln)) }()
	return nil
}

// Run drives the background loops until Quit.
func (s *Service) Run() {
	tick := time.NewTicker(250 * time.Millisecond)
	defer tick.Stop()
	for {
		select {
		case <-s.quit:
			return
		case now := <-tick.C:
			snap := s.Eng.Snapshot()
			s.report(snap, now)
			s.maybePrefetch(snap)
			state := snap.State
			if s.inTransition() {
				// Keep games muted while the music is held for an app
				// switch, so the new game's first sounds don't leak through.
				state = player.StatePlaying
			}
			s.reconcileGameAudio(state, now)
			s.followSink(snap.State, now)
			if snap.State == player.StatePlaying || snap.State == player.StateLoading || s.uiActive() {
				s.mu.Lock()
				s.lastBusy = now
				s.mu.Unlock()
			}
		}
	}
}

func (s *Service) Quit() {
	select {
	case <-s.quit:
	default:
		close(s.quit)
	}
}

func (s *Service) Done() <-chan struct{} { return s.quit }

// IdleFor reports how long nothing has needed the daemon. A loaded queue,
// even paused, keeps it alive so in-game buttons can resume it.
func (s *Service) IdleFor() time.Duration {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.q != nil {
		return 0
	}
	return time.Since(s.lastBusy)
}

func (s *Service) UIHeartbeat(foreground bool) {
	s.mu.Lock()
	if foreground {
		s.uiSeen = time.Now()
	} else {
		s.uiSeen = time.Time{}
	}
	s.mu.Unlock()
}

func (s *Service) uiActive() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return !s.uiSeen.IsZero() && time.Since(s.uiSeen) < 5*time.Second
}

// HotkeysActive is true when a queue is loaded and the UI isn't in front
// (the UI has its own controls).
func (s *Service) HotkeysActive() bool {
	if !s.set.Get().Hotkeys.Enabled || s.uiActive() {
		return false
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.q != nil
}

func (s *Service) Hotkey(action string) {
	var err error
	switch action {
	case "toggle":
		err = s.Toggle()
	case "prev":
		err = s.Prev()
	case "next":
		err = s.Next()
	}
	if err != nil {
		log.Printf("hotkey %s: %v", action, err)
	}
}

// ---------------------------------------------------------------------------
// Queue and playback

// SetQueue replaces the queue and starts playing tracks[start].
func (s *Service) SetQueue(tracks []Track, start int, shuffle bool) error {
	if len(tracks) == 0 {
		return errors.New("empty queue")
	}
	s.endTransition()
	s.stopCurrent("stopped")
	q := NewQueue(tracks, start)
	if shuffle {
		q.ToggleShuffle(s.rng)
	}
	s.dropPrefetches()
	s.mu.Lock()
	s.q = q
	s.queueID++
	s.failed = 0
	s.mu.Unlock()
	return s.playCurrent()
}

// playCurrent loads the queue's current track into the engine and plays it.
func (s *Service) playCurrent() error {
	s.mu.Lock()
	t := s.q.Current()
	if t == nil {
		s.mu.Unlock()
		return errors.New("nothing queued")
	}
	tr := *t
	s.mu.Unlock()

	media, err := s.media(tr)
	s.mu.Lock()
	if err != nil {
		s.errMsg = userError(err)
		s.mu.Unlock()
		return err
	}
	s.playing = tr.RatingKey
	s.scrobbled = false
	s.lastTimeline = time.Time{}
	s.errMsg = ""
	s.mu.Unlock()

	s.Eng.Load(media, 0)
	return s.Eng.Play()
}

// media builds the player input for a track: the downloaded file if there is
// one, otherwise a stream through the loopback proxy, transcoded to Opus
// when the streaming quality (or a relay connection) calls for it.
func (s *Service) media(t Track) (*player.Media, error) {
	m := &player.Media{
		ItemID: strconv.Itoa(t.RatingKey), Title: t.Title, Author: t.Artist,
		Duration: float64(t.DurationMs) / 1000,
	}
	tr := player.Track{Duration: m.Duration}
	if p := s.takePrefetch(t.RatingKey); p != nil {
		tr.Source = p.path
		m.Tracks = []player.Track{tr}
		return m, nil
	}
	s.releasePlaying(nil)
	if t.LocalPath != "" {
		if _, err := os.Stat(t.LocalPath); err == nil {
			tr.Source = t.LocalPath
			m.Tracks = []player.Track{tr}
			return m, nil
		}
		log.Printf("downloaded file missing, streaming instead: %s", t.LocalPath)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if _, _, err := s.Plex.Connection(ctx); err != nil {
		return nil, err
	}
	if kbps := s.Plex.StreamKbps(); kbps > 0 {
		clientID, rk, base := s.Plex.Config().ClientID, t.RatingKey, s.proxyBase
		tr.SourceAt = func(offset float64) string {
			// A fresh session per start, so a seek doesn't fight the
			// transcode it replaces.
			session := fmt.Sprintf("pm-%d-%d", rk, time.Now().UnixNano())
			return base + "/s" + plex.TranscodePath(rk, kbps, clientID, session, offset)
		}
	} else {
		if t.MediaKey == "" {
			return nil, errors.New("track has no media")
		}
		tr.Source = s.proxyBase + "/s" + t.MediaKey
	}
	m.Tracks = []player.Track{tr}
	return m, nil
}

func (s *Service) loaded() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.q != nil && s.q.Current() != nil
}

func (s *Service) Pause() {
	s.endTransition()
	s.Eng.Pause()
	s.timelineNow("paused")
}

func (s *Service) Resume() error {
	if !s.loaded() {
		return errors.New("nothing is playing")
	}
	s.endTransition()
	if s.Eng.Snapshot().Media == nil {
		return s.playCurrent()
	}
	return s.Eng.Play()
}

func (s *Service) Toggle() error {
	if s.inTransition() {
		s.Pause()
		return nil
	}
	switch s.Eng.Snapshot().State {
	case player.StatePlaying, player.StateLoading:
		s.Pause()
		return nil
	}
	return s.Resume()
}

func (s *Service) Seek(ms int) error {
	return s.Eng.Seek(float64(ms) / 1000)
}

func (s *Service) Skip(seconds float64) error {
	return s.Eng.Seek(s.Eng.Position() + seconds)
}

// Next and Prev move through the queue (Next wraps on Repeat All).
func (s *Service) Next() error { return s.step(true) }
func (s *Service) Prev() error { return s.step(false) }

func (s *Service) step(forward bool) error {
	s.endTransition()
	s.mu.Lock()
	if s.q == nil {
		s.mu.Unlock()
		return errors.New("nothing is playing")
	}
	moved := s.q.Prev
	if forward {
		moved = s.q.Next
	}
	s.mu.Unlock()
	s.stopCurrent("stopped")
	s.mu.Lock()
	moved() // at either end of the queue this restarts the current track
	s.failed = 0
	s.mu.Unlock()
	return s.playCurrent()
}

func (s *Service) ToggleShuffle() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.q.ToggleShuffle(s.rng)
}

func (s *Service) CycleRepeat() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.q != nil {
		s.q.CycleRepeat()
	}
}

// Stop ends playback and forgets the queue.
func (s *Service) Stop() {
	s.endTransition()
	s.stopCurrent("stopped")
	s.dropPrefetches()
	s.mu.Lock()
	s.q = nil
	s.queueID++
	s.lastBusy = time.Now()
	s.mu.Unlock()
}

// stopCurrent unloads the engine, reporting the track's final state to Plex.
func (s *Service) stopCurrent(state string) {
	s.timelineNow(state)
	s.Eng.Unload()
	s.mu.Lock()
	s.playing = 0
	s.mu.Unlock()
}

// SetVolume applies a software gain for outputs NextUI can't set the level
// of. Presses come in quick bursts; only the last one restarts the stream.
func (s *Service) SetVolume(v float64) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.volTimer != nil {
		s.volTimer.Stop()
	}
	s.volTimer = time.AfterFunc(400*time.Millisecond, func() {
		if err := s.Eng.SetVolume(v); err != nil {
			log.Printf("volume: %v", err)
		}
	})
}

func (s *Service) onPlayerEvent(ev player.Event) {
	switch ev {
	case player.EventEnded:
		go s.trackEnded(false)
	case player.EventError:
		go s.trackEnded(true)
	}
}

// trackEnded moves on after a track finishes, or fails for good (the engine
// has already retried). A failing track is skipped so a queue playing
// behind a game doesn't stall, but a run of failures (offline) stops it.
func (s *Service) trackEnded(failed bool) {
	snap := s.Eng.Snapshot()
	s.mu.Lock()
	if s.q == nil || snap.Media == nil || snap.Media.ItemID != strconv.Itoa(s.playing) {
		s.mu.Unlock()
		return
	}
	cur := *s.q.Current()
	scrobbled := s.scrobbled
	if failed {
		s.failed++
		s.errMsg = snap.Error
	} else {
		s.failed = 0
	}
	nFailed := s.failed
	s.mu.Unlock()

	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		_ = s.Plex.Timeline(ctx, cur.RatingKey, "stopped", int(snap.Position*1000), cur.DurationMs)
		if !failed && !scrobbled {
			_ = s.Plex.Scrobble(ctx, cur.RatingKey)
		}
	}()

	if nFailed >= maxFailedTracks {
		log.Printf("player: %d tracks in a row failed; stopping", nFailed)
		return
	}
	s.mu.Lock()
	again := s.q.Repeat == RepeatOne && !failed
	ok := again || s.q.Next()
	s.mu.Unlock()
	if !ok {
		log.Printf("player: end of queue")
		s.mu.Lock()
		s.playing = 0
		s.mu.Unlock()
		return
	}
	if err := s.playCurrent(); err != nil {
		log.Printf("player: next track: %v", err)
	}
}

// report sends Plex a timeline every few seconds while playing and scrobbles
// once the track is mostly played.
func (s *Service) report(snap player.Snapshot, now time.Time) {
	if snap.State != player.StatePlaying || snap.Media == nil {
		return
	}
	s.mu.Lock()
	rk, dur := s.playing, snap.Media.Duration
	due := now.Sub(s.lastTimeline) >= timelineInterval
	scrobble := !s.scrobbled && dur > 0 && snap.Position/dur >= scrobbleAt
	if due {
		s.lastTimeline = now
	}
	if scrobble {
		s.scrobbled = true
	}
	s.mu.Unlock()
	if rk == 0 || (!due && !scrobble) {
		return
	}
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		if due {
			_ = s.Plex.Timeline(ctx, rk, "playing", int(snap.Position*1000), int(dur*1000))
		}
		if scrobble {
			_ = s.Plex.Scrobble(ctx, rk)
		}
	}()
}

func (s *Service) timelineNow(state string) {
	snap := s.Eng.Snapshot()
	s.mu.Lock()
	rk := s.playing
	s.mu.Unlock()
	if rk == 0 || snap.Media == nil {
		return
	}
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
		defer cancel()
		_ = s.Plex.Timeline(ctx, rk, state, int(snap.Position*1000), int(snap.Media.Duration*1000))
	}()
}

// PreSleep runs from NextUI's pre-sleep hook: pause before the device
// suspends.
func (s *Service) PreSleep() {
	if !s.set.Get().PauseOnSleep {
		return
	}
	held := s.endTransition()
	switch st := s.Eng.Snapshot().State; {
	case held, st == player.StatePlaying, st == player.StateLoading:
		s.Pause()
	}
}

// Shutdown is called on SIGTERM or quit.
func (s *Service) Shutdown() {
	s.Stop()
	s.restoreGameAudio()
}

// ---------------------------------------------------------------------------
// App switches

// TransitionPause holds the music for d while NextUI switches apps, then
// resumes it where it stopped.
//
// Every NextUI app sets the speaker to full volume for a moment when it
// starts (libmsettings InitSettings) before restoring the user's level,
// which is a loud blip with music playing. NextUI's pre-/post-launch hooks
// call this around each launch so the music is silent through it. Calling
// it again while held restarts the wait.
func (s *Service) TransitionPause(d time.Duration) {
	s.mu.Lock()
	held := s.trans != nil
	if held {
		s.trans.Stop()
	}
	s.mu.Unlock()
	if !held {
		switch s.Eng.Snapshot().State {
		case player.StatePlaying, player.StateLoading:
		default:
			return
		}
		s.Eng.Pause()
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	var t *time.Timer
	t = time.AfterFunc(d, func() {
		s.mu.Lock()
		mine := s.trans == t
		if mine {
			s.trans = nil
		}
		s.mu.Unlock()
		if mine {
			_ = s.Eng.Play()
		}
	})
	s.trans = t
}

// endTransition cancels a pending automatic resume; it reports whether the
// music was being held for an app switch (and so counts as playing).
func (s *Service) endTransition() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.trans == nil {
		return false
	}
	s.trans.Stop()
	s.trans = nil
	return true
}

func (s *Service) inTransition() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.trans != nil
}

// userError turns errors into short messages fit for a handheld screen.
func userError(err error) string {
	var se *plex.StatusError
	var ne net.Error
	switch {
	case errors.Is(err, plex.ErrNotSignedIn), errors.Is(err, plex.ErrNoServer):
		return err.Error()
	case errors.As(err, &se) && (se.Code == http.StatusUnauthorized || se.Code == http.StatusForbidden):
		return "Plex refused the sign-in; sign in again"
	case errors.As(err, &se):
		return se.Error()
	case errors.Is(err, context.DeadlineExceeded), errors.As(err, &ne):
		return "Can't reach the Plex server"
	}
	return err.Error()
}
