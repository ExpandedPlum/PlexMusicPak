// Package gameaudio silences games (emulators) while music plays.
//
// NextUI runs every program with HOME=$USERDATA_PATH, so $HOME/.asoundrc
// decides where the ALSA default device, which games use, goes. While music
// plays, the daemon writes one that sends the default device through a
// zero-gain route into the built-in codec ("Playback" in
// /etc/asound.conf): silent, but still paced by real hardware so games run
// at normal speed. The music plays straight to the real output (speaker,
// headphones, USB DAC or Bluetooth), so it's the only thing heard and never
// has to share a USB DAC or Bluetooth link with a game. When the music stops,
// NextUI's own file is put back. Emulators watch the file and reopen their
// audio whenever it changes.
//
// Adapted from Audiobookshelf.pak, which does the same for audiobooks. A file
// written by another pak (any "# Managed by" header that isn't ours) is left
// alone: games are already silent, and stacking our file on top of theirs
// could leave games muted after both stop.
//
// NextUI's audiomon writes the file for USB and Bluetooth audio and deletes
// it on unplug. Ours carries a copy of the file it replaced so it can be
// restored; if audiomon rewrites or deletes it while music plays, that
// becomes the new original.
package gameaudio

import (
	"bytes"
	"encoding/base64"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
)

const (
	// Every file we write starts with this (older versions used other
	// endings).
	managedPrefix = "# Managed by "
	markerPrefix  = managedPrefix + "PlexMusic.pak"
	marker        = markerPrefix + ": games muted while music plays."
	origPrefix    = "# orig: "
)

var (
	ErrUnknown     = errors.New("unrecognized audio setup")
	ErrUnsupported = errors.New("this device's audio setup isn't supported")
	ErrOtherApp    = errors.New("another app is already muting games")
)

// Sink is where NextUI is sending audio.
type Sink struct {
	Kind string // "speaker" (also the headphone jack), "usb", "bluetooth", "unknown"
	ID   string // USB card or Bluetooth address
}

func (k Sink) String() string {
	if k.ID == "" {
		return k.Kind
	}
	return k.Kind + " " + k.ID
}

var (
	// audiomon's files: pcm.!default { type hw card N } for USB, and
	// defaults.bluealsa.device "MAC" for Bluetooth.
	usbFile    = regexp.MustCompile(`pcm\.!default\s*\{\s*type\s+hw\s+card\s+([A-Za-z0-9_]+)\s*\}`)
	btAddr     = regexp.MustCompile(`"([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})"`)
	legacyUSB  = regexp.MustCompile(`(?m)^# sink: usb ([A-Za-z0-9_]+)$`)
	validCard  = regexp.MustCompile(`^[A-Za-z0-9_]+$`)
	validBTDev = regexp.MustCompile(`^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$`)
)

type Router struct {
	Path       string // $USERDATA_PATH/.asoundrc
	SystemConf string // /etc/asound.conf

	mu sync.Mutex
}

func New(userdata string) *Router {
	return &Router{
		Path:       filepath.Join(userdata, ".asoundrc"),
		SystemConf: "/etc/asound.conf",
	}
}

// Supported reports whether the system config has the built-in output that
// games are silenced into.
func (r *Router) Supported() bool {
	b, err := os.ReadFile(r.SystemConf)
	return err == nil && strings.Contains(string(b), "pcm.Playback")
}

// read returns the file's content, whether it is ours or another pak's, and
// NextUI's original (nil when there is none: the built-in speaker).
func (r *Router) read() (cur []byte, ours, foreign bool, orig []byte) {
	cur, err := os.ReadFile(r.Path)
	if err != nil {
		return nil, false, false, nil
	}
	if !bytes.HasPrefix(cur, []byte(managedPrefix)) {
		if len(bytes.TrimSpace(cur)) == 0 {
			return cur, false, false, nil
		}
		return cur, false, false, cur
	}
	ours = bytes.HasPrefix(cur, []byte(markerPrefix))
	foreign = !ours
	for _, line := range strings.Split(string(cur), "\n") {
		if v, ok := strings.CutPrefix(line, origPrefix); ok {
			o, _ := base64.StdEncoding.DecodeString(strings.TrimSpace(v))
			if len(o) == 0 {
				o = nil
			}
			return cur, ours, foreign, o
		}
	}
	// Audiobookshelf.pak's older versions: speaker routing, or a shared USB DAC.
	if m := legacyUSB.FindSubmatch(cur); m != nil {
		return cur, ours, foreign, []byte(audiomonUSB(string(m[1])))
	}
	return cur, ours, foreign, nil
}

