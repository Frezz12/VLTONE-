CREATE TABLE releases (
    id uuid PRIMARY KEY,
    version text,
    version_major integer,
    version_minor integer,
    version_patch integer,
    status text NOT NULL DEFAULT 'draft' CHECK (status IN ('draft', 'published')),
    summary_ru text NOT NULL DEFAULT '',
    summary_en text NOT NULL DEFAULT '',
    features_ru jsonb NOT NULL DEFAULT '[]'::jsonb,
    features_en jsonb NOT NULL DEFAULT '[]'::jsonb,
    changes_ru jsonb NOT NULL DEFAULT '[]'::jsonb,
    changes_en jsonb NOT NULL DEFAULT '[]'::jsonb,
    fixes_ru jsonb NOT NULL DEFAULT '[]'::jsonb,
    fixes_en jsonb NOT NULL DEFAULT '[]'::jsonb,
    created_by uuid REFERENCES admin_users(id) ON DELETE SET NULL,
    updated_by uuid REFERENCES admin_users(id) ON DELETE SET NULL,
    published_at timestamptz,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now(),
    CHECK ((version IS NULL AND version_major IS NULL AND version_minor IS NULL AND version_patch IS NULL)
        OR (version IS NOT NULL AND version_major >= 0 AND version_minor >= 0 AND version_patch >= 0)),
    CHECK (status = 'draft' OR (version IS NOT NULL AND published_at IS NOT NULL
        AND length(btrim(summary_ru)) > 0 AND length(btrim(summary_en)) > 0))
);

CREATE UNIQUE INDEX releases_version_unique_idx ON releases (version) WHERE version IS NOT NULL;
CREATE INDEX releases_public_order_idx
    ON releases (status, version_major DESC, version_minor DESC, version_patch DESC);

CREATE TABLE release_artifacts (
    id uuid PRIMARY KEY,
    release_id uuid NOT NULL REFERENCES releases(id) ON DELETE CASCADE,
    kind text NOT NULL CHECK (kind IN (
        'windows-exe', 'macos-dmg', 'linux-appimage', 'linux-deb',
        'linux-rpm', 'linux-tar-gz', 'linux-tar-xz')),
    file_name text NOT NULL,
    mime_type text NOT NULL,
    path text NOT NULL,
    bytes bigint NOT NULL CHECK (bytes > 0),
    sha256 text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE (release_id, kind)
);

CREATE INDEX release_artifacts_release_idx ON release_artifacts (release_id);

CREATE TABLE release_screenshots (
    id uuid PRIMARY KEY,
    release_id uuid NOT NULL REFERENCES releases(id) ON DELETE CASCADE,
    caption_ru text NOT NULL DEFAULT '',
    caption_en text NOT NULL DEFAULT '',
    sort_order integer NOT NULL DEFAULT 0,
    mime_type text NOT NULL,
    path text NOT NULL,
    bytes bigint NOT NULL CHECK (bytes > 0),
    width integer NOT NULL CHECK (width > 0),
    height integer NOT NULL CHECK (height > 0),
    sha256 text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);

CREATE INDEX release_screenshots_release_order_idx
    ON release_screenshots (release_id, sort_order, created_at);
