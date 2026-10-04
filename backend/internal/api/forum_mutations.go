package api

import (
	"encoding/json"
	"net/http"
	"strings"
	"time"

	"github.com/google/uuid"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"

	"vltstudio/backend/internal/model"
)

type forumTopicInput struct {
	SectionID uuid.UUID `json:"section_id"`
	Title     string    `json:"title"`
	Body      string    `json:"body"`
}

type forumPostInput struct {
	Body string `json:"body"`
}

type forumArticleInput struct {
	Title   string `json:"title"`
	Excerpt string `json:"excerpt"`
	Body    string `json:"body"`
	Status  string `json:"status"`
}

type forumCommentInput struct {
	Body string `json:"body"`
}

type forumReactionInput struct {
	TargetType string    `json:"target_type"`
	TargetID   uuid.UUID `json:"target_id"`
}

type forumProfileInput struct {
	About           string      `json:"about"`
	Location        string      `json:"location"`
	FamilyStatus    string      `json:"family_status"`
	Interests       string      `json:"interests"`
	Signature       string      `json:"signature"`
	Links           []forumLink `json:"links"`
	ProfileComplete *bool       `json:"profile_completed"`
}

type forumLink struct {
	Label string `json:"label"`
	URL   string `json:"url"`
}

func (s *Server) forumAllow(w http.ResponseWriter, r *http.Request, key string, limit int, window time.Duration) bool {
	user := userFrom(r)
	if !s.limiter.Allow("forum:"+key+":"+user.ID.String(), limit, window, time.Now().UTC()) {
		writeError(w, r, http.StatusTooManyRequests, "rate_limited",
			"You are doing that too often. Try again later.", nil)
		return false
	}
	return true
}

// POST /v1/forum/topics
func (s *Server) forumCreateTopic(w http.ResponseWriter, r *http.Request) {
	user := userFrom(r)
	if !s.forumAllow(w, r, "topic", 10, time.Hour) {
		return
	}
	var input forumTopicInput
	if !decodeJSON(w, r, &input) {
		return
	}
	input.Title = strings.TrimSpace(input.Title)
	input.Body = strings.TrimSpace(input.Body)
	fields := map[string]string{}
	forumLengthError(fields, "title", input.Title, 3, 200)
	forumLengthError(fields, "body", input.Body, 1, 100_000)
	if len(fields) > 0 {
		writeError(w, r, http.StatusUnprocessableEntity, "validation_failed", "Check the highlighted fields.", fields)
		return
	}
	var section model.ForumSection
	if err := s.DB.First(&section, "id = ?", input.SectionID).Error; err != nil {
		writeError(w, r, http.StatusNotFound, "section_not_found", "Section was not found.", nil)
		return
	}
	topic := model.ForumTopic{
		ID: uuid.New(), SectionID: section.ID, AuthorID: user.ID,
		Title: input.Title, Body: input.Body, Status: model.ForumStatusPublished,
		LastPostAt: time.Now().UTC(),
	}
	if err := s.DB.Create(&topic).Error; err != nil {
		writeError(w, r, http.StatusInternalServerError, "topic_create_failed", "Topic could not be created.", nil)
		return
	}
	writeJSON(w, http.StatusCreated, map[string]any{
		"topic": forumTopicView(topic, forumAuthor{ID: user.ID, Nickname: user.Nickname}, false),
	})
}

