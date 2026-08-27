"use client";
import type { APIError } from "@vlt/api-client";
import { api } from "@vlt/api-client";
import { History, RotateCcw, Save, Trash2 } from "lucide-react";
import { useEffect, useState } from "react";
import { AdminShell } from "./admin-shell";
import { useAdmin } from "./use-admin";

// The assistant's instructions. The main prompt tells it how to operate the
// program; a playbook tells it how to do one kind of work — write a bass part,
// program a beat, process a vocal. The desktop fetches all of it and falls back
// to the copy compiled into the app, so an edit here reaches users without a
// release, and a bad edit is one "Вернуть встроенный" away from being undone.

type Prompt = {
  id: string;
  kind: "main" | "playbook";
  title: string;
  use_when: string;
  tags: string[];
  body: string;
  enabled: boolean;
  updated_at?: string;
  builtin: boolean;
};

type Revision = { id: string; body: string; created_at: string };

export function PromptRegistry() {
  const { session, error } = useAdmin();
  const [prompts, setPrompts] = useState<Prompt[]>([]);
  const [version, setVersion] = useState("");
  const [selected, setSelected] = useState<string>("");
  const [draft, setDraft] = useState<Prompt>();
  const [revisions, setRevisions] = useState<Revision[]>();
  const [status, setStatus] = useState("");
  const [busy, setBusy] = useState(false);

  async function load(keep = selected) {
    const result = await api.request<{ version: string; prompts: Prompt[] }>("/v1/admin/ai/prompts");
    setPrompts(result.prompts);
    setVersion(result.version);
    const next = result.prompts.find((prompt) => prompt.id === keep) ?? result.prompts[0];
    if (next) { setSelected(next.id); setDraft({ ...next }); }
  }

  useEffect(() => { if (session) void load(); }, [session]);

  function choose(prompt: Prompt) {
    setSelected(prompt.id);
    setDraft({ ...prompt });
    setRevisions(undefined);
    setStatus("");
  }

  async function run(work: () => Promise<void>, done: string) {
    if (!session) return;
    setBusy(true);
    setStatus("");
    try {
      await work();
      setStatus(done);
    } catch (reason) {
      setStatus((reason as APIError).message);
    } finally {
      setBusy(false);
    }
  }

  function save() {
    if (!draft) return;
    void run(async () => {
      await api.json(`/v1/admin/ai/prompts/${draft.id}`, "PUT", {
        title: draft.title, use_when: draft.use_when, tags: draft.tags, body: draft.body,
        enabled: draft.enabled,
      }, session?.csrf_token);
      await load(draft.id);
    }, "Сохранено. Программа подтянет новый текст при следующем обновлении.");
  }

  function revert() {
    if (!draft) return;
    void run(async () => {
      await api.json(`/v1/admin/ai/prompts/${draft.id}/revert`, "POST", {}, session?.csrf_token);
      await load(draft.id);
    }, "Возвращён встроенный текст.");
  }

  function remove() {
    if (!draft || draft.kind === "main") return;
    if (!window.confirm(`Удалить «${draft.id}»? Ассистент перестанет его загружать.`)) return;
    void run(async () => {
      await api.request(`/v1/admin/ai/prompts/${draft.id}`, {
        method: "DELETE", headers: { "X-CSRF-Token": session?.csrf_token ?? "" },
      });
      await load("main");
    }, "Удалено.");
  }

  function create() {
    const id = window.prompt("Идентификатор нового плейбука — строчные буквы, цифры и дефис:");
    if (!id) return;
    void run(async () => {
      await api.json("/v1/admin/ai/prompts", "POST", {
        id, title: id, use_when: "", tags: [],
        body: "Опишите здесь, как делать эту работу.\n",
      }, session?.csrf_token);
      await load(id);
    }, "Плейбук создан.");
  }

  function history() {
    if (!draft) return;
    void run(async () => {
      const result = await api.request<{ revisions: Revision[] }>(`/v1/admin/ai/prompts/${draft.id}/revisions`);
      setRevisions(result.revisions);
    }, "");
  }

  return (
    <AdminShell>
      <div className="admin-page-head">
        <div>
          <h1 className="vlt-title">Промпты ассистента</h1>
          <p className="vlt-subtitle">
            Основной промпт и плейбуки. Версия: <span className="vlt-code">{version || "—"}</span>
          </p>
        </div>
        <button className="vlt-button" onClick={create} disabled={busy}>Новый плейбук</button>
      </div>
      {error && <div className="vlt-error">{error}</div>}
      <div className="detail-grid">
        <div className="vlt-card vlt-card-pad">
          {draft ? (
            <>
              <div className="vlt-row" style={{ justifyContent: "space-between", marginBottom: 12 }}>
                <strong className="vlt-code">{draft.id}</strong>
                <span className="vlt-badge">
                  <span className={`status-dot ${draft.builtin ? "" : "off"}`} />
                  {draft.builtin ? "как в сборке" : "изменён"}
                </span>
              </div>
              <label className="sr-only" htmlFor="prompt-title">Название</label>
              <input
                id="prompt-title" className="vlt-input" value={draft.title} placeholder="Название"
                onChange={(event) => setDraft({ ...draft, title: event.target.value })}
              />
              <label className="sr-only" htmlFor="prompt-use-when">Когда применять</label>
              <input
                id="prompt-use-when" className="vlt-input" value={draft.use_when}
                placeholder="Когда применять — эту строку модель читает в индексе"
                style={{ marginTop: 8 }}
                onChange={(event) => setDraft({ ...draft, use_when: event.target.value })}
              />
              <label className="sr-only" htmlFor="prompt-tags">Теги</label>
              <input
                id="prompt-tags" className="vlt-input" value={draft.tags.join(", ")}
                placeholder="Теги через запятую" style={{ marginTop: 8 }}
                onChange={(event) => setDraft({ ...draft, tags: event.target.value.split(",").map((tag) => tag.trim()).filter(Boolean) })}
              />
              <label className="sr-only" htmlFor="prompt-body">Текст промпта</label>
              <textarea
                id="prompt-body" className="vlt-input" value={draft.body} spellCheck={false}
                style={{ marginTop: 8, minHeight: 460, fontFamily: "var(--vlt-font-mono, monospace)" }}
                onChange={(event) => setDraft({ ...draft, body: event.target.value })}
              />
              <div className="vlt-row" style={{ marginTop: 12, gap: 8 }}>
                <button className="vlt-button" onClick={save} disabled={busy}><Save size={15} /> Сохранить</button>
                <button className="vlt-button vlt-button-secondary" onClick={revert} disabled={busy}><RotateCcw size={15} /> Вернуть встроенный</button>
                <button className="vlt-button vlt-button-secondary" onClick={history} disabled={busy}><History size={15} /> История</button>
                {draft.kind !== "main" && (
                  <button className="vlt-button vlt-button-secondary danger-zone" onClick={remove} disabled={busy}><Trash2 size={15} /> Удалить</button>
                )}
              </div>
              {status && <p className="vlt-subtitle" style={{ marginTop: 10 }}>{status}</p>}
              {revisions && (
                <div style={{ marginTop: 14 }}>
                  <h2 className="vlt-title" style={{ fontSize: 15 }}>История</h2>
                  {revisions.length === 0 && <p className="vlt-subtitle">Правок ещё не было.</p>}
                  {revisions.map((revision) => (
                    <details className="crash-context" key={revision.id}>
                      <summary>{new Date(revision.created_at).toLocaleString("ru")}</summary>
                      <pre className="vlt-code" style={{ whiteSpace: "pre-wrap" }}>{revision.body}</pre>
                    </details>
                  ))}
                </div>
              )}
            </>
          ) : (
            <p className="vlt-subtitle">Загрузка…</p>
          )}
        </div>
        <div className="vlt-card vlt-card-pad">
          <h2 className="vlt-title" style={{ fontSize: 15, marginBottom: 10 }}>Документы</h2>
          <div className="admin-nav">
            {prompts.map((prompt) => (
              <button
                key={prompt.id}
                className="admin-notification-button"
                style={{ color: prompt.id === selected ? "var(--vlt-text)" : undefined }}
                onClick={() => choose(prompt)}
              >
                <span className={`status-dot ${prompt.enabled ? "" : "off"}`} />
                <span style={{ flex: 1 }}>{prompt.kind === "main" ? "Основной промпт" : prompt.title || prompt.id}</span>
                {!prompt.builtin && <span className="vlt-badge">ред.</span>}
              </button>
            ))}
          </div>
        </div>
      </div>
    </AdminShell>
  );
}
