// Package plex talks to the Plex Media Server for plexd: it reads the
// sign-in and connection list the PlexMusic UI saves in config.json, picks a
// connection that answers, and builds stream, timeline and scrobble requests.
//
// HTTPS certificates are verified (plex.direct and plex.tv use public CAs).
package plex

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"sync"
	"time"

	// Mozilla CA bundle for devices with no system certificates.
	_ "golang.org/x/crypto/x509roots/fallback"
)

// LegacyClientID is the X-Plex-Client-Identifier every install sent before
// per-device ids; tokens issued to it keep using it (see plex_config.c).
const LegacyClientID = "nextui-plex-music"

// Kinds of connection, as plex.tv reports them and config.json stores them.
const (
	KindLocal  = "local"
	KindRemote = "remote"
	KindRelay  = "relay"
)

// RelayStreamKbps replaces Original quality on a relay connection, whose
// bandwidth Plex caps at about 1-2 Mbps (PLEX_RELAY_STREAM_KBPS in the UI).
const RelayStreamKbps = 128

var (
	ErrNotSignedIn = errors.New("not signed in to Plex")
	ErrNoServer    = errors.New("can't reach the Plex server")
)

type Conn struct {
	URI  string `json:"uri"`
	Kind string `json:"kind"`
}

// UnmarshalJSON accepts {"uri","kind"} objects and bare URI strings (from
// earlier builds of the UI).
func (c *Conn) UnmarshalJSON(b []byte) error {
	var s string
	if json.Unmarshal(b, &s) == nil {
		*c = Conn{URI: s, Kind: KindRemote}
		return nil
	}
	type plain Conn
	var p plain
	if err := json.Unmarshal(b, &p); err != nil {
		return err
	}
	*c = Conn(p)
	if c.Kind == "" {
		c.Kind = KindRemote
	}
	return nil
}

// Config is the part of the UI's config.json plexd uses.
type Config struct {
	Token       string `json:"token"`
	ServerURL   string `json:"server_url"`
	RelayURL    string `json:"relay_url"` // configs from before "connections"
	Connections []Conn `json:"connections"`
	ClientID    string `json:"client_id"`
	StreamKbps  int    `json:"stream_bitrate_kbps"`
}

// ConfigPath is where the UI keeps config.json.
func ConfigPath() string {
	base := os.Getenv("SHARED_USERDATA_PATH")
	if base == "" {
		base = "/mnt/SDCARD/.userdata/shared"
	}
	return filepath.Join(base, "plexmusic", "config.json")
}

func LoadConfig(path string) (Config, error) {
	var c Config
	b, err := os.ReadFile(path)
	if err != nil {
		return c, err
	}
	if err := json.Unmarshal(b, &c); err != nil {
		return c, err
	}
	if len(c.Connections) == 0 {
		for _, u := range []string{c.ServerURL, c.RelayURL} {
			if u != "" && (len(c.Connections) == 0 || c.Connections[0].URI != u) {
				c.Connections = append(c.Connections, Conn{URI: u, Kind: KindRemote})
			}
		}
	}
	if c.ClientID == "" {
		c.ClientID = LegacyClientID
	}
	return c, nil
}

// Client is safe for concurrent use.
type Client struct {
	Product, Version string

	cfgPath string
	tr      *http.Transport
	http    *http.Client
	stream  *http.Client // no overall timeout: audio streams for minutes
	forward *http.Client // for the UI's requests (see ForwardProxy)

	mu       sync.Mutex
	cfg      Config
	cfgMod   time.Time
	base     string // selected connection URI; "" = none yet
	kind     string
	selected time.Time
}

// plexDirectIP matches the IP that a plex.direct hostname encodes:
// 192-168-1-5.<hash>.plex.direct -> 192.168.1.5.
var plexDirectIP = regexp.MustCompile(`^(\d{1,3})-(\d{1,3})-(\d{1,3})-(\d{1,3})\.[0-9a-f]+\.plex\.direct$`)

// dialPlexDirect falls back to the encoded IP when a plex.direct name doesn't
// resolve: routers with DNS-rebinding protection refuse names that point at
// private addresses. TLS still checks the certificate against the name.
func dialPlexDirect(dial func(ctx context.Context, network, addr string) (net.Conn, error)) func(ctx context.Context, network, addr string) (net.Conn, error) {
	return func(ctx context.Context, network, addr string) (net.Conn, error) {
		conn, err := dial(ctx, network, addr)
		var dnsErr *net.DNSError
		if err == nil || !errors.As(err, &dnsErr) {
			return conn, err
		}
		host, port, serr := net.SplitHostPort(addr)
		m := plexDirectIP.FindStringSubmatch(host)
		if serr != nil || m == nil {
			return conn, err
		}
		ip := net.ParseIP(m[1] + "." + m[2] + "." + m[3] + "." + m[4])
		if ip == nil {
			return conn, err
		}
		log.Printf("plex: %s doesn't resolve; connecting to %s", host, ip)
		return dial(ctx, network, net.JoinHostPort(ip.String(), port))
	}
}

