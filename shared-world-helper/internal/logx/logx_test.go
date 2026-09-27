package logx

import (
	"bytes"
	"log/slog"
	"strings"
	"testing"
)

func TestSecretsAreRedacted(t *testing.T) {
	var buf bytes.Buffer
	log := New(&buf, slog.LevelInfo)
	log.Info("x", "token", "abc123", "refresh_token", "r1", "Authorization", "Bearer z", "generation", 7)
	out := buf.String()
	for _, secret := range []string{"abc123", "r1", "Bearer z"} {
		if strings.Contains(out, secret) {
			t.Fatalf("secret %q logged: %s", secret, out)
		}
	}
	if !strings.Contains(out, `"generation":7`) || !strings.Contains(out, `"component":"SharedWorldHelper"`) {
		t.Fatalf("expected fields missing: %s", out)
	}
}
