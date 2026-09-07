package api

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestRateLimiterReclaimsUnvisitedKeys(t *testing.T) {
	limiter := newRateLimiter()
	now := time.Now()
	for i := 0; i < 2000; i++ {
		limiter.Allow(fmt.Sprint(i), 1, time.Second, now)
	}
	if !limiter.Allow("fresh", 1, time.Second, now.Add(2*time.Minute)) {
		t.Fatal("new request rejected after expiry")
	}
	if len(limiter.buckets) != 1 || len(limiter.expires) != 1 {
		t.Fatalf("expired keys retained: %d/%d", len(limiter.buckets), len(limiter.expires))
	}
}

func TestCrashArtifactStream(t *testing.T) {
	for _, tc := range []struct {
		name, suffix string
		body         []byte
	}{
		{"crash.log", ".log", bytes.Repeat([]byte("Аудио 🙂\n"), 200000)},
		{"crash.log", ".bin", []byte{0xff, 0xfe}},
		{"crash.json", ".json", []byte(`{"signal":"SIGSEGV"}`)},
		{"crash.log", ".bin", []byte{0xe2, 0x82}},
	} {
		t.Run(tc.name+tc.suffix, func(t *testing.T) {
			directory := t.TempDir()
			path, digest, size, err := storeCrashArtifact(bytes.NewReader(tc.body), tc.name, directory, 50<<20)
			if err != nil {
				t.Fatal(err)
			}
			if filepath.Ext(path) != tc.suffix || size != int64(len(tc.body)) {
				t.Fatalf("path=%s size=%d", path, size)
			}
			actual, err := os.ReadFile(path)
			if err != nil || !bytes.Equal(actual, tc.body) {
				t.Fatal("stored bytes differ", err)
			}
			expected := sha256.Sum256(tc.body)
			if digest != hex.EncodeToString(expected[:]) {
				t.Fatal("incorrect hash")
			}
		})
	}
	directory := t.TempDir()
	if _, _, _, err := storeCrashArtifact(bytes.NewReader(make([]byte, 101)), "crash.dmp", directory, 100); !errors.Is(err, errCrashArtifactTooLarge) {
		t.Fatal("oversized artifact accepted", err)
	}
	files, _ := os.ReadDir(directory)
	if len(files) != 0 {
		t.Fatal("partial artifact retained")
	}
}

func TestArtifactUTF8ChunkBoundaries(t *testing.T) {
	for _, data := range [][]byte{[]byte("a€🙂яz"), []byte("ascii"), {0xe2, 0x82, 0xff}, {0xff, 0x80}} {
		for chunk := 1; chunk < 8; chunk++ {
			inspector := artifactInspection{validUTF8: true}
			for start := 0; start < len(data); start += chunk {
				inspector.Write(data[start:min(start+chunk, len(data))])
			}
			want := bytes.Equal(data, []byte("a€🙂яz")) || bytes.Equal(data, []byte("ascii"))
			if got := inspector.validUTF8 && len(inspector.tail) == 0; got != want {
				t.Fatalf("chunk=%d input=%x valid=%v", chunk, data, got)
			}
		}
	}
}
