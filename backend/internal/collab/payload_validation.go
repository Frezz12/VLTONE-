package collab

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"math"
	"math/big"
	"regexp"
	"sort"
	"strings"
	"unicode/utf8"

	"github.com/google/uuid"
)

var lowercaseSHA256Pattern = regexp.MustCompile(`^[0-9a-f]{64}$`)

// Field heads include the parameter id. Keeping this comfortably below the
// 512-character field-key ceiling makes every accepted command representable
// by both PostgreSQL and the desktop snapshot codec.
const (
	maximumPluginParameterIDBytes = 400
	maximumSendLevel              = 1.9953 // +6 dB, desktop parity
)

// validateCommandPayloadShape is the server-side copy of the locked v2
// payload schema. Committing a command that the desktop cannot decode/apply
// would permanently diverge replicas, so shape and reducer-level scalar
// bounds are checked before sequence allocation.
func validateCommandPayloadShape(kind string, payload json.RawMessage, allowBatch bool) error {
	return validateCommandPayloadShapeForSchema(kind, payload, allowBatch,
		CollaborationCommandSchemaVersion)
}

func validateCommandPayloadShapeForSchema(kind string, payload json.RawMessage,
	allowBatch bool, schemaVersion int) error {
	body, err := commandPayloadObject(payload)
	if err != nil {
		return err
	}
	requireIDs := func(names ...string) error {
		for _, name := range names {
			if _, err := requiredPayloadUUID(body, name); err != nil {
				return err
			}
		}
		return nil
	}

	switch kind {
	case "project.setScalar":
		if err := exactPayloadKeys(body, []string{"field", "value"}, nil); err != nil {
			return err
		}
		field, err := payloadEnum(body, "field", "name", "tempo", "aiInstructions", "renderSampleRate", "masterVolume", "masterPan")
		if err != nil {
			return err
		}
		switch field {
		case "name", "aiInstructions":
			_, err = payloadString(body, "value", 65536, true)
		case "tempo":
			_, err = payloadNumber(body, "value", 0, 999, true)
		case "renderSampleRate":
			_, err = payloadNumber(body, "value", 8000, 768000, false)
		case "masterVolume":
			_, err = payloadNumber(body, "value", 0, 2, false)
		case "masterPan":
			_, err = payloadNumber(body, "value", -1, 1, false)
		}
		return err
	case "project.setTimeSignature":
		if err := exactPayloadKeys(body, []string{"numerator", "denominator"}, nil); err != nil {
			return err
		}
		if _, err := payloadInteger(body, "numerator", 1, 32); err != nil {
			return err
		}
		denominator, err := payloadInteger(body, "denominator", 1, 32)
		if err != nil {
			return err
		}
		if denominator != 1 && denominator != 2 && denominator != 4 && denominator != 8 && denominator != 16 && denominator != 32 {
			return invalidf("command payload denominator is unsupported")
		}
		return nil
	case "project.setKey":
		if err := exactPayloadKeys(body, []string{"root", "scale"}, nil); err != nil {
			return err
		}
		if _, err := payloadInteger(body, "root", math.MinInt32, math.MaxInt32); err != nil {
			return err
		}
		_, err := payloadString(body, "scale", 128, false)
		return err
	case "track.add":
		if err := exactPayloadKeys(body, []string{"trackId", "trackKind", "name", "color", "parentId", "afterId"}, nil); err != nil {
			return err
		}
		trackID, err := requiredPayloadUUID(body, "trackId")
		if err != nil {
			return err
		}
		if _, err := payloadEnum(body, "trackKind", "audio", "instrument", "midi", "pattern", "automation", "bus", "aux", "group", "master", "folder"); err != nil {
			return err
		}
		if _, err := payloadString(body, "name", 4096, true); err != nil {
			return err
		}
		if _, err := payloadInteger(body, "color", 0, math.MaxUint32); err != nil {
			return err
		}
		if err := optionalPayloadUUID(body, "parentId"); err != nil {
			return err
		}
		if err := optionalPayloadUUID(body, "afterId"); err != nil {
			return err
		}
		return rejectSelfReferences(body, trackID, "parentId", "afterId")
	case "track.delete":
		if err := exactPayloadKeys(body, []string{"trackId"}, nil); err != nil {
			return err
		}
		return requireIDs("trackId")
	case "track.restore":
		if err := exactPayloadKeys(body, []string{"trackId", "deleteOperationId"}, nil); err != nil {
			return err
		}
		return requireIDs("trackId", "deleteOperationId")
	case "track.move":
		if err := exactPayloadKeys(body, []string{"trackId", "afterId"}, nil); err != nil {
			return err
		}
		trackID, err := requiredPayloadUUID(body, "trackId")
		if err != nil {
			return err
		}
		if err := optionalPayloadUUID(body, "afterId"); err != nil {
			return err
		}
		return rejectSelfReferences(body, trackID, "afterId")
	case "track.setProperty":
		if err := exactPayloadKeys(body, []string{"trackId", "property", "value"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId"); err != nil {
			return err
		}
		property, err := payloadEnum(body, "property", "name", "color", "volume", "pan", "muted", "mono", "summing")
		if err != nil {
			return err
		}
		return validateTrackPropertyValue(body, property)
	case "track.setParent":
		if err := exactPayloadKeys(body, []string{"trackId", "parentId"}, nil); err != nil {
			return err
		}
		trackID, err := requiredPayloadUUID(body, "trackId")
		if err != nil {
			return err
		}
		if err := optionalPayloadUUID(body, "parentId"); err != nil {
			return err
		}
		return rejectSelfReferences(body, trackID, "parentId")
	case "track.setOutput":
		if err := exactPayloadKeys(body, []string{"trackId", "outputTrackId"}, nil); err != nil {
			return err
		}
		trackID, err := requiredPayloadUUID(body, "trackId")
		if err != nil {
			return err
		}
		if err := optionalPayloadUUID(body, "outputTrackId"); err != nil {
			return err
		}
		return rejectSelfReferences(body, trackID, "outputTrackId")
	case "send.add":
		if err := exactPayloadKeys(body, []string{"trackId", "send", "afterId"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId"); err != nil {
			return err
		}
		if err := validateSendPayload(body["send"]); err != nil {
			return err
		}
		if err := optionalPayloadUUID(body, "afterId"); err != nil {
			return err
		}
		sendID, _ := requiredNestedPayloadUUID(body, "send", "id")
		return rejectSelfReferences(body, sendID, "afterId")
	case "send.delete":
		if err := exactPayloadKeys(body, []string{"trackId", "sendId"}, nil); err != nil {
			return err
		}
		return requireIDs("trackId", "sendId")
	case "send.restore":
		if err := exactPayloadKeys(body, []string{"trackId", "sendId", "deleteOperationId"}, nil); err != nil {
			return err
		}
		return requireIDs("trackId", "sendId", "deleteOperationId")
	case "send.move":
		if err := exactPayloadKeys(body, []string{"trackId", "sendId", "afterId"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId", "sendId"); err != nil {
			return err
		}
		if err := optionalPayloadUUID(body, "afterId"); err != nil {
			return err
		}
		sendID, _ := requiredPayloadUUID(body, "sendId")
		return rejectSelfReferences(body, sendID, "afterId")
	case "send.setProperty":
		if err := exactPayloadKeys(body, []string{"trackId", "sendId", "property", "value"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId", "sendId"); err != nil {
			return err
		}
		property, err := payloadEnum(body, "property", "destinationTrackId", "level", "preFader", "enabled")
		if err != nil {
			return err
		}
		return validateSendPropertyValue(body, property)
	case "clip.add":
		if err := exactPayloadKeys(body,
			[]string{"trackId", "clipId", "clipKind", "name", "startSeconds", "durationSeconds", "color"},
			[]string{"afterId"}); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId"); err != nil {
			return err
		}
		if _, err := payloadEnum(body, "clipKind", "audio", "midi", "pattern", "automation"); err != nil {
			return err
		}
		if _, err := payloadString(body, "name", 4096, true); err != nil {
			return err
		}
		if _, err := payloadNumber(body, "startSeconds", 0, math.MaxFloat64, false); err != nil {
			return err
		}
		if _, err := payloadNumber(body, "durationSeconds", 0, math.MaxFloat64, false); err != nil {
			return err
		}
		if _, err := payloadInteger(body, "color", 0, math.MaxUint32); err != nil {
			return err
		}
		if err := optionalPayloadUUIDIfPresent(body, "afterId"); err != nil {
			return err
		}
		clipID, _ := requiredPayloadUUID(body, "clipId")
		return rejectSelfReferences(body, clipID, "afterId")
	case "clip.delete":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId"}, nil); err != nil {
			return err
		}
		return requireIDs("trackId", "clipId")
	case "clip.restore":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "deleteOperationId"}, nil); err != nil {
			return err
		}
		return requireIDs("trackId", "clipId", "deleteOperationId")
	case "clip.move":
		if err := exactPayloadKeys(body, []string{"clipId", "sourceTrackId", "trackId", "afterId"}, nil); err != nil {
			return err
		}
		if err := requireIDs("clipId", "sourceTrackId", "trackId"); err != nil {
			return err
		}
		if err := optionalPayloadUUIDIfPresent(body, "afterId"); err != nil {
			return err
		}
		clipID, _ := requiredPayloadUUID(body, "clipId")
		return rejectSelfReferences(body, clipID, "afterId")
	case "clip.setProperty":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "property", "value"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId"); err != nil {
			return err
		}
		property, err := payloadEnum(body, "property", "name", "startSeconds", "durationSeconds", "offsetSeconds", "gain", "pan", "muted", "color", "compCrossfadeMs")
		if err != nil {
			return err
		}
		return validateClipPropertyValue(body, property)
	case "clip.setAsset":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "asset"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId"); err != nil {
			return err
		}
		if rawJSONNull(body["asset"]) {
			return nil
		}
		_, err := validateAssetRef(body["asset"], "audio")
		return err
	case "clip.setSampleEdit":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "sampleEdit"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId"); err != nil {
			return err
		}
		return validateClipSampleEdit(body["sampleEdit"])
	case "clip.setFade":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "fadeInSeconds", "fadeOutSeconds"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId"); err != nil {
			return err
		}
		if _, err := payloadNumber(body, "fadeInSeconds", 0, math.MaxFloat64, false); err != nil {
			return err
		}
		_, err := payloadNumber(body, "fadeOutSeconds", 0, math.MaxFloat64, false)
		return err
	case "clip.setFadeCurve":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "edge", "curve"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId"); err != nil {
			return err
		}
		if _, err := payloadEnum(body, "edge", "in", "out"); err != nil {
			return err
		}
		_, err := payloadNumber(body, "curve", -1, 1, false)
		return err
	case "clip.setFadeMode":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "edge", "mode"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId"); err != nil {
			return err
		}
		if _, err := payloadEnum(body, "edge", "in", "out"); err != nil {
			return err
		}
		_, err := payloadEnum(body, "mode", "gain", "tape")
		return err
	case "clip.setPatternOwner":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "patternClipId"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId"); err != nil {
			return err
		}
		return optionalPayloadUUID(body, "patternClipId")
	case "clip.setMusicalAnalysis":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "analysis"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId"); err != nil {
			return err
		}
		return validateMusicalAnalysis(body["analysis"])
	case "plugin.add":
		if err := exactPayloadKeys(body, []string{"location", "insert", "afterId"}, nil); err != nil {
			return err
		}
		location, err := validatePluginLocation(body["location"])
		if err != nil {
			return err
		}
		insertID, uid, format, err := validateSharedInsert(body["insert"], schemaVersion)
		if err != nil {
			return err
		}
		if format == "internal" &&
			(location.Chain == "instrument") != (uid == "daw.sampler") {
			return invalidf("command payload plugin kind is unsupported for its chain")
		}
		if err := optionalPayloadUUID(body, "afterId"); err != nil {
			return err
		}
		if location.Chain == "instrument" {
			afterID, _ := optionalPayloadUUIDValue(body, "afterId")
			if afterID != "" {
				return invalidf("command payload instrument chain cannot have an anchor")
			}
		}
		return rejectSelfReferences(body, insertID, "afterId")
	case "plugin.delete":
		return validatePluginReferencePayload(body, false, false)
	case "plugin.restore":
		return validatePluginReferencePayload(body, true, false)
	case "plugin.move":
		return validatePluginReferencePayload(body, false, true)
	case "plugin.replace":
		if err := exactPayloadKeys(body, []string{"location", "insertId", "replacement"}, nil); err != nil {
			return err
		}
		location, err := validatePluginLocation(body["location"])
		if err != nil {
			return err
		}
		insertID, err := requiredPayloadUUID(body, "insertId")
		if err != nil {
			return err
		}
		replacementID, uid, format, err := validateSharedInsert(
			body["replacement"], schemaVersion)
		if err != nil || replacementID != insertID {
			return invalidf("command payload replacement must preserve insertId")
		}
		if format == "internal" &&
			(location.Chain == "instrument") != (uid == "daw.sampler") {
			return invalidf("command payload plugin kind is unsupported for its chain")
		}
		return nil
	case "plugin.setProperty":
		if err := exactPayloadKeys(body, []string{"location", "insertId", "property", "value"}, nil); err != nil {
			return err
		}
		if _, err := validatePluginLocation(body["location"]); err != nil {
			return err
		}
		if err := requireIDs("insertId"); err != nil {
			return err
		}
		property, err := payloadEnum(body, "property", "name", "bypassed", "mix", "channelMode", "sidechainTrackId")
		if err != nil {
			return err
		}
		return validatePluginPropertyValue(body, property)
	case "plugin.setState":
		if err := exactPayloadKeys(body, []string{"location", "insertId", "pluginVersion", "stateSchemaVersion", "stateAsset", "rightStateAsset", "parameters", "rightParameters", "assetBindings"}, nil); err != nil {
			return err
		}
		location, err := validatePluginLocation(body["location"])
		if err != nil {
			return err
		}
		if err := requireIDs("insertId"); err != nil {
			return err
		}
		if _, err := payloadString(body, "pluginVersion", 64, false); err != nil {
			return err
		}
		minimumStateSchema := int64(1)
		if schemaVersion == CollaborationCommandSchemaV3 {
			minimumStateSchema = 0
		}
		if _, err := payloadInteger(body, "stateSchemaVersion", minimumStateSchema, math.MaxInt32); err != nil {
			return err
		}
		for _, name := range []string{"stateAsset", "rightStateAsset"} {
			if !rawJSONNull(body[name]) {
				if _, err := validateAssetRef(body[name], "plugin-state"); err != nil {
					return err
				}
			}
		}
		if err := validatePluginParameters(body["parameters"], 4096); err != nil {
			return err
		}
		if err := validatePluginParameters(body["rightParameters"], 4096); err != nil {
			return err
		}
		return validatePluginBindings(body["assetBindings"], location.Chain == "instrument")
	case "plugin.setParameter":
		if err := exactPayloadKeys(body, []string{"location", "insertId", "parameterId", "value", "rightChannel"}, nil); err != nil {
			return err
		}
		if _, err := validatePluginLocation(body["location"]); err != nil {
			return err
		}
		if err := requireIDs("insertId"); err != nil {
			return err
		}
		if _, err := payloadString(body, "parameterId", maximumPluginParameterIDBytes, false); err != nil {
			return err
		}
		if _, err := payloadNumber(body, "value", -math.MaxFloat64, math.MaxFloat64, false); err != nil {
			return err
		}
		_, err = payloadBool(body, "rightChannel")
		return err
	case "plugin.removeParameter":
		if err := exactPayloadKeys(body, []string{"location", "insertId", "parameterId", "rightChannel"}, nil); err != nil {
			return err
		}
		if _, err := validatePluginLocation(body["location"]); err != nil {
			return err
		}
		if err := requireIDs("insertId"); err != nil {
			return err
		}
		if _, err := payloadString(body, "parameterId", maximumPluginParameterIDBytes, false); err != nil {
			return err
		}
		_, err := payloadBool(body, "rightChannel")
		return err
	case "plugin.setAssetBinding":
		if err := exactPayloadKeys(body, []string{"location", "insertId", "binding"}, nil); err != nil {
			return err
		}
		location, err := validatePluginLocation(body["location"])
		if err != nil {
			return err
		}
		if err := requireIDs("insertId"); err != nil {
			return err
		}
		key, required, err := validatedPluginBinding(body["binding"])
		if err != nil {
			return err
		}
		if location.Chain == "instrument" && key == "sample" && !required {
			return invalidf("command payload Sampler sample binding must be required")
		}
		return nil
	case "plugin.removeAssetBinding":
		if err := exactPayloadKeys(body, []string{"location", "insertId", "key"}, nil); err != nil {
			return err
		}
		if _, err := validatePluginLocation(body["location"]); err != nil {
			return err
		}
		if err := requireIDs("insertId"); err != nil {
			return err
		}
		_, err := payloadString(body, "key", 96, false)
		return err
	case "samplerFx.setLevels":
		if err := exactPayloadKeys(body, []string{"trackId", "instrumentId", "volume", "pan"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId", "instrumentId"); err != nil {
			return err
		}
		if _, err := payloadNumber(body, "volume", 0, 2, false); err != nil {
			return err
		}
		_, err := payloadNumber(body, "pan", -1, 1, false)
		return err
	case "note.upsert":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "note"}, []string{"afterId"}); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId"); err != nil {
			return err
		}
		if err := validateNotePayload(body["note"]); err != nil {
			return err
		}
		if err := optionalPayloadUUIDIfPresent(body, "afterId"); err != nil {
			return err
		}
		noteID, _ := requiredNestedPayloadUUID(body, "note", "id")
		return rejectSelfReferences(body, noteID, "afterId")
	case "note.delete":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "noteId"}, nil); err != nil {
			return err
		}
		return requireIDs("trackId", "clipId", "noteId")
	case "note.restore":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "noteId", "deleteOperationId"}, nil); err != nil {
			return err
		}
		return requireIDs("trackId", "clipId", "noteId", "deleteOperationId")
	case "automationPoint.upsert":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "point"}, []string{"laneId", "afterId"}); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId"); err != nil {
			return err
		}
		if err := optionalPayloadUUIDIfPresent(body, "laneId"); err != nil {
			return err
		}
		if err := optionalPayloadUUIDIfPresent(body, "afterId"); err != nil {
			return err
		}
		if err := validateAutomationPointPayload(body["point"]); err != nil {
			return err
		}
		pointID, _ := requiredNestedPayloadUUID(body, "point", "id")
		return rejectSelfReferences(body, pointID, "afterId")
	case "automationPoint.delete":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "pointId"}, []string{"laneId"}); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId", "pointId"); err != nil {
			return err
		}
		return optionalPayloadUUIDIfPresent(body, "laneId")
	case "automationPoint.restore":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "pointId", "deleteOperationId"}, []string{"laneId"}); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId", "pointId", "deleteOperationId"); err != nil {
			return err
		}
		return optionalPayloadUUIDIfPresent(body, "laneId")
	case "controllerLane.add":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "laneId", "name", "target", "defaultValue", "afterId"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId", "laneId"); err != nil {
			return err
		}
		if _, err := payloadString(body, "name", 4096, true); err != nil {
			return err
		}
		if err := validateControllerLaneTarget(body["target"]); err != nil {
			return err
		}
		if _, err := payloadNumber(body, "defaultValue", 0, 1, false); err != nil {
			return err
		}
		if err := optionalPayloadUUID(body, "afterId"); err != nil {
			return err
		}
		laneID, _ := requiredPayloadUUID(body, "laneId")
		return rejectSelfReferences(body, laneID, "afterId")
	case "controllerLane.delete":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "laneId"}, nil); err != nil {
			return err
		}
		return requireIDs("trackId", "clipId", "laneId")
	case "controllerLane.restore":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "laneId", "deleteOperationId"}, nil); err != nil {
			return err
		}
		return requireIDs("trackId", "clipId", "laneId", "deleteOperationId")
	case "controllerLane.setTarget":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "laneId", "target"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId", "laneId"); err != nil {
			return err
		}
		return validateControllerLaneTarget(body["target"])
	case "controllerLane.setDefault":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "laneId", "defaultValue"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId", "laneId"); err != nil {
			return err
		}
		_, err := payloadNumber(body, "defaultValue", 0, 1, false)
		return err
	case "automation.setTarget":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "target"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId"); err != nil {
			return err
		}
		return validateAutomationTarget(body["target"])
	case "automation.setDefault":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "defaultValue"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId"); err != nil {
			return err
		}
		_, err := payloadNumber(body, "defaultValue", 0, 1, false)
		return err
	case "automation.setActive":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "active"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId"); err != nil {
			return err
		}
		_, err := payloadBool(body, "active")
		return err
	case "take.add":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "take", "afterId"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId"); err != nil {
			return err
		}
		if err := validateTakePayload(body["take"]); err != nil {
			return err
		}
		if err := optionalPayloadUUID(body, "afterId"); err != nil {
			return err
		}
		takeID, _ := requiredNestedPayloadUUID(body, "take", "id")
		return rejectSelfReferences(body, takeID, "afterId")
	case "take.delete":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "takeId"}, nil); err != nil {
			return err
		}
		return requireIDs("trackId", "clipId", "takeId")
	case "take.restore":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "takeId", "deleteOperationId"}, nil); err != nil {
			return err
		}
		return requireIDs("trackId", "clipId", "takeId", "deleteOperationId")
	case "take.move":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "takeId", "afterId"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId", "takeId"); err != nil {
			return err
		}
		if err := optionalPayloadUUID(body, "afterId"); err != nil {
			return err
		}
		takeID, _ := requiredPayloadUUID(body, "takeId")
		return rejectSelfReferences(body, takeID, "afterId")
	case "take.setProperty":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "takeId", "property", "value"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId", "takeId"); err != nil {
			return err
		}
		property, err := payloadEnum(body, "property", "name", "offsetSeconds", "lengthSeconds", "clipOffsetSeconds", "gain", "muted", "color")
		if err != nil {
			return err
		}
		return validateTakePropertyValue(body, property)
	case "compSegment.upsert":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "segment", "afterId"}, nil); err != nil {
			return err
		}
		if err := requireIDs("trackId", "clipId"); err != nil {
			return err
		}
		if err := validateCompSegmentPayload(body["segment"]); err != nil {
			return err
		}
		if err := optionalPayloadUUID(body, "afterId"); err != nil {
			return err
		}
		segmentID, _ := requiredNestedPayloadUUID(body, "segment", "id")
		return rejectSelfReferences(body, segmentID, "afterId")
	case "compSegment.delete":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "segmentId"}, nil); err != nil {
			return err
		}
		return requireIDs("trackId", "clipId", "segmentId")
	case "compSegment.restore":
		if err := exactPayloadKeys(body, []string{"trackId", "clipId", "segmentId", "deleteOperationId"}, nil); err != nil {
			return err
		}
		return requireIDs("trackId", "clipId", "segmentId", "deleteOperationId")
	case "batch":
		if !allowBatch {
			return invalidf("nested batch commands are unsupported")
		}
		return validateBatchPayload(body, schemaVersion)
	case "recording.commit":
		if !allowBatch {
			return invalidf("nested recording commits are unsupported")
		}
		return validateRecordingCommitPayload(body, schemaVersion)
	default:
		return invalidf("operation kind is unsupported by command schema version 2")
	}
}

