"use client";

import { api } from "@vlt/api-client";
import { useEffect, useRef, useState, type FormEvent } from "react";
import { AdminShell } from "./admin-shell";
import { useAdmin } from "./use-admin";

type Background = { id: string; title: string; published: boolean; sort_order: number; width: number; height: number; bytes: number; thumbnail_url: string; url: string };
const imageURL = (path: string) => /^\/v1\/(admin\/)?browser-backgrounds\/[0-9a-f-]+\/(image|thumbnail)$/.test(path) ? `/api${path}` : "";

function BackgroundCard({ item, csrf, onSaved, onDeleted }: { item: Background; csrf: string; onSaved: (item: Background) => void; onDeleted: (id: string) => void }) {
  const [title, setTitle] = useState(item.title), [order, setOrder] = useState(item.sort_order), [published, setPublished] = useState(item.published);
  const [busy, setBusy] = useState(false), [error, setError] = useState(""), [status, setStatus] = useState("");
  useEffect(() => { setTitle(item.title); setOrder(item.sort_order); setPublished(item.published); }, [item.title, item.sort_order, item.published]);
  async function save(event: FormEvent) {
    event.preventDefault(); setBusy(true); setError(""); setStatus("");
    try { onSaved(await api.json<Background>(`/v1/admin/browser-backgrounds/${item.id}`, "PUT", { title, sort_order: order, published }, csrf)); setStatus("Изменения сохранены."); }
    catch (e) { setError((e as Error).message); } finally { setBusy(false); }
  }
  async function remove() {
    if (!window.confirm(`Удалить фон «${item.title}» из коллекции? Уже выбранный фон останется в локальном кэше пользователей.`)) return;
    setBusy(true); setError("");
    try { await api.request(`/v1/admin/browser-backgrounds/${item.id}`, { method: "DELETE", headers: { "X-CSRF-Token": csrf } }); onDeleted(item.id); }
    catch (e) { setError((e as Error).message); } finally { setBusy(false); }
  }
  return <article className="vlt-card background-card">
    <a href={imageURL(item.url)} target="_blank" rel="noreferrer" aria-label={`Открыть изображение: ${item.title}`}><img src={imageURL(item.thumbnail_url)} alt={item.title} loading="lazy" /></a>
    <form className="vlt-stack" onSubmit={save}><div className="telemetry-toolbar"><span className={`vlt-badge ${item.published ? "vlt-badge-accent" : ""}`}>{item.published ? "В коллекции" : "Скрыт"}</span><span className="vlt-muted">{item.width} × {item.height} · {(item.bytes / 1048576).toFixed(1)} MB</span></div>
      <label className="vlt-label">Название<input className="vlt-input" value={title} onChange={(e) => setTitle(e.target.value)} required maxLength={160} disabled={busy} /></label>
      <label className="vlt-label">Порядок в коллекции<input className="vlt-input" type="number" min={0} max={100000} value={order} onChange={(e) => setOrder(Number(e.target.value))} required disabled={busy} /></label>
      <label className="vlt-checkbox"><input type="checkbox" checked={published} onChange={(e) => setPublished(e.target.checked)} disabled={busy} />Предлагать в настройках браузера</label>
      {error && <p className="vlt-error" role="alert">{error}</p>}{status && <p role="status" className="vlt-muted">{status}</p>}
      <div className="telemetry-toolbar"><button className="vlt-button vlt-button-secondary" disabled={busy}>{busy ? "Сохраняем…" : "Сохранить"}</button><button type="button" className="vlt-button vlt-button-danger" disabled={busy} onClick={() => void remove()}>Удалить</button></div>
    </form>
  </article>;
}

