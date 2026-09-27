// Package savefile handles local Satisfactory save files: hashing, a
// conservative header sanity check, waiting until the game has finished
// writing, and atomic replacement.
package savefile

import (
	"bufio"
	"bytes"
	"compress/zlib"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"time"
	"unicode/utf16"
)

// Extension used by Satisfactory save files.
const Extension = ".sav"

// MaxSaveSize guards against absurd downloads (Satisfactory saves are tens of MB).
const MaxSaveSize = 2 << 30

var saveNamePattern = regexp.MustCompile(`^[A-Za-z0-9_-]{1,64}$`)

// ValidateSaveName ensures a save name is a plain file stem that cannot
// traverse directories.
func ValidateSaveName(name string) error {
	if !saveNamePattern.MatchString(name) {
		return fmt.Errorf("invalid save name %q", name)
	}
	return nil
}

// PathFor returns <dir>/<name>.sav after validating name.
func PathFor(dir, name string) (string, error) {
	if err := ValidateSaveName(name); err != nil {
		return "", err
	}
	return filepath.Join(dir, name+Extension), nil
}

// HashFile returns the lowercase hex SHA-256 and size of a file.
func HashFile(p string) (string, int64, error) {
	f, err := os.Open(p)
	if err != nil {
		return "", 0, err
	}
	defer f.Close()
	return HashReader(f)
}

func HashReader(r io.Reader) (string, int64, error) {
	h := sha256.New()
	n, err := io.Copy(h, io.LimitReader(r, MaxSaveSize+1))
	if err != nil {
		return "", n, err
	}
	if n > MaxSaveSize {
		return "", n, errors.New("save file exceeds maximum size")
	}
	return hex.EncodeToString(h.Sum(nil)), n, nil
}

// Header holds the save header fields this helper reads. The on-disk order
// follows FSaveHeader's version history in FGSaveManagerInterface.h and was
// cross-checked against @etothepii/satisfactory-file-parser 4.1.2.
type Header struct {
	HeaderVersion int32
	SaveVersion   int32
	BuildVersion  int32
	SaveName      string
	MapName       string
	MapOptions    string
	SessionName   string
	PlayDuration  int32
	SaveTicks     int64
}

// FSaveHeader::Type values that change the layout.
const (
	hdrUE425EngineUpdate             = 7
	hdrAddedModdingParams            = 8
	hdrAddedSaveIdentifier           = 10
	hdrAddedWorldPartitionSupport    = 11
	hdrAddedSaveModificationChecksum = 12
	hdrAddedIsCreativeModeEnabled    = 13
	hdrAddedSaveName                 = 14
	// Oldest header this helper accepts; anything older predates the
	// compressed-chunk body format checked below.
	minHeaderVersion = hdrUE425EngineUpdate
	// Save versions >= 37 (SaveCustomVersion::UnrealEngine5) carry an 8
	// byte body-size prefix instead of 4.
	saveVersionUE5 = 37
)

var errBadHeader = errors.New("not a valid Satisfactory save header")

func readFString(r io.Reader) (string, error) {
	var n int32
	if err := binary.Read(r, binary.LittleEndian, &n); err != nil {
		return "", err
	}
	switch {
	case n == 0:
		return "", nil
	case n > 0 && n <= 1<<20:
		b := make([]byte, n)
		if _, err := io.ReadFull(r, b); err != nil {
			return "", err
		}
		if b[n-1] != 0 {
			return "", errBadHeader
		}
		return string(b[:n-1]), nil
	case n < 0 && n >= -(1<<20):
		u := make([]uint16, -n)
		if err := binary.Read(r, binary.LittleEndian, u); err != nil {
			return "", err
		}
		if u[len(u)-1] != 0 {
			return "", errBadHeader
		}
		return string(utf16.Decode(u[:len(u)-1])), nil
	}
	return "", errBadHeader
}

func readI32(r io.Reader) (int32, error) {
	var v int32
	err := binary.Read(r, binary.LittleEndian, &v)
	return v, err
}