// PUT /v1/forum/topics/{topicID}
func (s *Server) forumUpdateTopic(w http.ResponseWriter, r *http.Request) {
	user := userFrom(r)
	topicID, ok := parseUUIDParam(w, r, "topicID")
	if !ok {
		return
	}
	var input forumTopicInput
	if !decodeJSON(w, r, &input) {
		return
	}
	input.Title = strings.TrimSpace(input.Title)
	input.Body = strings.TrimSpace(input.Body)
	fields := map[string]string{}
	forumLengthError(fields, "title", input.Title, 3, 200)
	forumLengthError(fields, "body", input.Body, 1, 100_000)
	if len(fields) > 0 {
		writeError(w, r, http.StatusUnprocessableEntity, "validation_failed", "Check the highlighted fields.", fields)
		return
	}
	var topic model.ForumTopic
	if err := s.DB.Where("id = ? AND author_id = ?", topicID, user.ID).First(&topic).Error; err != nil {
		writeError(w, r, http.StatusNotFound, "topic_not_found", "Topic was not found.", nil)
		return
	}
	topic.Title, topic.Body = input.Title, input.Body
	if err := s.DB.Save(&topic).Error; err != nil {
		writeError(w, r, http.StatusInternalServerError, "topic_update_failed", "Topic could not be updated.", nil)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"topic": forumTopicView(topic, forumAuthor{ID: user.ID, Nickname: user.Nickname}, false),
	})
}

// DELETE /v1/forum/topics/{topicID}
func (s *Server) forumDeleteTopic(w http.ResponseWriter, r *http.Request) {
	user := userFrom(r)
	topicID, ok := parseUUIDParam(w, r, "topicID")
	if !ok {
		return
	}
	result := s.DB.Where("id = ? AND author_id = ?", topicID, user.ID).Delete(&model.ForumTopic{})
	if result.Error != nil {
		writeError(w, r, http.StatusInternalServerError, "topic_delete_failed", "Topic could not be deleted.", nil)
		return
	}
	if result.RowsAffected == 0 {
		writeError(w, r, http.StatusNotFound, "topic_not_found", "Topic was not found.", nil)
		return
	}
	s.DB.Where("target_type = ? AND target_id = ?", model.ForumTargetTopic, topicID).
		Delete(&model.ForumReaction{})
	w.WriteHeader(http.StatusNoContent)
}

// POST /v1/forum/topics/{topicID}/posts
func (s *Server) forumCreatePost(w http.ResponseWriter, r *http.Request) {
	user := userFrom(r)
	if !s.forumAllow(w, r, "post", 30, 10*time.Minute) {
		return
	}
	topicID, ok := parseUUIDParam(w, r, "topicID")
	if !ok {
		return
	}
	var input forumPostInput
	if !decodeJSON(w, r, &input) {
		return
	}
	input.Body = strings.TrimSpace(input.Body)
	fields := map[string]string{}
	forumLengthError(fields, "body", input.Body, 1, 50_000)
	if len(fields) > 0 {
		writeError(w, r, http.StatusUnprocessableEntity, "validation_failed", "Check the highlighted fields.", fields)
		return
	}
	var topic model.ForumTopic
	if err := s.DB.Where("id = ? AND status = ?", topicID, model.ForumStatusPublished).
		First(&topic).Error; err != nil {
		writeError(w, r, http.StatusNotFound, "topic_not_found", "Topic was not found.", nil)
		return
	}
	if topic.Locked {
		writeError(w, r, http.StatusForbidden, "topic_locked", "This topic is locked.", nil)
		return
	}
	now := time.Now().UTC()
	post := model.ForumPost{
		ID: uuid.New(), TopicID: topic.ID, AuthorID: user.ID,
		Body: input.Body, Status: model.ForumStatusPublished,
	}
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Create(&post).Error; err != nil {
			return err
		}
		return tx.Model(&model.ForumTopic{}).Where("id = ?", topic.ID).Updates(map[string]any{
			"reply_count":  gorm.Expr("reply_count + 1"),
			"last_post_at": now,
		}).Error
	})
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "post_create_failed", "Reply could not be created.", nil)
		return
	}
	writeJSON(w, http.StatusCreated, map[string]any{
		"post": forumPostView(post, forumAuthor{ID: user.ID, Nickname: user.Nickname}, false),
	})
}

