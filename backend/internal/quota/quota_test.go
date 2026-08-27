package quota

import (
	"testing"
	"time"

	"vltstudio/backend/internal/model"
)

func TestMonthUsesUTCBoundaries(t *testing.T) {
	local := time.Date(2026, time.September, 1, 1, 30, 0, 0, time.FixedZone("UTC+3", 3*60*60))
	start, end := Month(local)
	wantStart := time.Date(2026, time.August, 1, 0, 0, 0, 0, time.UTC)
	wantEnd := time.Date(2026, time.September, 1, 0, 0, 0, 0, time.UTC)
	if !start.Equal(wantStart) || !end.Equal(wantEnd) {
		t.Fatalf("Month() = %s..%s, want %s..%s", start, end, wantStart, wantEnd)
	}
}

func TestRemainingIncludesAdjustmentAndReservation(t *testing.T) {
	cycle := model.TokenCycle{BaseLimit: 20_000_000, Adjustment: 500, UsedTokens: 200, ReservedTokens: 100}
	if got := Remaining(cycle); got != 20_000_200 {
		t.Fatalf("Remaining() = %d", got)
	}
	cycle.UsedTokens = 30_000_000
	if got := Remaining(cycle); got != 0 {
		t.Fatalf("negative remaining was not clamped: %d", got)
	}
}