func audiomonUSB(card string) string {
	return "pcm.!default {\n    type hw\n    card " + card + "\n}\nctl.!default {\n    type hw\n    card " + card + "\n}\n"
}

func sinkOf(orig []byte) Sink {
	if orig == nil {
		return Sink{Kind: "speaker"}
	}
	s := string(orig)
	if strings.Contains(s, "bluealsa") {
		if m := btAddr.FindStringSubmatch(s); m != nil {
			return Sink{Kind: "bluetooth", ID: m[1]}
		}
		return Sink{Kind: "unknown"}
	}
	if m := usbFile.FindStringSubmatch(s); m != nil {
		return Sink{Kind: "usb", ID: m[1]}
	}
	return Sink{Kind: "unknown"}
}

// Sink returns where NextUI is sending audio, whether or not games are muted.
func (r *Router) Sink() Sink {
	_, _, _, orig := r.read()
	return sinkOf(orig)
}

// Muted reports whether we are silencing games right now.
func (r *Router) Muted() bool {
	_, ours, _, _ := r.read()
	return ours
}

// mutedConfig silences the default device and keeps NextUI's original.
// It must not look like a Bluetooth file to NextUI (which scans for
// "type bluealsa" and "defaults.bluealsa.device"), or emulators would open
// the "bluealsa" device directly instead of the default one.
func mutedConfig(orig []byte) string {
	var b strings.Builder
	b.WriteString(marker + "\n")
	if orig != nil {
		b.WriteString(origPrefix + base64.StdEncoding.EncodeToString(orig) + "\n")
	}
	b.WriteString(`# While music plays, games use a silent default device; the music
# plays to the real output directly. NextUI's own file (encoded above) is
# put back when the music stops.
pcm.!default {
    type asym
    playback.pcm "PlexSilent"
    capture.pcm "Capture"
}

pcm.PlexSilent {
    type plug
    slave.pcm {
        type route
        slave.pcm "Playback"
        slave.channels 2
        ttable.0.0 0
        ttable.1.1 0
    }
}
`)
	// Keep the mixer device and the Bluetooth address NextUI's volume
	// control relies on.
	switch k := sinkOf(orig); k.Kind {
	case "usb":
		fmt.Fprintf(&b, "\nctl.!default {\n    type hw\n    card %s\n}\n", k.ID)
	case "bluetooth":
		fmt.Fprintf(&b, "\ndefaults {\n    bluealsa {\n        device %q\n    }\n}\n", k.ID)
		b.WriteString("\nctl.!default {\n    type \"bluealsa\"\n}\n")
	}
	return b.String()
}

// SetMuted silences games (on) or gives NextUI back its own file (off). It
// reports whether the file changed. Safe to call often: it only writes when
// something differs, and picks up audiomon's changes while muted.
func (r *Router) SetMuted(on bool) (bool, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	cur, ours, foreign, orig := r.read()
	if !on {
		if !ours {
			return false, nil
		}
		if orig == nil {
			return true, os.Remove(r.Path)
		}
		return true, r.write(orig)
	}
	if foreign {
		return false, ErrOtherApp
	}
	if !r.Supported() {
		return false, ErrUnsupported
	}
	if sinkOf(orig).Kind == "unknown" {
		return false, ErrUnknown
	}
	want := mutedConfig(orig)
	if string(cur) == want {
		return false, nil
	}
	return true, r.write([]byte(want))
}

func (r *Router) write(b []byte) error {
	tmp := r.Path + ".tmp"
	if err := os.WriteFile(tmp, b, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, r.Path)
}

// MusicDevice returns the ALSA device for music: the real output,
// bypassing the default device that games are silenced through.
func (r *Router) MusicDevice() string {
	k := r.Sink()
	switch k.Kind {
	case "speaker":
		if r.Supported() {
			return "Playback"
		}
	case "usb":
		if validCard.MatchString(k.ID) {
			return "plughw:" + k.ID + ",0"
		}
	case "bluetooth":
		if validBTDev.MatchString(k.ID) {
			return "bluealsa:DEV=" + k.ID + ",PROFILE=a2dp"
		}
	}
	return "default"
}
