// Package store abstracts cloud storage. It is split in two because the two
// halves need different guarantees:
//
//   - MetadataStore holds one small record per world and MUST provide an
//     atomic compare-and-swap. Every correctness property of the system
//     (single host, fencing, "revision N never overwrites N+1") reduces to
//     this one primitive. A backend that cannot provide it must not be used
//     as a MetadataStore.
//   - BlobStore holds save files. Blobs are immutable and written under
//     unique keys with create-only semantics, so a blob write can never
//     damage an existing save. A blob only becomes "the current save" when
//     the metadata record is CAS-updated to reference it.
package store

import (
	"context"
	"errors"
	"fmt"
	"io"
	"regexp"
	"strings"
	"time"
)

var (
	// ErrNotFound is returned when a key does not exist.
	ErrNotFound = errors.New("not found")
	// ErrPreconditionFailed is returned when a CAS write lost the race.
	ErrPreconditionFailed = errors.New("precondition failed: object changed concurrently")
	// ErrAlreadyExists is returned when a create-only blob write hits an existing key.
	ErrAlreadyExists = errors.New("already exists")
)

// Version is an opaque token identifying one exact version of a record
// (an ETag, a content hash, a Dropbox rev, ...).
type Version string

// NoVersion as the expected version means "only succeed if the key does not exist".
const NoVersion Version = ""

// MetadataStore stores small records with compare-and-swap.
type MetadataStore interface {
	// GetRecord returns the record bytes and version, or ErrNotFound.
	GetRecord(ctx context.Context, key string) ([]byte, Version, error)
	// PutRecord atomically replaces the record only if its current version
	// equals expected (or, with NoVersion, only if it does not exist).
	// Returns ErrPreconditionFailed otherwise.
	PutRecord(ctx context.Context, key string, data []byte, expected Version) (Version, error)
}

// BlobInfo describes a stored blob.
type BlobInfo struct {
	Key      string
	Size     int64
	Modified time.Time
}

// BlobStore stores immutable save files.
type BlobStore interface {
	// PutBlob writes a new blob. It must be create-only (ErrAlreadyExists if
	// the key exists) and must never leave a partially written blob visible
	// under key.
	PutBlob(ctx context.Context, key string, r io.Reader, size int64) error
	GetBlob(ctx context.Context, key string) (io.ReadCloser, error)
	StatBlob(ctx context.Context, key string) (BlobInfo, error)
	DeleteBlob(ctx context.Context, key string) error
	ListBlobs(ctx context.Context, prefix string) ([]BlobInfo, error)
}

// Provider is a complete storage backend.
type Provider interface {
	MetadataStore
	BlobStore
	Name() string
}

var keyPattern = regexp.MustCompile(`^[a-z0-9][a-z0-9._/-]*$`)

// ValidateKey rejects keys that could escape a provider's root.
func ValidateKey(key string) error {
	if len(key) > 512 || !keyPattern.MatchString(key) {
		return fmt.Errorf("invalid storage key %q", key)
	}
	for _, part := range strings.Split(key, "/") {
		if part == "" || part == "." || part == ".." {
			return fmt.Errorf("invalid storage key %q", key)
		}
	}
	return nil
}
