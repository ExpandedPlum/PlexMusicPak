package plex

import (
	"io"
	"log"
	"net/http"
	"strings"
)

// Hop-by-hop headers belong to one connection and are not forwarded.
var hopHeaders = []string{
	"Connection", "Keep-Alive", "Proxy-Authenticate", "Proxy-Authorization",
	"Proxy-Connection", "Te", "Trailer", "Transfer-Encoding", "Upgrade",
}

func copyHeaders(dst, src http.Header) {
	for k, v := range src {
		dst[k] = append([]string(nil), v...)
	}
	for _, k := range hopHeaders {
		dst.Del(k)
	}
}

// ForwardProxy makes the UI's HTTP requests for it: browsing, sign-in, cover
// art and downloads. The UI writes plain HTTP to a local socket with the full
// URL in the request line (like a forward proxy), and plexd makes the real
// request with Go's HTTP and TLS stack: certificates are verified, TLS 1.3
// works, plex.direct names fall back to their encoded IP, and redirects are
// followed. The UI's own headers (token, client id) pass through.
//
// If the upstream body fails partway, the connection is cut instead of
// ending the response normally, so the UI sees the body as incomplete (a
// short Content-Length body, or a chunked one without its final chunk) and
// doesn't keep a truncated download.
func (c *Client) ForwardProxy() http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		u := r.URL
		if !u.IsAbs() || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" {
			http.Error(w, "plexd: absolute http(s) URL required", http.StatusBadRequest)
			return
		}
		var body io.Reader
		if r.ContentLength != 0 {
			body = r.Body
		}
		out, err := http.NewRequestWithContext(r.Context(), r.Method, u.String(), body)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		copyHeaders(out.Header, r.Header)
		out.ContentLength = r.ContentLength

		resp, err := c.forward.Do(out)
		if err != nil {
			if r.Context().Err() == nil {
				log.Printf("net: %s %s://%s%s: %v", r.Method, u.Scheme, u.Host, u.Path, err)
			}
			msg := err.Error()
			if strings.Contains(msg, "certificate") {
				msg = "plexd: certificate check failed: " + msg
			}
			http.Error(w, msg, http.StatusBadGateway)
			return
		}
		defer resp.Body.Close()
		copyHeaders(w.Header(), resp.Header)
		w.WriteHeader(resp.StatusCode)
		if _, err := io.Copy(w, resp.Body); err != nil {
			if r.Context().Err() == nil {
				log.Printf("net: %s %s://%s%s: body: %v", r.Method, u.Scheme, u.Host, u.Path, err)
			}
			panic(http.ErrAbortHandler) // cut the connection: the body is incomplete
		}
	})
}
