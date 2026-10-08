"use client";

import releaseTemplate from "../release-template.json";
import type { APIError } from "@vlt/api-client";
import { api } from "@vlt/api-client";
import { CheckCircle2, Circle, FileArchive, ImagePlus, PackageOpen, Plus, Rocket, Save, Trash2, Upload, Undo2 } from "lucide-react";
import { useEffect, useRef, useState } from "react";
import { AdminShell } from "./admin-shell";
import { useAdmin } from "./use-admin";
import { ReleaseHighlightEditor, type ReleaseHighlight } from "./release-highlight-editor";

type ArtifactKind = "windows-exe" | "macos-dmg" | "linux-appimage" | "linux-deb" | "linux-rpm" | "linux-tar-gz" | "linux-tar-xz";
type Artifact = { id: string; kind: ArtifactKind; platform: string; label: string; file_name: string; bytes: number; sha256: string; download_url: string; updated_at: string };
type Screenshot = { id: string; caption_ru: string; caption_en: string; sort_order: number; width: number; height: number; sha256: string; url: string };
type Release = {
  id: string; version: string; status: "draft" | "published"; summary_ru: string; summary_en: string;
  features_ru: string[]; features_en: string[]; changes_ru: string[]; changes_en: string[]; fixes_ru: string[]; fixes_en: string[];
  highlights: ReleaseHighlight[];
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
  highlights: [],
  artifacts: [], screenshots: [], created_at: "", updated_at: "",
});

const currentReleaseTemplate = (): Release => ({
  ...emptyRelease(),
  ...releaseTemplate,
});
function normalizeRelease(item: Release): Release { return { ...emptyRelease(), ...item, highlights: item.highlights ?? [], screenshots: (item.screenshots ?? []).map(shot => ({ ...shot, caption_ru: shot.caption_ru ?? "", caption_en: shot.caption_en ?? "" })) }; }

