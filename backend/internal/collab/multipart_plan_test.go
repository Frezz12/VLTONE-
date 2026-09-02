package collab

import (
	"errors"
	"testing"
	"time"

	"vltstudio/backend/internal/objectstore"
)

func TestMultipartPlanStaysWithinProviderCaps(t *testing.T) {
	plan, err := planMultipart(64<<30, 64<<20, 16<<20)
	if err != nil {
		t.Fatal(err)
	}
	if !plan.Enabled || plan.Parts < 2 || plan.Parts > objectstore.MaximumMultipartParts ||
		plan.PartSize < objectstore.MinimumMultipartBytes ||
		plan.PartSize > objectstore.MaximumMultipartBytes {
		t.Fatalf("invalid large-object plan: %+v", plan)
	}
	last, err := multipartPartBytes(64<<30, plan.PartSize, plan.Parts, plan.Parts)
	if err != nil || last <= 0 || last > plan.PartSize {
		t.Fatalf("invalid final part: bytes=%d err=%v", last, err)
	}
}

func TestMultipartPlanKeepsSmallObjectsSingleAndThresholdEdgeResumable(t *testing.T) {
	small, err := planMultipart(63<<20, 64<<20, 16<<20)
	if err != nil || small.Enabled {
		t.Fatalf("small object unexpectedly used multipart: %+v err=%v", small, err)
	}
	edge, err := planMultipart(10<<20, 10<<20, 5<<30)
	if err != nil {
		t.Fatal(err)
	}
	if !edge.Enabled || edge.Parts != 2 || edge.PartSize != 5<<20 {
		t.Fatalf("threshold edge did not produce two valid parts: %+v", edge)
	}
}

func TestMultipartPlanForcesObjectsPastSinglePutLimit(t *testing.T) {
	plan, err := planMultipart((5<<30)+1, 64<<30, 64<<20)
	if err != nil {
		t.Fatal(err)
	}
	if !plan.Enabled || plan.Parts < 2 {
		t.Fatalf("object above the S3 single-PUT limit was not forced to multipart: %+v", plan)
	}
}

func TestMissingMultipartPartPageUsesExplicitCursorBeyondOneHundred(t *testing.T) {
	uploaded := map[int]struct{}{2: {}, 4: {}}
	first, next := missingMultipartPartPage(250, 1, 100, uploaded)
	if len(first) != 100 || first[0] != 1 || first[1] != 3 || next == nil || *next != 103 {
		t.Fatalf("unexpected first multipart page: first=%v next=%v", first, next)
	}
	second, finalCursor := missingMultipartPartPage(250, *next, 100, uploaded)
	if len(second) != 100 || second[0] != 103 || finalCursor == nil || *finalCursor != 203 {
		t.Fatalf("explicit cursor did not advance: second=%v next=%v", second, finalCursor)
	}
	last, noMore := missingMultipartPartPage(250, *finalCursor, 100, uploaded)
	if len(last) != 48 || last[0] != 203 || last[len(last)-1] != 250 || noMore != nil {
		t.Fatalf("unexpected final multipart page: last=%v next=%v", last, noMore)
	}
}

func TestUploadPresignTTLNeverOutlivesSession(t *testing.T) {
	now := time.Date(2026, 8, 29, 12, 0, 0, 250_000_000, time.UTC)
	service := &AssetService{
		Now: func() time.Time { return now }, UploadURLTTL: 15 * time.Minute,
	}
	ttl, err := service.uploadPresignTTL(now.Add(2*time.Minute + 900*time.Millisecond))
	if err != nil || ttl != 2*time.Minute {
		t.Fatalf("sub-second expiry was not safely floored: ttl=%s err=%v", ttl, err)
	}
	if _, err := service.uploadPresignTTL(now.Add(900 * time.Millisecond)); !errors.Is(err, ErrUploadExpired) {
		t.Fatalf("sub-second upload session received a delegated URL: %v", err)
	}
}
