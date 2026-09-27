package store

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

// FS is a Provider backed by a directory.
//
// SAFETY: compare-and-swap is implemented with an exclusive lock file plus an
// atomic rename. That is correct for several processes on one machine and on
// network filesystems with working O_EXCL semantics. It is NOT correct on
// folders replicated by sync clients (Google Drive for desktop, OneDrive,
// Dropbox folders): those replicate files eventually, so two machines can both
// "win". Use a cloud provider with native conditional writes for real
// multi-machine play.
type FS struct {
	root string
	mu   sync.Mutex // serializes CAS within this process

	// StaleLockAfter is how old a lock file must be before it is presumed
	// abandoned by a crashed process and broken.
	StaleLockAfter time.Duration
}

func NewFS(root string) (*FS, error) {
	abs, err := filepath.Abs(root)
	if err != nil {
		return nil, err
	}
	if err := os.MkdirAll(abs, 0o700); err != nil {
		return nil, err
	}
	return &FS{root: abs, StaleLockAfter: 30 * time.Second}, nil
}

func (f *FS) Name() string { return "filesystem" }

func (f *FS) path(key string) (string, error) {
	if err := ValidateKey(key); err != nil {
		return "", err
	}
	p := filepath.Join(f.root, filepath.FromSlash(key))
	// Defense in depth on top of ValidateKey.
	if !strings.HasPrefix(p, f.root+string(filepath.Separator)) {
		return "", fmt.Errorf("key %q escapes root", key)
	}
	return p, nil
}

// envelope wraps a record with a random per-write version so a CAS can never
// be fooled by content that changed and changed back (ABA).
type envelope struct {
	Version string          `json:"version"`
	Record  json.RawMessage `json:"record"`
}

func randomToken(n int) string {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		panic(err) // crypto/rand failing is unrecoverable
	}
	return hex.EncodeToString(b)
}

func (f *FS) readEnvelope(p string) (*envelope, error) {
	b, err := os.ReadFile(p)
	if errors.Is(err, fs.ErrNotExist) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	var e envelope
	if err := json.Unmarshal(b, &e); err != nil || e.Version == "" {
		return nil, fmt.Errorf("corrupt metadata file %s", filepath.Base(p))
	}
	return &e, nil
}

func (f *FS) GetRecord(_ context.Context, key string) ([]byte, Version, error) {
	p, err := f.path(key)
	if err != nil {
		return nil, NoVersion, err
	}
	e, err := f.readEnvelope(p)
	if err != nil {
		return nil, NoVersion, err
	}
	return e.Record, Version(e.Version), nil
}

func (f *FS) lock(ctx context.Context, p string) (func(), error) {
	lockPath := p + ".lock"
	delay := 5 * time.Millisecond
	for {
		fh, err := os.OpenFile(lockPath, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o600)
		if err == nil {
			_, _ = fmt.Fprintf(fh, "%d %s\n", os.Getpid(), time.Now().UTC().Format(time.RFC3339Nano))
			_ = fh.Close()
			return func() { _ = os.Remove(lockPath) }, nil
		}
		// On Windows a lock file that another process is deleting is in a
		// "delete pending" state and creating it fails with "Access is
		// denied" instead of "exists"; both mean "busy, retry".
		if !errors.Is(err, fs.ErrExist) && !errors.Is(err, fs.ErrPermission) {
			return nil, err
		}
		if st, serr := os.Stat(lockPath); serr == nil && time.Since(st.ModTime()) > f.StaleLockAfter {
			_ = os.Remove(lockPath) // abandoned by a crashed process
			continue
		}
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-time.After(delay):
		}
		if delay < 100*time.Millisecond {
			delay *= 2
		}
	}
}

