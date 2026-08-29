"use client";

import type { APIError } from "@vlt/api-client";
import { api } from "@vlt/api-client";
import { FileArchive, ImagePlus, PackageOpen, Plus, Rocket, Save, Trash2, Upload } from "lucide-react";
import { useEffect, useRef, useState } from "react";
import { AdminShell } from "./admin-shell";
import { useAdmin } from "./use-admin";

type ArtifactKind = "windows-exe" | "macos-dmg" | "linux-appimage" | "linux-deb" | "linux-rpm" | "linux-tar-gz" | "linux-tar-xz";
type Artifact = { id: string; kind: ArtifactKind; platform: string; label: string; file_name: string; bytes: number; sha256: string; download_url: string; updated_at: string };
type Screenshot = { id: string; caption_ru: string; caption_en: string; sort_order: number; width: number; height: number; sha256: string; url: string };
type Release = {
  id: string; version: string; status: "draft" | "published"; summary_ru: string; summary_en: string;
  features_ru: string[]; features_en: string[]; changes_ru: string[]; changes_en: string[]; fixes_ru: string[]; fixes_en: string[];
  artifacts: Artifact[]; screenshots: Screenshot[]; published_at?: string | null; created_at: string; updated_at: string;
};
type ListField = "features_ru" | "features_en" | "changes_ru" | "changes_en" | "fixes_ru" | "fixes_en";

const artifactDefinitions: Array<{ kind: ArtifactKind; title: string; help: string; accept: string }> = [
  { kind: "windows-exe", title: "Windows Setup", help: "Один установщик EXE", accept: ".exe" },
  { kind: "macos-dmg", title: "macOS DMG", help: "Один образ DMG", accept: ".dmg" },
  { kind: "linux-appimage", title: "Linux AppImage", help: "Переносимый AppImage", accept: ".AppImage,.appimage" },
  { kind: "linux-deb", title: "Linux DEB", help: "Пакет Debian/Ubuntu", accept: ".deb" },
  { kind: "linux-rpm", title: "Linux RPM", help: "Пакет Fedora/RHEL", accept: ".rpm" },
  { kind: "linux-tar-gz", title: "Linux TAR.GZ", help: "Архив tar.gz", accept: ".tar.gz" },
  { kind: "linux-tar-xz", title: "Linux TAR.XZ", help: "Архив tar.xz", accept: ".tar.xz" },
];

const emptyRelease = (): Release => ({
  id: "", version: "", status: "draft", summary_ru: "", summary_en: "",
  features_ru: [], features_en: [], changes_ru: [], changes_en: [], fixes_ru: [], fixes_en: [],
  artifacts: [], screenshots: [], created_at: "", updated_at: "",
});

function lines(value: string) { return value.split("\n").map((item) => item.trim()).filter(Boolean); }
function readableBytes(value: number) {
  if (value >= 1024 ** 3) return `${(value / 1024 ** 3).toFixed(2)} ГиБ`;
  if (value >= 1024 ** 2) return `${(value / 1024 ** 2).toFixed(1)} МиБ`;
  return `${Math.max(1, Math.round(value / 1024))} КиБ`;
}

function uploadWithProgress(url: string, method: "PUT" | "POST", body: FormData, csrf: string, progress: (value: number) => void) {
  return new Promise<unknown>((resolve, reject) => {
    const request = new XMLHttpRequest();
    request.open(method, url);
    request.withCredentials = true;
    request.setRequestHeader("X-CSRF-Token", csrf);
    request.upload.onprogress = (event) => { if (event.lengthComputable) progress(Math.round(event.loaded * 100 / event.total)); };
    request.onerror = () => reject({ code: "upload_failed", message: "Соединение прервано во время загрузки." } satisfies APIError);
    request.onload = () => {
      const fallback = { code: "upload_failed", message: `Загрузка завершилась с кодом ${request.status}.` };
      const response = (() => { try { return JSON.parse(request.responseText || "null") ?? fallback; } catch { return fallback; } })();
      if (request.status >= 200 && request.status < 300) resolve(response); else reject(response);
    };
    request.send(body);
  });
}

