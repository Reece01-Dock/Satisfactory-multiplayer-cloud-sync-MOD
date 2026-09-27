package world

import (
	"context"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/clock"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/lease"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/model"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/savefile"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/store"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/syncer"
)

const wid = "our-factory"

var quiet = slog.New(slog.NewTextHandler(io.Discard, nil))

// peer is one player's machine: its own helper, save dir and data dir,
// sharing the cloud store and clock with the others.
type peer struct {
	name     string
	mgr      *Manager
	saveDir  string
	gameUp   atomic.Bool
	playerID string
}

type cluster struct {
	cloud *store.Mem
	clock *clock.Fake
}

func newCluster(t *testing.T) *cluster {
	c := &cluster{cloud: store.NewMem(), clock: clock.NewFake(time.Date(2026, 9, 27, 12, 0, 0, 0, time.UTC))}
	c.cloud.BeforePutRecord = func(string) error { time.Sleep(200 * time.Microsecond); return nil }
	return c
}

func (c *cluster) peer(t *testing.T, name string) *peer {
	t.Helper()
	data := t.TempDir()
	lm := lease.NewManager(c.cloud, c.clock, lease.Config{TTL: 90 * time.Second, SkewGrace: 30 * time.Second, MaxCASAttempts: 100}, quiet)
	st, _ := syncer.NewLocalStateStore(filepath.Join(data, "state"))
	sy := syncer.New(c.cloud, lm, syncer.NewBackupManager(filepath.Join(data, "backups"), 10, c.clock.Now), st, c.clock,
		syncer.Config{StagingDir: filepath.Join(data, "staging"), StableQuiet: 20 * time.Millisecond, StableTimeout: 5 * time.Second}, quiet)
	cfg := Config{
		Worlds:              []WorldConfig{{ID: wid, Name: "Our Factory"}},
		HeartbeatInterval:   30 * time.Millisecond,
		GameLivenessTimeout: time.Hour,
		JoinWaitTimeout:     10 * time.Second,
		JoinPollInterval:    10 * time.Millisecond,
		StatusCacheTTL:      0,
	}
	m, err := NewManager(cfg, lm, sy, c.clock, "install-"+name, quiet)
	if err != nil {
		t.Fatal(err)
	}
	p := &peer{name: name, mgr: m, saveDir: t.TempDir(), playerID: "player-" + name}
	p.gameUp.Store(true)
	m.ProcessAlive = func(int) bool { return p.gameUp.Load() }
	t.Cleanup(m.Shutdown)
	return p
}

func (p *peer) req() PlayRequest {
	return PlayRequest{PlayerID: p.playerID, DisplayName: p.name, Platform: "steam", SaveDirectory: p.saveDir, GamePID: 4242}
}

// seedWorld creates the world with revision 1 uploaded by p.
func (c *cluster) seedWorld(t *testing.T, p *peer, content string) {
	t.Helper()
	src := filepath.Join(p.saveDir, "MyOldSave.sav")
	os.WriteFile(src, savefile.BuildSynthetic("MyOldSave", []byte(content)), 0o600)
	if _, err := p.mgr.CreateWorld(context.Background(), CreateRequest{PlayRequest: p.req(), WorldID: wid, WorldName: "Our Factory", ImportSaveName: "MyOldSave"}); err != nil {
		t.Fatal(err)
	}
}

func waitState(t *testing.T, p *peer, want ...State) SessionView {
	t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for {
		v, _ := p.mgr.Session(wid)
		for _, w := range want {
			if v.State == w {
				return v
			}
		}
		if time.Now().After(deadline) {
			t.Fatalf("%s: state %s (%s), want %v; error=%+v", p.name, v.State, v.Message, want, v.Error)
		}
		time.Sleep(5 * time.Millisecond)
	}
}

