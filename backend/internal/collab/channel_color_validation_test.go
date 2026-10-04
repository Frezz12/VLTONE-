package collab

import (
	"encoding/json"
	"github.com/google/uuid"
	"testing"
)

func TestChannelColorFixedStageContract(t *testing.T) {
	trackID := uuid.New()
	insertID := uuid.MustParse(channelColorSlotID(trackID.String()))
	location := extensionLocation("channelColor", trackID, uuid.Nil)
	color := func() map[string]any {
		insert := extensionInsert(insertID, "daw.channel-color", []any{})
		insert["profileSeed"] = "123456789abcdef0"
		insert["parameters"] = []any{map[string]any{"id": "drive", "value": -20.0}, map[string]any{"id": "tone", "value": 0.0}}
		return insert
	}
	validate := func(kind string, payload map[string]any, version int, want bool) {
		t.Helper()
		raw, err := json.Marshal(payload)
		if err != nil {
			t.Fatal(err)
		}
		err = validateCommandPayloadShapeForSchema(kind, raw, true, version)
		if (err == nil) != want {
			t.Fatalf("%s v%d valid=%v, wanted %v: %v", kind, version, err == nil, want, err)
		}
	}
	add := func(insert map[string]any, chain string) map[string]any {
		return map[string]any{"location": extensionLocation(chain, trackID, uuid.Nil), "insert": insert, "afterId": ""}
	}
	validate("plugin.add", add(color(), "channelColor"), 6, true)
	validate("plugin.add", add(color(), "channelColor"), 5, false)
	validate("plugin.add", add(color(), "track"), 6, false)
	validate("plugin.add", add(extensionInsert(insertID, "daw.cla2a", []any{}), "channelColor"), 6, false)
	for _, field := range []string{"profileSeed", "channelMode", "mix", "parameters", "assetBindings"} {
		insert := color()
		switch field {
		case "profileSeed":
			insert[field] = "NOT-A-PROFILE"
		case "channelMode":
			insert[field] = "dualMono"
		case "mix":
			insert[field] = .5
		case "parameters":
			insert[field] = []any{map[string]any{"id": "drive", "value": 101.0}}
		case "assetBindings":
			insert[field] = []any{map[string]any{"key": "sample", "asset": extensionAsset(uuid.New(), "audio", "a"), "required": true}}
		}
		validate("plugin.add", add(insert, "channelColor"), 6, false)
	}
	scalar := func(parameter string, value float64, right bool) map[string]any {
		return map[string]any{"location": location, "insertId": insertID.String(), "parameterId": parameter, "value": value, "rightChannel": right}
	}
	validate("plugin.setParameter", scalar("drive", -100, false), 6, true)
	validate("plugin.setParameter", scalar("tone", 100, false), 6, true)
	validate("plugin.setParameter", scalar("drive", 101, false), 6, false)
	validate("plugin.setParameter", scalar("drive", 10, true), 6, false)
	validate("plugin.setParameter", scalar("mix", 0, false), 6, false)
	validate("plugin.setParameter", scalar("drive", 0, false), 5, false)
	property := map[string]any{"location": location, "insertId": insertID.String(), "property": "bypassed", "value": true}
	validate("plugin.setProperty", property, 6, true)
	property["property"] = "mix"
	property["value"] = .5
	validate("plugin.setProperty", property, 6, false)
	lifecycle := map[string]any{"location": location, "insertId": insertID.String()}
	validate("plugin.delete", lifecycle, 6, true)
	lifecycle["deleteOperationId"] = uuid.New().String()
	validate("plugin.restore", lifecycle, 6, true)
	move := map[string]any{"location": location, "insertId": insertID.String(), "afterId": uuid.New().String()}
	validate("plugin.move", move, 6, false)
}
