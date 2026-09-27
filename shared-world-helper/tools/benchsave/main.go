package main

// Generates a sequence of synthetic saves in the real container format
// (header + 128 KiB zlib chunks) whose body mimics a factory: fixed-size
// object records with names, transforms and inventories. Each revision
// changes ~1% of records and appends new buildings (like real play).
import (
	"encoding/binary"
	"fmt"
	"math/rand"
	"os"

	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/savefile"
)

func record(r *rand.Rand, i int) []byte {
	b := make([]byte, 0, 200)
	b = append(b, []byte(fmt.Sprintf("/Game/FactoryGame/Buildable/Factory/Build_%d.Build_%d_C:Persistent_Level.Obj_%08d", i%40, i%40, i))...)
	for k := 0; k < 10; k++ { // transform + state floats
		b = binary.LittleEndian.AppendUint32(b, r.Uint32())
	}
	for k := 0; k < 4; k++ { // inventory slots
		b = append(b, []byte(fmt.Sprintf("Desc_IronPlate_C:%d;", r.Intn(500)))...)
	}
	return b
}

func main() {
	r := rand.New(rand.NewSource(1))
	n := 150000
	recs := make([][]byte, n)
	for i := range recs {
		recs[i] = record(r, i)
	}
	for rev := 1; rev <= 10; rev++ {
		if rev > 1 {
			for k := 0; k < n/100; k++ { // 1% of objects change state
				j := r.Intn(len(recs))
				recs[j] = record(r, j)
			}
			for k := 0; k < 500; k++ { // new buildings
				recs = append(recs, record(r, len(recs)))
			}
		}
		var body []byte
		for _, x := range recs {
			body = append(body, x...)
		}
		os.WriteFile(fmt.Sprintf("rev%02d.sav", rev), savefile.BuildSynthetic("Bench", body), 0o644)
	}
}
