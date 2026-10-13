package config

import (
	"fmt"
	"net/url"
	"strings"
)

type ForumConfig struct {
	ForumObjectEndpoint        string
	ForumObjectRegion          string
	ForumObjectBucket          string
	ForumObjectAccessKeyID     string
	ForumObjectSecretAccessKey string
	ForumObjectSessionToken    string
	ForumVideoMaxBytes         int64
	ForumFileMaxBytes          int64
	ForumImageMaxBytes         int64
	ForumUserMaxBytes          int64
	ForumFFmpeg                string
	ForumFFprobe               string
}

func loadForumConfig() ForumConfig {
	return ForumConfig{
		ForumObjectEndpoint: strings.TrimRight(env("FORUM_OBJECT_ENDPOINT", ""), "/"), ForumObjectRegion: env("FORUM_OBJECT_REGION", "us-east-1"), ForumObjectBucket: env("FORUM_OBJECT_BUCKET", ""), ForumObjectAccessKeyID: env("FORUM_OBJECT_ACCESS_KEY_ID", ""), ForumObjectSecretAccessKey: env("FORUM_OBJECT_SECRET_ACCESS_KEY", ""), ForumObjectSessionToken: env("FORUM_OBJECT_SESSION_TOKEN", ""),
		ForumVideoMaxBytes: boundedInt64Env("FORUM_VIDEO_MAX_BYTES", 250000000, 1000000, 1000000000), ForumFileMaxBytes: boundedInt64Env("FORUM_FILE_MAX_BYTES", 1000000000, 1000000, 8000000000), ForumImageMaxBytes: boundedInt64Env("FORUM_IMAGE_MAX_BYTES", 10000000, 1000000, 50000000), ForumUserMaxBytes: boundedInt64Env("FORUM_USER_MAX_BYTES", 10000000000, 1000000000, 1000000000000), ForumFFmpeg: env("FORUM_FFMPEG", "ffmpeg"), ForumFFprobe: env("FORUM_FFPROBE", "ffprobe")}
}
func (c ForumConfig) validate(production bool) error {
	if c.ForumObjectEndpoint == "" {
		if c.ForumObjectBucket != "" || c.ForumObjectAccessKeyID != "" || c.ForumObjectSecretAccessKey != "" {
			return fmt.Errorf("FORUM_OBJECT_ENDPOINT is required with forum storage credentials")
		}
		return nil
	}
	u, e := url.Parse(c.ForumObjectEndpoint)
	if e != nil || u.Host == "" || u.User != nil || u.RawQuery != "" || u.Fragment != "" || u.Path != "" || (u.Scheme != "https" && u.Scheme != "http") || (production && u.Scheme != "https") {
		return fmt.Errorf("invalid FORUM_OBJECT_ENDPOINT")
	}
	if !validS3Bucket(c.ForumObjectBucket) || c.ForumObjectAccessKeyID == "" || c.ForumObjectSecretAccessKey == "" {
		return fmt.Errorf("forum object storage requires a bucket and credentials")
	}
	return nil
}
