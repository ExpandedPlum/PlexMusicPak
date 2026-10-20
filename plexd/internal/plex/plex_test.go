package plex

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func load(t *testing.T, js string) Config {
	p := filepath.Join(t.TempDir(), "config.json")
	if err := os.WriteFile(p, []byte(js), 0o644); err != nil {
		t.Fatal(err)
	}
	c, err := LoadConfig(p)
	if err != nil {
		t.Fatal(err)
	}
	return c
}

func TestLoadConfigFormats(t *testing.T) {
	c := load(t, `{"token":"t","client_id":"plexmusic-1","connections":[{"uri":"https://a","kind":"local"},"https://b"]}`)
	if len(c.Connections) != 2 || c.Connections[0].Kind != KindLocal || c.Connections[1] != (Conn{"https://b", KindRemote}) {
		t.Fatalf("connections: %+v", c.Connections)
	}
	if c.ClientID != "plexmusic-1" {
		t.Fatal("client id")
	}
	// Before "connections": server_url + relay_url, and the legacy client id.
	c = load(t, `{"token":"t","server_url":"https://a","relay_url":"https://b"}`)
	if len(c.Connections) != 2 || c.Connections[1].URI != "https://b" || c.ClientID != LegacyClientID {
		t.Fatalf("legacy: %+v", c)
	}
}

func TestTranscodePath(t *testing.T) {
	p := TranscodePath(42, 128, "id", "s1", 0)
	for _, want := range []string{"musicBitrate=128", "path=%2Flibrary%2Fmetadata%2F42", "session=s1",
		"X-Plex-Client-Identifier=id", "audioCodec%3Dopus"} {
		if !strings.Contains(p, want) {
			t.Fatalf("%s missing %s", p, want)
		}
	}
	if strings.Contains(p, "offset=") || !strings.Contains(TranscodePath(42, 128, "id", "s1", 61.25), "offset=61.2") {
		t.Fatal("offset")
	}
}
