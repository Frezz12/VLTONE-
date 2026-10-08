package api

import (
	"encoding/json"
	"fmt"
	"strings"

	"github.com/google/uuid"
	"gorm.io/datatypes"
	"gorm.io/gorm"

	"vltstudio/backend/internal/model"
)

type releaseHighlight struct {
	ID           string   `json:"id"`
	Tone         string   `json:"tone"`
	TitleRU      string   `json:"title_ru"`
	TitleEN      string   `json:"title_en"`
	LeadRU       string   `json:"lead_ru"`
	LeadEN       string   `json:"lead_en"`
	LabelRU      string   `json:"label_ru"`
	LabelEN      string   `json:"label_en"`
	DetailsRU    []string `json:"details_ru"`
	DetailsEN    []string `json:"details_en"`
	ScreenshotID string   `json:"screenshot_id"`
}

func releaseJSONHighlights(raw datatypes.JSON) []releaseHighlight {
	result := []releaseHighlight{}
	_ = json.Unmarshal(raw, &result)
	if result == nil {
		return []releaseHighlight{}
	}
	return result
}

func normalizedReleaseHighlights(values []releaseHighlight, published bool) ([]releaseHighlight, map[string]string) {
	fields := map[string]string{}
	if len(values) > 12 {
		fields["highlights"] = "Добавьте не больше 12 главных обновлений."
		return nil, fields
	}
	result := make([]releaseHighlight, 0, len(values))
	ids := map[string]bool{}
	toneAllowed := map[string]bool{"coral": true, "lilac": true, "lime": true, "sky": true, "peach": true, "mint": true, "rose": true, "periwinkle": true}
	for index, item := range values {
		item.ID = strings.TrimSpace(item.ID)
		validID := item.ID != "" && len(item.ID) <= 64
		for _, character := range item.ID {
			if !(character >= 'a' && character <= 'z') && !(character >= '0' && character <= '9') && character != '-' && character != '_' {
				validID = false
			}
		}
		if !validID || ids[item.ID] {
			fields["highlights"] = "Идентификаторы блоков должны быть уникальными и состоять из строчных букв, цифр, дефиса или подчёркивания."
		}
		ids[item.ID] = true
		if item.Tone == "" {
			item.Tone = "lime"
		}
		if !toneAllowed[item.Tone] {
			fields["highlights"] = "Выберите цвет блока из палитры VLTone."
		}
		for _, text := range []struct {
			value *string
			limit int
		}{
			{&item.TitleRU, 160}, {&item.TitleEN, 160}, {&item.LeadRU, 600}, {&item.LeadEN, 600}, {&item.LabelRU, 60}, {&item.LabelEN, 60},
		} {
			*text.value = strings.TrimSpace(*text.value)
			if len([]rune(*text.value)) > text.limit {
				fields["highlights"] = fmt.Sprintf("Текст блока %d превышает допустимую длину.", index+1)
			}
		}
		var valid bool
		item.DetailsRU, valid = normalizedReleaseList(item.DetailsRU)
		if !valid {
			fields["highlights"] = "Подробности блока: не больше 100 пунктов по 1000 символов."
		}
		item.DetailsEN, valid = normalizedReleaseList(item.DetailsEN)
		if !valid {
			fields["highlights"] = "Подробности блока: не больше 100 пунктов по 1000 символов."
		}
		item.ScreenshotID = strings.TrimSpace(item.ScreenshotID)
		if item.ScreenshotID != "" {
			if id, err := uuid.Parse(item.ScreenshotID); err != nil || id == uuid.Nil {
				fields["highlights"] = "Выберите загруженный скриншот для блока."
			} else {
				item.ScreenshotID = id.String()
			}
		}
		if published && (item.TitleRU == "" || item.TitleEN == "" || item.LeadRU == "" || item.LeadEN == "" || item.ScreenshotID == "") {
			fields["highlights"] = fmt.Sprintf("Блок %d: добавьте заголовок и краткий текст на двух языках и выберите скриншот.", index+1)
		}
		result = append(result, item)
	}
	return result, fields
}

func releaseHighlightImageErrors(db *gorm.DB, item model.Release) map[string]string {
	fields := map[string]string{}
	for _, highlight := range releaseJSONHighlights(item.Highlights) {
		if highlight.ScreenshotID == "" {
			continue
		}
		var count int64
		if err := db.Model(&model.ReleaseScreenshot{}).Where("id = ? AND release_id = ?", highlight.ScreenshotID, item.ID).Count(&count).Error; err != nil || count != 1 {
			fields["highlights"] = "Скриншот блока должен принадлежать этому релизу. Загрузите изображение и выберите его заново."
			break
		}
	}
	return fields
}
