package syncer

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/model"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/savefile"
)

// BackupManager keeps local copies of saves before they are replaced and of
// saves that could not be uploaded (conflicts). Cloud revisions are backed
// up inherently: every accepted revision is an immutable blob.
type BackupManager struct {
	dir  string
	Keep int // most recent local backups kept per world; 0 = unlimited
	now  func() time.Time
}

func NewBackupManager(dir string, keep int, now func() time.Time) *BackupManager {
	return &BackupManager{dir: dir, Keep: keep, now: now}
}

// LocalBackup describes one backup file.
type LocalBackup struct {
	Path    string    `json:"path"`
	Name    string    `json:"name"`
	Size    int64     `json:"size"`
	Created time.Time `json:"created"`
}

func (b *BackupManager) worldDir(worldID string) (string, error) {
	if err := model.ValidateWorldID(worldID); err != nil {
		return "", err
	}
	return filepath.Join(b.dir, worldID), nil
}

// Preserve copies src into the world's backup directory. label is a short
// machine-generated tag such as "pre-download-r184" or "conflict-g591".
func (b *BackupManager) Preserve(worldID, src, label string) (string, error) {
	d, err := b.worldDir(worldID)
	if err != nil {
		return "", err
	}
	if err := savefile.ValidateSaveName(label); err != nil {
		return "", fmt.Errorf("invalid backup label: %w", err)
	}
	if err := os.MkdirAll(d, 0o700); err != nil {
		return "", err
	}
	name := b.now().UTC().Format("20060102T150405.000Z") + "_" + label + savefile.Extension
	dst := filepath.Join(d, name)
	if err := savefile.CopyFile(src, dst); err != nil {
		return "", fmt.Errorf("backup %s: %w", label, err)
	}
	b.prune(worldID)
	return dst, nil
}

// List returns backups newest first.
func (b *BackupManager) List(worldID string) ([]LocalBackup, error) {
	d, err := b.worldDir(worldID)
	if err != nil {
		return nil, err
	}
	entries, err := os.ReadDir(d)
	if os.IsNotExist(err) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	var out []LocalBackup
	for _, e := range entries {
		if e.IsDir() || !strings.HasSuffix(e.Name(), savefile.Extension) {
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		out = append(out, LocalBackup{Path: filepath.Join(d, e.Name()), Name: e.Name(), Size: info.Size(), Created: info.ModTime().UTC()})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name > out[j].Name })
	return out, nil
}

// prune removes the oldest non-conflict backups beyond Keep. Conflict
// backups hold progress that exists nowhere else and are never auto-deleted.
func (b *BackupManager) prune(worldID string) {
	if b.Keep <= 0 {
		return
	}
	list, err := b.List(worldID)
	if err != nil {
		return
	}
	kept := 0
	for _, bk := range list {
		if strings.Contains(bk.Name, "_conflict-") {
			continue
		}
		kept++
		if kept > b.Keep {
			_ = os.Remove(bk.Path)
		}
	}
}
