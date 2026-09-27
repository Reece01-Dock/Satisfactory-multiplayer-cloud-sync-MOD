package syncer

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/clock"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/lease"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/model"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/savefile"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/store"
)

const world = "our-factory"

var quiet = slog.New(slog.NewTextHandler(io.Discard, nil))

type env struct {
	mem    *store.Mem
	blobs  store.BlobStore
	meta   store.MetadataStore
	clock  *clock.Fake
	leases *lease.Manager
	sync   *Syncer
	dir    string // local save directory
	data   string // helper data directory
}

// hookBlobs lets a test act right after a blob upload completes.
type hookBlobs struct {
	store.BlobStore
	afterPut func()
}

func (h *hookBlobs) PutBlob(ctx context.Context, key string, r io.Reader, size int64) error {
	err := h.BlobStore.PutBlob(ctx, key, r, size)
	if err == nil && h.afterPut != nil {
		f := h.afterPut
		h.afterPut = nil
		f()
	}
	return err
}

// lostResponseMeta applies a write but reports a network error, once.
type lostResponseMeta struct {
	store.MetadataStore
	armed bool
}

func (l *lostResponseMeta) PutRecord(ctx context.Context, key string, data []byte, v store.Version) (store.Version, error) {
	nv, err := l.MetadataStore.PutRecord(ctx, key, data, v)
	if err == nil && l.armed {
		l.armed = false
		return store.NoVersion, errors.New("connection reset by peer")
	}
	return nv, err
}

func newEnv(t *testing.T) *env {
	t.Helper()
	e := &env{mem: store.NewMem(), clock: clock.NewFake(time.Date(2026, 9, 27, 12, 0, 0, 0, time.UTC)), dir: t.TempDir(), data: t.TempDir()}
	e.blobs, e.meta = e.mem, e.mem
	e.build(t)
	if _, err := e.leases.CreateWorld(context.Background(), world, "Our Factory"); err != nil {
		t.Fatal(err)
	}
	return e
}

func (e *env) build(t *testing.T) {
	e.leases = lease.NewManager(e.meta, e.clock, lease.Config{TTL: 90 * time.Second, SkewGrace: 30 * time.Second, MaxCASAttempts: 20}, quiet)
	st, err := NewLocalStateStore(filepath.Join(e.data, "state"))
	if err != nil {
		t.Fatal(err)
	}
	e.sync = New(e.blobs, e.leases, NewBackupManager(filepath.Join(e.data, "backups"), 5, e.clock.Now), st, e.clock,
		Config{StagingDir: filepath.Join(e.data, "staging"), StableQuiet: 20 * time.Millisecond, StableTimeout: 5 * time.Second}, quiet)
}

func id(n int) model.Identity {
	return model.Identity{PlayerID: fmt.Sprintf("p%d", n), DisplayName: fmt.Sprintf("Player%d", n), Platform: "steam", InstallID: fmt.Sprintf("i%d", n)}
}

func (e *env) acquire(t *testing.T, n int) *lease.Token {
	t.Helper()
	r, err := e.leases.Acquire(context.Background(), world, id(n), fmt.Sprintf("nonce-%d-%d", n, time.Now().UnixNano()))
	if err != nil || r.Outcome != lease.Acquired {
		t.Fatalf("acquire: %v %v", r, err)
	}
	return r.Token
}

func (e *env) writeSave(t *testing.T, name, content string) string {
	t.Helper()
	p := filepath.Join(e.dir, name+".sav")
	if err := os.WriteFile(p, savefile.BuildSynthetic("S", []byte(content)), 0o600); err != nil {
		t.Fatal(err)
	}
	return p
}

func (e *env) record(t *testing.T) *model.WorldRecord {
	t.Helper()
	r, _, err := e.leases.Load(context.Background(), world)
	if err != nil {
		t.Fatal(err)
	}
	return r
}

func backupsWith(t *testing.T, e *env, substr string) []LocalBackup {
	list, _ := e.sync.Backups().List(world)
	var out []LocalBackup
	for _, b := range list {
		if strings.Contains(b.Name, substr) {
			out = append(out, b)
		}
	}
	return out
}

