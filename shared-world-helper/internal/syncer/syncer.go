// Package syncer moves save files between the local save directory and the
// cloud. It never makes a blob authoritative itself: uploads end in a
// fenced lease.Commit, downloads only replace a local file after the
// downloaded bytes match the committed hash.
package syncer

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"time"

	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/clock"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/lease"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/model"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/savefile"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/store"
)

var (
	// ErrNoSave means the world has no revision yet.
	ErrNoSave = errors.New("this shared world has no save yet")
	// ErrCorruptDownload means the downloaded bytes did not match metadata.
	ErrCorruptDownload = errors.New("downloaded save failed verification")
	// ErrUploadVerify means the uploaded blob did not read back correctly.
	ErrUploadVerify = errors.New("uploaded save failed verification")
)

// ConflictError wraps a refused upload and records where the local save was
// preserved.
type ConflictError struct {
	Err        error
	BackupPath string
	LocalBase  uint64
	CloudHead  uint64
}

func (e *ConflictError) Error() string {
	return fmt.Sprintf("%v (cloud revision %d, local base revision %d); local save preserved at %s", e.Err, e.CloudHead, e.LocalBase, e.BackupPath)
}
func (e *ConflictError) Unwrap() error { return e.Err }

// Config tunes the syncer.
type Config struct {
	StagingDir string
	// StableQuiet is how long a save file must be unchanged before upload.
	StableQuiet   time.Duration
	StableTimeout time.Duration
}

// Syncer implements download and upload for all worlds.
type Syncer struct {
	blobs   store.BlobStore
	leases  *lease.Manager
	backups *BackupManager
	state   *LocalStateStore
	clock   clock.Clock
	cfg     Config
	log     *slog.Logger
}

func New(blobs store.BlobStore, leases *lease.Manager, backups *BackupManager, state *LocalStateStore, c clock.Clock, cfg Config, log *slog.Logger) *Syncer {
	return &Syncer{blobs: blobs, leases: leases, backups: backups, state: state, clock: c, cfg: cfg, log: log}
}

func (s *Syncer) Backups() *BackupManager      { return s.backups }
func (s *Syncer) LocalState() *LocalStateStore { return s.state }

// LocalStatus classifies the local save relative to what was last synced.
type LocalStatus string

const (
	LocalMissing  LocalStatus = "MISSING"
	LocalInSync   LocalStatus = "IN_SYNC"  // identical to last synced revision
	LocalModified LocalStatus = "MODIFIED" // has changes never uploaded
	LocalUnknown  LocalStatus = "UNKNOWN"  // exists but we have no sync record
)

// InspectLocal hashes the local save and compares it with local state.
func (s *Syncer) InspectLocal(worldID, path string) (LocalStatus, string, LocalState, error) {
	st, err := s.state.Load(worldID)
	if err != nil {
		return "", "", st, err
	}
	sha, _, err := savefile.HashFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return LocalMissing, "", st, nil
	}
	if err != nil {
		return "", "", st, err
	}
	switch {
	case st.SyncedSHA256 == "":
		return LocalUnknown, sha, st, nil
	case st.SyncedSHA256 == sha:
		return LocalInSync, sha, st, nil
	default:
		return LocalModified, sha, st, nil
	}
}

// DownloadResult describes a completed download.
type DownloadResult struct {
	Revision     uint64
	Path         string
	AlreadyLocal bool   // local file already matched; nothing transferred
	BackupPath   string // previous local file, if one was replaced
}