func validateTrackPropertyValue(body map[string]json.RawMessage, property string) error {
	switch property {
	case "name":
		_, err := payloadString(body, "value", 4096, true)
		return err
	case "color":
		_, err := payloadInteger(body, "value", 0, math.MaxUint32)
		return err
	case "volume":
		_, err := payloadNumber(body, "value", 0, 2, false)
		return err
	case "pan":
		_, err := payloadNumber(body, "value", -1, 1, false)
		return err
	case "muted", "mono", "summing":
		_, err := payloadBool(body, "value")
		return err
	default:
		return invalidf("track property is unsupported")
	}
}

func validateClipPropertyValue(body map[string]json.RawMessage, property string) error {
	switch property {
	case "name":
		_, err := payloadString(body, "value", 4096, true)
		return err
	case "startSeconds", "durationSeconds", "offsetSeconds":
		_, err := payloadNumber(body, "value", 0, math.MaxFloat64, false)
		return err
	case "gain":
		_, err := payloadNumber(body, "value", 0, 4, false)
		return err
	case "pan":
		_, err := payloadNumber(body, "value", -1, 1, false)
		return err
	case "compCrossfadeMs":
		_, err := payloadNumber(body, "value", 0, 20, false)
		return err
	case "muted":
		_, err := payloadBool(body, "value")
		return err
	case "color":
		_, err := payloadInteger(body, "value", 0, math.MaxUint32)
		return err
	default:
		return invalidf("clip property is unsupported")
	}
}

