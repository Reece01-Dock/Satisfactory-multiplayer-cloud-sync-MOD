package savefile

import (
	"bytes"
	"crypto/rand"
	"os"
	"path/filepath"
	"testing"
)

func writeTemp(t *testing.T, b []byte) string {
	t.Helper()
	p := filepath.Join(t.TempDir(), "x.sav")
	if err := os.WriteFile(p, b, 0o600); err != nil {
		t.Fatal(err)
	}
	return p
}

func TestCheckFileAcceptsValidSave(t *testing.T) {
	body := make([]byte, 400_000) // spans several chunks
	rand.Read(body)
	h, err := CheckFile(writeTemp(t, BuildSynthetic("MyFactory", body)))
	if err != nil {
		t.Fatal(err)
	}
	if h.SessionName != "MyFactory" || h.MapName != "Persistent_Level" || h.SaveName != "MyFactory" {
		t.Fatalf("unexpected header %+v", h)
	}
}

func TestCheckFileRejectsTruncatedSave(t *testing.T) {
	full := BuildSynthetic("S", bytes.Repeat([]byte("factory"), 100_000))
	for _, cut := range []int{len(full) - 1, len(full) - 1000, len(full) / 2, 200} {
		if _, err := CheckFile(writeTemp(t, full[:cut])); err == nil {
			t.Fatalf("truncated save (%d of %d bytes) accepted", cut, len(full))
		}
	}
}

func TestCheckFileRejectsCorruptChunk(t *testing.T) {
	full := BuildSynthetic("S", bytes.Repeat([]byte("factory"), 50_000))
	full[len(full)-10] ^= 0xFF
	if _, err := CheckFile(writeTemp(t, full)); err == nil {
		t.Fatal("corrupted save accepted")
	}
}

func TestCheckFileRejectsGarbage(t *testing.T) {
	junk := make([]byte, 5000)
	rand.Read(junk)
	if _, err := CheckFile(writeTemp(t, junk)); err == nil {
		t.Fatal("garbage accepted")
	}
}

func TestValidateSaveName(t *testing.T) {
	for _, bad := range []string{"", "../x", "a/b", `a\b`, "x.sav", "con:"} {
		if ValidateSaveName(bad) == nil {
			t.Fatalf("%q accepted", bad)
		}
	}
	if ValidateSaveName("SharedWorld_our-factory") != nil {
		t.Fatal("valid name rejected")
	}
}
