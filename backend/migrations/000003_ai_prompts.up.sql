-- The assistant's instructions, edited in the admin panel and served to the
-- desktop. Seeded from the text compiled into the binary (internal/promptlib)
-- on the first run that finds the table empty, so a fresh deployment already
-- serves the same prompts the app ships with.
CREATE TABLE ai_prompt_documents (
    id text PRIMARY KEY,
    kind text NOT NULL,
    title text NOT NULL DEFAULT '',
    use_when text NOT NULL DEFAULT '',
    tags jsonb NOT NULL DEFAULT '[]'::jsonb,
    body text NOT NULL,
    enabled boolean NOT NULL DEFAULT true,
    updated_by uuid REFERENCES admin_users(id) ON DELETE SET NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);

-- Every edit keeps the text it replaced. Prompts are behaviour: an edit that
-- makes the assistant worse has to be reversible without a database restore.
CREATE TABLE ai_prompt_revisions (
    id uuid PRIMARY KEY,
    document_id text NOT NULL REFERENCES ai_prompt_documents(id) ON DELETE CASCADE,
    title text NOT NULL DEFAULT '',
    use_when text NOT NULL DEFAULT '',
    tags jsonb NOT NULL DEFAULT '[]'::jsonb,
    body text NOT NULL,
    updated_by uuid REFERENCES admin_users(id) ON DELETE SET NULL,
    created_at timestamptz NOT NULL DEFAULT now()
);

CREATE INDEX ai_prompt_revisions_document_idx
    ON ai_prompt_revisions (document_id, created_at DESC);
