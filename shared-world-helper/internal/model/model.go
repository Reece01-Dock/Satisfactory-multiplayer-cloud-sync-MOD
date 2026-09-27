// Package model defines the authoritative shared-world record stored in the
// cloud, plus the validation applied to it. Remote metadata is never trusted
// blindly: every record read from a provider goes through Validate before use.
package model

import (
	"encoding/hex"
	"errors"
	"fmt"
	"regexp"
	"strings"
	"time"
	"unicode/utf8"
)

// SchemaVersion of WorldRecord. Readers refuse records with a newer schema
// rather than guessing, so an old helper can never rewrite a record it does
// not fully understand.
const SchemaVersion = 1

// MaxHistory is how many accepted revisions are kept listed in the record.
// Older blobs remain in storage until a retention policy removes them.
const MaxHistory = 50

var worldIDPattern = regexp.MustCompile(`^[a-z0-9][a-z0-9-]{0,62}$`)

// ValidateWorldID rejects anything that could be used for path traversal or
// that is awkward as a storage key.
func ValidateWorldID(id string) error {
	if !worldIDPattern.MatchString(id) {
		return fmt.Errorf("invalid world id %q: must match %s", id, worldIDPattern)
	}
	return nil
}

// ValidateSHA256 checks for a lowercase hex SHA-256 digest.
func ValidateSHA256(h string) error {
	if len(h) != 64 || strings.ToLower(h) != h {
		return fmt.Errorf("invalid sha256 %q", h)
	}
	if _, err := hex.DecodeString(h); err != nil {
		return fmt.Errorf("invalid sha256 %q: %w", h, err)
	}
	return nil
}

func validateText(field, s string, max int) error {
	if !utf8.ValidString(s) {
		return fmt.Errorf("%s is not valid UTF-8", field)
	}
	if len(s) > max {
		return fmt.Errorf("%s too long (%d > %d)", field, len(s), max)
	}
	for _, r := range s {
		if r < 0x20 && r != '\t' {
			return fmt.Errorf("%s contains control characters", field)
		}
	}
	return nil
}

// Identity describes a player running a helper. PlayerID is the game's
// online account id (as reported by the mod); InstallID is a random id per
// helper installation; neither is a secret.
type Identity struct {
	PlayerID    string `json:"playerId"`
	DisplayName string `json:"displayName"`
	Platform    string `json:"platform"`
	InstallID   string `json:"installId"`
}

func (i Identity) Validate() error {
	if i.PlayerID == "" || i.InstallID == "" {
		return errors.New("identity requires playerId and installId")
	}
	for f, v := range map[string]string{"playerId": i.PlayerID, "displayName": i.DisplayName, "platform": i.Platform, "installId": i.InstallID} {
		if err := validateText(f, v, 128); err != nil {
			return err
		}
	}
	return nil
}

// Revision is one accepted, immutable save of the world.
type Revision struct {
	Number       uint64    `json:"number"`
	BlobKey      string    `json:"blobKey"`
	SHA256       string    `json:"sha256"`
	Size         int64     `json:"size"`
	CreatedAt    time.Time `json:"createdAt"`
	Uploader     Identity  `json:"uploader"`
	Generation   uint64    `json:"generation"`
	BaseRevision uint64    `json:"baseRevision"`
	Reason       string    `json:"reason"`
}

// LeasePhase is what the current lease holder is doing. It is informational
// for other players; authority comes from Generation alone.
type LeasePhase string

const (
	PhasePreparing LeasePhase = "PREPARING" // lease acquired, downloading / loading
	PhaseHosting   LeasePhase = "HOSTING"   // world loaded, joinable
	PhaseSaving    LeasePhase = "SAVING"    // host is saving / uploading a checkpoint
	PhaseStopping  LeasePhase = "STOPPING"  // final save + upload before release
)

func (p LeasePhase) valid() bool {
	switch p {
	case PhasePreparing, PhaseHosting, PhaseSaving, PhaseStopping:
		return true
	}
	return false
}