// The first vertical slice: Play -> HOST for the first player, Play -> JOIN
// (with the host's join data) for the second.
func TestPlayDecidesHostThenJoin(t *testing.T) {
	c := newCluster(t)
	a, b := c.peer(t, "Reece"), c.peer(t, "Vojta")
	c.seedWorld(t, a, "the factory")

	if _, err := a.mgr.Play(wid, a.req()); err != nil {
		t.Fatal(err)
	}
	va := waitState(t, a, StateReadyToHost)
	if va.Decision != DecisionHost || va.Revision != 1 || va.SaveName != "SharedWorld_our-factory" {
		t.Fatalf("host view %+v", va)
	}
	if _, err := savefile.CheckFile(va.SavePath); err != nil {
		t.Fatalf("placed save invalid: %v", err)
	}
	join := model.JoinInfo{Kind: model.JoinOnlineSessionID, Value: "session-abc", Backend: "EOS"}
	if v, err := a.mgr.SessionStarted(context.Background(), wid, &join); err != nil || v.State != StateHosting {
		t.Fatalf("started: %v %+v", err, v)
	}
	a.mgr.Keepalive(wid, []model.Player{{DisplayName: "Reece"}})

	b.mgr.Play(wid, b.req())
	vb := waitState(t, b, StateJoinReady)
	if vb.Decision != DecisionJoin || vb.HostName != "Reece" || vb.Join == nil || vb.Join.Value != "session-abc" {
		t.Fatalf("join view %+v", vb)
	}
	// B never downloaded anything.
	if _, err := os.Stat(filepath.Join(b.saveDir, "SharedWorld_our-factory.sav")); !os.IsNotExist(err) {
		t.Fatal("joining player downloaded the save")
	}
	st, _ := b.mgr.Status(context.Background(), wid)
	if st.Status != RemoteOnline || st.HostName != "Reece" || st.Revision != 1 {
		t.Fatalf("status %+v", st)
	}
}

// Race 1 end-to-end: two players press Play at the same instant.
func TestSimultaneousPlayOneHostOneJoin(t *testing.T) {
	for i := 0; i < 5; i++ {
		t.Run(fmt.Sprint(i), func(t *testing.T) {
			c := newCluster(t)
			a, b := c.peer(t, "A"), c.peer(t, "B")
			c.seedWorld(t, a, "x")
			var wg sync.WaitGroup
			for _, p := range []*peer{a, b} {
				wg.Add(1)
				go func(p *peer) { defer wg.Done(); p.mgr.Play(wid, p.req()) }(p)
			}
			wg.Wait()
			// The loser waits for the winner to publish join data.
			var host, other *peer
			deadline := time.Now().Add(10 * time.Second)
			for host == nil {
				for _, p := range []*peer{a, b} {
					if v, _ := p.mgr.Session(wid); v.State == StateReadyToHost {
						host = p
					}
				}
				if time.Now().After(deadline) {
					t.Fatal("nobody became host")
				}
				time.Sleep(5 * time.Millisecond)
			}
			other = a
			if host == a {
				other = b
			}
			if v, _ := other.mgr.Session(wid); v.Decision == DecisionHost {
				t.Fatal("two hosts")
			}
			host.mgr.SessionStarted(context.Background(), wid, &model.JoinInfo{Kind: model.JoinOnlineSessionID, Value: "s1"})
			vo := waitState(t, other, StateJoinReady)
			if vo.Decision != DecisionJoin || vo.Join.Value != "s1" {
				t.Fatalf("other %+v", vo)
			}
		})
	}
}

