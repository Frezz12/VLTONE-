CREATE TABLE release_download_clicks (
    release_id uuid NOT NULL REFERENCES releases(id) ON DELETE CASCADE,
    kind text NOT NULL,
    clicks bigint NOT NULL DEFAULT 0 CHECK (clicks >= 0),
    first_clicked_at timestamptz NOT NULL DEFAULT now(),
    last_clicked_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (release_id, kind)
);