func (f *FS) PutRecord(ctx context.Context, key string, data []byte, expected Version) (Version, error) {
	p, err := f.path(key)
	if err != nil {
		return NoVersion, err
	}
	if !json.Valid(data) {
		return NoVersion, errors.New("record is not valid JSON")
	}
	if err := os.MkdirAll(filepath.Dir(p), 0o700); err != nil {
		return NoVersion, err
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	if _, ok := ctx.Deadline(); !ok {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, 10*time.Second)
		defer cancel()
	}
	unlock, err := f.lock(ctx, p)
	if err != nil {
		return NoVersion, fmt.Errorf("acquire metadata lock: %w", err)
	}
	defer unlock()

	cur, err := f.readEnvelope(p)
	switch {
	case errors.Is(err, ErrNotFound):
		if expected != NoVersion {
			return NoVersion, ErrPreconditionFailed
		}
	case err != nil:
		return NoVersion, err
	default:
		if expected == NoVersion || Version(cur.Version) != expected {
			return NoVersion, ErrPreconditionFailed
		}
	}
	nv := randomToken(16)
	out, err := json.Marshal(envelope{Version: nv, Record: data})
	if err != nil {
		return NoVersion, err
	}
	if err := writeFileAtomic(p, out); err != nil {
		return NoVersion, err
	}
	return Version(nv), nil
}

// writeFileAtomic writes to a temp file in the same directory, fsyncs, and
// renames over the destination so readers see either old or new content.
func writeFileAtomic(p string, data []byte) error {
	tmp := p + ".tmp-" + randomToken(6)
	fh, err := os.OpenFile(tmp, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o600)
	if err != nil {
		return err
	}
	if _, err := fh.Write(data); err != nil {
		fh.Close()
		os.Remove(tmp)
		return err
	}
	if err := fh.Sync(); err != nil {
		fh.Close()
		os.Remove(tmp)
		return err
	}
	if err := fh.Close(); err != nil {
		os.Remove(tmp)
		return err
	}
	if err := os.Rename(tmp, p); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}

func (f *FS) PutBlob(ctx context.Context, key string, r io.Reader, size int64) error {
	p, err := f.path(key)
	if err != nil {
		return err
	}
	if _, err := os.Stat(p); err == nil {
		return ErrAlreadyExists
	}
	if err := os.MkdirAll(filepath.Dir(p), 0o700); err != nil {
		return err
	}
	tmp := p + ".partial-" + randomToken(6)
	fh, err := os.OpenFile(tmp, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o600)
	if err != nil {
		return err
	}
	n, err := io.Copy(fh, r)
	if err == nil && size >= 0 && n != size {
		err = fmt.Errorf("short blob write: %d of %d bytes", n, size)
	}
	if err == nil {
		err = fh.Sync()
	}
	if cerr := fh.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		os.Remove(tmp)
		return err
	}
	// Hard-linking fails if the destination exists, giving create-only
	// semantics atomically on both NTFS and POSIX filesystems.
	if err := os.Link(tmp, p); err != nil {
		os.Remove(tmp)
		if errors.Is(err, fs.ErrExist) {
			return ErrAlreadyExists
		}
		return err
	}
	return os.Remove(tmp)
}

func (f *FS) GetBlob(_ context.Context, key string) (io.ReadCloser, error) {
	p, err := f.path(key)
	if err != nil {
		return nil, err
	}
	fh, err := os.Open(p)
	if errors.Is(err, fs.ErrNotExist) {
		return nil, ErrNotFound
	}
	return fh, err
}

func (f *FS) StatBlob(_ context.Context, key string) (BlobInfo, error) {
	p, err := f.path(key)
	if err != nil {
		return BlobInfo{}, err
	}
	st, err := os.Stat(p)
	if errors.Is(err, fs.ErrNotExist) {
		return BlobInfo{}, ErrNotFound
	}
	if err != nil {
		return BlobInfo{}, err
	}
	return BlobInfo{Key: key, Size: st.Size(), Modified: st.ModTime().UTC()}, nil
}

func (f *FS) DeleteBlob(_ context.Context, key string) error {
	p, err := f.path(key)
	if err != nil {
		return err
	}
	err = os.Remove(p)
	if errors.Is(err, fs.ErrNotExist) {
		return ErrNotFound
	}
	return err
}

func (f *FS) ListBlobs(_ context.Context, prefix string) ([]BlobInfo, error) {
	dir, err := f.path(strings.TrimSuffix(prefix, "/"))
	if err != nil {
		return nil, err
	}
	entries, err := os.ReadDir(dir)
	if errors.Is(err, fs.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	var out []BlobInfo
	for _, e := range entries {
		if e.IsDir() || strings.Contains(e.Name(), ".partial-") {
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		out = append(out, BlobInfo{Key: strings.TrimSuffix(prefix, "/") + "/" + e.Name(), Size: info.Size(), Modified: info.ModTime().UTC()})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Key < out[j].Key })
	return out, nil
}