// Download fetches rec.Head into target. The local file is replaced only
// after the full download matched the committed SHA-256 and size, and after
// the previous local file (if any) was backed up.
func (s *Syncer) Download(ctx context.Context, rec *model.WorldRecord, target string) (*DownloadResult, error) {
	head := rec.Head
	if head == nil {
		return nil, ErrNoSave
	}
	log := s.log.With("world", rec.WorldID, "revision", head.Number)

	if sha, _, err := savefile.HashFile(target); err == nil && sha == head.SHA256 {
		log.Info("download_skipped_already_current", "sha256", sha)
		if err := s.markSynced(rec.WorldID, head.Number, sha); err != nil {
			return nil, err
		}
		return &DownloadResult{Revision: head.Number, Path: target, AlreadyLocal: true}, nil
	}

	log.Info("download_started", "blob", head.BlobKey, "size", head.Size)
	rc, err := s.blobs.GetBlob(ctx, head.BlobKey)
	if err != nil {
		return nil, fmt.Errorf("download revision %d: %w", head.Number, err)
	}
	defer rc.Close()

	tmp := savefile.TempPathNextTo(target)
	out, err := os.OpenFile(tmp, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o600)
	if err != nil {
		return nil, fmt.Errorf("create temp file: %w", err)
	}
	cleanup := func() { out.Close(); os.Remove(tmp) }
	h := sha256.New()
	n, err := io.Copy(io.MultiWriter(out, h), io.LimitReader(rc, head.Size+1))
	if err != nil {
		cleanup()
		return nil, fmt.Errorf("download revision %d interrupted: %w", head.Number, err)
	}
	if err := out.Sync(); err != nil {
		cleanup()
		return nil, err
	}
	if err := out.Close(); err != nil {
		os.Remove(tmp)
		return nil, err
	}
	got := hex.EncodeToString(h.Sum(nil))
	if n != head.Size || got != head.SHA256 {
		os.Remove(tmp)
		log.Error("download_verification_failed", "expected_sha256", head.SHA256, "got_sha256", got, "expected_size", head.Size, "got_size", n)
		return nil, fmt.Errorf("%w: revision %d expected %d bytes sha256 %s, got %d bytes sha256 %s", ErrCorruptDownload, head.Number, head.Size, head.SHA256[:12], n, got[:12])
	}
	if _, err := savefile.CheckFile(tmp); err != nil {
		os.Remove(tmp)
		return nil, fmt.Errorf("%w: %v", ErrCorruptDownload, err)
	}
	log.Info("download_verified", "sha256", got)

	res := &DownloadResult{Revision: head.Number, Path: target}
	if _, err := os.Stat(target); err == nil {
		bp, err := s.backups.Preserve(rec.WorldID, target, fmt.Sprintf("pre-download-r%d", head.Number))
		if err != nil {
			os.Remove(tmp)
			return nil, fmt.Errorf("could not back up existing local save, refusing to replace it: %w", err)
		}
		res.BackupPath = bp
		log.Info("local_backup_created", "path", bp)
	}
	if err := savefile.ReplaceAtomic(tmp, target); err != nil {
		os.Remove(tmp)
		return nil, fmt.Errorf("place save: %w", err)
	}
	if err := s.markSynced(rec.WorldID, head.Number, got); err != nil {
		return nil, err
	}
	log.Info("download_placed", "path", target)
	return res, nil
}

func (s *Syncer) markSynced(worldID string, rev uint64, sha string) error {
	now := s.clock.Now()
	return s.state.Update(worldID, func(st *LocalState) {
		st.SyncedRevision, st.SyncedSHA256, st.SyncedAt = rev, sha, now
	})
}

// UploadResult describes an upload attempt.
type UploadResult struct {
	Revision  model.Revision
	Unchanged bool // local save identical to head; nothing uploaded
}