// ReadHeader parses the complete save header, leaving r positioned at the
// start of the compressed body.
func ReadHeader(r io.Reader) (*Header, error) {
	var h Header
	var err error
	fail := func() (*Header, error) { return nil, errBadHeader }
	for _, p := range []*int32{&h.HeaderVersion, &h.SaveVersion, &h.BuildVersion} {
		if *p, err = readI32(r); err != nil {
			return fail()
		}
	}
	if h.HeaderVersion < minHeaderVersion || h.HeaderVersion > 64 || h.SaveVersion < 0 || h.BuildVersion < 0 {
		return fail()
	}
	if h.HeaderVersion >= hdrAddedSaveName {
		if h.SaveName, err = readFString(r); err != nil {
			return fail()
		}
	}
	for _, p := range []*string{&h.MapName, &h.MapOptions, &h.SessionName} {
		if *p, err = readFString(r); err != nil {
			return fail()
		}
	}
	if h.MapName == "" {
		return fail()
	}
	if h.PlayDuration, err = readI32(r); err != nil {
		return fail()
	}
	if err := binary.Read(r, binary.LittleEndian, &h.SaveTicks); err != nil {
		return fail()
	}
	var skip [1]byte // session visibility
	if _, err := io.ReadFull(r, skip[:]); err != nil {
		return fail()
	}
	if _, err := readI32(r); err != nil { // editor object version (>= UE425EngineUpdate)
		return fail()
	}
	if h.HeaderVersion >= hdrAddedModdingParams {
		if _, err := readFString(r); err != nil { // mod metadata
			return fail()
		}
		if _, err := readI32(r); err != nil { // is modded
			return fail()
		}
	}
	if h.HeaderVersion >= hdrAddedSaveIdentifier {
		if _, err := readFString(r); err != nil {
			return fail()
		}
	}
	if h.HeaderVersion >= hdrAddedWorldPartitionSupport {
		if _, err := readI32(r); err != nil {
			return fail()
		}
	}
	if h.HeaderVersion >= hdrAddedSaveModificationChecksum {
		valid, err := readI32(r)
		if err != nil {
			return fail()
		}
		if valid == 1 {
			var md5 [16]byte
			if _, err := io.ReadFull(r, md5[:]); err != nil {
				return fail()
			}
		}
	}
	if h.HeaderVersion >= hdrAddedIsCreativeModeEnabled {
		if _, err := readI32(r); err != nil {
			return fail()
		}
	}
	return &h, nil
}

// Compressed body chunk format (UE package-file-tag chunks).
const (
	packageFileTag  = 0x9E2A83C1
	chunkHeaderV1   = 0x00000000
	chunkHeaderV2   = 0x22222222
	compressionZlib = 3
	maxChunkBytes   = 256 << 20
)

// verifyBody walks every compressed chunk to the end of the file, inflates
// it, and checks the body's self-declared size. A save truncated by a crash
// mid-write, or with any corrupted chunk, fails here.
func verifyBody(r io.Reader, saveVersion int32) error {
	br := bufio.NewReaderSize(r, 1<<16)
	var total int64
	var declared int64 = -1
	var firstBytes []byte
	for i := 0; ; i++ {
		var tag [4]byte
		n, err := io.ReadFull(br, tag[:])
		if err == io.EOF && n == 0 {
			if i == 0 {
				return errors.New("save body is empty")
			}
			break
		}
		if err != nil {
			return fmt.Errorf("chunk %d: truncated chunk header", i)
		}
		if binary.LittleEndian.Uint32(tag[:]) != packageFileTag {
			return fmt.Errorf("chunk %d: bad package file tag", i)
		}
		var ver [4]byte
		if _, err := io.ReadFull(br, ver[:]); err != nil {
			return fmt.Errorf("chunk %d: truncated chunk header", i)
		}
		hdrLen := 49
		switch binary.LittleEndian.Uint32(ver[:]) {
		case chunkHeaderV2:
		case chunkHeaderV1:
			hdrLen = 48
		default:
			return fmt.Errorf("chunk %d: unknown chunk header version", i)
		}
		rest := make([]byte, hdrLen-8)
		if _, err := io.ReadFull(br, rest); err != nil {
			return fmt.Errorf("chunk %d: truncated chunk header", i)
		}
		// Offsets below are relative to the chunk start, minus the 8 bytes read.
		off := 0
		if hdrLen == 49 {
			if rest[16-8] != compressionZlib {
				return fmt.Errorf("chunk %d: unsupported compression %d", i, rest[16-8])
			}
			off = 1
		}
		compressed := int64(binary.LittleEndian.Uint64(rest[32+off-8:]))
		uncompressed := int64(binary.LittleEndian.Uint64(rest[40+off-8:]))
		if compressed <= 0 || compressed > maxChunkBytes || uncompressed < 0 || uncompressed > maxChunkBytes {
			return fmt.Errorf("chunk %d: implausible chunk sizes", i)
		}
		lr := &io.LimitedReader{R: br, N: compressed}
		zr, err := zlib.NewReader(lr)
		if err != nil {
			return fmt.Errorf("chunk %d: %w", i, err)
		}
		var sink io.Writer = io.Discard
		var head bytes.Buffer
		if i == 0 {
			sink = &limitWriter{w: &head, n: 8}
		}
		got, err := io.Copy(sink, zr)
		zr.Close()
		if err != nil {
			return fmt.Errorf("chunk %d: corrupt compressed data: %w", i, err)
		}
		if lr.N != 0 {
			return fmt.Errorf("chunk %d: compressed length mismatch", i)
		}
		if got != uncompressed {
			return fmt.Errorf("chunk %d: inflated %d bytes, header says %d", i, got, uncompressed)
		}
		if i == 0 {
			firstBytes = head.Bytes()
		}
		total += got
	}
	if len(firstBytes) < 4 {
		return errors.New("save body too short")
	}
	declared = int64(int32(binary.LittleEndian.Uint32(firstBytes)))
	extra := int64(4)
	if saveVersion >= saveVersionUE5 {
		extra = 8
	}
	if declared+extra != total {
		return fmt.Errorf("save body declares %d bytes but contains %d", declared+extra, total)
	}
	return nil
}