export function BrowserBackgroundRegistry() {
  const { session, error: sessionError } = useAdmin();
  const [items, setItems] = useState<Background[]>([]), [loading, setLoading] = useState(true);
  const [error, setError] = useState(""), [busy, setBusy] = useState(false), [status, setStatus] = useState("");
  const [file, setFile] = useState<File>(), [preview, setPreview] = useState(""), [title, setTitle] = useState("");
  const fileInput = useRef<HTMLInputElement>(null);
  useEffect(() => { if (!file) { setPreview(""); return; } const url = URL.createObjectURL(file); setPreview(url); return () => URL.revokeObjectURL(url); }, [file]);
  async function load() { setLoading(true); try { const result = await api.request<{ backgrounds: Background[] }>("/v1/admin/browser-backgrounds"); setItems(result.backgrounds); setError(""); } catch (e) { setError((e as Error).message); } finally { setLoading(false); } }
  useEffect(() => { if (session) void load(); }, [session]);
  async function upload(event: FormEvent) {
    event.preventDefault(); if (!session || !file || busy) return;
    if (file.size > 10 * 1048576) { setError("Выберите изображение до 10 MB."); return; }
    setBusy(true); setError(""); setStatus("");
    const form = new FormData(); form.append("title", title); form.append("file", file);
    try {
      // Streaming route avoids the rewrite body's size limit for large images.
      const response = await fetch("/release-upload/v1/admin/browser-backgrounds", { method: "POST", headers: { "X-CSRF-Token": session.csrf_token }, body: form });
      const result = await response.json();
      if (!response.ok) throw new Error(result.message || "Не удалось загрузить изображение.");
      setItems((old) => [result as Background, ...old]); setFile(undefined); setTitle(""); if (fileInput.current) fileInput.current.value = "";
      setStatus("Фон добавлен и доступен в коллекции приложения.");
    } catch (e) { setError((e as Error).message); } finally { setBusy(false); }
  }
  return <AdminShell><div className="admin-page-head"><div><h1 className="vlt-title">Фоны браузера</h1><p className="vlt-subtitle">Изображения для стартовой страницы. Пользователи выбирают их в настройках браузера VLTONE.</p></div></div>
    {(sessionError || error) && <p role="alert" className="vlt-error">{sessionError || error}</p>}{status && <p role="status">{status}</p>}
    <section className="vlt-card vlt-card-pad background-upload"><form className="vlt-stack" onSubmit={upload}><h2 className="vlt-section-title">Добавить изображение</h2><p className="vlt-muted">PNG, JPEG или WebP · до 10 MB и 16 мегапикселей · не более 100 фонов. Выбранные фоны сохраняются на компьютере и работают без интернета.</p>
      <label className="vlt-label">Название фона<input className="vlt-input" value={title} onChange={(e) => setTitle(e.target.value)} required maxLength={160} disabled={busy} /></label>
      <label className="vlt-label">Файл изображения<input ref={fileInput} className="vlt-input" type="file" accept="image/png,image/jpeg,image/webp" required disabled={busy} onChange={(e) => setFile(e.target.files?.[0])} /></label>
      <button className="vlt-button" disabled={busy || !session || !file || items.length >= 100}>{busy ? "Загрузка…" : "Добавить в коллекцию"}</button>
    </form>{preview && <img className="background-upload-preview" src={preview} alt="Предпросмотр нового фона" />}</section>
    <div className="telemetry-toolbar background-list-head"><h2 className="vlt-section-title">Коллекция · {items.length}</h2><button className="vlt-button vlt-button-secondary" disabled={loading} onClick={() => void load()}>Обновить</button></div>
    {loading && <p role="status">Загрузка коллекции…</p>}{!loading && !items.length && <p className="vlt-muted">Фонов пока нет. Добавьте первое изображение выше.</p>}
    <div className="background-grid">{items.map((item) => <BackgroundCard key={item.id} item={item} csrf={session?.csrf_token || ""} onSaved={(updated) => setItems((old) => old.map((entry) => entry.id === updated.id ? updated : entry).sort((a,b) => a.sort_order - b.sort_order))} onDeleted={(id) => setItems((old) => old.filter((entry) => entry.id !== id))} />)}</div>
  </AdminShell>;
}
