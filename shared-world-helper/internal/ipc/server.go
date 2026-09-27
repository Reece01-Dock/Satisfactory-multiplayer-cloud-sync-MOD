// Package ipc is the helper's local HTTP API used by the Satisfactory mod.
//
// Protection model: the server binds to 127.0.0.1 only, requires a random
// bearer token that is written to a per-user discovery file, rejects any
// request carrying an Origin header (so a web page in a browser cannot
// drive it) and validates the Host header (DNS rebinding). Processes running
// as the same OS user can read the token; that is accepted, as such a
// process could equally read the save files directly.
package ipc

import (
	"context"
	"crypto/subtle"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"time"

	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/lease"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/model"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/world"
)

// APIVersion is reported by /v1/health so the mod can refuse an
// incompatible helper.
const APIVersion = 1

// Server serves the IPC API.
type Server struct {
	mgr     *world.Manager
	token   string
	version string
	log     *slog.Logger
	port    int
}

func NewServer(mgr *world.Manager, token, version string, log *slog.Logger) *Server {
	return &Server{mgr: mgr, token: token, version: version, log: log}
}

// Listen binds a loopback listener. port 0 picks a free port.
func Listen(port int) (net.Listener, error) {
	return net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", port))
}

// Serve runs until ctx is cancelled.
func (s *Server) Serve(ctx context.Context, ln net.Listener) error {
	s.port = ln.Addr().(*net.TCPAddr).Port
	srv := &http.Server{
		Handler:           s.Handler(),
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       15 * time.Second,
		WriteTimeout:      30 * time.Second,
		IdleTimeout:       60 * time.Second,
	}
	go func() {
		<-ctx.Done()
		sctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = srv.Shutdown(sctx)
	}()
	err := srv.Serve(ln)
	if errors.Is(err, http.ErrServerClosed) {
		return nil
	}
	return err
}

// Handler builds the routed, protected handler.
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /v1/health", s.health)
	mux.HandleFunc("GET /v1/worlds", s.auth(s.listWorlds))
	mux.HandleFunc("POST /v1/worlds", s.auth(s.createWorld))
	mux.HandleFunc("GET /v1/worlds/{id}", s.auth(s.worldStatus))
	mux.HandleFunc("GET /v1/worlds/{id}/history", s.auth(s.history))
	mux.HandleFunc("POST /v1/worlds/{id}/play", s.auth(s.play))
	mux.HandleFunc("GET /v1/worlds/{id}/session", s.auth(s.session))
	mux.HandleFunc("POST /v1/worlds/{id}/session/started", s.auth(s.started))
	mux.HandleFunc("POST /v1/worlds/{id}/session/keepalive", s.auth(s.keepalive))
	mux.HandleFunc("POST /v1/worlds/{id}/session/saved", s.auth(s.saved))
	mux.HandleFunc("POST /v1/worlds/{id}/session/abort", s.auth(s.abort))
	mux.HandleFunc("POST /v1/worlds/{id}/session/ack", s.auth(s.ack))
	mux.HandleFunc("POST /v1/worlds/{id}/session/attach", s.auth(s.attach))
	return s.guard(mux)
}

type statusRecorder struct {
	http.ResponseWriter
	code int
}

func (r *statusRecorder) WriteHeader(c int) { r.code = c; r.ResponseWriter.WriteHeader(c) }

// guard applies transport-level protections and access logging.
func (s *Server) guard(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		host, _, err := net.SplitHostPort(r.RemoteAddr)
		if err != nil || !net.ParseIP(host).IsLoopback() {
			writeErr(w, http.StatusForbidden, "FORBIDDEN", "loopback only")
			return
		}
		if r.Header.Get("Origin") != "" {
			writeErr(w, http.StatusForbidden, "FORBIDDEN", "browser requests are not allowed")
			return
		}
		if h, _, err := net.SplitHostPort(r.Host); err != nil || (h != "127.0.0.1" && h != "localhost") {
			writeErr(w, http.StatusForbidden, "FORBIDDEN", "bad host header")
			return
		}
		r.Body = http.MaxBytesReader(w, r.Body, 64<<10)
		rec := &statusRecorder{ResponseWriter: w, code: 200}
		next.ServeHTTP(rec, r)
		lvl := slog.LevelDebug
		if rec.code >= 400 || r.Method != http.MethodGet {
			lvl = slog.LevelInfo
		}
		s.log.Log(r.Context(), lvl, "ipc_request", "method", r.Method, "path", r.URL.Path, "status", rec.code, "duration_ms", time.Since(start).Milliseconds())
	})
}

func (s *Server) auth(h http.HandlerFunc) http.HandlerFunc {
	want := []byte("Bearer " + s.token)
	return func(w http.ResponseWriter, r *http.Request) {
		got := []byte(r.Header.Get("Authorization"))
		if subtle.ConstantTimeCompare(got, want) != 1 {
			writeErr(w, http.StatusUnauthorized, "UNAUTHORIZED", "missing or invalid helper token")
			return
		}
		h(w, r)
	}
}

type apiError struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

func writeErr(w http.ResponseWriter, code int, c, msg string) {
	writeJSON(w, code, map[string]apiError{"error": {Code: c, Message: msg}})
}

