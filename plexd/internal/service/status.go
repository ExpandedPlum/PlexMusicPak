package service

import (
	"strconv"
	"time"

	"github.com/ExpandedPlum/PlexMusicPak/plexd/internal/player"
)

type PlayerStatus struct {
	// "idle" (nothing queued), "loading", "playing", "paused", "ended"
	// (queue finished) or "error".
	State      string `json:"state"`
	QueueID    int    `json:"queueId"` // changes whenever a new queue is set
	Index      int    `json:"index"`   // current track's index in the queue's list order
	Position   int    `json:"position"`
	Count      int    `json:"count"`
	PositionMs int    `json:"positionMs"`
	DurationMs int    `json:"durationMs"`
	Shuffle    bool   `json:"shuffle"`
	Repeat     string `json:"repeat"`
	HasNext    bool   `json:"hasNext"`
	HasPrev    bool   `json:"hasPrev"`
	Track      *Track `json:"track,omitempty"`
	// Opus bitrate when the current track streams transcoded, 0 for the
	// original file (streamed or downloaded).
	TranscodeKbps int    `json:"transcodeKbps"`
	Local         bool   `json:"local"` // playing a downloaded file
	Error         string `json:"error,omitempty"`
}

type Status struct {
	Version    string          `json:"version"`
	Connection string          `json:"connection"` // "local", "remote", "relay" or "" (none yet)
	GameAudio  GameAudioStatus `json:"gameAudio"`
	Player     PlayerStatus    `json:"player"`
	Settings   Settings        `json:"settings"`
	Time       int64           `json:"time"`
}

func (s *Service) Status() Status {
	st := Status{
		Version: Version, Connection: s.Plex.Kind(), Settings: s.set.Get(),
		Time: time.Now().Unix(), GameAudio: s.gameAudioStatus(),
	}
	snap := s.Eng.Snapshot()
	held := s.inTransition()

	s.mu.Lock()
	defer s.mu.Unlock()
	ps := PlayerStatus{State: "idle", QueueID: s.queueID, Index: -1, Repeat: RepeatOff, Error: s.errMsg}
	if q := s.q; q != nil && q.Current() != nil {
		t := *q.Current()
		ps.Track = &t
		ps.Index, ps.Position, ps.Count = q.Index(), q.Pos, len(q.Order)
		ps.Shuffle, ps.Repeat = q.Shuffle, q.Repeat
		ps.HasNext, ps.HasPrev = q.HasNext(), q.HasPrev()
		ps.DurationMs = t.DurationMs
		ps.State = "paused"
		if snap.Media != nil && snap.Media.ItemID == strconv.Itoa(t.RatingKey) {
			ps.PositionMs = int(snap.Position * 1000)
			ps.State = string(snap.State)
			if len(snap.Media.Tracks) > 0 {
				tr := snap.Media.Tracks[0]
				ps.Local = tr.Source != "" && tr.Source == t.LocalPath
				if tr.SourceAt != nil {
					ps.TranscodeKbps = s.Plex.StreamKbps()
				}
			}
			if snap.Error != "" {
				ps.Error = snap.Error
			}
		} else if s.errMsg != "" {
			ps.State = string(player.StateError)
		}
		if held {
			ps.State = string(player.StatePlaying) // resumes on its own in a moment
		}
	}
	st.Player = ps
	return st
}