// Host stops cleanly: final save uploaded as a new revision, lease released,
// next player hosts from that revision.
func TestHostStopUploadsAndReleases(t *testing.T) {
	c := newCluster(t)
	a, b := c.peer(t, "A"), c.peer(t, "B")
	c.seedWorld(t, a, "rev1")
	a.mgr.Play(wid, a.req())
	va := waitState(t, a, StateReadyToHost)
	a.mgr.SessionStarted(context.Background(), wid, &model.JoinInfo{Kind: model.JoinAddress, Value: "127.0.0.1:7777"})
	os.WriteFile(va.SavePath, savefile.BuildSynthetic("S", []byte("rev2 progress")), 0o600)
	if _, err := a.mgr.Saved(wid, SavedRequest{SaveName: va.SaveName, Final: true}); err != nil {
		t.Fatal(err)
	}
	waitState(t, a, StateIdle)
	st, _ := a.mgr.Status(context.Background(), wid)
	if st.Status != RemoteAvailable || st.Revision != 2 {
		t.Fatalf("after stop: %+v", st)
	}
	b.mgr.Play(wid, b.req())
	vb := waitState(t, b, StateReadyToHost)
	got, _ := os.ReadFile(vb.SavePath)
	want, _ := os.ReadFile(va.SavePath)
	if string(got) != string(want) || vb.Revision != 2 {
		t.Fatal("B did not receive A's final revision")
	}
}

// Race 2 end-to-end: the host's game crashes. The heartbeat stops, the lease
// expires and another player recovers the world from the last revision.
// When the crashed host comes back, its unsynced save is preserved, never
// uploaded over the newer revision.
func TestHostCrashRecoveryAndStaleReturn(t *testing.T) {
	c := newCluster(t)
	a, b := c.peer(t, "A"), c.peer(t, "B")
	c.seedWorld(t, a, "rev1")
	a.mgr.Play(wid, a.req())
	va := waitState(t, a, StateReadyToHost)
	a.mgr.SessionStarted(context.Background(), wid, &model.JoinInfo{Kind: model.JoinAddress, Value: "h:1"})
	// A plays for a while; the game writes progress locally, then crashes.
	os.WriteFile(va.SavePath, savefile.BuildSynthetic("S", []byte("A unsynced progress")), 0o600)
	a.gameUp.Store(false)
	v := waitState(t, a, StateError)
	if v.Error.Code != "GAME_EXITED" {
		t.Fatalf("error %+v", v.Error)
	}
	// Lease still live for others until TTL + grace.
	if st, _ := b.mgr.Status(context.Background(), wid); st.Status != RemoteOnline {
		t.Fatalf("status right after crash: %s", st.Status)
	}
	c.clock.Advance(3 * time.Minute)
	if st, _ := b.mgr.Status(context.Background(), wid); st.Status != RemoteRecoverable {
		t.Fatalf("status after expiry: %s", st.Status)
	}
	b.mgr.Play(wid, b.req())
	vb := waitState(t, b, StateReadyToHost)
	if vb.Decision != DecisionHost || vb.Generation != 3 { // 1 = import, 2 = A, 3 = B
		t.Fatalf("B view %+v", vb)
	}
	b.mgr.SessionStarted(context.Background(), wid, &model.JoinInfo{Kind: model.JoinAddress, Value: "h:2"})
	os.WriteFile(vb.SavePath, savefile.BuildSynthetic("S", []byte("B progress")), 0o600)
	b.mgr.Saved(wid, SavedRequest{SaveName: vb.SaveName, Final: true})
	waitState(t, b, StateIdle)

	// A returns. Its local save (based on rev1) must not overwrite rev2.
	a.gameUp.Store(true)
	a.mgr.Ack(wid)
	a.mgr.Play(wid, a.req())
	va2 := waitState(t, a, StateReadyToHost)
	if va2.Revision != 2 {
		t.Fatalf("A hosts revision %d, want 2", va2.Revision)
	}
	got, _ := os.ReadFile(va2.SavePath)
	if want, _ := os.ReadFile(vb.SavePath); string(got) != string(want) {
		t.Fatal("A's local save is not B's revision 2")
	}
	list, _ := a.mgr.sync.Backups().List(wid)
	found := false
	for _, bk := range list {
		if b, _ := os.ReadFile(bk.Path); string(b) == string(savefile.BuildSynthetic("S", []byte("A unsynced progress"))) {
			found = true
		}
	}
	if !found {
		t.Fatal("A's unsynced progress was not preserved as a backup")
	}
}

