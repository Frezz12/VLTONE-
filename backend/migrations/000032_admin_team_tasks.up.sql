ALTER TABLE admin_users ADD COLUMN user_id uuid UNIQUE REFERENCES users(id) ON DELETE SET NULL;
ALTER TABLE admin_users ADD COLUMN is_owner boolean NOT NULL DEFAULT false;
ALTER TABLE admin_users ADD COLUMN permissions jsonb NOT NULL DEFAULT '[]' CHECK (jsonb_typeof(permissions) = 'array');
-- Preserve existing administrators as owners; new grants never become owners.
UPDATE admin_users SET is_owner = true;

CREATE TABLE admin_tasks (
    id uuid PRIMARY KEY,
    number bigserial UNIQUE NOT NULL,
    title text NOT NULL CHECK (length(title) BETWEEN 1 AND 200),
    description text NOT NULL DEFAULT '' CHECK (length(description) <= 20000),
    status text NOT NULL DEFAULT 'idea' CHECK (status IN ('idea','planned','in_progress','review','done','archived')),
    priority text NOT NULL DEFAULT 'normal' CHECK (priority IN ('low','normal','high','urgent')),
    author_id uuid NOT NULL REFERENCES admin_users(id),
    assignee_id uuid REFERENCES admin_users(id),
    due_date date,
    version integer NOT NULL DEFAULT 1,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX admin_tasks_created ON admin_tasks(created_at DESC, id DESC);
CREATE TABLE admin_task_comments (
    id uuid PRIMARY KEY,
    task_id uuid NOT NULL REFERENCES admin_tasks(id) ON DELETE CASCADE,
    author_id uuid NOT NULL REFERENCES admin_users(id),
    body text NOT NULL CHECK (length(body) BETWEEN 1 AND 10000),
    created_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX admin_task_comments_task ON admin_task_comments(task_id, created_at);
CREATE TABLE admin_task_attachments (
    id uuid PRIMARY KEY,
    task_id uuid NOT NULL REFERENCES admin_tasks(id) ON DELETE CASCADE,
    author_id uuid NOT NULL REFERENCES admin_users(id),
    name text NOT NULL,
    size bigint NOT NULL CHECK (size BETWEEN 1 AND 10485760),
    data bytea NOT NULL CHECK (octet_length(data) = size),
    created_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX admin_task_attachments_task ON admin_task_attachments(task_id);
