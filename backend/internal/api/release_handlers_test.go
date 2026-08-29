package api

import (
	"path/filepath"
	"testing"

	"vltstudio/backend/internal/config"
)

func TestReleaseVersionAndArtifactValidation(t *testing.T) {
	for _, version := range []string{"0.0.1", "1.2.3", "2147483647.0.9"} {
		if _, ok := parseReleaseVersion(version); !ok {
			t.Fatalf("valid version rejected: %s", version)
		}
	}
	for _, version := range []string{"", "1", "1.2", "1.2.3.4", "01.2.3", "1.02.3", "1.2-beta", "2147483648.0.0"} {
		if _, ok := parseReleaseVersion(version); ok {
			t.Fatalf("invalid version accepted: %s", version)
		}
	}
	checks := []struct {
		kind, file string
		valid      bool
	}{
		{"windows-exe", "Setup.EXE", true}, {"macos-dmg", "VLT.dmg", true},
		{"linux-tar-gz", "vlt.tar.gz", true}, {"linux-tar-gz", "vlt.gz", false},
		{"windows-exe", "setup.dmg", false}, {"unknown", "setup.exe", false},
	}
	for _, check := range checks {
		_, valid := artifactExtension(check.kind, check.file)
		if valid != check.valid {
			t.Fatalf("artifact validation mismatch for %s/%s", check.kind, check.file)
		}
	}
}

func TestReleaseFilePathAndImageExtension(t *testing.T) {
	server := Server{Config: config.Config{StorageRoot: "storage"}}
	inside := filepath.Join("storage", "releases", "release-id", "artifact.exe")
	outside := filepath.Join("storage", "other", "artifact.exe")
	if !server.releaseFileAllowed(inside) {
		t.Fatal("relative release storage path was rejected")
	}
	if server.releaseFileAllowed(outside) {
		t.Fatal("path outside release storage was accepted")
	}
	for mimeType, want := range map[string]string{
		"image/jpeg": ".jpg", "image/png": ".png", "image/webp": ".webp",
	} {
		if got := releaseImageExtension(mimeType); got != want {
			t.Fatalf("extension for %s = %s, want %s", mimeType, got, want)
		}
	}
}