// If the host crashed and nobody else hosted in the meantime, the host's
// unsynced local progress is recovered on its next Play.
func TestUnsyncedProgressRecoveredWhenNobodyElseHosted(t *testing.T) {
	c := newCluster(t)
	a := c.peer(t, "A")
	c.seedWorld(t, a, "rev1")
	a.mgr.Play(wid, a.req())
	va := waitState(t, a, StateReadyToHost)
	a.mgr.SessionStarted(context.Background(), wid, &model.JoinInfo{Kind: model.JoinAddress, Value: "h:1"})
	progress := savefile.BuildSynthetic("S", []byte("progress made before the crash"))
	os.WriteFile(va.SavePath, progress, 0o600)
	a.gameUp.Store(false)
	waitState(t, a, StateError)
	c.clock.Advance(5 * time.Minute)
	a.gameUp.Store(true)
	a.mgr.Ack(wid)
	a.mgr.Play(wid, a.req())
	v := waitState(t, a, StateReadyToHost)
	if v.Revision != 2 {
		t.Fatalf("revision %d, want 2 (recovered)", v.Revision)
	}
	if got, _ := os.ReadFile(v.SavePath); string(got) != string(progress) {
		t.Fatal("recovered progress not kept locally")
	}
	st, _ := a.mgr.Status(context.Background(), wid)
	if st.Revision != 2 {
		t.Fatalf("cloud revision %d", st.Revision)
	}
}

// Race 3 end-to-end: the old host's helper keeps running after it was
// replaced; its heartbeat detects the fencing and it stops.
func TestReplacedHostDetectsLeaseLoss(t *testing.T) {
	c := newCluster(t)
	a, b := c.peer(t, "A"), c.peer(t, "B")
	c.seedWorld(t, a, "rev1")
	a.mgr.Play(wid, a.req())
	waitState(t, a, StateReadyToHost)
	a.mgr.SessionStarted(context.Background(), wid, &model.JoinInfo{Kind: model.JoinAddress, Value: "h:1"})
	// Simulate A's network being down for a long time: stop its heartbeats.
	a.mgr.stopHeartbeat(a.mgr.sessions[wid])
	c.clock.Advance(5 * time.Minute)
	b.mgr.Play(wid, b.req())
	waitState(t, b, StateReadyToHost)
	// A's network comes back.
	a.mgr.startHeartbeat(a.mgr.sessions[wid])
	v := waitState(t, a, StateLeaseLost)
	if v.Error == nil || v.Error.Code != "LEASE_LOST" {
		t.Fatalf("%+v", v.Error)
	}
	if _, err := a.mgr.Saved(wid, SavedRequest{SaveName: "SharedWorld_our-factory", Final: true}); err == nil {
		t.Fatal("fenced host accepted a save upload")
	}
}

func TestSameUserSecondInstanceBlocked(t *testing.T) {
	c := newCluster(t)
	a := c.peer(t, "A")
	c.seedWorld(t, a, "rev1")
	a.mgr.Play(wid, a.req())
	waitState(t, a, StateReadyToHost)
	a2 := c.peer(t, "A-second-pc")
	a2.playerID = a.playerID // same account, different machine
	a2.mgr.Play(wid, a2.req())
	v := waitState(t, a2, StateError)
	if v.Error.Code != "ALREADY_HOSTING_ELSEWHERE" {
		t.Fatalf("%+v", v.Error)
	}
}

func TestCorruptCloudSaveReleasesLease(t *testing.T) {
	c := newCluster(t)
	a, b := c.peer(t, "A"), c.peer(t, "B")
	c.seedWorld(t, a, "rev1")
	c.cloud.CorruptBlob = func(_ string, d []byte) []byte { d[100] ^= 0xFF; return d }
	b.mgr.Play(wid, b.req())
	v := waitState(t, b, StateError)
	if v.Error.Code != "CORRUPT_DOWNLOAD" || !v.Error.LocalSaveUnchanged {
		t.Fatalf("%+v", v.Error)
	}
	st, _ := a.mgr.Status(context.Background(), wid)
	if st.Status != RemoteAvailable {
		t.Fatalf("lease not released after failed download: %s", st.Status)
	}
}

