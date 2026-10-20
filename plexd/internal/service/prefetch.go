package service

import (
	"context"
	"fmt"
	"io"
	"log"
	"os"
	"path"
	"path/filepath"
	"strings"
	"time"

	"github.com/ExpandedPlum/PlexMusicPak/plexd/internal/player"
	"github.com/ExpandedPlum/PlexMusicPak/plexd/internal/plex"
)

// Prefetching: while a track plays, the next one is downloaded to the cache
// directory so it starts from a local file. That takes the network out of
// the gap between tracks (what's left is ffmpeg starting up), and on a slow
// relay connection the next track doesn't stall at its start.

const prefetchMaxBytes = 300 << 20 // skip anything bigger (long lossless mixes)

type prefetchJob struct {
	rk     int
	path   string // set once done
	kbps   int    // Opus bitrate it was fetched at; 0 = the original file
	done   bool
	cancel context.CancelFunc
}

// clearCache removes prefetched files left by an earlier run.
func (s *Service) clearCache() {
	if s.cacheDir == "" {
		return
	}
	files, _ := filepath.Glob(filepath.Join(s.cacheDir, "next-*"))
	for _, f := range files {
		_ = os.Remove(f)
	}
}

// maybePrefetch starts downloading the next track once the current one is
// half played or within 90 s of its end (so on a slow link the download
// doesn't compete with the start of the track that's playing).
func (s *Service) maybePrefetch(snap player.Snapshot) {
	if s.cacheDir == "" || snap.State != player.StatePlaying || snap.Media == nil {
		return
	}
	dur := snap.Media.Duration
	if dur <= 0 || !(snap.Position/dur >= 0.5 || dur-snap.Position <= 90) {
		return
	}
	s.mu.Lock()
	next := s.q.Peek()
	if next == nil || (s.pf != nil && s.pf.rk == next.RatingKey) ||
		(s.pfPlaying != nil && s.pfPlaying.rk == next.RatingKey) {
		s.mu.Unlock()
		return
	}
	if next.LocalPath != "" {
		if _, err := os.Stat(next.LocalPath); err == nil {
			s.mu.Unlock()
			return // downloaded already
		}
	}
	t := *next
	s.dropJobLocked(s.pf)
	ctx, cancel := context.WithCancel(context.Background())
	job := &prefetchJob{rk: t.RatingKey, cancel: cancel}
	s.pf = job
	s.mu.Unlock()
	go s.prefetch(ctx, job, t)
}

func (s *Service) prefetch(ctx context.Context, job *prefetchJob, t Track) {
	kbps := s.Plex.StreamKbps()
	ext := ".opus"
	var pq string
	switch {
	case kbps > 0:
		session := fmt.Sprintf("pm-%d-%d", t.RatingKey, time.Now().UnixNano())
		pq = plex.TranscodePath(t.RatingKey, kbps, s.Plex.Config().ClientID, session, 0)
	case t.MediaKey != "":
		pq = t.MediaKey
		ext = path.Ext(strings.SplitN(t.MediaKey, "?", 2)[0])
	default:
		return
	}
	final := filepath.Join(s.cacheDir, fmt.Sprintf("next-%d%s", t.RatingKey, ext))
	if err := s.fetchTo(ctx, pq, final); err != nil {
		if ctx.Err() == nil {
			log.Printf("prefetch %d: %v", t.RatingKey, err)
		}
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.pf != job {
		_ = os.Remove(final) // the queue moved on meanwhile
		return
	}
	job.path, job.kbps, job.done = final, kbps, true
	log.Printf("prefetched %d (%q)", t.RatingKey, t.Title)
}

func (s *Service) fetchTo(ctx context.Context, pq, final string) error {
	if err := os.MkdirAll(filepath.Dir(final), 0o755); err != nil {
		return err
	}
	resp, err := s.Plex.Get(ctx, pq, nil, true)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.ContentLength > prefetchMaxBytes {
		return fmt.Errorf("too big to prefetch (%d MB)", resp.ContentLength>>20)
	}
	tmp := final + ".part"
	f, err := os.Create(tmp)
	if err != nil {
		return err
	}
	n, err := io.Copy(f, io.LimitReader(resp.Body, prefetchMaxBytes+1))
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err == nil && n > prefetchMaxBytes {
		err = fmt.Errorf("too big to prefetch")
	}
	if err == nil && resp.ContentLength >= 0 && n != resp.ContentLength {
		err = fmt.Errorf("short download (%d of %d bytes)", n, resp.ContentLength)
	}
	if err != nil {
		_ = os.Remove(tmp)
		return err
	}
	return os.Rename(tmp, final)
}

// takePrefetch returns the downloaded copy of track rk, if there is one,
// and keeps it while it plays.
func (s *Service) takePrefetch(rk int) *prefetchJob {
	s.mu.Lock()
	defer s.mu.Unlock()
	if p := s.pfPlaying; p != nil && p.rk == rk {
		if _, err := os.Stat(p.path); err == nil {
			return p // replaying it (repeat one, previous track)
		}
	}
	if s.pf == nil || !s.pf.done || s.pf.rk != rk {
		return nil
	}
	s.dropJobLocked(s.pfPlaying)
	s.pfPlaying, s.pf = s.pf, nil
	return s.pfPlaying
}

// releasePlaying deletes the prefetched file of a track that's no longer
// playing, unless it is keep.
func (s *Service) releasePlaying(keep *prefetchJob) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.pfPlaying != nil && s.pfPlaying != keep {
		s.dropJobLocked(s.pfPlaying)
		s.pfPlaying = nil
	}
}

// dropPrefetches cancels the pending download and deletes both files (the
// queue was replaced or stopped).
func (s *Service) dropPrefetches() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.dropJobLocked(s.pf)
	s.dropJobLocked(s.pfPlaying)
	s.pf, s.pfPlaying = nil, nil
}

func (s *Service) dropJobLocked(j *prefetchJob) {
	if j == nil {
		return
	}
	j.cancel()
	if j.path != "" {
		_ = os.Remove(j.path)
	}
}
