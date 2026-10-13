export type ForumAttachment = {
  id: string;
  name: string;
  bytes: number;
  sha256: string;
  content_type: string;
  kind: "image" | "video" | "vlt";
  resource_type: string;
  minimum_version: string;
  status: "uploading" | "ready" | "cancelled";
  media_status:
    | "none"
    | "pending"
    | "processing"
    | "ready"
    | "failed"
    | "rejected";
  media_error: string;
};
export type ForumAuthor = { id: string; nickname: string; avatar_id?: string };
export type ForumContent = {
  id: string;
  author: ForumAuthor;
  body: string;
  title?: string;
  excerpt?: string;
  kind?: "article" | "post";
  status?: string;
  created_at: string;
  like_count: number;
  liked: boolean;
  saved?: boolean;
  attachments: ForumAttachment[];
  can_edit?: boolean;
};
export type ForumArticle = ForumContent & {
  comment_count: number;
  published_at?: string;
};
export type ForumTopic = ForumContent & {
  section_id: string;
  section?: { slug: string };
  pinned: boolean;
  locked: boolean;
  accepted_post_id?: string;
  reply_count: number;
  view_count: number;
  last_post_at: string;
};
export type ForumReply = ForumContent & {
  topic_id?: string;
  article_id?: string;
  parent_id?: string;
};
export type ForumSection = {
  id: string;
  slug: string;
  title_ru: string;
  title_en: string;
  description_ru: string;
  description_en: string;
  sort_order: number;
  topic_count: number;
};
export type ForumProfile = {
  about: string;
  location: string;
  interests: string;
  signature: string;
  avatar_id?: string;
  links: { label: string; url: string }[];
};
export type ForumNotification = {
  id: string;
  actor_id: string;
  actor: ForumAuthor;
  kind: "reply" | "comment" | "comment_reply" | "follow";
  target_type: "topic" | "article" | "user";
  target_id: string;
  created_at: string;
  read_at?: string;
};
export type ForumConfig = {
  public_origin: string;
  forum_origin: string;
  video_max_bytes: number;
  video_max_seconds: number;
  file_max_bytes: number;
  image_max_bytes: number;
  max_attachments: number;
};
export type ForumUpload = {
  attachment: ForumAttachment;
  parts: {
    number: number;
    bytes: number;
    done: boolean;
    request?: {
      method: string;
      url: string;
      headers: Record<string, string>;
      expiresAt: string;
    };
  }[];
};
