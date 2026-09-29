package collab

import (
	"encoding/json"
	"testing"

	"github.com/google/uuid"
)

func TestMasterAutomationTargets(t *testing.T) {
	trackID, clipID, slotID := uuid.NewString(), uuid.NewString(), uuid.NewString()
	for _, test := range []struct {
		name, channel, kind, slot, parameter, send string
		valid                                      bool
	}{
		{"volume", "master", "volume", "", "", "", true},
		{"pan", "master", "pan", "", "", "", true},
		{"mute", "master", "mute", "", "", "", true},
		{"insert", "master", "parameter", slotID, "mix", "", true},
		{"track", trackID, "volume", "", "", "", true},
		{"invalid channel", "unknown", "volume", "", "", "", false},
		{"master send", "master", "send", "", "", uuid.NewString(), false},
		{"master instrument", "master", "parameter", "", "mix", "", false},
	} {
		t.Run(test.name, func(t *testing.T) {
			payload, err := json.Marshal(map[string]any{
				"trackId": trackID, "clipId": clipID,
				"target": map[string]any{
					"kind": test.kind, "channelId": test.channel, "slotId": test.slot,
					"parameterId": test.parameter, "sendId": test.send,
				},
			})
			if err != nil {
				t.Fatal(err)
			}
			steps, err := deriveLifecycleSteps("automation.setTarget", payload, true)
			if !test.valid {
				if err == nil {
					t.Fatal("invalid automation target accepted")
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			if len(steps) != 1 {
				t.Fatalf("lifecycle steps = %d", len(steps))
			}
			for _, requirement := range steps[0].Requirements {
				if requirement.FieldKey == "track:master:lifecycle" {
					t.Fatal("master automation requires a nonexistent project track")
				}
			}
			if test.channel == "master" && len(steps[0].Requirements) != 2 {
				t.Fatal("master automation must retain its lane and clip lifecycle checks")
			}
		})
	}
}
