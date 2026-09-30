package model

import (
	"encoding/json"
	"testing"
	"time"

	"github.com/google/uuid"
)

func TestInviteJSONMatchesDesktopContract(t *testing.T) {
	lookup, email := "private-lookup", "private-email"
	until := time.Now()
	value := ProjectInvite{ID: uuid.New(), ProjectID: uuid.New(), Role: ProjectRoleEditor,
		CodeDigits: 12, CodeLookup: &lookup, TokenHash: "private-token-hash", TargetEmailKey: &email,
		AttemptCount: 3, LockedUntil: &until, CreatedAt: until, ExpiresAt: until.Add(time.Hour)}
	encoded, err := json.Marshal(value)
	if err != nil {
		t.Fatal(err)
	}
	var fields map[string]json.RawMessage
	if err := json.Unmarshal(encoded, &fields); err != nil {
		t.Fatal(err)
	}
	for _, key := range []string{"id", "project_id", "role", "created_at", "expires_at"} {
		if _, ok := fields[key]; !ok {
			t.Fatalf("missing public invitation field %s", key)
		}
		delete(fields, key)
	}
	if len(fields) != 0 {
		t.Fatalf("database-only invitation fields leaked to the strict desktop parser: %v", fields)
	}
}