// Upload publishes the save at src as revision tok.BaseRevision+1.
//
// Order of operations (each step fails closed):
//  1. wait until the file stops changing and parses as a save
//  2. snapshot it privately and hash the snapshot
//  3. pre-check the cloud record (fence + base revision)
//  4. upload the snapshot to a new, unique, create-only blob key
//  5. read the blob back and verify hash and size
//  6. fenced CAS commit of the record to point at the new blob
//
// If anything refuses the write, the snapshot is preserved as a conflict
// backup and the cloud head is untouched.
func (s *Syncer) Upload(ctx context.Context, tok *lease.Token, src, reason string) (*UploadResult, error) {
	log := s.log.With("world", tok.WorldID, "generation", tok.Generation, "base_revision", tok.BaseRevision)
	if err := savefile.WaitStable(src, s.cfg.StableQuiet, s.cfg.StableTimeout); err != nil {
		return nil, fmt.Errorf("save not ready for upload: %w", err)
	}
	if _, err := savefile.CheckFile(src); err != nil {
		return nil, fmt.Errorf("refusing to upload a file that is not a valid save: %w", err)
	}
	snap, sha, size, err := savefile.Snapshot(src, s.cfg.StagingDir)
	if err != nil {
		return nil, fmt.Errorf("snapshot save: %w", err)
	}
	defer os.Remove(snap)
	log = log.With("sha256", sha, "size", size)

	preserve := func(cause error, cloudHead uint64) error {
		bp, berr := s.backups.Preserve(tok.WorldID, snap, fmt.Sprintf("conflict-g%d-r%d", tok.Generation, tok.BaseRevision))
		if berr != nil {
			log.Error("conflict_backup_failed", "error", berr.Error())
			bp = "(backup failed: " + berr.Error() + "; the original file at " + src + " was not modified)"
		}
		log.Warn("upload_refused", "reason", cause.Error(), "cloud_revision", cloudHead, "backup", bp)
		return &ConflictError{Err: cause, BackupPath: bp, LocalBase: tok.BaseRevision, CloudHead: cloudHead}
	}

	rec, _, err := s.leases.Load(ctx, tok.WorldID)
	if err != nil {
		return nil, fmt.Errorf("check cloud state before upload: %w", err)
	}
	if rec.Lease == nil || rec.Generation != tok.Generation || rec.Lease.SessionNonce != tok.Nonce {
		return nil, preserve(lease.ErrFenced, rec.HeadNumber())
	}
	if rec.HeadNumber() != tok.BaseRevision {
		return nil, preserve(lease.ErrStaleRevision, rec.HeadNumber())
	}
	if rec.Head != nil && rec.Head.SHA256 == sha {
		log.Info("upload_skipped_unchanged")
		if err := s.markSynced(tok.WorldID, rec.Head.Number, sha); err != nil {
			return nil, err
		}
		return &UploadResult{Revision: *rec.Head, Unchanged: true}, nil
	}

	rev := model.Revision{
		Number:       tok.BaseRevision + 1,
		BlobKey:      model.BlobKeyFor(tok.WorldID, tok.BaseRevision+1, tok.Generation, sha),
		SHA256:       sha,
		Size:         size,
		CreatedAt:    s.clock.Now(),
		Uploader:     tok.Holder,
		Generation:   tok.Generation,
		BaseRevision: tok.BaseRevision,
		Reason:       reason,
	}
	log = log.With("revision", rev.Number, "blob", rev.BlobKey)
	log.Info("upload_started")
	if err := s.putBlob(ctx, rev, snap); err != nil {
		log.Error("upload_failed", "error", err.Error())
		return nil, err
	}
	if err := s.verifyBlob(ctx, rev); err != nil {
		log.Error("upload_verification_failed", "error", err.Error())
		_ = s.blobs.DeleteBlob(ctx, rev.BlobKey) // unreferenced, safe to remove
		return nil, err
	}
	log.Info("upload_verified")

	if _, err := s.leases.Commit(ctx, tok, rev); err != nil {
		if errors.Is(err, lease.ErrFenced) || errors.Is(err, lease.ErrStaleRevision) {
			// Definitely not committed: the blob is unreferenced.
			_ = s.blobs.DeleteBlob(ctx, rev.BlobKey)
			cur := uint64(0)
			if r, _, lerr := s.leases.Load(ctx, tok.WorldID); lerr == nil {
				cur = r.HeadNumber()
			}
			return nil, preserve(err, cur)
		}
		// Ambiguous (e.g. the response was lost): check whether it landed.
		if r, _, lerr := s.leases.Load(ctx, tok.WorldID); lerr == nil && r.Head != nil && r.Head.BlobKey == rev.BlobKey && r.Generation == tok.Generation {
			tok.BaseRevision = rev.Number
			log.Warn("commit_outcome_recovered", "error", err.Error())
		} else {
			// Keep the blob: a later retry may still need it. It is
			// unreferenced, so it can never be mistaken for the head.
			return nil, fmt.Errorf("commit revision %d: %w", rev.Number, err)
		}
	}
	if err := s.markSynced(tok.WorldID, rev.Number, sha); err != nil {
		return nil, err
	}
	return &UploadResult{Revision: rev}, nil
}

func (s *Syncer) putBlob(ctx context.Context, rev model.Revision, snap string) error {
	f, err := os.Open(snap)
	if err != nil {
		return err
	}
	defer f.Close()
	err = s.blobs.PutBlob(ctx, rev.BlobKey, f, rev.Size)
	if errors.Is(err, store.ErrAlreadyExists) {
		// A previous attempt of this exact upload got this far. The key
		// embeds generation and hash, so verification decides.
		return nil
	}
	if err != nil {
		return fmt.Errorf("upload revision %d: %w", rev.Number, err)
	}
	return nil
}

func (s *Syncer) verifyBlob(ctx context.Context, rev model.Revision) error {
	rc, err := s.blobs.GetBlob(ctx, rev.BlobKey)
	if err != nil {
		return fmt.Errorf("%w: %v", ErrUploadVerify, err)
	}
	defer rc.Close()
	sha, n, err := savefile.HashReader(rc)
	if err != nil {
		return fmt.Errorf("%w: %v", ErrUploadVerify, err)
	}
	if sha != rev.SHA256 || n != rev.Size {
		return fmt.Errorf("%w: expected %d bytes %s, read back %d bytes %s", ErrUploadVerify, rev.Size, rev.SHA256[:12], n, sha[:12])
	}
	return nil
}
