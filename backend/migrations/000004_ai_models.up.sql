-- Administrator-managed model connections. The desktop's model list receives
-- only public fields. A checked one-request lease decrypts and returns the
-- current endpoint and credential after reserving that user's quota.
CREATE TABLE ai_models (
    id uuid PRIMARY KEY,
    display_name text NOT NULL,
    provider text NOT NULL CHECK (provider IN ('openai', 'anthropic')),
    model_name text NOT NULL,
    endpoint_url text NOT NULL,
    api_key_ciphertext text NOT NULL,
    enabled boolean NOT NULL DEFAULT true,
    sort_order integer NOT NULL DEFAULT 0,
    updated_by uuid REFERENCES admin_users(id) ON DELETE SET NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);

CREATE INDEX ai_models_visible_idx
    ON ai_models (enabled, sort_order, display_name);