// PUT /v1/forum/posts/{postID}
func (s *Server) forumUpdatePost(w http.ResponseWriter, r *http.Request) {
	user := userFrom(r)
	postID, ok := parseUUIDParam(w, r, "postID")
	if !ok {
		return
	}
	var input forumPostInput
	if !decodeJSON(w, r, &input) {
		return
	}
	input.Body = strings.TrimSpace(input.Body)
	fields := map[string]string{}
	forumLengthError(fields, "body", input.Body, 1, 50_000)
	if len(fields) > 0 {
		writeError(w, r, http.StatusUnprocessableEntity, "validation_failed", "Check the highlighted fields.", fields)
		return
	}
	var post model.ForumPost
	if err := s.DB.Where("id = ? AND author_id = ?", postID, user.ID).First(&post).Error; err != nil {
		writeError(w, r, http.StatusNotFound, "post_not_found", "Reply was not found.", nil)
		return
	}
	post.Body = input.Body
	if err := s.DB.Save(&post).Error; err != nil {
		writeError(w, r, http.StatusInternalServerError, "post_update_failed", "Reply could not be updated.", nil)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"post": forumPostView(post, forumAuthor{ID: user.ID, Nickname: user.Nickname}, false),
	})
}

// DELETE /v1/forum/posts/{postID}
func (s *Server) forumDeletePost(w http.ResponseWriter, r *http.Request) {
	user := userFrom(r)
	postID, ok := parseUUIDParam(w, r, "postID")
	if !ok {
		return
	}
	var post model.ForumPost
	if err := s.DB.Where("id = ? AND author_id = ?", postID, user.ID).First(&post).Error; err != nil {
		writeError(w, r, http.StatusNotFound, "post_not_found", "Reply was not found.", nil)
		return
	}
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Delete(&post).Error; err != nil {
			return err
		}
		if err := tx.Where("target_type = ? AND target_id = ?", model.ForumTargetPost, postID).
			Delete(&model.ForumReaction{}).Error; err != nil {
			return err
		}
		return tx.Model(&model.ForumTopic{}).Where("id = ?", post.TopicID).
			UpdateColumn("reply_count", gorm.Expr("GREATEST(reply_count - 1, 0)")).Error
	})
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "post_delete_failed", "Reply could not be deleted.", nil)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

// POST /v1/forum/articles
func (s *Server) forumCreateArticle(w http.ResponseWriter, r *http.Request) {
	user := userFrom(r)
	if !s.forumAllow(w, r, "article", 10, time.Hour) {
		return
	}
	var input forumArticleInput
	if !decodeJSON(w, r, &input) {
		return
	}
	input.Title = strings.TrimSpace(input.Title)
	input.Excerpt = strings.TrimSpace(input.Excerpt)
	input.Body = strings.TrimSpace(input.Body)
	fields := map[string]string{}
	forumLengthError(fields, "title", input.Title, 3, 200)
	forumLengthError(fields, "excerpt", input.Excerpt, 0, 500)
	forumLengthError(fields, "body", input.Body, 1, 200_000)
	if len(fields) > 0 {
		writeError(w, r, http.StatusUnprocessableEntity, "validation_failed", "Check the highlighted fields.", fields)
		return
	}
	status := input.Status
	if status != model.ForumStatusDraft && status != model.ForumStatusPublished {
		status = model.ForumStatusPublished
	}
	article := model.ForumArticle{
		ID: uuid.New(), AuthorID: user.ID, Title: input.Title,
		Excerpt: input.Excerpt, Body: input.Body, Status: status,
	}
	if status == model.ForumStatusPublished {
		now := time.Now().UTC()
		article.PublishedAt = &now
	}
	if err := s.DB.Create(&article).Error; err != nil {
		writeError(w, r, http.StatusInternalServerError, "article_create_failed", "Article could not be created.", nil)
		return
	}
	writeJSON(w, http.StatusCreated, map[string]any{
		"article": forumArticleView(article, forumAuthor{ID: user.ID, Nickname: user.Nickname}, false),
	})
}

