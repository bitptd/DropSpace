package zipcheck

import (
	"archive/zip"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"path"
	"sort"
	"strconv"
	"strings"
	"unicode/utf8"

	"golang.org/x/text/unicode/norm"
)

const (
	DefaultMaxExpandedBytes = int64(4 * 1024 * 1024 * 1024)
	DefaultMaxEntries       = 10_000
	DefaultMaxDepth         = 64
	DefaultMaxRatio         = uint64(1_000)
)

type Expected struct {
	ExpandedBytes int64
	EntryCount    int64
	TreeSHA256    string
}

type Limits struct {
	MaxExpandedBytes int64
	MaxEntries       int
	MaxDepth         int
	MaxRatio         uint64
}

type Result struct {
	ExpandedBytes int64
	EntryCount    int64
	TreeSHA256    string
	Entries       []Entry
}

type Entry struct {
	Path      string `json:"path"`
	Directory bool   `json:"directory"`
	Size      int64  `json:"size"`
}

type treeRecord struct {
	path string
	kind byte
	size int64
	hash string
}

func DefaultLimits() Limits {
	return Limits{DefaultMaxExpandedBytes, DefaultMaxEntries, DefaultMaxDepth, DefaultMaxRatio}
}

func Validate(filename string, expected Expected, limits Limits) (Result, error) {
	reader, err := zip.OpenReader(filename)
	if err != nil {
		return Result{}, fmt.Errorf("open zip: %w", err)
	}
	defer reader.Close()
	if limits.MaxEntries <= 0 || limits.MaxExpandedBytes < 0 || limits.MaxDepth <= 0 {
		return Result{}, errors.New("invalid validator limits")
	}
	if len(reader.File) > limits.MaxEntries {
		return Result{}, errors.New("zip has too many entries")
	}

	seen := make(map[string]string, len(reader.File))
	directoryByKey := make(map[string]bool, len(reader.File))
	records := make([]treeRecord, 0, len(reader.File))
	var expanded int64
	manifest := make([]Entry, 0, len(reader.File))
	for _, file := range reader.File {
		if file.Flags&1 != 0 {
			return Result{}, fmt.Errorf("encrypted zip entry rejected: %q", file.Name)
		}
		name, directory, err := validatePath(file.Name, limits.MaxDepth)
		if err != nil {
			return Result{}, err
		}
		key := strings.ToLower(norm.NFC.String(name))
		if previous, exists := seen[key]; exists {
			return Result{}, fmt.Errorf("case or Unicode-normalization path conflict: %q and %q", previous, name)
		}
		seen[key] = name
		mode := file.Mode()
		if mode&(^mode.Perm()) != 0 && !mode.IsRegular() && !mode.IsDir() {
			return Result{}, fmt.Errorf("special or link entry rejected: %q", name)
		}
		if directory != file.FileInfo().IsDir() {
			return Result{}, fmt.Errorf("directory marker mismatch: %q", name)
		}
		parts := strings.Split(key, "/")
		for index := 1; index < len(parts); index++ {
			ancestor := strings.Join(parts[:index], "/")
			if ancestorIsDirectory, exists := directoryByKey[ancestor]; exists && !ancestorIsDirectory {
				return Result{}, fmt.Errorf("file is used as a parent directory: %q", name)
			}
		}
		if !directory {
			prefix := key + "/"
			for existing := range directoryByKey {
				if strings.HasPrefix(existing, prefix) {
					return Result{}, fmt.Errorf("file conflicts with existing child path: %q", name)
				}
			}
		}
		directoryByKey[key] = directory
		if directory {
			records = append(records, treeRecord{path: name, kind: 'D'})
			manifest = append(manifest, Entry{Path: name, Directory: true})
			continue
		}
		if !mode.IsRegular() {
			return Result{}, fmt.Errorf("non-regular entry rejected: %q", name)
		}
		if limits.MaxRatio > 0 && file.UncompressedSize64 > 0 {
			if file.CompressedSize64 == 0 || file.UncompressedSize64/file.CompressedSize64 > limits.MaxRatio {
				return Result{}, fmt.Errorf("compression ratio limit exceeded: %q", name)
			}
		}
		if file.UncompressedSize64 > uint64(limits.MaxExpandedBytes) ||
			expanded > limits.MaxExpandedBytes-int64(file.UncompressedSize64) {
			return Result{}, errors.New("zip expanded byte limit exceeded")
		}
		stream, err := file.Open()
		if err != nil {
			return Result{}, fmt.Errorf("open zip entry %q: %w", name, err)
		}
		digest := sha256.New()
		written, copyErr := io.CopyN(digest, stream, int64(file.UncompressedSize64)+1)
		closeErr := stream.Close()
		if copyErr != nil && !errors.Is(copyErr, io.EOF) {
			return Result{}, fmt.Errorf("read zip entry %q: %w", name, copyErr)
		}
		if closeErr != nil {
			return Result{}, fmt.Errorf("close zip entry %q: %w", name, closeErr)
		}
		if written != int64(file.UncompressedSize64) {
			return Result{}, fmt.Errorf("zip entry size mismatch: %q", name)
		}
		expanded += written
		records = append(records, treeRecord{
			path: name, kind: 'F', size: written, hash: hex.EncodeToString(digest.Sum(nil)),
		})
		manifest = append(manifest, Entry{Path: name, Size: written})
	}

	result := Result{ExpandedBytes: expanded, EntryCount: int64(len(records)), TreeSHA256: treeDigest(records), Entries: manifest}
	if result.ExpandedBytes != expected.ExpandedBytes || result.EntryCount != expected.EntryCount ||
		!strings.EqualFold(result.TreeSHA256, expected.TreeSHA256) {
		return Result{}, fmt.Errorf("folder metadata mismatch: expanded=%d entries=%d tree=%s",
			result.ExpandedBytes, result.EntryCount, result.TreeSHA256)
	}
	return result, nil
}