// JoinKind is how a client reaches the host.
type JoinKind string

const (
	// JoinOnlineSessionID is the string form produced by
	// UCommonSessionSubsystem::OnlineSessionIdToString on the host.
	JoinOnlineSessionID JoinKind = "online-session-id"
	// JoinAddress is a raw host:port for FSessionJoinParams::RawAddress.
	JoinAddress JoinKind = "address"
)

// JoinInfo is published by the host once its game session exists.
type JoinInfo struct {
	Kind    JoinKind `json:"kind"`
	Value   string   `json:"value"`
	Backend string   `json:"backend,omitempty"`
}

func (j JoinInfo) Validate() error {
	if j.Kind != JoinOnlineSessionID && j.Kind != JoinAddress {
		return fmt.Errorf("unknown join kind %q", j.Kind)
	}
	if j.Value == "" {
		return errors.New("join value is empty")
	}
	if err := validateText("join.value", j.Value, 512); err != nil {
		return err
	}
	return validateText("join.backend", j.Backend, 64)
}

// Player is a connected player as reported by the host's game.
type Player struct {
	DisplayName string `json:"displayName"`
	PlayerID    string `json:"playerId,omitempty"`
}

// Lease grants exclusive host and upload authority for one Generation.
type Lease struct {
	Generation   uint64     `json:"generation"`
	Holder       Identity   `json:"holder"`
	SessionNonce string     `json:"sessionNonce"`
	AcquiredAt   time.Time  `json:"acquiredAt"`
	RenewedAt    time.Time  `json:"renewedAt"`
	ExpiresAt    time.Time  `json:"expiresAt"`
	BaseRevision uint64     `json:"baseRevision"`
	Phase        LeasePhase `json:"phase"`
	Join         *JoinInfo  `json:"join,omitempty"`
	Players      []Player   `json:"players,omitempty"`
}

// SessionEnd records how the previous hosting session ended.
type SessionEnd struct {
	Host       Identity  `json:"host"`
	Generation uint64    `json:"generation"`
	EndedAt    time.Time `json:"endedAt"`
	Reason     string    `json:"reason"` // "released" | "expired"
}

// WorldRecord is the single authoritative metadata object per world. It is
// only ever modified with compare-and-swap, which is what makes host
// acquisition atomic and fencing enforceable.
type WorldRecord struct {
	SchemaVersion int         `json:"schemaVersion"`
	WorldID       string      `json:"worldId"`
	WorldName     string      `json:"worldName"`
	Generation    uint64      `json:"generation"` // last fencing token issued
	Head          *Revision   `json:"head,omitempty"`
	History       []Revision  `json:"history,omitempty"` // newest first, includes Head
	Lease         *Lease      `json:"lease,omitempty"`
	LastSession   *SessionEnd `json:"lastSession,omitempty"`
	UpdatedAt     time.Time   `json:"updatedAt"`
}

// HeadNumber is the current revision number, 0 when no save exists yet.
func (r *WorldRecord) HeadNumber() uint64 {
	if r.Head == nil {
		return 0
	}
	return r.Head.Number
}

// BlobPrefix is the storage prefix for a world's immutable save blobs.
func BlobPrefix(worldID string) string { return "worlds/" + worldID + "/saves/" }

// RecordKey is the storage key of a world's metadata object.
func RecordKey(worldID string) string { return "worlds/" + worldID + "/world.json" }

// BlobKeyFor builds the immutable key for a new revision. The generation and
// hash are part of the key so two writers can never collide on a key.
func BlobKeyFor(worldID string, rev, gen uint64, sha string) string {
	return fmt.Sprintf("%sr%08d-g%08d-%s.sav", BlobPrefix(worldID), rev, gen, sha[:16])
}

var blobNamePattern = regexp.MustCompile(`^r\d{8}-g\d{8}-[0-9a-f]{16}\.sav$`)

