package lease

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"math/rand/v2"
	"sync"
	"testing"
	"time"

	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/clock"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/model"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/store"
)

const world = "our-factory"

var quiet = slog.New(slog.NewTextHandler(io.Discard, nil))

func player(n int) model.Identity {
	return model.Identity{PlayerID: fmt.Sprintf("player-%d", n), DisplayName: fmt.Sprintf("P%d", n), Platform: "steam", InstallID: fmt.Sprintf("install-%d", n)}
}

// stores runs a test against every MetadataStore implementation.
func stores(t *testing.T) map[string]store.MetadataStore {
	fsStore, err := store.NewFS(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	mem := store.NewMem()
	// Simulated network latency between read and conditional write, so
	// concurrent read-modify-write cycles genuinely interleave.
	mem.BeforePutRecord = func(string) error {
		time.Sleep(time.Duration(rand.IntN(2000)) * time.Microsecond)
		return nil
	}
	return map[string]store.MetadataStore{"mem": mem, "fs": fsStore}
}

func setup(t *testing.T, s store.MetadataStore) (*Manager, *clock.Fake) {
	t.Helper()
	c := clock.NewFake(time.Date(2026, 9, 27, 12, 0, 0, 0, time.UTC))
	m := NewManager(s, c, Config{TTL: 90 * time.Second, SkewGrace: 30 * time.Second, MaxCASAttempts: 200}, quiet)
	if _, err := m.CreateWorld(context.Background(), world, "Our Factory"); err != nil {
		t.Fatal(err)
	}
	return m, c
}

func fakeRev(tok *Token, n int) model.Revision {
	sha := fmt.Sprintf("%064x", n+1)
	return model.Revision{
		Number: tok.BaseRevision + 1, BlobKey: model.BlobKeyFor(world, tok.BaseRevision+1, tok.Generation, sha),
		SHA256: sha, Size: 1234, Uploader: tok.Holder, Generation: tok.Generation, BaseRevision: tok.BaseRevision, Reason: "test",
	}
}

// Race 1: many players press Play at the same instant. Exactly one host.
func TestRace1_SimultaneousAcquireYieldsExactlyOneHost(t *testing.T) {
	for name, s := range stores(t) {
		t.Run(name, func(t *testing.T) {
			m, _ := setup(t, s)
			const n = 16
			var wg sync.WaitGroup
			results := make([]*AcquireResult, n)
			errs := make([]error, n)
			start := make(chan struct{})
			for i := 0; i < n; i++ {
				wg.Add(1)
				go func(i int) {
					defer wg.Done()
					<-start
					results[i], errs[i] = m.Acquire(context.Background(), world, player(i), fmt.Sprintf("nonce-%d", i))
				}(i)
			}
			close(start)
			wg.Wait()
			hosts, joiners := 0, 0
			for i := 0; i < n; i++ {
				if errs[i] != nil {
					t.Fatalf("player %d: %v", i, errs[i])
				}
				switch results[i].Outcome {
				case Acquired:
					hosts++
				case HeldByOther:
					joiners++
				default:
					t.Fatalf("unexpected outcome %s", results[i].Outcome)
				}
			}
			if hosts != 1 || joiners != n-1 {
				t.Fatalf("hosts=%d joiners=%d, want 1 and %d", hosts, joiners, n-1)
			}
			rec, _, _ := m.Load(context.Background(), world)
			if rec.Generation != 1 {
				t.Fatalf("generation = %d, want 1 (only one acquisition may succeed)", rec.Generation)
			}
		})
	}
}

// Race 2: the host crashes (no heartbeats). The lease expires and another
// player can acquire with a higher generation.
func TestRace2_CrashedHostLeaseExpires(t *testing.T) {
	for name, s := range stores(t) {
		t.Run(name, func(t *testing.T) {
			m, c := setup(t, s)
			ctx := context.Background()
			a, _ := m.Acquire(ctx, world, player(1), "a")
			if a.Outcome != Acquired {
				t.Fatal(a.Outcome)
			}
			// Before TTL: B must join.
			c.Advance(60 * time.Second)
			if b, _ := m.Acquire(ctx, world, player(2), "b"); b.Outcome != HeldByOther {
				t.Fatalf("before expiry: %s", b.Outcome)
			}
			// After TTL but inside skew grace: still respected.
			c.Advance(40 * time.Second) // 100s > 90s TTL, < 120s
			if b, _ := m.Acquire(ctx, world, player(2), "b"); b.Outcome != HeldByOther {
				t.Fatalf("inside skew grace: %s", b.Outcome)
			}
			c.Advance(30 * time.Second)
			b, err := m.Acquire(ctx, world, player(2), "b")
			if err != nil || b.Outcome != Acquired {
				t.Fatalf("after expiry: %v %v", b, err)
			}
			if b.Token.Generation != a.Token.Generation+1 {
				t.Fatalf("generation %d, want %d", b.Token.Generation, a.Token.Generation+1)
			}
			if b.TookOverExpired == nil || b.TookOverExpired.Holder.PlayerID != "player-1" {
				t.Fatal("takeover of expired lease not reported")
			}
			if b.Record.LastSession == nil || b.Record.LastSession.Reason != "expired" {
				t.Fatal("last session not recorded as expired")
			}
		})
	}
}

// Race 3: the old host comes back after being replaced. Every write with
// the old fencing token is rejected.
func TestRace3_OldHostIsFenced(t *testing.T) {
	for name, s := range stores(t) {
		t.Run(name, func(t *testing.T) {
			m, c := setup(t, s)
			ctx := context.Background()
			a, _ := m.Acquire(ctx, world, player(1), "a")
			oldTok := *a.Token
			c.Advance(5 * time.Minute)
			b, _ := m.Acquire(ctx, world, player(2), "b")
			if b.Outcome != Acquired {
				t.Fatal(b.Outcome)
			}
			if _, err := m.Renew(ctx, &oldTok, LeaseUpdate{Phase: model.PhaseHosting}); !errors.Is(err, ErrFenced) {
				t.Fatalf("renew: %v", err)
			}
			if _, err := m.Commit(ctx, &oldTok, fakeRev(&oldTok, 1)); !errors.Is(err, ErrFenced) {
				t.Fatalf("commit: %v", err)
			}
			if err := m.Release(ctx, &oldTok); !errors.Is(err, ErrFenced) {
				t.Fatalf("release: %v", err)
			}
			rec, _, _ := m.Load(ctx, world)
			if rec.Lease == nil || rec.Lease.Holder.PlayerID != "player-2" || rec.HeadNumber() != 0 {
				t.Fatalf("new host's state was disturbed: %+v", rec.Lease)
			}
		})
	}
}

// A client with revision N must never overwrite N+1.
func TestCommitRequiresCurrentBaseRevision(t *testing.T) {
	m, _ := setup(t, store.NewMem())
	ctx := context.Background()
	a, _ := m.Acquire(ctx, world, player(1), "a")
	tok := a.Token
	if _, err := m.Commit(ctx, tok, fakeRev(tok, 1)); err != nil {
		t.Fatal(err)
	}
	stale := *tok
	stale.BaseRevision = 0
	r := fakeRev(&stale, 2)
	if _, err := m.Commit(ctx, &stale, r); !errors.Is(err, ErrStaleRevision) {
		t.Fatalf("stale commit: %v", err)
	}
	rec, _, _ := m.Load(ctx, world)
	if rec.HeadNumber() != 1 || rec.Head.SHA256 != fmt.Sprintf("%064x", 2) {
		t.Fatalf("head changed by stale commit: %+v", rec.Head)
	}
}

// A host that lost connectivity past its TTL but was not replaced may renew
// and continue (nobody else can have written).
func TestRenewAfterExpiryWithoutTakeover(t *testing.T) {
	m, c := setup(t, store.NewMem())
	ctx := context.Background()
	a, _ := m.Acquire(ctx, world, player(1), "a")
	c.Advance(10 * time.Minute)
	if _, err := m.Renew(ctx, a.Token, LeaseUpdate{}); err != nil {
		t.Fatalf("renew after unobserved expiry: %v", err)
	}
	if _, err := m.Commit(ctx, a.Token, fakeRev(a.Token, 1)); err != nil {
		t.Fatalf("commit: %v", err)
	}
}

// Same user launching twice (second game instance / second PC).
func TestSameUserSecondInstanceIsNotAHost(t *testing.T) {
	m, _ := setup(t, store.NewMem())
	ctx := context.Background()
	if a, _ := m.Acquire(ctx, world, player(1), "first"); a.Outcome != Acquired {
		t.Fatal(a.Outcome)
	}
	b, _ := m.Acquire(ctx, world, player(1), "second")
	if b.Outcome != HeldBySelfElsewhere || b.Token != nil {
		t.Fatalf("second instance: %s", b.Outcome)
	}
	// Retrying the same session is idempotent.
	again, _ := m.Acquire(ctx, world, player(1), "first")
	if again.Outcome != AlreadyHeld || again.Token == nil {
		t.Fatalf("idempotent retry: %s", again.Outcome)
	}
}

func TestReleaseMakesWorldAvailable(t *testing.T) {
	m, _ := setup(t, store.NewMem())
	ctx := context.Background()
	a, _ := m.Acquire(ctx, world, player(1), "a")
	if err := m.Release(ctx, a.Token); err != nil {
		t.Fatal(err)
	}
	b, _ := m.Acquire(ctx, world, player(2), "b")
	if b.Outcome != Acquired || b.Token.Generation != 2 {
		t.Fatalf("%s gen=%v", b.Outcome, b.Token)
	}
	if b.Record.LastSession.Reason != "released" {
		t.Fatal("release not recorded")
	}
}

// Tampered or foreign metadata is refused rather than trusted.
func TestInvalidRemoteRecordRejected(t *testing.T) {
	mem := store.NewMem()
	m, _ := setup(t, mem)
	for name, raw := range map[string]string{
		"newer schema":   `{"schemaVersion":99,"worldId":"our-factory","worldName":"x","generation":0}`,
		"wrong world":    `{"schemaVersion":1,"worldId":"other","worldName":"x","generation":0}`,
		"traversal blob": `{"schemaVersion":1,"worldId":"our-factory","worldName":"x","generation":1,"head":{"number":1,"blobKey":"worlds/our-factory/saves/../../x.sav","sha256":"` + fmt.Sprintf("%064x", 1) + `","size":5,"generation":1},"history":[]}`,
		"not json":       `{`,
	} {
		mem.SetRecordRaw(model.RecordKey(world), []byte(raw))
		if _, _, err := m.Load(context.Background(), world); err == nil {
			t.Fatalf("%s: accepted", name)
		}
		if _, err := m.Acquire(context.Background(), world, player(1), "n"); err == nil {
			t.Fatalf("%s: acquire proceeded on invalid record", name)
		}
	}
}
