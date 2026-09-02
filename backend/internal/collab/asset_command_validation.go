package collab

import (
	"encoding/json"
	"math"

	"github.com/google/uuid"
	"gorm.io/gorm"
)

type commandAssetRequirement struct {
	AssetID uuid.UUID
	SHA256  string
	Bytes   int64
	Kind    string
}

func requireCommandAssetsReadyTx(tx *gorm.DB, projectID uuid.UUID,
	kind string, payload json.RawMessage) error {
	requirements, err := commandAssetRequirements(kind, payload, true)
	if err != nil {
		return err
	}
	if len(requirements) == 0 {
		return nil
	}
	ids := make([]uuid.UUID, 0, len(requirements))
	byID := make(map[uuid.UUID]commandAssetRequirement, len(requirements))
	for _, requirement := range requirements {
		if previous, exists := byID[requirement.AssetID]; exists {
			if previous != requirement {
				return invalidf("one asset identifier has conflicting immutable metadata")
			}
			continue
		}
		byID[requirement.AssetID] = requirement
		ids = append(ids, requirement.AssetID)
	}
	type readyAsset struct {
		AssetID uuid.UUID
		Kind    string
		SHA256  string
		Bytes   int64
		Status  string
	}
	var rows []readyAsset
	if err := tx.Table("project_assets AS assets").
		Select("assets.asset_id, assets.kind, blobs.sha256, blobs.bytes, blobs.status").
		Joins("JOIN blobs ON blobs.id = assets.blob_id").
		Where("assets.project_id = ? AND assets.asset_id IN ?", projectID, ids).
		Scan(&rows).Error; err != nil {
		return err
	}
	if len(rows) != len(byID) {
		return ErrAssetUnavailable
	}
	for _, row := range rows {
		expected, exists := byID[row.AssetID]
		if !exists || row.Status != BlobReady || !storageAssetKindMatches(expected.Kind, row.Kind) ||
			row.SHA256 != expected.SHA256 || row.Bytes != expected.Bytes {
			return ErrAssetUnavailable
		}
	}
	return nil
}

func commandAssetRequirements(kind string, payload json.RawMessage,
	allowBatch bool) ([]commandAssetRequirement, error) {
	switch kind {
	case "take.add":
		body, err := commandPayloadObject(payload)
		if err != nil {
			return nil, err
		}
		take, err := commandPayloadObject(body["take"])
		if err != nil {
			return nil, invalidf("take command contains an invalid asset reference")
		}
		requirement, err := assetRequirement(take["asset"])
		if err != nil || requirement.Kind != "audio" {
			return nil, invalidf("take command contains an invalid asset reference")
		}
		return []commandAssetRequirement{requirement}, nil
	case "clip.setAsset":
		body, err := commandPayloadObject(payload)
		if err != nil {
			return nil, err
		}
		if rawJSONNull(body["asset"]) {
			return nil, nil
		}
		requirement, err := assetRequirement(body["asset"])
		if err != nil || requirement.Kind != "audio" {
			return nil, invalidf("clip command contains an invalid asset reference")
		}
		return []commandAssetRequirement{requirement}, nil
	case "plugin.add":
		body, err := commandPayloadObject(payload)
		if err != nil {
			return nil, err
		}
		insert, err := commandPayloadObject(body["insert"])
		if err != nil {
			return nil, invalidf("plugin command contains an invalid insert")
		}
		return pluginAssetRequirements(insert)
	case "plugin.setState":
		body, err := commandPayloadObject(payload)
		if err != nil {
			return nil, err
		}
		return pluginAssetRequirements(body)
	case "plugin.setAssetBinding":
		body, err := commandPayloadObject(payload)
		if err != nil {
			return nil, err
		}
		binding, err := commandPayloadObject(body["binding"])
		if err != nil {
			return nil, invalidf("plugin command contains an invalid asset binding")
		}
		requirement, err := assetRequirement(binding["asset"])
		if err != nil {
			return nil, err
		}
		return []commandAssetRequirement{requirement}, nil
	case "batch", "recording.commit":
		if !allowBatch {
			return nil, invalidf("nested aggregate commands are unsupported")
		}
		var body struct {
			Commands []struct {
				Kind    string          `json:"kind"`
				Payload json.RawMessage `json:"payload"`
			} `json:"commands"`
		}
		if err := json.Unmarshal(payload, &body); err != nil {
			return nil, invalidf("batch command is invalid")
		}
		var result []commandAssetRequirement
		for _, command := range body.Commands {
			child, err := commandAssetRequirements(command.Kind, command.Payload, false)
			if err != nil {
				return nil, err
			}
			result = append(result, child...)
		}
		return result, nil
	default:
		return nil, nil
	}
}

func assetRequirement(raw json.RawMessage) (commandAssetRequirement, error) {
	body, err := commandPayloadObject(raw)
	if err != nil {
		return commandAssetRequirement{}, invalidf("command contains an invalid asset reference")
	}
	assetID, err := requiredPayloadUUID(body, "assetId")
	if err != nil {
		return commandAssetRequirement{}, err
	}
	parsedID, _ := uuid.Parse(assetID)
	digest, err := payloadString(body, "sha256", 64, false)
	if err != nil || !lowercaseSHA256(digest) {
		return commandAssetRequirement{}, invalidf("command contains an invalid asset digest")
	}
	kind, err := payloadEnum(body, "kind", "audio", "plugin-state", "plugin-resource", "freeze")
	if err != nil {
		return commandAssetRequirement{}, err
	}
	byteSize, err := payloadInteger(body, "byteSize", 1, math.MaxInt64)
	if err != nil {
		return commandAssetRequirement{}, err
	}
	return commandAssetRequirement{
		AssetID: parsedID, SHA256: digest, Bytes: byteSize, Kind: kind,
	}, nil
}

func pluginAssetRequirements(body map[string]json.RawMessage) ([]commandAssetRequirement, error) {
	result := make([]commandAssetRequirement, 0)
	for _, name := range []string{"stateAsset", "rightStateAsset"} {
		raw, exists := body[name]
		if !exists || rawJSONNull(raw) {
			continue
		}
		requirement, err := assetRequirement(raw)
		if err != nil || requirement.Kind != "plugin-state" {
			return nil, invalidf("plugin command contains an invalid state asset")
		}
		result = append(result, requirement)
	}
	var bindings []json.RawMessage
	if err := json.Unmarshal(body["assetBindings"], &bindings); err != nil || bindings == nil {
		return nil, invalidf("plugin command contains invalid asset bindings")
	}
	for _, raw := range bindings {
		binding, err := commandPayloadObject(raw)
		if err != nil {
			return nil, invalidf("plugin command contains an invalid asset binding")
		}
		requirement, err := assetRequirement(binding["asset"])
		if err != nil {
			return nil, err
		}
		result = append(result, requirement)
	}
	return result, nil
}

func storageAssetKindMatches(commandKind, storageKind string) bool {
	switch commandKind {
	case "audio":
		return storageKind == "audio" || storageKind == "sample"
	case "plugin-state":
		return storageKind == "plugin_state"
	case "plugin-resource":
		return storageKind == "sample" || storageKind == "other"
	case "freeze":
		return storageKind == "audio" || storageKind == "other"
	default:
		return false
	}
}
