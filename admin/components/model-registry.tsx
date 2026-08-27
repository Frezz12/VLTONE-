"use client";

import type { APIError } from "@vlt/api-client";
import { api } from "@vlt/api-client";
import { Bot, Eye, EyeOff, Plus, Save, Trash2 } from "lucide-react";
import { useEffect, useState } from "react";
import { AdminShell } from "./admin-shell";
import { useAdmin } from "./use-admin";

type Provider = "openai" | "anthropic";
type AIModel = {
  id: string;
  display_name: string;
  provider: Provider;
  model: string;
  endpoint_url: string;
  has_api_key: boolean;
  enabled: boolean;
  sort_order: number;
};

type Draft = Omit<AIModel, "id" | "has_api_key"> & { id?: string; api_key: string; has_api_key?: boolean };

const emptyDraft = (): Draft => ({
  display_name: "", provider: "openai", model: "",
  endpoint_url: "https://api.openai.com/v1",
  api_key: "", enabled: true, sort_order: 0,
});

export function ModelRegistry() {
  const { session, error } = useAdmin();
  const [models, setModels] = useState<AIModel[]>();
  const [draft, setDraft] = useState<Draft>(emptyDraft);
  const [busy, setBusy] = useState(false);
  const [status, setStatus] = useState("");
  const [fieldErrors, setFieldErrors] = useState<Record<string, string>>({});
  const [showKey, setShowKey] = useState(false);

  async function load(preferred?: string) {
    const result = await api.request<{ models: AIModel[] }>("/v1/admin/ai/models");
    setModels(result.models);
    const selected = result.models.find((item) => item.id === preferred);
    if (selected) setDraft({ ...selected, api_key: "" });
  }

  useEffect(() => { if (session) void load(); }, [session]);

  function choose(model: AIModel) {
    setDraft({ ...model, api_key: "" });
    setStatus("");
    setFieldErrors({});
    setShowKey(false);
  }

  function startNew() {
    setDraft(emptyDraft());
    setStatus("");
    setFieldErrors({});
    setShowKey(false);
  }

  function changeProvider(provider: Provider) {
    const defaultEndpoint = provider === "openai"
      ? "https://api.openai.com/v1"
      : "https://api.anthropic.com/v1";
    setDraft({ ...draft, provider, endpoint_url: draft.id ? draft.endpoint_url : defaultEndpoint });
  }

  async function save() {
    if (!session) return;
    setBusy(true);
    setStatus("");
    setFieldErrors({});
    try {
      const payload = {
        display_name: draft.display_name, provider: draft.provider, model: draft.model,
        endpoint_url: draft.endpoint_url, api_key: draft.api_key,
        enabled: draft.enabled, sort_order: Number(draft.sort_order) || 0,
      };
      const saved = draft.id
        ? await api.json<AIModel>(`/v1/admin/ai/models/${draft.id}`, "PUT", payload, session.csrf_token)
        : await api.json<AIModel>("/v1/admin/ai/models", "POST", payload, session.csrf_token);
      await load(saved.id);
      setStatus("Модель сохранена и доступна программе.");
    } catch (reason) {
      const failure = reason as APIError;
      setStatus(failure.message);
      setFieldErrors(failure.field_errors ?? {});
    } finally {
      setBusy(false);
    }
  }

  async function remove() {
    if (!session || !draft.id) return;
    if (!window.confirm(`Удалить модель «${draft.display_name}»?`)) return;
    setBusy(true);
    setStatus("");
    try {
      await api.request(`/v1/admin/ai/models/${draft.id}`, {
        method: "DELETE", headers: { "X-CSRF-Token": session.csrf_token },
      });
      startNew();
      await load();
      setStatus("Модель удалена.");
    } catch (reason) {
      setStatus((reason as APIError).message);
    } finally {
      setBusy(false);
    }
  }

  const fieldError = (name: string) => fieldErrors[name]
    ? <span className="model-field-error" role="alert">{fieldErrors[name]}</span>
    : null;

  return (
    <AdminShell>
      <div className="admin-page-head">
        <div>
          <h1 className="vlt-title">Модели AI</h1>
          <p className="vlt-subtitle">Основные модели, которые пользователи видят в чате только по заданному названию.</p>
        </div>
        <button className="vlt-button" onClick={startNew} disabled={busy}><Plus size={16} aria-hidden /> Новая модель</button>
      </div>
      {error && <div className="vlt-error">{error}</div>}
      <div className="model-registry-grid">
        <section className="vlt-card vlt-card-pad" aria-label="Список моделей">
          <h2 className="vlt-section-title">Основные модели</h2>
          <p className="vlt-subtitle model-registry-help">Порядок задаётся полем «Позиция».</p>
          <div className="model-registry-list">
            {!models && <p className="vlt-muted" role="status">Загрузка моделей…</p>}
            {models?.length === 0 && <p className="vlt-muted">Моделей пока нет. Добавьте первую.</p>}
            {models?.map((model) => (
              <button
                key={model.id}
                className={`model-registry-item ${draft.id === model.id ? "selected" : ""}`}
                onClick={() => choose(model)}
                aria-pressed={draft.id === model.id}
              >
                <Bot size={18} aria-hidden />
                <span><strong>{model.display_name}</strong><small>{model.model}</small></span>
                <span className="vlt-badge"><span className={`status-dot ${model.enabled ? "" : "off"}`} />{model.enabled ? "вкл." : "выкл."}</span>
              </button>
            ))}
          </div>
        </section>

        <section className="vlt-card vlt-card-pad">
          <div className="vlt-row vlt-between">
            <h2 className="vlt-section-title">{draft.id ? "Настройки модели" : "Новая модель"}</h2>
            {draft.has_api_key && <span className="vlt-badge">ключ сохранён</span>}
          </div>
          <div className="model-form">
            <label className="vlt-label">Название в программе
              <input className="vlt-input" value={draft.display_name} autoComplete="off" onChange={(event) => setDraft({ ...draft, display_name: event.target.value })} />
              {fieldError("display_name")}
            </label>
            <label className="vlt-label">Тип подключения
              <select className="vlt-input" value={draft.provider} onChange={(event) => changeProvider(event.target.value as Provider)}>
                <option value="openai">GPT / OpenAI-compatible</option>
                <option value="anthropic">Claude / Anthropic-compatible</option>
              </select>
              {fieldError("provider")}
            </label>
            <label className="vlt-label">ID модели у провайдера
              <input className="vlt-input vlt-code" value={draft.model} placeholder={draft.provider === "openai" ? "gpt-4.1" : "claude-sonnet-4-5"} autoComplete="off" onChange={(event) => setDraft({ ...draft, model: event.target.value })} />
              {fieldError("model")}
            </label>
            <label className="vlt-label">Base URL или URL запроса
              <input className="vlt-input vlt-code" type="url" value={draft.endpoint_url} autoComplete="url" onChange={(event) => setDraft({ ...draft, endpoint_url: event.target.value })} />
              <span className="vlt-muted">Например, https://anymodel.org/v1. Путь чата добавится автоматически.</span>
              {fieldError("endpoint_url")}
            </label>
            <div className="vlt-label"><label htmlFor="model-api-key">API-ключ</label>
              <span className="secret-input-row">
                <input id="model-api-key" className="vlt-input vlt-code" type={showKey ? "text" : "password"} value={draft.api_key} autoComplete="new-password" placeholder={draft.has_api_key ? "Оставьте пустым, чтобы сохранить текущий" : "Введите ключ провайдера"} onChange={(event) => setDraft({ ...draft, api_key: event.target.value })} />
                <button type="button" className="vlt-button vlt-button-secondary" onClick={() => setShowKey((shown) => !shown)} aria-label={showKey ? "Скрыть API-ключ" : "Показать API-ключ"}>{showKey ? <EyeOff size={17} aria-hidden /> : <Eye size={17} aria-hidden />}</button>
              </span>
              {fieldError("api_key")}
            </div>
            <label className="vlt-label">Позиция
              <input className="vlt-input" type="number" value={draft.sort_order} onChange={(event) => setDraft({ ...draft, sort_order: Number(event.target.value) })} />
            </label>
            <label className="vlt-checkbox"><input type="checkbox" checked={draft.enabled} onChange={(event) => setDraft({ ...draft, enabled: event.target.checked })} />Показывать модель пользователям</label>
          </div>
          <div className="vlt-row model-form-actions">
            <button className="vlt-button" onClick={() => void save()} disabled={busy}><Save size={16} aria-hidden /> {busy ? "Сохранение…" : "Сохранить"}</button>
            {draft.id && <button className="vlt-button vlt-button-danger" onClick={() => void remove()} disabled={busy}><Trash2 size={16} aria-hidden /> Удалить</button>}
          </div>
          {status && <p className="model-form-status" role="status">{status}</p>}
        </section>
      </div>
    </AdminShell>
  );
}