func treeDigest(records []treeRecord) string {
	sort.Slice(records, func(i, j int) bool { return records[i].path < records[j].path })
	hash := sha256.New()
	for _, record := range records {
		hash.Write([]byte{record.kind, 0})
		hash.Write([]byte(record.path))
		hash.Write([]byte{0})
		if record.kind == 'F' {
			hash.Write([]byte(strconv.FormatInt(record.size, 10)))
			hash.Write([]byte{0})
			hash.Write([]byte(record.hash))
			hash.Write([]byte{0})
		}
	}
	return hex.EncodeToString(hash.Sum(nil))
}

func validatePath(raw string, maxDepth int) (string, bool, error) {
	if raw == "" || strings.IndexByte(raw, 0) >= 0 || !utf8.ValidString(raw) {
		return "", false, errors.New("empty, NUL, or invalid UTF-8 zip path")
	}
	if strings.Contains(raw, "\\") || strings.HasPrefix(raw, "/") || strings.Contains(raw, "//") {
		return "", false, fmt.Errorf("absolute or non-canonical zip path rejected: %q", raw)
	}
	directory := strings.HasSuffix(raw, "/")
	name := strings.TrimSuffix(raw, "/")
	if name == "" || path.Clean(name) != name || strings.HasPrefix(name, "../") || name == ".." {
		return "", false, fmt.Errorf("path traversal rejected: %q", raw)
	}
	parts := strings.Split(name, "/")
	if len(parts) > maxDepth {
		return "", false, fmt.Errorf("path depth limit exceeded: %q", raw)
	}
	for _, component := range parts {
		if err := validateComponent(component); err != nil {
			return "", false, fmt.Errorf("unsafe path component in %q: %w", raw, err)
		}
	}
	return norm.NFC.String(name), directory, nil
}

var reserved = map[string]struct{}{
	"CON": {}, "PRN": {}, "AUX": {}, "NUL": {},
	"COM1": {}, "COM2": {}, "COM3": {}, "COM4": {}, "COM5": {}, "COM6": {}, "COM7": {}, "COM8": {}, "COM9": {},
	"LPT1": {}, "LPT2": {}, "LPT3": {}, "LPT4": {}, "LPT5": {}, "LPT6": {}, "LPT7": {}, "LPT8": {}, "LPT9": {},
}

func validateComponent(component string) error {
	if component == "" || component == "." || component == ".." {
		return errors.New("empty or dot component")
	}
	if strings.HasSuffix(component, ".") || strings.HasSuffix(component, " ") {
		return errors.New("trailing dot or space")
	}
	for _, character := range component {
		if character < 32 || strings.ContainsRune(`<>:"/\|?*`, character) {
			return errors.New("Windows-invalid character")
		}
	}
	base := strings.ToUpper(component)
	if index := strings.IndexByte(base, '.'); index >= 0 {
		base = base[:index]
	}
	if _, found := reserved[base]; found {
		return errors.New("Windows reserved name")
	}
	return nil
}
