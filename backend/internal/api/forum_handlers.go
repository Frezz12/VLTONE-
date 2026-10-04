package api

import (
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/google/uuid"
	"gorm.io/gorm"

	"vltstudio/backend/internal/auth"
	"vltstudio/backend/internal/model"
)

// forumAuthor is the only user projection the forum exposes. Emails and other
// account details never leave the API through forum responses.
type forumAuthor struct {
	ID       uuid.UUID `json:"id"`
	Nickname string    `json:"nickname"`
}

type forumSectionResponse struct {
	model.ForumSection
	TopicCount int64      `json:"topic_count"`
	LastPostAt *time.Time `json:"last_post_at,omitempty"`
}

type forumTopicResponse struct {
	ID         uuid.UUID   `json:"id"`
	SectionID  uuid.UUID   `json:"section_id"`
	Title      string      `json:"title"`
	Excerpt    string      `json:"excerpt"`
	Pinned     bool        `json:"pinned"`
	Locked     bool        `json:"locked"`
	Status     string      `json:"status"`
	ReplyCount int         `json:"reply_count"`
	LikeCount  int         `json:"like_count"`
	ViewCount  int64       `json:"view_count"`
	LastPostAt time.Time   `json:"last_post_at"`
	CreatedAt  time.Time   `json:"created_at"`
	Author     forumAuthor `json:"author"`
	Liked      bool        `json:"liked"`
	Body       string      `json:"body,omitempty"`
	Section    *struct {
		ID      uuid.UUID `json:"id"`
		Slug    string    `json:"slug"`
		TitleRU string    `json:"title_ru"`
		TitleEN string    `json:"title_en"`
	} `json:"section,omitempty"`
}

type forumPostResponse struct {
	ID        uuid.UUID   `json:"id"`
	TopicID   uuid.UUID   `json:"topic_id"`
	Body      string      `json:"body"`
	Status    string      `json:"status"`
	LikeCount int         `json:"like_count"`
	CreatedAt time.Time   `json:"created_at"`
	UpdatedAt time.Time   `json:"updated_at"`
	Author    forumAuthor `json:"author"`
	Liked     bool        `json:"liked"`
	CanEdit   bool        `json:"can_edit"`
}

type forumArticleResponse struct {
	ID           uuid.UUID   `json:"id"`
	Title        string      `json:"title"`
	Excerpt      string      `json:"excerpt"`
	Status       string      `json:"status"`
	LikeCount    int         `json:"like_count"`
	CommentCount int         `json:"comment_count"`
	PublishedAt  *time.Time  `json:"published_at"`
	CreatedAt    time.Time   `json:"created_at"`
	Author       forumAuthor `json:"author"`
	Liked        bool        `json:"liked"`
	Body         string      `json:"body,omitempty"`
}

type forumCommentResponse struct {
	ID        uuid.UUID   `json:"id"`
	ArticleID uuid.UUID   `json:"article_id"`
	Body      string      `json:"body"`
	LikeCount int         `json:"like_count"`
	CreatedAt time.Time   `json:"created_at"`
	Author    forumAuthor `json:"author"`
	Liked     bool        `json:"liked"`
	CanEdit   bool        `json:"can_edit"`
}

type forumProfileResponse struct {
	User struct {
		ID        uuid.UUID `json:"id"`
		Nickname  string    `json:"nickname"`
		CreatedAt time.Time `json:"created_at"`
	} `json:"user"`
	Profile model.ForumProfile `json:"profile"`
	Stats   struct {
		Topics   int64 `json:"topics"`
		Posts    int64 `json:"posts"`
		Articles int64 `json:"articles"`
	} `json:"stats"`
	RecentTopics   []forumTopicResponse   `json:"recent_topics"`
	RecentArticles []forumArticleResponse `json:"recent_articles"`
}

// viewerFrom reads the optional session placed by webAuthOptional.
func viewerFrom(r *http.Request) (model.User, bool) {
	value := r.Context().Value(ctxUser)
	if value == nil {
		return model.User{}, false
	}
	user, ok := value.(model.User)
	return user, ok
}

