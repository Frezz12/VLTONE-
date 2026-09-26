package api

import (
	"context"
	"errors"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"
	"time"

	"vltstudio/backend/internal/model"
)

const diagnosticRetention = 90 * 24 * time.Hour

// A withdrawal remains effective for already received records even if the user
// later opts in again; use the append-only decision log, not just current state.
func diagnosticExpiry(table, timeColumn string) string {
	return fmt.Sprintf(`%s.%s < ? OR EXISTS (
 SELECT 1 FROM legal_acceptances a WHERE a.user_id = %s.user_id
 AND a.purpose = 'diagnostics' AND a.action = 'revoked'
 AND a.occurred_at >= %s.%s)`, table, timeColumn, table, table, timeColumn)
}

func (s *Server) RunPrivacyMaintenance(ctx context.Context) {
	// Do not activate retention changes before the operator completes the legal
	// release configuration. No migration or cleanup is performed by the website.
	if !s.Config.LegalProfile.Ready() {
		return
	}
	ticker := time.NewTicker(12 * time.Hour)
	defer ticker.Stop()
	for {
		run, cancel := context.WithTimeout(ctx, 2*time.Minute)
		if err := s.cleanupExpiredDiagnostics(run, time.Now().UTC()); err != nil && ctx.Err() == nil {
			log.Printf("diagnostic retention: %v", err)
		}
		cancel()
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
	}
}

func (s *Server) cleanupExpiredDiagnostics(ctx context.Context, now time.Time) error {
	cutoff := now.Add(-diagnosticRetention)
	db := s.DB.WithContext(ctx)
	// Drain batches within the run's timeout. Remove the row only after its
	// artifact is gone. A bad artifact must not prevent telemetry cleanup.
	var cleanupErr error
	for {
		var reports []model.CrashReport
		if err := db.Where(diagnosticExpiry("crash_reports", "created_at"), cutoff).Limit(500).Find(&reports).Error; err != nil {
			return err
		}
		for _, report := range reports {
			if report.ArtifactPath != "" {
				if err := removeDiagnosticArtifact(s.Config.StorageRoot, report.ArtifactPath); err != nil {
					cleanupErr = errors.Join(cleanupErr, fmt.Errorf("crash %s: %w", report.ID, err))
					continue
				}
			}
			if err := db.Delete(&report).Error; err != nil {
				return err
			}
		}
		if len(reports) < 500 || cleanupErr != nil {
			break
		}
	}
	// Session deletion cascades only when the entire session is expired. For
	// ongoing sessions retain newer events/samples and remove older records.
	for _, item := range []struct{ table, timestamp string }{
		{"telemetry_samples", "recorded_at"}, {"telemetry_events", "created_at"}, {"telemetry_sessions", "last_seen_at"},
	} {
		if err := db.Exec("DELETE FROM "+item.table+" WHERE "+diagnosticExpiry(item.table, item.timestamp), cutoff).Error; err != nil {
			return err
		}
	}
	return cleanupErr
}

func removeDiagnosticArtifact(storageRoot, artifact string) error {
	root, err := filepath.Abs(filepath.Join(storageRoot, "crashes"))
	if err != nil {
		return err
	}
	path, err := filepath.Abs(artifact)
	if err != nil {
		return err
	}
	rel, err := filepath.Rel(root, path)
	if err != nil || rel == "." || rel == ".." || strings.HasPrefix(rel, ".."+string(filepath.Separator)) || filepath.IsAbs(rel) {
		return errors.New("crash artifact is outside diagnostic storage")
	}
	// Uploads use a per-report directory. Resolve the parent to reject any
	// symlink escaping the storage root, and unlink only the named file.
	parent, err := filepath.EvalSymlinks(filepath.Dir(path))
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	resolvedRoot, err := filepath.EvalSymlinks(root)
	if err != nil {
		return err
	}
	parentRel, err := filepath.Rel(resolvedRoot, parent)
	if err != nil || parentRel == ".." || strings.HasPrefix(parentRel, ".."+string(filepath.Separator)) || filepath.IsAbs(parentRel) {
		return errors.New("crash artifact parent escapes diagnostic storage")
	}
	info, err := os.Lstat(path)
	if err == nil && info.IsDir() {
		return errors.New("crash artifact must be a file")
	}
	if err := os.Remove(path); err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	if parentRel != "." {
		_ = os.Remove(filepath.Dir(path))
	} // Only an empty directory.
	return nil
}
