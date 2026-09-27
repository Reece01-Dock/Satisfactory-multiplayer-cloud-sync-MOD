package ipc

import (
	"bytes"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/clock"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/lease"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/savefile"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/store"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/syncer"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/world"
)

var quiet = slog.New(slog.NewTextHandler(io.Discard, nil))

const token = "test-token-0123456789"

type client struct {
	t    *testing.T
	base string
	tok  string
}

func (c *client) do(method, path string, body any, headers ...string) (int, map[string]any) {
	c.t.Helper()
	var r io.Reader
	if body != nil {
		b, _ := json.Marshal(body)
		r = bytes.NewReader(b)
	}
	req, _ := http.NewRequest(method, c.base+path, r)
	if c.tok != "" {
		req.Header.Set("Authorization", "Bearer "+c.tok)
	}
	for i := 0; i+1 < len(headers); i += 2 {
		if headers[i] == "Host" {
			req.Host = headers[i+1]
		} else {
			req.Header.Set(headers[i], headers[i+1])
		}
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		c.t.Fatal(err)
	}
	defer resp.Body.Close()
	var out map[string]any
	_ = json.NewDecoder(resp.Body).Decode(&out)
	return resp.StatusCode, out
}

func newHelper(t *testing.T, cloud *store.Mem, clk clock.Clock, install string) *client {
	t.Helper()
	data := t.TempDir()
	lm := lease.NewManager(cloud, clk, lease.DefaultConfig(), quiet)
	st, _ := syncer.NewLocalStateStore(filepath.Join(data, "state"))
	sy := syncer.New(cloud, lm, syncer.NewBackupManager(filepath.Join(data, "backups"), 5, clk.Now), st, clk,
		syncer.Config{StagingDir: filepath.Join(data, "staging"), StableQuiet: 10 * time.Millisecond, StableTimeout: 5 * time.Second}, quiet)
	cfg := world.DefaultConfig()
	cfg.Worlds = []world.WorldConfig{{ID: "our-factory", Name: "Our Factory"}}
	cfg.JoinPollInterval = 10 * time.Millisecond
	cfg.StatusCacheTTL = 0
	m, err := world.NewManager(cfg, lm, sy, clk, install, quiet)
	if err != nil {
		t.Fatal(err)
	}
	m.ProcessAlive = func(int) bool { return true }
	srv := httptest.NewServer(NewServer(m, token, "test", quiet).Handler())
	t.Cleanup(func() { srv.Close(); m.Shutdown() })
	return &client{t: t, base: srv.URL, tok: token}
}

func TestAuthAndBrowserProtection(t *testing.T) {
	c := newHelper(t, store.NewMem(), clock.System{}, "i1")
	if code, body := c.do("GET", "/v1/health", nil); code != 200 || body["ok"] != true {
		t.Fatalf("health %d %v", code, body)
	}
	anon := &client{t: t, base: c.base}
	if code, _ := anon.do("GET", "/v1/worlds", nil); code != 401 {
		t.Fatalf("no token: %d", code)
	}
	wrong := &client{t: t, base: c.base, tok: "nope"}
	if code, _ := wrong.do("POST", "/v1/worlds/our-factory/play", map[string]any{}); code != 401 {
		t.Fatalf("wrong token: %d", code)
	}
	if code, _ := c.do("GET", "/v1/worlds", nil, "Origin", "https://evil.example"); code != 403 {
		t.Fatalf("browser origin: %d", code)
	}
	if code, _ := c.do("GET", "/v1/worlds", nil, "Host", "evil.example:80"); code != 403 {
		t.Fatalf("rebinding host: %d", code)
	}
	if code, _ := c.do("GET", "/v1/worlds/..%2F..%2Fetc", nil); code != 400 && code != 404 {
		t.Fatalf("traversal id: %d", code)
	}
	if code, _ := c.do("POST", "/v1/worlds/our-factory/play", map[string]any{"unexpected": 1}); code != 400 {
		t.Fatalf("unknown field: %d", code)
	}
}

// The vertical slice over real HTTP: helper A answers HOST, helper B JOIN.
func TestHTTPPlayHostThenJoin(t *testing.T) {
	cloud := store.NewMem()
	clk := clock.System{}
	a := newHelper(t, cloud, clk, "install-a")
	b := newHelper(t, cloud, clk, "install-b")
	aDir, bDir := t.TempDir(), t.TempDir()
	os.WriteFile(filepath.Join(aDir, "Existing.sav"), savefile.BuildSynthetic("Existing", []byte("factory")), 0o600)

	player := func(id, name, dir string) map[string]any {
		return map[string]any{"playerId": id, "displayName": name, "platform": "steam", "saveDirectory": dir, "gamePid": 0}
	}
	create := player("pa", "Reece", aDir)
	create["worldId"], create["worldName"], create["importSaveName"] = "our-factory", "Our Factory", "Existing"
	if code, body := a.do("POST", "/v1/worlds", create); code != 201 {
		t.Fatalf("create %d %v", code, body)
	}

	if code, body := a.do("POST", "/v1/worlds/our-factory/play", player("pa", "Reece", aDir)); code != 200 {
		t.Fatalf("play A %d %v", code, body)
	}
	waitFor(t, a, "READY_TO_HOST")
	if code, body := a.do("POST", "/v1/worlds/our-factory/session/started", map[string]any{"join": map[string]any{"kind": "online-session-id", "value": "EOS-123", "backend": "EOS"}}); code != 200 || body["state"] != "HOSTING" {
		t.Fatalf("started %d %v", code, body)
	}
	b.do("POST", "/v1/worlds/our-factory/play", player("pb", "Vojta", bDir))
	v := waitFor(t, b, "JOIN_READY")
	if v["decision"] != "JOIN" || v["hostName"] != "Reece" || v["join"].(map[string]any)["value"] != "EOS-123" {
		t.Fatalf("B view %v", v)
	}
	_, list := b.do("GET", "/v1/worlds", nil)
	w0 := list["worlds"].([]any)[0].(map[string]any)
	if w0["status"] != "ONLINE" || w0["hostName"] != "Reece" || w0["revision"].(float64) != 1 {
		t.Fatalf("status %v", w0)
	}
	// The status response must never contain the helper token.
	raw, _ := json.Marshal(list)
	if strings.Contains(string(raw), token) {
		t.Fatal("token leaked in status")
	}
}

func waitFor(t *testing.T, c *client, state string) map[string]any {
	t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for {
		_, v := c.do("GET", "/v1/worlds/our-factory/session", nil)
		if v["state"] == state {
			return v
		}
		if time.Now().After(deadline) {
			t.Fatalf("state %v, want %s (%v)", v["state"], state, v)
		}
		time.Sleep(10 * time.Millisecond)
	}
}