func (s *Server) fail(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, world.ErrUnknownWorld):
		writeErr(w, http.StatusNotFound, "UNKNOWN_WORLD", err.Error())
	case errors.Is(err, world.ErrBadState):
		writeErr(w, http.StatusConflict, "BAD_STATE", err.Error())
	case errors.Is(err, lease.ErrNoWorld):
		writeErr(w, http.StatusNotFound, "WORLD_NOT_FOUND", err.Error())
	default:
		writeErr(w, http.StatusBadRequest, "REQUEST_FAILED", err.Error())
	}
}

func decode(r *http.Request, v any) error {
	dec := json.NewDecoder(r.Body)
	dec.DisallowUnknownFields()
	if err := dec.Decode(v); err != nil {
		return fmt.Errorf("invalid request body: %w", err)
	}
	if dec.More() {
		return errors.New("invalid request body: trailing data")
	}
	return nil
}

func worldID(r *http.Request) (string, error) {
	id := r.PathValue("id")
	return id, model.ValidateWorldID(id)
}

// --- handlers ---

func (s *Server) health(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "apiVersion": APIVersion, "version": s.version})
}

func (s *Server) listWorlds(w http.ResponseWriter, r *http.Request) {
	out := []world.WorldStatus{}
	for _, wc := range s.mgr.Worlds() {
		st, err := s.mgr.Status(r.Context(), wc.ID)
		if err != nil {
			s.fail(w, err)
			return
		}
		out = append(out, st)
	}
	writeJSON(w, http.StatusOK, map[string]any{"worlds": out})
}

func (s *Server) worldStatus(w http.ResponseWriter, r *http.Request) {
	id, err := worldID(r)
	if err != nil {
		s.fail(w, err)
		return
	}
	st, err := s.mgr.Status(r.Context(), id)
	if err != nil {
		s.fail(w, err)
		return
	}
	writeJSON(w, http.StatusOK, st)
}

func (s *Server) history(w http.ResponseWriter, r *http.Request) {
	id, err := worldID(r)
	if err != nil {
		s.fail(w, err)
		return
	}
	h, err := s.mgr.History(r.Context(), id)
	if err != nil {
		s.fail(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"revisions": h})
}

func (s *Server) createWorld(w http.ResponseWriter, r *http.Request) {
	var req world.CreateRequest
	if err := decode(r, &req); err != nil {
		s.fail(w, err)
		return
	}
	rev, err := s.mgr.CreateWorld(r.Context(), req)
	if err != nil {
		s.fail(w, err)
		return
	}
	writeJSON(w, http.StatusCreated, map[string]any{"revision": rev})
}

// sessionOp runs a session operation and returns the resulting view.
func (s *Server) sessionOp(w http.ResponseWriter, r *http.Request, body any, op func(id string) (world.SessionView, error)) {
	id, err := worldID(r)
	if err != nil {
		s.fail(w, err)
		return
	}
	if body != nil {
		if err := decode(r, body); err != nil {
			s.fail(w, err)
			return
		}
	} else if _, err := io.Copy(io.Discard, r.Body); err != nil {
		s.fail(w, err)
		return
	}
	v, err := op(id)
	if err != nil {
		if errors.Is(err, world.ErrBadState) && v.WorldID != "" {
			writeJSON(w, http.StatusConflict, map[string]any{"error": apiError{Code: "BAD_STATE", Message: err.Error()}, "session": v})
			return
		}
		s.fail(w, err)
		return
	}
	writeJSON(w, http.StatusOK, v)
}

func (s *Server) play(w http.ResponseWriter, r *http.Request) {
	var req world.PlayRequest
	s.sessionOp(w, r, &req, func(id string) (world.SessionView, error) { return s.mgr.Play(id, req) })
}

func (s *Server) session(w http.ResponseWriter, r *http.Request) {
	s.sessionOp(w, r, nil, s.mgr.Session)
}

func (s *Server) started(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Join *model.JoinInfo `json:"join"`
	}
	s.sessionOp(w, r, &req, func(id string) (world.SessionView, error) {
		return s.mgr.SessionStarted(r.Context(), id, req.Join)
	})
}

func (s *Server) keepalive(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Players []model.Player `json:"players"`
	}
	s.sessionOp(w, r, &req, func(id string) (world.SessionView, error) { return s.mgr.Keepalive(id, req.Players) })
}

func (s *Server) saved(w http.ResponseWriter, r *http.Request) {
	var req world.SavedRequest
	s.sessionOp(w, r, &req, func(id string) (world.SessionView, error) { return s.mgr.Saved(id, req) })
}

func (s *Server) abort(w http.ResponseWriter, r *http.Request) {
	s.sessionOp(w, r, nil, s.mgr.Abort)
}

func (s *Server) ack(w http.ResponseWriter, r *http.Request) {
	s.sessionOp(w, r, nil, s.mgr.Ack)
}

func (s *Server) attach(w http.ResponseWriter, r *http.Request) {
	var req world.PlayRequest
	s.sessionOp(w, r, &req, func(id string) (world.SessionView, error) { return s.mgr.Attach(r.Context(), id, req) })
}
