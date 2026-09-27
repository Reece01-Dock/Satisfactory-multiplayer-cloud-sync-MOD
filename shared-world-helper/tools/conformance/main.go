// Command conformance writes a corpus of valid and damaged save files plus
// expected.json holding the Go reference validator's verdict for each file.
// The C++ core (shared-world-mod/core-tests) must reach the same verdicts.
//
//	go run ./tools/conformance ../shared-world-mod/core-tests/testdata/conformance
package main

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"math/rand"
	"os"
	"path/filepath"
	"sort"
	"unicode/utf16"

	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/savefile"
)

type opts struct {
	headerVersion int32
	saveVersion   int32
	utf16Name     bool
	chunkV1       bool
	checksum      bool
	chunkSize     int
}

// build mirrors savefile.BuildSynthetic with layout variations.
func build(session string, body []byte, o opts) []byte {
	var b bytes.Buffer
	le := binary.LittleEndian
	w32 := func(v int32) { _ = binary.Write(&b, le, v) }
	wstr := func(s string, wide bool) {
		if s == "" {
			w32(0)
			return
		}
		if wide {
			u := utf16.Encode([]rune(s))
			w32(-int32(len(u) + 1))
			for _, c := range u {
				_ = binary.Write(&b, le, c)
			}
			_ = binary.Write(&b, le, uint16(0))
			return
		}
		w32(int32(len(s) + 1))
		b.WriteString(s)
		b.WriteByte(0)
	}
	w32(o.headerVersion)
	w32(o.saveVersion)
	w32(400000)
	if o.headerVersion >= 14 {
		wstr(session, o.utf16Name)
	}
	wstr("Persistent_Level", false)
	wstr("?startloc=Grass Fields", false)
	wstr(session, o.utf16Name)
	w32(3600)
	_ = binary.Write(&b, le, int64(638000000000000000))
	b.WriteByte(0)
	w32(0)
	if o.headerVersion >= 8 {
		wstr(`{"Version":1,"FullMods":[]}`, false)
		w32(1)
	}
	if o.headerVersion >= 10 {
		wstr("00000000000000000000000000000000", false)
	}
	if o.headerVersion >= 11 {
		w32(1)
	}
	if o.headerVersion >= 12 {
		if o.checksum {
			w32(1)
			b.Write(bytes.Repeat([]byte{0xAB}, 16))
		} else {
			w32(0)
		}
	}
	if o.headerVersion >= 13 {
		w32(0)
	}
	var raw bytes.Buffer
	_ = binary.Write(&raw, le, int32(len(body)))
	if o.saveVersion >= 37 {
		_ = binary.Write(&raw, le, int32(0))
	}
	raw.Write(body)
	data := raw.Bytes()
	for off := 0; off < len(data); off += o.chunkSize {
		end := min(off+o.chunkSize, len(data))
		var z bytes.Buffer
		zw := zlib.NewWriter(&z)
		_, _ = zw.Write(data[off:end])
		_ = zw.Close()
		_ = binary.Write(&b, le, uint32(0x9E2A83C1))
		if o.chunkV1 {
			_ = binary.Write(&b, le, uint32(0))
			_ = binary.Write(&b, le, int64(o.chunkSize))
		} else {
			_ = binary.Write(&b, le, uint32(0x22222222))
			_ = binary.Write(&b, le, int64(o.chunkSize))
			b.WriteByte(3)
		}
		for i := 0; i < 2; i++ {
			_ = binary.Write(&b, le, int64(z.Len()))
			_ = binary.Write(&b, le, int64(end-off))
		}
		b.Write(z.Bytes())
	}
	return b.Bytes()
}

