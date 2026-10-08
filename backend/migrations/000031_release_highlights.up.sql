ALTER TABLE releases ADD COLUMN highlights jsonb NOT NULL DEFAULT '[]'::jsonb;
ALTER TABLE releases ADD CONSTRAINT release_highlights_array CHECK (jsonb_typeof(highlights) = 'array');
