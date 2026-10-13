package api

import (
	"encoding/base64"
	"encoding/json"
	"errors"
	"net/http"
	"net/url"
	"sort"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/google/uuid"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"
	"vltstudio/backend/internal/model"
)

var errForumInput = errors.New("forum resource unavailable or invalid")

// Resolve visibility through ancestors as well as the target; a hidden topic
// must not leak its replies, reactions or attachments through another route.
func forumTarget(db *gorm.DB, kind string, id uuid.UUID, viewer uuid.UUID, lock bool) (uuid.UUID, error) {
	table, ok := forumTargetTable(kind)
	if !ok {
		return uuid.Nil, errForumInput
	}
	var row struct {
		AuthorID  uuid.UUID
		Status    string
		TopicID   *uuid.UUID
		ArticleID *uuid.UUID
	}
	// Read immutable parent IDs first, then lock root before child consistently
	// with publication, deletion and moderation.
	if err := db.Table(table).Where("id = ?", id).Take(&row).Error; err != nil {
		return uuid.Nil, err
	}
	if row.TopicID != nil {
		if _, err := forumTarget(db, "topic", *row.TopicID, viewer, lock); err != nil {
			return uuid.Nil, err
		}
	}
	if row.ArticleID != nil {
		if _, err := forumTarget(db, "article", *row.ArticleID, viewer, lock); err != nil {
			return uuid.Nil, err
		}
	}
	if lock {
		if err := db.Table(table).Clauses(clause.Locking{Strength: "UPDATE"}).Where("id = ?", id).Take(&row).Error; err != nil {
			return uuid.Nil, err
		}
	}
	if row.Status != "published" && row.AuthorID != viewer {
		return uuid.Nil, errForumInput
	}
	return row.AuthorID, nil
}
func viewerID(r *http.Request) uuid.UUID {
	if u, ok := viewerFrom(r); ok {
		return u.ID
	}
	return uuid.Nil
}
func forumFailure(w http.ResponseWriter, r *http.Request, err error) bool {
	if err == nil {
		return false
	}
	code := 422
	message := "The resource is unavailable or the request is invalid."
	if !errors.Is(err, errForumInput) && !errors.Is(err, gorm.ErrRecordNotFound) {
		code = 500
		message = "The request could not be completed. Please retry."
	}
	writeError(w, r, code, "forum_request_failed", message, nil)
	return true
}

