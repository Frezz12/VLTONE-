package api

import (
	"encoding/json"
	"strings"
	"testing"
	"time"
)

func TestRateLimiterWindow(t *testing.T) {
	limiter := newRateLimiter()
	now := time.Date(2026, 8, 23, 12, 0, 0, 0, time.UTC)
	if !limiter.Allow("ip", 2, time.Hour, now) || !limiter.Allow("ip", 2, time.Hour, now.Add(time.Minute)) {
		t.Fatal("requests inside the limit were rejected")
	}
	if limiter.Allow("ip", 2, time.Hour, now.Add(2*time.Minute)) {
		t.Fatal("request above the limit was accepted")
	}
	if !limiter.Allow("ip", 2, time.Hour, now.Add(time.Hour+time.Second)) {
		t.Fatal("expired bucket entry was not released")
	}
}

func TestReporterTokenCoversOfflineEntitlement(t *testing.T) {
	if reporterTokenLifetime != 72*time.Hour {
		t.Fatalf("reporter token lifetime = %s, want 72h", reporterTokenLifetime)
	}
}

func TestHardwareSanitizerKeepsOnlyTypedAnonymousFields(t *testing.T) {
	raw := json.RawMessage(`{
		"cpu_model":"/Users/alice/private", "cpu_cores":8, "cpu_threads":16,
		"ram_bytes":17179869184, "project_name":"Secret Song",
		"gpu":[{"name":"GPU", "driver":"C:\\private\\driver", "serial":"123"}]
	}`)
	clean := string(sanitizeHardware(raw))
	for _, forbidden := range []string{"alice", "private", "project_name", "serial", "123"} {
		if strings.Contains(clean, forbidden) {
			t.Fatalf("hardware sanitizer retained %q: %s", forbidden, clean)
		}
	}
	if !strings.Contains(clean, `"cpu_cores":8`) || !strings.Contains(clean, `"name":"GPU"`) ||
		!strings.Contains(clean, "path-redacted") {
		t.Fatalf("hardware sanitizer removed valid fields: %s", clean)
	}
}

func TestCrashMetadataSanitizerRedactsPathValues(t *testing.T) {
	value := sanitizeCrashArray(json.RawMessage(`[{"name":"C:\\plugins\\secret.vst3","version":"1.0","path":"hidden"}]`), 5,
		map[string]bool{"name": true, "version": true})
	body, _ := json.Marshal(value)
	if strings.Contains(string(body), "plugins") || strings.Contains(string(body), "hidden") ||
		!strings.Contains(string(body), "path-redacted") {
		t.Fatalf("crash metadata was not sanitized: %s", body)
	}
}

func TestCrashHealthSanitizerKeepsDiagnosticContext(t *testing.T) {
	value := sanitizeCrashArray(json.RawMessage(`[{
		"recorded_at":"2026-08-23T19:07:11Z","dsp_load":18.5,"sample_rate":48000,
		"buffer_frames":512,"track_count":5,"clip_count":3,"heartbeat":330,
		"last_plugin":"Pigments","project_path":"/private/project.vlt"
	}]`), 600, map[string]bool{
		"recorded_at": true, "dsp_load": true, "sample_rate": true,
		"buffer_frames": true, "track_count": true, "clip_count": true,
		"heartbeat": true, "last_plugin": true,
	})
	body, _ := json.Marshal(value)
	for _, expected := range []string{"recorded_at", "sample_rate", "buffer_frames", "track_count", "last_plugin"} {
		if !strings.Contains(string(body), expected) {
			t.Fatalf("health metadata lost %s: %s", expected, body)
		}
	}
	if strings.Contains(string(body), "project_path") || strings.Contains(string(body), "/private") {
		t.Fatalf("health metadata retained a forbidden path: %s", body)
	}
}

func TestCrashArtifactSuffixKeepsReadableLogs(t *testing.T) {
	if got := crashArtifactSuffix("crash.log", []byte("[crash marker]\nexception=access_violation\n")); got != ".log" {
		t.Fatalf("readable crash log suffix = %q, want .log", got)
	}
	if got := crashArtifactSuffix("crash.dmp", []byte("MDMP\x00\x00")); got != ".bin" {
		t.Fatalf("legacy minidump suffix = %q, want .bin", got)
	}
}
