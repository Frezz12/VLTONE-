package api

import (
	"net/http"
	"sync"
	"testing"

	"github.com/google/uuid"
	"vltstudio/backend/internal/auth"
	"vltstudio/backend/internal/model"
)

func checkDesktopRefreshRetry(t *testing.T, server *Server, router http.Handler, token string) {
	refresh := func(token, id string) testResponse {
		input := map[string]any{"refresh_token": token, "app_version": "test"}
		headers := map[string]string{}
		if id != "" {
			headers["Idempotency-Key"] = id
		}
		return performJSON(router, "POST", "/v1/desktop/auth/refresh", input, "203.0.113.65:1234", nil, headers)
	}
	legacy := refresh(token, "")
	if legacy.Status != 200 {
		t.Fatalf("legacy refresh: %d", legacy.Status)
	}
	token = legacy.Body["refresh_token"].(string)
	id := uuid.NewString()
	// Reproduce a server failure AFTER the rotation commits but before its
	// response can be delivered (quota lookup is outside that transaction).
	if err := server.DB.Exec("ALTER TABLE token_cycles RENAME TO auth_test_token_cycles").Error; err != nil {
		t.Fatal(err)
	}
	restored := false
	t.Cleanup(func() {
		if !restored {
			server.DB.Exec("ALTER TABLE auth_test_token_cycles RENAME TO token_cycles")
		}
	})
	lost := refresh(token, id)
	if err := server.DB.Exec("ALTER TABLE auth_test_token_cycles RENAME TO token_cycles").Error; err != nil {
		t.Fatal(err)
	}
	restored = true
	if lost.Status != 500 || lost.Body["code"] != "quota_unavailable" {
		t.Fatalf("post-commit failure fixture: %d", lost.Status)
	}
	results := make([]testResponse, 2)
	var workers sync.WaitGroup
	for i := range results {
		workers.Add(1)
		go func(i int) { defer workers.Done(); results[i] = refresh(token, id) }(i)
	}
	workers.Wait()
	for _, result := range results {
		if result.Status != 200 {
			t.Fatalf("concurrent retry: %d %v", result.Status, result.Body["code"])
		}
	}
	next := results[0].Body["refresh_token"].(string)
	if next == token || results[1].Body["refresh_token"] != next || results[1].Body["reporter_token"] != results[0].Body["reporter_token"] {
		t.Fatal("same request rotated twice")
	}
	var receipt model.DesktopSession
	if err := server.DB.Where("refresh_token_hash = ?", auth.HashToken(token)).First(&receipt).Error; err != nil {
		t.Fatal(err)
	}
	if receipt.RotatedToID == nil || receipt.RefreshRequestHash != auth.HashToken(id) {
		t.Fatal("rotation receipt missing")
	}
	retry := refresh(token, id)
	if retry.Status != 200 || retry.Body["refresh_token"] != next {
		t.Fatal("lost-response retry did not recover credential")
	}
	newer := refresh(next, uuid.NewString())
	if newer.Status != 200 {
		t.Fatalf("successor cannot rotate: %d", newer.Status)
	}
	if stale := refresh(token, id); stale.Status != 401 || stale.Body["code"] != "refresh_token_invalid" {
		t.Fatal("old receipt revived a rotated successor")
	}
	// A stale matching retry must not revoke the healthy latest session.
	newest := refresh(newer.Body["refresh_token"].(string), uuid.NewString())
	if newest.Status != 200 {
		t.Fatal("stale retry revoked healthy session")
	}
	// Different/missing operation IDs keep the existing reuse protection.
	if reused := refresh(token, uuid.NewString()); reused.Status != 401 || reused.Body["code"] != "refresh_token_reused" {
		t.Fatal("different intent bypassed reuse protection")
	}
	if revoked := refresh(newest.Body["refresh_token"].(string), uuid.NewString()); revoked.Status != 401 {
		t.Fatal("reuse did not revoke account sessions")
	}
}
