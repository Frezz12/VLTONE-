ALTER TABLE forum_articles ADD COLUMN kind text NOT NULL DEFAULT 'article' CHECK (kind IN ('article','post'));
ALTER TABLE forum_topics ADD COLUMN accepted_post_id uuid REFERENCES forum_posts(id) ON DELETE SET NULL;
ALTER TABLE forum_comments ADD COLUMN parent_id uuid REFERENCES forum_comments(id) ON DELETE SET NULL;
ALTER TABLE forum_profiles ADD COLUMN avatar_id uuid;
ALTER TABLE web_sessions ADD COLUMN device_id uuid REFERENCES devices(id) ON DELETE CASCADE;
CREATE INDEX web_sessions_device_idx ON web_sessions(device_id) WHERE device_id IS NOT NULL;
CREATE TABLE forum_follows (user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE, author_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE, created_at timestamptz NOT NULL DEFAULT now(), PRIMARY KEY(user_id,author_id), CHECK(user_id <> author_id));
CREATE INDEX forum_follows_author_idx ON forum_follows(author_id);
CREATE TABLE forum_bookmarks (user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE, target_type text NOT NULL CHECK(target_type IN ('topic','article')), target_id uuid NOT NULL, created_at timestamptz NOT NULL DEFAULT now(), PRIMARY KEY(user_id,target_type,target_id));
CREATE TABLE forum_notifications (id uuid PRIMARY KEY, user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE, actor_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE, kind text NOT NULL, target_type text NOT NULL, target_id uuid NOT NULL, read_at timestamptz, created_at timestamptz NOT NULL DEFAULT now());
CREATE INDEX forum_notifications_user_idx ON forum_notifications(user_id,created_at DESC,id DESC);
CREATE TABLE forum_reports (id uuid PRIMARY KEY, user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE, target_type text NOT NULL, target_id uuid NOT NULL, reason text NOT NULL, status text NOT NULL DEFAULT 'open' CHECK(status IN ('open','resolved')), created_at timestamptz NOT NULL DEFAULT now());
CREATE TABLE forum_attachments (
 id uuid PRIMARY KEY, owner_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 name text NOT NULL, bytes bigint NOT NULL CHECK(bytes > 0), sha256 text NOT NULL, content_type text NOT NULL,
 kind text NOT NULL CHECK(kind IN ('image','video','vlt')), resource_type text NOT NULL DEFAULT '', minimum_version text NOT NULL DEFAULT '',
 status text NOT NULL DEFAULT 'uploading' CHECK(status IN ('uploading','ready','cancelled')),
 media_status text NOT NULL DEFAULT 'none' CHECK(media_status IN ('none','pending','processing','ready','failed','rejected')),
 media_key text NOT NULL DEFAULT '', media_error text NOT NULL DEFAULT '', object_key text NOT NULL, provider_upload_id text NOT NULL DEFAULT '',
 part_bytes bigint NOT NULL DEFAULT 0, upload_token_hash text NOT NULL DEFAULT '', expires_at timestamptz NOT NULL,
 article_id uuid REFERENCES forum_articles(id) ON DELETE CASCADE, topic_id uuid REFERENCES forum_topics(id) ON DELETE CASCADE,
 post_id uuid REFERENCES forum_posts(id) ON DELETE CASCADE, comment_id uuid REFERENCES forum_comments(id) ON DELETE CASCADE,
 created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
 CHECK(num_nonnulls(article_id,topic_id,post_id,comment_id) <= 1)
);
ALTER TABLE forum_profiles ADD CONSTRAINT forum_avatar_fk FOREIGN KEY(avatar_id) REFERENCES forum_attachments(id) ON DELETE SET NULL;
CREATE INDEX forum_attachments_owner_idx ON forum_attachments(owner_id,status);
CREATE INDEX forum_attachments_article_idx ON forum_attachments(article_id);
CREATE INDEX forum_attachments_topic_idx ON forum_attachments(topic_id);
CREATE INDEX forum_attachments_post_idx ON forum_attachments(post_id);
CREATE INDEX forum_attachments_comment_idx ON forum_attachments(comment_id);
CREATE TABLE forum_media_jobs (attachment_id uuid PRIMARY KEY REFERENCES forum_attachments(id) ON DELETE CASCADE, attempts integer NOT NULL DEFAULT 0, available_at timestamptz NOT NULL DEFAULT now(), lease_until timestamptz, lease_id uuid, last_error text NOT NULL DEFAULT '', created_at timestamptz NOT NULL DEFAULT now());
CREATE INDEX forum_media_jobs_due_idx ON forum_media_jobs(available_at,lease_until);
CREATE TABLE forum_sso_codes (id uuid PRIMARY KEY, token_hash text NOT NULL UNIQUE, user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE, device_id uuid REFERENCES devices(id) ON DELETE CASCADE, source_session_id uuid REFERENCES web_sessions(id) ON DELETE CASCADE, challenge text NOT NULL, state text NOT NULL, redirect_uri text NOT NULL, expires_at timestamptz NOT NULL, consumed_at timestamptz);
CREATE INDEX forum_sso_expiry_idx ON forum_sso_codes(expires_at);
INSERT INTO forum_sections(id,slug,title_ru,title_en,description_ru,description_en,sort_order) VALUES
 ('a0000000-0000-4000-8000-000000000006','creator','Creator и Mini','Creator and Mini','Создаём модули и обмениваемся опытом','Build modules and share knowledge',25),
 ('a0000000-0000-4000-8000-000000000007','feedback','Ошибки и предложения','Bugs and ideas','Помогаем сделать VLTONE лучше','Help improve VLTONE',22) ON CONFLICT(slug) DO NOTHING;
CREATE INDEX forum_articles_feed_idx ON forum_articles(published_at DESC,id DESC) WHERE status='published';
CREATE TABLE forum_object_deletions (id uuid PRIMARY KEY, object_key text NOT NULL, media_key text NOT NULL DEFAULT '', provider_upload_id text NOT NULL DEFAULT '', created_at timestamptz NOT NULL DEFAULT now());
CREATE FUNCTION forum_queue_object_deletion() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN
 INSERT INTO forum_object_deletions(id,object_key,media_key,provider_upload_id) VALUES(OLD.id,OLD.object_key,OLD.media_key,OLD.provider_upload_id) ON CONFLICT(id) DO NOTHING; RETURN OLD;
END $$;
CREATE TRIGGER forum_attachment_deleted AFTER DELETE ON forum_attachments FOR EACH ROW EXECUTE FUNCTION forum_queue_object_deletion();
