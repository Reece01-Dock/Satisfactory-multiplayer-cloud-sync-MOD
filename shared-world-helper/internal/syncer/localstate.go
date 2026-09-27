package syncer

import (
	"encoding/json"
	"errors"
	"io/fs"
	"os"
	"path/filepath"
	"sync"
	"time"

	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/lease"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/model"
)

// LocalState is what this machine knows about its copy of a world. It is
// the basis for detecting unsynced local progress after a crash.
type LocalState struct {
	WorldID string `json:"worldId"`
	// SyncedRevision/SHA256 describe the local save file as of the last
	// successful download or upload. A local file whose hash differs has
	// changes that never reached the cloud.
	SyncedRevision uint64    `json:"syncedRevision"`
	SyncedSHA256   string    `json:"syncedSha256,omitempty"`
	SyncedAt       time.Time `json:"syncedAt,omitempty"`
	// ActiveLease is persisted while hosting so a restarted helper can
	// resume heartbeats (only if the cloud still shows this generation).
	ActiveLease *lease.Token `json:"activeLease,omitempty"`
}

// LocalStateStore persists LocalState as one JSON file per world.
type LocalStateStore struct {
	dir string
	mu  sync.Mutex
}

func NewLocalStateStore(dir string) (*LocalStateStore, error) {
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return nil, err
	}
	return &LocalStateStore{dir: dir}, nil
}

func (s *LocalStateStore) path(worldID string) (string, error) {
	if err := model.ValidateWorldID(worldID); err != nil {
		return "", err
	}
	return filepath.Join(s.dir, worldID+".json"), nil
}

func (s *LocalStateStore) Load(worldID string) (LocalState, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	p, err := s.path(worldID)
	if err != nil {
		return LocalState{}, err
	}
	b, err := os.ReadFile(p)
	if errors.Is(err, fs.ErrNotExist) {
		return LocalState{WorldID: worldID}, nil
	}
	if err != nil {
		return LocalState{}, err
	}
	var st LocalState
	if err := json.Unmarshal(b, &st); err != nil || st.WorldID != worldID {
		// A corrupt local state only loses the "unsynced changes" hint; the
		// safe interpretation is "nothing known", which forces backups.
		return LocalState{WorldID: worldID}, nil
	}
	return st, nil
}

func (s *LocalStateStore) Update(worldID string, fn func(*LocalState)) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	p, err := s.path(worldID)
	if err != nil {
		return err
	}
	st := LocalState{WorldID: worldID}
	if b, err := os.ReadFile(p); err == nil {
		_ = json.Unmarshal(b, &st)
		st.WorldID = worldID
	}
	fn(&st)
	b, err := json.MarshalIndent(st, "", "  ")
	if err != nil {
		return err
	}
	tmp := p + ".tmp"
	if err := os.WriteFile(tmp, b, 0o600); err != nil {
		return err
	}
	return os.Rename(tmp, p)
}
