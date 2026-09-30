package collab

import "encoding/json"

func validateSharedAsset(raw json.RawMessage, kind string, nullable bool) error {
	if nullable && rawJSONNull(raw) {
		return nil
	}
	_, err := validateAssetRef(raw, kind)
	return err
}

func validateRenderCommand(kind string, body map[string]json.RawMessage) error {
	if _, err := requiredPayloadUUID(body, "trackId"); err != nil {
		return err
	}
	if kind == "track.setFreeze" {
		if err := exactPayloadKeys(body, []string{"trackId", "asset", "durationSeconds", "sampleRate"}, nil); err != nil {
			return err
		}
		if rawJSONNull(body["asset"]) {
			if _, err := payloadNumber(body, "durationSeconds", 0, 0, false); err != nil {
				return err
			}
			_, err := payloadNumber(body, "sampleRate", 0, 0, false)
			return err
		}
		if err := validateSharedAsset(body["asset"], "audio", false); err != nil {
			return err
		}
		if _, err := payloadNumber(body, "durationSeconds", 0, 1e9, true); err != nil {
			return err
		}
		_, err := payloadNumber(body, "sampleRate", 8000, 768000, false)
		return err
	}
	if err := exactPayloadKeys(body, []string{"trackId", "clipId", "source", "history", "versionId", "injection"}, nil); err != nil {
		return err
	}
	if _, err := requiredPayloadUUID(body, "clipId"); err != nil {
		return err
	}
	if err := validateRenderSource(body["source"]); err != nil {
		return err
	}
	history, err := renderArray(body["history"], 64)
	if err != nil {
		return err
	}
	versions := map[string]bool{}
	for _, raw := range history {
		v, err := commandPayloadObject(raw)
		if err != nil {
			return err
		}
		if err := exactPayloadKeys(v, []string{"id", "parentId", "label", "source"}, nil); err != nil {
			return err
		}
		id, err := requiredPayloadUUID(v, "id")
		if err != nil {
			return err
		}
		if versions[id] {
			return invalidf("duplicate render version")
		}
		if err := optionalPayloadUUID(v, "parentId"); err != nil {
			return err
		}
		var parent string
		_ = json.Unmarshal(v["parentId"], &parent)
		if parent != "" && !versions[parent] {
			return invalidf("render parent must precede its child")
		}
		versions[id] = true
		if _, err := payloadString(v, "label", 4096, true); err != nil {
			return err
		}
		if err := validateRenderSource(v["source"]); err != nil {
			return err
		}
	}
	if err := optionalPayloadUUID(body, "versionId"); err != nil {
		return err
	}
	var version string
	_ = json.Unmarshal(body["versionId"], &version)
	if version != "" && !versions[version] || len(history) > 0 && version == "" {
		return invalidf("invalid selected render version")
	}
	injection, err := commandPayloadObject(body["injection"])
	if err != nil {
		return err
	}
	if err := exactPayloadKeys(injection, []string{"stage", "anchorChannelId"}, nil); err != nil {
		return err
	}
	if _, err := payloadEnum(injection, "stage", "none", "trackSource", "beforeTrackFader", "beforeFolderFader", "beforeMasterFx", "beforeMasterFader"); err != nil {
		return err
	}
	anchor, err := payloadString(injection, "anchorChannelId", 36, true)
	if err != nil {
		return err
	}
	if anchor != "master" {
		return optionalPayloadUUID(injection, "anchorChannelId")
	}
	return nil
}

func renderArray(raw json.RawMessage, limit int) ([]json.RawMessage, error) {
	var result []json.RawMessage
	if len(raw) == 0 || rawJSONNull(raw) || json.Unmarshal(raw, &result) != nil || len(result) > limit {
		return nil, invalidf("invalid render array")
	}
	return result, nil
}

