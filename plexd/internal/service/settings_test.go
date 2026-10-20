package service

import (
	"testing"

	"github.com/ExpandedPlum/PlexMusicPak/plexd/internal/hotkeys"
)

func TestEarphoneKeysAlwaysBound(t *testing.T) {
	var fired []string
	l := hotkeys.New(func() bool { return true }, func(a string) { fired = append(fired, a) })
	h := DefaultSettings().Hotkeys
	h.Next = "" // even with a button binding removed
	if err := l.SetBindings(h.Specs()); err != nil {
		t.Fatal(err)
	}
	for _, code := range []uint16{164, 200, 163, 165} { // PLAYPAUSE, PLAYCD, NEXTSONG, PREVIOUSSONG
		l.Handle(code, 1)
		l.Handle(code, 0)
	}
	want := []string{"toggle", "toggle", "next", "prev"}
	if len(fired) != len(want) {
		t.Fatalf("fired %v, want %v", fired, want)
	}
	for i := range want {
		if fired[i] != want[i] {
			t.Fatalf("fired %v, want %v", fired, want)
		}
	}
}
