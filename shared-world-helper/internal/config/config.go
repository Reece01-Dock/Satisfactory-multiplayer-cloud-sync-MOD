// Package config loads the helper configuration and resolves its data
// directory. Credentials never live in this file's world entries and are
// never sent to the mod.
package config

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"time"

	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/model"
)

const SchemaVersion = 1

// DirName is the per-user data directory name. On Windows it lives in
// %LOCALAPPDATA%, which is what the mod resolves via
// FPlatformProcess::UserSettingsDir().
const DirName = "SatisfactorySharedWorld"

type ProviderConfig struct {
	// Type selects the storage backend. Implemented: "filesystem".
	Type string `json:"type"`
	// Root is the directory for the filesystem provider.
	Root string `json:"root,omitempty"`
}

type LeaseConfig struct {
	TTLSeconds       int `json:"ttlSeconds"`
	SkewGraceSeconds int `json:"skewGraceSeconds"`
	HeartbeatSeconds int `json:"heartbeatSeconds"`
}

type World struct {
	ID   string `json:"id"`
	Name string `json:"name"`
}

type Config struct {
	SchemaVersion    int            `json:"schemaVersion"`
	InstallID        string         `json:"installId"`
	Provider         ProviderConfig `json:"provider"`
	Lease            LeaseConfig    `json:"lease"`
	Worlds           []World        `json:"worlds"`
	KeepLocalBackups int            `json:"keepLocalBackups"`
	IPCPort          int            `json:"ipcPort"`
}

func (c *Config) applyDefaults() {
	if c.SchemaVersion == 0 {
		c.SchemaVersion = SchemaVersion
	}
	if c.Lease.TTLSeconds == 0 {
		c.Lease.TTLSeconds = 90
	}
	if c.Lease.SkewGraceSeconds == 0 {
		c.Lease.SkewGraceSeconds = 30
	}
	if c.Lease.HeartbeatSeconds == 0 {
		c.Lease.HeartbeatSeconds = 20
	}
	if c.KeepLocalBackups == 0 {
		c.KeepLocalBackups = 20
	}
}

// Validate rejects unsafe or inconsistent settings.
func (c *Config) Validate() error {
	if c.SchemaVersion != SchemaVersion {
		return fmt.Errorf("unsupported config schemaVersion %d", c.SchemaVersion)
	}
	if c.Lease.TTLSeconds < 30 {
		return errors.New("lease.ttlSeconds must be at least 30")
	}
	if c.Lease.HeartbeatSeconds*3 > c.Lease.TTLSeconds {
		return errors.New("lease.heartbeatSeconds must be at most a third of ttlSeconds so a few missed heartbeats do not lose the lease")
	}
	if c.Lease.SkewGraceSeconds < 0 {
		return errors.New("lease.skewGraceSeconds must not be negative")
	}
	switch c.Provider.Type {
	case "filesystem":
		if c.Provider.Root == "" || !filepath.IsAbs(c.Provider.Root) {
			return errors.New("provider.root must be an absolute path")
		}
	case "":
		return errors.New("provider.type is required")
	default:
		return fmt.Errorf("provider %q is not implemented yet (available: filesystem)", c.Provider.Type)
	}
	seen := map[string]bool{}
	for _, w := range c.Worlds {
		if err := model.ValidateWorldID(w.ID); err != nil {
			return err
		}
		if seen[w.ID] {
			return fmt.Errorf("duplicate world id %q", w.ID)
		}
		seen[w.ID] = true
		if w.Name == "" || len(w.Name) > 128 {
			return fmt.Errorf("world %q needs a name (max 128 chars)", w.ID)
		}
	}
	if c.IPCPort < 0 || c.IPCPort > 65535 {
		return errors.New("ipcPort out of range")
	}
	return nil
}

func (c *Config) TTL() time.Duration { return time.Duration(c.Lease.TTLSeconds) * time.Second }
func (c *Config) SkewGrace() time.Duration {
	return time.Duration(c.Lease.SkewGraceSeconds) * time.Second
}
func (c *Config) Heartbeat() time.Duration {
	return time.Duration(c.Lease.HeartbeatSeconds) * time.Second
}

// DefaultDataDir returns the per-user data directory.
func DefaultDataDir() (string, error) {
	if runtime.GOOS == "windows" {
		if d := os.Getenv("LOCALAPPDATA"); d != "" {
			return filepath.Join(d, DirName), nil
		}
	}
	if d := os.Getenv("XDG_DATA_HOME"); d != "" {
		return filepath.Join(d, DirName), nil
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(home, ".local", "share", DirName), nil
}

// Load reads dataDir/config.json, filling defaults and generating an install
// id on first run.
func Load(path string) (*Config, error) {
	b, err := os.ReadFile(path)
	if errors.Is(err, fs.ErrNotExist) {
		return nil, fmt.Errorf("no configuration at %s; see docs/setup.md", path)
	}
	if err != nil {
		return nil, err
	}
	var c Config
	if err := json.Unmarshal(b, &c); err != nil {
		return nil, fmt.Errorf("config %s: %w", path, err)
	}
	c.applyDefaults()
	if c.InstallID == "" {
		rb := make([]byte, 16)
		if _, err := rand.Read(rb); err != nil {
			return nil, err
		}
		c.InstallID = hex.EncodeToString(rb)
		out, _ := json.MarshalIndent(&c, "", "  ")
		if err := os.WriteFile(path, out, 0o600); err != nil {
			return nil, fmt.Errorf("persist install id: %w", err)
		}
	}
	return &c, c.Validate()
}
