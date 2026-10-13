package api

import (
	"github.com/go-chi/chi/v5"
	"github.com/google/uuid"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"
	"net/http"
	"regexp"
	"strings"
	"vltstudio/backend/internal/model"
)

func (s *Server) adminForum(w http.ResponseWriter, r *http.Request) {
	var reports []model.ForumReport
	var topics []model.ForumTopic
	var articles []model.ForumArticle
	var posts []model.ForumPost
	var comments []model.ForumComment
	for _, destination := range []any{&reports, &topics, &articles, &posts, &comments} {
		if forumFailure(w, r, s.DB.Order("created_at DESC").Limit(pageLimit(r)).Offset(pageOffset(r)).Find(destination).Error) {
			return
		}
	}
	writeJSON(w, 200, map[string]any{"reports": reports, "topics": topics, "articles": articles, "posts": posts, "comments": comments})
}
func (s *Server) adminForumContent(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "targetID")
	if !ok {
		return
	}
	kind := chi.URLParam(r, "targetType")
	table, ok := forumTargetTable(kind)
	if !ok {
		forumFailure(w, r, errForumInput)
		return
	}
	var in struct {
		Status         string     `json:"status"`
		Pinned         *bool      `json:"pinned"`
		Locked         *bool      `json:"locked"`
		AcceptedPostID *uuid.UUID `json:"accepted_post_id"`
	}
	if !decodeJSON(w, r, &in) {
		return
	}
	updates := map[string]any{}
	if in.Status != "" {
		if in.Status != "published" && in.Status != "hidden" {
			forumFailure(w, r, errForumInput)
			return
		}
		updates["status"] = in.Status
	}
	if kind == "topic" {
		if in.Pinned != nil {
			updates["pinned"] = *in.Pinned
		}
		if in.Locked != nil {
			updates["locked"] = *in.Locked
		}
		if in.AcceptedPostID != nil {
			var p model.ForumPost
			if forumFailure(w, r, s.DB.First(&p, "id = ? AND topic_id = ? AND status = 'published'", *in.AcceptedPostID, id).Error) {
				return
			}
			updates["accepted_post_id"] = *in.AcceptedPostID
		}
	}
	if len(updates) == 0 {
		forumFailure(w, r, errForumInput)
		return
	}
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		var old struct {
			Status    string
			TopicID   *uuid.UUID
			ArticleID *uuid.UUID
		}
		if e := tx.Table(table).Where("id = ?", id).Take(&old).Error; e != nil {
			return e
		}
		if old.TopicID != nil {
			var parent model.ForumTopic
			if e := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&parent, "id = ?", *old.TopicID).Error; e != nil {
				return e
			}
		}
		if old.ArticleID != nil {
			var parent model.ForumArticle
			if e := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&parent, "id = ?", *old.ArticleID).Error; e != nil {
				return e
			}
		}
		if e := tx.Table(table).Clauses(clause.Locking{Strength: "UPDATE"}).Where("id = ?", id).Take(&old).Error; e != nil {
			return e
		}
		if e := tx.Table(table).Where("id = ?", id).Updates(updates).Error; e != nil {
			return e
		}
		if in.Status != "" && old.Status != in.Status {
			delta := -1
			if in.Status == "published" {
				delta = 1
			}
			if kind == "post" {
				if e := tx.Model(&model.ForumTopic{}).Where("id = ?", old.TopicID).UpdateColumn("reply_count", gorm.Expr("GREATEST(reply_count + ?,0)", delta)).Error; e != nil {
					return e
				}
			}
			if kind == "comment" {
				if e := tx.Model(&model.ForumArticle{}).Where("id = ?", old.ArticleID).UpdateColumn("comment_count", gorm.Expr("GREATEST(comment_count + ?,0)", delta)).Error; e != nil {
					return e
				}
			}
			if kind == "article" && old.Status == "draft" && in.Status == "published" {
				tx.Table(table).Where("id = ?", id).UpdateColumn("published_at", gorm.Expr("now()"))
			}
		}
		return s.audit(tx, r, "forum.moderate", kind, id, updates)
	})
	if !forumFailure(w, r, err) {
		w.WriteHeader(204)
	}
}

var forumSlugPattern = regexp.MustCompile(`^[a-z0-9][a-z0-9-]{1,59}$`)

func (s *Server) adminForumSection(w http.ResponseWriter, r *http.Request) {
	var in model.ForumSection
	if !decodeJSON(w, r, &in) {
		return
	}
	if !forumSlugPattern.MatchString(in.Slug) || strings.TrimSpace(in.TitleRU) == "" || strings.TrimSpace(in.TitleEN) == "" || len(in.TitleRU) > 200 || len(in.TitleEN) > 200 || len(in.DescriptionRU) > 2000 || len(in.DescriptionEN) > 2000 {
		forumFailure(w, r, errForumInput)
		return
	}
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		if in.ID == uuid.Nil {
			in.ID = uuid.New()
			if e := tx.Create(&in).Error; e != nil {
				return e
			}
		} else {
			res := tx.Model(&model.ForumSection{}).Where("id = ?", in.ID).Updates(map[string]any{"slug": in.Slug, "title_ru": in.TitleRU, "title_en": in.TitleEN, "description_ru": in.DescriptionRU, "description_en": in.DescriptionEN, "sort_order": in.SortOrder})
			if res.Error != nil {
				return res.Error
			}
			if res.RowsAffected != 1 {
				return errForumInput
			}
		}
		return s.audit(tx, r, "forum.section", "forum_section", in.ID, map[string]any{"slug": in.Slug})
	})
	if !forumFailure(w, r, err) {
		writeJSON(w, 200, map[string]any{"section": in})
	}
}
func (s *Server) adminForumReport(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "reportID")
	if !ok {
		return
	}
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		res := tx.Model(&model.ForumReport{}).Where("id = ?", id).Update("status", "resolved")
		if res.Error != nil {
			return res.Error
		}
		if res.RowsAffected != 1 {
			return errForumInput
		}
		return s.audit(tx, r, "forum.report.resolve", "forum_report", id, nil)
	})
	if !forumFailure(w, r, err) {
		w.WriteHeader(204)
	}
}

func (s *Server) adminForumReadContent(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "targetID")
	if !ok {
		return
	}
	table, ok := forumTargetTable(chi.URLParam(r, "targetType"))
	if !ok {
		forumFailure(w, r, errForumInput)
		return
	}
	var content map[string]any
	if !forumFailure(w, r, s.DB.Table(table).Where("id = ?", id).Take(&content).Error) {
		writeJSON(w, 200, map[string]any{"content": content})
	}
}
