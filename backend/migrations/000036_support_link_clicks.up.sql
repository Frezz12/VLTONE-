CREATE TABLE support_link_clicks (
    source text PRIMARY KEY CHECK (source IN ('home', 'footer')),
    clicks bigint NOT NULL DEFAULT 0 CHECK (clicks >= 0),
    first_clicked_at timestamptz NOT NULL DEFAULT now(),
    last_clicked_at timestamptz NOT NULL DEFAULT now()
);
