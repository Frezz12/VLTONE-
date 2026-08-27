package api

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/google/uuid"
	"gorm.io/datatypes"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"

	"vltstudio/backend/internal/model"
)

type telemetryBatchRequest struct {
	Events []telemetryInputEvent `json:"events"`
}

type telemetryInputEvent struct {
	EventID    uuid.UUID       `json:"event_id"`
	SessionID  uuid.UUID       `json:"session_id"`
	Kind       string          `json:"kind"`
	OccurredAt time.Time       `json:"occurred_at"`
	Payload    json.RawMessage `json:"payload"`
}

type sessionStartedPayload struct {
	AppVersion string          `json:"app_version"`
	BuildID    string          `json:"build_id"`
	Hardware   json.RawMessage `json:"hardware"`
}

type telemetryPlugin struct {
	Name    string `json:"name"`
	Vendor  string `json:"vendor"`
	Version string `json:"version"`
	Format  string `json:"format"`
	Count   int    `json:"count"`
}

type telemetrySamplePayload struct {
	ProcessCPU    float64           `json:"process_cpu"`
	SystemCPU     float64           `json:"system_cpu"`
	DSPLoad       float64           `json:"dsp_load"`
	DSPPeak       float64           `json:"dsp_peak"`
	Xruns         int64             `json:"xruns"`
	ResidentBytes int64             `json:"resident_bytes"`
	SampleRate    float64           `json:"sample_rate"`
	BufferFrames  int               `json:"buffer_frames"`
	TrackCount    int               `json:"track_count"`
	ClipCount     int               `json:"clip_count"`
	PluginCount   int               `json:"plugin_count"`
	PlaybackState string            `json:"playback_state"`
	Recording     bool              `json:"recording"`
	Foreground    bool              `json:"foreground"`
	Plugins       []telemetryPlugin `json:"plugins"`
}

type sessionEndedPayload struct {
	Reason string `json:"reason"`
}

func (s *Server) telemetryBatch(w http.ResponseWriter, r *http.Request) {
	var input telemetryBatchRequest
	if !decodeJSON(w, r, &input) {
		return
	}
	if len(input.Events) == 0 || len(input.Events) > 500 {
		writeError(w, r, http.StatusUnprocessableEntity, "validation_failed", "A batch must contain between 1 and 500 events.", nil)
		return
	}
	user, device := userFrom(r), deviceFrom(r)
	accepted := 0
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		for _, event := range input.Events {
			if event.EventID == uuid.Nil || event.SessionID == uuid.Nil || event.OccurredAt.IsZero() {
				return errors.New("event identifiers and time are required")
			}
			if event.OccurredAt.Before(time.Now().UTC().AddDate(-1, 0, 0)) || event.OccurredAt.After(time.Now().UTC().Add(10*time.Minute)) {
				return errors.New("event time is outside accepted range")
			}
			var exists int64
			if err := tx.Model(&model.TelemetryEvent{}).Where("event_id = ?", event.EventID).Count(&exists).Error; err != nil {
				return err
			}
			if exists != 0 {
				continue
			}
			payload, err := s.applyTelemetryEvent(tx, user, device, event)
			if err != nil {
				return err
			}
			stored := model.TelemetryEvent{
				ID: uuid.New(), EventID: event.EventID, SessionID: event.SessionID,
				UserID: user.ID, DeviceID: device.ID, Kind: event.Kind,
				OccurredAt: event.OccurredAt.UTC(), Payload: payload,
			}
			if err := tx.Clauses(clause.OnConflict{DoNothing: true}).Create(&stored).Error; err != nil {
				return err
			}
			accepted++
		}
		return tx.Model(&device).Update("last_seen_at", time.Now().UTC()).Error
	})
	if err != nil {
		writeError(w, r, http.StatusUnprocessableEntity, "telemetry_batch_invalid", "Telemetry batch was rejected: "+err.Error(), nil)
		return
	}
	writeJSON(w, http.StatusAccepted, map[string]any{"accepted": accepted, "duplicates": len(input.Events) - accepted})
}

