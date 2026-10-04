CREATE TABLE forum_sections (
    id uuid PRIMARY KEY,
    slug text NOT NULL UNIQUE,
    title_ru text NOT NULL DEFAULT '',
    title_en text NOT NULL DEFAULT '',
    description_ru text NOT NULL DEFAULT '',
    description_en text NOT NULL DEFAULT '',
    sort_order integer NOT NULL DEFAULT 0,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);

INSERT INTO forum_sections (id, slug, title_ru, title_en, description_ru, description_en, sort_order)
VALUES
    ('a0000000-0000-4000-8000-000000000001', 'general', 'Обсуждения', 'General', 'Общие разговоры о Velton', 'General discussion about Velton', 10),
    ('a0000000-0000-4000-8000-000000000002', 'help', 'Вопросы и помощь', 'Questions and help', 'Помощь с программой и взаимопомощь', 'Help with the app and community support', 20),
    ('a0000000-0000-4000-8000-000000000003', 'files', 'Файлы и пресеты', 'Files and presets', 'Обмен файлами, пресетами и темами для Velton', 'Sharing files, presets and themes for Velton', 30),
    ('a0000000-0000-4000-8000-000000000004', 'creative', 'Творчество', 'Creative', 'Демо, проекты и творческий обмен', 'Demos, projects and creative exchange', 40),
    ('a0000000-0000-4000-8000-000000000005', 'offtopic', 'Оффтоп', 'Off-topic', 'Всё остальное', 'Everything else', 50);

CREATE TABLE forum_profiles (
    user_id uuid PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    about text NOT NULL DEFAULT '',
    location text NOT NULL DEFAULT '',
    family_status text NOT NULL DEFAULT '',
    interests text NOT NULL DEFAULT '',
    links jsonb NOT NULL DEFAULT '[]',
    signature text NOT NULL DEFAULT '',
    profile_completed boolean NOT NULL DEFAULT false,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE forum_topics (
    id uuid PRIMARY KEY,
    section_id uuid NOT NULL REFERENCES forum_sections(id) ON DELETE RESTRICT,
    author_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    title text NOT NULL,
    body text NOT NULL,
    pinned boolean NOT NULL DEFAULT false,
    locked boolean NOT NULL DEFAULT false,
    status text NOT NULL DEFAULT 'published' CHECK (status IN ('published', 'hidden')),
    reply_count integer NOT NULL DEFAULT 0,
    like_count integer NOT NULL DEFAULT 0,
    view_count bigint NOT NULL DEFAULT 0,
    last_post_at timestamptz NOT NULL DEFAULT now(),
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX forum_topics_section_activity_idx ON forum_topics(section_id, pinned DESC, last_post_at DESC);
CREATE INDEX forum_topics_author_idx ON forum_topics(author_id, created_at DESC);

CREATE TABLE forum_posts (
    id uuid PRIMARY KEY,
    topic_id uuid NOT NULL REFERENCES forum_topics(id) ON DELETE CASCADE,
    author_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    body text NOT NULL,
    status text NOT NULL DEFAULT 'published' CHECK (status IN ('published', 'hidden')),
    like_count integer NOT NULL DEFAULT 0,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX forum_posts_topic_idx ON forum_posts(topic_id, created_at);
CREATE INDEX forum_posts_author_idx ON forum_posts(author_id, created_at DESC);

CREATE TABLE forum_articles (
    id uuid PRIMARY KEY,
    author_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    title text NOT NULL,
    excerpt text NOT NULL DEFAULT '',
    body text NOT NULL,
    status text NOT NULL DEFAULT 'published' CHECK (status IN ('draft', 'published', 'hidden')),
    like_count integer NOT NULL DEFAULT 0,
    comment_count integer NOT NULL DEFAULT 0,
    published_at timestamptz,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX forum_articles_published_idx ON forum_articles(status, published_at DESC);
CREATE INDEX forum_articles_author_idx ON forum_articles(author_id, created_at DESC);

CREATE TABLE forum_comments (
    id uuid PRIMARY KEY,
    article_id uuid NOT NULL REFERENCES forum_articles(id) ON DELETE CASCADE,
    author_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    body text NOT NULL,
    status text NOT NULL DEFAULT 'published' CHECK (status IN ('published', 'hidden')),
    like_count integer NOT NULL DEFAULT 0,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX forum_comments_article_idx ON forum_comments(article_id, created_at);
CREATE INDEX forum_comments_author_idx ON forum_comments(author_id, created_at DESC);

CREATE TABLE forum_reactions (
    id uuid PRIMARY KEY,
    user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    target_type text NOT NULL CHECK (target_type IN ('topic', 'post', 'article', 'comment')),
    target_id uuid NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE (target_type, target_id, user_id)
);
CREATE INDEX forum_reactions_user_idx ON forum_reactions(user_id, created_at DESC);
