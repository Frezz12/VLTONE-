package collab

import (
	"bytes"
	"encoding/json"
	"github.com/google/uuid"
	"gorm.io/gorm"
	"strconv"
)

// Project-row locking in SubmitOperation serializes these immutable preparations
// with their final commit. Parts live in the retained operation log and snapshots.
func checkMidiPreparationTx(tx *gorm.DB, projectID uuid.UUID, kind string, payload json.RawMessage) error {
	if kind == "batch" || kind == "recording.commit" {
		var b struct {
			Commands []batchCommandPayload `json:"commands"`
		}
		if err := json.Unmarshal(payload, &b); err != nil {
			return err
		}
		for _, c := range b.Commands {
			if err := checkMidiPreparationTx(tx, projectID, c.Kind, c.Payload); err != nil {
				return err
			}
		}
		return nil
	}
	if kind == "recording.restoreMidi" {
		var restore struct {
			OperationID string `json:"operationId"`
			ClipID      string `json:"clipId"`
			TrackID     string `json:"trackId"`
		}
		if err := json.Unmarshal(payload, &restore); err != nil {
			return err
		}
		var source struct {
			Kind    string
			Payload json.RawMessage
		}
		if err := tx.Table("project_ops").Select("kind, payload").Where("project_id=? AND op_id=?", projectID, restore.OperationID).Take(&source).Error; err != nil {
			return invalidf("MIDI undo source is unavailable")
		}
		if !containsMidiHistory(source.Kind, source.Payload, restore.TrackID, restore.ClipID) {
			return invalidf("operation does not contain MIDI undo material")
		}
		return nil
	}
	if kind != "recording.prepareMidi" && kind != "recording.applyMidi" {
		return nil
	}
	var b struct {
		RecordingID string `json:"recordingId"`
		ContentID   string `json:"contentId"`
		Index       int    `json:"index"`
		Count       int    `json:"count"`
	}
	if err := json.Unmarshal(payload, &b); err != nil {
		return err
	}
	first, last := 0, b.Count
	if kind == "recording.prepareMidi" {
		first = b.Index
		last = first + 1
	}
	keys := make([]string, 0, last-first)
	for i := first; i < last; i++ {
		keys = append(keys, "midiContent:"+b.ContentID+":part:"+strconv.Itoa(i))
	}
	var rows []struct {
		FieldKey string
		Kind     string
		Payload  json.RawMessage
	}
	if err := tx.Table("project_field_heads AS heads").Select("heads.field_key, ops.kind, ops.payload").
		Joins("JOIN project_ops AS ops ON ops.project_id=heads.project_id AND ops.seq=heads.head_seq AND ops.op_id=heads.head_op_id").
		Where("heads.project_id=? AND heads.field_key IN ?", projectID, keys).Scan(&rows).Error; err != nil {
		return err
	}
	if kind == "recording.prepareMidi" {
		// Only resending the very same opId is idempotent; SubmitOperation has
		// already resolved that case before reaching this guard.
		if len(rows) != 0 {
			return ErrOperationIDReuse
		}
		return nil
	}
	if len(rows) != b.Count {
		return invalidf("MIDI preparation is incomplete")
	}
	contents := make([]json.RawMessage, 0, len(rows))
	for _, row := range rows {
		var part struct {
			RecordingID string          `json:"recordingId"`
			ContentID   string          `json:"contentId"`
			Index       int             `json:"index"`
			Count       int             `json:"count"`
			Content     json.RawMessage `json:"content"`
		}
		if row.Kind != "recording.prepareMidi" || json.Unmarshal(row.Payload, &part) != nil || part.RecordingID != b.RecordingID || part.ContentID != b.ContentID || part.Count != b.Count || row.FieldKey != "midiContent:"+part.ContentID+":part:"+strconv.Itoa(part.Index) {
			return ErrConflict
		}
		contents = append(contents, part.Content)
	}
	return validateAssembledMidiIdentities(contents)
}