func (s *Server) applyTelemetryEvent(tx *gorm.DB, user model.User, device model.Device, event telemetryInputEvent) (datatypes.JSON, error) {
	switch event.Kind {
	case "session_started":
		var value sessionStartedPayload
		if err := strictUnmarshal(event.Payload, &value); err != nil {
			return nil, err
		}
		value.AppVersion = redactDiagnosticText(value.AppVersion, 64)
		value.BuildID = redactDiagnosticText(value.BuildID, 128)
		hardware := sanitizeHardware(value.Hardware)
		session := model.TelemetrySession{
			ID: event.SessionID, UserID: user.ID, DeviceID: device.ID,
			AppVersion: value.AppVersion, BuildID: value.BuildID,
			StartedAt: event.OccurredAt.UTC(), LastSeenAt: event.OccurredAt.UTC(), Hardware: hardware,
		}
		if err := tx.Clauses(clause.OnConflict{DoNothing: true}).Create(&session).Error; err != nil {
			return nil, err
		}
		return datatypes.JSON(jsonBytes(map[string]any{"app_version": value.AppVersion, "build_id": value.BuildID, "hardware": json.RawMessage(hardware)})), nil
	case "sample":
		var value telemetrySamplePayload
		if err := strictUnmarshal(event.Payload, &value); err != nil {
			return nil, err
		}
		if err := validateSample(&value); err != nil {
			return nil, err
		}
		plugins := datatypes.JSON(jsonBytes(value.Plugins))
		if err := ensureTelemetryPartition(tx, event.OccurredAt); err != nil {
			return nil, err
		}
		sample := model.TelemetrySample{
			ID: uuid.New(), EventID: event.EventID, SessionID: event.SessionID,
			UserID: user.ID, DeviceID: device.ID, RecordedAt: event.OccurredAt.UTC(),
			ProcessCPU: value.ProcessCPU, SystemCPU: value.SystemCPU, DSPLoad: value.DSPLoad,
			DSPPeak: value.DSPPeak, Xruns: value.Xruns, ResidentBytes: value.ResidentBytes,
			SampleRate: value.SampleRate, BufferFrames: value.BufferFrames,
			TrackCount: value.TrackCount, ClipCount: value.ClipCount, PluginCount: value.PluginCount,
			PlaybackState: value.PlaybackState, Recording: value.Recording,
			Foreground: value.Foreground, Plugins: plugins,
		}
		if err := tx.Create(&sample).Error; err != nil {
			return nil, err
		}
		if err := tx.Model(&model.TelemetrySession{}).Where("id = ? AND user_id = ? AND device_id = ?", event.SessionID, user.ID, device.ID).
			Update("last_seen_at", event.OccurredAt.UTC()).Error; err != nil {
			return nil, err
		}
		return datatypes.JSON(jsonBytes(value)), nil
	case "session_ended":
		var value sessionEndedPayload
		if err := strictUnmarshal(event.Payload, &value); err != nil {
			return nil, err
		}
		value.Reason = redactDiagnosticText(value.Reason, 64)
		result := tx.Model(&model.TelemetrySession{}).
			Where("id = ? AND user_id = ? AND device_id = ?", event.SessionID, user.ID, device.ID).
			Updates(map[string]any{"last_seen_at": event.OccurredAt.UTC(), "ended_at": event.OccurredAt.UTC(), "end_reason": value.Reason})
		if result.Error != nil || result.RowsAffected == 0 {
			return nil, errors.New("telemetry session not found")
		}
		return datatypes.JSON(jsonBytes(value)), nil
	default:
		return nil, fmt.Errorf("unsupported telemetry event kind %q", event.Kind)
	}
}

func ensureTelemetryPartition(tx *gorm.DB, recordedAt time.Time) error {
	start := time.Date(recordedAt.UTC().Year(), recordedAt.UTC().Month(), 1, 0, 0, 0, 0, time.UTC)
	end := start.AddDate(0, 1, 0)
	name := fmt.Sprintf("telemetry_samples_%04d_%02d", start.Year(), int(start.Month()))
	if err := tx.Exec("SELECT pg_advisory_xact_lock(hashtext(?))", name).Error; err != nil {
		return err
	}
	statement := fmt.Sprintf("CREATE TABLE IF NOT EXISTS %s PARTITION OF telemetry_samples FOR VALUES FROM ('%s') TO ('%s')",
		name, start.Format(time.RFC3339), end.Format(time.RFC3339))
	return tx.Exec(statement).Error
}

