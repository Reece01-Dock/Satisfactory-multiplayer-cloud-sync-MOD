// Package lease implements host acquisition, heartbeats, release and the
// fencing rules that protect save upload authority.
//
// Every operation is a read-modify-CAS loop on the single world record:
//
//   - Acquire succeeds only if no live lease exists; it increments the
//     record's Generation, which becomes the holder's fencing token.
//   - Renew, Commit and Release succeed only while the record still carries
//     the holder's Generation and nonce. Once anyone else acquires, the
//     generation moves on and every later write from the old holder is
//     rejected (ErrFenced), even if that process wakes up much later.
//   - Commit additionally requires Head == the holder's base revision, so a
//     writer holding revision N can never replace revision N+1.
package lease

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"math/rand/v2"
	"time"

	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/clock"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/model"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/store"
)

var (
	// ErrFenced means this holder's generation is no longer current:
	// someone else acquired the world. The caller must stop hosting
	// authority immediately and must not retry the write.
	ErrFenced = errors.New("lease lost: another host has taken over this world")
	// ErrStaleRevision means the cloud head moved past the holder's base.
	ErrStaleRevision = errors.New("a newer shared save exists in the cloud")
	// ErrNoWorld means the world record does not exist.
	ErrNoWorld = errors.New("shared world does not exist")
	// ErrContention means CAS kept failing; the operation is safe to retry.
	ErrContention = errors.New("too much concurrent activity on this world, try again")
)

// Config controls lease timing. Defaults suit consumer internet links.
type Config struct {
	// TTL is how long a lease lives without a heartbeat.
	TTL time.Duration
	// SkewGrace is added when judging SOMEONE ELSE's lease expired, to
	// tolerate clock differences between machines.
	SkewGrace time.Duration
	// MaxCASAttempts bounds retries on contention.
	MaxCASAttempts int
}

func DefaultConfig() Config {
	return Config{TTL: 90 * time.Second, SkewGrace: 30 * time.Second, MaxCASAttempts: 20}
}

// Token is the local proof of lease ownership. It is never trusted by
// anyone but its owner: the record is the source of truth.
type Token struct {
	WorldID      string         `json:"worldId"`
	Generation   uint64         `json:"generation"`
	Nonce        string         `json:"nonce"`
	BaseRevision uint64         `json:"baseRevision"`
	Holder       model.Identity `json:"holder"`
}

// Manager performs lease operations against a MetadataStore.
type Manager struct {
	store store.MetadataStore
	clock clock.Clock
	cfg   Config
	log   *slog.Logger
}

func NewManager(s store.MetadataStore, c clock.Clock, cfg Config, log *slog.Logger) *Manager {
	return &Manager{store: s, clock: c, cfg: cfg, log: log}
}

func (m *Manager) Config() Config { return m.cfg }

// Load reads and validates a world record.
func (m *Manager) Load(ctx context.Context, worldID string) (*model.WorldRecord, store.Version, error) {
	if err := model.ValidateWorldID(worldID); err != nil {
		return nil, store.NoVersion, err
	}
	b, v, err := m.store.GetRecord(ctx, model.RecordKey(worldID))
	if errors.Is(err, store.ErrNotFound) {
		return nil, store.NoVersion, ErrNoWorld
	}
	if err != nil {
		return nil, store.NoVersion, fmt.Errorf("read world record: %w", err)
	}
	var rec model.WorldRecord
	if err := json.Unmarshal(b, &rec); err != nil {
		return nil, store.NoVersion, fmt.Errorf("world record is not valid JSON: %w", err)
	}
	if err := rec.Validate(worldID); err != nil {
		return nil, store.NoVersion, fmt.Errorf("world record failed validation: %w", err)
	}
	return &rec, v, nil
}

// CreateWorld writes an initial empty record. It fails if one exists.
func (m *Manager) CreateWorld(ctx context.Context, worldID, name string) (*model.WorldRecord, error) {
	if err := model.ValidateWorldID(worldID); err != nil {
		return nil, err
	}
	rec := model.NewRecord(worldID, name, m.clock.Now())
	if err := rec.Validate(worldID); err != nil {
		return nil, err
	}
	b, _ := json.Marshal(rec)
	if _, err := m.store.PutRecord(ctx, model.RecordKey(worldID), b, store.NoVersion); err != nil {
		if errors.Is(err, store.ErrPreconditionFailed) {
			return nil, fmt.Errorf("world %q already exists", worldID)
		}
		return nil, err
	}
	m.log.Info("world_created", "world", worldID)
	return rec, nil
}

// errNoChange lets a mutation decide nothing needs writing.
var errNoChange = errors.New("no change")

