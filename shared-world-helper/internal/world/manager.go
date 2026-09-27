// Package world orchestrates everything behind "Play Shared World": it
// decides HOST or JOIN, drives the lease through the hosting lifecycle, and
// runs downloads and uploads. The mod only renders SessionView and reports
// game events (session started, save written, keepalive).
package world

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"sync"
	"time"

	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/clock"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/lease"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/model"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/savefile"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/syncer"
)

// WorldConfig is one configured shared world.
type WorldConfig struct {
	ID   string `json:"id"`
	Name string `json:"name"`
}

// Config tunes the manager.
type Config struct {
	Worlds []WorldConfig
	// HeartbeatInterval between lease renewals; must be well below the TTL.
	HeartbeatInterval time.Duration
	// GameLivenessTimeout: stop renewing if the mod has not sent a
	// keepalive for this long (game hung). A dead game process is detected
	// immediately via its PID.
	GameLivenessTimeout time.Duration
	// JoinWaitTimeout bounds waiting for a starting host to publish join info.
	JoinWaitTimeout  time.Duration
	JoinPollInterval time.Duration
	StatusCacheTTL   time.Duration
}

func DefaultConfig() Config {
	return Config{
		HeartbeatInterval:   20 * time.Second,
		GameLivenessTimeout: 5 * time.Minute,
		JoinWaitTimeout:     3 * time.Minute,
		JoinPollInterval:    3 * time.Second,
		StatusCacheTTL:      5 * time.Second,
	}
}

// SaveNameFor is the local save file stem used for a world.
func SaveNameFor(worldID string) string { return "SharedWorld_" + worldID }

// ErrUnknownWorld is returned for worlds not in the configuration.
var ErrUnknownWorld = errors.New("unknown world")

// ErrBadState is returned when an event arrives in a state that cannot accept it.
var ErrBadState = errors.New("operation not valid in the current session state")

type session struct {
	mu            sync.Mutex
	view          SessionView
	token         *lease.Token
	player        model.Identity
	saveDir       string
	gamePID       int
	lastKeepalive time.Time
	players       []model.Player
	join          *model.JoinInfo
	stopping      bool // current upload is the final one
	hbCancel      context.CancelFunc
}

type cacheEntry struct {
	status WorldStatus
	at     time.Time
}

// Manager coordinates all worlds on this machine.
type Manager struct {
	cfg       Config
	leases    *lease.Manager
	sync      *syncer.Syncer
	clock     clock.Clock
	log       *slog.Logger
	installID string

	// ProcessAlive reports whether a game process is still running.
	ProcessAlive func(pid int) bool

	mu       sync.Mutex
	worlds   map[string]WorldConfig
	sessions map[string]*session
	cache    map[string]cacheEntry

	ctx    context.Context
	cancel context.CancelFunc
	wg     sync.WaitGroup
}

func NewManager(cfg Config, leases *lease.Manager, s *syncer.Syncer, c clock.Clock, installID string, log *slog.Logger) (*Manager, error) {
	m := &Manager{
		cfg: cfg, leases: leases, sync: s, clock: c, log: log, installID: installID,
		ProcessAlive: ProcessAlive,
		worlds:       map[string]WorldConfig{},
		sessions:     map[string]*session{},
		cache:        map[string]cacheEntry{},
	}
	for _, w := range cfg.Worlds {
		if err := model.ValidateWorldID(w.ID); err != nil {
			return nil, err
		}
		m.worlds[w.ID] = w
		m.sessions[w.ID] = &session{view: SessionView{WorldID: w.ID, State: StateIdle, Steps: []Step{}}}
	}
	m.ctx, m.cancel = context.WithCancel(context.Background())
	return m, nil
}

// Shutdown stops background work. Leases are NOT released: a running game
// may still be hosting, and a restarted helper resumes via Attach. If
// nothing resumes them they expire on their own.
func (m *Manager) Shutdown() {
	m.cancel()
	m.wg.Wait()
}

func (m *Manager) session(worldID string) (*session, WorldConfig, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	s, ok := m.sessions[worldID]
	if !ok {
		return nil, WorldConfig{}, fmt.Errorf("%w: %q", ErrUnknownWorld, worldID)
	}
	return s, m.worlds[worldID], nil
}

// Worlds lists configured worlds.
func (m *Manager) Worlds() []WorldConfig {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]WorldConfig, 0, len(m.worlds))
	for _, w := range m.cfg.Worlds {
		out = append(out, m.worlds[w.ID])
	}
	return out
}

// --- session view helpers (callers hold s.mu) ---