func pageOffset(r *http.Request) int {
	offset, err := strconv.Atoi(r.URL.Query().Get("offset"))
	if err != nil || offset < 0 || offset > 1_000_000 {
		return 0
	}
	return offset
}

func forumExcerpt(body string, limit int) string {
	runes := []rune(strings.TrimSpace(body))
	if len(runes) <= limit {
		return string(runes)
	}
	return string(runes[:limit]) + "…"
}

func (s *Server) forumAuthors(ids []uuid.UUID) map[uuid.UUID]forumAuthor {
	authors := make(map[uuid.UUID]forumAuthor, len(ids))
	unique := make([]uuid.UUID, 0, len(ids))
	seen := make(map[uuid.UUID]bool, len(ids))
	for _, id := range ids {
		if id != uuid.Nil && !seen[id] {
			seen[id] = true
			unique = append(unique, id)
		}
	}
	if len(unique) == 0 {
		return authors
	}
	var users []model.User
	if err := s.DB.Select("id", "nickname").Where("id IN ?", unique).Find(&users).Error; err != nil {
		return authors
	}
	for _, user := range users {
		authors[user.ID] = forumAuthor{ID: user.ID, Nickname: user.Nickname}
	}
	return authors
}

func (s *Server) forumLikedSet(r *http.Request, targetType string, ids []uuid.UUID) map[uuid.UUID]bool {
	liked := make(map[uuid.UUID]bool, len(ids))
	user, ok := viewerFrom(r)
	if !ok || len(ids) == 0 {
		return liked
	}
	var rows []model.ForumReaction
	if err := s.DB.Where("user_id = ? AND target_type = ? AND target_id IN ?",
		user.ID, targetType, ids).Find(&rows).Error; err != nil {
		return liked
	}
	for _, row := range rows {
		liked[row.TargetID] = true
	}
	return liked
}

func forumVisibleFilter(db *gorm.DB, viewer *model.User) *gorm.DB {
	if viewer != nil {
		return db.Where("status = ? OR author_id = ?", model.ForumStatusPublished, viewer.ID)
	}
	return db.Where("status = ?", model.ForumStatusPublished)
}

func (s *Server) viewerOrNil(r *http.Request) *model.User {
	if user, ok := viewerFrom(r); ok {
		return &user
	}
	return nil
}

// GET /v1/forum/sections
func (s *Server) forumSections(w http.ResponseWriter, r *http.Request) {
	var sections []model.ForumSection
	if err := s.DB.Order("sort_order, created_at").Find(&sections).Error; err != nil {
		writeError(w, r, http.StatusInternalServerError, "sections_unavailable", "Sections are unavailable.", nil)
		return
	}
	type sectionStats struct {
		SectionID uuid.UUID
		Total     int64
		LastPost  *time.Time
	}
	var stats []sectionStats
	s.DB.Model(&model.ForumTopic{}).
		Where("status = ?", model.ForumStatusPublished).
		Select("section_id, count(*) AS total, max(last_post_at) AS last_post").
		Group("section_id").Scan(&stats)
	bySection := make(map[uuid.UUID]sectionStats, len(stats))
	for _, row := range stats {
		bySection[row.SectionID] = row
	}
	response := make([]forumSectionResponse, 0, len(sections))
	for _, section := range sections {
		item := forumSectionResponse{ForumSection: section}
		if stat, ok := bySection[section.ID]; ok {
			item.TopicCount = stat.Total
			item.LastPostAt = stat.LastPost
		}
		response = append(response, item)
	}
	writeJSON(w, http.StatusOK, map[string]any{"sections": response})
}

