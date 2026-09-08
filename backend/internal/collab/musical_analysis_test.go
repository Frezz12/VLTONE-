package collab

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestMusicalAnalysisPayload(t *testing.T) {
	legacy := `{"version":1,"offsetSeconds":0,"durationSeconds":8,"tempo":{"status":2,"bpm":127.86,"confidence":0.99,"stability":1,"alternatives":[64],"variable":false},"key":{"status":1,"root":0,"scale":"major","confidence":0.8,"alternateRoot":9,"alternateScale":"natural_minor","tuningCents":0}}`
	if err := validateMusicalAnalysis(json.RawMessage(legacy)); err != nil {
		t.Fatalf("legacy analysis must remain valid: %v", err)
	}
	tests := []struct {
		name  string
		field string
		value any
		valid bool
	}{
		{"version", "algorithmVersion", 2, true},
		{"calibration", "calibrated", false, true},
		{"backend", "backend", "beat-this+grid", true},
		{"reason", "reason", "tempo is ambiguous", true},
		{"variable", "variable", true, true},
		{"bad version", "algorithmVersion", -1, false},
		{"bad calibration", "calibrated", "yes", false},
		{"bad variable", "variable", 1, false},
		{"long backend", "backend", strings.Repeat("x", 129), false},
		{"long reason", "reason", strings.Repeat("x", 513), false},
		{"unknown field", "futureField", true, false},
	}
	for _, section := range []string{"tempo", "key"} {
		for _, test := range tests {
			t.Run(section+"/"+test.name, func(t *testing.T) {
				var body map[string]any
				if err := json.Unmarshal([]byte(legacy), &body); err != nil {
					t.Fatal(err)
				}
				body[section].(map[string]any)[test.field] = test.value
				raw, err := json.Marshal(body)
				if err != nil {
					t.Fatal(err)
				}
				if err := validateMusicalAnalysis(raw); (err == nil) != test.valid {
					t.Fatalf("valid=%t: %v", test.valid, err)
				}
			})
		}
	}
	// Source measurements can exceed the project's tempo range after stretch.
	// They must survive collaboration even when they cannot be auto-applied.
	transformed := strings.Replace(legacy, `"bpm":127.86`, `"bpm":511.44`, 1)
	transformed = strings.Replace(transformed, `"alternatives":[64]`, `"alternatives":[1022.88]`, 1)
	if err := validateMusicalAnalysis(json.RawMessage(transformed)); err != nil {
		t.Fatalf("fractional transformed analysis must remain valid: %v", err)
	}
}
