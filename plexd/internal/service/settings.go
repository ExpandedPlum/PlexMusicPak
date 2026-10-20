package service

import (
	"encoding/json"
	"os"
	"path/filepath"
	"sync"
)

// Hotkeys map actions to button specs like "FN1" or "SELECT+R1"; several
// alternatives can be comma separated (see package hotkeys).
type Hotkeys struct {
	Enabled bool   `json:"enabled"`
	Toggle  string `json:"toggle"`
	Prev    string `json:"prev"`
	Next    string `json:"next"`
}

func (h Hotkeys) Specs() map[string]string {
	return map[string]string{"toggle": h.Toggle, "prev": h.Prev, "next": h.Next}
}

// Settings are plexd's own, kept next to the UI's config.json.
type Settings struct {
	Hotkeys      Hotkeys `json:"hotkeys"`
	MuteGames    bool    `json:"muteGames"`    // silence games while music plays
	PauseOnSleep bool    `json:"pauseOnSleep"` // pause when the device sleeps
	AudioDevice  string  `json:"audioDevice"`  // "" or "default" = follow NextUI's output
}

func DefaultSettings() Settings {
	return Settings{
		Hotkeys: Hotkeys{
			Enabled: true,
			Toggle:  "FN1,SELECT+START",
			Prev:    "SELECT+L1",
			Next:    "SELECT+R1",
		},
		MuteGames:    true,
		PauseOnSleep: true,
	}
}

type settingsStore struct {
	path string
	mu   sync.Mutex
	cur  Settings
}

func openSettings(path string) *settingsStore {
	st := &settingsStore{path: path, cur: DefaultSettings()}
	if b, err := os.ReadFile(path); err == nil {
		_ = json.Unmarshal(b, &st.cur) // missing keys keep their defaults
	}
	return st
}

func (st *settingsStore) Get() Settings {
	st.mu.Lock()
	defer st.mu.Unlock()
	return st.cur
}

func (st *settingsStore) Save(s Settings) error {
	st.mu.Lock()
	defer st.mu.Unlock()
	b, err := json.MarshalIndent(s, "", "  ")
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(st.path), 0o755); err != nil {
		return err
	}
	tmp := st.path + ".tmp"
	if err := os.WriteFile(tmp, b, 0o644); err != nil {
		return err
	}
	if err := os.Rename(tmp, st.path); err != nil {
		return err
	}
	st.cur = s
	return nil
}