func validateTakePropertyValue(body map[string]json.RawMessage, property string) error {
	switch property {
	case "name":
		_, err := payloadString(body, "value", 4096, true)
		return err
	case "offsetSeconds", "lengthSeconds", "clipOffsetSeconds":
		_, err := payloadNumber(body, "value", 0, math.MaxFloat64, false)
		return err
	case "gain":
		_, err := payloadNumber(body, "value", 0, 4, false)
		return err
	case "muted":
		_, err := payloadBool(body, "value")
		return err
	case "color":
		_, err := payloadInteger(body, "value", 0, math.MaxUint32)
		return err
	default:
		return invalidf("take property is unsupported")
	}
}

func validateMusicalAnalysis(raw json.RawMessage) error {
	body, err := commandPayloadObject(raw)
	if err != nil {
		return invalidf("command payload musical analysis must be an object")
	}
	if err := exactPayloadKeys(body,
		[]string{"version", "offsetSeconds", "durationSeconds", "tempo", "key"}, nil); err != nil {
		return err
	}
	if _, err := payloadInteger(body, "version", 0, 1000000); err != nil {
		return err
	}
	if _, err := payloadNumber(body, "offsetSeconds", 0, math.MaxFloat64, false); err != nil {
		return err
	}
	if _, err := payloadNumber(body, "durationSeconds", 0, math.MaxFloat64, false); err != nil {
		return err
	}
	tempo, err := commandPayloadObject(body["tempo"])
	if err != nil {
		return invalidf("command payload tempo analysis must be an object")
	}
	if err := exactPayloadKeys(tempo,
		[]string{"status", "bpm", "confidence", "stability", "alternatives", "variable"}, nil); err != nil {
		return err
	}
	if _, err := payloadInteger(tempo, "status", 0, 2); err != nil {
		return err
	}
	for name, bounds := range map[string][2]float64{
		"bpm": {0, 300}, "confidence": {0, 1}, "stability": {0, 1},
	} {
		if _, err := payloadNumber(tempo, name, bounds[0], bounds[1], false); err != nil {
			return err
		}
	}
	var alternatives []json.RawMessage
	if err := json.Unmarshal(tempo["alternatives"], &alternatives); err != nil ||
		alternatives == nil || len(alternatives) > 3 {
		return invalidf("command payload tempo alternatives are invalid")
	}
	for _, alternative := range alternatives {
		candidate := map[string]json.RawMessage{"value": alternative}
		if _, err := payloadNumber(candidate, "value", 0, 300, true); err != nil {
			return err
		}
	}
	if _, err := payloadBool(tempo, "variable"); err != nil {
		return err
	}
	key, err := commandPayloadObject(body["key"])
	if err != nil {
		return invalidf("command payload key analysis must be an object")
	}
	if err := exactPayloadKeys(key,
		[]string{"status", "root", "scale", "confidence", "alternateRoot", "alternateScale", "tuningCents"}, nil); err != nil {
		return err
	}
	if _, err := payloadInteger(key, "status", 0, 2); err != nil {
		return err
	}
	if _, err := payloadInteger(key, "root", -1, 11); err != nil {
		return err
	}
	if _, err := payloadString(key, "scale", 128, true); err != nil {
		return err
	}
	if _, err := payloadNumber(key, "confidence", 0, 1, false); err != nil {
		return err
	}
	if _, err := payloadInteger(key, "alternateRoot", -1, 11); err != nil {
		return err
	}
	if _, err := payloadString(key, "alternateScale", 128, true); err != nil {
		return err
	}
	_, err = payloadNumber(key, "tuningCents", -200, 200, false)
	return err
}