// PUT /v1/forum/articles/{articleID}
func (s *Server) forumUpdateArticle(w http.ResponseWriter, r *http.Request) {
	user := userFrom(r)
	articleID, ok := parseUUIDParam(w, r, "articleID")
	if !ok {
		return
	}
	var input forumArticleInput
	if !decodeJSON(w, r, &input) {
		return
	}
	input.Title = strings.TrimSpace(input.Title)
	input.Excerpt = strings.TrimSpace(input.Excerpt)
	input.Body = strings.TrimSpace(input.Body)
	fields := map[string]string{}
	forumLengthError(fields, "title", input.Title, 3, 200)
	forumLengthError(fields, "excerpt", input.Excerpt, 0, 500)
	forumLengthError(fields, "body", input.Body, 1, 200_000)
	if len(fields) > 0 {
		writeError(w, r, http.StatusUnprocessableEntity, "validation_failed", "Check the highlighted fields.", fields)
		return
	}
	var article model.ForumArticle
	if err := s.DB.Where("id = ? AND author_id = ?", articleID, user.ID).First(&article).Error; err != nil {
		writeError(w, r, http.StatusNotFound, "article_not_found", "Article was not found.", nil)
		return
	}
	article.Title, article.Excerpt, article.Body = input.Title, input.Excerpt, input.Body
	if input.Status == model.ForumStatusDraft || input.Status == model.ForumStatusPublished {
		if article.Status != model.ForumStatusPublished && input.Status == model.ForumStatusPublished {
			now := time.Now().UTC()
			article.PublishedAt = &now
		}
		article.Status = input.Status
	}
	if err := s.DB.Save(&article).Error; err != nil {
		writeError(w, r, http.StatusInternalServerError, "article_update_failed", "Article could not be updated.", nil)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"article": forumArticleView(article, forumAuthor{ID: user.ID, Nickname: user.Nickname}, false),
	})
}

// DELETE /v1/forum/articles/{articleID}
func (s *Server) forumDeleteArticle(w http.ResponseWriter, r *http.Request) {
	user := userFrom(r)
	articleID, ok := parseUUIDParam(w, r, "articleID")
	if !ok {
		return
	}
	result := s.DB.Where("id = ? AND author_id = ?", articleID, user.ID).Delete(&model.ForumArticle{})
	if result.Error != nil {
		writeError(w, r, http.StatusInternalServerError, "article_delete_failed", "Article could not be deleted.", nil)
		return
	}
	if result.RowsAffected == 0 {
		writeError(w, r, http.StatusNotFound, "article_not_found", "Article was not found.", nil)
		return
	}
	s.DB.Where("target_type = ? AND target_id = ?", model.ForumTargetArticle, articleID).
		Delete(&model.ForumReaction{})
	s.DB.Where("target_type = ? AND target_id IN (?)", model.ForumTargetComment,
		s.DB.Model(&model.ForumComment{}).Select("id").Where("article_id = ?", articleID)).
		Delete(&model.ForumReaction{})
	w.WriteHeader(http.StatusNoContent)
}

