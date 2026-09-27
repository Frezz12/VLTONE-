package collab

import (
	"encoding/json"
	"errors"
	"strings"
	"testing"

	"github.com/google/uuid"
)

func TestTrackIconIdentifiers(t *testing.T) {
	trackID := uuid.NewString()
	for _, version := range []int{2, 3} {
		for _, value := range []any{"", "builtin:drum-kit", "custom:" + strings.Repeat("a", 64), "builtin:future-instrument", "../../icon.png", "C:\\icon.png", "https://example.com/icon", strings.Repeat("a", 97), "piano\n", 42} {
			payload, err := json.Marshal(map[string]any{"trackId": trackID, "property": "iconId", "value": value})
			if err != nil {
				t.Fatal(err)
			}
			text, isText := value.(string)
			valid := isText && (text == "" || strings.HasPrefix(text, "builtin:") || strings.HasPrefix(text, "custom:"))
			err = validateCommandPayloadShapeForSchema("track.setProperty", payload, true, version)
			if !valid {
				if !errors.Is(err, ErrValidation) {
					t.Fatalf("v%d accepted invalid icon identifier %q: %v", version, value, err)
				}
				continue
			}
			if err != nil {
				t.Fatalf("v%d rejected icon identifier %q: %v", version, value, err)
			}
			fields, _, err := deriveCommandMetadata("track.setProperty", payload, true)
			if err != nil || !equalStrings(fields, []string{"track:" + trackID + ":iconId"}) {
				t.Fatalf("icon change metadata = %v, %v", fields, err)
			}
		}
	}
}