func validateSendPayload(raw json.RawMessage) error {
	body, err := commandPayloadObject(raw)
	if err != nil {
		return invalidf("command payload send must be an object")
	}
	if err := exactPayloadKeys(body, []string{"id", "destinationTrackId", "level", "preFader", "enabled"}, nil); err != nil {
		return err
	}
	if _, err := requiredPayloadUUID(body, "id"); err != nil {
		return err
	}
	if _, err := requiredPayloadUUID(body, "destinationTrackId"); err != nil {
		return err
	}
	if _, err := payloadNumber(body, "level", 0, maximumSendLevel, false); err != nil {
		return err
	}
	if _, err := payloadBool(body, "preFader"); err != nil {
		return err
	}
	_, err = payloadBool(body, "enabled")
	return err
}

func validateSendPropertyValue(body map[string]json.RawMessage, property string) error {
	switch property {
	case "destinationTrackId":
		_, err := requiredPayloadUUID(body, "value")
		return err
	case "level":
		_, err := payloadNumber(body, "value", 0, maximumSendLevel, false)
		return err
	case "preFader", "enabled":
		_, err := payloadBool(body, "value")
		return err
	default:
		return invalidf("send property is unsupported")
	}
}

func validateClipSampleEdit(raw json.RawMessage) error {
	body, err := commandPayloadObject(raw)
	if err != nil {
		return invalidf("command payload sampleEdit must be an object")
	}
	required := []string{
		"loopMode", "loopStart", "loopEnd", "stretchMode", "stretchTime",
		"stretchPitch", "formant", "rootNote", "boost", "eqLow", "eqMid",
		"eqHigh", "ringMix", "ringFreq", "cut", "res", "reverbType",
		"reverb", "stereoDelay", "pogo", "removeDc", "reversePolarity",
		"normalize", "fadeStereo", "reverse", "swapStereo",
	}
	if err := exactPayloadKeys(body, required, nil); err != nil {
		return err
	}
	for name, bounds := range map[string][2]int64{
		"loopMode": {0, 2}, "stretchMode": {0, 4}, "rootNote": {0, 127},
		"reverbType": {0, 16},
	} {
		if _, err := payloadInteger(body, name, bounds[0], bounds[1]); err != nil {
			return err
		}
	}
	loopStart, err := payloadNumber(body, "loopStart", 0, 1, false)
	if err != nil {
		return err
	}
	loopEnd, err := payloadNumber(body, "loopEnd", 0, 1, false)
	if err != nil {
		return err
	}
	if loopStart > loopEnd {
		return invalidf("command payload sample loop is inverted")
	}
	numeric := map[string][2]float64{
		"stretchTime": {0.01, 100}, "stretchPitch": {-96, 96},
		"formant": {-96, 96}, "boost": {-4, 4}, "eqLow": {-4, 4},
		"eqMid": {-4, 4}, "eqHigh": {-4, 4}, "ringMix": {0, 1},
		"ringFreq": {0, 1}, "cut": {0, 1}, "res": {0, 1},
		"reverb": {0, 1}, "stereoDelay": {0, 1}, "pogo": {0, 1},
	}
	for name, bounds := range numeric {
		if _, err := payloadNumber(body, name, bounds[0], bounds[1], false); err != nil {
			return err
		}
	}
	for _, name := range []string{"removeDc", "reversePolarity", "normalize", "fadeStereo", "reverse", "swapStereo"} {
		if _, err := payloadBool(body, name); err != nil {
			return err
		}
	}
	return nil
}

type validatedPluginLocation struct {
	Chain   string
	TrackID string
	ClipID  string
}

