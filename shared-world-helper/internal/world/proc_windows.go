//go:build windows

package world

import "syscall"

const (
	processQueryLimitedInformation = 0x1000
	stillActive                    = 259
)

// ProcessAlive reports whether pid refers to a running process.
func ProcessAlive(pid int) bool {
	h, err := syscall.OpenProcess(processQueryLimitedInformation, false, uint32(pid))
	if err != nil {
		return false
	}
	defer syscall.CloseHandle(h)
	var code uint32
	if err := syscall.GetExitCodeProcess(h, &code); err != nil {
		return false
	}
	return code == stillActive
}
