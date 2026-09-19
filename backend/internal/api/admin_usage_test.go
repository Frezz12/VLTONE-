package api

import (
	"math"
	"net/http/httptest"
	"net/url"
	"testing"
	"time"

	"github.com/google/uuid"
	"vltstudio/backend/internal/model"
)

// Called by the PostgreSQL integration suite after its account/session setup.
func checkDashboardUsage(t *testing.T, s *Server, userID uuid.UUID) {
	t.Helper()
	var original model.TelemetrySession
	if err := s.DB.Where("user_id = ?", userID).First(&original).Error; err != nil {
		t.Fatal(err)
	}
	now := time.Now().UTC()
	request := httptest.NewRequest("GET", "/?usage_page=0", nil)
	before, err := s.dashboardUsage(request, now)
	if err != nil {
		t.Fatal(err)
	}
	ended, future := now.Add(-time.Hour), now.Add(time.Hour)
	fixtures := []model.TelemetrySession{
		{StartedAt: now.Add(-3 * time.Hour), LastSeenAt: ended, EndedAt: &ended},
		{StartedAt: now.Add(-30 * time.Hour), LastSeenAt: now.Add(-27 * time.Hour)},
		{StartedAt: now.Add(time.Hour), LastSeenAt: now},
		{StartedAt: now.Add(-time.Hour), LastSeenAt: future, EndedAt: &future},
	}
	ids := make([]uuid.UUID, len(fixtures))
	for i := range fixtures {
		fixtures[i].ID = uuid.New()
		ids[i] = fixtures[i].ID
		fixtures[i].UserID, fixtures[i].DeviceID = userID, original.DeviceID
		fixtures[i].AppVersion = "0.2.4"
	}
	if err := s.DB.Create(&fixtures).Error; err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.DB.Where("id IN ?", ids).Delete(&model.TelemetrySession{}) })
	after, err := s.dashboardUsage(request, now)
	if err != nil {
		t.Fatal(err)
	}
	if math.Abs(after.Summary.TotalSeconds-before.Summary.TotalSeconds-6*3600) > .01 || after.Summary.TotalSessions-before.Summary.TotalSessions != 4 {
		t.Fatalf("session durations (closed, stale, negative and future): before=%+v after=%+v", before.Summary, after.Summary)
	}
	var user model.User
	if err := s.DB.First(&user, "id = ?", userID).Error; err != nil {
		t.Fatal(err)
	}
	request.URL.RawQuery = "usage_q=" + userID.String() + "&usage_page=-1"
	empty, err := s.dashboardUsage(request, now)
	if err != nil || empty.Page != 0 || empty.UserCount != 0 || len(empty.Users) != 0 {
		t.Fatalf("empty filtered usage: %+v %v", empty, err)
	}
	request.URL.RawQuery = "usage_q=" + url.QueryEscape(user.Nickname)
	filtered, err := s.dashboardUsage(request, now)
	if err != nil || len(filtered.Users) != 1 || filtered.Users[0].UserID != userID || filtered.Users[0].Sessions < 4 {
		t.Fatalf("per-user usage: %+v %v", filtered, err)
	}
	if len(after.RecentSessions) == 0 || after.RecentSessions[0].ID != fixtures[2].ID {
		t.Fatal("recent sessions are not ordered by launch time")
	}
}
