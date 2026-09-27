// Command synthsave writes a structurally valid (but not game-loadable)
// Satisfactory save file for helper development and smoke tests.
package main

import (
	"fmt"
	"os"

	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/savefile"
)

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "usage: synthsave <out.sav> <body-text>")
		os.Exit(2)
	}
	if err := os.WriteFile(os.Args[1], savefile.BuildSynthetic("Synthetic", []byte(os.Args[2])), 0o600); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
