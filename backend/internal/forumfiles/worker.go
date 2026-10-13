package forumfiles

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"github.com/google/uuid"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"
	"io"
	"log"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"
	"vltstudio/backend/internal/config"
	"vltstudio/backend/internal/model"
)

var errRejected = errors.New("Video must be readable and no longer than five minutes")

type Worker struct {
	DB     *gorm.DB
	Files  *Storage
	Config config.Config
}
type probeResult struct {
	Format struct {
		Duration string `json:"duration"`
	} `json:"format"`
	Streams []struct {
		CodecType string `json:"codec_type"`
		Width     int    `json:"width"`
		Height    int    `json:"height"`
	} `json:"streams"`
}

func validateProbe(data []byte) error {
	var p probeResult
	if json.Unmarshal(data, &p) != nil {
		return errRejected
	}
	duration, e := strconv.ParseFloat(p.Format.Duration, 64)
	if e != nil || math.IsNaN(duration) || math.IsInf(duration, 0) || duration <= 0 || duration > 300 {
		return errRejected
	}
	for _, s := range p.Streams {
		if s.CodecType == "video" && s.Width > 0 && s.Height > 0 && s.Width <= 8192 && s.Height <= 8192 {
			return nil
		}
	}
	return errRejected
}
func (w *Worker) Run(ctx context.Context) {
	timer := time.NewTicker(3 * time.Second)
	defer timer.Stop()
	nextCleanup := time.Time{}
	for {
		if ctx.Err() != nil {
			return
		}
		if time.Now().After(nextCleanup) {
			w.Cleanup(ctx)
			nextCleanup = time.Now().Add(time.Minute)
		}
		worked, e := w.Once(ctx)
		if e != nil {
			log.Printf("forum media: %v", e)
		}
		if worked {
			continue
		}
		select {
		case <-ctx.Done():
			return
		case <-timer.C:
		}
	}
}
func (w *Worker) Once(ctx context.Context) (bool, error) {
	now := time.Now().UTC()
	lease := uuid.New()
	until := now.Add(25 * time.Minute)
	var job model.ForumMediaJob
	err := w.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		claim := tx.Clauses(clause.Locking{Strength: "UPDATE", Options: "SKIP LOCKED"}).Where("available_at <= ? AND (lease_until IS NULL OR lease_until < ?)", now, now).Order("available_at").Limit(1).Find(&job)
		if claim.Error != nil {
			return claim.Error
		}
		if claim.RowsAffected == 0 {
			return gorm.ErrRecordNotFound
		}
		prefix := "media/" + job.AttachmentID.String() + "/" + lease.String()
		if e := tx.Exec("INSERT INTO forum_media_attempts(prefix,expires_at) VALUES(?,?)", prefix, until).Error; e != nil {
			return e
		}
		job.Attempts++
		job.LeaseUntil = &until
		job.LeaseID = &lease
		if e := tx.Save(&job).Error; e != nil {
			return e
		}
		return tx.Model(&model.ForumAttachment{}).Where("id = ?", job.AttachmentID).Update("media_status", "processing").Error
	})
	if errors.Is(err, gorm.ErrRecordNotFound) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	var a model.ForumAttachment
	if e := w.DB.First(&a, "id = ?", job.AttachmentID).Error; e != nil {
		return true, e
	}
	child, cancel := context.WithTimeout(ctx, 20*time.Minute)
	defer cancel()
	key := "media/" + a.ID.String() + "/" + lease.String()
	processErr := w.process(child, a, key)
	if processErr != nil {
		log.Printf("forum media attachment=%s attempt=%d error=%v", a.ID, job.Attempts, processErr)
	}
	if ctx.Err() != nil {
		return true, ctx.Err()
	} // lease recovery retains work after shutdown
	status := "ready"
	message := ""
	if processErr != nil {
		status = "pending"
		message = "Video preparation failed. Retry upload if the problem persists."
		if errors.Is(processErr, errRejected) {
			status = "rejected"
			message = errRejected.Error()
		} else if job.Attempts >= 3 {
			status = "failed"
		}
	}
	err = w.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		var current model.ForumMediaJob
		if e := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&current, "attachment_id = ? AND lease_id = ?", a.ID, lease).Error; e != nil {
			return e
		}
		changes := map[string]any{"media_status": status, "media_error": message}
		if processErr == nil {
			changes["media_key"] = key
			if e := tx.Exec("DELETE FROM forum_media_attempts WHERE prefix=?", key).Error; e != nil {
				return e
			}
		}
		if e := tx.Model(&model.ForumAttachment{}).Where("id = ?", a.ID).Updates(changes).Error; e != nil {
			return e
		}
		if status != "pending" {
			return tx.Delete(&current).Error
		}
		return tx.Model(&current).Updates(map[string]any{"lease_until": nil, "lease_id": nil, "last_error": message, "available_at": time.Now().Add(time.Duration(job.Attempts) * time.Minute)}).Error
	})
	if processErr != nil || err != nil {
		for _, file := range []string{"poster.jpg", "video.mp4", "video.webm"} {
			w.Files.Delete(ctx, key+"/"+file)
		}
	}
	return true, err
}
func (w *Worker) process(ctx context.Context, a model.ForumAttachment, key string) error {
	temp, e := os.MkdirTemp(w.Files.Root, ".video-")
	if e != nil {
		return e
	}
	defer os.RemoveAll(temp)
	reader, e := w.Files.Open(ctx, a.ObjectKey)
	if e != nil {
		return e
	}
	input := filepath.Join(temp, "input"+filepath.Ext(a.Name))
	out, e := os.Create(input)
	if e != nil {
		reader.Close()
		return e
	}
	n, e := io.Copy(out, io.LimitReader(reader, a.Bytes+1))
	reader.Close()
	out.Close()
	if e != nil {
		return e
	}
	if n != a.Bytes {
		return fmt.Errorf("incomplete video object")
	}
	probeCtx, stop := context.WithTimeout(ctx, 30*time.Second)
	defer stop()
	data, e := exec.CommandContext(probeCtx, w.Config.ForumFFprobe, "-v", "error", "-protocol_whitelist", "file,pipe", "-format_whitelist", "mov,matroska,webm", "-show_entries", "format=duration:stream=codec_type,width,height", "-of", "json", input).Output()
	if e != nil {
		if probeCtx.Err() != nil {
			return probeCtx.Err()
		}
		return errRejected
	}
	if e = validateProbe(data); e != nil {
		return e
	}
	filter := "scale=w='min(1920,iw)':h='min(1080,ih)':force_original_aspect_ratio=decrease:force_divisible_by=2,setsar=1,fps=30"
	common := []string{"-nostdin", "-v", "error", "-threads", "2", "-protocol_whitelist", "file,pipe", "-format_whitelist", "mov,matroska,webm", "-i", input, "-map_metadata", "-1", "-map", "0:v:0", "-map", "0:a:0?", "-sn", "-dn", "-vf", filter, "-t", "300"}
	type output struct {
		name, mime string
		args       []string
	}
	outputs := []output{
		{"video.mp4", "video/mp4", []string{"-c:v", "libx264", "-threads", "2", "-preset", "fast", "-crf", "23", "-maxrate", "5M", "-bufsize", "10M", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "192k", "-ac", "2", "-movflags", "+faststart"}},
		{"video.webm", "video/webm", []string{"-c:v", "libvpx-vp9", "-threads", "2", "-row-mt", "1", "-deadline", "realtime", "-cpu-used", "5", "-crf", "32", "-b:v", "0", "-c:a", "libopus", "-b:a", "160k", "-ac", "2"}},
	}
	for _, o := range outputs {
		args := append(append([]string{}, common...), o.args...)
		dest := filepath.Join(temp, o.name)
		args = append(args, "-y", dest)
		cmd := exec.CommandContext(ctx, w.Config.ForumFFmpeg, args...)
		if e = cmd.Run(); e != nil {
			return e
		}
		if e = w.Files.Put(ctx, key+"/"+o.name, dest, o.mime); e != nil {
			return e
		}
	}
	poster := filepath.Join(temp, "poster.jpg")
	if e = exec.CommandContext(ctx, w.Config.ForumFFmpeg, "-nostdin", "-v", "error", "-threads", "2", "-i", filepath.Join(temp, "video.mp4"), "-frames:v", "1", "-vf", "scale=640:-2", "-y", poster).Run(); e != nil {
		return e
	}
	return w.Files.Put(ctx, key+"/poster.jpg", poster, "image/jpeg")
}
func (w *Worker) Cleanup(ctx context.Context) {
	var attempts []struct{ Prefix string }
	if w.DB.WithContext(ctx).Table("forum_media_attempts").Where("expires_at < now()").Limit(20).Find(&attempts).Error == nil {
		for _, attempt := range attempts {
			var references int64
			if w.DB.WithContext(ctx).Model(&model.ForumAttachment{}).Where("media_key=?", attempt.Prefix).Count(&references).Error != nil {
				continue
			}
			ok := true
			if references == 0 {
				for _, name := range []string{"poster.jpg", "video.mp4", "video.webm"} {
					if w.Files.Delete(ctx, attempt.Prefix+"/"+name) != nil {
						ok = false
					}
				}
			}
			if ok {
				w.DB.WithContext(ctx).Exec("DELETE FROM forum_media_attempts WHERE prefix=?", attempt.Prefix)
			}
		}
	}
	cleanupLocalWork(w.Files.Root)
	// Expired unattached uploads are temporary, while avatars remain referenced.
	w.DB.WithContext(ctx).Exec(`DELETE FROM forum_attachments WHERE expires_at < now() AND article_id IS NULL AND topic_id IS NULL AND post_id IS NULL AND comment_id IS NULL AND NOT EXISTS(SELECT 1 FROM forum_profiles WHERE avatar_id=forum_attachments.id)`)
	w.DB.WithContext(ctx).Exec("DELETE FROM forum_sso_codes WHERE expires_at < now()")
	var rows []struct {
		ID               uuid.UUID
		ObjectKey        string
		MediaKey         string
		ProviderUploadID string
	}
	if w.DB.WithContext(ctx).Table("forum_object_deletions").Limit(20).Find(&rows).Error != nil {
		return
	}
	for _, row := range rows {
		if ctx.Err() != nil {
			return
		}
		ok := true
		if w.Files.Remote != nil && row.ProviderUploadID != "" {
			w.Files.Remote.AbortMultipart(ctx, "staging/"+row.ID.String(), row.ProviderUploadID)
		}
		keys := []string{row.ObjectKey}
		if w.Files.Remote != nil {
			keys = append(keys, "staging/"+row.ID.String())
		} else {
			p, _ := w.Files.Path("staging/" + row.ID.String())
			os.RemoveAll(p)
		}
		if row.MediaKey != "" {
			for _, name := range []string{"poster.jpg", "video.mp4", "video.webm"} {
				keys = append(keys, row.MediaKey+"/"+name)
			}
		}
		for _, key := range keys {
			if w.Files.Delete(ctx, key) != nil {
				ok = false
			}
		}
		if ok {
			w.DB.WithContext(ctx).Table("forum_object_deletions").Where("id = ?", row.ID).Delete(map[string]any{})
		}
	}
}

// API transfers stop after 30 minutes; conversion stops after 20. A one-hour
// grace period also protects work owned by other processes on this host.
func cleanupLocalWork(root string) {
	for _, folder := range []string{root, filepath.Join(root, "verification")} {
		entries, e := os.ReadDir(folder)
		if e != nil {
			continue
		}
		for _, entry := range entries {
			name := entry.Name()
			owned := (folder == root && ((entry.IsDir() && strings.HasPrefix(name, ".video-")) || strings.HasPrefix(name, ".assemble-"))) || (folder != root && strings.HasPrefix(name, ".collab-object-"))
			if owned {
				if info, e := entry.Info(); e == nil && time.Since(info.ModTime()) > time.Hour {
					os.RemoveAll(filepath.Join(folder, name))
				}
			}
		}
	}
}
