"use client";
import { useEffect, useState } from "react";
import Link from "next/link";
import Markdown from "react-markdown";
import { api, type ForumAttachment, type ForumContent } from "@vlt/api-client";
import {
  Heart,
  MessageCircle,
  Bookmark,
  Download,
  FileBox,
  MoreHorizontal,
  Check,
  Quote,
} from "lucide-react";
import { useForum } from "./shell";
import { Composer } from "./composer";
export const attachmentURL = (id: string, variant?: string) =>
  `/api/v1/forum/attachments/${id}/${variant ? `media/${variant}` : "download"}`;
export function RichText({ text }: { text: string }) {
  return (
    <div className="prose">
      <Markdown
        skipHtml
        components={{
          a: ({ children, href }) => (
            <a href={href} target="_blank" rel="nofollow noopener noreferrer">
              {children}
            </a>
          ),
          img: ({ alt }) => <span>{alt}</span>,
        }}
      >
        {text}
      </Markdown>
    </div>
  );
}
export function Attachment({ value }: { value: ForumAttachment }) {
  const { t } = useForum();
  const [a, setA] = useState(value);
  useEffect(() => setA(value), [value]);
  useEffect(() => {
    if (
      a.kind !== "video" ||
      !["pending", "processing"].includes(a.media_status)
    )
      return;
    const timer = setInterval(() => {
      if (!document.hidden)
        api
          .request<{ attachment: ForumAttachment }>(
            `/v1/forum/attachments/${a.id}`,
          )
          .then((r) => setA(r.attachment))
          .catch(() => {});
    }, 5000);
    return () => clearInterval(timer);
  }, [a.id, a.kind, a.media_status]);
  if (a.kind === "image")
    return (
      <a className="image-attachment" href={attachmentURL(a.id)}>
        <img
          src={attachmentURL(a.id, "original")}
          alt={a.name}
          loading="lazy"
        />
      </a>
    );
  if (a.kind === "video")
    return (
      <div className="video-attachment">
        {a.media_status === "ready" ? (
          <video
            controls
            playsInline
            preload="metadata"
            poster={attachmentURL(a.id, "poster.jpg")}
          >
            <source src={attachmentURL(a.id, "video.mp4")} type="video/mp4" />
            <source src={attachmentURL(a.id, "video.webm")} type="video/webm" />
            {t(
              "Браузер не поддерживает воспроизведение.",
              "Your browser cannot play this video.",
            )}
          </video>
        ) : (
          <div className="video-state">
            <FileBox size={24} />
            <strong>{a.name}</strong>
            <span>
              {a.media_status === "failed" || a.media_status === "rejected"
                ? t(
                    "Не удалось подготовить видео. Автор может заменить файл.",
                    "Video could not be prepared. The author can replace the file.",
                  )
                : t("Готовим видео к просмотру…", "Preparing your video…")}
            </span>
          </div>
        )}
        {a.media_status !== "rejected" && (
          <a className="file-caption" href={attachmentURL(a.id)}>
            <Download size={15} />
            {t("Скачать оригинал", "Download original")} ·{" "}
            {(a.bytes / 1000000).toFixed(1)} MB
          </a>
        )}
      </div>
    );
  return (
    <a className="file-attachment" href={attachmentURL(a.id)}>
      <span className="file-icon">
        <FileBox size={23} />
      </span>
      <span>
        <strong>{a.name}</strong>
        <small>
          {a.resource_type.toUpperCase()} · {(a.bytes / 1000000).toFixed(2)} MB
          {a.minimum_version && ` · VLTONE ${a.minimum_version}+`}
        </small>
      </span>
      <Download size={19} className="push" />
    </a>
  );
}
export function ContentCard({
  content,
  kind = "article",
  detail = false,
  replyCount = 0,
  onChange,
  onQuote,
  onAccept,
  accepted = false,
}: {
  content: ForumContent;
  kind?: "article" | "topic" | "post" | "comment";
  detail?: boolean;
  replyCount?: number;
  onChange?: () => void;
  onQuote?: (text: string, id: string) => void;
  onAccept?: () => void;
  accepted?: boolean;
}) {
  const { session, t, locale, login, notify } = useForum();
  const [liked, setLiked] = useState(content.liked);
  const [likes, setLikes] = useState(content.like_count);
  const [saved, setSaved] = useState(content.saved ?? false);
  const [busy, setBusy] = useState(false);
  const [editing, setEditing] = useState<ForumContent>();
  const own = session?.user.id === content.author.id;
  useEffect(() => {
    setLiked(content.liked);
    setLikes(content.like_count);
    setSaved(content.saved ?? false);
  }, [content.liked, content.like_count, content.saved]);
  const href =
    kind === "topic" ? `/topics/${content.id}` : `/posts/${content.id}`;
  async function action(fn: () => Promise<void>) {
    if (!session) {
      login();
      return;
    }
    setBusy(true);
    try {
      await fn();
    } catch (e) {
      notify((e as Error).message);
    } finally {
      setBusy(false);
    }
  }
  if (editing)
    return (
      <Composer
        existing={editing}
        kind={kind}
        onSaved={() => {
          setEditing(undefined);
          onChange?.();
        }}
        onCancel={() => setEditing(undefined)}
      />
    );
  return (
    <article
      className={`post-card ${accepted ? "accepted" : ""}`}
      id={content.id}
    >
      <header className="post-author">
        <Link
          href={`/u/${encodeURIComponent(content.author.nickname)}`}
          className="avatar"
        >
          {content.author.avatar_id ? (
            <img
              src={attachmentURL(content.author.avatar_id, "original")}
              alt=""
            />
          ) : (
            content.author.nickname.slice(0, 1).toUpperCase()
          )}
        </Link>
        <div>
          <Link
            className="author-name"
            href={`/u/${encodeURIComponent(content.author.nickname)}`}
          >
            {content.author.nickname}
          </Link>
          <time dateTime={content.created_at}>
            {new Date(content.created_at).toLocaleString(
              locale === "ru" ? "ru-RU" : "en-US",
              { dateStyle: "medium", timeStyle: "short" },
            )}
          </time>
        </div>
        {accepted && (
          <span className="accepted-label">
            <Check size={14} />
            {t("Решение", "Solution")}
          </span>
        )}
        <details className="post-menu push">
          <summary aria-label={t("Действия с публикацией", "Post actions")}>
            <MoreHorizontal size={20} />
          </summary>
          <div>
            {own && (
              <>
                <button
                  onClick={() =>
                    void action(async () => {
                      if (kind === "article" || kind === "topic") {
                        const r = await api.request<
                          Record<string, ForumContent>
                        >(
                          `/v1/forum/${kind === "article" ? "articles" : "topics"}/${content.id}`,
                        );
                        setEditing(r[kind]);
                      } else setEditing(content);
                    })
                  }
                >
                  {t("Редактировать", "Edit")}
                </button>
                <button
                  disabled={busy}
                  onClick={() => {
                    if (confirm(t("Удалить публикацию?", "Delete this post?")))
                      void action(async () => {
                        await api.json(
                          `/v1/forum/${kind === "article" ? "articles" : kind === "topic" ? "topics" : kind === "post" ? "posts" : "comments"}/${content.id}`,
                          "DELETE",
                          {},
                          session!.csrf_token,
                        );
                        onChange?.();
                      });
                  }}
                >
                  {t("Удалить", "Delete")}
                </button>
              </>
            )}
            <button
              onClick={() => {
                if (!session) {
                  login();
                  return;
                }
                const reason = prompt(
                  t("Причина жалобы", "Reason for reporting"),
                );
                if (reason)
                  void action(async () => {
                    await api.json(
                      "/v1/forum/reports",
                      "POST",
                      { target_type: kind, target_id: content.id, reason },
                      session!.csrf_token,
                    );
                    notify(
                      t(
                        "Жалоба отправлена администраторам.",
                        "Report sent to administrators.",
                      ),
                    );
                  });
              }}
            >
              {t("Пожаловаться", "Report")}
            </button>
          </div>
        </details>
      </header>
      {content.title && (
        <h2 className="post-title">
          {detail ? content.title : <Link href={href}>{content.title}</Link>}
        </h2>
      )}
      <RichText text={content.body || content.excerpt || ""} />
      {(content.attachments ?? []).map((a) => (
        <Attachment key={a.id} value={a} />
      ))}
      <footer className="post-actions">
        <button
          className={liked ? "liked" : ""}
          aria-pressed={liked}
          disabled={busy}
          onClick={() =>
            void action(async () => {
              const r = await api.json<{ liked: boolean; like_count: number }>(
                `/v1/forum/reactions/${kind}/${content.id}`,
                liked ? "DELETE" : "PUT",
                {},
                session!.csrf_token,
              );
              setLiked(r.liked);
              setLikes(r.like_count);
            })
          }
        >
          <Heart size={17} fill={liked ? "currentColor" : "none"} />
          <span>{likes}</span>
          <span className="sr-only">{t("Нравится", "Like")}</span>
        </button>
        {(kind === "article" || kind === "topic") && (
          <>
            <Link href={href}>
              <MessageCircle size={17} />
              {replyCount}
              <span className="sr-only">{t("Ответы", "Replies")}</span>
            </Link>
            <button
              className="push"
              aria-label={
                saved ? t("Сохранено", "Saved") : t("Сохранить", "Save")
              }
              aria-pressed={saved}
              disabled={busy}
              onClick={() =>
                void action(async () => {
                  await api.json(
                    `/v1/forum/bookmarks/${kind}/${content.id}`,
                    saved ? "DELETE" : "PUT",
                    {},
                    session!.csrf_token,
                  );
                  setSaved(!saved);
                })
              }
            >
              <Bookmark size={17} fill={saved ? "currentColor" : "none"} />
              <span>
                {saved ? t("Сохранено", "Saved") : t("Сохранить", "Save")}
              </span>
            </button>
          </>
        )}
        {onQuote && (
          <button onClick={() => onQuote(content.body, content.id)}>
            <Quote size={16} />
            {t("Ответить", "Reply")}
          </button>
        )}
        {onAccept && (
          <button onClick={onAccept}>
            <Check size={16} />
            {t("Отметить решением", "Mark as solution")}
          </button>
        )}
      </footer>
    </article>
  );
}
