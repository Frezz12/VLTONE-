package api

import (
	"encoding/json"
	"testing"
)

func TestSubmissionSessionVersionCompatibility(t *testing.T) {
	for _, test := range []struct {
		name, payload  string
		envelope, want int64
		invalid        bool
	}{
		{"current desktop", `{"command":{}}`, 7, 7, false},
		{"first 0.3.2 desktop", `{"command":{},"sessionVersion":7}`, 0, 7, false},
		{"matching duplicate", `{"command":{},"sessionVersion":7}`, 7, 7, false},
		{"conflicting revisions", `{"command":{},"sessionVersion":6}`, 7, 0, true},
		{"negative payload", `{"command":{},"sessionVersion":-1}`, 0, 0, true},
		{"negative envelope", `{"command":{}}`, -1, 0, true},
		{"old protocol", `{"command":{}}`, 0, 0, false},
	} {
		t.Run(test.name, func(t *testing.T) {
			var request collaborationOpSubmit
			if err := decodeCollaborationJSON(json.RawMessage(test.payload), &request); err != nil {
				t.Fatal(err)
			}
			got, err := request.sessionVersion(test.envelope)
			if (err != nil) != test.invalid || got != test.want {
				t.Fatalf("version=%d, err=%v", got, err)
			}
		})
	}
}