// POST /v1/forum/articles/{articleID}/comments
func (s *Server) forumCreateComment(w http.ResponseWriter, r *http.Request) {
	user := userFrom(r)
	if !s.forumAllow(w, r, "comment", 30, 10*time.Minute) {
		return
	}
	articleID, ok := parseUUIDParam(w, r, "articleID")
	if !ok {
		return
	}
	var input forumCommentInput
	if !decodeJSON(w, r, &input) {
		return
	}
	input.Body = strings.TrimSpace(input.Body)
	fields := map[string]string{}
	forumLengthError(fields, "body", input.Body, 1, 50_000)
	if len(fields) > 0 {
		writeError(w, r, http.StatusUnprocessableEntity, "validation_failed", "Check the highlighted fields.", fields)
		return
	}
	var article model.ForumArticle
	if err := s.DB.Where("id = ? AND status = ?", articleID, model.ForumStatusPublished).
		First(&article).Error; err != nil {
		writeError(w, r, http.StatusNotFound, "article_not_found", "Article was not found.", nil)
		return
	}
	comment := model.ForumComment{
		ID: uuid.New(), ArticleID: article.ID, AuthorID: user.ID,
		Body: input.Body, Status: model.ForumStatusPublished,
	}
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Create(&comment).Error; err != nil {
			return err
		}
		return tx.Model(&model.ForumArticle{}).Where("id = ?", article.ID).
			UpdateColumn("comment_count", gorm.Expr("comment_count + 1")).Error
	})
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "comment_create_failed", "Comment could not be created.", nil)
		return
	}
	writeJSON(w, http.StatusCreated, map[string]any{
		"comment": forumCommentView(comment, forumAuthor{ID: user.ID, Nickname: user.Nickname}, false),
	})
}

// DELETE /v1/forum/comments/{commentID}
func (s *Server) forumDeleteComment(w http.ResponseWriter, r *http.Request) {
	user := userFrom(r)
	commentID, ok := parseUUIDParam(w, r, "commentID")
	if !ok {
		return
	}
	var comment model.ForumComment
	if err := s.DB.Where("id = ? AND author_id = ?", commentID, user.ID).First(&comment).Error; err != nil {
		writeError(w, r, http.StatusNotFound, "comment_not_found", "Comment was not found.", nil)
		return
	}
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Delete(&comment).Error; err != nil {
			return err
		}
		if err := tx.Where("target_type = ? AND target_id = ?", model.ForumTargetComment, commentID).
			Delete(&model.ForumReaction{}).Error; err != nil {
			return err
		}
		return tx.Model(&model.ForumArticle{}).Where("id = ?", comment.ArticleID).
			UpdateColumn("comment_count", gorm.Expr("GREATEST(comment_count - 1, 0)")).Error
	})
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "comment_delete_failed", "Comment could not be deleted.", nil)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

// POST /v1/forum/reactions toggles a like. One write either inserts the row
// and bumps the counter or removes both, so the pair stays consistent.
func (s *Server) forumToggleReaction(w http.ResponseWriter, r *http.Request) {
	user := userFrom(r)
	if !s.forumAllow(w, r, "reaction", 60, 10*time.Minute) {
		return
	}
	var input forumReactionInput
	if !decodeJSON(w, r, &input) {
		return
	}
	table, ok := forumTargetTable(input.TargetType)
	if !ok {
		writeError(w, r, http.StatusUnprocessableEntity, "invalid_target", "Unsupported reaction target.", nil)
		return
	}
	var target struct {
		ID       uuid.UUID
		AuthorID uuid.UUID
		Status   string
	}
	if err := s.DB.Table(table).Select("id, author_id, status").Where("id = ?", input.TargetID).
		Scan(&target).Error; err != nil || target.ID == uuid.Nil {
		writeError(w, r, http.StatusNotFound, "target_not_found", "Target was not found.", nil)
		return
	}
	if target.Status != model.ForumStatusPublished && target.AuthorID != user.ID {
		writeError(w, r, http.StatusNotFound, "target_not_found", "Target was not found.", nil)
		return
	}
	now := time.Now().UTC()
	liked := false
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		reaction := model.ForumReaction{
			ID: uuid.New(), UserID: user.ID,
			TargetType: input.TargetType, TargetID: input.TargetID, CreatedAt: now,
		}
		result := tx.Clauses(clause.OnConflict{
			Columns:   []clause.Column{{Name: "target_type"}, {Name: "target_id"}, {Name: "user_id"}},
			DoNothing: true,
		}).Create(&reaction)
		if result.Error != nil {
			return result.Error
		}
		if result.RowsAffected > 0 {
			liked = true
			return tx.Table(table).Where("id = ?", input.TargetID).
				UpdateColumn("like_count", gorm.Expr("like_count + 1")).Error
		}
		deleted := tx.Where("user_id = ? AND target_type = ? AND target_id = ?",
			user.ID, input.TargetType, input.TargetID).Delete(&model.ForumReaction{})
		if deleted.Error != nil {
			return deleted.Error
		}
		if deleted.RowsAffected == 0 {
			// A concurrent request already removed it; report the current state.
			return nil
		}
		return tx.Table(table).Where("id = ?", input.TargetID).
			UpdateColumn("like_count", gorm.Expr("GREATEST(like_count - 1, 0)")).Error
	})
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "reaction_failed", "Reaction could not be saved.", nil)
		return
	}
	var likeCount int
	s.DB.Table(table).Select("like_count").Where("id = ?", input.TargetID).Scan(&likeCount)
	writeJSON(w, http.StatusOK, map[string]any{"liked": liked, "like_count": likeCount})
}