func validatePluginLocation(raw json.RawMessage) (validatedPluginLocation, error) {
	body, err := commandPayloadObject(raw)
	if err != nil {
		return validatedPluginLocation{}, invalidf("command payload plugin location must be an object")
	}
	if err := exactPayloadKeys(body, []string{"chain", "trackId", "clipId"}, nil); err != nil {
		return validatedPluginLocation{}, err
	}
	chain, err := payloadEnum(body, "chain", "master", "track", "instrument", "samplerFx", "clip")
	if err != nil {
		return validatedPluginLocation{}, err
	}
	trackID, err := optionalPayloadUUIDValue(body, "trackId")
	if err != nil {
		return validatedPluginLocation{}, err
	}
	clipID, err := optionalPayloadUUIDValue(body, "clipId")
	if err != nil {
		return validatedPluginLocation{}, err
	}
	valid := false
	switch chain {
	case "master":
		valid = trackID == "" && clipID == ""
	case "track", "instrument", "samplerFx":
		valid = trackID != "" && clipID == ""
	case "clip":
		valid = trackID != "" && clipID != ""
	}
	if !valid {
		return validatedPluginLocation{}, invalidf("command payload plugin location identifiers do not match its chain")
	}
	return validatedPluginLocation{Chain: chain, TrackID: trackID, ClipID: clipID}, nil
}

func validatePluginReferencePayload(body map[string]json.RawMessage, restore, move bool) error {
	required := []string{"location", "insertId"}
	if restore {
		required = append(required, "deleteOperationId")
	}
	if move {
		required = append(required, "afterId")
	}
	if err := exactPayloadKeys(body, required, nil); err != nil {
		return err
	}
	location, err := validatePluginLocation(body["location"])
	if err != nil {
		return err
	}
	insertID, err := requiredPayloadUUID(body, "insertId")
	if err != nil {
		return err
	}
	if restore {
		if _, err := requiredPayloadUUID(body, "deleteOperationId"); err != nil {
			return err
		}
	}
	if move {
		if err := optionalPayloadUUID(body, "afterId"); err != nil {
			return err
		}
		if location.Chain == "instrument" {
			return invalidf("command payload instrument chain cannot be reordered")
		}
		return rejectSelfReferences(body, insertID, "afterId")
	}
	return nil
}

func validatePluginPropertyValue(body map[string]json.RawMessage, property string) error {
	switch property {
	case "name":
		_, err := payloadString(body, "value", 4096, true)
		return err
	case "bypassed":
		_, err := payloadBool(body, "value")
		return err
	case "mix":
		_, err := payloadNumber(body, "value", 0, 1, false)
		return err
	case "channelMode":
		_, err := payloadEnum(body, "value", "auto", "mono", "stereo", "dual-mono")
		return err
	case "sidechainTrackId":
		return optionalPayloadUUID(body, "value")
	default:
		return invalidf("plugin property is unsupported")
	}
}

func validateSharedInsert(raw json.RawMessage,
	schemaVersion int) (string, string, string, error) {
	body, err := commandPayloadObject(raw)
	if err != nil {
		return "", "", "", invalidf("command payload insert must be an object")
	}
	required := []string{"id", "name", "bypassed", "format", "uid", "vendor", "pluginVersion", "stateSchemaVersion", "mix", "channelMode", "sidechainTrackId", "stateAsset", "rightStateAsset", "parameters", "rightParameters", "assetBindings"}
	if err := exactPayloadKeys(body, required, nil); err != nil {
		return "", "", "", err
	}
	insertID, err := requiredPayloadUUID(body, "id")
	if err != nil {
		return "", "", "", err
	}
	if _, err := payloadString(body, "name", 4096, true); err != nil {
		return "", "", "", err
	}
	if _, err := payloadBool(body, "bypassed"); err != nil {
		return "", "", "", err
	}
	format, err := payloadString(body, "format", 16, false)
	if err != nil || (format != "internal" &&
		(schemaVersion != CollaborationCommandSchemaV3 || !validPluginFormat(format))) {
		return "", "", "", invalidf("command payload plugin format is unsupported")
	}
	// Must stay in lockstep with supportedBuiltin() in ProjectReducer.cpp and
	// the sharedInsert uid enum in protocol/schema/project-command-v2.schema.json.
	// scripts/check-collaboration-contracts.mjs asserts all three agree.
	uid, err := payloadString(body, "uid", 400, false)
	if err != nil || !safePluginContractText(uid, 400) {
		return "", "", "", invalidf("command payload plugin uid is invalid")
	}
	if format == "internal" && uid != "daw.sampler" && uid != "daw.equalizer" &&
		uid != "daw.gravity" && uid != "daw.graphit" {
		return "", "", "", invalidf("command payload built-in plugin uid is unsupported")
	}
	vendor, err := payloadString(body, "vendor", 4096, format == "internal")
	if err != nil || (format != "internal" && !safePluginContractText(vendor, 200)) {
		return "", "", "", invalidf("command payload plugin vendor is invalid")
	}
	maximumVersionLength := 64
	minimumStateSchema := int64(1)
	if schemaVersion == CollaborationCommandSchemaV3 {
		maximumVersionLength = 200
		minimumStateSchema = 0
	}
	version, err := payloadString(body, "pluginVersion", maximumVersionLength, false)
	if err != nil || !safePluginContractText(version, maximumVersionLength) {
		return "", "", "", invalidf("command payload plugin version is invalid")
	}
	if _, err := payloadInteger(body, "stateSchemaVersion", minimumStateSchema, math.MaxInt32); err != nil {
		return "", "", "", err
	}
	if _, err := payloadNumber(body, "mix", 0, 1, false); err != nil {
		return "", "", "", err
	}
	if _, err := payloadEnum(body, "channelMode", "auto", "mono", "stereo", "dual-mono"); err != nil {
		return "", "", "", err
	}
	if err := optionalPayloadUUID(body, "sidechainTrackId"); err != nil {
		return "", "", "", err
	}
	for _, name := range []string{"stateAsset", "rightStateAsset"} {
		if !rawJSONNull(body[name]) {
			if _, err := validateAssetRef(body[name], "plugin-state"); err != nil {
				return "", "", "", err
			}
		}
	}
	if err := validatePluginParameters(body["parameters"], 4096); err != nil {
		return "", "", "", err
	}
	if err := validatePluginParameters(body["rightParameters"], 4096); err != nil {
		return "", "", "", err
	}
	if err := validatePluginBindings(body["assetBindings"], uid == "daw.sampler"); err != nil {
		return "", "", "", err
	}
	return insertID, uid, format, nil
}

func validatePluginParameters(raw json.RawMessage, maximumIDBytes int) error {
	var values []json.RawMessage
	if err := json.Unmarshal(raw, &values); err != nil || values == nil || len(values) > 16384 {
		return invalidf("command payload plugin parameters must be an array with at most 16384 items")
	}
	seen := make(map[string]struct{}, len(values))
	for _, rawValue := range values {
		body, err := commandPayloadObject(rawValue)
		if err != nil {
			return invalidf("command payload plugin parameter must be an object")
		}
		if err := exactPayloadKeys(body, []string{"id", "value"}, nil); err != nil {
			return err
		}
		id, err := payloadString(body, "id", maximumIDBytes, false)
		if err != nil {
			return err
		}
		if _, exists := seen[id]; exists {
			return invalidf("command payload plugin parameter identifiers must be unique")
		}
		seen[id] = struct{}{}
		if _, err := payloadNumber(body, "value", -math.MaxFloat64, math.MaxFloat64, false); err != nil {
			return err
		}
	}
	return nil
}

func validatePluginBindings(raw json.RawMessage, sampler bool) error {
	var values []json.RawMessage
	if err := json.Unmarshal(raw, &values); err != nil || values == nil || len(values) > 1024 {
		return invalidf("command payload plugin bindings must be an array with at most 1024 items")
	}
	seen := make(map[string]struct{}, len(values))
	for _, rawValue := range values {
		key, required, err := validatedPluginBinding(rawValue)
		if err != nil {
			return err
		}
		if _, exists := seen[key]; exists {
			return invalidf("command payload plugin binding keys must be unique")
		}
		seen[key] = struct{}{}
		if sampler && key == "sample" && !required {
			return invalidf("command payload Sampler sample binding must be required")
		}
	}
	return nil
}