export function ReleaseRegistry() {
  const { session, error } = useAdmin();
  const [releases, setReleases] = useState<Release[]>();
  const [draft, setDraft] = useState<Release>(emptyRelease);
  const [busy, setBusy] = useState(false);
  const [status, setStatus] = useState("");
  const [fieldErrors, setFieldErrors] = useState<Record<string, string>>({});
  const [uploadProgress, setUploadProgress] = useState<Record<string, number>>({});
  const [shotFile, setShotFile] = useState<File>();
  const [shotRU, setShotRU] = useState("");
  const [shotEN, setShotEN] = useState("");
  const errorSummary = useRef<HTMLDivElement>(null);

  async function load(preferred?: string) {
    const result = await api.request<{ releases: Release[] }>("/v1/admin/releases");
    setReleases(result.releases);
    if (preferred) {
      const selected = result.releases.find((item) => item.id === preferred);
      if (selected) setDraft(selected);
    }
  }
  useEffect(() => { if (session) void load(); }, [session]);
  useEffect(() => {
    if (Object.keys(fieldErrors).length > 0) errorSummary.current?.focus();
  }, [fieldErrors]);

  function choose(item: Release) { setDraft(item); setStatus(""); setFieldErrors({}); }
  function startNew() { setDraft(emptyRelease()); setStatus(""); setFieldErrors({}); }
  function setList(field: ListField, value: string) { setDraft({ ...draft, [field]: lines(value) }); }
  function fail(reason: unknown) {
    const failure = reason as APIError;
    setStatus(failure.message || "Операция не выполнена.");
    setFieldErrors(failure.field_errors ?? {});
  }

  function payload() {
    return {
      version: draft.version, summary_ru: draft.summary_ru, summary_en: draft.summary_en,
      features_ru: draft.features_ru, features_en: draft.features_en, changes_ru: draft.changes_ru,
      changes_en: draft.changes_en, fixes_ru: draft.fixes_ru, fixes_en: draft.fixes_en,
    };
  }

  async function save(showStatus = true): Promise<Release | undefined> {
    if (!session) return;
    setBusy(true); setStatus(""); setFieldErrors({});
    try {
      const saved = draft.id
        ? await api.json<Release>(`/v1/admin/releases/${draft.id}`, "PUT", payload(), session.csrf_token)
        : await api.json<Release>("/v1/admin/releases", "POST", payload(), session.csrf_token);
      setDraft(saved);
      await load(saved.id);
      if (showStatus) setStatus("Черновик сохранён.");
      return saved;
    } catch (reason) { fail(reason); return undefined; }
    finally { setBusy(false); }
  }

  async function publish() {
    if (!session) return;
    const saved = await save(false);
    if (!saved) return;
    setBusy(true); setStatus(""); setFieldErrors({});
    try {
      const published = await api.json<Release>(`/v1/admin/releases/${saved.id}/publish`, "POST", {}, session.csrf_token);
      setDraft(published); await load(published.id); setStatus(`Версия ${published.version} опубликована.`);
    } catch (reason) { fail(reason); }
    finally { setBusy(false); }
  }

  async function removeDraft() {
    if (!session || !draft.id || draft.status !== "draft") return;
    if (!window.confirm(`Удалить черновик ${draft.version || "без номера"} и все его файлы?`)) return;
    setBusy(true);
    try {
      await api.request(`/v1/admin/releases/${draft.id}`, { method: "DELETE", headers: { "X-CSRF-Token": session.csrf_token } });
      startNew(); await load(); setStatus("Черновик удалён.");
    } catch (reason) { fail(reason); }
    finally { setBusy(false); }
  }

  async function uploadArtifact(kind: ArtifactKind, file?: File) {
    if (!session || !draft.id || !file) return;
    const existing = draft.artifacts.find((item) => item.kind === kind);
    if (existing && !window.confirm(`Заменить ${existing.file_name} файлом ${file.name}?`)) return;
    setStatus(""); setFieldErrors({}); setUploadProgress((value) => ({ ...value, [kind]: 0 }));
    const body = new FormData(); body.append("file", file);
    try {
      await uploadWithProgress(`/release-upload/v1/admin/releases/${draft.id}/artifacts/${kind}`, "PUT", body, session.csrf_token,
        (value) => setUploadProgress((current) => ({ ...current, [kind]: value })));
      await load(draft.id); setStatus(`${file.name} загружен.`);
    } catch (reason) { fail(reason); }
    finally { setUploadProgress((value) => { const next = { ...value }; delete next[kind]; return next; }); }
  }

  async function deleteArtifact(item: Artifact) {
    if (!session || !window.confirm(`Удалить ${item.file_name} из релиза?`)) return;
    try {
      await api.request(`/v1/admin/releases/${draft.id}/artifacts/${item.kind}`, { method: "DELETE", headers: { "X-CSRF-Token": session.csrf_token } });
      await load(draft.id); setStatus("Установщик удалён.");
    } catch (reason) { fail(reason); }
  }

  async function uploadScreenshot() {
    if (!session || !draft.id || !shotFile) return;
    const body = new FormData(); body.append("file", shotFile); body.append("caption_ru", shotRU); body.append("caption_en", shotEN);
    setUploadProgress((value) => ({ ...value, screenshot: 0 }));
    try {
      await uploadWithProgress(`/release-upload/v1/admin/releases/${draft.id}/screenshots`, "POST", body, session.csrf_token,
        (value) => setUploadProgress((current) => ({ ...current, screenshot: value })));
      setShotFile(undefined); setShotRU(""); setShotEN(""); await load(draft.id); setStatus("Скриншот добавлен.");
    } catch (reason) { fail(reason); }
    finally { setUploadProgress((value) => { const next = { ...value }; delete next.screenshot; return next; }); }
  }

  function editShot(id: string, patch: Partial<Screenshot>) {
    setDraft({ ...draft, screenshots: draft.screenshots.map((item) => item.id === id ? { ...item, ...patch } : item) });
  }
  async function saveShot(item: Screenshot) {
    if (!session) return;
    try {
      await api.json(`/v1/admin/releases/${draft.id}/screenshots/${item.id}`, "PUT", {
        caption_ru: item.caption_ru, caption_en: item.caption_en, sort_order: Number(item.sort_order) || 0,
      }, session.csrf_token);
      await load(draft.id); setStatus("Подписи скриншота сохранены.");
    } catch (reason) { fail(reason); }
  }
  async function deleteShot(item: Screenshot) {
    if (!session || !window.confirm("Удалить скриншот?")) return;
    try {
      await api.request(`/v1/admin/releases/${draft.id}/screenshots/${item.id}`, { method: "DELETE", headers: { "X-CSRF-Token": session.csrf_token } });
      await load(draft.id); setStatus("Скриншот удалён.");
    } catch (reason) { fail(reason); }
  }

  const fieldError = (name: string) => fieldErrors[name] ? <span className="release-field-error" role="alert">{fieldErrors[name]}</span> : null;

  return <AdminShell>
    <div className="admin-page-head"><div><h1 className="vlt-title">Релизы</h1><p className="vlt-subtitle">Черновики, установщики и публичная история обновлений VLT Studio Pro.</p></div><button className="vlt-button" onClick={startNew} disabled={busy}><Plus size={16} aria-hidden />Новый релиз</button></div>
    {error && <div className="vlt-error">{error}</div>}
    {Object.keys(fieldErrors).length > 0 && <div className="vlt-error release-error-summary" ref={errorSummary} tabIndex={-1} role="alert"><strong>Исправьте поля перед продолжением:</strong><ul>{Object.entries(fieldErrors).map(([name, message]) => <li key={name}><a href={`#release-${name}`}>{message}</a></li>)}</ul></div>}
    <div className="release-registry-grid">
      <section className="vlt-card vlt-card-pad" aria-label="Список релизов"><h2 className="vlt-section-title">Версии</h2><div className="release-list">
        {!releases && <p className="vlt-muted" role="status">Загрузка…</p>}
        {releases?.length === 0 && <p className="vlt-muted">Релизов пока нет.</p>}
        {releases?.map((item) => <button key={item.id} className={`release-list-item ${item.id === draft.id ? "selected" : ""}`} onClick={() => choose(item)} aria-pressed={item.id === draft.id}><PackageOpen size={18} aria-hidden /><span><strong>{item.version || "Без номера"}</strong><small>{new Date(item.updated_at).toLocaleDateString("ru-RU")} · {item.artifacts.length} файл.</small></span><span className={`vlt-badge ${item.status === "published" ? "vlt-badge-accent" : ""}`}>{item.status === "published" ? "выпущен" : "черновик"}</span></button>)}
      </div></section>

      <div className="release-editor">
        <section className="vlt-card vlt-card-pad"><div className="vlt-row vlt-between"><h2 className="vlt-section-title">{draft.id ? `Версия ${draft.version || "без номера"}` : "Новый черновик"}</h2>{draft.status === "published" && <span className="vlt-badge vlt-badge-accent">Опубликован</span>}</div>
          <div className="release-form">
            <label className="vlt-label" htmlFor="release-version">Версия X.Y.Z<input id="release-version" className="vlt-input vlt-code" value={draft.version} disabled={draft.status === "published"} placeholder="0.1.2" onChange={(event) => setDraft({ ...draft, version: event.target.value })} />{fieldError("version")}</label>
            <label className="vlt-label" htmlFor="release-summary_ru">Кратко — русский<textarea id="release-summary_ru" className="vlt-input" value={draft.summary_ru} onChange={(event) => setDraft({ ...draft, summary_ru: event.target.value })} />{fieldError("summary_ru")}</label>
            <label className="vlt-label" htmlFor="release-summary_en">Summary — English<textarea id="release-summary_en" className="vlt-input" value={draft.summary_en} onChange={(event) => setDraft({ ...draft, summary_en: event.target.value })} />{fieldError("summary_en")}</label>
            {([ ["features", "Новое / New"], ["changes", "Изменения / Changes"], ["fixes", "Исправления / Fixes"] ] as const).map(([key, label]) => <div className="release-paired-fields" key={key}><h3>{label}</h3><label className="vlt-label" htmlFor={`release-${key}_ru`}>Русский, один пункт на строку<textarea id={`release-${key}_ru`} className="vlt-input" value={draft[`${key}_ru`].join("\n")} onChange={(event) => setList(`${key}_ru`, event.target.value)} />{fieldError(`${key}_ru`)}</label><label className="vlt-label" htmlFor={`release-${key}_en`}>English, one item per line<textarea id={`release-${key}_en`} className="vlt-input" value={draft[`${key}_en`].join("\n")} onChange={(event) => setList(`${key}_en`, event.target.value)} />{fieldError(`${key}_en`)}</label></div>)}
          </div>
          <div className="vlt-row release-actions"><button className="vlt-button" onClick={() => void save()} disabled={busy}><Save size={16} aria-hidden />{busy ? "Сохранение…" : "Сохранить черновик"}</button><button className="vlt-button vlt-button-secondary" onClick={() => void publish()} disabled={busy || draft.status === "published"}><Rocket size={16} aria-hidden />Опубликовать</button>{draft.id && draft.status === "draft" && <button className="vlt-button vlt-button-danger" onClick={() => void removeDraft()} disabled={busy}><Trash2 size={16} aria-hidden />Удалить черновик</button>}</div>
        </section>

        <section className="vlt-card vlt-card-pad" id="release-artifacts"><h2 className="vlt-section-title">Установщики</h2><p className="vlt-subtitle">До 2 ГиБ на файл. Сначала сохраните новый черновик.</p>{fieldError("artifacts")}<div className="artifact-grid">{artifactDefinitions.map((definition) => {
          const artifact = draft.artifacts.find((item) => item.kind === definition.kind); const progress = uploadProgress[definition.kind];
          return <article className="artifact-card" key={definition.kind}><FileArchive size={19} aria-hidden /><div><strong>{definition.title}</strong><small>{artifact ? `${artifact.file_name} · ${readableBytes(artifact.bytes)}` : definition.help}</small>{artifact && <code title={artifact.sha256}>{artifact.sha256.slice(0, 16)}…</code>}</div><label className="vlt-button vlt-button-secondary artifact-upload">{progress === undefined ? <><Upload size={15} aria-hidden />{artifact ? "Заменить" : "Загрузить"}</> : `${progress}%`}<input type="file" accept={definition.accept} disabled={!draft.id || progress !== undefined} onChange={(event) => { void uploadArtifact(definition.kind, event.target.files?.[0]); event.currentTarget.value = ""; }} /></label>{artifact && <button className="artifact-delete" type="button" onClick={() => void deleteArtifact(artifact)} aria-label={`Удалить ${artifact.file_name}`}><Trash2 size={16} aria-hidden /></button>}{progress !== undefined && <progress max="100" value={progress} aria-label={`Загрузка ${definition.title}`} />}</article>;
        })}</div></section>

        <section className="vlt-card vlt-card-pad" id="release-screenshots"><h2 className="vlt-section-title">Скриншоты</h2><p className="vlt-subtitle">До 10 изображений JPEG, PNG или WebP по 10 МБ.</p>{fieldError("screenshots")}
          <div className="screenshot-upload-form"><label className="vlt-label">Изображение<input className="vlt-input" type="file" accept="image/jpeg,image/png,image/webp" disabled={!draft.id} onChange={(event) => setShotFile(event.target.files?.[0])} /></label><label className="vlt-label">Подпись RU<input className="vlt-input" value={shotRU} onChange={(event) => setShotRU(event.target.value)} /></label><label className="vlt-label">Caption EN<input className="vlt-input" value={shotEN} onChange={(event) => setShotEN(event.target.value)} /></label><button className="vlt-button" disabled={!draft.id || !shotFile || uploadProgress.screenshot !== undefined} onClick={() => void uploadScreenshot()}><ImagePlus size={16} aria-hidden />{uploadProgress.screenshot === undefined ? "Добавить" : `${uploadProgress.screenshot}%`}</button></div>
          <div className="release-screenshot-list">{draft.screenshots.map((item) => <article className="release-screenshot-card" key={item.id}><img src={`/api${item.url}`} width={item.width} height={item.height} alt={item.caption_ru || item.caption_en || "Скриншот релиза"} loading="lazy" /><div><label className="vlt-label">Подпись RU<input className="vlt-input" value={item.caption_ru} onChange={(event) => editShot(item.id, { caption_ru: event.target.value })} /></label><label className="vlt-label">Caption EN<input className="vlt-input" value={item.caption_en} onChange={(event) => editShot(item.id, { caption_en: event.target.value })} /></label><label className="vlt-label">Порядок<input className="vlt-input" type="number" value={item.sort_order} onChange={(event) => editShot(item.id, { sort_order: Number(event.target.value) })} /></label><div className="vlt-row"><button className="vlt-button vlt-button-secondary" onClick={() => void saveShot(item)}>Сохранить</button><button className="vlt-button vlt-button-danger" onClick={() => void deleteShot(item)}><Trash2 size={16} aria-hidden />Удалить</button></div></div></article>)}</div>
        </section>
        {status && <p className="release-status" role="status">{status}</p>}
      </div>
    </div>
  </AdminShell>;
}
