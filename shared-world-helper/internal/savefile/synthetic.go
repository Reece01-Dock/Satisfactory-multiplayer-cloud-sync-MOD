package savefile

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
)

// BuildSynthetic produces a structurally valid save file (current header
// layout, zlib body chunks) with arbitrary body content. It is used by tests
// and by the helper's self-test; it is not loadable by the game.
func BuildSynthetic(sessionName string, body []byte) []byte {
	var b bytes.Buffer
	le := binary.LittleEndian
	w32 := func(v int32) { _ = binary.Write(&b, le, v) }
	wstr := func(s string) {
		if s == "" {
			w32(0)
			return
		}
		w32(int32(len(s) + 1))
		b.WriteString(s)
		b.WriteByte(0)
	}
	w32(hdrAddedSaveName) // header version
	w32(52)               // save version (>= UE5 layout)
	w32(400000)           // build version
	wstr(sessionName)     // save name
	wstr("Persistent_Level")
	wstr("?startloc=Grass Fields")
	wstr(sessionName)
	w32(3600)                                           // play duration
	_ = binary.Write(&b, le, int64(638000000000000000)) // save ticks
	b.WriteByte(0)                                      // session visibility
	w32(0)                                              // editor object version
	wstr(`{"Version":1,"FullMods":[]}`)                 // mod metadata
	w32(1)                                              // is modded
	wstr("00000000000000000000000000000000")            // save identifier
	w32(1)                                              // partitioned world
	w32(0)                                              // checksum not valid
	w32(0)                                              // creative mode

	// Body: int32 declared size + 4 padding bytes (UE5 layout: extra = 8), then payload.
	var raw bytes.Buffer
	_ = binary.Write(&raw, le, int32(len(body)))
	_ = binary.Write(&raw, le, int32(0))
	raw.Write(body)
	data := raw.Bytes()
	const chunk = 1 << 17
	for off := 0; off < len(data); off += chunk {
		end := min(off+chunk, len(data))
		var z bytes.Buffer
		zw := zlib.NewWriter(&z)
		_, _ = zw.Write(data[off:end])
		_ = zw.Close()
		_ = binary.Write(&b, le, uint32(packageFileTag))
		_ = binary.Write(&b, le, uint32(chunkHeaderV2))
		_ = binary.Write(&b, le, int64(chunk))
		b.WriteByte(compressionZlib)
		for i := 0; i < 2; i++ {
			_ = binary.Write(&b, le, int64(z.Len()))
			_ = binary.Write(&b, le, int64(end-off))
		}
		b.Write(z.Bytes())
	}
	return b.Bytes()
}