func bindForumAttachments(tx *gorm.DB, owner uuid.UUID, kind string, id uuid.UUID, ids []uuid.UUID) error {
	column := map[string]string{"article": "article_id", "topic": "topic_id", "post": "post_id", "comment": "comment_id"}[kind]
	if column == "" || len(ids) > 10 {
		return errForumInput
	}
	var existing int64
	if err := tx.Model(&model.ForumAttachment{}).Where(column+" = ?", id).Count(&existing).Error; err != nil {
		return err
	}
	var videoCount int64
	tx.Model(&model.ForumAttachment{}).Where(column+" = ? AND kind = 'video'", id).Count(&videoCount)
	ids = append([]uuid.UUID(nil), ids...)
	sort.Slice(ids, func(i, j int) bool { return ids[i].String() < ids[j].String() })
	for _, attachmentID := range ids {
		var a model.ForumAttachment
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&a, "id = ? AND owner_id = ? AND status = 'ready'", attachmentID, owner).Error; err != nil {
			return err
		}
		var same bool
		switch kind {
		case "article":
			same = a.ArticleID != nil && *a.ArticleID == id
		case "topic":
			same = a.TopicID != nil && *a.TopicID == id
		case "post":
			same = a.PostID != nil && *a.PostID == id
		case "comment":
			same = a.CommentID != nil && *a.CommentID == id
		}
		if same {
			continue
		}
		if a.ArticleID != nil || a.TopicID != nil || a.PostID != nil || a.CommentID != nil {
			return errForumInput
		}
		existing++
		if a.Kind == "video" {
			videoCount++
		}
		if existing > 10 || videoCount > 1 {
			return errForumInput
		}
		if err := tx.Model(&a).Update(column, id).Error; err != nil {
			return err
		}
	}
	return nil
}
func (s *Server) saveForumContent(r *http.Request, value any, kind string, id uuid.UUID, ids []uuid.UUID, update bool) error {
	return s.DB.WithContext(r.Context()).Transaction(func(tx *gorm.DB) error {
		var err error
		if update {
			if _, e := forumTarget(tx, kind, id, userFrom(r).ID, true); e != nil {
				return e
			}
			table, _ := forumTargetTable(kind)
			var current struct {
				Status   string
				AuthorID uuid.UUID
			}
			if e := tx.Table(table).Where("id = ?", id).Take(&current).Error; e != nil {
				return e
			}
			if current.AuthorID != userFrom(r).ID || current.Status == "hidden" {
				return errForumInput
			}
			fields := []string{"body", "updated_at"}
			switch kind {
			case "topic":
				fields = append(fields, "title")
			case "article":
				fields = append(fields, "title", "excerpt", "kind", "status", "published_at")
			}
			err = tx.Model(value).Select(fields).Updates(value).Error
		} else {
			err = tx.Create(value).Error
		}
		if err != nil {
			return err
		}
		if err = bindForumAttachments(tx, userFrom(r).ID, kind, id, ids); err != nil {
			return err
		}
		// Empty short posts are allowed only when they actually own attachments.
		var body string
		switch v := value.(type) {
		case *model.ForumArticle:
			body = v.Body
		case *model.ForumTopic:
			body = v.Body
		case *model.ForumPost:
			body = v.Body
		case *model.ForumComment:
			body = v.Body
		}
		if strings.TrimSpace(body) == "" {
			column := map[string]string{"article": "article_id", "topic": "topic_id", "post": "post_id", "comment": "comment_id"}[kind]
			var count int64
			if err := tx.Model(&model.ForumAttachment{}).Where(column+" = ?", id).Count(&count).Error; err != nil {
				return err
			}
			if count == 0 {
				return errForumInput
			}
		}
		return tx.First(value, "id = ?", id).Error
	})
}
func forumNotify(tx *gorm.DB, to, actor uuid.UUID, kind, target string, id uuid.UUID) error {
	if to == actor {
		return nil
	}
	return tx.Create(&model.ForumNotification{ID: uuid.New(), UserID: to, ActorID: actor, Kind: kind, TargetType: target, TargetID: id}).Error
}

type forumCursor struct {
	At time.Time `json:"at"`
	ID uuid.UUID `json:"id"`
}