function lines(value: string) { return value.split("\n").map((item) => item.trim()).filter(Boolean); }
function validVersion(raw: string) { const value = raw.trim(); return [...value].length <= 80 && /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?: [\p{L}\p{N}._()-](?:[\p{L}\p{N} ._()-]*[\p{L}\p{N}._()-])?)?$/u.test(value) && value.split(" ")[0].split(".").every(part => Number(part) <= 2147483647); }
function releaseContent(item: Release) {
  return { version: item.version, summary_ru: item.summary_ru, summary_en: item.summary_en, features_ru: item.features_ru, features_en: item.features_en, changes_ru: item.changes_ru, changes_en: item.changes_en, fixes_ru: item.fixes_ru, fixes_en: item.fixes_en, highlights: item.highlights ?? [] };
}
type Recovery = { id: string; content: ReturnType<typeof releaseContent>; updatedAt: string };
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
  const [savedContent, setSavedContent] = useState(() => JSON.stringify(releaseContent(emptyRelease())));
  const [recovery, setRecovery] = useState<Recovery>();
  const [busy, setBusy] = useState(false);
  const [status, setStatus] = useState("");
  const [fieldErrors, setFieldErrors] = useState<Record<string, string>>({});
  const [uploadProgress, setUploadProgress] = useState<Record<string, number>>({});
  const [shotFile, setShotFile] = useState<File>();
  const [shotRU, setShotRU] = useState("");
  const [shotEN, setShotEN] = useState("");
  const shotInput = useRef<HTMLInputElement>(null);
  const errorSummary = useRef<HTMLDivElement>(null);
  const recoveryKey = session ? `vlt-admin-release-draft:${session.admin.id}` : "";
  const dirty = JSON.stringify(releaseContent(draft)) !== savedContent;
  const uploading = Object.keys(uploadProgress).length > 0;
  const locked = busy || uploading;

  function adopt(item: Release) { const next = normalizeRelease(item); setDraft(next); setSavedContent(JSON.stringify(releaseContent(next))); }
  function clearRecovery() { try { if (recoveryKey) localStorage.removeItem(recoveryKey); } catch { /* Local recovery is optional. */ } setRecovery(undefined); }

  async function load(preferred?: string) {
    const result = await api.request<{ releases: Release[] }>("/v1/admin/releases");
    setReleases(result.releases.map(normalizeRelease));
    if (preferred) {
      const selected = result.releases.find((item) => item.id === preferred);
      if (selected) adopt(selected);
    }
  }
  useEffect(() => { if (session) void load().catch(fail); }, [session]);
  useEffect(() => {
    if (!recoveryKey) return;
    try { const raw = localStorage.getItem(recoveryKey); if (!raw) return; const recovered = JSON.parse(raw) as Recovery; if (typeof recovered.id === "string" && typeof recovered.content?.version === "string" && typeof recovered.content.summary_ru === "string" && typeof recovered.content.summary_en === "string" && [recovered.content.features_ru, recovered.content.features_en, recovered.content.changes_ru, recovered.content.changes_en, recovered.content.fixes_ru, recovered.content.fixes_en, recovered.content.highlights].every(Array.isArray)) setRecovery(recovered); } catch { /* An invalid local draft must not prevent opening the editor. */ }
  }, [recoveryKey]);
  useEffect(() => {
    if (!recoveryKey || !dirty) return;
    const timer = window.setTimeout(() => { try { localStorage.setItem(recoveryKey, JSON.stringify({ id: draft.id, content: releaseContent(draft), updatedAt: new Date().toISOString() } satisfies Recovery)); } catch { /* Storage may be unavailable. */ } }, 300);
    const warn = (event: BeforeUnloadEvent) => { try { localStorage.setItem(recoveryKey, JSON.stringify({ id: draft.id, content: releaseContent(draft), updatedAt: new Date().toISOString() } satisfies Recovery)); } catch { /* Optional recovery. */ } event.preventDefault(); event.returnValue = ""; };
    window.addEventListener("beforeunload", warn);
    return () => { window.clearTimeout(timer); window.removeEventListener("beforeunload", warn); };
  }, [draft, dirty, recoveryKey]);
  useEffect(() => {
    if (Object.keys(fieldErrors).length > 0) errorSummary.current?.focus();
  }, [fieldErrors]);

  function canSwitch() { return !dirty || window.confirm("Перейти без сохранения текущего текста? Несохранённый текст будет заменён."); }
  function choose(item: Release) { if (!canSwitch()) return; clearRecovery(); adopt(item); setStatus(""); setFieldErrors({}); }
  function startNew() { if (!canSwitch()) return; clearRecovery(); adopt(emptyRelease()); setStatus(""); setFieldErrors({}); }
  function restoreRecovery() {
    if (!recovery || !releases) return;
    const original = releases.find(item => item.id === recovery.id) ?? emptyRelease();
    const content = { ...recovery.content, highlights: recovery.content.highlights.map(item => ({ ...item, screenshot_id: original.screenshots.some(shot => shot.id === item.screenshot_id) ? item.screenshot_id : "" })) };
    setSavedContent(JSON.stringify(releaseContent(original))); setDraft({ ...original, ...content }); setRecovery(undefined); setStatus("Текст восстановлен. Проверьте изменения и сохраните их на сервере.");
  }
  function useCurrentReleaseTemplate() { if (!canSwitch()) return; setDraft(currentReleaseTemplate()); setStatus("Шаблон релиза заполнен. Проверьте текст и сохраните черновик."); setFieldErrors({}); }
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
      highlights: draft.highlights,
    };
  }

  async function save(showStatus = true): Promise<Release | undefined> {
    if (!session) return;
    setBusy(true); setStatus(""); setFieldErrors({});
    try {
      const saved = draft.id
        ? await api.json<Release>(`/v1/admin/releases/${draft.id}`, "PUT", payload(), session.csrf_token)
        : await api.json<Release>("/v1/admin/releases", "POST", payload(), session.csrf_token);
      adopt(saved); clearRecovery();
      await load(saved.id);
      if (showStatus) setStatus(saved.status === "published" ? "Изменения опубликованного релиза сохранены." : "Черновик сохранён.");
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
      adopt(published); await load(published.id); setStatus(`Версия ${published.version} опубликована.`);
    } catch (reason) { fail(reason); }
    finally { setBusy(false); }
  }

  async function removeDraft() {
    if (!session || !draft.id || draft.status !== "draft") return;
    if (!window.confirm(`Удалить черновик ${draft.version || "без номера"} и все его файлы?`)) return;
    setBusy(true);
    try {
      await api.request(`/v1/admin/releases/${draft.id}`, { method: "DELETE", headers: { "X-CSRF-Token": session.csrf_token } });
      adopt(emptyRelease()); clearRecovery(); await load(); setStatus("Черновик удалён.");
    } catch (reason) { fail(reason); }
    finally { setBusy(false); }
  }

  async function uploadArtifact(kind: ArtifactKind, file?: File) {
    if (!session || !file) return;
    const existing = draft.artifacts.find((item) => item.kind === kind);
    if (existing && !window.confirm(`Заменить ${existing.file_name} файлом ${file.name}?`)) return;
    setStatus(""); setFieldErrors({}); setUploadProgress((value) => ({ ...value, [kind]: 0 }));
    try {
      const target = await save(false);
      if (!target) return;
      const body = new FormData(); body.append("file", file);
      await uploadWithProgress(`/release-upload/v1/admin/releases/${target.id}/artifacts/${kind}`, "PUT", body, session.csrf_token,
        (value) => setUploadProgress((current) => ({ ...current, [kind]: value })));
      await load(target.id); setStatus(`${file.name} загружен.`);
    } catch (reason) { fail(reason); }
    finally { setUploadProgress((value) => { const next = { ...value }; delete next[kind]; return next; }); }
  }

  async function deleteArtifact(item: Artifact) {
    if (!session || !window.confirm(`Удалить ${item.file_name} из релиза?`)) return;
    try {
      const target = await save(false); if (!target) return;
      await api.request(`/v1/admin/releases/${draft.id}/artifacts/${item.kind}`, { method: "DELETE", headers: { "X-CSRF-Token": session.csrf_token } });
      await load(draft.id); setStatus("Установщик удалён.");
    } catch (reason) { fail(reason); }
  }

  async function uploadScreenshot() {
    if (!session || !shotFile) return;
    setUploadProgress((value) => ({ ...value, screenshot: 0 }));
    try {
      const target = await save(false);
      if (!target) return;
      const body = new FormData(); body.append("file", shotFile); body.append("caption_ru", shotRU); body.append("caption_en", shotEN);
      await uploadWithProgress(`/release-upload/v1/admin/releases/${target.id}/screenshots`, "POST", body, session.csrf_token,
        (value) => setUploadProgress((current) => ({ ...current, screenshot: value })));
      setShotFile(undefined); setShotRU(""); setShotEN("");
      if (shotInput.current) shotInput.current.value = "";
      await load(target.id); setStatus("Скриншот добавлен.");
    } catch (reason) { fail(reason); }
    finally { setUploadProgress((value) => { const next = { ...value }; delete next.screenshot; return next; }); }
  }

  function editShot(id: string, patch: Partial<Screenshot>) {
    setDraft({ ...draft, screenshots: draft.screenshots.map((item) => item.id === id ? { ...item, ...patch } : item) });
  }
  async function saveShot(item: Screenshot) {
    if (!session) return;
    try {
      const target = await save(false); if (!target) return;
      await api.json(`/v1/admin/releases/${draft.id}/screenshots/${item.id}`, "PUT", {
        caption_ru: item.caption_ru, caption_en: item.caption_en, sort_order: Number(item.sort_order) || 0,
      }, session.csrf_token);
      await load(draft.id); setStatus("Подписи скриншота сохранены.");
    } catch (reason) { fail(reason); }
  }
  async function deleteShot(item: Screenshot) {
    if (!session) return;
    if (draft.highlights.some(block => block.screenshot_id === item.id)) { setStatus("Этот скриншот используется в блоке обновления. Выберите другое изображение в блоке и сохраните релиз перед удалением."); return; }
    if (!window.confirm("Удалить скриншот?")) return;
    try {
      const target = await save(false); if (!target) return;
      await api.request(`/v1/admin/releases/${draft.id}/screenshots/${item.id}`, { method: "DELETE", headers: { "X-CSRF-Token": session.csrf_token } });
      await load(draft.id); setStatus("Скриншот удалён.");
    } catch (reason) { fail(reason); }
  }

  const fieldError = (name: string) => fieldErrors[name] ? <span className="release-field-error" role="alert">{fieldErrors[name]}</span> : null;
  const readiness = [
    { label: "Номер версии", done: validVersion(draft.version), href: "#release-version" },
    { label: "Описание на двух языках", done: !!draft.summary_ru.trim() && !!draft.summary_en.trim(), href: "#release-summary_ru" },
    { label: "Хотя бы один установщик", done: draft.artifacts.length > 0, href: "#release-artifacts" },
    { label: "Подписи скриншотов RU / EN", done: draft.screenshots.every(shot => shot.caption_ru.trim() && shot.caption_en.trim()), href: "#release-screenshots" },
    { label: "Тексты и изображения блоков", done: draft.highlights.every(block => block.title_ru.trim() && block.title_en.trim() && block.lead_ru.trim() && block.lead_en.trim() && draft.screenshots.some(shot => shot.id === block.screenshot_id)), href: "#release-highlights" },
  ];

  return <AdminShell>
    <div className="admin-page-head"><div><h1 className="vlt-title">Релизы</h1><p className="vlt-subtitle">От черновика до страницы обновлений: тексты, цветные блоки и файлы приложения.</p></div><button className="vlt-button" onClick={startNew} disabled={locked}><Plus size={16} aria-hidden />Новый релиз</button></div>
    {error && <div className="vlt-error">{error}</div>}
    {recovery && <div className="release-recovery" role="status"><div><Undo2 size={20} aria-hidden /><span><strong>Есть несохранённый текст{recovery.content.version ? ` версии ${recovery.content.version}` : ""}</strong><small>Восстановите его после перезагрузки или продолжите с данными сервера.</small></span></div><div className="vlt-row"><button className="vlt-button" onClick={restoreRecovery} disabled={!releases || locked}>Восстановить</button><button className="vlt-button vlt-button-secondary" onClick={clearRecovery}>Удалить копию</button></div></div>}
    {Object.keys(fieldErrors).length > 0 && <div className="vlt-error release-error-summary" ref={errorSummary} tabIndex={-1} role="alert"><strong>Исправьте поля перед продолжением:</strong><ul>{Object.entries(fieldErrors).map(([name, message]) => <li key={name}><a href={`#release-${name}`}>{message}</a></li>)}</ul></div>}
    <div className="release-registry-grid">
      <section className="vlt-card vlt-card-pad release-version-panel" aria-label="Список релизов"><h2 className="vlt-section-title">Версии</h2><div className="release-list">
        {!releases && <p className="vlt-muted" role="status">Загрузка…</p>}
        {releases?.length === 0 && <p className="vlt-muted">Релизов пока нет.</p>}
        {releases?.map((item) => <button key={item.id} disabled={locked} className={`release-list-item ${item.id === draft.id ? "selected" : ""}`} onClick={() => choose(item)} aria-pressed={item.id === draft.id}><PackageOpen size={18} aria-hidden /><span><strong>{item.version || "Без номера"}</strong><small>{new Date(item.updated_at).toLocaleDateString("ru-RU")} · {item.artifacts.length} файл.</small></span><span className={`vlt-badge ${item.status === "published" ? "vlt-badge-accent" : ""}`}>{item.status === "published" ? "выпущен" : "черновик"}</span></button>)}
      </div><div className="release-readiness"><h3>Готовность к публикации</h3><p>{readiness.filter(item => item.done).length} из {readiness.length} условий выполнено</p>{readiness.map(item => <a href={item.href} key={item.href} data-ready={!!item.done}>{item.done ? <CheckCircle2 size={17} aria-hidden /> : <Circle size={17} aria-hidden />}<span>{item.label}</span><span className="sr-only">{item.done ? "выполнено" : "не выполнено"}</span></a>)}</div></section>

      <fieldset className="release-editor" disabled={locked} aria-label="Редактор релиза">
