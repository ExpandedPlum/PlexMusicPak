package service

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"strconv"
	"time"
)

// ServeControl exposes a small JSON API on a unix socket for the UI and the
// `plexd ctl` command. Every endpoint is a POST or GET returning JSON;
// failures come back as {"error": "..."} with a 4xx/5xx status.
func (s *Service) ServeControl(socketPath string) error {
	_ = os.Remove(socketPath)
	ln, err := net.Listen("unix", socketPath)
	if err != nil {
		return err
	}
	_ = os.Chmod(socketPath, 0o666)
	srv := &http.Server{Handler: s.routes(), ReadHeaderTimeout: 5 * time.Second}
	go func() {
		<-s.Done()
		ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
		defer cancel()
		_ = srv.Shutdown(ctx)
	}()
	err = srv.Serve(ln)
	if errors.Is(err, http.ErrServerClosed) {
		return nil
	}
	return err
}

type req struct {
	Tracks     []Track          `json:"tracks"`
	Start      int              `json:"start"`
	Shuffle    bool             `json:"shuffle"`
	PositionMs *int             `json:"positionMs"`
	Seconds    float64          `json:"seconds"`
	Volume     *float64         `json:"volume"`
	Fg         bool             `json:"foreground"`
	Settings   *json.RawMessage `json:"settings"`
}

func (s *Service) routes() http.Handler {
	mux := http.NewServeMux()
	h := func(path string, fn func(r req) (any, error)) {
		mux.HandleFunc(path, func(w http.ResponseWriter, hr *http.Request) {
			var r req
			if hr.Method == http.MethodPost {
				body, _ := io.ReadAll(io.LimitReader(hr.Body, 4<<20))
				if len(body) > 0 {
					if err := json.Unmarshal(body, &r); err != nil {
						writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad request: " + err.Error()})
						return
					}
				}
			}
			out, err := fn(r)
			if err != nil {
				log.Printf("%s: %v", path, err)
				writeJSON(w, http.StatusInternalServerError, map[string]string{"error": userError(err)})
				return
			}
			if out == nil {
				out = map[string]bool{"ok": true}
			}
			writeJSON(w, http.StatusOK, out)
		})
	}

	h("/status", func(r req) (any, error) { return s.Status(), nil })
	h("/ui", func(r req) (any, error) { s.UIHeartbeat(r.Fg); return s.Status(), nil })
	h("/queue", func(r req) (any, error) {
		if err := s.SetQueue(r.Tracks, r.Start, r.Shuffle); err != nil {
			return nil, err
		}
		return s.Status(), nil
	})
	h("/play", func(r req) (any, error) { return nil, s.Resume() })
	h("/pause", func(r req) (any, error) { s.Pause(); return nil, nil })
	h("/toggle", func(r req) (any, error) { return nil, s.Toggle() })
	h("/next", func(r req) (any, error) { return nil, s.Next() })
	h("/prev", func(r req) (any, error) { return nil, s.Prev() })
	h("/seek", func(r req) (any, error) {
		if r.PositionMs == nil {
			return nil, errors.New("positionMs required")
		}
		return nil, s.Seek(*r.PositionMs)
	})
	h("/skip", func(r req) (any, error) { return nil, s.Skip(r.Seconds) })
	h("/shuffle", func(r req) (any, error) { s.ToggleShuffle(); return s.Status(), nil })
	h("/repeat", func(r req) (any, error) { s.CycleRepeat(); return s.Status(), nil })
	h("/stop", func(r req) (any, error) { s.Stop(); return nil, nil })
	h("/volume", func(r req) (any, error) {
		if r.Volume == nil {
			return nil, errors.New("volume required")
		}
		s.SetVolume(*r.Volume)
		return nil, nil
	})
	h("/presleep", func(r req) (any, error) { s.PreSleep(); return nil, nil })
	h("/transition", func(r req) (any, error) {
		d := 3 * time.Second
		if r.Seconds > 0 && r.Seconds <= 30 {
			d = time.Duration(r.Seconds * float64(time.Second))
		}
		s.TransitionPause(d)
		return nil, nil
	})
	h("/settings", func(r req) (any, error) {
		if r.Settings != nil {
			set := s.Settings()
			if err := json.Unmarshal(*r.Settings, &set); err != nil {
				return nil, err
			}
			if err := s.SaveSettings(set); err != nil {
				return nil, err
			}
		}
		return s.Settings(), nil
	})
	h("/quit", func(r req) (any, error) {
		go func() {
			time.Sleep(100 * time.Millisecond)
			s.Shutdown()
			s.Quit()
		}()
		return nil, nil
	})
	return mux
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	b, err := json.Marshal(v)
	if err != nil {
		code = http.StatusInternalServerError
		b = []byte(`{"error":"encoding failed"}`)
	}
	w.Header().Set("Content-Length", strconv.Itoa(len(b)))
	w.WriteHeader(code)
	_, _ = w.Write(b)
}
