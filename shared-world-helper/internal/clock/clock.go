// Package clock abstracts wall-clock time so lease expiry can be tested
// deterministically.
package clock

import (
	"sync"
	"time"
)

// Clock returns the current wall-clock time.
type Clock interface {
	Now() time.Time
}

// System is the real clock. Times are always UTC so serialized metadata is
// comparable across machines in different time zones.
type System struct{}

func (System) Now() time.Time { return time.Now().UTC() }

// Fake is a manually advanced clock for tests.
type Fake struct {
	mu  sync.Mutex
	now time.Time
}

func NewFake(start time.Time) *Fake { return &Fake{now: start.UTC()} }

func (f *Fake) Now() time.Time {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.now
}

func (f *Fake) Advance(d time.Duration) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.now = f.now.Add(d)
}
