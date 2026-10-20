package hotkeys

import "testing"

func TestParse(t *testing.T) {
	b, err := Parse("toggle", "FN1, SELECT+START")
	if err != nil {
		t.Fatal(err)
	}
	if len(b) != 2 {
		t.Fatalf("got %d bindings, want 2", len(b))
	}
	if len(b[0].Mods) != 0 || b[0].Trigger != 317 {
		t.Errorf("FN1 parsed as %+v", b[0])
	}
	if len(b[1].Mods) != 1 || b[1].Mods[0] != 314 || b[1].Trigger != 315 {
		t.Errorf("SELECT+START parsed as %+v", b[1])
	}
	if _, err := Parse("x", "SELECT+NOPE"); err == nil {
		t.Error("expected error for unknown button")
	}
}

func TestChordBeatsSingleButton(t *testing.T) {
	var fired []string
	l := New(func() bool { return true }, func(a string) { fired = append(fired, a) })
	if err := l.SetBindings(map[string]string{"toggle": "R1", "forward": "SELECT+R1"}); err != nil {
		t.Fatal(err)
	}
	l.handle(314, 1) // SELECT down
	l.handle(311, 1) // R1 down
	l.handle(311, 0)
	l.handle(314, 0)
	l.handle(311, 1) // R1 alone
	if len(fired) != 2 || fired[0] != "forward" || fired[1] != "toggle" {
		t.Fatalf("fired %v, want [forward toggle]", fired)
	}
}

func TestDisabledDoesNotFire(t *testing.T) {
	fired := 0
	l := New(func() bool { return false }, func(string) { fired++ })
	_ = l.SetBindings(map[string]string{"toggle": "FN1"})
	l.handle(317, 1)
	if fired != 0 {
		t.Fatal("hotkey fired while disabled")
	}
}