// GET /v1/forum/sections/{slug}/topics
func (s *Server) forumSectionTopics(w http.ResponseWriter, r *http.Request) {
	slug := strings.TrimSpace(chi.URLParam(r, "slug"))
	var section model.ForumSection
	if err := s.DB.Where("slug = ?", slug).First(&section).Error; err != nil {
		writeError(w, r, http.StatusNotFound, "section_not_found", "Section was not found.", nil)
		return
	}
	viewer := s.viewerOrNil(r)
	limit, offset := pageLimit(r), pageOffset(r)
	query := forumVisibleFilter(s.DB.Where("section_id = ?", section.ID), viewer)
	var total int64
	query.Model(&model.ForumTopic{}).Count(&total)
	var topics []model.ForumTopic
	if err := query.Order("pinned DESC, last_post_at DESC").
		Limit(limit).Offset(offset).Find(&topics).Error; err != nil {
		writeError(w, r, http.StatusInternalServerError, "topics_unavailable", "Topics are unavailable.", nil)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"section": section, "topics": s.forumTopicViews(r, topics, false),
		"total": total, "limit": limit, "offset": offset,
	})
}

// GET /v1/forum/topics/{topicID}
func (s *Server) forumTopic(w http.ResponseWriter, r *http.Request) {
	topicID, ok := parseUUIDParam(w, r, "topicID")
	if !ok {
		return
	}
	viewer := s.viewerOrNil(r)
	var topic model.ForumTopic
	if err := forumVisibleFilter(s.DB.Where("id = ?", topicID), viewer).First(&topic).Error; err != nil {
		writeError(w, r, http.StatusNotFound, "topic_not_found", "Topic was not found.", nil)
		return
	}
	// Count a view per successful read. The response still reports the old
	// value on the first read, which is harmless and saves a round trip.
	s.DB.Model(&model.ForumTopic{}).Where("id = ?", topic.ID).
		UpdateColumn("view_count", gorm.Expr("view_count + 1"))
	topic.ViewCount++
	var section model.ForumSection
	s.DB.First(&section, "id = ?", topic.SectionID)
	views := s.forumTopicViews(r, []model.ForumTopic{topic}, true)
	view := views[0]
	view.Section = &struct {
		ID      uuid.UUID `json:"id"`
		Slug    string    `json:"slug"`
		TitleRU string    `json:"title_ru"`
		TitleEN string    `json:"title_en"`
	}{section.ID, section.Slug, section.TitleRU, section.TitleEN}
	writeJSON(w, http.StatusOK, map[string]any{"topic": view})
}

// GET /v1/forum/topics/{topicID}/posts
func (s *Server) forumTopicPosts(w http.ResponseWriter, r *http.Request) {
	topicID, ok := parseUUIDParam(w, r, "topicID")
	if !ok {
		return
	}
	viewer := s.viewerOrNil(r)
	var topic model.ForumTopic
	if err := forumVisibleFilter(s.DB.Where("id = ?", topicID), viewer).First(&topic).Error; err != nil {
		writeError(w, r, http.StatusNotFound, "topic_not_found", "Topic was not found.", nil)
		return
	}
	limit, offset := pageLimit(r), pageOffset(r)
	query := forumVisibleFilter(s.DB.Where("topic_id = ?", topic.ID), viewer)
	var total int64
	query.Model(&model.ForumPost{}).Count(&total)
	var posts []model.ForumPost
	if err := query.Order("created_at").Limit(limit).Offset(offset).Find(&posts).Error; err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]any{"code": "posts_unavailable"})
		return
	}
	authors := s.forumAuthors(postAuthorIDs(posts))
	liked := s.forumLikedSet(r, model.ForumTargetPost, postIDs(posts))
	response := make([]forumPostResponse, 0, len(posts))
	for _, post := range posts {
		view := forumPostView(post, authors[post.AuthorID], liked[post.ID])
		if viewer != nil && viewer.ID == post.AuthorID {
			view.CanEdit = true
		}
		response = append(response, view)
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"posts": response, "total": total, "limit": limit, "offset": offset,
		"topic": s.forumTopicViews(r, []model.ForumTopic{topic}, false)[0],
	})
}