<div className="release-composer-toolbar">        <nav className="release-section-nav" aria-label="Разделы редактора"><a href="#release-description">Описание</a><a href="#release-highlights">Главные обновления</a><a href="#release-artifacts">Установщики</a><a href="#release-screenshots">Скриншоты</a><span>{dirty ? "Есть изменения" : draft.id ? "Сохранено" : "Новый черновик"}</span></nav>          <div className="vlt-row release-actions"><button className="vlt-button" onClick={() => void save()} disabled={busy}><Save size={16} aria-hidden />{busy ? "Сохранение…" : draft.status === "published" ? "Сохранить изменения" : "Сохранить черновик"}</button><button className="vlt-button vlt-button-secondary" onClick={() => void publish()} disabled={busy || draft.status === "published"}><Rocket size={16} aria-hidden />Опубликовать</button>{draft.id && draft.status === "draft" && <button className="vlt-button vlt-button-danger" onClick={() => void removeDraft()} disabled={busy}><Trash2 size={16} aria-hidden />Удалить черновик</button>}</div></div>
        <section className="vlt-card vlt-card-pad" id="release-description"><div className="vlt-row vlt-between"><h2 className="vlt-section-title">{draft.id ? `Версия ${draft.version || "без номера"}` : "Новый черновик"}</h2>{draft.status === "published" ? <span className="vlt-badge vlt-badge-accent">Опубликован</span> : !draft.id && <button className="vlt-button vlt-button-secondary" type="button" onClick={useCurrentReleaseTemplate} disabled={locked}><PackageOpen size={16} aria-hidden />Заполнить шаблон</button>}</div>
          <div className="release-form">
            <label className="vlt-label" htmlFor="release-version">Версия<input id="release-version" className="vlt-input vlt-code" value={draft.version} disabled={draft.status === "published"} placeholder="0.2.4 Alpha 1" onChange={(event) => setDraft({ ...draft, version: event.target.value })} /><span className="vlt-muted">Формат X.Y.Z; после номера можно добавить метку, например Alpha 1 или Build 1.</span>{fieldError("version")}</label>
            <label className="vlt-label" htmlFor="release-summary_ru">Кратко — русский<textarea id="release-summary_ru" className="vlt-input" value={draft.summary_ru} onChange={(event) => setDraft({ ...draft, summary_ru: event.target.value })} />{fieldError("summary_ru")}</label>
            <label className="vlt-label" htmlFor="release-summary_en">Summary — English<textarea id="release-summary_en" className="vlt-input" value={draft.summary_en} onChange={(event) => setDraft({ ...draft, summary_en: event.target.value })} />{fieldError("summary_en")}</label>
            <details className="release-extra-notes"><summary>Остальные изменения и исправления</summary>{([ ["features", "Новое / New"], ["changes", "Изменения / Changes"], ["fixes", "Исправления / Fixes"] ] as const).map(([key, label]) => <div className="release-paired-fields" key={key}><h3>{label}</h3><label className="vlt-label" htmlFor={`release-${key}_ru`}>Русский, один пункт на строку<textarea id={`release-${key}_ru`} className="vlt-input" value={draft[`${key}_ru`].join("\n")} onChange={(event) => setList(`${key}_ru`, event.target.value)} />{fieldError(`${key}_ru`)}</label><label className="vlt-label" htmlFor={`release-${key}_en`}>English, one item per line<textarea id={`release-${key}_en`} className="vlt-input" value={draft[`${key}_en`].join("\n")} onChange={(event) => setList(`${key}_en`, event.target.value)} />{fieldError(`${key}_en`)}</label></div>)}</details>
          </div>

        </section>

        {fieldError("highlights")}
        <ReleaseHighlightEditor value={draft.highlights} screenshots={draft.screenshots} onChange={highlights => setDraft({ ...draft, highlights })} disabled={locked} />
        <section className="vlt-card vlt-card-pad" id="release-artifacts"><h2 className="vlt-section-title">Установщики</h2><p className="vlt-subtitle">До 2 ГиБ на файл. Изменения текста сохраняются перед загрузкой.</p>{fieldError("artifacts")}<div className="artifact-grid">{artifactDefinitions.map((definition) => {
          const artifact = draft.artifacts.find((item) => item.kind === definition.kind); const progress = uploadProgress[definition.kind];
          return <article className="artifact-card" key={definition.kind}><FileArchive size={19} aria-hidden /><div><strong>{definition.title}</strong><small>{artifact ? `${artifact.file_name} · ${readableBytes(artifact.bytes)}` : definition.help}</small>{artifact && <code title={artifact.sha256}>{artifact.sha256.slice(0, 16)}…</code>}</div><label className="vlt-button vlt-button-secondary artifact-upload">{progress === undefined ? <><Upload size={15} aria-hidden />{artifact ? "Заменить" : "Загрузить"}</> : `${progress}%`}<input type="file" accept={definition.accept} disabled={busy || progress !== undefined} onChange={(event) => { void uploadArtifact(definition.kind, event.target.files?.[0]); event.currentTarget.value = ""; }} /></label>{artifact && <button className="artifact-delete" type="button" onClick={() => void deleteArtifact(artifact)} aria-label={`Удалить ${artifact.file_name}`}><Trash2 size={16} aria-hidden /></button>}{progress !== undefined && <progress max="100" value={progress} aria-label={`Загрузка ${definition.title}`} />}</article>;
        })}</div></section>

        <section className="vlt-card vlt-card-pad" id="release-screenshots"><h2 className="vlt-section-title">Скриншоты</h2><p className="vlt-subtitle">До 10 изображений JPEG, PNG или WebP по 10 МБ.</p>{fieldError("screenshots")}
          <div className="screenshot-upload-form"><label className="vlt-label">Изображение<input ref={shotInput} className="vlt-input" type="file" accept="image/jpeg,image/png,image/webp" disabled={busy || uploadProgress.screenshot !== undefined} onChange={(event) => setShotFile(event.target.files?.[0])} /></label><label className="vlt-label">Подпись RU<input className="vlt-input" value={shotRU} onChange={(event) => setShotRU(event.target.value)} /></label><label className="vlt-label">Caption EN<input className="vlt-input" value={shotEN} onChange={(event) => setShotEN(event.target.value)} /></label><button className="vlt-button" disabled={busy || !shotFile || uploadProgress.screenshot !== undefined} onClick={() => void uploadScreenshot()}><ImagePlus size={16} aria-hidden />{uploadProgress.screenshot === undefined ? "Добавить" : `${uploadProgress.screenshot}%`}</button></div>
          <div className="release-screenshot-list">{draft.screenshots.map((item) => <article className="release-screenshot-card" key={item.id}><img src={`/api${item.url}`} width={item.width} height={item.height} alt={item.caption_ru || item.caption_en || "Скриншот релиза"} loading="lazy" /><div><label className="vlt-label">Подпись RU<input className="vlt-input" value={item.caption_ru} onChange={(event) => editShot(item.id, { caption_ru: event.target.value })} /></label><label className="vlt-label">Caption EN<input className="vlt-input" value={item.caption_en} onChange={(event) => editShot(item.id, { caption_en: event.target.value })} /></label><label className="vlt-label">Порядок<input className="vlt-input" type="number" value={item.sort_order} onChange={(event) => editShot(item.id, { sort_order: Number(event.target.value) })} /></label><div className="vlt-row"><button className="vlt-button vlt-button-secondary" onClick={() => void saveShot(item)}>Сохранить</button><button className="vlt-button vlt-button-danger" onClick={() => void deleteShot(item)}><Trash2 size={16} aria-hidden />Удалить</button></div></div></article>)}</div>
        </section>
        {status && <p className="release-status" role="status">{status}</p>}
      </fieldset>
    </div>
  </AdminShell>;
}
