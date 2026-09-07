package api

import "errors"

// Explicit metadata allowlist; opaque plugin state and file paths are never accepted.
type telemetrySnapshot struct {
	SchemaVersion            int              `json:"schema_version"`
	Truncated                bool             `json:"truncated"`
	WindowMS                 int64            `json:"window_ms"`
	MeasurementCount         int              `json:"measurement_count"`
	Tempo                    float64          `json:"tempo"`
	TimeSignatureNumerator   int              `json:"time_signature_numerator"`
	TimeSignatureDenominator int              `json:"time_signature_denominator"`
	PositionSeconds          float64          `json:"position_seconds"`
	LoopEnabled              bool             `json:"loop_enabled"`
	LoopStartSeconds         float64          `json:"loop_start_seconds"`
	LoopEndSeconds           float64          `json:"loop_end_seconds"`
	MasterVolume             float64          `json:"master_volume"`
	MasterPan                float64          `json:"master_pan"`
	MasterInserts            []telemetrySlot  `json:"master_inserts"`
	Tracks                   []telemetryTrack `json:"tracks"`
	Audio                    telemetryAudio   `json:"audio"`
}

type telemetryClipFX struct {
	ID              string          `json:"id"`
	Name            string          `json:"name"`
	Kind            string          `json:"kind"`
	StartSeconds    float64         `json:"start_seconds"`
	DurationSeconds float64         `json:"duration_seconds"`
	Muted           bool            `json:"muted"`
	Inserts         []telemetrySlot `json:"inserts"`
	OfflineInserts  []telemetrySlot `json:"offline_inserts"`
}

type telemetryTrack struct {
	ClipFX            []telemetryClipFX `json:"clip_fx"`
	ID                string            `json:"id"`
	Name              string            `json:"name"`
	Kind              string            `json:"kind"`
	ParentID          string            `json:"parent_id"`
	OutputBusID       string            `json:"output_bus_id"`
	Volume            float64           `json:"volume"`
	Pan               float64           `json:"pan"`
	Muted             bool              `json:"muted"`
	Soloed            bool              `json:"soloed"`
	Armed             bool              `json:"armed"`
	Monitor           bool              `json:"monitor"`
	Mono              bool              `json:"mono"`
	Frozen            bool              `json:"frozen"`
	InputEnabled      bool              `json:"input_enabled"`
	InputChannel      int               `json:"input_channel"`
	InputChannelCount int               `json:"input_channel_count"`
	ClipCount         int               `json:"clip_count"`
	Instrument        *telemetrySlot    `json:"instrument"`
	Inserts           []telemetrySlot   `json:"inserts"`
	SamplerFX         []telemetrySlot   `json:"sampler_fx"`
	SamplerFXActive   bool              `json:"sampler_fx_active"`
	Sends             []telemetrySend   `json:"sends"`
}

type telemetrySlot struct {
	ID               string  `json:"id"`
	Slot             int     `json:"slot"`
	Name             string  `json:"name"`
	Vendor           string  `json:"vendor"`
	Version          string  `json:"version"`
	Format           string  `json:"format"`
	Bypassed         bool    `json:"bypassed"`
	Mix              float64 `json:"mix"`
	ChannelMode      string  `json:"channel_mode"`
	SidechainTrackID string  `json:"sidechain_track_id"`
}

type telemetrySend struct {
	DestinationTrackID string  `json:"destination_track_id"`
	Level              float64 `json:"level"`
	PreFader           bool    `json:"pre_fader"`
	Enabled            bool    `json:"enabled"`
}

type telemetryAudio struct {
	Input            telemetryAudioDevice `json:"input"`
	Output           telemetryAudioDevice `json:"output"`
	Running          bool                 `json:"running"`
	InputEnabled     bool                 `json:"input_enabled"`
	InputChannels    []int                `json:"input_channels"`
	OutputChannels   []int                `json:"output_channels"`
	InputUnderflow   int64                `json:"input_underflow"`
	InputOverflow    int64                `json:"input_overflow"`
	OutputUnderflow  int64                `json:"output_underflow"`
	OutputOverflow   int64                `json:"output_overflow"`
	GatedBlocks      int64                `json:"gated_blocks"`
	Workers          int                  `json:"workers"`
	RealtimeWorkers  int                  `json:"realtime_workers"`
	WorkgroupWorkers int                  `json:"workgroup_workers"`
}