// PUT /v1/forum/me/profile
func (s *Server) forumUpdateProfile(w http.ResponseWriter, r *http.Request) {
	user := userFrom(r)
	if !s.forumAllow(w, r, "profile", 30, 10*time.Minute) {
		return
	}
	var input forumProfileInput
	if !decodeJSON(w, r, &input) {
		return
	}
	input.About = strings.TrimSpace(input.About)
	input.Location = strings.TrimSpace(input.Location)
	input.FamilyStatus = strings.TrimSpace(input.FamilyStatus)
	input.Interests = strings.TrimSpace(input.Interests)
	input.Signature = strings.TrimSpace(input.Signature)
	fields := map[string]string{}
	forumLengthError(fields, "about", input.About, 0, 5_000)
	forumLengthError(fields, "location", input.Location, 0, 200)
	forumLengthError(fields, "family_status", input.FamilyStatus, 0, 200)
	forumLengthError(fields, "interests", input.Interests, 0, 2_000)
	forumLengthError(fields, "signature", input.Signature, 0, 1_000)
	if len(input.Links) > 8 {
		fields["links"] = "Add no more than eight links."
	}
	for _, link := range input.Links {
		if len([]rune(link.Label)) > 80 || len([]rune(link.URL)) > 500 {
			fields["links"] = "Each link needs a label and URL within the allowed length."
			break
		}
	}
	if len(fields) > 0 {
		writeError(w, r, http.StatusUnprocessableEntity, "validation_failed", "Check the highlighted fields.", fields)
		return
	}
	linksJSON, err := json.Marshal(input.Links)
	if err != nil {
		writeError(w, r, http.StatusUnprocessableEntity, "validation_failed", "Links are invalid.", map[string]string{
			"links": "Links must be valid.",
		})
		return
	}
	profile, err := s.forumEnsureProfile(user.ID)
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "profile_unavailable", "Profile is unavailable.", nil)
		return
	}
	profile.About = input.About
	profile.Location = input.Location
	profile.FamilyStatus = input.FamilyStatus
	profile.Interests = input.Interests
	profile.Signature = input.Signature
	profile.Links = linksJSON
	if input.ProfileComplete != nil {
		profile.ProfileComplete = *input.ProfileComplete
	} else if !profile.ProfileComplete {
		profile.ProfileComplete = input.About != ""
	}
	if err := s.DB.Save(&profile).Error; err != nil {
		writeError(w, r, http.StatusInternalServerError, "profile_update_failed", "Profile could not be saved.", nil)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"profile": profile})
}

func forumTargetTable(targetType string) (string, bool) {
	switch targetType {
	case model.ForumTargetTopic:
		return "forum_topics", true
	case model.ForumTargetPost:
		return "forum_posts", true
	case model.ForumTargetArticle:
		return "forum_articles", true
	case model.ForumTargetComment:
		return "forum_comments", true
	default:
		return "", false
	}
}
