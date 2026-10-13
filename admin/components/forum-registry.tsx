"use client";
import { api, type ForumSection } from "@vlt/api-client";
import { useEffect, useState } from "react";
import { AdminShell } from "./admin-shell";
import { useAdmin } from "./use-admin";
import { canAdmin } from "./admin-permissions";

type Content = {
  id: string;
  title?: string;
  body: string;
  status: string;
  pinned?: boolean;
  locked?: boolean;
  created_at: string;
};
type Report = {
  id: string;
  target_type: string;
  target_id: string;
  reason: string;
  status: string;
  created_at: string;
};
type Result = {
  reports: Report[];
  topics: Content[];
  articles: Content[];
  posts: Content[];
  comments: Content[];
};
const blank = {
  slug: "",
  title_ru: "",
  title_en: "",
  description_ru: "",
  description_en: "",
  sort_order: 0,
};
export function ForumRegistry() {
  const { session, error } = useAdmin();
  const canWrite = canAdmin(session?.admin, "forum.write");
  const [data, setData] = useState<Result>();
  const [sections, setSections] = useState<ForumSection[]>([]);
  const [draft, setDraft] = useState<Partial<ForumSection>>(blank);
  const [offset, setOffset] = useState(0);
  const [status, setStatus] = useState("");
  const [busy, setBusy] = useState(false);
  const [tab, setTab] = useState<keyof Result | "sections">("reports");
  const [inspected, setInspected] = useState<Content>();
  async function load() {
    const [result, list] = await Promise.all([
      api.request<Result>(`/v1/admin/forum?limit=30&offset=${offset}`),
      api.request<{ sections: ForumSection[] }>("/v1/forum/sections"),
    ]);
    setData(result);
    setSections(list.sections);
  }
  useEffect(() => {
    if (session) load().catch((e) => setStatus(e.message));
  }, [session, offset]);
  async function run(path: string, body: unknown) {
    if (!canWrite || busy) return;
    setBusy(true);
    setStatus("");
    try {
      await api.json(path, "PUT", body, session?.csrf_token);
      await load();
      setStatus("Сохранено в журнале аудита.");
    } catch (e) {
      setStatus((e as Error).message);
    } finally {
      setBusy(false);
    }
  }
  const moderate = (kind: string, id: string, body: unknown) =>
    run(`/v1/admin/forum/content/${kind}/${id}`, body);
  return (
    <AdminShell>
      <div className="admin-page-head">
        <div>
          <h1 className="vlt-title">Форум</h1>
          <p className="vlt-subtitle">
            Жалобы, опубликованные материалы и разделы сообщества.
          </p>
        </div>
      </div>
      {(error || status) && (
        <p role="status" className="vlt-card vlt-card-pad">
          {error || status}
        </p>
      )}
      <div className="vlt-row" style={{ flexWrap: "wrap", marginBottom: 20 }}>
        {(
          [
            ["reports", "Жалобы"],
            ["topics", "Темы"],
            ["articles", "Публикации"],
            ["posts", "Ответы"],
            ["comments", "Комментарии"],
            ["sections", "Разделы"],
          ] as const
        ).map(([key, label]) => (
          <button
            key={key}
            className={`vlt-button ${tab === key ? "" : "vlt-button-secondary"}`}
            onClick={() => {
              setTab(key);
              setOffset(0);
              setInspected(undefined);
            }}
          >
            {label}
          </button>
        ))}
      </div>
      {tab === "sections" ? (
        <div className="detail-grid">
          <div className="vlt-card vlt-card-pad">
            <button
              className="vlt-button"
              disabled={!canWrite}
              onClick={() => setDraft(blank)}
            >
              Новый раздел
            </button>
            {sections.map((s) => (
              <p key={s.id}>
                <button
                  className="vlt-button vlt-button-secondary"
                  onClick={() => setDraft(s)}
                >
                  {s.title_ru} · {s.slug}
                </button>
              </p>
            ))}
          </div>
          <form
            className="vlt-card vlt-card-pad vlt-stack"
            onSubmit={(e) => {
              e.preventDefault();
              void run("/v1/admin/forum/sections", {
                id: draft.id,
                slug: draft.slug,
                title_ru: draft.title_ru,
                title_en: draft.title_en,
                description_ru: draft.description_ru,
                description_en: draft.description_en,
                sort_order: draft.sort_order,
              });
            }}
          >
            {(
              [
                ["slug", "Адрес раздела"],
                ["title_ru", "Название (RU)"],
                ["title_en", "Название (EN)"],
                ["description_ru", "Описание (RU)"],
                ["description_en", "Описание (EN)"],
              ] as const
            ).map(([key, label]) => (
              <label key={key} className="vlt-label">
                {label}
                <input
                  className="vlt-input"
                  value={draft[key] || ""}
                  readOnly={!canWrite}
                  required={!key.startsWith("description")}
                  maxLength={key.startsWith("description") ? 2000 : 200}
                  onChange={(e) =>
                    setDraft({ ...draft, [key]: e.target.value })
                  }
                />
              </label>
            ))}
            <label className="vlt-label">
              Порядок
              <input
                type="number"
                className="vlt-input"
                value={draft.sort_order || 0}
                onChange={(e) =>
                  setDraft({ ...draft, sort_order: Number(e.target.value) })
                }
              />
            </label>
            <button className="vlt-button" disabled={busy || !canWrite}>
              Сохранить раздел
            </button>
          </form>
        </div>
      ) : !data ? (
        <p role="status">Загрузка…</p>
      ) : (
        <div className="vlt-stack">
          {tab === "reports"
            ? data.reports.map((r) => (
                <article key={r.id} className="vlt-card vlt-card-pad">
                  <div className="vlt-row">
                    <strong>{r.reason}</strong>
                    <span className="vlt-badge">{r.status}</span>
                  </div>
                  <p>
                    {r.target_type} · {r.target_id} ·{" "}
                    {new Date(r.created_at).toLocaleString("ru")}
                  </p>
                  <div className="vlt-row">
                    <button
                      className="vlt-button vlt-button-secondary"
                      onClick={() =>
                        api
                          .request<{ content: Content }>(
                            `/v1/admin/forum/content/${r.target_type}/${r.target_id}`,
                          )
                          .then((v) => setInspected(v.content))
                          .catch((e) => setStatus(e.message))
                      }
                    >
                      Прочитать
                    </button>
                    <button
                      className="vlt-button vlt-button-secondary"
                      disabled={!canWrite || busy}
                      onClick={() =>
                        void moderate(r.target_type, r.target_id, {
                          status: "hidden",
                        })
                      }
                    >
                      Скрыть
                    </button>
                    <button
                      className="vlt-button vlt-button-secondary"
                      disabled={!canWrite || busy}
                      onClick={() =>
                        void moderate(r.target_type, r.target_id, {
                          status: "published",
                        })
                      }
                    >
                      Восстановить
                    </button>
                    <button
                      className="vlt-button"
                      disabled={!canWrite || busy || r.status === "resolved"}
                      onClick={() =>
                        void run(`/v1/admin/forum/reports/${r.id}`, {})
                      }
                    >
                      Закрыть жалобу
                    </button>
                  </div>
                </article>
              ))
            : data[tab].map((c) => (
                <article className="vlt-card vlt-card-pad" key={c.id}>
                  <h2>{c.title || c.body.slice(0, 100)}</h2>
                  <p className="vlt-subtitle">
                    {c.status} · {new Date(c.created_at).toLocaleString("ru")}
                  </p>
                  <details>
                    <summary>Текст материала</summary>
                    <p
                      style={{
                        whiteSpace: "pre-wrap",
                        overflowWrap: "anywhere",
                      }}
                    >
                      {c.body}
                    </p>
                  </details>
                  <div className="vlt-row" style={{ marginTop: 12 }}>
                    <button
                      className="vlt-button vlt-button-secondary"
                      disabled={!canWrite || busy}
                      onClick={() =>
                        void moderate(
                          {
                            topics: "topic",
                            articles: "article",
                            posts: "post",
                            comments: "comment",
                          }[tab],
                          c.id,
                          {
                            status:
                              c.status === "hidden" ? "published" : "hidden",
                          },
                        )
                      }
                    >
                      {c.status === "hidden" ? "Восстановить" : "Скрыть"}
                    </button>
                    {tab === "topics" && (
                      <>
                        <button
                          className="vlt-button vlt-button-secondary"
                          disabled={!canWrite || busy}
                          onClick={() =>
                            void moderate("topic", c.id, { pinned: !c.pinned })
                          }
                        >
                          {c.pinned ? "Открепить" : "Закрепить"}
                        </button>
                        <button
                          className="vlt-button vlt-button-secondary"
                          disabled={!canWrite || busy}
                          onClick={() =>
                            void moderate("topic", c.id, { locked: !c.locked })
                          }
                        >
                          {c.locked ? "Открыть тему" : "Закрыть тему"}
                        </button>
                      </>
                    )}
                  </div>
                </article>
              ))}
          {data[tab].length === 0 && <p>Материалов пока нет.</p>}
          {inspected && (
            <section className="vlt-card vlt-card-pad">
              <h2>{inspected.title || "Содержимое жалобы"}</h2>
              <p style={{ whiteSpace: "pre-wrap", overflowWrap: "anywhere" }}>
                {inspected.body}
              </p>
              <button
                className="vlt-button vlt-button-secondary"
                onClick={() => setInspected(undefined)}
              >
                Закрыть просмотр
              </button>
            </section>
          )}
          <nav className="vlt-row" aria-label="Страницы">
            <button
              className="vlt-button vlt-button-secondary"
              disabled={offset === 0}
              onClick={() => setOffset(Math.max(0, offset - 30))}
            >
              Назад
            </button>
            <span>
              {offset + 1}–{offset + data[tab].length}
            </span>
            <button
              className="vlt-button vlt-button-secondary"
              disabled={data[tab].length < 30}
              onClick={() => setOffset(offset + 30)}
            >
              Далее
            </button>
          </nav>
        </div>
      )}
    </AdminShell>
  );
}
