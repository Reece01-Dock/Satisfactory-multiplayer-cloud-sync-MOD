//go:build !windows

package world

import (
	"errors"
	"os"
	"syscall"
)

// ProcessAlive reports whether pid refers to a running process.
func ProcessAlive(pid int) bool {
	p, err := os.FindProcess(pid)
	if err != nil {
		return false
	}
	err = p.Signal(syscall.Signal(0))
	return err == nil || errors.Is(err, syscall.EPERM)
}