func (m *Manager) setState(s *session, to State, msg string) error {
	from := s.view.State
	if from != to && !canTransition(from, to) {
		m.log.Error("illegal_state_transition", "world", s.view.WorldID, "from", from, "to", to)
		return fmt.Errorf("%w: %s -> %s", ErrBadState, from, to)
	}
	s.view.State = to
	if to == StateIdle {
		s.view.Decision, s.view.Error, s.view.Join, s.view.HostName = DecisionNone, nil, nil, ""
		s.view.SaveName, s.view.SavePath, s.view.Generation = "", "", 0
	}
	if msg != "" {
		s.view.Message = msg
		s.view.Steps = append(s.view.Steps, Step{At: m.clock.Now(), Message: msg})
		if len(s.view.Steps) > 30 {
			s.view.Steps = s.view.Steps[len(s.view.Steps)-30:]
		}
	}
	s.view.UpdatedAt = m.clock.Now()
	m.log.Info("session_state", "world", s.view.WorldID, "from", from, "to", to, "message", msg)
	return nil
}

func (m *Manager) note(s *session, msg string) {
	s.view.Message = msg
	s.view.Steps = append(s.view.Steps, Step{At: m.clock.Now(), Message: msg})
	s.view.UpdatedAt = m.clock.Now()
}

func (m *Manager) fail(s *session, e *ErrorInfo) {
	s.view.Error = e
	_ = m.setState(s, StateError, e.Message)
	m.log.Warn("session_error", "world", s.view.WorldID, "code", e.Code, "detail", e.Detail)
}