func encodeForumCursor(at time.Time, id uuid.UUID) string {
	b, _ := json.Marshal(forumCursor{at, id})
	return base64.RawURLEncoding.EncodeToString(b)
}
func decodeForumCursor(raw string) (forumCursor, error) {
	var c forumCursor
	if len(raw) > 256 {
		return c, errForumInput
	}
	b, e := base64.RawURLEncoding.DecodeString(raw)
	if e != nil {
		return c, e
	}
	e = json.Unmarshal(b, &c)
	if e == nil && (c.At.IsZero() || c.ID == uuid.Nil) {
		e = errForumInput
	}
	return c, e
}
func (s *Server) forumFeed(w http.ResponseWriter, r *http.Request) {
	q := s.DB.Where("status = 'published'")
	uid := viewerID(r)
	if r.URL.Query().Get("following") == "true" {
		if uid == uuid.Nil {
			writeError(w, r, 401, "authentication_required", "Sign in is required.", nil)
			return
		}
		q = q.Where("author_id IN (SELECT author_id FROM forum_follows WHERE user_id = ?)", uid)
	}
	if raw := r.URL.Query().Get("cursor"); raw != "" {
		c, e := decodeForumCursor(raw)
		if forumFailure(w, r, e) {
			return
		}
		q = q.Where("(published_at, id) < (?, ?)", c.At, c.ID)
	}
	limit := pageLimit(r)
	var rows []model.ForumArticle
	if forumFailure(w, r, q.Order("published_at DESC,id DESC").Limit(limit+1).Find(&rows).Error) {
		return
	}
	next := ""
	if len(rows) > limit {
		rows = rows[:limit]
		last := rows[len(rows)-1]
		next = encodeForumCursor(*last.PublishedAt, last.ID)
	}
	writeJSON(w, 200, map[string]any{"articles": s.forumArticleViews(r, rows, true), "next_cursor": next})
}
func (s *Server) forumFollow(w http.ResponseWriter, r *http.Request) {
	uid := userFrom(r).ID
	id, ok := parseUUIDParam(w, r, "userID")
	if !ok {
		return
	}
	if id == uid {
		forumFailure(w, r, errForumInput)
		return
	}
	if !s.forumAllow(w, r, "follow", 120, time.Hour) {
		return
	}
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		var u model.User
		if err := tx.First(&u, "id = ? AND status = 'active'", id).Error; err != nil {
			return err
		}
		// Serialize both follow and unfollow for this pair, including first insert.
		if err := tx.Exec("SELECT pg_advisory_xact_lock(hashtextextended(?,0))", uid.String()+":"+id.String()).Error; err != nil {
			return err
		}
		row := model.ForumFollow{UserID: uid, AuthorID: id}
		if r.Method == "DELETE" {
			return tx.Where("user_id = ? AND author_id = ?", uid, id).Delete(&row).Error
		}
		res := tx.Clauses(clause.OnConflict{DoNothing: true}).Create(&row)
		if res.Error != nil {
			return res.Error
		}
		if res.RowsAffected > 0 {
			return forumNotify(tx, id, uid, "follow", "user", uid)
		}
		return nil
	})
	if !forumFailure(w, r, err) {
		w.WriteHeader(204)
	}
}
func (s *Server) forumSocialProfile(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "userID")
	if !ok {
		return
	}
	var u model.User
	if forumFailure(w, r, s.DB.First(&u, "id = ? AND status = 'active'", id).Error) {
		return
	}
	var followers, following, mine int64
	s.DB.Model(&model.ForumFollow{}).Where("author_id = ?", id).Count(&followers)
	s.DB.Model(&model.ForumFollow{}).Where("user_id = ?", id).Count(&following)
	s.DB.Model(&model.ForumFollow{}).Where("user_id = ? AND author_id = ?", viewerID(r), id).Count(&mine)
	writeJSON(w, 200, map[string]any{"followers": followers, "following": following, "followed": mine > 0})
}
func (s *Server) forumSetReaction(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "targetID")
	if !ok {
		return
	}
	kind := chi.URLParam(r, "targetType")
	uid := userFrom(r).ID
	if !s.forumAllow(w, r, "reaction", 120, time.Minute) {
		return
	}
	var count int
	liked := r.Method == "PUT"
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		if _, err := forumTarget(tx, kind, id, uid, true); err != nil {
			return err
		}
		table, _ := forumTargetTable(kind)
		reaction := model.ForumReaction{ID: uuid.New(), UserID: uid, TargetType: kind, TargetID: id}
		var res *gorm.DB
		delta := -1
		if liked {
			res = tx.Clauses(clause.OnConflict{DoNothing: true}).Create(&reaction)
			delta = 1
		} else {
			res = tx.Where("user_id = ? AND target_type = ? AND target_id = ?", uid, kind, id).Delete(&model.ForumReaction{})
		}
		if res.Error != nil {
			return res.Error
		}
		if res.RowsAffected > 0 {
			if err := tx.Table(table).Where("id = ?", id).UpdateColumn("like_count", gorm.Expr("GREATEST(like_count + ?,0)", delta)).Error; err != nil {
				return err
			}
		}
		return tx.Table(table).Select("like_count").Where("id = ?", id).Scan(&count).Error
	})
	if !forumFailure(w, r, err) {
		writeJSON(w, 200, map[string]any{"liked": liked, "like_count": count})
	}
}
func (s *Server) forumBookmark(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "targetID")
	if !ok {
		return
	}
	kind := chi.URLParam(r, "targetType")
	uid := userFrom(r).ID
	if kind != "article" && kind != "topic" {
		forumFailure(w, r, errForumInput)
		return
	}
	if r.Method == "DELETE" {
		if !forumFailure(w, r, s.DB.Where("user_id = ? AND target_type = ? AND target_id = ?", uid, kind, id).Delete(&model.ForumBookmark{}).Error) {
			w.WriteHeader(204)
		}
		return
	}
	if _, err := forumTarget(s.DB, kind, id, uid, false); forumFailure(w, r, err) {
		return
	}
	if !forumFailure(w, r, s.DB.Clauses(clause.OnConflict{DoNothing: true}).Create(&model.ForumBookmark{UserID: uid, TargetType: kind, TargetID: id}).Error) {
		w.WriteHeader(204)
	}
}
func (s *Server) forumBookmarks(w http.ResponseWriter, r *http.Request) {
	var rows []model.ForumBookmark
	uid := userFrom(r).ID
	if forumFailure(w, r, s.DB.Where("user_id = ?", uid).Order("created_at DESC").Limit(pageLimit(r)).Offset(pageOffset(r)).Find(&rows).Error) {
		return
	}
	articles := []model.ForumArticle{}
	topics := []model.ForumTopic{}
	for _, row := range rows {
		if _, err := forumTarget(s.DB, row.TargetType, row.TargetID, uid, false); err != nil {
			continue
		}
		if row.TargetType == "article" {
			var a model.ForumArticle
			if s.DB.First(&a, "id = ?", row.TargetID).Error == nil {
				articles = append(articles, a)
			}
		} else {
			var t model.ForumTopic
			if s.DB.First(&t, "id = ?", row.TargetID).Error == nil {
				topics = append(topics, t)
			}
		}
	}
	writeJSON(w, 200, map[string]any{"articles": s.forumArticleViews(r, articles, true), "topics": s.forumTopicViews(r, topics, false), "has_more": len(rows) == pageLimit(r)})
}
func (s *Server) forumNotifications(w http.ResponseWriter, r *http.Request) {
	uid := userFrom(r).ID
	if r.Method == "PUT" {
		now := time.Now().UTC()
		if !forumFailure(w, r, s.DB.Model(&model.ForumNotification{}).Where("user_id = ? AND read_at IS NULL", uid).Update("read_at", now).Error) {
			w.WriteHeader(204)
		}
		return
	}
	var rows []model.ForumNotification
	var unread int64
	if forumFailure(w, r, s.DB.Where("user_id = ?", uid).Order("created_at DESC,id DESC").Limit(pageLimit(r)).Offset(pageOffset(r)).Find(&rows).Error) {
		return
	}
	s.DB.Model(&model.ForumNotification{}).Where("user_id = ? AND read_at IS NULL", uid).Count(&unread)
	ids := make([]uuid.UUID, 0, len(rows))
	for _, n := range rows {
		ids = append(ids, n.ActorID)
	}
	authors := s.forumAuthors(ids)
	type item struct {
		model.ForumNotification
		Actor forumAuthor `json:"actor"`
	}
	result := make([]item, 0, len(rows))
	for _, n := range rows {
		result = append(result, item{n, authors[n.ActorID]})
	}
	writeJSON(w, 200, map[string]any{"notifications": result, "unread": unread})
}
func (s *Server) forumSearch(w http.ResponseWriter, r *http.Request) {
	text := strings.TrimSpace(r.URL.Query().Get("q"))
	if len([]rune(text)) < 2 || len(text) > 400 {
		writeJSON(w, 200, map[string]any{"articles": []any{}, "topics": []any{}})
		return
	}
	pattern := "%" + strings.NewReplacer("\\", "\\\\", "%", "\\%", "_", "\\_").Replace(text) + "%"
	var articles []model.ForumArticle
	var topics []model.ForumTopic
	if forumFailure(w, r, s.DB.Where("status = 'published' AND (title ILIKE ? OR body ILIKE ?)", pattern, pattern).Order("published_at DESC").Limit(pageLimit(r)).Offset(pageOffset(r)).Find(&articles).Error) {
		return
	}
	if forumFailure(w, r, s.DB.Where("status = 'published' AND (title ILIKE ? OR body ILIKE ?)", pattern, pattern).Order("last_post_at DESC").Limit(pageLimit(r)).Offset(pageOffset(r)).Find(&topics).Error) {
		return
	}
	writeJSON(w, 200, map[string]any{"articles": s.forumArticleViews(r, articles, true), "topics": s.forumTopicViews(r, topics, false)})
}
func (s *Server) forumReport(w http.ResponseWriter, r *http.Request) {
	if !s.forumAllow(w, r, "report", 20, time.Hour) {
		return
	}
	var in struct {
		TargetType string    `json:"target_type"`
		TargetID   uuid.UUID `json:"target_id"`
		Reason     string    `json:"reason"`
	}
	if !decodeJSON(w, r, &in) {
		return
	}
	if len(strings.TrimSpace(in.Reason)) < 3 || len(in.Reason) > 2000 {
		forumFailure(w, r, errForumInput)
		return
	}
	if _, err := forumTarget(s.DB, in.TargetType, in.TargetID, viewerID(r), false); forumFailure(w, r, err) {
		return
	}
	report := model.ForumReport{ID: uuid.New(), UserID: userFrom(r).ID, TargetType: in.TargetType, TargetID: in.TargetID, Reason: in.Reason, Status: "open"}
	if !forumFailure(w, r, s.DB.Create(&report).Error) {
		writeJSON(w, 201, map[string]any{"report": report})
	}
}
func (s *Server) forumAcceptAnswer(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "topicID")
	if !ok {
		return
	}
	var in struct {
		PostID *uuid.UUID `json:"post_id"`
	}
	if !decodeJSON(w, r, &in) {
		return
	}
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		var t model.ForumTopic
		if e := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&t, "id = ? AND status = 'published'", id).Error; e != nil {
			return e
		}
		if t.AuthorID != userFrom(r).ID {
			return errForumInput
		}
		var sec model.ForumSection
		tx.First(&sec, "id = ?", t.SectionID)
		if sec.Slug != "help" {
			return errForumInput
		}
		if in.PostID != nil {
			var p model.ForumPost
			if e := tx.First(&p, "id = ? AND topic_id = ? AND status = 'published'", *in.PostID, id).Error; e != nil {
				return e
			}
		}
		return tx.Model(&t).Update("accepted_post_id", in.PostID).Error
	})
	if !forumFailure(w, r, err) {
		w.WriteHeader(204)
	}
}
func validForumLink(raw string) bool {
	u, e := url.Parse(raw)
	return e == nil && u.Host != "" && (u.Scheme == "https" || u.Scheme == "http") && u.User == nil
}

func (s *Server) forumUpdateComment(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "commentID")
	if !ok {
		return
	}
	var in forumCommentInput
	if !decodeJSON(w, r, &in) {
		return
	}
	in.Body = strings.TrimSpace(in.Body)
	if in.Body == "" || len([]rune(in.Body)) > 50000 {
		forumFailure(w, r, errForumInput)
		return
	}
	var c model.ForumComment
	if forumFailure(w, r, s.DB.First(&c, "id = ? AND author_id = ? AND status = 'published'", id, userFrom(r).ID).Error) {
		return
	}
	c.Body = in.Body
	if !forumFailure(w, r, s.saveForumContent(r, &c, "comment", id, in.AttachmentIDs, true)) {
		writeJSON(w, 200, map[string]any{"comment": c})
	}
}