func validatedPluginBinding(raw json.RawMessage) (string, bool, error) {
	body, err := commandPayloadObject(raw)
	if err != nil {
		return "", false, invalidf("command payload plugin binding must be an object")
	}
	if err := exactPayloadKeys(body, []string{"key", "asset", "required"}, nil); err != nil {
		return "", false, err
	}
	key, err := payloadString(body, "key", 96, false)
	if err != nil {
		return "", false, err
	}
	kind, err := validateAssetRef(body["asset"])
	if err != nil {
		return "", false, err
	}
	if key == "sample" && kind != "audio" {
		return "", false, invalidf("command payload sample binding must reference audio")
	}
	required, err := payloadBool(body, "required")
	if err != nil {
		return "", false, err
	}
	return key, required, nil
}

func validateNotePayload(raw json.RawMessage) error {
	body, err := commandPayloadObject(raw)
	if err != nil {
		return invalidf("command payload note must be an object")
	}
	if err := exactPayloadKeys(body, []string{"id", "pitch", "startBeats", "lengthBeats", "velocity", "muted", "color", "pan"}, nil); err != nil {
		return err
	}
	if _, err := requiredPayloadUUID(body, "id"); err != nil {
		return err
	}
	if _, err := payloadInteger(body, "pitch", 0, 127); err != nil {
		return err
	}
	if _, err := payloadNumber(body, "startBeats", 0, math.MaxFloat64, false); err != nil {
		return err
	}
	if _, err := payloadNumber(body, "lengthBeats", 0, math.MaxFloat64, true); err != nil {
		return err
	}
	if _, err := payloadInteger(body, "velocity", 1, 127); err != nil {
		return err
	}
	if _, err := payloadBool(body, "muted"); err != nil {
		return err
	}
	if _, err := payloadInteger(body, "color", 0, math.MaxUint32); err != nil {
		return err
	}
	_, err = payloadNumber(body, "pan", -1, 1, false)
	return err
}

func validateAutomationPointPayload(raw json.RawMessage) error {
	body, err := commandPayloadObject(raw)
	if err != nil {
		return invalidf("command payload point must be an object")
	}
	if err := exactPayloadKeys(body, []string{"id", "beats", "value", "shape", "curve"}, nil); err != nil {
		return err
	}
	if _, err := requiredPayloadUUID(body, "id"); err != nil {
		return err
	}
	if _, err := payloadNumber(body, "beats", 0, math.MaxFloat64, false); err != nil {
		return err
	}
	if _, err := payloadNumber(body, "value", 0, 1, false); err != nil {
		return err
	}
	if _, err := payloadEnum(body, "shape", "linear", "hold", "scurve"); err != nil {
		return err
	}
	_, err = payloadNumber(body, "curve", -1, 1, false)
	return err
}

func validateControllerLaneTarget(raw json.RawMessage) error {
	body, err := commandPayloadObject(raw)
	if err != nil {
		return invalidf("command payload target must be an object")
	}
	if err := exactPayloadKeys(body, []string{"cc", "parameterId", "slotId"}, nil); err != nil {
		return err
	}
	cc, err := payloadInteger(body, "cc", -1, 127)
	if err != nil {
		return err
	}
	parameterID, err := payloadString(body, "parameterId", 4096, true)
	if err != nil {
		return err
	}
	if err := optionalPayloadUUID(body, "slotId"); err != nil {
		return err
	}
	if cc == -1 && parameterID == "" {
		return invalidf("controller lane target requires a CC or parameter")
	}
	return nil
}

func validateAutomationTarget(raw json.RawMessage) error {
	body, err := commandPayloadObject(raw)
	if err != nil {
		return invalidf("command payload target must be an object")
	}
	if err := exactPayloadKeys(body, []string{"kind", "channelId", "slotId", "parameterId", "sendId"}, nil); err != nil {
		return err
	}
	kind, err := payloadEnum(body, "kind", "volume", "pan", "mute", "send", "parameter")
	if err != nil {
		return err
	}
	if _, err := requiredPayloadUUID(body, "channelId"); err != nil {
		return err
	}
	slotID, err := optionalPayloadUUIDValue(body, "slotId")
	if err != nil {
		return err
	}
	parameterID, err := payloadString(body, "parameterId", 4096, true)
	if err != nil {
		return err
	}
	sendID, err := optionalPayloadUUIDValue(body, "sendId")
	if err != nil {
		return err
	}
	switch kind {
	case "volume", "pan", "mute":
		if slotID != "" || parameterID != "" || sendID != "" {
			return invalidf("automation channel target contains unrelated identifiers")
		}
	case "send":
		if slotID != "" || parameterID != "" || sendID == "" {
			return invalidf("automation send target is incomplete")
		}
	case "parameter":
		if parameterID == "" || sendID != "" {
			return invalidf("automation parameter target is incomplete")
		}
	}
	return nil
}

func validateTakePayload(raw json.RawMessage) error {
	body, err := commandPayloadObject(raw)
	if err != nil {
		return invalidf("command payload take must be an object")
	}
	if err := exactPayloadKeys(body, []string{"id", "name", "offsetSeconds", "lengthSeconds", "clipOffsetSeconds", "gain", "muted", "channels", "color", "asset"}, nil); err != nil {
		return err
	}
	if _, err := requiredPayloadUUID(body, "id"); err != nil {
		return err
	}
	if _, err := payloadString(body, "name", 4096, true); err != nil {
		return err
	}
	for _, name := range []string{"offsetSeconds", "lengthSeconds", "clipOffsetSeconds"} {
		if _, err := payloadNumber(body, name, 0, math.MaxFloat64, false); err != nil {
			return err
		}
	}
	if _, err := payloadNumber(body, "gain", 0, 4, false); err != nil {
		return err
	}
	if _, err := payloadBool(body, "muted"); err != nil {
		return err
	}
	if _, err := payloadInteger(body, "channels", 1, 1024); err != nil {
		return err
	}
	if _, err := payloadInteger(body, "color", 0, math.MaxUint32); err != nil {
		return err
	}
	return validateAudioAssetRef(body["asset"])
}

func validateAudioAssetRef(raw json.RawMessage) error {
	_, err := validateAssetRef(raw, "audio")
	return err
}

func validateAssetRef(raw json.RawMessage, expectedKinds ...string) (string, error) {
	body, err := commandPayloadObject(raw)
	if err != nil {
		return "", invalidf("command payload asset must be an object")
	}
	if err := exactPayloadKeys(body, []string{"assetId", "sha256", "kind", "byteSize", "originalName"}, []string{"audioMetadata"}); err != nil {
		return "", err
	}
	if _, err := requiredPayloadUUID(body, "assetId"); err != nil {
		return "", err
	}
	digest, err := payloadString(body, "sha256", 64, false)
	if err != nil || !lowercaseSHA256Pattern.MatchString(digest) {
		return "", invalidf("command payload sha256 must be a lowercase SHA-256 digest")
	}
	kind, err := payloadEnum(body, "kind", "audio", "plugin-state", "plugin-resource", "freeze")
	if err != nil {
		return "", err
	}
	if len(expectedKinds) != 0 {
		matched := false
		for _, expected := range expectedKinds {
			matched = matched || kind == expected
		}
		if !matched {
			return "", invalidf("command payload asset kind is unsupported in this context")
		}
	}
	if err := payloadPositiveUint64(body, "byteSize"); err != nil {
		return "", err
	}
	if _, err := payloadString(body, "originalName", 4096, true); err != nil {
		return "", err
	}
	if metadata, exists := body["audioMetadata"]; exists {
		if err := validateAudioMetadata(metadata); err != nil {
			return "", err
		}
	}
	return kind, nil
}