// GET /v1/forum/articles
func (s *Server) forumArticles(w http.ResponseWriter, r *http.Request) {
	viewer := s.viewerOrNil(r)
	limit, offset := pageLimit(r), pageOffset(r)
	query := forumVisibleFilter(s.DB, viewer)
	if author := strings.TrimSpace(r.URL.Query().Get("author")); author != "" {
		if id, err := uuid.Parse(author); err == nil {
			query = query.Where("author_id = ?", id)
		}
	}
	var total int64
	query.Model(&model.ForumArticle{}).Count(&total)
	var articles []model.ForumArticle
	if err := query.Order("COALESCE(published_at, created_at) DESC").
		Limit(limit).Offset(offset).Find(&articles).Error; err != nil {
		writeError(w, r, http.StatusInternalServerError, "articles_unavailable", "Articles are unavailable.", nil)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"articles": s.forumArticleViews(r, articles, false),
		"total": total, "limit": limit, "offset": offset,
	})
}

// GET /v1/forum/articles/{articleID}
func (s *Server) forumArticle(w http.ResponseWriter, r *http.Request) {
	articleID, ok := parseUUIDParam(w, r, "articleID")
	if !ok {
		return
	}
	viewer := s.viewerOrNil(r)
	var article model.ForumArticle
	if err := forumVisibleFilter(s.DB.Where("id = ?", articleID), viewer).First(&article).Error; err != nil {
		writeError(w, r, http.StatusNotFound, "article_not_found", "Article was not found.", nil)
		return
	}
	views := s.forumArticleViews(r, []model.ForumArticle{article}, true)
	writeJSON(w, http.StatusOK, map[string]any{"article": views[0]})
}

// GET /v1/forum/articles/{articleID}/comments
func (s *Server) forumArticleComments(w http.ResponseWriter, r *http.Request) {
	articleID, ok := parseUUIDParam(w, r, "articleID")
	if !ok {
		return
	}
	viewer := s.viewerOrNil(r)
	var article model.ForumArticle
	if err := forumVisibleFilter(s.DB.Where("id = ?", articleID), viewer).First(&article).Error; err != nil {
		writeError(w, r, http.StatusNotFound, "article_not_found", "Article was not found.", nil)
		return
	}
	limit, offset := pageLimit(r), pageOffset(r)
	query := forumVisibleFilter(s.DB.Where("article_id = ?", article.ID), viewer)
	var total int64
	query.Model(&model.ForumComment{}).Count(&total)
	var comments []model.ForumComment
	if err := query.Order("created_at").Limit(limit).Offset(offset).Find(&comments).Error; err != nil {
		writeError(w, r, http.StatusInternalServerError, "comments_unavailable", "Comments are unavailable.", nil)
		return
	}
	authors := s.forumAuthors(commentAuthorIDs(comments))
	liked := s.forumLikedSet(r, model.ForumTargetComment, commentIDs(comments))
	response := make([]forumCommentResponse, 0, len(comments))
	for _, comment := range comments {
		view := forumCommentView(comment, authors[comment.AuthorID], liked[comment.ID])
		if viewer != nil && viewer.ID == comment.AuthorID {
			view.CanEdit = true
		}
		response = append(response, view)
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"comments": response, "total": total, "limit": limit, "offset": offset,
	})
}

