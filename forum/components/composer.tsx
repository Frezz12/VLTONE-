"use client";
import { useEffect, useRef, useState } from "react";
import { Paperclip, Send, X } from "lucide-react";
import {
  api,
  type ForumAttachment,
  type ForumContent,
  type ForumSection,
  type ForumTopic,
  type ForumConfig,
} from "@vlt/api-client";
import { useForum, useResource } from "./shell";
import { uploadFile } from "@/lib/upload";
type UploadItem = {
  metadata?: { resource_type?: string; minimum_version?: string };
  key: string;
  file: File;
  id?: string;
  progress: number;
  attachment?: ForumAttachment;
  error?: string;
  controller: AbortController;
};
export function Composer({
  kind = "article",
  parent,
  section,
  existing,
  quote = "",
  parentComment,
  onSaved,
  onCancel,
}: {
  kind?: "article" | "topic" | "post" | "comment";
  parent?: string;
  section?: string;
  existing?: ForumContent;
  quote?: string;
  parentComment?: string;
  onSaved: (id: string) => void;
  onCancel?: () => void;
}) {
  const { session, t, login } = useForum();
  const [presetDestination, setPresetDestination] = useState("strip");
  const [minimumVersion, setMinimumVersion] = useState("");
  const [title, setTitle] = useState(existing?.title ?? "");
  const [body, setBody] = useState(existing?.body ?? quote);
  const [sectionID, setSectionID] = useState(
    section ?? (existing as ForumTopic)?.section_id ?? "",
  );
  const [attached, setAttached] = useState(existing?.attachments ?? []);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const [items, setItems] = useState<UploadItem[]>([]);
  const itemsRef = useRef(items);
  itemsRef.current = items;
  const { data: sections } = useResource<{ sections: ForumSection[] }>(
    "/v1/forum/sections",
  );
  const { data: limits } = useResource<ForumConfig>("/v1/forum/config");
  useEffect(
    () => () => {
      itemsRef.current.forEach((i) => i.controller.abort());
    },
    [],
  );
  useEffect(() => {
    if (quote) setBody(quote);
  }, [quote]);
  function patch(key: string, values: Partial<UploadItem>) {
    setItems((list) =>
      list.map((i) => (i.key === key ? { ...i, ...values } : i)),
    );
  }
  async function start(item: UploadItem) {
    if (!session) return;
    patch(item.key, { error: undefined });
    try {
      const attachment = await uploadFile(
        item.file,
        session.csrf_token,
        item.controller.signal,
        (p) => patch(item.key, { progress: p }),
        (id) => patch(item.key, { id }),
        item.id,
        item.metadata,
      );
      patch(item.key, { attachment, id: attachment.id, progress: 100 });
    } catch (e) {
      patch(item.key, {
        error:
          (e as Error).name === "AbortError"
            ? t("Отменено", "Cancelled")
            : (e as Error).message,
      });
    }
  }
  function add(files: FileList | null) {
    if (!files || !limits) return;
    const next: UploadItem[] = [];
    let videos =
      items.filter((i) => /\.(mp4|mov|webm)$/i.test(i.file.name)).length +
      attached.filter((a) => a.kind === "video").length;
    for (const file of Array.from(files)) {
      const video = /\.(mp4|mov|webm)$/i.test(file.name);
      const image = /\.(png|jpe?g|webp|gif)$/i.test(file.name);
      if (
        items.length + next.length + attached.length >= 10 ||
        file.size >
          (video
            ? limits.video_max_bytes
            : image
              ? limits.image_max_bytes
              : limits.file_max_bytes) ||
        (video && videos >= 1)
      ) {
        setError(
          t(
            "Превышен лимит размера или количества файлов.",
            "File size or attachment count limit exceeded.",
          ),
        );
        continue;
      }
      if (video) videos++;
      next.push({
        metadata: {
          resource_type: /\.vlts$/i.test(file.name)
            ? presetDestination
            : undefined,
          minimum_version: minimumVersion || undefined,
        },
        key: crypto.randomUUID(),
        file,
        progress: 0,
        controller: new AbortController(),
      });
    }
    setItems((list) => [...list, ...next]);
    next.forEach((i) => void start(i));
  }
  async function remove(item: UploadItem) {
    item.controller.abort();
    if (item.id && session) {
      try {
        await api.json(
          `/v1/forum/uploads/${item.id}`,
          "DELETE",
          {},
          session.csrf_token,
        );
      } catch (e) {
        setError((e as Error).message);
        return;
      }
    }
    setItems((list) => list.filter((i) => i.key !== item.key));
  }
  if (!session)
    return (
      <div className="composer guest">
        <p>
          {t(
            "Есть идея? Поделитесь с сообществом.",
            "Have an idea? Share it with the community.",
          )}
        </p>
        <button onClick={login} className="primary">
          {t("Войти и написать", "Sign in to post")}
        </button>
      </div>
    );
  async function submit(e: React.FormEvent) {
    e.preventDefault();
    if (!session) return;
    setBusy(true);
    setError("");
    try {
      const path = existing
        ? `/v1/forum/${kind === "article" ? "articles" : kind === "topic" ? "topics" : kind === "post" ? "posts" : "comments"}/${existing.id}`
        : kind === "article"
          ? "/v1/forum/articles"
          : kind === "topic"
            ? "/v1/forum/topics"
            : kind === "post"
              ? `/v1/forum/topics/${parent}/posts`
              : `/v1/forum/articles/${parent}/comments`;
      const result = await api.json<Record<string, ForumContent>>(
        path,
        existing ? "PUT" : "POST",
        {
          body,
          attachment_ids: items.map((i) => i.attachment?.id).filter(Boolean),
          ...(kind === "article"
            ? { title, kind: existing?.kind ?? "post", status: "published" }
            : kind === "topic"
              ? { title, section_id: sectionID }
              : kind === "comment"
                ? { parent_id: parentComment }
                : {}),
        },
        session.csrf_token,
      );
      const id = result[kind]?.id ?? existing?.id ?? "";
      setBody("");
      setTitle("");
      setItems([]);
      onSaved(id);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  }
  return (
    <form className="composer" onSubmit={submit}>
      <div className="composer-label">
        <span className="avatar">{session.user.nickname.slice(0, 1)}</span>
        <strong>
          {existing
            ? t("Редактирование", "Edit")
            : kind === "topic"
              ? t("Новая тема", "New topic")
              : kind === "article"
                ? t("Что нового?", "What’s new?")
                : t("Ваш ответ", "Your reply")}
        </strong>
        {onCancel && (
          <button
            type="button"
            className="icon-button push"
            aria-label={t("Закрыть", "Close")}
            onClick={onCancel}
          >
            <X size={18} />
          </button>
        )}
      </div>
      {kind === "topic" && (
        <>
          <label>
            {t("Раздел", "Section")}
            <select
              value={sectionID}
              onChange={(e) => setSectionID(e.target.value)}
              required
            >
              <option value="">{t("Выберите раздел", "Choose section")}</option>
              {sections?.sections.map((s) => (
                <option key={s.id} value={s.id}>
                  {t(s.title_ru, s.title_en)}
                </option>
              ))}
            </select>
          </label>
          <input
            aria-label={t("Заголовок", "Title")}
            placeholder={t("О чём поговорим?", "What shall we discuss?")}
            value={title}
            onChange={(e) => setTitle(e.target.value)}
            minLength={3}
            maxLength={200}
            required
          />
        </>
      )}
      {kind === "article" && (
        <input
          aria-label={t("Заголовок — необязательно", "Title — optional")}
          placeholder={t("Заголовок — необязательно", "Title — optional")}
          value={title}
          onChange={(e) => setTitle(e.target.value)}
          maxLength={200}
        />
      )}
      <textarea
        aria-label={t("Текст публикации", "Post text")}
        placeholder={t(
          "Расскажите, над чем работаете…",
          "Tell us what you’re working on…",
        )}
        value={body}
        onChange={(e) => setBody(e.target.value)}
        rows={kind === "article" ? 3 : 5}
        maxLength={kind === "article" ? 200000 : 50000}
        required={!items.length && !attached.length}
      />
      {items.map((item) => (
        <div className="upload-row" key={item.key}>
          <span>
            {item.file.name}
            <small>{item.error ?? `${Math.round(item.progress)}%`}</small>
            <progress value={item.progress} max={100} />
          </span>
          {item.error && (
            <button
              type="button"
              onClick={() => {
                const next = { ...item, controller: new AbortController() };
                patch(item.key, { controller: next.controller });
                void start(next);
              }}
            >
              {t("Повторить", "Retry")}
            </button>
          )}
          <button
            type="button"
            aria-label={t("Удалить вложение", "Remove attachment")}
            onClick={() => void remove(item)}
          >
            <X size={16} />
          </button>
        </div>
      ))}
      {attached.map((a) => (
        <div className="upload-row" key={a.id}>
          <span>
            {a.name}
            <small>
              {a.media_status === "rejected"
                ? t(
                    "Видео длиннее 5 минут или повреждено. Удалите и загрузите замену.",
                    "Video exceeds 5 minutes or is unreadable. Remove it and upload a replacement.",
                  )
                : t("Прикреплённый файл", "Attached file")}
            </small>
          </span>
          <button
            type="button"
            aria-label={t("Удалить вложение", "Remove attachment")}
            onClick={async () => {
              if (
                !confirm(
                  t(
                    "Удалить файл из публикации?",
                    "Remove this file from the post?",
                  ),
                )
              )
                return;
              try {
                await api.json(
                  `/v1/forum/attachments/${a.id}`,
                  "DELETE",
                  {},
                  session.csrf_token,
                );
                setAttached((list) => list.filter((v) => v.id !== a.id));
              } catch (e) {
                setError((e as Error).message);
              }
            }}
          >
            <X size={16} />
          </button>
        </div>
      ))}
      <div className="composer-toolbar">
        <label className="attach-button">
          <Paperclip size={17} />
          {t("Добавить файлы", "Attach files")}
          <input
            type="file"
            multiple
            accept=".png,.jpg,.jpeg,.webp,.gif,.mp4,.mov,.webm,.vltmini,.vltnode,.vltcreator,.vlts,.vltmorph,.vltslicer,.vlttheme,.json,.vlt,.vltt,.zip"
            onChange={(e) => {
              add(e.target.files);
              e.target.value = "";
            }}
            disabled={!limits || busy}
          />
        </label>
        <span className="hint">Markdown</span>
        <button
          className="primary push"
          disabled={busy || items.some((i) => !i.attachment)}
        >
          <Send size={15} />
          {busy ? t("Сохраняем…", "Saving…") : t("Опубликовать", "Publish")}
        </button>
      </div>
      <details className="upload-options">
        <summary>{t("Настройки файлов VLT", "VLT file options")}</summary>
        <label>
          {t("Установка новых пресетов VLTS", "Install new VLTS presets in")}
          <select
            value={presetDestination}
            onChange={(e) => setPresetDestination(e.target.value)}
          >
            <option value="strip">Channel Strip</option>
            <option value="offline-strip">Offline Processing</option>
          </select>
        </label>
        <label>
          {t(
            "Минимальная версия VLTONE — необязательно",
            "Minimum VLTONE version — optional",
          )}
          <input
            placeholder="0.5.1"
            value={minimumVersion}
            pattern="[0-9]+(\.[0-9]+){0,2}"
            maxLength={40}
            onChange={(e) => setMinimumVersion(e.target.value)}
          />
        </label>
        <small>
          {t(
            "Применяется к файлам, добавленным после изменения настроек.",
            "Applies to files added after changing these settings.",
          )}
        </small>
      </details>
      <p className="hint">
        {t(
          "Видео: до 5 минут и 250 МБ. Файлы VLT: до 1 ГБ.",
          "Video: up to 5 minutes and 250 MB. VLT files: up to 1 GB.",
        )}
      </p>
      {error && (
        <p className="error" role="alert">
          {error}
        </p>
      )}
    </form>
  );
}
