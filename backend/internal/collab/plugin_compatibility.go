package collab

import (
	"encoding/json"
	"sort"
	"strconv"
	"strings"
	"unicode"
)

const MaxPluginRequirements = 512

const (
	PluginReady           = "ready"
	PluginMissing         = "missing"
	PluginVersionMismatch = "version_mismatch"
	PluginProbeFailed     = "probe_failed"
)

// PluginRequirement is deliberately a product/state contract, not an
// installation record. Paths, binary names and window coordinates never cross
// the collaboration boundary.
type PluginRequirement struct {
	Format             string `json:"format"`
	NativeUID          string `json:"nativeUid"`
	Vendor             string `json:"vendor"`
	Version            string `json:"version"`
	StateSchemaVersion int    `json:"stateSchemaVersion"`
	Kind               string `json:"kind"`
	ChannelMode        string `json:"channelMode"`
}

type PluginReadinessResult struct {
	Format             string `json:"format"`
	NativeUID          string `json:"nativeUid"`
	Vendor             string `json:"vendor"`
	Version            string `json:"version"`
	StateSchemaVersion int    `json:"stateSchemaVersion"`
	Kind               string `json:"kind"`
	ChannelMode        string `json:"channelMode"`
	Status             string `json:"status"`
	// BuildHMAC is an optional session-scoped proof. It is never a module hash,
	// and therefore cannot be correlated outside this session.
	BuildHMAC string `json:"buildHmac,omitempty"`
}

type PluginReadinessReport struct {
	Revision   int64                   `json:"revision"`
	StayViewer bool                    `json:"stayViewer"`
	Plugins    []PluginReadinessResult `json:"plugins"`
}

func normalizePluginRequirements(values []PluginRequirement) ([]PluginRequirement, error) {
	if len(values) > MaxPluginRequirements {
		return nil, invalidf("plugin manifest contains too many requirements")
	}
	result := append([]PluginRequirement(nil), values...)
	seen := make(map[string]bool, len(result))
	for index := range result {
		value := &result[index]
		value.Format = strings.TrimSpace(value.Format)
		value.NativeUID = strings.TrimSpace(value.NativeUID)
		value.Vendor = strings.TrimSpace(value.Vendor)
		value.Version = strings.TrimSpace(value.Version)
		value.Kind = strings.TrimSpace(value.Kind)
		value.ChannelMode = strings.TrimSpace(value.ChannelMode)
		if !validPluginFormat(value.Format) ||
			!safePluginContractText(value.NativeUID, 400) ||
			!safePluginContractText(value.Vendor, 200) ||
			!safePluginContractText(value.Version, 200) ||
			(value.Kind != "instrument" && value.Kind != "effect") ||
			!validPluginChannelMode(value.ChannelMode) || value.StateSchemaVersion < 0 ||
			(value.Format == "internal" && value.StateSchemaVersion == 0) {
			return nil, invalidf("plugin requirement is invalid")
		}
		key := pluginRequirementKey(*value)
		if seen[key] {
			return nil, invalidf("plugin manifest contains duplicate requirements")
		}
		seen[key] = true
	}
	sort.Slice(result, func(left, right int) bool {
		return pluginRequirementKey(result[left]) < pluginRequirementKey(result[right])
	})
	return result, nil
}

func normalizePluginReadiness(requirements []PluginRequirement, revision int64,
	report PluginReadinessReport, projectRole string) (PluginReadinessReport,
	string, string, error) {
	if revision <= 0 || report.Revision != revision ||
		len(report.Plugins) > MaxPluginRequirements {
		return PluginReadinessReport{}, "", "", invalidf("plugin readiness revision is stale")
	}
	reportResults := make(map[string]PluginReadinessResult, len(report.Plugins))
	for _, result := range report.Plugins {
		result.Format = strings.TrimSpace(result.Format)
		result.NativeUID = strings.TrimSpace(result.NativeUID)
		result.Vendor = strings.TrimSpace(result.Vendor)
		result.Version = strings.TrimSpace(result.Version)
		result.Kind = strings.TrimSpace(result.Kind)
		result.ChannelMode = strings.TrimSpace(result.ChannelMode)
		result.Status = strings.TrimSpace(result.Status)
		result.BuildHMAC = strings.TrimSpace(result.BuildHMAC)
		if !validPluginFormat(result.Format) ||
			!safePluginContractText(result.NativeUID, 400) ||
			!safePluginContractText(result.Vendor, 200) ||
			!safePluginContractText(result.Version, 200) ||
			result.StateSchemaVersion < 0 ||
			(result.Format == "internal" && result.StateSchemaVersion == 0) ||
			(result.Kind != "instrument" && result.Kind != "effect") ||
			!validPluginChannelMode(result.ChannelMode) ||
			!validPluginReadinessStatus(result.Status) ||
			(result.BuildHMAC != "" && !validLowerHex(result.BuildHMAC, 64)) {
			return PluginReadinessReport{}, "", "", invalidf("plugin readiness result is invalid")
		}
		key := pluginReadinessKey(result)
		if _, exists := reportResults[key]; exists {
			return PluginReadinessReport{}, "", "", invalidf("plugin readiness contains duplicate results")
		}
		reportResults[key] = result
	}
	blocked := false
	for _, requirement := range requirements {
		key := pluginRequirementKey(requirement)
		result, exists := reportResults[key]
		if !exists {
			return PluginReadinessReport{}, "", "", invalidf("plugin readiness is incomplete")
		}
		blocked = blocked || result.Status != PluginReady
	}
	if len(reportResults) != len(requirements) {
		return PluginReadinessReport{}, "", "", invalidf("plugin readiness contains an unknown plugin")
	}
	sort.Slice(report.Plugins, func(left, right int) bool {
		leftKey := pluginReadinessKey(report.Plugins[left])
		rightKey := pluginReadinessKey(report.Plugins[right])
		return leftKey < rightKey
	})
	if projectRole == "viewer" || report.StayViewer {
		return report, "viewer", "viewer", nil
	}
	if blocked {
		return report, "blocked", "viewer", nil
	}
	return report, "ready", projectRole, nil
}