func TestUploadThenDownloadRoundTrip(t *testing.T) {
	e := newEnv(t)
	ctx := context.Background()
	tok := e.acquire(t, 1)
	src := e.writeSave(t, "host", "revision one")
	res, err := e.sync.Upload(ctx, tok, src, "checkpoint")
	if err != nil {
		t.Fatal(err)
	}
	if res.Revision.Number != 1 || tok.BaseRevision != 1 {
		t.Fatalf("revision %d base %d", res.Revision.Number, tok.BaseRevision)
	}
	// Same content again: no new revision.
	if res2, err := e.sync.Upload(ctx, tok, src, "checkpoint"); err != nil || !res2.Unchanged {
		t.Fatalf("unchanged upload: %+v %v", res2, err)
	}
	target := filepath.Join(e.dir, "SharedWorld_our-factory.sav")
	os.WriteFile(target, []byte("old local junk that must be backed up"), 0o600)
	dl, err := e.sync.Download(ctx, e.record(t), target)
	if err != nil {
		t.Fatal(err)
	}
	a, _ := os.ReadFile(src)
	b, _ := os.ReadFile(target)
	if !bytes.Equal(a, b) {
		t.Fatal("downloaded save differs from uploaded")
	}
	if dl.BackupPath == "" {
		t.Fatal("previous local file was not backed up")
	}
	if old, _ := os.ReadFile(dl.BackupPath); string(old) != "old local junk that must be backed up" {
		t.Fatal("backup content wrong")
	}
}

// Race 4: the cloud revision changes while an upload is in flight (another
// player took over after this host's lease expired and committed).
func TestRace4_CloudChangedDuringUpload(t *testing.T) {
	e := newEnv(t)
	hb := &hookBlobs{BlobStore: e.mem}
	e.blobs = hb
	e.build(t)
	ctx := context.Background()

	a := e.acquire(t, 1)
	if _, err := e.sync.Upload(ctx, a, e.writeSave(t, "a1", "A rev1"), "checkpoint"); err != nil {
		t.Fatal(err)
	}
	var bRev uint64
	hb.afterPut = func() {
		// While A's blob is uploading, A's lease expires and B takes over
		// and commits a revision of its own.
		e.clock.Advance(10 * time.Minute)
		b := e.acquire(t, 2)
		r, err := e.sync.Upload(ctx, b, e.writeSave(t, "b", "B rev2"), "checkpoint")
		if err != nil {
			t.Errorf("B upload: %v", err)
			return
		}
		bRev = r.Revision.Number
	}
	aSrc := e.writeSave(t, "a2", "A rev2 attempt")
	aBefore, _ := os.ReadFile(aSrc)
	_, err := e.sync.Upload(ctx, a, aSrc, "checkpoint")
	var ce *ConflictError
	if !errors.As(err, &ce) || !errors.Is(err, lease.ErrFenced) {
		t.Fatalf("expected fenced conflict, got %v", err)
	}
	rec := e.record(t)
	if rec.HeadNumber() != bRev || rec.Head.Uploader.PlayerID != "p2" {
		t.Fatalf("head = %d by %s, want B's revision %d", rec.HeadNumber(), rec.Head.Uploader.PlayerID, bRev)
	}
	if aAfter, _ := os.ReadFile(aSrc); !bytes.Equal(aBefore, aAfter) {
		t.Fatal("local save modified")
	}
	if len(backupsWith(t, e, "_conflict-g1-")) != 1 {
		t.Fatal("A's save was not preserved as a conflict backup")
	}
	// A's orphaned blob was cleaned up; B's head blob is intact.
	blobs, _ := e.mem.ListBlobs(ctx, model.BlobPrefix(world))
	for _, bl := range blobs {
		if strings.Contains(bl.Key, "r00000002-g00000001") {
			t.Fatal("fenced upload left a blob behind")
		}
	}
}

// A client holding revision N may never publish over revision N+1.
func TestStaleClientUploadRefused(t *testing.T) {
	e := newEnv(t)
	ctx := context.Background()
	tok := e.acquire(t, 1)
	if _, err := e.sync.Upload(ctx, tok, e.writeSave(t, "one", "rev1"), "checkpoint"); err != nil {
		t.Fatal(err)
	}
	if _, err := e.sync.Upload(ctx, tok, e.writeSave(t, "two", "rev2"), "checkpoint"); err != nil {
		t.Fatal(err)
	}
	stale := *tok
	stale.BaseRevision = 1 // e.g. a process that missed the rev2 commit
	_, err := e.sync.Upload(ctx, &stale, e.writeSave(t, "old", "stale edit"), "checkpoint")
	var ce *ConflictError
	if !errors.As(err, &ce) || !errors.Is(err, lease.ErrStaleRevision) || ce.CloudHead != 2 || ce.LocalBase != 1 {
		t.Fatalf("expected stale refusal cloud=2 local=1, got %v", err)
	}
	if e.record(t).HeadNumber() != 2 {
		t.Fatal("head changed")
	}
}