func validateAudioMetadata(raw json.RawMessage) error {
	body, err := commandPayloadObject(raw)
	if err != nil {
		return invalidf("command payload audioMetadata must be an object")
	}
	if err := exactPayloadKeys(body, nil, []string{"mimeType", "codec", "sampleRate", "channels", "frames"}); err != nil {
		return err
	}
	if _, exists := body["mimeType"]; exists {
		if _, err := payloadString(body, "mimeType", 255, true); err != nil {
			return err
		}
	}
	if _, exists := body["codec"]; exists {
		if _, err := payloadString(body, "codec", 255, true); err != nil {
			return err
		}
	}
	if _, exists := body["sampleRate"]; exists {
		if _, err := payloadNumber(body, "sampleRate", 0, 768000, true); err != nil {
			return err
		}
	}
	if _, exists := body["channels"]; exists {
		if _, err := payloadInteger(body, "channels", 1, 1024); err != nil {
			return err
		}
	}
	if _, exists := body["frames"]; exists {
		if err := payloadPositiveUint64(body, "frames"); err != nil {
			return err
		}
	}
	return nil
}

func validateCompSegmentPayload(raw json.RawMessage) error {
	body, err := commandPayloadObject(raw)
	if err != nil {
		return invalidf("command payload segment must be an object")
	}
	if err := exactPayloadKeys(body, []string{"id", "takeId", "startSeconds", "endSeconds"}, nil); err != nil {
		return err
	}
	if _, err := requiredPayloadUUID(body, "id"); err != nil {
		return err
	}
	if _, err := requiredPayloadUUID(body, "takeId"); err != nil {
		return err
	}
	start, err := payloadNumber(body, "startSeconds", 0, math.MaxFloat64, false)
	if err != nil {
		return err
	}
	end, err := payloadNumber(body, "endSeconds", 0, math.MaxFloat64, true)
	if err != nil {
		return err
	}
	if end-start < 0.001 {
		return invalidf("command payload comp segment must be at least 0.001 seconds long")
	}
	return nil
}

func validateBatchPayload(body map[string]json.RawMessage,
	schemaVersion int) error {
	if err := exactPayloadKeys(body, []string{"commands"}, nil); err != nil {
		return err
	}
	_, err := validateLockedCommandChildren(body["commands"], "batch", schemaVersion)
	return err
}

func validateRecordingCommitPayload(body map[string]json.RawMessage,
	schemaVersion int) error {
	if err := exactPayloadKeys(body, []string{"leases", "commands"}, nil); err != nil {
		return err
	}
	leaseReferences, err := recordingCommitLeaseReferencesFromRaw(body["leases"], schemaVersion)
	if err != nil {
		return err
	}
	commands, err := validateLockedCommandChildren(body["commands"],
		"recording commit", schemaVersion)
	if err != nil {
		return err
	}
	commandTracks := make(map[uuid.UUID]struct{}, len(leaseReferences))
	type clipTarget struct {
		TrackID uuid.UUID
		ClipID  uuid.UUID
	}
	newClips := make(map[clipTarget]struct{})
	for _, child := range commands {
		kind, _ := payloadString(child, "kind", 100, false)
		if !recordingCommitChildKindAllowed(kind) {
			return invalidf("recording commit child kind %s is unsupported", kind)
		}
		trackID, found, err := commandTargetTrackID(kind, child["payload"])
		if err != nil {
			return err
		}
		if found {
			commandTracks[trackID] = struct{}{}
		}
		if kind == "clip.add" {
			clipID, found, err := commandTargetClipID(child["payload"])
			if err != nil || !found {
				return invalidf("recording commit clip.add target is invalid")
			}
			newClips[clipTarget{TrackID: trackID, ClipID: clipID}] = struct{}{}
		}
	}
	if len(leaseReferences) == 0 {
		for _, child := range commands {
			kind, _ := payloadString(child, "kind", 100, false)
			trackID, trackFound, err := commandTargetTrackID(kind, child["payload"])
			if err != nil || !trackFound {
				return invalidf("lease-free recording child target is invalid")
			}
			clipID, clipFound, err := commandTargetClipID(child["payload"])
			if err != nil || !clipFound {
				return invalidf("lease-free recording child clip target is invalid")
			}
			if _, exists := newClips[clipTarget{TrackID: trackID, ClipID: clipID}]; !exists {
				return invalidf("lease-free recording commit may only mutate clips created in the same command")
			}
		}
		return nil
	}
	if len(commandTracks) != len(leaseReferences) {
		return invalidf("recording commit leases must exactly match command tracks")
	}
	for _, reference := range leaseReferences {
		if _, exists := commandTracks[reference.TrackID]; !exists {
			return invalidf("recording commit leases must exactly match command tracks")
		}
	}
	return nil
}

func commandTargetClipID(payload json.RawMessage) (uuid.UUID, bool, error) {
	body, err := commandPayloadObject(payload)
	if err != nil {
		return uuid.Nil, false, err
	}
	if _, exists := body["clipId"]; !exists {
		return uuid.Nil, false, nil
	}
	value, err := requiredCanonicalPayloadUUID(body, "clipId")
	if err != nil {
		return uuid.Nil, false, err
	}
	return value, true, nil
}

func recordingCommitChildKindAllowed(kind string) bool {
	switch kind {
	case "clip.add", "clip.setProperty", "clip.setAsset", "take.add", "compSegment.upsert":
		return true
	default:
		return false
	}
}

func commandTargetTrackID(kind string, payload json.RawMessage) (uuid.UUID, bool, error) {
	body, err := commandPayloadObject(payload)
	if err != nil {
		return uuid.Nil, false, err
	}
	if _, exists := body["trackId"]; exists {
		value, err := requiredPayloadUUID(body, "trackId")
		if err != nil {
			return uuid.Nil, false, err
		}
		parsed, _ := uuid.Parse(value)
		return parsed, true, nil
	}
	if strings.HasPrefix(kind, "plugin.") {
		location, err := validatePluginLocation(body["location"])
		if err != nil {
			return uuid.Nil, false, err
		}
		if location.TrackID != "" {
			parsed, _ := uuid.Parse(location.TrackID)
			return parsed, true, nil
		}
	}
	return uuid.Nil, false, nil
}

func validateLockedCommandChildren(raw json.RawMessage, label string,
	schemaVersion int) ([]map[string]json.RawMessage, error) {
	var commands []json.RawMessage
	if err := json.Unmarshal(raw, &commands); err != nil || len(commands) == 0 || len(commands) > maxBatchCommands {
		return nil, invalidf("%s must contain between 1 and %d commands", label, maxBatchCommands)
	}
	result := make([]map[string]json.RawMessage, 0, len(commands))
	for index, raw := range commands {
		child, err := commandPayloadObject(raw)
		if err != nil {
			return nil, invalidf("%s child %d must be an object", label, index)
		}
		if err := exactPayloadKeys(child, []string{"kind", "payload", "preconditions"}, nil); err != nil {
			return nil, invalidf("%s child %d: %v", label, index, err)
		}
		kind, err := payloadString(child, "kind", 100, false)
		if err != nil || kind == "batch" || kind == "recording.commit" {
			return nil, invalidf("%s child %d kind is unsupported", label, index)
		}
		if err := validateCommandPayloadShapeForSchema(kind, child["payload"], false,
			schemaVersion); err != nil {
			return nil, invalidf("%s child %d: %v", label, index, err)
		}
		if err := validateRawPreconditions(child["preconditions"]); err != nil {
			return nil, invalidf("%s child %d: %v", label, index, err)
		}
		result = append(result, child)
	}
	return result, nil
}

func recordingCommitLeaseReferences(payload json.RawMessage,
	schemaVersion int) ([]recordingLeaseReference, error) {
	body, err := commandPayloadObject(payload)
	if err != nil {
		return nil, err
	}
	return recordingCommitLeaseReferencesFromRaw(body["leases"], schemaVersion)
}

