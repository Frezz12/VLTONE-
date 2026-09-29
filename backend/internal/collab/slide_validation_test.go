package collab

import (
	"encoding/json"
	"testing"
)

func TestSlideV5Contract(t *testing.T) {
	const id = "11111111-1111-4111-8111-111111111111"
	slide := map[string]any{"id": id, "startBeats": 1, "lengthBeats": 2, "referenceNoteId": id, "targetNoteIds": []string{id}, "chord": false, "muted": false, "points": []any{map[string]any{"time": 0, "pitch": 60, "shape": 0, "curve": 0}, map[string]any{"time": 1, "pitch": 72, "shape": 1, "curve": 0}}}
	payload := map[string]any{"trackId": id, "clipId": id, "takeId": "", "slideId": id, "slide": slide}
	raw, _ := json.Marshal(payload)
	if err := validateCommandPayloadShapeForSchema("slide.set", raw, true, 5); err != nil {
		t.Fatal(err)
	}
	if err := validateCommandPayloadShapeForSchema("slide.set", raw, true, 4); err == nil {
		t.Fatal("legacy session accepted slide command")
	}
	slide["targetNoteIds"] = []string{id, id}
	raw, _ = json.Marshal(slide)
	if validateSlidePayload(raw) == nil {
		t.Fatal("duplicate targets accepted")
	}
	slide["targetNoteIds"] = []string{id}
	slide["lengthBeats"] = 0
	raw, _ = json.Marshal(slide)
	if validateSlidePayload(raw) == nil {
		t.Fatal("empty slide accepted")
	}
	slide["lengthBeats"] = 2
	slide["resumed"] = true
	raw, _ = json.Marshal(slide)
	if err := validateSlidePayload(raw); err != nil {
		t.Fatal(err)
	}
	payload["slide"] = nil
	raw, _ = json.Marshal(payload)
	if err := validateCommandPayloadShapeForSchema("slide.set", raw, true, 5); err != nil {
		t.Fatal(err)
	}
}

func TestLegacyTakeCannotSmuggleSlideData(t *testing.T) {
	const id = "11111111-1111-4111-8111-111111111111"
	body := map[string]any{"trackId": id, "clipId": id, "afterId": "", "take": map[string]any{"slideNotes": []any{map[string]any{"id": id}}}}
	raw, _ := json.Marshal(body)
	if validateCommandPayloadShapeForSchema("take.add", raw, true, 4) == nil {
		t.Fatal("protocol 4 accepted slide data through take.add")
	}
}