func validateRevision(worldID string, r *Revision) error {
	if r.Number == 0 {
		return errors.New("revision number must be >= 1")
	}
	if !strings.HasPrefix(r.BlobKey, BlobPrefix(worldID)) || !blobNamePattern.MatchString(strings.TrimPrefix(r.BlobKey, BlobPrefix(worldID))) {
		return fmt.Errorf("revision %d has unexpected blob key %q", r.Number, r.BlobKey)
	}
	if err := ValidateSHA256(r.SHA256); err != nil {
		return err
	}
	if r.Size <= 0 {
		return fmt.Errorf("revision %d has invalid size %d", r.Number, r.Size)
	}
	if r.Generation == 0 {
		return fmt.Errorf("revision %d has no generation", r.Number)
	}
	return validateText("revision.reason", r.Reason, 64)
}

// Validate checks a record read from (or about to be written to) storage.
func (r *WorldRecord) Validate(expectedWorldID string) error {
	if r.SchemaVersion != SchemaVersion {
		return fmt.Errorf("unsupported schemaVersion %d (this helper understands %d); update the helper", r.SchemaVersion, SchemaVersion)
	}
	if r.WorldID != expectedWorldID {
		return fmt.Errorf("record world id %q does not match expected %q", r.WorldID, expectedWorldID)
	}
	if err := ValidateWorldID(r.WorldID); err != nil {
		return err
	}
	if err := validateText("worldName", r.WorldName, 128); err != nil {
		return err
	}
	if r.Head != nil {
		if err := validateRevision(r.WorldID, r.Head); err != nil {
			return fmt.Errorf("head: %w", err)
		}
		if r.Head.Generation > r.Generation {
			return errors.New("head generation is newer than record generation")
		}
		if len(r.History) == 0 || r.History[0].Number != r.Head.Number || r.History[0].SHA256 != r.Head.SHA256 || r.History[0].BlobKey != r.Head.BlobKey {
			return errors.New("history[0] must equal head")
		}
	} else if len(r.History) != 0 {
		return errors.New("history present without head")
	}
	if len(r.History) > MaxHistory {
		return errors.New("history too long")
	}
	for i := range r.History {
		if err := validateRevision(r.WorldID, &r.History[i]); err != nil {
			return fmt.Errorf("history[%d]: %w", i, err)
		}
		if i > 0 && r.History[i].Number >= r.History[i-1].Number {
			return errors.New("history is not strictly descending")
		}
	}
	if l := r.Lease; l != nil {
		if l.Generation == 0 || l.Generation != r.Generation {
			return fmt.Errorf("lease generation %d does not match record generation %d", l.Generation, r.Generation)
		}
		if err := l.Holder.Validate(); err != nil {
			return fmt.Errorf("lease holder: %w", err)
		}
		if l.SessionNonce == "" || len(l.SessionNonce) > 64 {
			return errors.New("lease session nonce invalid")
		}
		if !l.ExpiresAt.After(l.AcquiredAt) {
			return errors.New("lease expiry precedes acquisition")
		}
		if !l.Phase.valid() {
			return fmt.Errorf("unknown lease phase %q", l.Phase)
		}
		if l.BaseRevision > r.HeadNumber() {
			return errors.New("lease base revision is ahead of head")
		}
		if l.Join != nil {
			if err := l.Join.Validate(); err != nil {
				return err
			}
		}
		if len(l.Players) > 128 {
			return errors.New("too many players")
		}
		for _, p := range l.Players {
			if err := validateText("player.displayName", p.DisplayName, 128); err != nil {
				return err
			}
		}
	}
	return nil
}

// NewRecord creates the initial record for a world that has no metadata yet.
func NewRecord(worldID, name string, now time.Time) *WorldRecord {
	return &WorldRecord{SchemaVersion: SchemaVersion, WorldID: worldID, WorldName: name, UpdatedAt: now}
}