func containsMidiHistory(kind string, payload json.RawMessage, track, clip string) bool {
	if kind == "batch" || kind == "recording.commit" {
		var b struct {
			Commands []batchCommandPayload `json:"commands"`
		}
		if json.Unmarshal(payload, &b) != nil {
			return false
		}
		for _, c := range b.Commands {
			if containsMidiHistory(c.Kind, c.Payload, track, clip) {
				return true
			}
		}
		return false
	}
	if kind != "recording.applyMidi" && kind != "recording.restoreMidi" {
		return false
	}
	var b struct {
		Track string `json:"trackId"`
		Clip  string `json:"clipId"`
	}
	return json.Unmarshal(payload, &b) == nil && b.Track == track && b.Clip == clip
}

func validateAssembledMidiIdentities(parts []json.RawMessage) error {
	ids := map[string]string{}
	metadata := map[string][]byte{}
	takes := map[string]bool{}
	compTakes := []string{}
	claim := func(id, owner string, repeated bool) error {
		if previous, exists := ids[id]; exists && (!repeated || previous != owner) {
			return invalidf("duplicate MIDI entity identity")
		}
		ids[id] = owner
		return nil
	}
	stable := func(id string, b map[string]json.RawMessage) error {
		wire, _ := json.Marshal(b)
		if old, ok := metadata[id]; ok && !bytes.Equal(old, wire) {
			return invalidf("MIDI metadata changes between parts")
		}
		metadata[id] = wire
		return nil
	}
	var content func(map[string]json.RawMessage, string) error
	content = func(b map[string]json.RawMessage, owner string) error {
		var notes, lanes []map[string]json.RawMessage
		if json.Unmarshal(b["notes"], &notes) != nil {
			return invalidf("invalid MIDI notes")
		}
		if raw, ok := b["lanes"]; ok && json.Unmarshal(raw, &lanes) != nil {
			return invalidf("invalid MIDI lanes")
		}
		for _, n := range notes {
			id, _ := payloadString(n, "id", 64, false)
			if err := claim(id, owner+":note", false); err != nil {
				return err
			}
		}
		for _, l := range lanes {
			id, _ := payloadString(l, "id", 64, false)
			if err := claim(id, owner+":lane", true); err != nil {
				return err
			}
			var points []map[string]json.RawMessage
			if json.Unmarshal(l["points"], &points) != nil {
				return invalidf("invalid MIDI points")
			}
			delete(l, "points")
			if err := stable(id, l); err != nil {
				return err
			}
			for _, p := range points {
				point, _ := payloadString(p, "id", 64, false)
				if err := claim(point, id+":point", false); err != nil {
					return err
				}
			}
		}
		return nil
	}
	for _, raw := range parts {
		b, err := commandPayloadObject(raw)
		if err != nil {
			return err
		}
		if err = content(b, "clip"); err != nil {
			return err
		}
		var ts, cs []map[string]json.RawMessage
		if json.Unmarshal(b["takes"], &ts) != nil || json.Unmarshal(b["comp"], &cs) != nil {
			return invalidf("invalid MIDI takes")
		}
		for _, t := range ts {
			id, _ := payloadString(t, "id", 64, false)
			if err = claim(id, "take", true); err != nil {
				return err
			}
			takes[id] = true
			if err = content(t, id); err != nil {
				return err
			}
			delete(t, "notes")
			delete(t, "lanes")
			if err = stable(id, t); err != nil {
				return err
			}
		}
		for _, c := range cs {
			id, _ := payloadString(c, "id", 64, false)
			if err = claim(id, "comp", false); err != nil {
				return err
			}
			take, _ := payloadString(c, "takeId", 64, false)
			compTakes = append(compTakes, take)
		}
	}
	for _, take := range compTakes {
		if !takes[take] {
			return invalidf("MIDI comp references a missing take")
		}
	}
	return nil
}
