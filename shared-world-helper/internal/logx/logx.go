// Package logx sets up structured logging with secret redaction.
package logx

import (
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
)

var secretKeys = []string{"token", "secret", "password", "authorization", "credential", "refresh", "apikey", "api_key"}

// isSecretKey matches attribute keys that must never be logged in clear.
// "generation" etc. are fine; anything that smells like a credential is not.
func isSecretKey(k string) bool {
	k = strings.ToLower(k)
	for _, s := range secretKeys {
		if strings.Contains(k, s) {
			return true
		}
	}
	return false
}

func redact(_ []string, a slog.Attr) slog.Attr {
	if isSecretKey(a.Key) {
		return slog.String(a.Key, "[REDACTED]")
	}
	return a
}

// New returns a JSON logger tagged component=SharedWorldHelper.
func New(w io.Writer, level slog.Level) *slog.Logger {
	h := slog.NewJSONHandler(w, &slog.HandlerOptions{Level: level, ReplaceAttr: redact})
	return slog.New(h).With("component", "SharedWorldHelper")
}

// OpenLogFile opens dir/helper.log for append, rotating it to helper.log.1
// when it exceeds maxBytes.
func OpenLogFile(dir string, maxBytes int64) (*os.File, error) {
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return nil, err
	}
	p := filepath.Join(dir, "helper.log")
	if st, err := os.Stat(p); err == nil && st.Size() > maxBytes {
		_ = os.Rename(p, p+".1")
	}
	return os.OpenFile(p, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
}