// limitWriter keeps the first n bytes and discards the rest.
type limitWriter struct {
	w io.Writer
	n int
}

func (l *limitWriter) Write(p []byte) (int, error) {
	if l.n > 0 {
		k := min(l.n, len(p))
		if _, err := l.w.Write(p[:k]); err != nil {
			return 0, err
		}
		l.n -= k
	}
	return len(p), nil
}

// CheckFile verifies that a file is a complete, internally consistent save:
// parsable header, every body chunk intact, body length as declared.
func CheckFile(p string) (*Header, error) {
	f, err := os.Open(p)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		return nil, err
	}
	if st.Size() < 64 || st.Size() > MaxSaveSize {
		return nil, fmt.Errorf("save file has implausible size %d", st.Size())
	}
	br := bufio.NewReader(f)
	h, err := ReadHeader(br)
	if err != nil {
		return nil, err
	}
	if err := verifyBody(br, h.SaveVersion); err != nil {
		return nil, fmt.Errorf("save body check failed: %w", err)
	}
	return h, nil
}

// WaitStable waits until the file's size and modification time stop
// changing for `quiet`, so a save still being written is never uploaded.
// The mod also tells the helper when the game's SaveGame callback has fired;
// this is a second, independent guard.
func WaitStable(p string, quiet, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	last, err := os.Stat(p)
	if err != nil {
		return err
	}
	stableSince := time.Now()
	for {
		time.Sleep(quiet / 4)
		cur, err := os.Stat(p)
		if err != nil {
			return err
		}
		if cur.Size() != last.Size() || !cur.ModTime().Equal(last.ModTime()) {
			last, stableSince = cur, time.Now()
		} else if time.Since(stableSince) >= quiet {
			return nil
		}
		if time.Now().After(deadline) {
			return errors.New("save file kept changing; the game may still be writing it")
		}
	}
}

// Snapshot copies src to a private file in dir and returns its path, hash
// and size. Uploads work from the snapshot so the game can keep writing the
// original without affecting the upload.
func Snapshot(src, dir string) (path, sha string, size int64, err error) {
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return "", "", 0, err
	}
	in, err := os.Open(src)
	if err != nil {
		return "", "", 0, err
	}
	defer in.Close()
	path = filepath.Join(dir, "snapshot-"+randomHex(8)+Extension)
	out, err := os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o600)
	if err != nil {
		return "", "", 0, err
	}
	h := sha256.New()
	size, err = io.Copy(io.MultiWriter(out, h), io.LimitReader(in, MaxSaveSize+1))
	if err == nil && size > MaxSaveSize {
		err = errors.New("save file exceeds maximum size")
	}
	if err == nil {
		err = out.Sync()
	}
	if cerr := out.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		os.Remove(path)
		return "", "", 0, err
	}
	return path, hex.EncodeToString(h.Sum(nil)), size, nil
}

func randomHex(n int) string {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	return hex.EncodeToString(b)
}

// TempPathNextTo returns a temp path in the same directory as target, so a
// later rename is atomic. The extension is not ".sav", so the game's save
// browser never lists a half-written file.
func TempPathNextTo(target string) string {
	return target + ".swtmp-" + randomHex(6)
}

// ReplaceAtomic renames a fully written and verified temp file over target.
func ReplaceAtomic(tmp, target string) error {
	f, err := os.Open(tmp)
	if err != nil {
		return err
	}
	serr := f.Sync()
	f.Close()
	if serr != nil {
		return serr
	}
	return os.Rename(tmp, target)
}

// CopyFile copies src to dst (dst must not exist), fsyncing the result.
func CopyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.OpenFile(dst, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o600)
	if err != nil {
		return err
	}
	_, err = io.Copy(out, in)
	if err == nil {
		err = out.Sync()
	}
	if cerr := out.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		os.Remove(dst)
	}
	return err
}
