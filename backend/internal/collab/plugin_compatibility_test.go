package collab

import (
	"encoding/json"
	"errors"
	"testing"

	"github.com/google/uuid"
)

func TestExternalPluginInsertIsV3Only(t *testing.T) {
	insertID, trackID := uuid.New(), uuid.New()
	insert := extensionInsert(insertID, "org.example.effect", []any{})
	insert["format"] = "clap"
	insert["vendor"] = "Example"
	insert["stateSchemaVersion"] = 0
	payload, err := json.Marshal(map[string]any{
		"location": extensionLocation("track", trackID, uuid.Nil),
		"insert":   insert, "afterId": "",
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := validateCommandPayloadShape("plugin.add", payload, true); !errors.Is(err, ErrValidation) {
		t.Fatalf("v2 accepted an external plugin: %v", err)
	}
	fields, _, err := deriveCommandMetadataForSchema("plugin.add", payload,
		true, CollaborationCommandSchemaV3)
	if err != nil || len(fields) == 0 {
		t.Fatalf("v3 rejected an external plugin: fields=%v err=%v", fields, err)
	}
}

func TestNormalizePluginReadinessPromotesOnlyExactReadyManifest(t *testing.T) {
	requirements, err := normalizePluginRequirements([]PluginRequirement{{
		Format: "vst3", NativeUID: "00112233445566778899aabbccddeeff",
		Vendor: "Vendor", Version: "1.2.3", StateSchemaVersion: 0,
		Kind: "effect", ChannelMode: "stereo",
	}})
	if err != nil {
		t.Fatal(err)
	}
	ready := PluginReadinessReport{Revision: 4, Plugins: []PluginReadinessResult{{
		Format: "vst3", NativeUID: requirements[0].NativeUID,
		Vendor: "Vendor", Version: "1.2.3", StateSchemaVersion: 0,
		Kind: "effect", ChannelMode: "stereo", Status: PluginReady,
	}}}
	_, status, role, err := normalizePluginReadiness(requirements, 4, ready, "editor")
	if err != nil || status != "ready" || role != "editor" {
		t.Fatalf("ready editor was not promoted: status=%q role=%q err=%v", status, role, err)
	}
	ready.Plugins[0].Status = PluginMissing
	_, status, role, err = normalizePluginReadiness(requirements, 4, ready, "editor")
	if err != nil || status != "blocked" || role != "viewer" {
		t.Fatalf("missing plugin did not downgrade editor: status=%q role=%q err=%v", status, role, err)
	}
	ready.Plugins[0].Status = PluginReady
	ready.Plugins[0].Version = "1.2.4"
	if _, _, _, err = normalizePluginReadiness(requirements, 4, ready, "editor"); !errors.Is(err, ErrValidation) {
		t.Fatalf("readiness for a different plugin contract was accepted: %v", err)
	}
	ready.Plugins[0].Version = "1.2.3"
	ready.StayViewer = true
	_, status, role, err = normalizePluginReadiness(requirements, 4, ready, "editor")
	if err != nil || status != "viewer" || role != "viewer" {
		t.Fatalf("explicit viewer was not accepted: status=%q role=%q err=%v", status, role, err)
	}
}

func TestNormalizePluginManifestRejectsInstallationDataAndStaleReports(t *testing.T) {
	_, err := normalizePluginRequirements([]PluginRequirement{{
		Format: "vst3", NativeUID: `C:\\Plugins\\secret.vst3`, Vendor: "Vendor",
		Version: "1.0.0", Kind: "effect", ChannelMode: "auto",
	}})
	if !errors.Is(err, ErrValidation) {
		t.Fatalf("plugin path escaped into manifest: %v", err)
	}
	requirements := []PluginRequirement{{
		Format: "clap", NativeUID: "org.example.effect", Vendor: "Vendor",
		Version: "1.0.0", Kind: "effect", ChannelMode: "auto",
	}}
	_, _, _, err = normalizePluginReadiness(requirements, 2,
		PluginReadinessReport{Revision: 1}, "editor")
	if !errors.Is(err, ErrValidation) {
		t.Fatalf("stale readiness was accepted: %v", err)
	}
}

func TestExternalPluginCommandRequiresExactSessionManifest(t *testing.T) {
	requirement := PluginRequirement{
		Format: "clap", NativeUID: "org.example.effect", Vendor: "Vendor",
		Version: "1.0.0", Kind: "effect", ChannelMode: "stereo",
	}
	payload, err := json.Marshal(map[string]any{
		"location": map[string]any{"chain": "track", "trackId": uuid.New(),
			"clipId": ""},
		"insert": map[string]any{
			"format": "clap", "uid": requirement.NativeUID,
			"vendor": requirement.Vendor, "pluginVersion": requirement.Version,
			"stateSchemaVersion": 0, "channelMode": "stereo",
		},
		"afterId": "",
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := requireExternalPluginCapabilities("plugin.add", payload,
		[]PluginRequirement{requirement}); err != nil {
		t.Fatalf("exact plugin requirement was rejected: %v", err)
	}
	wrong := requirement
	wrong.Version = "1.0.1"
	if err := requireExternalPluginCapabilities("plugin.add", payload,
		[]PluginRequirement{wrong}); !errors.Is(err, ErrPluginNotReady) {
		t.Fatalf("unnegotiated plugin version was accepted: %v", err)
	}
}
