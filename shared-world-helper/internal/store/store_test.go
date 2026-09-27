package store

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"os"
	"path/filepath"
	"sync"
	"testing"
)

// Several FS instances on one directory behave like several helper
// processes: a CAS counter must never lose an increment.
func TestFSCompareAndSwapAcrossInstances(t *testing.T) {
	dir := t.TempDir()
	const instances, perInstance = 6, 25
	ctx := context.Background()
	first, _ := NewFS(dir)
	if _, err := first.PutRecord(ctx, "k/counter.json", []byte(`{"n":0}`), NoVersion); err != nil {
		t.Fatal(err)
	}
	var wg sync.WaitGroup
	for i := 0; i < instances; i++ {
		fsi, _ := NewFS(dir)
		wg.Add(1)
		go func() {
			defer wg.Done()
			for done := 0; done < perInstance; {
				b, v, err := fsi.GetRecord(ctx, "k/counter.json")
				if err != nil {
					t.Error(err)
					return
				}
				var c struct{ N int }
				_ = json.Unmarshal(b, &c)
				c.N++
				nb, _ := json.Marshal(c)
				if _, err := fsi.PutRecord(ctx, "k/counter.json", nb, v); err == nil {
					done++
				} else if !errors.Is(err, ErrPreconditionFailed) {
					t.Error(err)
					return
				}
			}
		}()
	}
	wg.Wait()
	b, _, _ := first.GetRecord(ctx, "k/counter.json")
	var c struct{ N int }
	_ = json.Unmarshal(b, &c)
	if c.N != instances*perInstance {
		t.Fatalf("counter = %d, want %d (lost updates)", c.N, instances*perInstance)
	}
}

func TestFSCreateOnlyAndNoPartialBlobs(t *testing.T) {
	f, _ := NewFS(t.TempDir())
	ctx := context.Background()
	if err := f.PutBlob(ctx, "w/a.sav", bytes.NewReader([]byte("hello")), 5); err != nil {
		t.Fatal(err)
	}
	if err := f.PutBlob(ctx, "w/a.sav", bytes.NewReader([]byte("other")), 5); !errors.Is(err, ErrAlreadyExists) {
		t.Fatalf("overwrite allowed: %v", err)
	}
	// A short write (connection drop) must not leave anything under the key.
	if err := f.PutBlob(ctx, "w/b.sav", io.LimitReader(bytes.NewReader(make([]byte, 100)), 10), 100); err == nil {
		t.Fatal("short write accepted")
	}
	if _, err := f.StatBlob(ctx, "w/b.sav"); !errors.Is(err, ErrNotFound) {
		t.Fatal("partial blob visible")
	}
	rc, _ := f.GetBlob(ctx, "w/a.sav")
	b, _ := io.ReadAll(rc)
	rc.Close()
	if string(b) != "hello" {
		t.Fatalf("blob content %q", b)
	}
	entries, _ := os.ReadDir(filepath.Join(f.root, "w"))
	for _, e := range entries {
		if e.Name() != "a.sav" {
			t.Fatalf("leftover file %s", e.Name())
		}
	}
}

func TestKeyValidationBlocksTraversal(t *testing.T) {
	f, _ := NewFS(t.TempDir())
	for _, k := range []string{"../x", "a/../../x", "/abs", `a\b`, "a//b", "A/B", "", "a/./b"} {
		if _, _, err := f.GetRecord(context.Background(), k); err == nil || errors.Is(err, ErrNotFound) {
			t.Fatalf("key %q not rejected: %v", k, err)
		}
	}
}
