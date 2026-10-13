package forumfiles

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	"vltstudio/backend/internal/config"
	"vltstudio/backend/internal/model"
)

func TestProbeLimits(t *testing.T) {
	for _, duration := range []string{"0", "-1", "300.001", "NaN", "+Inf", "bad"} {
		if validateProbe([]byte(`{"format":{"duration":"`+duration+`"},"streams":[{"codec_type":"video","width":1920,"height":1080}]}`)) == nil {
			t.Fatal("accepted", duration)
		}
	}
	for _, data := range []string{`{}`, `{"format":{"duration":"60"},"streams":[{"codec_type":"audio"}]}`, `{"format":{"duration":"60"},"streams":[{"codec_type":"video","width":99999,"height":1080}]}`} {
		if validateProbe([]byte(data)) == nil {
			t.Fatal("accepted", data)
		}
	}
	if e := validateProbe([]byte(`{"format":{"duration":"300"},"streams":[{"codec_type":"video","width":1080,"height":1920}]}`)); e != nil {
		t.Fatal(e)
	}
}
func TestStorageTraversalAndImmutableCopy(t *testing.T) {
	files, e := New(config.Config{StorageRoot: t.TempDir()})
	if e != nil {
		t.Fatal(e)
	}
	for _, key := range []string{"", "/etc/passwd", "../escape", "x/../../escape", "x\\escape"} {
		if _, e := files.Path(key); e == nil {
			t.Fatal("accepted", key)
		}
	}
	source := filepath.Join(t.TempDir(), "source")
	if e = os.WriteFile(source, []byte("hello"), 0600); e != nil {
		t.Fatal(e)
	}
	if e = files.Put(context.Background(), "objects/abc", source, "application/octet-stream"); e != nil {
		t.Fatal(e)
	}
	_ = os.WriteFile(source, []byte("different"), 0600)
	path, _ := files.Path("objects/abc")
	data, e := os.ReadFile(path)
	if e != nil || string(data) != "hello" {
		t.Fatal(string(data), e)
	}
}
func TestFFmpegProducesBothBrowserFormats(t *testing.T) {
	ffmpeg, e := exec.LookPath("ffmpeg")
	if e != nil {
		t.Skip("ffmpeg not installed")
	}
	ffprobe, e := exec.LookPath("ffprobe")
	if e != nil {
		t.Skip("ffprobe not installed")
	}
	cfg := config.Config{StorageRoot: t.TempDir(), ForumConfig: config.ForumConfig{ForumFFmpeg: ffmpeg, ForumFFprobe: ffprobe}}
	files, e := New(cfg)
	if e != nil {
		t.Fatal(e)
	}
	source := filepath.Join(t.TempDir(), "input.mp4")
	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()
	if out, e := exec.CommandContext(ctx, ffmpeg, "-v", "error", "-f", "lavfi", "-i", "color=c=blue:s=180x320:r=15:d=1", "-c:v", "libx264", "-pix_fmt", "yuv420p", source).CombinedOutput(); e != nil {
		t.Fatalf("fixture: %v %s", e, out)
	}
	info, _ := os.Stat(source)
	if e = files.Put(ctx, "objects/input", source, "video/mp4"); e != nil {
		t.Fatal(e)
	}
	worker := Worker{Files: files, Config: cfg}
	if e = worker.process(ctx, model.ForumAttachment{Name: "input.mp4", Bytes: info.Size(), ObjectKey: "objects/input"}, "media/test"); e != nil {
		t.Fatal(e)
	}
	for _, name := range []string{"video.mp4", "video.webm", "poster.jpg"} {
		path, _ := files.Path("media/test/" + name)
		info, e := os.Stat(path)
		if e != nil || info.Size() == 0 {
			t.Fatal(name, e)
		}
		if name == "poster.jpg" {
			continue
		}
		out, e := exec.CommandContext(ctx, ffprobe, "-v", "error", "-show_entries", "format=duration:stream=codec_type,width,height", "-of", "json", path).Output()
		if e != nil || validateProbe(out) != nil {
			t.Fatalf("invalid %s %v %s", name, e, out)
		}
	}
}

func TestAbandonedLocalWorkCleanup(t *testing.T) {
	root := t.TempDir()
	old := time.Now().Add(-2 * time.Hour)
	for _, name := range []string{".assemble-old", ".assemble-fresh", "published-object"} {
		path := filepath.Join(root, name)
		if e := os.WriteFile(path, []byte("fixture"), 0600); e != nil {
			t.Fatal(e)
		}
		if name != ".assemble-fresh" {
			if e := os.Chtimes(path, old, old); e != nil {
				t.Fatal(e)
			}
		}
	}
	video := filepath.Join(root, ".video-old")
	if e := os.Mkdir(video, 0700); e != nil {
		t.Fatal(e)
	}
	os.Chtimes(video, old, old)
	cleanupLocalWork(root)
	for _, name := range []string{".assemble-old", ".video-old"} {
		if _, e := os.Stat(filepath.Join(root, name)); !os.IsNotExist(e) {
			t.Fatal("abandoned work retained", name, e)
		}
	}
	for _, name := range []string{".assemble-fresh", "published-object"} {
		if _, e := os.Stat(filepath.Join(root, name)); e != nil {
			t.Fatal("live or published data removed", name, e)
		}
	}
}
