// Command shared-world-helper is the local companion service for the
// Satisfactory Shared World mod. The mod starts it automatically; it serves
// an authenticated API on 127.0.0.1 and talks to cloud storage.
package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"log/slog"
	"net"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/clock"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/config"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/ipc"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/lease"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/logx"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/store"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/syncer"
	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/world"
)

var version = "0.1.0-dev"

// Discovery is written to <data>/discovery.json so the mod can find and
// authenticate to the helper. Only the current OS user can read it.
type Discovery struct {
	SchemaVersion int       `json:"schemaVersion"`
	APIVersion    int       `json:"apiVersion"`
	Port          int       `json:"port"`
	Token         string    `json:"token"`
	PID           int       `json:"pid"`
	Version       string    `json:"version"`
	StartedAt     time.Time `json:"startedAt"`
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "shared-world-helper:", err)
		os.Exit(1)
	}
}

func run() error {
	defDir, err := config.DefaultDataDir()
	if err != nil {
		return err
	}
	dataDir := flag.String("data-dir", defDir, "helper data directory")
	verbose := flag.Bool("verbose", false, "debug logging")
	showVersion := flag.Bool("version", false, "print version and exit")
	flag.Parse()
	if *showVersion {
		fmt.Println(version)
		return nil
	}
	if err := os.MkdirAll(*dataDir, 0o700); err != nil {
		return err
	}

	level := slog.LevelInfo
	if *verbose {
		level = slog.LevelDebug
	}
	lf, err := logx.OpenLogFile(filepath.Join(*dataDir, "logs"), 10<<20)
	if err != nil {
		return err
	}
	defer lf.Close()
	log := logx.New(io.MultiWriter(lf, os.Stderr), level)

	release, err := singleInstance(*dataDir)
	if err != nil {
		return err
	}
	defer release()

	cfg, err := config.Load(filepath.Join(*dataDir, "config.json"))
	if err != nil {
		return err
	}
	log.Info("helper_starting", "version", version, "install", cfg.InstallID, "provider", cfg.Provider.Type, "worlds", len(cfg.Worlds))

	var prov store.Provider
	switch cfg.Provider.Type {
	case "filesystem":
		fsp, err := store.NewFS(cfg.Provider.Root)
		if err != nil {
			return err
		}
		prov = fsp
		log.Warn("filesystem_provider_limits", "detail", "the filesystem provider is only safe for one machine or a true network share; never point it at a Google Drive/OneDrive/Dropbox sync folder")
	}

	clk := clock.System{}
	lcfg := lease.DefaultConfig()
	lcfg.TTL, lcfg.SkewGrace = cfg.TTL(), cfg.SkewGrace()
	leases := lease.NewManager(prov, clk, lcfg, log)
	states, err := syncer.NewLocalStateStore(filepath.Join(*dataDir, "state"))
	if err != nil {
		return err
	}
	backups := syncer.NewBackupManager(filepath.Join(*dataDir, "backups"), cfg.KeepLocalBackups, clk.Now)
	sy := syncer.New(prov, leases, backups, states, clk, syncer.Config{
		StagingDir:    filepath.Join(*dataDir, "staging"),
		StableQuiet:   3 * time.Second,
		StableTimeout: 2 * time.Minute,
	}, log)
	wcfg := world.DefaultConfig()
	wcfg.HeartbeatInterval = cfg.Heartbeat()
	for _, w := range cfg.Worlds {
		wcfg.Worlds = append(wcfg.Worlds, world.WorldConfig{ID: w.ID, Name: w.Name})
	}
	mgr, err := world.NewManager(wcfg, leases, sy, clk, cfg.InstallID, log)
	if err != nil {
		return err
	}
	defer mgr.Shutdown()

	ln, err := ipc.Listen(cfg.IPCPort)
	if err != nil {
		return err
	}
	tb := make([]byte, 32)
	if _, err := rand.Read(tb); err != nil {
		return err
	}
	token := hex.EncodeToString(tb)
	port := ln.Addr().(*net.TCPAddr).Port
	discPath := filepath.Join(*dataDir, "discovery.json")
	if err := writeDiscovery(discPath, Discovery{
		SchemaVersion: 1, APIVersion: ipc.APIVersion, Port: port, Token: token,
		PID: os.Getpid(), Version: version, StartedAt: clk.Now(),
	}); err != nil {
		return err
	}
	defer os.Remove(discPath)

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	log.Info("helper_listening", "address", ln.Addr().String())
	err = ipc.NewServer(mgr, token, version, log).Serve(ctx, ln)
	log.Info("helper_stopped")
	return err
}

func writeDiscovery(p string, d Discovery) error {
	b, err := json.MarshalIndent(d, "", "  ")
	if err != nil {
		return err
	}
	tmp := p + ".tmp"
	if err := os.WriteFile(tmp, b, 0o600); err != nil {
		return err
	}
	return os.Rename(tmp, p)
}

// singleInstance takes <data>/helper.pid exclusively. A pid file left by a
// crashed helper is detected and replaced.
func singleInstance(dir string) (func(), error) {
	p := filepath.Join(dir, "helper.pid")
	for i := 0; i < 2; i++ {
		f, err := os.OpenFile(p, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o600)
		if err == nil {
			fmt.Fprint(f, os.Getpid())
			f.Close()
			return func() { os.Remove(p) }, nil
		}
		if !errors.Is(err, fs.ErrExist) {
			return nil, err
		}
		b, _ := os.ReadFile(p)
		if pid, perr := strconv.Atoi(strings.TrimSpace(string(b))); perr == nil && pid != os.Getpid() && world.ProcessAlive(pid) {
			return nil, fmt.Errorf("another helper is already running (pid %d)", pid)
		}
		_ = os.Remove(p) // stale
	}
	return nil, errors.New("could not acquire helper.pid")
}
