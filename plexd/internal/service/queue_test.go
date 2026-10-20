package service

import (
	"math/rand"
	"testing"
)

func tracks(n int) []Track {
	t := make([]Track, n)
	for i := range t {
		t[i].RatingKey = 100 + i
	}
	return t
}

func TestQueueNextPrevRepeat(t *testing.T) {
	q := NewQueue(tracks(3), 1)
	if q.Current().RatingKey != 101 || !q.HasPrev() || !q.HasNext() {
		t.Fatal("start position")
	}
	q.Next()
	if q.HasNext() || q.Next() {
		t.Fatal("end of queue should stop with repeat off")
	}
	q.CycleRepeat() // all
	if !q.Next() || q.Current().RatingKey != 100 || q.HasPrev() {
		t.Fatal("repeat all should wrap to the first track")
	}
	q.CycleRepeat() // one
	if q.Repeat != RepeatOne {
		t.Fatal("repeat cycle")
	}
	q.CycleRepeat()
	if q.Repeat != RepeatOff {
		t.Fatal("repeat cycle back to off")
	}
}

func TestQueueShuffleKeepsCurrent(t *testing.T) {
	q := NewQueue(tracks(10), 4)
	q.ToggleShuffle(rand.New(rand.NewSource(1)))
	if q.Current().RatingKey != 104 || q.Pos != 0 || !q.Shuffle {
		t.Fatal("current track should stay current and come first")
	}
	seen := map[int]bool{}
	for _, i := range q.Order {
		seen[i] = true
	}
	if len(seen) != 10 {
		t.Fatal("shuffle lost or duplicated tracks")
	}
	q.Next()
	q.Next()
	at := q.Current().RatingKey
	q.ToggleShuffle(nil)
	if q.Shuffle || q.Current().RatingKey != at || q.Index() != at-100 {
		t.Fatal("unshuffle should keep the current track")
	}
	for i, v := range q.Order {
		if i != v {
			t.Fatal("unshuffle should restore list order")
		}
	}
}

func TestEmptyQueue(t *testing.T) {
	var q *Queue
	if q.Current() != nil || q.HasNext() || q.HasPrev() || q.Index() != -1 {
		t.Fatal("nil queue")
	}
	q = NewQueue(nil, 0)
	if q.Current() != nil || q.HasNext() {
		t.Fatal("empty queue")
	}
}