func New(cfgPath, version string) *Client {
	t := http.DefaultTransport.(*http.Transport).Clone()
	t.DialContext = dialPlexDirect((&net.Dialer{Timeout: 10 * time.Second, KeepAlive: 30 * time.Second}).DialContext)
	t.TLSHandshakeTimeout = 8 * time.Second
	t.ResponseHeaderTimeout = 30 * time.Second
	return &Client{
		Product: "PlexMusic", Version: version, cfgPath: cfgPath, tr: t,
		http:    &http.Client{Transport: t, Timeout: 20 * time.Second},
		stream:  &http.Client{Transport: t},
		forward: &http.Client{Transport: t},
	}
}

// Reload re-reads config.json when it changed. A changed token or connection
// list drops the selected connection.
func (c *Client) Reload() error {
	fi, err := os.Stat(c.cfgPath)
	if err != nil {
		return err
	}
	c.mu.Lock()
	same := fi.ModTime().Equal(c.cfgMod)
	c.mu.Unlock()
	if same {
		return nil
	}
	cfg, err := LoadConfig(c.cfgPath)
	if err != nil {
		return err
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if cfg.Token != c.cfg.Token || !sameConns(cfg.Connections, c.cfg.Connections) {
		c.base, c.kind = "", ""
	}
	c.cfg, c.cfgMod = cfg, fi.ModTime()
	return nil
}

func sameConns(a, b []Conn) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func (c *Client) Config() Config {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.cfg
}

// Connection returns the selected connection, selecting one if needed.
func (c *Client) Connection(ctx context.Context) (base, kind string, err error) {
	_ = c.Reload()
	c.mu.Lock()
	base, kind = c.base, c.kind
	c.mu.Unlock()
	if base != "" {
		return base, kind, nil
	}
	return c.Reselect(ctx)
}

// Kind of the selected connection, "" when none is selected yet.
func (c *Client) Kind() string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.kind
}

// Reselect probes every connection at once and picks the best-ranked one that
// answers (config.json lists them best first). It waits at most one probe
// timeout, not one per dead address.
func (c *Client) Reselect(ctx context.Context) (string, string, error) {
	_ = c.Reload()
	cfg := c.Config()
	if cfg.Token == "" {
		return "", "", ErrNotSignedIn
	}
	if len(cfg.Connections) == 0 {
		return "", "", ErrNoServer
	}
	ctx, cancel := context.WithTimeout(ctx, 3*time.Second)
	defer cancel()
	results := make([]chan bool, len(cfg.Connections))
	for i, conn := range cfg.Connections {
		results[i] = make(chan bool, 1)
		go func(ch chan bool, uri string) { ch <- c.probe(ctx, uri, cfg) }(results[i], conn.URI)
	}
	for i, ch := range results {
		if <-ch {
			c.mu.Lock()
			c.base, c.kind, c.selected = cfg.Connections[i].URI, cfg.Connections[i].Kind, time.Now()
			c.mu.Unlock()
			log.Printf("plex: using connection %d/%d (%s): %s", i+1, len(cfg.Connections),
				cfg.Connections[i].Kind, cfg.Connections[i].URI)
			return cfg.Connections[i].URI, cfg.Connections[i].Kind, nil
		}
	}
	log.Printf("plex: no connection answered")
	return "", "", ErrNoServer
}

func (c *Client) probe(ctx context.Context, base string, cfg Config) bool {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, base+"/identity", nil)
	if err != nil {
		return false
	}
	c.setHeaders(req, cfg)
	resp, err := c.http.Do(req)
	if err != nil {
		log.Printf("plex: %s unreachable: %v", base, err)
		return false
	}
	_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, 4096))
	resp.Body.Close()
	return resp.StatusCode < 400
}

func (c *Client) setHeaders(req *http.Request, cfg Config) {
	h := req.Header
	h.Set("Accept", "application/json")
	h.Set("X-Plex-Token", cfg.Token)
	h.Set("X-Plex-Client-Identifier", cfg.ClientID)
	h.Set("X-Plex-Product", c.Product)
	h.Set("X-Plex-Version", c.Version)
	h.Set("X-Plex-Platform", "Chrome") // matches the UI (plex_net.c)
	h.Set("X-Plex-Device-Name", "NextUI")
}

// StreamKbps is the bitrate to request: the UI setting, except Original (0)
// becomes RelayStreamKbps on a relay connection.
func (c *Client) StreamKbps() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.cfg.StreamKbps == 0 && c.kind == KindRelay {
		return RelayStreamKbps
	}
	return c.cfg.StreamKbps
}

// Get requests pathAndQuery on the server. A transport failure re-selects the
// connection once (the network may have changed) and retries.
func (c *Client) Get(ctx context.Context, pathAndQuery string, hdr http.Header, streaming bool) (*http.Response, error) {
	resp, err := c.get(ctx, pathAndQuery, hdr, streaming)
	if err == nil || ctx.Err() != nil || errors.Is(err, ErrNotSignedIn) {
		return resp, err
	}
	var se *StatusError
	if errors.As(err, &se) {
		return nil, err
	}
	c.mu.Lock()
	recent := time.Since(c.selected) < 5*time.Second
	c.mu.Unlock()
	if recent {
		return nil, err
	}
	if _, _, rerr := c.Reselect(ctx); rerr != nil {
		return nil, err
	}
	return c.get(ctx, pathAndQuery, hdr, streaming)
}