// GET /v1/forum/users/{nickname}
func (s *Server) forumUserProfile(w http.ResponseWriter, r *http.Request) {
	nickname := strings.TrimSpace(chi.URLParam(r, "nickname"))
	var user model.User
	err := s.DB.Where("nickname_key = ? AND status = ?", auth.NormalizeNickname(nickname), model.UserActive).
		First(&user).Error
	if err != nil {
		writeError(w, r, http.StatusNotFound, "user_not_found", "User was not found.", nil)
		return
	}
	profile, err := s.forumEnsureProfile(user.ID)
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "profile_unavailable", "Profile is unavailable.", nil)
		return
	}
	var response forumProfileResponse
	response.User.ID = user.ID
	response.User.Nickname = user.Nickname
	response.User.CreatedAt = user.CreatedAt
	response.Profile = profile
	s.DB.Model(&model.ForumTopic{}).Where("author_id = ? AND status = ?", user.ID, model.ForumStatusPublished).Count(&response.Stats.Topics)
	s.DB.Model(&model.ForumPost{}).Where("author_id = ? AND status = ?", user.ID, model.ForumStatusPublished).Count(&response.Stats.Posts)
	s.DB.Model(&model.ForumArticle{}).Where("author_id = ? AND status = ?", user.ID, model.ForumStatusPublished).Count(&response.Stats.Articles)
	var recentTopics []model.ForumTopic
	s.DB.Where("author_id = ? AND status = ?", user.ID, model.ForumStatusPublished).
		Order("created_at DESC").Limit(5).Find(&recentTopics)
	var recentArticles []model.ForumArticle
	s.DB.Where("author_id = ? AND status = ?", user.ID, model.ForumStatusPublished).
		Order("COALESCE(published_at, created_at) DESC").Limit(5).Find(&recentArticles)
	response.RecentTopics = s.forumTopicViews(r, recentTopics, false)
	response.RecentArticles = s.forumArticleViews(r, recentArticles, false)
	writeJSON(w, http.StatusOK, map[string]any{"profile": response})
}

// GET /v1/forum/me/profile creates the row on the first visit so every
// signed-in account always has a personal page to land on.
func (s *Server) forumMyProfile(w http.ResponseWriter, r *http.Request) {
	user := userFrom(r)
	profile, err := s.forumEnsureProfile(user.ID)
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "profile_unavailable", "Profile is unavailable.", nil)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"profile": profile})
}

func (s *Server) forumEnsureProfile(userID uuid.UUID) (model.ForumProfile, error) {
	profile := model.ForumProfile{UserID: userID}
	err := s.DB.FirstOrCreate(&profile, model.ForumProfile{UserID: userID}).Error
	return profile, err
}

// --- view builders -------------------------------------------------------

func (s *Server) forumTopicViews(r *http.Request, topics []model.ForumTopic, withBody bool) []forumTopicResponse {
	authors := s.forumAuthors(topicAuthorIDs(topics))
	liked := s.forumLikedSet(r, model.ForumTargetTopic, topicIDs(topics))
	response := make([]forumTopicResponse, 0, len(topics))
	for _, topic := range topics {
		view := forumTopicResponse{
			ID: topic.ID, SectionID: topic.SectionID, Title: topic.Title,
			Excerpt: forumExcerpt(topic.Body, 240), Pinned: topic.Pinned,
			Locked: topic.Locked, Status: topic.Status, ReplyCount: topic.ReplyCount,
			LikeCount: topic.LikeCount, ViewCount: topic.ViewCount,
			LastPostAt: topic.LastPostAt, CreatedAt: topic.CreatedAt,
			Author: authors[topic.AuthorID], Liked: liked[topic.ID],
		}
		if withBody {
			view.Body = topic.Body
		}
		response = append(response, view)
	}
	return response
}

func (s *Server) forumArticleViews(r *http.Request, articles []model.ForumArticle, withBody bool) []forumArticleResponse {
	authors := s.forumAuthors(articleAuthorIDs(articles))
	liked := s.forumLikedSet(r, model.ForumTargetArticle, articleIDs(articles))
	response := make([]forumArticleResponse, 0, len(articles))
	for _, article := range articles {
		view := forumArticleResponse{
			ID: article.ID, Title: article.Title, Excerpt: article.Excerpt,
			Status: article.Status, LikeCount: article.LikeCount,
			CommentCount: article.CommentCount, PublishedAt: article.PublishedAt,
			CreatedAt: article.CreatedAt, Author: authors[article.AuthorID],
			Liked: liked[article.ID],
		}
		if view.Excerpt == "" {
			view.Excerpt = forumExcerpt(article.Body, 240)
		}
		if withBody {
			view.Body = article.Body
		}
		response = append(response, view)
	}
	return response
}

