CREATE TABLE browser_backgrounds (
    id uuid PRIMARY KEY,
    title text NOT NULL,
    published boolean NOT NULL DEFAULT true,
    sort_order integer NOT NULL DEFAULT 0,
    mime_type text NOT NULL,
    extension text NOT NULL,
    bytes bigint NOT NULL CHECK (bytes > 0),
    width integer NOT NULL CHECK (width > 0),
    height integer NOT NULL CHECK (height > 0),
    sha256 text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX browser_backgrounds_catalog ON browser_backgrounds (published, sort_order, created_at);