// Race 5: corrupted cloud download. Local save must be byte-identical.
func TestRace5_CorruptDownloadLeavesLocalUntouched(t *testing.T) {
	e := newEnv(t)
	ctx := context.Background()
	tok := e.acquire(t, 1)
	if _, err := e.sync.Upload(ctx, tok, e.writeSave(t, "src", "good data"), "checkpoint"); err != nil {
		t.Fatal(err)
	}
	target := e.writeSave(t, "SharedWorld_our-factory", "my local copy")
	before, _ := os.ReadFile(target)
	for name, corrupt := range map[string]func(string, []byte) []byte{
		"bitflip":   func(_ string, b []byte) []byte { b[len(b)/2] ^= 1; return b },
		"truncated": func(_ string, b []byte) []byte { return b[:len(b)-100] },
		"extended":  func(_ string, b []byte) []byte { return append(b, 0) },
	} {
		e.mem.CorruptBlob = corrupt
		_, err := e.sync.Download(ctx, e.record(t), target)
		if !errors.Is(err, ErrCorruptDownload) {
			t.Fatalf("%s: expected ErrCorruptDownload, got %v", name, err)
		}
		if after, _ := os.ReadFile(target); !bytes.Equal(before, after) {
			t.Fatalf("%s: local save modified", name)
		}
	}
	entries, _ := os.ReadDir(e.dir)
	for _, en := range entries {
		if strings.Contains(en.Name(), ".swtmp-") {
			t.Fatalf("temp file left behind: %s", en.Name())
		}
	}
}

// Race 6: the connection drops halfway through an upload.
func TestRace6_InterruptedUploadKeepsAuthoritativeSave(t *testing.T) {
	e := newEnv(t)
	ctx := context.Background()
	tok := e.acquire(t, 1)
	if _, err := e.sync.Upload(ctx, tok, e.writeSave(t, "v1", "authoritative"), "checkpoint"); err != nil {
		t.Fatal(err)
	}
	headBefore := *e.record(t).Head
	e.mem.FailPutBlobAfter = 100
	if _, err := e.sync.Upload(ctx, tok, e.writeSave(t, "v2", strings.Repeat("new progress ", 5000)), "checkpoint"); err == nil {
		t.Fatal("interrupted upload reported success")
	}
	e.mem.FailPutBlobAfter = 0
	rec := e.record(t)
	if rec.Head.SHA256 != headBefore.SHA256 || rec.Head.Number != headBefore.Number {
		t.Fatal("head changed after interrupted upload")
	}
	// The authoritative save still downloads and verifies.
	if _, err := e.sync.Download(ctx, rec, filepath.Join(e.dir, "check.sav")); err != nil {
		t.Fatalf("authoritative save unusable: %v", err)
	}
	// The host still holds authority and can retry successfully.
	if r, err := e.sync.Upload(ctx, tok, filepath.Join(e.dir, "v2.sav"), "checkpoint"); err != nil || r.Revision.Number != 2 {
		t.Fatalf("retry: %v %v", r, err)
	}
}

// The commit reached the cloud but the response was lost. The helper must
// notice it succeeded instead of reporting failure or re-uploading.
func TestAmbiguousCommitIsResolved(t *testing.T) {
	e := newEnv(t)
	lr := &lostResponseMeta{MetadataStore: e.mem}
	e.meta = lr
	e.build(t)
	ctx := context.Background()
	tok := e.acquire(t, 1)
	lr.armed = true
	r, err := e.sync.Upload(ctx, tok, e.writeSave(t, "x", "data"), "checkpoint")
	if err != nil || r.Revision.Number != 1 || tok.BaseRevision != 1 {
		t.Fatalf("ambiguous commit not resolved: %v %v", r, err)
	}
}

// Game crash during save: a truncated file must never be uploaded.
func TestTruncatedLocalSaveNeverUploaded(t *testing.T) {
	e := newEnv(t)
	ctx := context.Background()
	tok := e.acquire(t, 1)
	full := savefile.BuildSynthetic("S", bytes.Repeat([]byte("x"), 300_000))
	p := filepath.Join(e.dir, "crash.sav")
	os.WriteFile(p, full[:len(full)*2/3], 0o600)
	if _, err := e.sync.Upload(ctx, tok, p, "checkpoint"); err == nil {
		t.Fatal("truncated save uploaded")
	}
	if e.record(t).Head != nil {
		t.Fatal("head created from truncated save")
	}
}

func TestInspectLocalDetectsUnsyncedChanges(t *testing.T) {
	e := newEnv(t)
	ctx := context.Background()
	tok := e.acquire(t, 1)
	p := e.writeSave(t, "SharedWorld_our-factory", "v1")
	if _, err := e.sync.Upload(ctx, tok, p, "checkpoint"); err != nil {
		t.Fatal(err)
	}
	if st, _, _, _ := e.sync.InspectLocal(world, p); st != LocalInSync {
		t.Fatalf("after upload: %s", st)
	}
	e.writeSave(t, "SharedWorld_our-factory", "v1 + unsynced progress")
	st, _, ls, _ := e.sync.InspectLocal(world, p)
	if st != LocalModified || ls.SyncedRevision != 1 {
		t.Fatalf("after local edit: %s rev %d", st, ls.SyncedRevision)
	}
}
