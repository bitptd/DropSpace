package zipcheck

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"testing"
)

type zipItem struct {
	name string
	body []byte
	mode os.FileMode
}

func makeZip(t *testing.T, items []zipItem) string {
	t.Helper()
	filename := filepath.Join(t.TempDir(), "object.zip")
	output, err := os.Create(filename)
	if err != nil {
		t.Fatal(err)
	}
	writer := zip.NewWriter(output)
	for _, item := range items {
		header := &zip.FileHeader{Name: item.name, Method: zip.Deflate}
		if item.mode != 0 {
			header.SetMode(item.mode)
		}
		entry, err := writer.CreateHeader(header)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = entry.Write(item.body); err != nil {
			t.Fatal(err)
		}
	}
	if err = writer.Close(); err != nil {
		t.Fatal(err)
	}
	if err = output.Close(); err != nil {
		t.Fatal(err)
	}
	return filename
}

func expectedFor(items []zipItem) Expected {
	var expanded int64
	records := make([]treeRecord, 0, len(items))
	for _, item := range items {
		name := item.name
		if len(name) > 0 && name[len(name)-1] == '/' {
			records = append(records, treeRecord{path: name[:len(name)-1], kind: 'D'})
			continue
		}
		expanded += int64(len(item.body))
		hash := sha256Bytes(item.body)
		records = append(records, treeRecord{path: name, kind: 'F', size: int64(len(item.body)), hash: hash})
	}
	return Expected{ExpandedBytes: expanded, EntryCount: int64(len(items)), TreeSHA256: treeDigest(records)}
}

func sha256Bytes(value []byte) string {
	file := makeZipDigest(value)
	return file
}

func makeZipDigest(value []byte) string {
	// Kept local so test expectations use the documented record contract but
	// not the validator's stream-reading implementation.
	digest := sha256.Sum256(value)
	return hex.EncodeToString(digest[:])
}

func TestValidateFolder(t *testing.T) {
	items := []zipItem{
		{name: "empty/", mode: os.ModeDir | 0755},
		{name: "nested/", mode: os.ModeDir | 0755},
		{name: "nested/Unicode-文件.txt", body: []byte("payload"), mode: 0644},
	}
	filename := makeZip(t, items)
	result, err := Validate(filename, expectedFor(items), DefaultLimits())
	if err != nil {
		t.Fatal(err)
	}
	if result.ExpandedBytes != 7 || result.EntryCount != 3 {
		t.Fatalf("unexpected result: %+v", result)
	}
}

func TestRejectsUnsafeArchives(t *testing.T) {
	tests := []struct {
		name  string
		items []zipItem
	}{
		{"traversal", []zipItem{{name: "../escape.txt", body: []byte("x"), mode: 0644}}},
		{"absolute", []zipItem{{name: "/escape.txt", body: []byte("x"), mode: 0644}}},
		{"backslash", []zipItem{{name: `folder\\escape.txt`, body: []byte("x"), mode: 0644}}},
		{"reserved", []zipItem{{name: "CON.txt", body: []byte("x"), mode: 0644}}},
		{"case conflict", []zipItem{{name: "A.txt", body: []byte("a"), mode: 0644}, {name: "a.txt", body: []byte("b"), mode: 0644}}},
		{"file as parent", []zipItem{{name: "parent", body: []byte("a"), mode: 0644}, {name: "parent/child", body: []byte("b"), mode: 0644}}},
		{"file after child", []zipItem{{name: "parent/child", body: []byte("b"), mode: 0644}, {name: "parent", body: []byte("a"), mode: 0644}}},
		{"symlink", []zipItem{{name: "link", body: []byte("target"), mode: os.ModeSymlink | 0777}}},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			filename := makeZip(t, test.items)
			if _, err := Validate(filename, expectedFor(test.items), DefaultLimits()); err == nil {
				t.Fatal("unsafe archive was accepted")
			}
		})
	}
}

func TestRejectsDeclaredMetadataMismatch(t *testing.T) {
	items := []zipItem{{name: "file.txt", body: bytes.Repeat([]byte{'x'}, 1024), mode: 0644}}
	expected := expectedFor(items)
	expected.ExpandedBytes++
	if _, err := Validate(makeZip(t, items), expected, DefaultLimits()); err == nil {
		t.Fatal("forged expanded size was accepted")
	}
}
