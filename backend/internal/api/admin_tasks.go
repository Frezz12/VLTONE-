package api

import (
	"bytes"
	"errors"
	"io"
	"mime"
	"net/http"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/google/uuid"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"
	"vltstudio/backend/internal/model"
)

var taskStatuses = []string{"idea", "planned", "in_progress", "review", "done", "archived"}
var taskPriorities = []string{"low", "normal", "high", "urgent"}
var errTaskConflict = errors.New("task conflict")
var errTaskFileLimit = errors.New("task file limit")
var errTaskAssignee = errors.New("task assignee unavailable")

func taskError(w http.ResponseWriter, r *http.Request, err error) {
	status, code, message := 500, "task_failed", "Не удалось сохранить или загрузить задачу. Повторите попытку."
	if errors.Is(err, gorm.ErrRecordNotFound) {
		status, code, message = 404, "task_not_found", "Задача или файл не найдены."
	}
	if errors.Is(err, errTaskConflict) {
		status, code, message = 409, "task_conflict", "Задача уже изменена. Обновите её перед сохранением."
	}
	if errors.Is(err, errTaskFileLimit) {
		status, code, message = 422, "task_file_limit", "К задаче можно прикрепить не больше 20 файлов."
	}
	if errors.Is(err, errTaskAssignee) {
		status, code, message = 422, "assignee_unavailable", "Выберите участника с доступом к доске задач."
	}
	writeError(w, r, status, code, message, nil)
}

func (s *Server) adminTaskMembers(w http.ResponseWriter, r *http.Request) {
	// Only names and assignment eligibility: board access does not expose account data.
	var members []struct {
		ID       uuid.UUID `json:"id"`
		Nickname string    `json:"nickname"`
		Active   bool      `json:"active"`
	}
	err := s.DB.Raw(`SELECT a.id, COALESCE(u.nickname,a.nickname) AS nickname,
 (a.status = 'active' AND (a.is_owner OR a.permissions ? 'tasks.read') AND
 ((a.user_id IS NULL AND a.is_owner) OR u.status = 'active')) AS active
 FROM admin_users a LEFT JOIN users u ON u.id = a.user_id ORDER BY nickname`).Scan(&members).Error
	if err != nil {
		taskError(w, r, err)
		return
	}
	writeJSON(w, 200, map[string]any{"members": members})
}

func (s *Server) adminTasks(w http.ResponseWriter, r *http.Request) {
	query := s.DB.Model(&model.AdminTask{})
	if q := strings.TrimSpace(r.URL.Query().Get("q")); q != "" {
		query = query.Where("title ILIKE ? OR description ILIKE ?", "%"+q+"%", "%"+q+"%")
	}
	if status := r.URL.Query().Get("status"); status != "" {
		query = query.Where("status = ?", status)
	} else {
		query = query.Where("status <> 'archived'")
	}
	if assignee := r.URL.Query().Get("assignee"); assignee != "" {
		id, err := uuid.Parse(assignee)
		if err != nil {
			writeError(w, r, 422, "invalid_assignee", "Неверный исполнитель.", nil)
			return
		}
		query = query.Where("assignee_id = ?", id)
	}
	if before := r.URL.Query().Get("before"); before != "" {
		n, err := strconv.ParseInt(before, 10, 64)
		if err != nil || n < 1 {
			writeError(w, r, 422, "invalid_cursor", "Неверный курсор.", nil)
			return
		}
		query = query.Where("number < ?", n)
	}
	tasks := []model.AdminTask{}
	if err := query.Order("number DESC").Limit(101).Find(&tasks).Error; err != nil {
		taskError(w, r, err)
		return
	}
	var next int64
	if len(tasks) > 100 {
		tasks = tasks[:100]
		next = tasks[99].Number
	}
	writeJSON(w, 200, map[string]any{"tasks": tasks, "next_cursor": next})
}