type verdict struct {
	Valid       bool   `json:"valid"`
	SessionName string `json:"sessionName,omitempty"`
	MapName     string `json:"mapName,omitempty"`
}

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: conformance <outdir>")
		os.Exit(2)
	}
	out := os.Args[1]
	_ = os.RemoveAll(out)
	if err := os.MkdirAll(out, 0o755); err != nil {
		panic(err)
	}
	r := rand.New(rand.NewSource(7))
	randBody := func(n int) []byte { b := make([]byte, n); r.Read(b); return b }
	textBody := bytes.Repeat([]byte("Build_ConveyorBeltMk5_C;"), 20000)
	cur := opts{headerVersion: 14, saveVersion: 52, chunkSize: 1 << 17}

	files := map[string][]byte{}
	files["valid_small.sav"] = build("Small", []byte("tiny body"), cur)
	files["valid_multichunk.sav"] = build("Multi", textBody, cur)
	files["valid_random_multichunk.sav"] = build("Rnd", randBody(300000), cur)
	o := cur
	o.utf16Name = true
	files["valid_utf16_name.sav"] = build("Fabrik Größe 工厂", textBody[:5000], o)
	o = cur
	o.chunkV1 = true
	files["valid_chunk_v1.sav"] = build("V1", textBody, o)
	o = cur
	o.checksum = true
	files["valid_with_checksum.sav"] = build("Sum", textBody[:9000], o)
	o = opts{headerVersion: 13, saveVersion: 46, chunkSize: 1 << 17}
	files["valid_header_v13.sav"] = build("Old13", textBody[:7000], o)
	o = opts{headerVersion: 10, saveVersion: 30, chunkSize: 1 << 17} // pre-UE5 4-byte body prefix
	files["valid_pre_ue5.sav"] = build("PreUE5", textBody[:7000], o)
	files["valid_go_builder.sav"] = savefile.BuildSynthetic("GoBuilder", textBody)

	base := files["valid_multichunk.sav"]
	for _, cut := range []int{len(base) - 1, len(base) - 50, len(base) / 2, 300, 100, 70} {
		files[fmt.Sprintf("bad_truncated_%d.sav", cut)] = base[:cut]
	}
	flip := func(b []byte, at int) []byte { c := bytes.Clone(b); c[at] ^= 0x5A; return c }
	files["bad_bitflip_last_chunk.sav"] = flip(base, len(base)-20)
	files["bad_bitflip_header_version.sav"] = flip(base, 0)
	files["bad_trailing_byte.sav"] = append(bytes.Clone(base), 0)
	files["bad_trailing_chunk_garbage.sav"] = append(bytes.Clone(base), bytes.Repeat([]byte{0xC1, 0x83, 0x2A, 0x9E}, 20)...)
	files["bad_random_garbage.sav"] = randBody(5000)
	files["bad_empty_body.sav"] = build("Empty", nil, cur)[:0]
	hdrOnly := build("H", []byte("x"), cur)
	files["bad_header_only.sav"] = hdrOnly[:len(hdrOnly)-60]
	o = cur
	o.headerVersion = 5
	files["bad_too_old_header.sav"] = build("Ancient", textBody[:3000], o)
	// Body declares a different size than it contains.
	lie := build("Lie", textBody[:4000], cur)
	files["bad_wrong_declared_size.sav"] = func() []byte {
		// Re-encode with a body whose declared length is off by one.
		var raw bytes.Buffer
		_ = binary.Write(&raw, binary.LittleEndian, int32(3999))
		_ = binary.Write(&raw, binary.LittleEndian, int32(0))
		raw.Write(textBody[:4000])
		hdrEnd := bytes.Index(lie, []byte{0xC1, 0x83, 0x2A, 0x9E})
		var z bytes.Buffer
		zw := zlib.NewWriter(&z)
		_, _ = zw.Write(raw.Bytes())
		_ = zw.Close()
		var b bytes.Buffer
		b.Write(lie[:hdrEnd])
		_ = binary.Write(&b, binary.LittleEndian, uint32(0x9E2A83C1))
		_ = binary.Write(&b, binary.LittleEndian, uint32(0x22222222))
		_ = binary.Write(&b, binary.LittleEndian, int64(1<<17))
		b.WriteByte(3)
		for i := 0; i < 2; i++ {
			_ = binary.Write(&b, binary.LittleEndian, int64(z.Len()))
			_ = binary.Write(&b, binary.LittleEndian, int64(raw.Len()))
		}
		b.Write(z.Bytes())
		return b.Bytes()
	}()

	expected := map[string]verdict{}
	names := make([]string, 0, len(files))
	for n := range files {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		p := filepath.Join(out, n)
		if err := os.WriteFile(p, files[n], 0o644); err != nil {
			panic(err)
		}
		h, err := savefile.CheckFile(p)
		v := verdict{Valid: err == nil}
		if h != nil {
			v.SessionName, v.MapName = h.SessionName, h.MapName
		}
		expected[n] = v
		fmt.Printf("%-34s valid=%v\n", n, v.Valid)
	}
	j, _ := json.MarshalIndent(expected, "", "  ")
	if err := os.WriteFile(filepath.Join(out, "expected.json"), j, 0o644); err != nil {
		panic(err)
	}
}
