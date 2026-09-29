package collab

import (
	"encoding/json"
)

func validateSlidePayload(raw json.RawMessage) error {
	body, err := commandPayloadObject(raw)
	if err != nil {
		return err
	}
	if err := exactPayloadKeys(body, []string{"id", "startBeats", "lengthBeats", "referenceNoteId", "targetNoteIds", "chord", "muted", "points"}, []string{"resumed"}); err != nil {
		return err
	}
	if _, err := requiredPayloadUUID(body, "id"); err != nil {
		return err
	}
	if err := optionalPayloadUUIDIfPresent(body, "referenceNoteId"); err != nil {
		return err
	}
	if _, err := payloadNumber(body, "startBeats", 0, 1e100, false); err != nil {
		return err
	}
	if _, err := payloadNumber(body, "lengthBeats", 0, 1e100, true); err != nil {
		return err
	}
	if _, ok := body["resumed"]; ok {
		if _, err := payloadBool(body, "resumed"); err != nil {
			return err
		}
	}
	if _, err := payloadBool(body, "chord"); err != nil {
		return err
	}
	if _, err := payloadBool(body, "muted"); err != nil {
		return err
	}
	var ids []json.RawMessage
	if err := json.Unmarshal(body["targetNoteIds"], &ids); err != nil || ids == nil || len(ids) > 128 {
		return invalidf("invalid slide targets")
	}
	seen := map[string]bool{}
	for _, id := range ids {
		holder := map[string]json.RawMessage{"id": id}
		value, err := requiredPayloadUUID(holder, "id")
		if err != nil {
			return err
		}
		if seen[value] {
			return invalidf("duplicate slide target")
		}
		seen[value] = true
	}
	var points []json.RawMessage
	if err := json.Unmarshal(body["points"], &points); err != nil || len(points) < 2 || len(points) > 256 {
		return invalidf("invalid slide points")
	}
	last := -1.0
	for i, raw := range points {
		p, err := commandPayloadObject(raw)
		if err != nil {
			return err
		}
		if err = exactPayloadKeys(p, []string{"time", "pitch", "shape", "curve"}, nil); err != nil {
			return err
		}
		time, err := payloadNumber(p, "time", 0, 1, false)
		if err != nil {
			return err
		}
		if time <= last || (i == 0 && time != 0) || (i == len(points)-1 && time != 1) {
			return invalidf("invalid slide point time")
		}
		last = time
		if _, err = payloadNumber(p, "pitch", 0, 127, false); err != nil {
			return err
		}
		if _, err = payloadNumber(p, "curve", -1, 1, false); err != nil {
			return err
		}
		if _, err = payloadInteger(p, "shape", 0, 2); err != nil {
			return err
		}
	}
	return nil
}

// Called only on validated JSON containers; the full validators still check every field.
func midiContentHasSlides(raw json.RawMessage) bool {
	var content struct {
		Slides []json.RawMessage `json:"slideNotes"`
		Takes  []json.RawMessage `json:"takes"`
	}
	if json.Unmarshal(raw, &content) != nil {
		return false
	}
	if len(content.Slides) > 0 {
		return true
	}
	for _, take := range content.Takes {
		if midiContentHasSlides(take) {
			return true
		}
	}
	return false
}