func strictUnmarshal(body []byte, out any) error {
	decoder := json.NewDecoder(strings.NewReader(string(body)))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(out); err != nil {
		return errors.New("event payload is invalid")
	}
	return nil
}

func validateSample(value *telemetrySamplePayload) error {
	if value.ProcessCPU < 0 || value.ProcessCPU > 10000 || value.SystemCPU < 0 || value.SystemCPU > 100 ||
		value.DSPLoad < 0 || value.DSPLoad > 10000 || value.DSPPeak < 0 || value.DSPPeak > 10000 ||
		value.Xruns < 0 || value.ResidentBytes < 0 || value.TrackCount < 0 || value.ClipCount < 0 ||
		value.PluginCount < 0 || len(value.Plugins) > 500 {
		return errors.New("telemetry values are outside accepted ranges")
	}
	if value.PlaybackState != "stopped" && value.PlaybackState != "playing" && value.PlaybackState != "paused" {
		return errors.New("playback state is invalid")
	}
	if value.Recording {
		value.PlaybackState = "recording"
	}
	for index := range value.Plugins {
		value.Plugins[index].Name = redactDiagnosticText(value.Plugins[index].Name, 160)
		value.Plugins[index].Vendor = redactDiagnosticText(value.Plugins[index].Vendor, 160)
		value.Plugins[index].Version = redactDiagnosticText(value.Plugins[index].Version, 64)
		value.Plugins[index].Format = redactDiagnosticText(value.Plugins[index].Format, 16)
		if value.Plugins[index].Count < 0 || value.Plugins[index].Count > 10_000 {
			return errors.New("plugin count is invalid")
		}
	}
	return nil
}

type crashMetadataInput struct {
	SessionID     *uuid.UUID      `json:"session_id"`
	ReportID      uuid.UUID       `json:"report_id"`
	BuildID       string          `json:"build_id"`
	AppVersion    string          `json:"app_version"`
	Platform      string          `json:"platform"`
	Reason        string          `json:"reason"`
	LastPlugin    string          `json:"last_plugin"`
	OccurredAt    time.Time       `json:"occurred_at"`
	ExceptionCode string          `json:"exception_code"`
	Signal        string          `json:"signal"`
	HealthSamples json.RawMessage `json:"health_samples"`
	Modules       json.RawMessage `json:"modules"`
}

