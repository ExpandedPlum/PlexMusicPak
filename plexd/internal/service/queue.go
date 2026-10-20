package service

import "math/rand"

// Track is one queue entry, as the UI sends it (fields of PlexTrack).
type Track struct {
	RatingKey   int    `json:"ratingKey"`
	DurationMs  int    `json:"durationMs"`
	TrackNumber int    `json:"trackNumber,omitempty"`
	Title       string `json:"title"`
	Artist      string `json:"artist"`
	Album       string `json:"album"`
	MediaKey    string `json:"mediaKey"`
	Thumb       string `json:"thumb,omitempty"`
	LocalPath   string `json:"localPath,omitempty"`
	AudioCodec  string `json:"audioCodec,omitempty"`
	BitrateKbps int    `json:"bitrateKbps,omitempty"`
}

const (
	RepeatOff = "off"
	RepeatAll = "all"
	RepeatOne = "one"
)

// Queue is the play order over Tracks. Order maps play position to track
// index; it is the identity unless shuffled.
type Queue struct {
	Tracks  []Track
	Order   []int
	Pos     int
	Shuffle bool
	Repeat  string
}

func NewQueue(tracks []Track, start int) *Queue {
	q := &Queue{Tracks: tracks, Repeat: RepeatOff}
	q.Order = make([]int, len(tracks))
	for i := range q.Order {
		q.Order[i] = i
	}
	if start >= 0 && start < len(tracks) {
		q.Pos = start
	}
	return q
}

func (q *Queue) Current() *Track {
	if q == nil || len(q.Order) == 0 {
		return nil
	}
	return &q.Tracks[q.Order[q.Pos]]
}

// Index is the current track's index in Tracks (the UI's list order).
func (q *Queue) Index() int {
	if q == nil || len(q.Order) == 0 {
		return -1
	}
	return q.Order[q.Pos]
}

func (q *Queue) HasNext() bool {
	return q != nil && len(q.Order) > 0 && (q.Pos < len(q.Order)-1 || q.Repeat == RepeatAll)
}

func (q *Queue) HasPrev() bool { return q != nil && q.Pos > 0 }

// Next moves to the following track, wrapping around on Repeat All.
func (q *Queue) Next() bool {
	if !q.HasNext() {
		return false
	}
	q.Pos = (q.Pos + 1) % len(q.Order)
	return true
}

func (q *Queue) Prev() bool {
	if !q.HasPrev() {
		return false
	}
	q.Pos--
	return true
}

// ToggleShuffle shuffles the tracks after the current one (which keeps
// playing, now first), or goes back to list order at the current track.
func (q *Queue) ToggleShuffle(rng *rand.Rand) {
	if q == nil || len(q.Order) == 0 {
		return
	}
	cur := q.Order[q.Pos]
	if q.Shuffle {
		for i := range q.Order {
			q.Order[i] = i
		}
		q.Pos = cur
		q.Shuffle = false
		return
	}
	rest := make([]int, 0, len(q.Order)-1)
	for i := range q.Tracks {
		if i != cur {
			rest = append(rest, i)
		}
	}
	rng.Shuffle(len(rest), func(i, j int) { rest[i], rest[j] = rest[j], rest[i] })
	q.Order = append([]int{cur}, rest...)
	q.Pos = 0
	q.Shuffle = true
}

// CycleRepeat goes Off -> All -> One -> Off, like the UI's R2.
func (q *Queue) CycleRepeat() {
	switch q.Repeat {
	case RepeatOff:
		q.Repeat = RepeatAll
	case RepeatAll:
		q.Repeat = RepeatOne
	default:
		q.Repeat = RepeatOff
	}
}
