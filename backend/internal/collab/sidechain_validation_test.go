package collab

import (
	"encoding/json"
	"testing"

	"github.com/google/uuid"
)

func TestMultiSidechainContract(t *testing.T) {
	destination, insert, first, second := uuid.New(), uuid.New(), uuid.New(), uuid.New()
	payload := map[string]any{
		"location": extensionLocation("track", destination, uuid.Nil),
		"insertId": insert.String(), "property": "sidechainTrackIds",
		"value": []string{first.String(), second.String()},
	}
	raw, _ := json.Marshal(payload)
	if err := validateCommandPayloadShape("plugin.setProperty", raw, true); err != nil {
		t.Fatal(err)
	}
	fields, _, err := deriveCommandMetadata("plugin.setProperty", raw, true)
	if err != nil || !equalStrings(fields, []string{"plugin:" + insert.String() + ":generation", "plugin:" + insert.String() + ":sidechainTrackId"}) {
		t.Fatalf("multi-source conflict fields: %v, %v", fields, err)
	}
	steps, err := deriveLifecycleSteps("plugin.setProperty", raw, true)
	if err != nil || len(steps) != 1 || len(steps[0].Requirements) != 4 {
		t.Fatalf("all sources must be live: %#v, %v", steps, err)
	}
	policy, err := deriveCommandLeasePolicy("plugin.setProperty", raw, true)
	if err != nil || len(policy.RoutedTrackIDs) != 1 || policy.RoutedTrackIDs[0] != destination {
		t.Fatalf("multi-source changes require the routing lease: %#v, %v", policy, err)
	}
	for _, value := range []any{nil, first.String(), []string{first.String(), first.String()}, []string{""}, []string{"unknown"}, make([]string, 65)} {
		payload["value"] = value
		raw, _ = json.Marshal(payload)
		if validateCommandPayloadShape("plugin.setProperty", raw, true) == nil {
			t.Fatalf("invalid sources accepted: %v", value)
		}
	}
	payload["value"] = []string{}
	raw, _ = json.Marshal(payload)
	if err := validateCommandPayloadShape("plugin.setProperty", raw, true); err != nil {
		t.Fatalf("clearing all sources failed: %v", err)
	}
	model := extensionInsert(insert, "daw.compressor", []any{})
	model["sidechainTrackId"] = first.String()
	model["sidechainTrackIds"] = []string{first.String(), second.String()}
	addition := map[string]any{"location": payload["location"], "insert": model, "afterId": ""}
	raw, _ = json.Marshal(addition)
	if err := validateCommandPayloadShape("plugin.add", raw, true); err != nil {
		t.Fatalf("insert with two sources failed: %v", err)
	}
	steps, err = deriveLifecycleSteps("plugin.add", raw, true)
	if err != nil || len(steps) != 1 || len(steps[0].Requirements) != 4 {
		t.Fatalf("insert checks both sources: %#v, %v", steps, err)
	}
	model["sidechainTrackId"] = second.String()
	raw, _ = json.Marshal(addition)
	if validateCommandPayloadShape("plugin.add", raw, true) == nil {
		t.Fatal("inconsistent source representations accepted")
	}
}