func forumTopicView(topic model.ForumTopic, author forumAuthor, liked bool) forumTopicResponse {
	return forumTopicResponse{
		ID: topic.ID, SectionID: topic.SectionID, Title: topic.Title,
		Excerpt: forumExcerpt(topic.Body, 240), Pinned: topic.Pinned,
		Locked: topic.Locked, Status: topic.Status, ReplyCount: topic.ReplyCount,
		LikeCount: topic.LikeCount, ViewCount: topic.ViewCount,
		LastPostAt: topic.LastPostAt, CreatedAt: topic.CreatedAt,
		Author: author, Liked: liked,
	}
}

func forumPostView(post model.ForumPost, author forumAuthor, liked bool) forumPostResponse {
	return forumPostResponse{
		ID: post.ID, TopicID: post.TopicID, Body: post.Body, Status: post.Status,
		LikeCount: post.LikeCount, CreatedAt: post.CreatedAt, UpdatedAt: post.UpdatedAt,
		Author: author, Liked: liked,
	}
}

func forumArticleView(article model.ForumArticle, author forumAuthor, liked bool) forumArticleResponse {
	view := forumArticleResponse{
		ID: article.ID, Title: article.Title, Excerpt: article.Excerpt,
		Status: article.Status, LikeCount: article.LikeCount,
		CommentCount: article.CommentCount, PublishedAt: article.PublishedAt,
		CreatedAt: article.CreatedAt, Author: author, Liked: liked,
	}
	if view.Excerpt == "" {
		view.Excerpt = forumExcerpt(article.Body, 240)
	}
	return view
}

func forumCommentView(comment model.ForumComment, author forumAuthor, liked bool) forumCommentResponse {
	return forumCommentResponse{
		ID: comment.ID, ArticleID: comment.ArticleID, Body: comment.Body,
		LikeCount: comment.LikeCount, CreatedAt: comment.CreatedAt,
		Author: author, Liked: liked,
	}
}

// --- id helpers ----------------------------------------------------------

func topicIDs(topics []model.ForumTopic) []uuid.UUID {
	ids := make([]uuid.UUID, len(topics))
	for i, topic := range topics {
		ids[i] = topic.ID
	}
	return ids
}

func topicAuthorIDs(topics []model.ForumTopic) []uuid.UUID {
	ids := make([]uuid.UUID, len(topics))
	for i, topic := range topics {
		ids[i] = topic.AuthorID
	}
	return ids
}

func postIDs(posts []model.ForumPost) []uuid.UUID {
	ids := make([]uuid.UUID, len(posts))
	for i, post := range posts {
		ids[i] = post.ID
	}
	return ids
}

func postAuthorIDs(posts []model.ForumPost) []uuid.UUID {
	ids := make([]uuid.UUID, len(posts))
	for i, post := range posts {
		ids[i] = post.AuthorID
	}
	return ids
}

func articleIDs(articles []model.ForumArticle) []uuid.UUID {
	ids := make([]uuid.UUID, len(articles))
	for i, article := range articles {
		ids[i] = article.ID
	}
	return ids
}

func articleAuthorIDs(articles []model.ForumArticle) []uuid.UUID {
	ids := make([]uuid.UUID, len(articles))
	for i, article := range articles {
		ids[i] = article.AuthorID
	}
	return ids
}

func commentIDs(comments []model.ForumComment) []uuid.UUID {
	ids := make([]uuid.UUID, len(comments))
	for i, comment := range comments {
		ids[i] = comment.ID
	}
	return ids
}

func commentAuthorIDs(comments []model.ForumComment) []uuid.UUID {
	ids := make([]uuid.UUID, len(comments))
	for i, comment := range comments {
		ids[i] = comment.AuthorID
	}
	return ids
}

func forumLengthError(fields map[string]string, name, value string, min, max int) {
	count := len([]rune(value))
	if count < min || count > max {
		fields[name] = fmt.Sprintf("Must contain between %d and %d characters.", min, max)
	}
}
