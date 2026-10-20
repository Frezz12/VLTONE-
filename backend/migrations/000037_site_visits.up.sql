CREATE TABLE site_visits (
    id uuid PRIMARY KEY,
    visited_at timestamptz NOT NULL DEFAULT now(),
    visitor_key text NOT NULL,
    user_id uuid REFERENCES users(id) ON DELETE SET NULL,
    path text NOT NULL CHECK (length(path) <= 512),
    referrer_host text NOT NULL DEFAULT '',
    browser text NOT NULL,
    os text NOT NULL
);
CREATE INDEX site_visits_time ON site_visits (visited_at DESC, id DESC);
CREATE INDEX site_visits_visitor_time ON site_visits (visitor_key, visited_at DESC);