func marshalPluginRequirements(values []PluginRequirement) (json.RawMessage, error) {
	normalized, err := normalizePluginRequirements(values)
	if err != nil {
		return nil, err
	}
	return json.Marshal(normalized)
}

func unmarshalPluginRequirements(value json.RawMessage) ([]PluginRequirement, error) {
	if len(value) == 0 {
		value = json.RawMessage("[]")
	}
	var result []PluginRequirement
	if err := json.Unmarshal(value, &result); err != nil {
		return nil, invalidf("stored plugin manifest is invalid")
	}
	return normalizePluginRequirements(result)
}

func pluginRequirementKey(value PluginRequirement) string {
	return strings.Join([]string{value.Format, value.NativeUID, value.Vendor,
		value.Version, value.Kind, value.ChannelMode}, "\x00") +
		"\x00" + strconv.Itoa(value.StateSchemaVersion)
}

func pluginReadinessKey(value PluginReadinessResult) string {
	return pluginRequirementKey(PluginRequirement{
		Format: value.Format, NativeUID: value.NativeUID,
		Vendor: value.Vendor, Version: value.Version,
		StateSchemaVersion: value.StateSchemaVersion,
		Kind:               value.Kind, ChannelMode: value.ChannelMode,
	})
}

func validPluginFormat(value string) bool {
	switch value {
	case "internal", "clap", "vst3", "au", "vst":
		return true
	default:
		return false
	}
}

func validPluginChannelMode(value string) bool {
	switch value {
	case "auto", "mono", "stereo", "dual-mono":
		return true
	default:
		return false
	}
}

func validPluginReadinessStatus(value string) bool {
	switch value {
	case PluginReady, PluginMissing, PluginVersionMismatch, PluginProbeFailed:
		return true
	default:
		return false
	}
}

func safePluginContractText(value string, limit int) bool {
	if value == "" || len(value) > limit || strings.TrimSpace(value) != value ||
		strings.Contains(value, "//") || strings.Contains(value, "\\") ||
		strings.Contains(value, "://") {
		return false
	}
	for _, character := range value {
		if unicode.IsControl(character) {
			return false
		}
	}
	return true
}

func validLowerHex(value string, size int) bool {
	if len(value) != size {
		return false
	}
	for index := range value {
		if (value[index] < '0' || value[index] > '9') &&
			(value[index] < 'a' || value[index] > 'f') {
			return false
		}
	}
	return true
}

// requireExternalPluginCapabilities is the final server-side guard for v3
// plugin insertion. The JSON shape validator proves each field is well-typed;
// this check proves the exact product/state contract was negotiated in the
// immutable session manifest. A local UI gate is only advisory.
func requireExternalPluginCapabilities(kind string, payload json.RawMessage,
	requirements []PluginRequirement) error {
	approved := make(map[string]bool, len(requirements))
	for _, requirement := range requirements {
		approved[pluginRequirementKey(requirement)] = true
	}
	requested, err := externalPluginRequirements(kind, payload)
	if err != nil {
		return err
	}
	for _, requirement := range requested {
		if !approved[pluginRequirementKey(requirement)] {
			return ErrPluginNotReady
		}
	}
	return nil
}

func externalPluginRequirements(kind string, payload json.RawMessage) (
	[]PluginRequirement, error) {
	if kind == "batch" {
		var batch struct {
			Commands []struct {
				Kind    string          `json:"kind"`
				Payload json.RawMessage `json:"payload"`
			} `json:"commands"`
		}
		if err := json.Unmarshal(payload, &batch); err != nil {
			return nil, invalidf("plugin capability payload is invalid")
		}
		var result []PluginRequirement
		for _, command := range batch.Commands {
			values, err := externalPluginRequirements(command.Kind,
				command.Payload)
			if err != nil {
				return nil, err
			}
			result = append(result, values...)
		}
		return result, nil
	}
	if kind != "plugin.add" && kind != "plugin.replace" {
		return nil, nil
	}
	var command struct {
		Location struct {
			Chain string `json:"chain"`
		} `json:"location"`
		Insert      json.RawMessage `json:"insert"`
		Replacement json.RawMessage `json:"replacement"`
	}
	if err := json.Unmarshal(payload, &command); err != nil {
		return nil, invalidf("plugin capability payload is invalid")
	}
	insert := command.Insert
	if kind == "plugin.replace" {
		insert = command.Replacement
	}
	var wire struct {
		Format             string `json:"format"`
		NativeUID          string `json:"uid"`
		Vendor             string `json:"vendor"`
		Version            string `json:"pluginVersion"`
		StateSchemaVersion int    `json:"stateSchemaVersion"`
		ChannelMode        string `json:"channelMode"`
	}
	if err := json.Unmarshal(insert, &wire); err != nil {
		return nil, invalidf("plugin capability insert is invalid")
	}
	if wire.Format == "internal" {
		return nil, nil
	}
	requirement := PluginRequirement{
		Format: wire.Format, NativeUID: wire.NativeUID, Vendor: wire.Vendor,
		Version: wire.Version, StateSchemaVersion: wire.StateSchemaVersion,
		Kind: "effect", ChannelMode: wire.ChannelMode,
	}
	if command.Location.Chain == "instrument" {
		requirement.Kind = "instrument"
	}
	normalized, err := normalizePluginRequirements([]PluginRequirement{
		requirement})
	if err != nil {
		return nil, err
	}
	return normalized, nil
}
