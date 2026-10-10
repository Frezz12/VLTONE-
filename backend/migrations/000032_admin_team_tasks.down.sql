DROP TABLE admin_task_attachments;
DROP TABLE admin_task_comments;
DROP TABLE admin_tasks;
ALTER TABLE admin_users DROP COLUMN permissions, DROP COLUMN is_owner, DROP COLUMN user_id;
