package api

import (
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"vltstudio/backend/internal/legal"
	"vltstudio/backend/internal/model"
)

func TestRegistrationRequiresIndependentCurrentDecisions(t *testing.T) {
	version := legal.Current.Version
	valid := registerRequest{ConsentAccepted: true, ConsentVersion: version, TermsAccepted: true, TermsVersion: version}
	cases := []struct {
		name   string
		change func(*registerRequest)
		field  string
	}{
		{"diagnostics optional", func(r *registerRequest) {}, ""},
		{"account unchecked", func(r *registerRequest) { r.ConsentAccepted = false }, "consent_accepted"},
		{"old account consent", func(r *registerRequest) { r.ConsentVersion = "2026-08-23" }, "consent_accepted"},
		{"terms unchecked", func(r *registerRequest) { r.TermsAccepted = false }, "terms_accepted"},
		{"old terms", func(r *registerRequest) { r.TermsVersion = "old" }, "terms_accepted"},
		{"diagnostics explicitly selected", func(r *registerRequest) { r.DiagnosticsAccepted = true; r.DiagnosticsVersion = version }, ""},
		{"stale diagnostics", func(r *registerRequest) { r.DiagnosticsAccepted = true; r.DiagnosticsVersion = "old" }, "diagnostics_accepted"},
	}
	for _, tt := range cases {
		t.Run(tt.name, func(t *testing.T) {
			input := valid
			tt.change(&input)
			fields := validateRegistrationLegal(input, version)
			if tt.field == "" && len(fields) != 0 {
				t.Fatalf("unexpected rejection: %v", fields)
			}
			if tt.field != "" && fields[tt.field] == "" {
				t.Fatalf("missing %s error: %v", tt.field, fields)
			}
		})
	}
}

func TestLegacyOrWithdrawnDiagnosticsAreNotOptedIn(t *testing.T) {
	now := time.Now()
	user := model.User{ConsentVersion: "2026-08-23", ConsentAcceptedAt: now}
	if diagnosticsAllowed(user) {
		t.Fatal("legacy bundled consent opted user in")
	}
	user.DiagnosticsConsentVersion = legal.Current.Version
	if diagnosticsAllowed(user) {
		t.Fatal("a revision without affirmative timestamp opted user in")
	}
	user.DiagnosticsAcceptedAt = &now
	if !diagnosticsAllowed(user) {
		t.Fatal("explicit current consent rejected")
	}
	user.DiagnosticsRevokedAt = &now
	if diagnosticsAllowed(user) {
		t.Fatal("withdrawal was ignored")
	}
}

func TestIncompleteLegalProfileRejectsRegistrationBeforeStorage(t *testing.T) {
	server := &Server{}
	recorder := httptest.NewRecorder()
	server.register(recorder, httptest.NewRequest(http.MethodPost, "/register", strings.NewReader(`{}`)))
	if recorder.Code != http.StatusServiceUnavailable || !strings.Contains(recorder.Body.String(), "legal_documents_pending") {
		t.Fatalf("unexpected response: %d %s", recorder.Code, recorder.Body.String())
	}
}

func TestDiagnosticArtifactCleanupStaysInsideStorage(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "crashes", "report-123")
	if err := os.MkdirAll(dir, 0700); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "crash.log")
	if err := os.WriteFile(path, []byte("fixture"), 0600); err != nil {
		t.Fatal(err)
	}
	outside := filepath.Join(root, "keep.txt")
	if err := os.WriteFile(outside, []byte("keep"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := removeDiagnosticArtifact(root, outside); err == nil {
		t.Fatal("allowed outside artifact")
	}
	if err := removeDiagnosticArtifact(root, dir); err == nil {
		t.Fatal("allowed directory deletion")
	}
	if err := removeDiagnosticArtifact(root, path); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Fatalf("artifact not removed: %v", err)
	}
	if err := removeDiagnosticArtifact(root, path); err != nil {
		t.Fatalf("retry not idempotent: %v", err)
	}
	if _, err := os.Stat(outside); err != nil {
		t.Fatal("unrelated file changed")
	}
	external := t.TempDir()
	linked := filepath.Join(root, "crashes", "linked")
	if err := os.Symlink(external, linked); err != nil {
		t.Skip(err)
	}
	escaped := filepath.Join(external, "private.log")
	if err := os.WriteFile(escaped, []byte("keep"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := removeDiagnosticArtifact(root, filepath.Join(linked, "private.log")); err == nil {
		t.Fatal("allowed parent symlink escape")
	}
}
