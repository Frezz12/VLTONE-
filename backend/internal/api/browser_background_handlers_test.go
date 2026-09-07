package api

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"image"
	"image/png"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"os"
	"testing"

	"github.com/google/uuid"
)

func checkBrowserBackgrounds(t *testing.T, server *Server, router http.Handler, cookies []*http.Cookie, headers map[string]string) {
	const path = "/v1/admin/browser-backgrounds"
	upload := func(data []byte, auth, csrf bool) *httptest.ResponseRecorder {
		var body bytes.Buffer
		form := multipart.NewWriter(&body)
		_ = form.WriteField("title", "Горы <test>")
		part, _ := form.CreateFormFile("file", "../../wallpaper.png")
		_, _ = part.Write(data)
		_ = form.Close()
		req := httptest.NewRequest("POST", path, &body)
		req.RemoteAddr = "203.0.113.40:1234"
		req.Header.Set("Content-Type", form.FormDataContentType())
		if auth {
			for _, c := range cookies {
				req.AddCookie(c)
			}
		}
		if csrf {
			for k, v := range headers {
				req.Header.Set(k, v)
			}
		}
		res := httptest.NewRecorder()
		router.ServeHTTP(res, req)
		return res
	}
	request := func(url string, auth bool) *httptest.ResponseRecorder {
		req := httptest.NewRequest("GET", url, nil)
		if auth {
			for _, c := range cookies {
				req.AddCookie(c)
			}
		}
		res := httptest.NewRecorder()
		router.ServeHTTP(res, req)
		return res
	}
	if r := upload([]byte("fake"), false, false); r.Code != 401 {
		t.Fatalf("unauth upload: %d", r.Code)
	}
	if r := upload([]byte("fake"), true, false); r.Code != 403 {
		t.Fatalf("csrf upload: %d", r.Code)
	}
	if r := upload([]byte("<svg onload='alert(1)'/>"), true, true); r.Code != 422 {
		t.Fatalf("invalid image: %d", r.Code)
	}
	// 4K must work: the pre-existing avatar/screenshot limit is only 8 MP.
	var source bytes.Buffer
	if err := png.Encode(&source, image.NewRGBA(image.Rect(0, 0, 3840, 2160))); err != nil {
		t.Fatal(err)
	}
	response := upload(source.Bytes(), true, true)
	if response.Code != 201 {
		t.Fatalf("4K upload: %d %s", response.Code, response.Body.String())
	}
	var item browserBackgroundView
	if err := json.Unmarshal(response.Body.Bytes(), &item); err != nil {
		t.Fatal(err)
	}
	if item.ID == uuid.Nil || item.Width != 3840 || item.Height != 2160 || !item.Published {
		t.Fatalf("bad metadata: %+v", item)
	}
	publicPath := "/v1/browser-backgrounds/" + item.ID.String()
	full := request(publicPath+"/image", false)
	digest := sha256.Sum256(full.Body.Bytes())
	if full.Code != 200 || hex.EncodeToString(digest[:]) != item.SHA256 || int64(full.Body.Len()) != item.Bytes {
		t.Fatal("download/hash mismatch")
	}
	if full.Header().Get("X-Content-Type-Options") != "nosniff" {
		t.Fatal("missing image content protection")
	}
	preview := request(publicPath+"/thumbnail", false)
	cfg, _, err := image.DecodeConfig(bytes.NewReader(preview.Body.Bytes()))
	if err != nil || cfg.Width > 480 || cfg.Height > 300 {
		t.Fatalf("preview: %v %+v", err, cfg)
	}
	list := request("/v1/browser-backgrounds", false)
	if list.Code != 200 || !bytes.Contains(list.Body.Bytes(), []byte(item.ID.String())) {
		t.Fatal("published background missing")
	}
	invalid := performJSON(router, "PUT", path+"/"+item.ID.String(), map[string]any{"title": " ", "published": true, "sort_order": -1}, "203.0.113.40:1234", cookies, headers)
	if invalid.Status != 422 {
		t.Fatal("invalid metadata accepted")
	}
	updated := performJSON(router, "PUT", path+"/"+item.ID.String(), map[string]any{"title": "Скрытый фон", "published": false, "sort_order": 7}, "203.0.113.40:1234", cookies, headers)
	if updated.Status != 200 || updated.Body["published"] != false {
		t.Fatalf("hide: %+v", updated)
	}
	if bytes.Contains(request("/v1/browser-backgrounds", false).Body.Bytes(), []byte(item.ID.String())) {
		t.Fatal("hidden background listed publicly")
	}
	for _, suffix := range []string{"/image", "/thumbnail"} {
		if request(publicPath+suffix, false).Code != 404 {
			t.Fatal("hidden image publicly accessible")
		}
		if request(path+"/"+item.ID.String()+suffix, false).Code != 401 {
			t.Fatal("admin image accessible without session")
		}
		if request(path+"/"+item.ID.String()+suffix, true).Code != 200 {
			t.Fatal("admin cannot preview hidden background")
		}
	}
	deleted := performJSON(router, "DELETE", path+"/"+item.ID.String(), nil, "203.0.113.40:1234", cookies, headers)
	if deleted.Status != 204 {
		t.Fatalf("delete: %+v", deleted)
	}
	if _, err := os.Stat(server.backgroundDirectory(item.ID)); !os.IsNotExist(err) {
		t.Fatal("deleted image left on disk")
	}
	if request(path+"/"+item.ID.String()+"/image", true).Code != 404 {
		t.Fatal("deleted image still accessible")
	}
}
