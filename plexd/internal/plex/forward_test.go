package plex

import (
	"bufio"
	"crypto/x509"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

// rawRequest sends an absolute-form request the way the UI's C client does
// and returns the status code, body and headers.
func rawRequest(t *testing.T, sock, method, url, extra, body string) (int, string, http.Header) {
	t.Helper()
	conn, err := net.Dial("unix", sock)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	host := strings.SplitN(strings.SplitN(url, "://", 2)[1], "/", 2)[0]
	req := method + " " + url + " HTTP/1.1\r\nHost: " + host + "\r\nX-Plex-Token: tok\r\n" + extra + "Connection: close\r\n"
	if body != "" {
		req += "Content-Length: " + strconv.Itoa(len(body)) + "\r\n"
	}
	req += "\r\n" + body
	if _, err := io.WriteString(conn, req); err != nil {
		t.Fatal(err)
	}
	resp, err := http.ReadResponse(bufio.NewReader(conn), nil)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	return resp.StatusCode, string(b), resp.Header
}

func serveForward(t *testing.T, c *Client) string {
	sock := filepath.Join(t.TempDir(), "net.sock")
	ln, err := net.Listen("unix", sock)
	if err != nil {
		t.Fatal(err)
	}
	srv := &http.Server{Handler: c.ForwardProxy()}
	go srv.Serve(ln)
	t.Cleanup(func() { srv.Close() })
	return sock
}

func TestForwardProxy(t *testing.T) {
	upstream := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/redirect":
			http.Redirect(w, r, "/elsewhere", http.StatusFound)
		case "/pins":
			b, _ := io.ReadAll(r.Body)
			w.Header().Set("Content-Type", "application/json")
			_, _ = io.WriteString(w, r.Method+" "+string(b)+" "+r.Header.Get("X-Plex-Token")+" "+r.URL.RawQuery)
		default:
			http.NotFound(w, r)
		}
	})
	plain := httptest.NewServer(upstream)
	defer plain.Close()
	secure := httptest.NewTLSServer(upstream)
	defer secure.Close()

	c := New(filepath.Join(t.TempDir(), "config.json"), "test")
	sock := serveForward(t, c)

	code, body, _ := rawRequest(t, sock, "GET", plain.URL+"/pins?x=1", "", "")
	if code != 200 || body != "GET  tok x=1" {
		t.Fatalf("plain GET: %d %q", code, body)
	}
	if code, _, _ := rawRequest(t, sock, "GET", plain.URL+"/redirect", "", ""); code != http.StatusNotFound {
		t.Fatalf("redirects should be followed (to a 404 here): %d", code)
	}
	if code, _, _ := rawRequest(t, sock, "GET", plain.URL+"/missing", "", ""); code != 404 {
		t.Fatalf("status should pass through: %d", code)
	}

	// The test server's certificate isn't trusted: refused.
	if code, body, _ := rawRequest(t, sock, "GET", secure.URL+"/pins", "", ""); code != http.StatusBadGateway ||
		!strings.Contains(body, "certificate") {
		t.Fatalf("untrusted certificate should be refused: %d %q", code, body)
	}
	// Trusted once its CA is added.
	pool := x509.NewCertPool()
	pool.AddCert(secure.Certificate())
	c.tr.TLSClientConfig = secure.Client().Transport.(*http.Transport).TLSClientConfig.Clone()
	c.tr.TLSClientConfig.RootCAs = pool
	if code, body, _ := rawRequest(t, sock, "GET", secure.URL+"/pins", "", ""); code != 200 || !strings.HasPrefix(body, "GET") {
		t.Fatalf("trusted HTTPS: %d %q", code, body)
	}

	if code, _, _ := rawRequest(t, sock, "GET", "ftp://example.com/x", "", ""); code != http.StatusBadRequest {
		t.Fatalf("non-http scheme: %d", code)
	}
}

func TestForwardProxyPostBody(t *testing.T) {
	got := make(chan string, 1)
	plain := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		b, _ := io.ReadAll(r.Body)
		got <- r.Method + " " + string(b) + " " + r.Header.Get("Content-Type")
	}))
	defer plain.Close()
	sock := serveForward(t, New(filepath.Join(t.TempDir(), "c.json"), "test"))
	if code, _, _ := rawRequest(t, sock, "POST", plain.URL+"/api/v2/pins",
		"Content-Type: application/x-www-form-urlencoded\r\n", "strong=false"); code != 200 {
		t.Fatalf("POST: %d", code)
	}
	if g := <-got; g != "POST strong=false application/x-www-form-urlencoded" {
		t.Fatalf("POST forwarded as %q", g)
	}
}

func TestForwardProxyCutsTruncatedBody(t *testing.T) {
	// Upstream promises 1000 bytes and sends 10.
	upstream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Length", "1000")
		_, _ = io.WriteString(w, "0123456789")
		w.(http.Flusher).Flush()
		if hj, ok := w.(http.Hijacker); ok {
			conn, _, _ := hj.Hijack()
			conn.Close()
		}
	}))
	defer upstream.Close()
	sock := serveForward(t, New(filepath.Join(t.TempDir(), "c.json"), "test"))
	conn, err := net.Dial("unix", sock)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	_, _ = io.WriteString(conn, "GET "+upstream.URL+"/f HTTP/1.1\r\nHost: plexd\r\nConnection: close\r\n\r\n")
	// Either way is an incomplete response: cut before the headers went out,
	// or a body shorter than its Content-Length.
	resp, err := http.ReadResponse(bufio.NewReader(conn), nil)
	if err != nil {
		return
	}
	b, err := io.ReadAll(resp.Body)
	if err == nil {
		t.Fatalf("a truncated upstream body must not look complete (got %d bytes, no error)", len(b))
	}
}