// mutate runs fn on the latest record and CAS-writes the result, retrying on
// lost races. fn must be pure with respect to the record it is given.
func (m *Manager) mutate(ctx context.Context, worldID string, fn func(rec *model.WorldRecord, now time.Time) error) (*model.WorldRecord, error) {
	for attempt := 0; attempt < m.cfg.MaxCASAttempts; attempt++ {
		rec, ver, err := m.Load(ctx, worldID)
		if err != nil {
			return nil, err
		}
		now := m.clock.Now()
		if err := fn(rec, now); err != nil {
			if errors.Is(err, errNoChange) {
				return rec, nil
			}
			return rec, err
		}
		rec.UpdatedAt = now
		if err := rec.Validate(worldID); err != nil {
			return nil, fmt.Errorf("refusing to write invalid record: %w", err)
		}
		b, err := json.Marshal(rec)
		if err != nil {
			return nil, err
		}
		_, err = m.store.PutRecord(ctx, model.RecordKey(worldID), b, ver)
		if err == nil {
			return rec, nil
		}
		if !errors.Is(err, store.ErrPreconditionFailed) {
			return nil, fmt.Errorf("write world record: %w", err)
		}
		m.log.Debug("cas_conflict", "world", worldID, "attempt", attempt+1)
		// Jittered backoff so simultaneous clickers do not stay in lockstep.
		d := time.Duration(5+rand.IntN(20*(attempt+1))) * time.Millisecond
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-time.After(d):
		}
	}
	return nil, ErrContention
}

// LiveForObserver reports whether a lease held by someone else must be
// respected. Expiry is judged with SkewGrace added so a fast local clock
// cannot steal a healthy host's world.
func (m *Manager) LiveForObserver(l *model.Lease, now time.Time) bool {
	return l != nil && now.Before(l.ExpiresAt.Add(m.cfg.SkewGrace))
}

// AcquireOutcome describes the result of Acquire.
type AcquireOutcome string

const (
	// Acquired: the caller is now the host with a fresh generation.
	Acquired AcquireOutcome = "ACQUIRED"
	// AlreadyHeld: the caller already holds this exact lease (idempotent retry).
	AlreadyHeld AcquireOutcome = "ALREADY_HELD"
	// HeldByOther: a live lease exists; the caller should join.
	HeldByOther AcquireOutcome = "HELD_BY_OTHER"
	// HeldBySelfElsewhere: a live lease exists for this same player or
	// install but a different session: a second game instance, another PC,
	// or a crashed session whose lease has not expired yet.
	HeldBySelfElsewhere AcquireOutcome = "HELD_BY_SELF_ELSEWHERE"
)

type AcquireResult struct {
	Outcome AcquireOutcome
	Token   *Token
	Record  *model.WorldRecord
	// TookOverExpired is set when the previous host's lease had expired
	// (crash / disconnect recovery).
	TookOverExpired *model.Lease
}

// Acquire atomically becomes host, or reports who is.
func (m *Manager) Acquire(ctx context.Context, worldID string, holder model.Identity, nonce string) (*AcquireResult, error) {
	if err := holder.Validate(); err != nil {
		return nil, err
	}
	if nonce == "" || len(nonce) > 64 {
		return nil, errors.New("invalid session nonce")
	}
	var res AcquireResult
	rec, err := m.mutate(ctx, worldID, func(rec *model.WorldRecord, now time.Time) error {
		res = AcquireResult{}
		if l := rec.Lease; l != nil {
			if l.SessionNonce == nonce && l.Holder.InstallID == holder.InstallID {
				res.Outcome = AlreadyHeld
				return errNoChange
			}
			if m.LiveForObserver(l, now) {
				if l.Holder.InstallID == holder.InstallID || l.Holder.PlayerID == holder.PlayerID {
					res.Outcome = HeldBySelfElsewhere
				} else {
					res.Outcome = HeldByOther
				}
				return errNoChange
			}
			expired := *l
			res.TookOverExpired = &expired
			rec.LastSession = &model.SessionEnd{Host: l.Holder, Generation: l.Generation, EndedAt: l.ExpiresAt, Reason: "expired"}
		}
		rec.Generation++
		rec.Lease = &model.Lease{
			Generation:   rec.Generation,
			Holder:       holder,
			SessionNonce: nonce,
			AcquiredAt:   now,
			RenewedAt:    now,
			ExpiresAt:    now.Add(m.cfg.TTL),
			BaseRevision: rec.HeadNumber(),
			Phase:        model.PhasePreparing,
		}
		res.Outcome = Acquired
		return nil
	})
	if err != nil {
		return nil, err
	}
	res.Record = rec
	if res.Outcome == Acquired || res.Outcome == AlreadyHeld {
		l := rec.Lease
		res.Token = &Token{WorldID: worldID, Generation: l.Generation, Nonce: l.SessionNonce, BaseRevision: l.BaseRevision, Holder: l.Holder}
	}
	if res.Outcome == Acquired {
		attrs := []any{"world", worldID, "owner", holder.PlayerID, "install", holder.InstallID, "generation", rec.Generation, "revision", rec.HeadNumber()}
		if res.TookOverExpired != nil {
			attrs = append(attrs, "recovered_from_generation", res.TookOverExpired.Generation)
		}
		m.log.Info("lease_acquired", attrs...)
	}
	return &res, nil
}

