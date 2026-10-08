package api

import (
	"bytes"
	"image"
	"image/png"
	"net/http"
	"strings"
	"testing"

	"gorm.io/datatypes"
	"vltstudio/backend/internal/model"
)

func TestReleaseHighlightsValidation(t *testing.T) {
	valid := releaseHighlight{ID: "mixer", Tone: "lime", TitleRU: " Микшер ", TitleEN: "Mixer", LeadRU: "Новые эффекты", LeadEN: "New effects", ScreenshotID: "30000000-0000-4000-8000-000000000001", DetailsRU: []string{"  Эффект  ", " "}}
	result, fields := normalizedReleaseHighlights([]releaseHighlight{valid}, true)
	if len(fields) != 0 || result[0].TitleRU != "Микшер" || len(result[0].DetailsRU) != 1 || result[0].DetailsRU[0] != "Эффект" || result[0].DetailsEN == nil {
		t.Fatalf("valid block not normalized: %+v %+v", result, fields)
	}
	if _, fields := normalizedReleaseHighlights([]releaseHighlight{{ID: "draft"}}, false); len(fields) != 0 {
		t.Fatalf("incomplete draft rejected: %v", fields)
	}
	if _, fields := normalizedReleaseHighlights([]releaseHighlight{{ID: "draft"}}, true); fields["highlights"] == "" {
		t.Fatal("incomplete block published")
	}
	for _, test := range []struct {
		name   string
		change func(*releaseHighlight)
	}{
		{"invalid ID", func(v *releaseHighlight) { v.ID = "Bad/ID" }},
		{"invalid tone", func(v *releaseHighlight) { v.Tone = "red" }},
		{"invalid screenshot", func(v *releaseHighlight) { v.ScreenshotID = "not-an-id" }},
		{"nil screenshot", func(v *releaseHighlight) { v.ScreenshotID = "00000000-0000-0000-0000-000000000000" }},
		{"long title", func(v *releaseHighlight) { v.TitleRU = strings.Repeat("я", 161) }},
		{"long lead", func(v *releaseHighlight) { v.LeadEN = strings.Repeat("x", 601) }},
		{"long detail", func(v *releaseHighlight) { v.DetailsRU = []string{strings.Repeat("я", 1001)} }},
		{"missing English", func(v *releaseHighlight) { v.TitleEN = " " }},
	} {
		t.Run(test.name, func(t *testing.T) {
			item := valid
			test.change(&item)
			if _, fields := normalizedReleaseHighlights([]releaseHighlight{item}, true); fields["highlights"] == "" {
				t.Fatal("invalid block accepted")
			}
		})
	}
	if _, fields := normalizedReleaseHighlights([]releaseHighlight{valid, valid}, false); fields["highlights"] == "" {
		t.Fatal("duplicate IDs accepted")
	}
	if _, fields := normalizedReleaseHighlights(make([]releaseHighlight, 13), false); fields["highlights"] == "" {
		t.Fatal("more than 12 blocks accepted")
	}
	if blocks, fields := normalizedReleaseHighlights(nil, true); len(fields) != 0 || blocks == nil {
		t.Fatal("legacy release without blocks must remain valid")
	}
}

func TestReleaseHighlightsOlderClientsPreserveBlocks(t *testing.T) {
	s := Server{}
	item := model.Release{Status: model.ReleaseDraft, Highlights: datatypes.JSON([]byte(`[{"id":"existing","tone":"lime"}]`))}
	before := string(item.Highlights)
	if fields := s.releaseFromInput(&item, releaseInput{Version: "0.3.2"}); len(fields) != 0 || string(item.Highlights) != before {
		t.Fatalf("older client erased blocks: %v", fields)
	}
	empty := []releaseHighlight{}
	if fields := s.releaseFromInput(&item, releaseInput{Version: "0.3.2", Highlights: &empty}); len(fields) != 0 || string(item.Highlights) != "[]" {
		t.Fatalf("explicit removal did not work: %v", fields)
	}
}

func checkReleaseHighlights(t *testing.T, router http.Handler, releaseID string, content map[string]any, cookies []*http.Cookie, headers map[string]string) {
	t.Helper()
	var imageData bytes.Buffer
	if err := png.Encode(&imageData, image.NewNRGBA(image.Rect(0, 0, 1280, 720))); err != nil {
		t.Fatal(err)
	}
	shot := performMultipart(router, http.MethodPost, "/v1/admin/releases/"+releaseID+"/screenshots", "file", "mixer.png", imageData.Bytes(), "203.0.113.40:1234", cookies, headers)
	if shot.Status != http.StatusCreated {
		t.Fatalf("screenshot upload failed: %d %v", shot.Status, shot.Body)
	}
	shotID := shot.Body["id"].(string)
	caption := performJSON(router, http.MethodPut, "/v1/admin/releases/"+releaseID+"/screenshots/"+shotID, map[string]any{"caption_ru": "Микшер", "caption_en": "Mixer", "sort_order": 0}, "203.0.113.40:1234", cookies, headers)
	if caption.Status != http.StatusOK {
		t.Fatalf("caption save failed: %d %v", caption.Status, caption.Body)
	}
	block := releaseHighlight{ID: "mixer", Tone: "sky", TitleRU: "Новый микшер", TitleEN: "New mixer", LeadRU: "Эффекты для вашего трека", LeadEN: "Effects for your track", ScreenshotID: shotID}
	withBlocks := map[string]any{}
	for key, value := range content {
		withBlocks[key] = value
	}
	withBlocks["highlights"] = []releaseHighlight{block}
	saved := performJSON(router, http.MethodPut, "/v1/admin/releases/"+releaseID, withBlocks, "203.0.113.40:1234", cookies, headers)
	if saved.Status != http.StatusOK || len(saved.Body["highlights"].([]any)) != 1 {
		t.Fatalf("blocks not saved: %d %v", saved.Status, saved.Body)
	}
	legacy := performJSON(router, http.MethodPut, "/v1/admin/releases/"+releaseID, content, "203.0.113.40:1234", cookies, headers)
	if legacy.Status != http.StatusOK || len(legacy.Body["highlights"].([]any)) != 1 {
		t.Fatal("legacy update erased blocks")
	}
	block.ScreenshotID = "30000000-0000-4000-8000-000000000099"
	withBlocks["highlights"] = []releaseHighlight{block}
	invalid := performJSON(router, http.MethodPut, "/v1/admin/releases/"+releaseID, withBlocks, "203.0.113.40:1234", cookies, headers)
	if invalid.Status != http.StatusUnprocessableEntity {
		t.Fatalf("foreign screenshot accepted: %d %v", invalid.Status, invalid.Body)
	}
	remove := performJSON(router, http.MethodDelete, "/v1/admin/releases/"+releaseID+"/screenshots/"+shotID, nil, "203.0.113.40:1234", cookies, headers)
	if remove.Status != http.StatusUnprocessableEntity || remove.Body["code"] != "screenshot_in_use" {
		t.Fatalf("linked screenshot deleted: %d %v", remove.Status, remove.Body)
	}
}