func recordingCommitLeaseReferencesFromRaw(raw json.RawMessage,
	schemaVersion int) ([]recordingLeaseReference, error) {
	var values []json.RawMessage
	if err := json.Unmarshal(raw, &values); err != nil ||
		(schemaVersion == CollaborationCommandSchemaVersion && len(values) == 0) ||
		len(values) > maxBatchCommands {
		minimum := 0
		if schemaVersion == CollaborationCommandSchemaVersion {
			minimum = 1
		}
		return nil, invalidf("recording commit must contain between %d and %d leases", minimum, maxBatchCommands)
	}
	result := make([]recordingLeaseReference, 0, len(values))
	seenTracks := make(map[uuid.UUID]struct{}, len(values))
	seenLeases := make(map[uuid.UUID]struct{}, len(values))
	for index, rawValue := range values {
		body, err := commandPayloadObject(rawValue)
		if err != nil {
			return nil, invalidf("recording commit lease %d must be an object", index)
		}
		if err := exactPayloadKeys(body, []string{"trackId", "leaseId"}, nil); err != nil {
			return nil, invalidf("recording commit lease %d: %v", index, err)
		}
		trackID, err := requiredCanonicalPayloadUUID(body, "trackId")
		if err != nil {
			return nil, invalidf("recording commit lease %d: %v", index, err)
		}
		leaseID, err := requiredCanonicalPayloadUUID(body, "leaseId")
		if err != nil {
			return nil, invalidf("recording commit lease %d: %v", index, err)
		}
		if _, exists := seenTracks[trackID]; exists {
			return nil, invalidf("recording commit contains duplicate trackId")
		}
		if _, exists := seenLeases[leaseID]; exists {
			return nil, invalidf("recording commit contains duplicate leaseId")
		}
		seenTracks[trackID] = struct{}{}
		seenLeases[leaseID] = struct{}{}
		result = append(result, recordingLeaseReference{TrackID: trackID, LeaseID: leaseID})
	}
	return result, nil
}

func requiredCanonicalPayloadUUID(body map[string]json.RawMessage, name string) (uuid.UUID, error) {
	value, err := requiredPayloadString(body, name)
	if err != nil {
		return uuid.Nil, err
	}
	parsed, err := uuid.Parse(value)
	if err != nil || parsed == uuid.Nil || parsed.String() != value {
		return uuid.Nil, invalidf("command payload %s must be a canonical UUID", name)
	}
	return parsed, nil
}

func validateRawPreconditions(raw json.RawMessage) error {
	var values []json.RawMessage
	if err := json.Unmarshal(raw, &values); err != nil || values == nil || len(values) > MaxOperationPreconditions {
		return invalidf("preconditions must be an array with at most %d items", MaxOperationPreconditions)
	}
	for _, rawValue := range values {
		body, err := commandPayloadObject(rawValue)
		if err != nil {
			return invalidf("field precondition must be an object")
		}
		if err := exactPayloadKeys(body, []string{"kind", "fieldKey", "operationId"}, nil); err != nil {
			return err
		}
		if kind, err := payloadString(body, "kind", 32, false); err != nil || kind != "fieldWriterIs" {
			return invalidf("field precondition kind must be fieldWriterIs")
		}
		fieldKey, err := payloadString(body, "fieldKey", 2048, false)
		if err != nil {
			return err
		}
		if err := validateFieldKey(fieldKey); err != nil {
			return err
		}
		if _, err := requiredPayloadUUID(body, "operationId"); err != nil {
			return err
		}
	}
	return nil
}

func exactPayloadKeys(body map[string]json.RawMessage, required, optional []string) error {
	allowed := make(map[string]bool, len(required)+len(optional))
	for _, name := range required {
		allowed[name] = true
		if _, exists := body[name]; !exists {
			return invalidf("command payload requires %s", name)
		}
	}
	for _, name := range optional {
		allowed[name] = true
	}
	unknown := make([]string, 0)
	for name := range body {
		if !allowed[name] {
			unknown = append(unknown, name)
		}
	}
	if len(unknown) != 0 {
		sort.Strings(unknown)
		return invalidf("command payload contains unsupported field %s", unknown[0])
	}
	return nil
}

func payloadString(body map[string]json.RawMessage, name string, maximumBytes int, allowEmpty bool) (string, error) {
	raw, exists := body[name]
	if !exists {
		return "", invalidf("command payload requires %s", name)
	}
	var value string
	if err := json.Unmarshal(raw, &value); err != nil || !utf8.ValidString(value) || (!allowEmpty && value == "") || (maximumBytes > 0 && len(value) > maximumBytes) {
		return "", invalidf("command payload %s is not a valid bounded string", name)
	}
	return value, nil
}

func payloadEnum(body map[string]json.RawMessage, name string, values ...string) (string, error) {
	value, err := payloadString(body, name, 128, false)
	if err != nil {
		return "", err
	}
	for _, candidate := range values {
		if value == candidate {
			return value, nil
		}
	}
	return "", invalidf("command payload %s is unsupported", name)
}

func payloadBool(body map[string]json.RawMessage, name string) (bool, error) {
	raw, exists := body[name]
	if !exists {
		return false, invalidf("command payload requires %s", name)
	}
	var value bool
	if err := json.Unmarshal(raw, &value); err != nil {
		return false, invalidf("command payload %s must be a boolean", name)
	}
	return value, nil
}

func payloadNumber(body map[string]json.RawMessage, name string, minimum, maximum float64, exclusiveMinimum bool) (float64, error) {
	number, err := rawJSONNumber(body, name)
	if err != nil {
		return 0, err
	}
	value, err := number.Float64()
	if err != nil || math.IsInf(value, 0) || math.IsNaN(value) || value > maximum || (exclusiveMinimum && value <= minimum) || (!exclusiveMinimum && value < minimum) {
		return 0, invalidf("command payload %s is outside the supported numeric range", name)
	}
	return value, nil
}

func payloadInteger(body map[string]json.RawMessage, name string, minimum, maximum int64) (int64, error) {
	number, err := rawJSONNumber(body, name)
	if err != nil {
		return 0, err
	}
	value, ok := new(big.Rat).SetString(number.String())
	if !ok || !value.IsInt() || value.Num().Cmp(big.NewInt(minimum)) < 0 || value.Num().Cmp(big.NewInt(maximum)) > 0 {
		return 0, invalidf("command payload %s must be an integer in range", name)
	}
	return value.Num().Int64(), nil
}

func payloadPositiveUint64(body map[string]json.RawMessage, name string) error {
	number, err := rawJSONNumber(body, name)
	if err != nil {
		return err
	}
	value, ok := new(big.Rat).SetString(number.String())
	maximum := new(big.Int).SetUint64(math.MaxUint64)
	if !ok || !value.IsInt() || value.Sign() <= 0 || value.Num().Cmp(maximum) > 0 {
		return invalidf("command payload %s must be a positive uint64", name)
	}
	return nil
}

func rawJSONNumber(body map[string]json.RawMessage, name string) (json.Number, error) {
	raw, exists := body[name]
	if !exists {
		return "", invalidf("command payload requires %s", name)
	}
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.UseNumber()
	var value any
	if err := decoder.Decode(&value); err != nil {
		return "", invalidf("command payload %s must be a number", name)
	}
	if err := decoder.Decode(&struct{}{}); !errors.Is(err, io.EOF) {
		return "", invalidf("command payload %s must contain one number", name)
	}
	number, ok := value.(json.Number)
	if !ok {
		return "", invalidf("command payload %s must be a number", name)
	}
	return number, nil
}

func rawJSONNull(raw json.RawMessage) bool {
	return bytes.Equal(bytes.TrimSpace(raw), []byte("null"))
}

func optionalPayloadUUIDValue(body map[string]json.RawMessage, name string) (string, error) {
	if err := optionalPayloadUUID(body, name); err != nil {
		return "", err
	}
	var value string
	_ = json.Unmarshal(body[name], &value)
	value = strings.TrimSpace(value)
	if value == "" {
		return "", nil
	}
	parsed, _ := uuid.Parse(value)
	return parsed.String(), nil
}

func rejectSelfReferences(body map[string]json.RawMessage, entityID string, names ...string) error {
	for _, name := range names {
		if _, exists := body[name]; !exists {
			continue
		}
		value, err := optionalPayloadUUIDValue(body, name)
		if err != nil {
			return err
		}
		if value == entityID {
			return invalidf("command payload %s cannot reference the command entity itself", name)
		}
	}
	return nil
}