func validateRenderSource(raw json.RawMessage) error {
	body, err := commandPayloadObject(raw)
	if err != nil {
		return err
	}
	if err := exactPayloadKeys(body, []string{"asset", "durationSeconds", "offsetSeconds", "fadeInSeconds", "fadeOutSeconds", "fadeInCurve", "fadeOutCurve", "fadeInMode", "fadeOutMode", "gain", "pan", "channels", "takes", "comp", "compCrossfadeMs", "sampleEdit", "analysis", "warp"}, nil); err != nil {
		return err
	}
	if err := validateSharedAsset(body["asset"], "audio", true); err != nil {
		return err
	}
	duration, err := payloadNumber(body, "durationSeconds", 0, 1e9, false)
	if err != nil {
		return err
	}
	ranges := []struct {
		name     string
		min, max float64
	}{{"offsetSeconds", 0, 1e9}, {"fadeInSeconds", 0, duration}, {"fadeOutSeconds", 0, duration}, {"fadeInCurve", -1, 1}, {"fadeOutCurve", -1, 1}, {"gain", 0, 4}, {"pan", -1, 1}, {"compCrossfadeMs", 0, 20}}
	for _, r := range ranges {
		if _, err := payloadNumber(body, r.name, r.min, r.max, false); err != nil {
			return err
		}
	}
	for _, key := range []string{"fadeInMode", "fadeOutMode"} {
		if _, err := payloadEnum(body, key, "gain", "tape"); err != nil {
			return err
		}
	}
	if _, err := payloadInteger(body, "channels", 0, 1024); err != nil {
		return err
	}
	if err := validateClipSampleEdit(body["sampleEdit"]); err != nil {
		return err
	}
	if err := validateMusicalAnalysis(body["analysis"]); err != nil {
		return err
	}
	takes, err := renderArray(body["takes"], 1024)
	if err != nil {
		return err
	}
	takeIDs := map[string]bool{}
	for _, raw := range takes {
		if err := validateTakePayload(raw); err != nil {
			return err
		}
		take, _ := commandPayloadObject(raw)
		id, _ := requiredPayloadUUID(take, "id")
		if takeIDs[id] || take["notes"] != nil {
			return invalidf("invalid audio render take")
		}
		takeIDs[id] = true
		if err := validateSharedAsset(take["asset"], "audio", false); err != nil {
			return err
		}
	}
	segments, err := renderArray(body["comp"], 8192)
	if err != nil {
		return err
	}
	ids := map[string]bool{}
	end := 0.0
	for _, raw := range segments {
		if err := validateCompSegmentPayload(raw); err != nil {
			return err
		}
		seg, _ := commandPayloadObject(raw)
		id, _ := requiredPayloadUUID(seg, "id")
		takeID, _ := requiredPayloadUUID(seg, "takeId")
		if ids[id] || !takeIDs[takeID] {
			return invalidf("invalid render comp take")
		}
		ids[id] = true
		start, err := payloadNumber(seg, "startSeconds", end, duration, false)
		if err != nil {
			return err
		}
		end, err = payloadNumber(seg, "endSeconds", start+.001, duration, false)
		if err != nil {
			return err
		}
	}
	return validateRenderWarp(body["warp"])
}

func validateRenderWarp(raw json.RawMessage) error {
	body, err := commandPayloadObject(raw)
	if err != nil {
		return err
	}
	if err := exactPayloadKeys(body, []string{"enabled", "preservePitch", "mode", "baselineDurationSeconds", "sensitivity", "markers"}, nil); err != nil {
		return err
	}
	enabled, err := payloadBool(body, "enabled")
	if err != nil {
		return err
	}
	if _, err := payloadBool(body, "preservePitch"); err != nil {
		return err
	}
	if _, err := payloadInteger(body, "mode", 1, 4); err != nil {
		return err
	}
	if _, err := payloadNumber(body, "baselineDurationSeconds", 0, 1e9, false); err != nil {
		return err
	}
	if _, err := payloadNumber(body, "sensitivity", 0, 100, false); err != nil {
		return err
	}
	markers, err := renderArray(body["markers"], 8192)
	if err != nil {
		return err
	}
	if enabled && len(markers) < 2 {
		return invalidf("enabled warp requires two markers")
	}
	ids := map[string]bool{}
	source, target := -1.0, -1.0
	for _, raw := range markers {
		marker, err := commandPayloadObject(raw)
		if err != nil {
			return err
		}
		if err := exactPayloadKeys(marker, []string{"id", "sourceSeconds", "targetBeats", "locked"}, nil); err != nil {
			return err
		}
		id, err := requiredPayloadUUID(marker, "id")
		if err != nil {
			return err
		}
		if ids[id] {
			return invalidf("duplicate warp marker")
		}
		ids[id] = true
		s, err := payloadNumber(marker, "sourceSeconds", 0, 1e9, false)
		if err != nil {
			return err
		}
		t, err := payloadNumber(marker, "targetBeats", 0, 1e9, false)
		if err != nil {
			return err
		}
		if s <= source || t <= target {
			return invalidf("warp markers must be ordered")
		}
		source, target = s, t
		if _, err := payloadBool(marker, "locked"); err != nil {
			return err
		}
	}
	return nil
}

func commandContainsRender(kind string, payload json.RawMessage) bool {
	if kind == "track.setFreeze" || kind == "clip.setRenderState" {
		return true
	}
	if kind != "batch" && kind != "recording.commit" {
		return false
	}
	var body struct {
		Commands []struct {
			Kind    string          `json:"kind"`
			Payload json.RawMessage `json:"payload"`
		} `json:"commands"`
	}
	if json.Unmarshal(payload, &body) != nil {
		return false
	}
	for _, c := range body.Commands {
		if commandContainsRender(c.Kind, c.Payload) {
			return true
		}
	}
	return false
}