func (s *Server) createCrashReport(w http.ResponseWriter, r *http.Request) {
	const maxUpload = int64(50 << 20)
	r.Body = http.MaxBytesReader(w, r.Body, maxUpload+(2<<20))
	if err := r.ParseMultipartForm(maxUpload + (2 << 20)); err != nil {
		writeError(w, r, http.StatusBadRequest, "crash_bundle_invalid", "Crash bundle is too large or invalid.", nil)
		return
	}
	var input crashMetadataInput
	if err := strictUnmarshal([]byte(r.FormValue("metadata")), &input); err != nil || input.ReportID == uuid.Nil || input.OccurredAt.IsZero() {
		writeError(w, r, http.StatusUnprocessableEntity, "crash_metadata_invalid", "Crash metadata is invalid.", nil)
		return
	}
	var existing model.CrashReport
	if s.DB.First(&existing, "id = ? AND user_id = ?", input.ReportID, userFrom(r).ID).Error == nil {
		writeJSON(w, http.StatusOK, map[string]any{"id": existing.ID, "duplicate": true})
		return
	}
	metadata := datatypes.JSON(jsonBytes(map[string]any{
		"exception_code": redactDiagnosticText(input.ExceptionCode, 64), "signal": redactDiagnosticText(input.Signal, 64),
		"health_samples": sanitizeCrashArray(input.HealthSamples, 600, map[string]bool{
			"recorded_at": true, "process_cpu": true, "system_cpu": true, "dsp_load": true,
			"dsp_peak": true, "xruns": true, "resident_bytes": true,
			"sample_rate": true, "buffer_frames": true, "track_count": true,
			"clip_count": true, "last_plugin": true, "heartbeat": true,
			"playback_state": true, "recording": true,
		}),
		"modules": sanitizeCrashArray(input.Modules, 1000, map[string]bool{
			"name": true, "version": true, "base_address": true,
		}),
	}))
	report := model.CrashReport{
		ID: input.ReportID, UserID: userFrom(r).ID, DeviceID: deviceFrom(r).ID,
		SessionID: input.SessionID, BuildID: redactDiagnosticText(input.BuildID, 128), AppVersion: redactDiagnosticText(input.AppVersion, 64),
		Platform: redactDiagnosticText(input.Platform, 16), Reason: redactDiagnosticText(input.Reason, 512), LastPlugin: redactDiagnosticText(input.LastPlugin, 160),
		Metadata: metadata, OccurredAt: input.OccurredAt.UTC(),
	}
	if report.SessionID != nil {
		var sessionCount int64
		if s.DB.Model(&model.TelemetrySession{}).
			Where("id = ? AND user_id = ? AND device_id = ?", *report.SessionID, report.UserID, report.DeviceID).
			Count(&sessionCount).Error != nil || sessionCount == 0 {
			report.SessionID = nil
		}
	}
	files := r.MultipartForm.File["artifact"]
	if len(files) > 1 {
		writeError(w, r, http.StatusUnprocessableEntity, "crash_bundle_invalid", "Only one crash artifact is allowed.", nil)
		return
	}
	if len(files) == 1 {
		file, err := files[0].Open()
		if err != nil {
			writeError(w, r, http.StatusBadRequest, "crash_bundle_invalid", "Crash artifact could not be read.", nil)
			return
		}
		defer file.Close()
		body, err := io.ReadAll(io.LimitReader(file, maxUpload+1))
		if err != nil || int64(len(body)) > maxUpload {
			writeError(w, r, http.StatusUnprocessableEntity, "crash_bundle_too_large", "Crash artifact must be no larger than 50 MB.", nil)
			return
		}
		directory := filepath.Join(s.Config.StorageRoot, "crashes", report.ID.String())
		if err := os.MkdirAll(directory, 0o700); err != nil {
			writeError(w, r, http.StatusInternalServerError, "storage_unavailable", "Crash storage is unavailable.", nil)
			return
		}
		report.ArtifactPath = filepath.Join(directory, "bundle"+crashArtifactSuffix(files[0].Filename, body))
		if err := os.WriteFile(report.ArtifactPath, body, 0o600); err != nil {
			writeError(w, r, http.StatusInternalServerError, "storage_unavailable", "Crash artifact could not be stored.", nil)
			return
		}
		digest := sha256.Sum256(body)
		report.SHA256 = hex.EncodeToString(digest[:])
		report.ArtifactBytes = int64(len(body))
	}
	if err := s.DB.Create(&report).Error; err != nil {
		if report.ArtifactPath != "" {
			os.RemoveAll(filepath.Dir(report.ArtifactPath))
		}
		writeError(w, r, http.StatusInternalServerError, "crash_report_failed", "Crash report could not be saved.", nil)
		return
	}
	go s.sendCrashNotification(report)
	writeJSON(w, http.StatusCreated, map[string]any{"id": report.ID, "created_at": report.CreatedAt})
}

func crashArtifactSuffix(filename string, body []byte) string {
	switch strings.ToLower(filepath.Ext(filename)) {
	case ".log":
		if utf8.Valid(body) {
			return ".log"
		}
	case ".json":
		if json.Valid(body) {
			return ".json"
		}
	}
	return ".bin"
}

func sanitizeCrashArray(raw json.RawMessage, max int, allowed map[string]bool) any {
	var values []map[string]any
	if json.Unmarshal(raw, &values) != nil || len(values) > max {
		return []any{}
	}
	clean := make([]map[string]any, 0, len(values))
	for _, value := range values {
		entry := make(map[string]any)
		for key, item := range value {
			if allowed[key] {
				switch typed := item.(type) {
				case string:
					entry[key] = redactDiagnosticText(typed, 256)
				case float64, bool, nil:
					entry[key] = typed
				}
			}
		}
		clean = append(clean, entry)
	}
	return clean
}

func redactDiagnosticText(value string, limit int) string {
	value = bounded(strings.TrimSpace(value), limit)
	// Diagnostic metadata never needs a local path. Treat both Windows and
	// POSIX separators as sensitive even if a custom client sends them.
	if strings.ContainsAny(value, `/\\`) {
		return "<path-redacted>"
	}
	return value
}