// checkFence verifies the record still belongs to tok.
func checkFence(rec *model.WorldRecord, tok *Token) error {
	l := rec.Lease
	if l == nil || rec.Generation != tok.Generation || l.Generation != tok.Generation || l.SessionNonce != tok.Nonce {
		return ErrFenced
	}
	return nil
}

// LeaseUpdate carries optional session details published with a heartbeat.
type LeaseUpdate struct {
	Phase   model.LeasePhase
	Join    *model.JoinInfo
	Players []model.Player
	// ClearJoin removes published join info (e.g. while stopping).
	ClearJoin bool
}

// Renew extends the lease. It still succeeds after local expiry as long as
// nobody else has acquired (the generation is unchanged), which lets a host
// that briefly lost internet continue safely.
func (m *Manager) Renew(ctx context.Context, tok *Token, upd LeaseUpdate) (*model.WorldRecord, error) {
	rec, err := m.mutate(ctx, tok.WorldID, func(rec *model.WorldRecord, now time.Time) error {
		if err := checkFence(rec, tok); err != nil {
			return err
		}
		l := rec.Lease
		if now.After(l.ExpiresAt) {
			m.log.Warn("lease_revived_after_expiry", "world", tok.WorldID, "generation", tok.Generation, "expired_for", now.Sub(l.ExpiresAt).String())
		}
		l.RenewedAt = now
		l.ExpiresAt = now.Add(m.cfg.TTL)
		if upd.Phase != "" {
			l.Phase = upd.Phase
		}
		if upd.ClearJoin {
			l.Join = nil
		} else if upd.Join != nil {
			j := *upd.Join
			l.Join = &j
		}
		if upd.Players != nil {
			l.Players = append([]model.Player(nil), upd.Players...)
		}
		return nil
	})
	if errors.Is(err, ErrFenced) {
		m.log.Warn("lease_fenced", "world", tok.WorldID, "generation", tok.Generation, "current_generation", generationOf(rec))
	}
	return rec, err
}

func generationOf(rec *model.WorldRecord) uint64 {
	if rec == nil {
		return 0
	}
	return rec.Generation
}

// Commit makes rev the new head. rev.Number must be tok.BaseRevision+1 and
// the blob must already be uploaded and verified. On success tok.BaseRevision
// advances to the new revision.
func (m *Manager) Commit(ctx context.Context, tok *Token, rev model.Revision) (*model.WorldRecord, error) {
	if rev.Number != tok.BaseRevision+1 || rev.Generation != tok.Generation || rev.BaseRevision != tok.BaseRevision {
		return nil, fmt.Errorf("commit: revision %d/gen %d does not follow token base %d/gen %d", rev.Number, rev.Generation, tok.BaseRevision, tok.Generation)
	}
	rec, err := m.mutate(ctx, tok.WorldID, func(rec *model.WorldRecord, now time.Time) error {
		if err := checkFence(rec, tok); err != nil {
			return err
		}
		if rec.HeadNumber() != tok.BaseRevision {
			return ErrStaleRevision
		}
		r := rev
		rec.Head = &r
		rec.History = append([]model.Revision{r}, rec.History...)
		if len(rec.History) > model.MaxHistory {
			rec.History = rec.History[:model.MaxHistory]
		}
		rec.Lease.BaseRevision = r.Number
		rec.Lease.RenewedAt = now
		rec.Lease.ExpiresAt = now.Add(m.cfg.TTL)
		return nil
	})
	if err != nil {
		m.log.Warn("commit_rejected", "world", tok.WorldID, "generation", tok.Generation, "base_revision", tok.BaseRevision, "current_generation", generationOf(rec), "error", err.Error())
		return rec, err
	}
	tok.BaseRevision = rev.Number
	m.log.Info("revision_committed", "world", tok.WorldID, "generation", tok.Generation, "revision", rev.Number, "sha256", rev.SHA256, "size", rev.Size, "reason", rev.Reason)
	return rec, nil
}

// Release ends the lease. Releasing a lease that was already taken over is
// reported as ErrFenced and changes nothing.
func (m *Manager) Release(ctx context.Context, tok *Token) error {
	_, err := m.mutate(ctx, tok.WorldID, func(rec *model.WorldRecord, now time.Time) error {
		if err := checkFence(rec, tok); err != nil {
			return err
		}
		rec.LastSession = &model.SessionEnd{Host: rec.Lease.Holder, Generation: rec.Lease.Generation, EndedAt: now, Reason: "released"}
		rec.Lease = nil
		return nil
	})
	if err == nil {
		m.log.Info("lease_released", "world", tok.WorldID, "generation", tok.Generation, "revision", tok.BaseRevision)
	}
	return err
}
