package store

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"sort"
	"strings"
	"sync"
)

// Mem is an in-memory Provider used by tests. It supports fault injection so
// tests can simulate lost races, corrupt downloads and dropped connections.
type Mem struct {
	mu      sync.Mutex
	records map[string]memRecord
	blobs   map[string][]byte
	seq     uint64

	// BeforePutRecord, if set, runs before every CAS write (outside the lock)
	// and may return an error to simulate a failing API call.
	BeforePutRecord func(key string) error
	// CorruptBlob, if set, may alter blob bytes returned by GetBlob.
	CorruptBlob func(key string, data []byte) []byte
	// FailPutBlobAfter, if > 0, makes PutBlob read that many bytes then fail
	// as if the connection dropped mid-upload.
	FailPutBlobAfter int64
}

type memRecord struct {
	data    []byte
	version Version
}

func NewMem() *Mem {
	return &Mem{records: map[string]memRecord{}, blobs: map[string][]byte{}}
}

func (m *Mem) Name() string { return "memory" }

func (m *Mem) GetRecord(_ context.Context, key string) ([]byte, Version, error) {
	if err := ValidateKey(key); err != nil {
		return nil, NoVersion, err
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	r, ok := m.records[key]
	if !ok {
		return nil, NoVersion, ErrNotFound
	}
	return bytes.Clone(r.data), r.version, nil
}

func (m *Mem) PutRecord(_ context.Context, key string, data []byte, expected Version) (Version, error) {
	if err := ValidateKey(key); err != nil {
		return NoVersion, err
	}
	if m.BeforePutRecord != nil {
		if err := m.BeforePutRecord(key); err != nil {
			return NoVersion, err
		}
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	cur, ok := m.records[key]
	if expected == NoVersion && ok {
		return NoVersion, ErrPreconditionFailed
	}
	if expected != NoVersion && (!ok || cur.version != expected) {
		return NoVersion, ErrPreconditionFailed
	}
	v := m.nextVersion()
	m.records[key] = memRecord{data: bytes.Clone(data), version: v}
	return v, nil
}

// SetRecordRaw overwrites a record bypassing CAS (tests only: simulates
// another writer or tampering).
func (m *Mem) SetRecordRaw(key string, data []byte) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.records[key] = memRecord{data: bytes.Clone(data), version: m.nextVersion()}
}

// nextVersion returns a fresh version token; callers hold m.mu. Every write
// gets a new version even if the content is identical, like a real ETag.
func (m *Mem) nextVersion() Version {
	m.seq++
	return Version(fmt.Sprintf("v%d", m.seq))
}

var errConnDropped = errors.New("simulated connection drop")

func (m *Mem) PutBlob(_ context.Context, key string, r io.Reader, size int64) error {
	if err := ValidateKey(key); err != nil {
		return err
	}
	var buf bytes.Buffer
	if m.FailPutBlobAfter > 0 {
		_, _ = io.CopyN(&buf, r, m.FailPutBlobAfter)
		return errConnDropped // partial data is discarded, like an aborted multipart upload
	}
	if _, err := io.Copy(&buf, r); err != nil {
		return err
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	if _, ok := m.blobs[key]; ok {
		return ErrAlreadyExists
	}
	m.blobs[key] = buf.Bytes()
	return nil
}

func (m *Mem) GetBlob(_ context.Context, key string) (io.ReadCloser, error) {
	if err := ValidateKey(key); err != nil {
		return nil, err
	}
	m.mu.Lock()
	b, ok := m.blobs[key]
	m.mu.Unlock()
	if !ok {
		return nil, ErrNotFound
	}
	b = bytes.Clone(b)
	if m.CorruptBlob != nil {
		b = m.CorruptBlob(key, b)
	}
	return io.NopCloser(bytes.NewReader(b)), nil
}

func (m *Mem) StatBlob(_ context.Context, key string) (BlobInfo, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	b, ok := m.blobs[key]
	if !ok {
		return BlobInfo{}, ErrNotFound
	}
	return BlobInfo{Key: key, Size: int64(len(b))}, nil
}

func (m *Mem) DeleteBlob(_ context.Context, key string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if _, ok := m.blobs[key]; !ok {
		return ErrNotFound
	}
	delete(m.blobs, key)
	return nil
}

func (m *Mem) ListBlobs(_ context.Context, prefix string) ([]BlobInfo, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	var out []BlobInfo
	for k, b := range m.blobs {
		if strings.HasPrefix(k, prefix) {
			out = append(out, BlobInfo{Key: k, Size: int64(len(b))})
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Key < out[j].Key })
	return out, nil
}