// Session returns the current local session view.
func (m *Manager) Session(worldID string) (SessionView, error) {
	s, _, err := m.session(worldID)
	if err != nil {
		return SessionView{}, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	return cloneView(s.view), nil
}

func cloneView(v SessionView) SessionView {
	v.Steps = append([]Step(nil), v.Steps...)
	if v.Error != nil {
		e := *v.Error
		v.Error = &e
	}
	if v.Join != nil {
		j := *v.Join
		v.Join = &j
	}
	return v
}

// --- status ---

// Status returns the shared-world status for the main menu, cached briefly
// so a menu polling every second does not hammer the cloud provider.
func (m *Manager) Status(ctx context.Context, worldID string) (WorldStatus, error) {
	s, wc, err := m.session(worldID)
	if err != nil {
		return WorldStatus{}, err
	}
	m.mu.Lock()
	ce, ok := m.cache[worldID]
	m.mu.Unlock()
	var st WorldStatus
	if ok && m.clock.Now().Sub(ce.at) < m.cfg.StatusCacheTTL {
		st = ce.status
	} else {
		st = m.fetchStatus(ctx, wc)
		m.mu.Lock()
		m.cache[worldID] = cacheEntry{status: st, at: m.clock.Now()}
		m.mu.Unlock()
	}
	s.mu.Lock()
	st.Local = cloneView(s.view)
	s.mu.Unlock()
	return st, nil
}

func (m *Manager) invalidate(worldID string) {
	m.mu.Lock()
	delete(m.cache, worldID)
	m.mu.Unlock()
}

func (m *Manager) fetchStatus(ctx context.Context, wc WorldConfig) WorldStatus {
	st := WorldStatus{WorldID: wc.ID, WorldName: wc.Name}
	rec, _, err := m.leases.Load(ctx, wc.ID)
	if errors.Is(err, lease.ErrNoWorld) {
		st.Status, st.StatusText = RemoteNoSave, "Not created yet"
		return st
	}
	if err != nil {
		st.Status, st.StatusText, st.Error = RemoteUnreachable, "Cloud storage unreachable", err.Error()
		return st
	}
	now := m.clock.Now()
	st.WorldName = rec.WorldName
	st.Revision, st.Generation = rec.HeadNumber(), rec.Generation
	st.Status, st.StatusText = describeRemote(rec, m.leases.LiveForObserver(rec.Lease, now), now)
	if l := rec.Lease; l != nil {
		st.HostName = l.Holder.DisplayName
		st.Players = append([]model.Player(nil), l.Players...)
		st.PlayerCount = len(l.Players)
		exp := l.ExpiresAt
		st.LeaseExpires = &exp
	}
	if ls := rec.LastSession; ls != nil {
		t := ls.EndedAt
		st.LastPlayedAt, st.LastHostName = &t, ls.Host.DisplayName
	} else if rec.Head != nil {
		t := rec.Head.CreatedAt
		st.LastPlayedAt, st.LastHostName = &t, rec.Head.Uploader.DisplayName
	}
	return st
}

// --- Play ---

// PlayRequest comes from the mod when the player presses Play Shared World.
type PlayRequest struct {
	PlayerID    string `json:"playerId"`
	DisplayName string `json:"displayName"`
	Platform    string `json:"platform"`
	// SaveDirectory is UFGSaveSystem::GetSaveDirectoryPath() as reported by
	// the game, so the helper never guesses save locations.
	SaveDirectory string `json:"saveDirectory"`
	GamePID       int    `json:"gamePid"`
}

func (m *Manager) identity(r PlayRequest) model.Identity {
	return model.Identity{PlayerID: r.PlayerID, DisplayName: r.DisplayName, Platform: r.Platform, InstallID: m.installID}
}

func validateSaveDir(dir string) error {
	if dir == "" || !filepath.IsAbs(dir) {
		return errors.New("save directory must be an absolute path")
	}
	st, err := os.Stat(dir)
	if err != nil {
		return fmt.Errorf("save directory: %w", err)
	}
	if !st.IsDir() {
		return errors.New("save directory is not a directory")
	}
	return nil
}

func newNonce() string {
	b := make([]byte, 16)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	return hex.EncodeToString(b)
}

// Play starts the Play Shared World flow and returns immediately; progress
// is observed via Session.
func (m *Manager) Play(worldID string, req PlayRequest) (SessionView, error) {
	s, wc, err := m.session(worldID)
	if err != nil {
		return SessionView{}, err
	}
	id := m.identity(req)
	if err := id.Validate(); err != nil {
		return SessionView{}, err
	}
	if err := validateSaveDir(req.SaveDirectory); err != nil {
		return SessionView{}, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	switch s.view.State {
	case StateIdle, StateJoinReady, StateError:
	default:
		// Already in progress (double click) or LEASE_LOST awaiting
		// acknowledgement: report the current state.
		return cloneView(s.view), nil
	}
	s.view.Steps = nil
	s.view.Error = nil
	if err := m.setState(s, StateChecking, "Checking shared world..."); err != nil {
		return SessionView{}, err
	}
	s.player, s.saveDir, s.gamePID = id, req.SaveDirectory, req.GamePID
	s.lastKeepalive = m.clock.Now()
	s.view.SaveName = SaveNameFor(worldID)
	s.view.SavePath = filepath.Join(req.SaveDirectory, SaveNameFor(worldID)+savefile.Extension)
	m.wg.Add(1)
	go func() {
		defer m.wg.Done()
		m.runPlay(s, wc)
	}()
	return cloneView(s.view), nil
}

func (m *Manager) runPlay(s *session, wc WorldConfig) {
	ctx := m.ctx
	defer m.invalidate(wc.ID)
	deadline := m.clock.Now().Add(m.cfg.JoinWaitTimeout)
	for attempt := 0; ; attempt++ {
		if attempt > 1000 {
			s.mu.Lock()
			m.fail(s, errorf("CONTENTION", "The shared world kept changing hands. Please try again.", nil))
			s.mu.Unlock()
			return
		}
		rec, _, err := m.leases.Load(ctx, wc.ID)
		if err != nil {
			s.mu.Lock()
			if errors.Is(err, lease.ErrNoWorld) {
				m.fail(s, errorf("WORLD_NOT_FOUND", "This shared world has not been created in the cloud yet.", err))
			} else {
				e := errorf("CLOUD_UNREACHABLE", "Could not reach the shared world's cloud storage.\n\nYour local save was NOT modified.", err)
				e.Retryable = true
				m.fail(s, e)
			}
			s.mu.Unlock()
			return
		}
		now := m.clock.Now()
		if l := rec.Lease; l != nil && m.leases.LiveForObserver(l, now) && l.SessionNonce != "" {
			if done := m.joinPath(s, rec, deadline); done {
				return
			}
			continue // host went away while we waited; re-evaluate
		}
		if done := m.hostPath(ctx, s, wc); done {
			return
		}
		// hostPath lost the acquisition race: loop to the join path.
	}
}

// joinPath handles a live lease held by someone else. Returns false if the
// lease disappeared and the caller should re-evaluate.
func (m *Manager) joinPath(s *session, rec *model.WorldRecord, deadline time.Time) bool {
	l := rec.Lease
	s.mu.Lock()
	defer s.mu.Unlock()
	if l.Holder.InstallID == s.player.InstallID || l.Holder.PlayerID == s.player.PlayerID {
		m.fail(s, &ErrorInfo{
			Code:               "ALREADY_HOSTING_ELSEWHERE",
			Message:            fmt.Sprintf("You are already hosting this world from another game instance or PC.\n\nIf the game crashed, the world becomes available again at %s.", l.ExpiresAt.Local().Format("15:04:05")),
			LocalSaveUnchanged: true,
			Retryable:          true,
		})
		return true
	}
	s.view.HostName = l.Holder.DisplayName
	s.view.Decision = DecisionJoin
	s.view.Revision = rec.HeadNumber()
	if l.Join != nil {
		j := *l.Join
		s.view.Join = &j
		_ = m.setState(s, StateJoinReady, fmt.Sprintf("%s is already hosting. Joining session...", l.Holder.DisplayName))
		return true
	}
	if l.Phase == model.PhaseHosting {
		// Hosting, but the host's game could not publish join data.
		_ = m.setState(s, StateJoinReady, fmt.Sprintf("%s is already hosting. Join them from the Satisfactory friends list.", l.Holder.DisplayName))
		return true
	}
	if s.view.State != StateWaitingForHost {
		_ = m.setState(s, StateWaitingForHost, fmt.Sprintf("%s is starting the world. Waiting for their session...", l.Holder.DisplayName))
	}
	if m.clock.Now().After(deadline) {
		e := errorf("HOST_NOT_READY", fmt.Sprintf("%s is still starting the world. Try again in a moment.", l.Holder.DisplayName), nil)
		e.Retryable = true
		m.fail(s, e)
		return true
	}
	s.mu.Unlock()
	select {
	case <-m.ctx.Done():
		s.mu.Lock()
		return true
	case <-time.After(m.cfg.JoinPollInterval):
	}
	s.mu.Lock()
	if s.view.State != StateWaitingForHost {
		return true // cancelled
	}
	// Re-evaluate from scratch; if the lease vanished we may become host.
	_ = m.setState(s, StateChecking, "")
	return false
}

// hostPath acquires the lease and prepares the save. Returns false if the
// acquisition race was lost (caller re-evaluates and joins).
func (m *Manager) hostPath(ctx context.Context, s *session, wc WorldConfig) bool {
	s.mu.Lock()
	_ = m.setState(s, StateAcquiring, "No active host found. Acquiring host lease...")
	player, savePath := s.player, s.view.SavePath
	s.mu.Unlock()

	res, err := m.leases.Acquire(ctx, wc.ID, player, newNonce())
	if err != nil {
		s.mu.Lock()
		e := errorf("LEASE_FAILED", "Could not claim the shared world.\n\nYour local save was NOT modified.", err)
		e.Retryable = true
		m.fail(s, e)
		s.mu.Unlock()
		return true
	}
	switch res.Outcome {
	case lease.HeldByOther, lease.HeldBySelfElsewhere:
		s.mu.Lock()
		_ = m.setState(s, StateChecking, "Another player started hosting first.")
		s.mu.Unlock()
		return false
	}
	tok := res.Token
	if err := m.sync.LocalState().Update(wc.ID, func(ls *syncer.LocalState) { ls.ActiveLease = tok }); err != nil {
		m.abortHosting(s, tok, errorf("LOCAL_STATE", "Could not write local state.\n\nYour local save was NOT modified.", err))
		return true
	}
	s.mu.Lock()
	s.token = tok
	s.view.Decision = DecisionHost
	s.view.Generation = tok.Generation
	s.view.Revision = tok.BaseRevision
	if res.TookOverExpired != nil {
		m.note(s, fmt.Sprintf("Previous host %s stopped responding; recovering the world.", res.TookOverExpired.Holder.DisplayName))
	}
	s.mu.Unlock()
	m.startHeartbeat(s)

	// Unsynced local progress from an earlier crashed session?
	status, _, ls, err := m.sync.InspectLocal(wc.ID, savePath)
	if err != nil {
		m.abortHosting(s, tok, errorf("LOCAL_SAVE_UNREADABLE", "Could not read your local copy of the shared save.\n\nIt was NOT modified.", err))
		return true
	}
	head := res.Record.HeadNumber()
	if status == syncer.LocalModified {
		if ls.SyncedRevision == head && head > 0 {
			s.mu.Lock()
			_ = m.setState(s, StateRecovering, "Found unsynced progress from your last session. Uploading it...")
			s.mu.Unlock()
			up, err := m.sync.Upload(ctx, tok, savePath, "recovered")
			if err == nil {
				s.mu.Lock()
				s.view.Revision = up.Revision.Number
				_ = m.setState(s, StateReadyToHost, fmt.Sprintf("Recovered progress saved as revision %d. Starting shared world...", up.Revision.Number))
				s.mu.Unlock()
				return true
			}
			// Could not recover (not a complete save, network...): keep it
			// safe and fall through to the cloud revision.
			bp, _ := m.sync.Backups().Preserve(wc.ID, savePath, fmt.Sprintf("conflict-unsynced-r%d", ls.SyncedRevision))
			s.mu.Lock()
			m.note(s, "Could not recover the unsynced local save; it was kept as a backup: "+bp)
			s.mu.Unlock()
			m.log.Warn("recovery_upload_failed", "world", wc.ID, "error", err.Error(), "backup", bp)
		} else {
			bp, err := m.sync.Backups().Preserve(wc.ID, savePath, fmt.Sprintf("conflict-local-r%d", ls.SyncedRevision))
			if err != nil {
				m.abortHosting(s, tok, errorf("BACKUP_FAILED", "Your local save has changes that are not in the cloud, and backing it up failed. Nothing was changed.", err))
				return true
			}
			s.mu.Lock()
			m.note(s, fmt.Sprintf("Your local save (based on revision %d) differs from the newer shared revision %d. It was kept as a backup: %s", ls.SyncedRevision, head, bp))
			s.mu.Unlock()
		}
	}

	s.mu.Lock()
	_ = m.setState(s, StateDownloading, fmt.Sprintf("Downloading revision %d...", head))
	s.mu.Unlock()
	dl, err := m.sync.Download(ctx, res.Record, savePath)
	if err != nil {
		var e *ErrorInfo
		switch {
		case errors.Is(err, syncer.ErrNoSave):
			e = errorf("NO_SAVE", "This shared world has no save yet. Import one from Settings.", err)
		case errors.Is(err, syncer.ErrCorruptDownload):
			e = errorf("CORRUPT_DOWNLOAD", "The shared save failed verification after download.\n\nYour local save was NOT modified.", err)
			e.Retryable = true
		default:
			e = errorf("DOWNLOAD_FAILED", "The shared save could not be downloaded.\n\nYour local save was NOT modified.", err)
			e.Retryable = true
		}
		m.abortHosting(s, tok, e)
		return true
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if dl.AlreadyLocal {
		m.note(s, fmt.Sprintf("Local save already matches revision %d.", dl.Revision))
	} else {
		m.note(s, "Save verified.")
	}
	s.view.Revision = dl.Revision
	_ = m.setState(s, StateReadyToHost, "Starting shared world...")
	return true
}

// abortHosting releases the lease after a failure before the game loaded.
func (m *Manager) abortHosting(s *session, tok *lease.Token, e *ErrorInfo) {
	m.stopHeartbeat(s)
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	if err := m.leases.Release(ctx, tok); err != nil && !errors.Is(err, lease.ErrFenced) {
		m.log.Warn("release_after_failure_failed", "world", tok.WorldID, "error", err.Error())
	}
	_ = m.sync.LocalState().Update(tok.WorldID, func(ls *syncer.LocalState) { ls.ActiveLease = nil })
	s.mu.Lock()
	s.token = nil
	m.fail(s, e)
	s.mu.Unlock()
}

// --- game events ---

// SessionStarted is reported by the host's mod once its game session
// exists. join is nil when the mod could not obtain join data; clients are
// then told to join through the game's friends list.
func (m *Manager) SessionStarted(ctx context.Context, worldID string, join *model.JoinInfo) (SessionView, error) {
	if join != nil {
		if err := join.Validate(); err != nil {
			return SessionView{}, err
		}
	}
	s, _, err := m.session(worldID)
	if err != nil {
		return SessionView{}, err
	}
	s.mu.Lock()
	if s.view.State != StateReadyToHost && s.view.State != StateHosting {
		defer s.mu.Unlock()
		return cloneView(s.view), ErrBadState
	}
	tok := s.token
	s.join = join
	s.lastKeepalive = m.clock.Now()
	s.mu.Unlock()

	_, err = m.leases.Renew(ctx, tok, lease.LeaseUpdate{Phase: model.PhaseHosting, Join: join, ClearJoin: join == nil})
	s.mu.Lock()
	defer s.mu.Unlock()
	defer m.invalidate(worldID)
	if errors.Is(err, lease.ErrFenced) {
		m.leaseLost(s)
		return cloneView(s.view), nil
	}
	if err != nil {
		// The heartbeat will publish the join info when the cloud is reachable again.
		m.note(s, "World started, but publishing the session to friends failed; retrying...")
		m.log.Warn("publish_join_failed", "world", worldID, "error", err.Error())
	}
	if s.view.State == StateReadyToHost {
		_ = m.setState(s, StateHosting, "Shared world is online. Friends can now join.")
	}
	return cloneView(s.view), nil
}

// Keepalive is sent periodically by the mod while it has an active session.
func (m *Manager) Keepalive(worldID string, players []model.Player) (SessionView, error) {
	s, _, err := m.session(worldID)
	if err != nil {
		return SessionView{}, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.lastKeepalive = m.clock.Now()
	if players != nil {
		if len(players) > 128 {
			players = players[:128]
		}
		s.players = append([]model.Player(nil), players...)
	}
	return cloneView(s.view), nil
}

// SavedRequest reports that the game finished writing the shared save.
type SavedRequest struct {
	// SaveName must be the world's save name; paths are never accepted.
	SaveName string `json:"saveName"`
	// Final: this is the save made when the host stops; upload then release.
	Final bool `json:"final"`
}

// Saved uploads the save the game just wrote.
func (m *Manager) Saved(worldID string, req SavedRequest) (SessionView, error) {
	s, _, err := m.session(worldID)
	if err != nil {
		return SessionView{}, err
	}
	if req.SaveName != SaveNameFor(worldID) {
		return SessionView{}, fmt.Errorf("unexpected save name %q", req.SaveName)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.view.State != StateHosting {
		return cloneView(s.view), ErrBadState
	}
	s.stopping = req.Final
	msg := "Uploading checkpoint..."
	if req.Final {
		msg = "Saving and uploading the shared world..."
	}
	s.view.Error = nil
	if err := m.setState(s, StateUploading, msg); err != nil {
		return cloneView(s.view), err
	}
	tok, path := s.token, s.view.SavePath
	m.wg.Add(1)
	go func() {
		defer m.wg.Done()
		m.runUpload(s, tok, path, req.Final)
	}()
	return cloneView(s.view), nil
}

func (m *Manager) runUpload(s *session, tok *lease.Token, path string, final bool) {
	ctx := m.ctx
	defer m.invalidate(tok.WorldID)
	reason := "checkpoint"
	if final {
		reason = "final"
	}
	res, err := m.sync.Upload(ctx, tok, path, reason)
	s.mu.Lock()
	defer s.mu.Unlock()
	if err != nil {
		var ce *syncer.ConflictError
		switch {
		case errors.Is(err, lease.ErrFenced):
			m.leaseLost(s)
			if errors.As(err, &ce) {
				s.view.Error.BackupPath = ce.BackupPath
			}
		case errors.As(err, &ce):
			e := &ErrorInfo{
				Code:               "NEWER_SAVE_EXISTS",
				Message:            fmt.Sprintf("A newer shared save exists.\n\nYour local save has been preserved as a backup.\n\nCloud revision: %d\nLocal revision: %d", ce.CloudHead, ce.LocalBase),
				Detail:             err.Error(),
				LocalSaveUnchanged: true,
				BackupPath:         ce.BackupPath,
				CloudRevision:      ce.CloudHead,
				LocalRevision:      ce.LocalBase,
			}
			m.stopHeartbeatLocked(s)
			s.token = nil
			m.fail(s, e)
		default:
			// Transient: keep hosting authority; the player can retry.
			s.view.Error = &ErrorInfo{Code: "UPLOAD_FAILED", Message: "Uploading the shared save failed. Your save is safe on this PC; the upload will be retried.", Detail: err.Error(), LocalSaveUnchanged: true, Retryable: true}
			_ = m.setState(s, StateHosting, "Upload failed; will retry.")
		}
		return
	}
	s.view.Revision = res.Revision.Number
	if !final {
		msg := fmt.Sprintf("Checkpoint uploaded as revision %d.", res.Revision.Number)
		if res.Unchanged {
			msg = "No changes since the last upload."
		}
		_ = m.setState(s, StateHosting, msg)
		return
	}
	_ = m.setState(s, StateReleasing, fmt.Sprintf("Uploaded revision %d. Releasing the world...", res.Revision.Number))
	m.releaseLocked(s)
}

// releaseLocked releases the lease and returns to IDLE. Caller holds s.mu.
func (m *Manager) releaseLocked(s *session) {
	tok := s.token
	m.stopHeartbeatLocked(s)
	s.mu.Unlock()
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	err := m.leases.Release(ctx, tok)
	cancel()
	_ = m.sync.LocalState().Update(tok.WorldID, func(ls *syncer.LocalState) { ls.ActiveLease = nil })
	s.mu.Lock()
	s.token, s.join, s.players = nil, nil, nil
	if err != nil && !errors.Is(err, lease.ErrFenced) {
		// Everything is uploaded; the lease simply expires on its own.
		m.log.Warn("release_failed", "world", tok.WorldID, "error", err.Error())
	}
	_ = m.setState(s, StateIdle, "Shared world saved and released.")
}

// Abort gives up hosting before the world was loaded (player cancelled or
// the game failed to load the save), or stops waiting for a host.
func (m *Manager) Abort(worldID string) (SessionView, error) {
	s, _, err := m.session(worldID)
	if err != nil {
		return SessionView{}, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	switch s.view.State {
	case StateReadyToHost:
		_ = m.setState(s, StateReleasing, "Cancelling...")
		m.releaseLocked(s)
	case StateWaitingForHost:
		_ = m.setState(s, StateIdle, "Cancelled.")
	default:
		return cloneView(s.view), ErrBadState
	}
	m.invalidate(worldID)
	return cloneView(s.view), nil
}

// Ack clears a finished JOIN decision or an error.
func (m *Manager) Ack(worldID string) (SessionView, error) {
	s, _, err := m.session(worldID)
	if err != nil {
		return SessionView{}, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	switch s.view.State {
	case StateJoinReady, StateError, StateLeaseLost:
		_ = m.setState(s, StateIdle, "")
		return cloneView(s.view), nil
	}
	return cloneView(s.view), ErrBadState
}

// Attach is called by a mod that is hosting when it reconnects to a
// (possibly restarted) helper. Hosting resumes only if the cloud record
// still carries the persisted generation.
func (m *Manager) Attach(ctx context.Context, worldID string, req PlayRequest) (SessionView, error) {
	s, _, err := m.session(worldID)
	if err != nil {
		return SessionView{}, err
	}
	s.mu.Lock()
	if s.token != nil {
		s.gamePID, s.lastKeepalive = req.GamePID, m.clock.Now()
		defer s.mu.Unlock()
		return cloneView(s.view), nil
	}
	s.mu.Unlock()
	ls, err := m.sync.LocalState().Load(worldID)
	if err != nil || ls.ActiveLease == nil {
		return SessionView{}, fmt.Errorf("%w: no hosting session to resume", ErrBadState)
	}
	if err := validateSaveDir(req.SaveDirectory); err != nil {
		return SessionView{}, err
	}
	tok := ls.ActiveLease
	_, err = m.leases.Renew(ctx, tok, lease.LeaseUpdate{})
	s.mu.Lock()
	defer s.mu.Unlock()
	s.view.Steps = nil
	s.view.Error = nil
	s.saveDir, s.gamePID, s.lastKeepalive = req.SaveDirectory, req.GamePID, m.clock.Now()
	s.view.SaveName = SaveNameFor(worldID)
	s.view.SavePath = filepath.Join(req.SaveDirectory, SaveNameFor(worldID)+savefile.Extension)
	s.view.Decision, s.view.Generation, s.view.Revision = DecisionHost, tok.Generation, tok.BaseRevision
	if errors.Is(err, lease.ErrFenced) {
		s.view.State = StateHosting // so leaseLost's transition is legal
		m.leaseLost(s)
		return cloneView(s.view), nil
	}
	if err != nil {
		return SessionView{}, err
	}
	s.token = tok
	s.view.State = StateHosting
	m.note(s, "Reconnected to the helper; still hosting.")
	m.log.Info("session_resumed", "world", worldID, "generation", tok.Generation)
	m.startHeartbeatLocked(s)
	return cloneView(s.view), nil
}

func (m *Manager) leaseLost(s *session) {
	m.stopHeartbeatLocked(s)
	if s.token != nil {
		_ = m.sync.LocalState().Update(s.token.WorldID, func(ls *syncer.LocalState) { ls.ActiveLease = nil })
	}
	s.token = nil
	s.view.Error = &ErrorInfo{
		Code:               "LEASE_LOST",
		Message:            "Another player has taken over this shared world because your connection timed out.\n\nYour progress since the last upload was kept as a backup on this PC. Please return to the main menu.",
		LocalSaveUnchanged: true,
	}
	_ = m.setState(s, StateLeaseLost, "Hosting authority lost.")
}

// --- heartbeat ---

func (m *Manager) startHeartbeat(s *session) {
	s.mu.Lock()
	defer s.mu.Unlock()
	m.startHeartbeatLocked(s)
}

func (m *Manager) startHeartbeatLocked(s *session) {
	if s.hbCancel != nil {
		return
	}
	ctx, cancel := context.WithCancel(m.ctx)
	s.hbCancel = cancel
	m.wg.Add(1)
	go func() {
		defer m.wg.Done()
		t := time.NewTicker(m.cfg.HeartbeatInterval)
		defer t.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-t.C:
				if !m.heartbeatOnce(ctx, s) {
					return
				}
			}
		}
	}()
}

func (m *Manager) stopHeartbeat(s *session) {
	s.mu.Lock()
	defer s.mu.Unlock()
	m.stopHeartbeatLocked(s)
}

func (m *Manager) stopHeartbeatLocked(s *session) {
	if s.hbCancel != nil {
		s.hbCancel()
		s.hbCancel = nil
	}
}

// heartbeatOnce renews the lease if the game is still alive. Returns false
// when the heartbeat loop should end.
func (m *Manager) heartbeatOnce(ctx context.Context, s *session) bool {
	s.mu.Lock()
	tok := s.token
	if tok == nil {
		s.mu.Unlock()
		return false
	}
	state := s.view.State
	gameGone := s.gamePID > 0 && m.ProcessAlive != nil && !m.ProcessAlive(s.gamePID)
	stale := m.clock.Now().Sub(s.lastKeepalive) > m.cfg.GameLivenessTimeout
	if gameGone || stale {
		reason := "the game stopped responding"
		if gameGone {
			reason = "the game closed"
		}
		m.log.Warn("heartbeat_stopped", "world", tok.WorldID, "generation", tok.Generation, "reason", reason, "state", state)
		m.stopHeartbeatLocked(s)
		s.token = nil
		_ = m.sync.LocalState().Update(tok.WorldID, func(ls *syncer.LocalState) { ls.ActiveLease = nil })
		m.fail(s, &ErrorInfo{
			Code:               "GAME_EXITED",
			Message:            fmt.Sprintf("Hosting stopped because %s before the world was uploaded.\n\nYour local save is kept. If nobody else hosts first, it is recovered automatically the next time you press Play.", reason),
			LocalSaveUnchanged: true,
		})
		s.mu.Unlock()
		return false
	}
	upd := lease.LeaseUpdate{Players: s.players}
	switch state {
	case StateHosting:
		upd.Phase, upd.Join, upd.ClearJoin = model.PhaseHosting, s.join, s.join == nil
	case StateUploading:
		upd.Phase = model.PhaseSaving
		if s.stopping {
			upd.Phase = model.PhaseStopping
		}
	default:
		upd.Phase = model.PhasePreparing
	}
	s.mu.Unlock()

	_, err := m.leases.Renew(ctx, tok, upd)
	if err == nil {
		return true
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if errors.Is(err, lease.ErrFenced) {
		if s.token == tok {
			m.leaseLost(s)
		}
		return false
	}
	if ctx.Err() == nil {
		m.log.Warn("heartbeat_failed", "world", tok.WorldID, "generation", tok.Generation, "error", err.Error())
		m.note(s, "Connection to cloud storage lost; retrying...")
	}
	return true
}

// --- world creation ---

// CreateRequest creates a new shared world from an existing local save.
type CreateRequest struct {
	PlayRequest
	WorldID   string `json:"worldId"`
	WorldName string `json:"worldName"`
	// ImportSaveName is the stem of a save in SaveDirectory to upload as revision 1.
	ImportSaveName string `json:"importSaveName"`
}

// CreateWorld creates the cloud record and uploads the first revision.
func (m *Manager) CreateWorld(ctx context.Context, req CreateRequest) (*model.Revision, error) {
	if _, _, err := m.session(req.WorldID); err != nil {
		return nil, err
	}
	id := m.identity(req.PlayRequest)
	if err := id.Validate(); err != nil {
		return nil, err
	}
	if err := validateSaveDir(req.SaveDirectory); err != nil {
		return nil, err
	}
	src, err := savefile.PathFor(req.SaveDirectory, req.ImportSaveName)
	if err != nil {
		return nil, err
	}
	if _, err := savefile.CheckFile(src); err != nil {
		return nil, fmt.Errorf("the save to import is not valid: %w", err)
	}
	if _, err := m.leases.CreateWorld(ctx, req.WorldID, req.WorldName); err != nil {
		return nil, err
	}
	res, err := m.leases.Acquire(ctx, req.WorldID, id, newNonce())
	if err != nil {
		return nil, err
	}
	if res.Token == nil {
		return nil, errors.New("someone else claimed the new world first")
	}
	defer func() {
		rctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
		defer cancel()
		_ = m.leases.Release(rctx, res.Token)
	}()
	up, err := m.sync.Upload(ctx, res.Token, src, "import")
	if err != nil {
		return nil, err
	}
	m.invalidate(req.WorldID)
	return &up.Revision, nil
}

// History returns the accepted revisions (newest first).
func (m *Manager) History(ctx context.Context, worldID string) ([]model.Revision, error) {
	if _, _, err := m.session(worldID); err != nil {
		return nil, err
	}
	rec, _, err := m.leases.Load(ctx, worldID)
	if err != nil {
		return nil, err
	}
	return rec.History, nil
}