func (s *Server) adminTaskDetail(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "taskID")
	if !ok {
		return
	}
	var task model.AdminTask
	if err := s.DB.First(&task, "id = ?", id).Error; err != nil {
		taskError(w, r, err)
		return
	}
	comments := []model.AdminTaskComment{}
	attachments := []model.AdminTaskAttachment{}
	if err := s.DB.Where("task_id = ?", id).Order("created_at ASC").Find(&comments).Error; err != nil {
		taskError(w, r, err)
		return
	}
	if err := s.DB.Omit("data").Where("task_id = ?", id).Order("created_at ASC").Find(&attachments).Error; err != nil {
		taskError(w, r, err)
		return
	}
	writeJSON(w, 200, map[string]any{"task": task, "comments": comments, "attachments": attachments})
}

type adminTaskInput struct {
	Title       string     `json:"title"`
	Description string     `json:"description"`
	Status      string     `json:"status"`
	Priority    string     `json:"priority"`
	AssigneeID  *uuid.UUID `json:"assignee_id"`
	DueDate     string     `json:"due_date"`
	Version     int        `json:"version"`
}

func (input *adminTaskInput) valid() bool {
	input.Title = strings.TrimSpace(input.Title)
	return input.Title != "" && utf8.RuneCountInString(input.Title) <= 200 && utf8.RuneCountInString(input.Description) <= 20000 && slices.Contains(taskStatuses, input.Status) && slices.Contains(taskPriorities, input.Priority)
}
func (s *Server) adminCreateTask(w http.ResponseWriter, r *http.Request) { s.adminSaveTask(w, r, true) }
func (s *Server) adminUpdateTask(w http.ResponseWriter, r *http.Request) {
	s.adminSaveTask(w, r, false)
}
func (s *Server) adminSaveTask(w http.ResponseWriter, r *http.Request, create bool) {
	var input adminTaskInput
	if !decodeJSON(w, r, &input) {
		return
	}
	if !input.valid() {
		writeError(w, r, 422, "invalid_task", "Проверьте название, описание, этап и приоритет.", nil)
		return
	}
	var due *time.Time
	if input.DueDate != "" {
		parsed, err := time.Parse("2006-01-02", input.DueDate)
		if err != nil {
			writeError(w, r, 422, "invalid_date", "Проверьте срок задачи.", nil)
			return
		}
		due = &parsed
	}
	id := uuid.New()
	if !create {
		var ok bool
		id, ok = parseUUIDParam(w, r, "taskID")
		if !ok {
			return
		}
	}
	task := model.AdminTask{ID: id, AuthorID: adminFrom(r).ID, Version: 1}
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		if !create {
			if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&task, "id = ?", id).Error; err != nil {
				return err
			}
			if task.Version != input.Version {
				return errTaskConflict
			}
		}
		if input.AssigneeID != nil && (create || task.AssigneeID == nil || *task.AssigneeID != *input.AssigneeID) {
			var member model.AdminUser
			if err := tx.First(&member, "id = ? AND status = ?", input.AssigneeID, model.UserActive).Error; err != nil {
				if errors.Is(err, gorm.ErrRecordNotFound) {
					return errTaskAssignee
				}
				return err
			}
			if !adminCan(member, "tasks.read") || !s.resolveAdminIdentity(&member) {
				return errTaskAssignee
			}
		}
		task.Title, task.Description, task.Status, task.Priority, task.AssigneeID, task.DueDate = input.Title, input.Description, input.Status, input.Priority, input.AssigneeID, due
		action := "task.create"
		if create {
			if err := tx.Create(&task).Error; err != nil {
				return err
			}
		} else {
			action = "task.update"
			task.Version++
			if err := tx.Save(&task).Error; err != nil {
				return err
			}
		}
		return s.audit(tx, r, action, "task", id, map[string]any{"status": task.Status, "version": task.Version})
	})
	if err != nil {
		taskError(w, r, err)
		return
	}
	status := 200
	if create {
		status = 201
	}
	writeJSON(w, status, task)
}
func (s *Server) adminTaskComment(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "taskID")
	if !ok {
		return
	}
	var input struct {
		Body string `json:"body"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	input.Body = strings.TrimSpace(input.Body)
	if input.Body == "" || utf8.RuneCountInString(input.Body) > 10000 {
		writeError(w, r, 422, "invalid_comment", "Комментарий должен содержать от 1 до 10 000 символов.", nil)
		return
	}
	comment := model.AdminTaskComment{ID: uuid.New(), TaskID: id, AuthorID: adminFrom(r).ID, Body: input.Body}
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		var task model.AdminTask
		if err := tx.First(&task, "id = ?", id).Error; err != nil {
			return err
		}
		if err := tx.Create(&comment).Error; err != nil {
			return err
		}
		return s.audit(tx, r, "task.comment", "task", id, map[string]any{"comment_id": comment.ID})
	})
	if err != nil {
		taskError(w, r, err)
		return
	}
	writeJSON(w, 201, comment)
}
func (s *Server) adminTaskUpload(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "taskID")
	if !ok {
		return
	}
	const maxFile = 10 << 20
	r.Body = http.MaxBytesReader(w, r.Body, maxFile+(1<<20))
	if err := r.ParseMultipartForm(1 << 20); err != nil {
		if r.MultipartForm != nil {
			r.MultipartForm.RemoveAll()
		}
		writeError(w, r, 413, "file_too_large", "Максимальный размер файла — 10 МБ.", nil)
		return
	}
	defer r.MultipartForm.RemoveAll()
	file, header, err := r.FormFile("file")
	if err != nil {
		writeError(w, r, 422, "file_required", "Выберите файл.", nil)
		return
	}
	defer file.Close()
	data, err := io.ReadAll(io.LimitReader(file, maxFile+1))
	if err != nil || len(data) == 0 || len(data) > maxFile {
		writeError(w, r, 422, "invalid_file", "Нужен непустой файл размером до 10 МБ.", nil)
		return
	}
	name := filepath.Base(strings.ReplaceAll(header.Filename, "\\", "/"))
	if utf8.RuneCountInString(name) > 240 || strings.ContainsAny(name, "\r\n\x00") {
		writeError(w, r, 422, "invalid_filename", "Слишком длинное или недопустимое имя файла.", nil)
		return
	}
	attachment := model.AdminTaskAttachment{ID: uuid.New(), TaskID: id, AuthorID: adminFrom(r).ID, Name: name, Size: int64(len(data)), Data: data}
	err = s.DB.Transaction(func(tx *gorm.DB) error {
		var task model.AdminTask
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&task, "id = ?", id).Error; err != nil {
			return err
		}
		var count int64
		if err := tx.Model(&model.AdminTaskAttachment{}).Where("task_id = ?", id).Count(&count).Error; err != nil {
			return err
		}
		if count >= 20 {
			return errTaskFileLimit
		}
		if err := tx.Create(&attachment).Error; err != nil {
			return err
		}
		return s.audit(tx, r, "task.attachment.add", "task", id, map[string]any{"attachment_id": attachment.ID})
	})
	if err != nil {
		taskError(w, r, err)
		return
	}
	writeJSON(w, 201, attachment)
}
func (s *Server) adminTaskDownload(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "taskID")
	if !ok {
		return
	}
	attachmentID, ok := parseUUIDParam(w, r, "attachmentID")
	if !ok {
		return
	}
	var attachment model.AdminTaskAttachment
	if err := s.DB.First(&attachment, "id = ? AND task_id = ?", attachmentID, id).Error; err != nil {
		taskError(w, r, err)
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Disposition", mime.FormatMediaType("attachment", map[string]string{"filename": attachment.Name}))
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("Cache-Control", "private, no-store")
	http.ServeContent(w, r, attachment.Name, attachment.CreatedAt, bytes.NewReader(attachment.Data))
}
func (s *Server) adminTaskDeleteAttachment(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "taskID")
	if !ok {
		return
	}
	attachmentID, ok := parseUUIDParam(w, r, "attachmentID")
	if !ok {
		return
	}
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		result := tx.Where("id = ? AND task_id = ?", attachmentID, id).Delete(&model.AdminTaskAttachment{})
		if result.Error != nil {
			return result.Error
		}
		if result.RowsAffected == 0 {
			return gorm.ErrRecordNotFound
		}
		return s.audit(tx, r, "task.attachment.delete", "task", id, map[string]any{"attachment_id": attachmentID})
	})
	if err != nil {
		taskError(w, r, err)
		return
	}
	w.WriteHeader(204)
}