func TestIllegalTransitionsRefused(t *testing.T) {
	c := newCluster(t)
	a := c.peer(t, "A")
	if _, err := a.mgr.Saved(wid, SavedRequest{SaveName: "SharedWorld_our-factory"}); err == nil {
		t.Fatal("save accepted while idle")
	}
	if _, err := a.mgr.SessionStarted(context.Background(), wid, &model.JoinInfo{Kind: model.JoinAddress, Value: "x"}); err == nil {
		t.Fatal("session start accepted while idle")
	}
	if _, err := a.mgr.Saved(wid, SavedRequest{SaveName: "../../evil"}); err == nil {
		t.Fatal("foreign save name accepted")
	}
}

// A host whose game could not produce join data still counts as hosting;
// clients get a JOIN decision without join info (use the friends list)
// instead of waiting or, worse, starting a second copy.
func TestHostWithoutJoinInfoStillMeansJoin(t *testing.T) {
	c := newCluster(t)
	a, b := c.peer(t, "A"), c.peer(t, "B")
	c.seedWorld(t, a, "rev1")
	a.mgr.Play(wid, a.req())
	waitState(t, a, StateReadyToHost)
	if _, err := a.mgr.SessionStarted(context.Background(), wid, nil); err != nil {
		t.Fatal(err)
	}
	b.mgr.Play(wid, b.req())
	v := waitState(t, b, StateJoinReady)
	if v.Decision != DecisionJoin || v.Join != nil || v.HostName != "A" {
		t.Fatalf("%+v", v)
	}
}

// The host's own session is somehow behind the cloud head (e.g. a second
// process of the same session committed). The upload must be refused with
// NEWER_SAVE_EXISTS, the local save preserved, and nothing may deadlock.
func TestNewerCloudRevisionRefusesUpload(t *testing.T) {
	c := newCluster(t)
	a := c.peer(t, "A")
	c.seedWorld(t, a, "rev1")
	a.mgr.Play(wid, a.req())
	va := waitState(t, a, StateReadyToHost)
	a.mgr.SessionStarted(context.Background(), wid, &model.JoinInfo{Kind: model.JoinAddress, Value: "h:1"})

	// Commit revision 2 behind the session's back with a copy of its token.
	s := a.mgr.sessions[wid]
	s.mu.Lock()
	tok := *s.token
	s.mu.Unlock()
	other := filepath.Join(t.TempDir(), "other.sav")
	os.WriteFile(other, savefile.BuildSynthetic("S", []byte("committed elsewhere")), 0o600)
	if _, err := a.mgr.sync.Upload(context.Background(), &tok, other, "checkpoint"); err != nil {
		t.Fatal(err)
	}

	os.WriteFile(va.SavePath, savefile.BuildSynthetic("S", []byte("local progress on rev1")), 0o600)
	if _, err := a.mgr.Saved(wid, SavedRequest{SaveName: va.SaveName}); err != nil {
		t.Fatal(err)
	}
	v := waitState(t, a, StateError)
	if v.Error.Code != "NEWER_SAVE_EXISTS" || v.Error.CloudRevision != 2 || v.Error.LocalRevision != 1 || v.Error.BackupPath == "" {
		t.Fatalf("%+v", v.Error)
	}
	if got, _ := os.ReadFile(v.Error.BackupPath); string(got) != string(savefile.BuildSynthetic("S", []byte("local progress on rev1"))) {
		t.Fatal("backup does not hold the refused save")
	}
	if st, _ := a.mgr.Status(context.Background(), wid); st.Revision != 2 {
		t.Fatalf("cloud revision %d", st.Revision)
	}
}
