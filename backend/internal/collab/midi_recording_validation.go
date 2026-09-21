package collab

import (
	"encoding/json"
	"math"
)

func validateMidiRecordingPayload(kind string, body map[string]json.RawMessage) error {
	if kind == "recording.prepareMidi" {
		if err := exactPayloadKeys(body, []string{"recordingId", "contentId", "index", "count", "content"}, nil); err != nil {
			return err
		}
		for _, key := range []string{"recordingId", "contentId"} {
			if _, err := requiredPayloadUUID(body, key); err != nil {
				return err
			}
		}
		count, err := payloadInteger(body, "count", 1, 1024)
		if err != nil {
			return err
		}
		if _, err = payloadInteger(body, "index", 0, count-1); err != nil {
			return err
		}
		return validateMidiContentPayload(body["content"])
	}
	if kind == "recording.applyMidi" {
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "recordingId", "contentId", "count"}, nil); err != nil {
			return err
		}
		for _, key := range []string{"trackId", "clipId", "recordingId", "contentId"} {
			if _, err := requiredPayloadUUID(body, key); err != nil {
				return err
			}
		}
		_, err := payloadInteger(body, "count", 1, 1024)
		return err
	}
	if err := exactPayloadKeys(body, []string{"trackId", "clipId", "operationId"}, nil); err != nil {
		return err
	}
	for _, key := range []string{"trackId", "clipId", "operationId"} {
		if _, err := requiredPayloadUUID(body, key); err != nil {
			return err
		}
	}
	return nil
}
func midiArray(raw json.RawMessage, validate func(json.RawMessage) error) error {
	var values []json.RawMessage
	if len(raw) == 0 || string(raw) == "null" || json.Unmarshal(raw, &values) != nil {
		return invalidf("MIDI content must be an array")
	}
	if len(values) > 4096 {
		return invalidf("MIDI part contains too many entries")
	}
	for _, v := range values {
		if err := validate(v); err != nil {
			return err
		}
	}
	return nil
}
func validateMidiLane(raw json.RawMessage) error {
	b, err := commandPayloadObject(raw)
	if err != nil {
		return err
	}
	if err = exactPayloadKeys(b, []string{"id", "name", "cc", "channel", "key", "parameterId", "slotId", "defaultValue", "points"}, nil); err != nil {
		return err
	}
	if _, err = requiredPayloadUUID(b, "id"); err != nil {
		return err
	}
	if _, err = payloadString(b, "name", 4096, true); err != nil {
		return err
	}
	if _, err = payloadNumber(b, "defaultValue", 0, 1, false); err != nil {
		return err
	}
	target := map[string]json.RawMessage{}
	for _, key := range []string{"cc", "channel", "key", "parameterId", "slotId"} {
		target[key] = b[key]
	}
	encoded, _ := json.Marshal(target)
	if err = validateControllerLaneTarget(encoded); err != nil {
		return err
	}
	return midiArray(b["points"], validateAutomationPointPayload)
}
func validateMidiTake(raw json.RawMessage) error {
	b, err := commandPayloadObject(raw)
	if err != nil {
		return err
	}
	if err = exactPayloadKeys(b, []string{"id", "name", "offsetSeconds", "lengthSeconds", "clipOffsetSeconds", "gain", "muted", "channels", "color", "notes"}, []string{"lanes"}); err != nil {
		return err
	}
	if _, err = requiredPayloadUUID(b, "id"); err != nil {
		return err
	}
	if _, err = payloadString(b, "name", 4096, true); err != nil {
		return err
	}
	for _, key := range []string{"offsetSeconds", "lengthSeconds", "clipOffsetSeconds"} {
		if _, err = payloadNumber(b, key, 0, math.MaxFloat64, false); err != nil {
			return err
		}
	}
	if _, err = payloadNumber(b, "gain", 0, 4, false); err != nil {
		return err
	}
	if _, err = payloadBool(b, "muted"); err != nil {
		return err
	}
	if _, err = payloadInteger(b, "channels", 0, 1024); err != nil {
		return err
	}
	if _, err = payloadInteger(b, "color", 0, math.MaxUint32); err != nil {
		return err
	}
	if err = midiArray(b["notes"], validateNotePayload); err != nil {
		return err
	}
	if lanes, ok := b["lanes"]; ok {
		return midiArray(lanes, validateMidiLane)
	}
	return nil
}
func validateMidiContentPayload(raw json.RawMessage) error {
	b, err := commandPayloadObject(raw)
	if err != nil {
		return err
	}
	if err = exactPayloadKeys(b, []string{"notes", "lanes", "takes", "comp", "expanded"}, nil); err != nil {
		return err
	}
	if _, err = payloadBool(b, "expanded"); err != nil {
		return err
	}
	for key, validate := range map[string]func(json.RawMessage) error{"notes": validateNotePayload, "lanes": validateMidiLane, "takes": validateMidiTake, "comp": validateMidiComp} {
		if err = midiArray(b[key], validate); err != nil {
			return err
		}
	}
	return nil
}
func validateMidiComp(raw json.RawMessage) error {
	b, err := commandPayloadObject(raw)
	if err != nil {
		return err
	}
	if err = exactPayloadKeys(b, []string{"id", "takeId", "startSeconds", "endSeconds"}, nil); err != nil {
		return err
	}
	for _, key := range []string{"id", "takeId"} {
		if _, err = requiredPayloadUUID(b, key); err != nil {
			return err
		}
	}
	start, err := payloadNumber(b, "startSeconds", 0, math.MaxFloat64, false)
	if err != nil {
		return err
	}
	end, err := payloadNumber(b, "endSeconds", 0, math.MaxFloat64, false)
	if err != nil {
		return err
	}
	if end <= start {
		return invalidf("MIDI comp range is empty")
	}
	return nil
}
