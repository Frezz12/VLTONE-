package collab

import (
	"context"
	"time"

	"vltstudio/backend/internal/model"
)

// CountStuckEndingSessions is the low-cardinality source for the operational
// gauge. It exposes no project/session identifiers and does not participate in
// readiness, so monitoring cannot take the account API offline.
func (s *Store) CountStuckEndingSessions(ctx context.Context,
	olderThan time.Duration) (int64, error) {
	if s == nil || s.DB == nil || olderThan < 30*time.Second ||
		olderThan > 24*time.Hour {
		return 0, invalidf("stuck-ending threshold is outside safe bounds")
	}
	var count int64
	err := s.DB.WithContext(ctx).Model(&model.ProjectSession{}).
		Where("status = ? AND updated_at <= ?", model.ProjectSessionEnding,
			s.now().Add(-olderThan)).Count(&count).Error
	return count, err
}
