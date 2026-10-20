// Package hotkeys watches the gamepad for buttons or button chords (e.g. FN1,
// or SELECT+R1) so playback can be controlled while a game is running.
// Devices are read without grabbing them, so games and NextUI still receive
// every press.
package hotkeys

import (
	"encoding/binary"
	"fmt"
	"log"
	"os"
	"strings"
	"sync"
	"time"
)

// Linux input event codes as reported by the TrimUI gamepad. The order of
// these codes is what produces NextUI's JOY_* indices for tg5040 (B=0, A=1,
// Y=2, X=3, L1=4, R1=5, SELECT=6, START=7, MENU=8, FN1/L3=9, FN2/R3=10).
var buttonCodes = map[string]uint16{
	"B": 304, "A": 305, "Y": 307, "X": 308,
	"L1": 310, "R1": 311, "L2": 312, "R2": 313,
	"SELECT": 314, "START": 315, "MENU": 316,
	"FN1": 317, "FN2": 318, "L3": 317, "R3": 318,
	// Media keys on USB-C and Bluetooth (AVRCP) earphones
	"NEXTSONG": 163, "PLAYPAUSE": 164, "PREVIOUSSONG": 165, "PLAYCD": 200, "PAUSECD": 201,
}

type Binding struct {
	Mods    []uint16
	Trigger uint16
	Action  string
}

// Parse turns "SELECT+R1" or "FN1,SELECT+START" into bindings for action.
func Parse(action, spec string) ([]Binding, error) {
	var out []Binding
	for _, combo := range strings.Split(spec, ",") {
		combo = strings.TrimSpace(combo)
		if combo == "" {
			continue
		}
		parts := strings.Split(combo, "+")
		var codes []uint16
		for _, p := range parts {
			c, ok := buttonCodes[strings.ToUpper(strings.TrimSpace(p))]
			if !ok {
				return nil, fmt.Errorf("unknown button %q in %q", p, combo)
			}
			codes = append(codes, c)
		}
		out = append(out, Binding{Mods: codes[:len(codes)-1], Trigger: codes[len(codes)-1], Action: action})
	}
	return out, nil
}

type Listener struct {
	mu       sync.Mutex
	bindings []Binding
	held     map[uint16]bool
	enabled  func() bool
	fire     func(action string)
	opened   map[string]bool
}

func New(enabled func() bool, fire func(string)) *Listener {
	return &Listener{held: map[uint16]bool{}, enabled: enabled, fire: fire, opened: map[string]bool{}}
}

// SetBindings replaces the table from an action -> spec map.
func (l *Listener) SetBindings(specs map[string]string) error {
	var all []Binding
	for action, spec := range specs {
		b, err := Parse(action, spec)
		if err != nil {
			return err
		}
		all = append(all, b...)
	}
	// Prefer chords over single buttons when both match.
	for i := 1; i < len(all); i++ {
		for j := i; j > 0 && len(all[j].Mods) > len(all[j-1].Mods); j-- {
			all[j], all[j-1] = all[j-1], all[j]
		}
	}
	l.mu.Lock()
	l.bindings = all
	l.mu.Unlock()
	return nil
}

// Run opens input devices and keeps rescanning for new ones (USB/BT pads).
func (l *Listener) Run() {
	for {
		for i := 0; i < 16; i++ {
			path := fmt.Sprintf("/dev/input/event%d", i)
			l.mu.Lock()
			already := l.opened[path]
			l.mu.Unlock()
			if already {
				continue
			}
			f, err := os.Open(path)
			if err != nil {
				continue
			}
			l.mu.Lock()
			l.opened[path] = true
			l.mu.Unlock()
			go l.read(path, f)
		}
		time.Sleep(10 * time.Second)
	}
}

// struct input_event on 64-bit Linux: timeval (16 bytes), type u16, code u16, value s32.
const eventSize = 24

func (l *Listener) read(path string, f *os.File) {
	defer func() {
		f.Close()
		l.mu.Lock()
		delete(l.opened, path)
		l.mu.Unlock()
	}()
	buf := make([]byte, eventSize*16)
	for {
		n, err := f.Read(buf)
		if err != nil {
			return
		}
		for off := 0; off+eventSize <= n; off += eventSize {
			typ := binary.LittleEndian.Uint16(buf[off+16:])
			code := binary.LittleEndian.Uint16(buf[off+18:])
			val := int32(binary.LittleEndian.Uint32(buf[off+20:]))
			if typ == 1 { // EV_KEY
				l.handle(code, val)
			}
		}
	}
}

// Handle feeds one key event (value 1 = press, 0 = release, 2 = repeat), as
// read from an input device; exported for tests elsewhere.
func (l *Listener) Handle(code uint16, val int32) { l.handle(code, val) }

func (l *Listener) handle(code uint16, val int32) {
	l.mu.Lock()
	switch val {
	case 0:
		delete(l.held, code)
		l.mu.Unlock()
		return
	case 2: // autorepeat
		l.mu.Unlock()
		return
	}
	l.held[code] = true
	var action string
	for _, b := range l.bindings {
		if b.Trigger != code {
			continue
		}
		ok := true
		for _, m := range b.Mods {
			if !l.held[m] {
				ok = false
				break
			}
		}
		if ok {
			action = b.Action
			break
		}
	}
	l.mu.Unlock()
	if action != "" && l.enabled() {
		log.Printf("hotkey: %s", action)
		l.fire(action)
	}
}