type StatusError struct{ Code int }

func (e *StatusError) Error() string { return fmt.Sprintf("Plex server returned HTTP %d", e.Code) }

func (c *Client) get(ctx context.Context, pathAndQuery string, hdr http.Header, streaming bool) (*http.Response, error) {
	base, _, err := c.Connection(ctx)
	if err != nil {
		return nil, err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, base+pathAndQuery, nil)
	if err != nil {
		return nil, err
	}
	c.setHeaders(req, c.Config())
	for k, v := range hdr {
		req.Header[k] = v
	}
	hc := c.http
	if streaming {
		hc = c.stream
	}
	resp, err := hc.Do(req)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode >= 400 {
		resp.Body.Close()
		return nil, &StatusError{resp.StatusCode}
	}
	return resp, nil
}

func (c *Client) fire(ctx context.Context, pathAndQuery string) error {
	ctx, cancel := context.WithTimeout(ctx, 8*time.Second)
	defer cancel()
	resp, err := c.Get(ctx, pathAndQuery, nil, false)
	if err != nil {
		return err
	}
	_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, 64*1024))
	resp.Body.Close()
	return nil
}

// Timeline reports playback state ("playing", "paused", "stopped").
func (c *Client) Timeline(ctx context.Context, ratingKey int, state string, timeMs, durationMs int) error {
	q := url.Values{
		"ratingKey":  {strconv.Itoa(ratingKey)},
		"key":        {"/library/metadata/" + strconv.Itoa(ratingKey)},
		"state":      {state},
		"time":       {strconv.Itoa(timeMs)},
		"duration":   {strconv.Itoa(durationMs)},
		"identifier": {"com.plexapp.plugins.library"},
	}
	return c.fire(ctx, "/:/timeline?"+q.Encode())
}

// Scrobble marks a track played.
func (c *Client) Scrobble(ctx context.Context, ratingKey int) error {
	q := url.Values{"key": {strconv.Itoa(ratingKey)}, "identifier": {"com.plexapp.plugins.library"}}
	return c.fire(ctx, "/:/scrobble?"+q.Encode())
}

// OpusProfileExtra asks the universal transcoder for Opus in Ogg
// (Plexamp-compatible; same as OPUS_PROFILE_EXTRA in plex_api.c).
const OpusProfileExtra = "add-transcode-target(replace=true&type=musicProfile&context=streaming" +
	"&protocol=http&container=ogg&audioCodec=opus)" +
	"+add-limitation(scope=musicCodec&scopeName=Opus&type=upperBound" +
	"&name=audio.channels&value=2&onlyTranscodes=true&replace=true)"

// TranscodePath streams ratingKey as Opus at kbps, starting offset seconds
// in (the transcoder seeks itself; ffmpeg can't seek its output).
func TranscodePath(ratingKey, kbps int, clientID, session string, offset float64) string {
	q := url.Values{
		"directPlay":                  {"0"},
		"musicBitrate":                {strconv.Itoa(kbps)},
		"path":                        {"/library/metadata/" + strconv.Itoa(ratingKey)},
		"session":                     {session},
		"X-Plex-Client-Identifier":    {clientID},
		"X-Plex-Client-Profile-Extra": {OpusProfileExtra},
		"X-Plex-Session-Identifier":   {session},
	}
	if offset > 0.5 {
		q.Set("offset", strconv.FormatFloat(offset, 'f', 1, 64))
	}
	return "/audio/:/transcode/universal/start?" + q.Encode()
}

// ProxyHandler serves /s<path> from the server over plain loopback HTTP so
// ffmpeg needs neither TLS nor the token, and so a changed network is
// handled by re-selecting the connection.
func (c *Client) ProxyHandler() http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		path := r.URL.RequestURI()
		if len(path) < 3 || path[:3] != "/s/" {
			http.NotFound(w, r)
			return
		}
		hdr := http.Header{}
		if rg := r.Header.Get("Range"); rg != "" {
			hdr.Set("Range", rg)
		}
		resp, err := c.Get(r.Context(), path[2:], hdr, true)
		if err != nil {
			if r.Context().Err() == nil {
				log.Printf("proxy %s: %v", trimQuery(path[2:]), err)
			}
			code := http.StatusBadGateway
			var se *StatusError
			if errors.As(err, &se) {
				code = se.Code
			}
			http.Error(w, err.Error(), code)
			return
		}
		defer resp.Body.Close()
		for _, k := range []string{"Content-Type", "Content-Length", "Content-Range", "Accept-Ranges"} {
			if v := resp.Header.Get(k); v != "" {
				w.Header().Set(k, v)
			}
		}
		w.WriteHeader(resp.StatusCode)
		_, _ = io.Copy(w, resp.Body)
	})
}

// trimQuery drops the query string (it can hold the session id) for logs.
func trimQuery(p string) string {
	for i := 0; i < len(p); i++ {
		if p[i] == '?' {
			return p[:i]
		}
	}
	return p
}