type telemetryAudioDevice struct {
	Name           string `json:"name"`
	Manufacturer   string `json:"manufacturer"`
	HostAPI        string `json:"host_api"`
	InputChannels  int    `json:"input_channels"`
	OutputChannels int    `json:"output_channels"`
	IsASIO         bool   `json:"is_asio"`
	IsAlive        bool   `json:"is_alive"`
}

func validateSnapshot(s *telemetrySnapshot) error {
	if s == nil {
		return nil
	} // Reports from older desktop versions remain readable.
	if s.SchemaVersion != 1 || len(s.Tracks) > 2048 || len(s.MasterInserts) > 256 || s.WindowMS < 0 || s.MeasurementCount < 0 || s.Tempo <= 0 || s.Tempo > 10000 {
		return errors.New("snapshot values are outside accepted ranges")
	}
	cleanID := func(value string) string { return redactDiagnosticText(value, 64) }
	cleanSlots := func(slots []telemetrySlot) error {
		if len(slots) > 256 {
			return errors.New("too many plugin slots")
		}
		for i := range slots {
			p := &slots[i]
			if p.Slot < 0 || p.Mix < 0 || p.Mix > 1 {
				return errors.New("invalid plugin slot")
			}
			p.ID = cleanID(p.ID)
			p.SidechainTrackID = cleanID(p.SidechainTrackID)
			p.Name = redactDiagnosticText(p.Name, 160)
			p.Vendor = redactDiagnosticText(p.Vendor, 160)
			p.Version = redactDiagnosticText(p.Version, 64)
			p.Format = redactDiagnosticText(p.Format, 16)
			p.ChannelMode = redactDiagnosticText(p.ChannelMode, 32)
		}
		return nil
	}
	if err := cleanSlots(s.MasterInserts); err != nil {
		return err
	}
	for i := range s.Tracks {
		t := &s.Tracks[i]
		t.ID = cleanID(t.ID)
		t.ParentID = cleanID(t.ParentID)
		t.OutputBusID = cleanID(t.OutputBusID)
		t.Name = redactDiagnosticText(t.Name, 160)
		t.Kind = redactDiagnosticText(t.Kind, 32)
		if t.ClipCount < 0 || t.InputChannel < 0 || t.InputChannelCount < 0 || t.Pan < -1 || t.Pan > 1 || t.Volume < 0 || len(t.Sends) > 256 || len(t.ClipFX) > 256 {
			return errors.New("invalid track metadata")
		}
		if t.Instrument != nil {
			slots := []telemetrySlot{*t.Instrument}
			if err := cleanSlots(slots); err != nil {
				return err
			}
			*t.Instrument = slots[0]
		}
		if err := cleanSlots(t.Inserts); err != nil {
			return err
		}
		if err := cleanSlots(t.SamplerFX); err != nil {
			return err
		}
		for j := range t.ClipFX {
			clip := &t.ClipFX[j]
			clip.ID = cleanID(clip.ID)
			clip.Name = redactDiagnosticText(clip.Name, 160)
			clip.Kind = redactDiagnosticText(clip.Kind, 32)
			if clip.DurationSeconds < 0 {
				return errors.New("invalid clip duration")
			}
			if err := cleanSlots(clip.Inserts); err != nil {
				return err
			}
			if err := cleanSlots(clip.OfflineInserts); err != nil {
				return err
			}
		}
		for j := range t.Sends {
			t.Sends[j].DestinationTrackID = cleanID(t.Sends[j].DestinationTrackID)
			if t.Sends[j].Level < 0 || t.Sends[j].Level > 1 {
				return errors.New("invalid send level")
			}
		}
	}
	a := &s.Audio
	if len(a.InputChannels) > 256 || len(a.OutputChannels) > 256 || a.InputUnderflow < 0 || a.InputOverflow < 0 || a.OutputUnderflow < 0 || a.OutputOverflow < 0 || a.GatedBlocks < 0 || a.Workers < 0 || a.RealtimeWorkers < 0 || a.WorkgroupWorkers < 0 {
		return errors.New("invalid audio metadata")
	}
	for _, d := range []*telemetryAudioDevice{&a.Input, &a.Output} {
		d.Name = redactDiagnosticText(d.Name, 160)
		d.Manufacturer = redactDiagnosticText(d.Manufacturer, 160)
		d.HostAPI = redactDiagnosticText(d.HostAPI, 64)
		if d.InputChannels < 0 || d.OutputChannels < 0 {
			return errors.New("invalid audio channels")
		}
	}
	return nil
}
