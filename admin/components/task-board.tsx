"use client";
import { api, type APIError } from "@vlt/api-client";
import { useEffect, useRef, useState, type FormEvent } from "react";
import { CalendarDays, MessageSquare, Paperclip, Plus, RefreshCw, Search, X } from "lucide-react";
import { AdminShell } from "./admin-shell";
import { canAdmin } from "./admin-permissions";
import { useAdmin } from "./use-admin";

const stages = [["idea", "Идеи"], ["planned", "Запланировано"], ["in_progress", "В работе"], ["review", "На проверке"], ["done", "Завершено"], ["archived", "Архив"]] as const;
const priorities = [["low", "Низкий"], ["normal", "Обычный"], ["high", "Высокий"], ["urgent", "Срочный"]] as const;
type Member = { id: string; nickname: string; active: boolean };
type Task = { id: string; number: number; title: string; description: string; status: string; priority: string; author_id: string; assignee_id: string | null; due_date: string | null; version: number; created_at: string; updated_at: string };
type Comment = { id: string; author_id: string; body: string; created_at: string };
type Attachment = { id: string; name: string; size: number; author_id: string };
type Detail = { task: Task; comments: Comment[]; attachments: Attachment[] };
type Draft = { title: string; description: string; status: string; priority: string; assignee_id: string; due_date: string };
const emptyDraft: Draft = { title: "", description: "", status: "idea", priority: "normal", assignee_id: "", due_date: "" };
const toDraft = (task: Task): Draft => ({ title: task.title, description: task.description, status: task.status, priority: task.priority, assignee_id: task.assignee_id ?? "", due_date: task.due_date?.slice(0, 10) ?? "" });
const dateLabel = (date: string) => new Date(date).toLocaleDateString("ru-RU", { day: "numeric", month: "short", timeZone: "UTC" });

export function TaskBoard() {
  const { session } = useAdmin();
  const writable = canAdmin(session?.admin, "tasks.write");
  const [tasks, setTasks] = useState<Task[]>([]);
  const [members, setMembers] = useState<Member[]>([]);
  const [query, setQuery] = useState("");
  const [search, setSearch] = useState("");
  const [assignee, setAssignee] = useState("");
  const [archive, setArchive] = useState(false);
  const [next, setNext] = useState(0);
  const [loading, setLoading] = useState(true);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [editorError, setEditorError] = useState("");
  const [notice, setNotice] = useState("");
  const [detail, setDetail] = useState<Detail>();
  const [draft, setDraft] = useState<Draft>(emptyDraft);
  const [comment, setComment] = useState("");
  const [dragging, setDragging] = useState<string>();
  const [dropStage, setDropStage] = useState<string>();
  const [confirmClose, setConfirmClose] = useState(false);
  const [deleteFile, setDeleteFile] = useState<string>();
  const dialog = useRef<HTMLDialogElement>(null);
  const uploadInput = useRef<HTMLInputElement>(null);
  const loadGeneration = useRef(0);
  const openGeneration = useRef(0);
  const busyRef = useRef(false);
  const savedDraft = useRef(JSON.stringify(emptyDraft));
  const memberName = (id: string | null) => members.find(member => member.id === id)?.nickname ?? (id ? "Участник" : "Без исполнителя");
  const dirty = JSON.stringify(draft) !== savedDraft.current || comment.trim() !== "";

  async function load(append = false) {
    const generation = ++loadGeneration.current;
    setLoading(true); setError("");
    const params = new URLSearchParams({ q: search, assignee, ...(archive ? { status: "archived" } : {}), ...(append && next ? { before: String(next) } : {}) });
    try {
      const result = await api.request<{ tasks: Task[]; next_cursor: number }>(`/v1/admin/tasks?${params}`);
      if (generation !== loadGeneration.current) return;
      setTasks(current => append ? [...current, ...result.tasks.filter(task => !current.some(item => item.id === task.id))] : result.tasks); setNext(result.next_cursor);
    } catch (e) { if (generation === loadGeneration.current) setError((e as APIError).message); }
    finally { if (generation === loadGeneration.current) setLoading(false); }
  }
  useEffect(() => {
    if (!canAdmin(session?.admin, "tasks.read")) return;
    void load();
    return () => { loadGeneration.current++; };
  }, [session, search, assignee, archive]);
  useEffect(() => {
    if (canAdmin(session?.admin, "tasks.read")) api.request<{ members: Member[] }>("/v1/admin/tasks/members").then(result => setMembers(result.members ?? [])).catch(e => setError((e as APIError).message));
  }, [session]);
  useEffect(() => {
    const leave = (event: BeforeUnloadEvent) => { if (dialog.current?.open && dirty) event.preventDefault(); };
    window.addEventListener("beforeunload", leave); return () => window.removeEventListener("beforeunload", leave);
  }, [dirty]);
  function startBusy() { if (busyRef.current) return false; busyRef.current = true; setBusy(true); return true; }
  function endBusy() { busyRef.current = false; setBusy(false); }
  function fillEditor(item?: Detail) { const value = item ? toDraft(item.task) : emptyDraft; setDetail(item); setDraft(value); savedDraft.current = JSON.stringify(value); setComment(""); setEditorError(""); setConfirmClose(false); setDeleteFile(undefined); }
  async function openTask(task?: Task) {
    if (busyRef.current) return;
    const generation = ++openGeneration.current;
    if (!task) { fillEditor(); dialog.current?.showModal(); return; }
    setError("");
    try { const result = await api.request<Detail>(`/v1/admin/tasks/${task.id}`); if (generation !== openGeneration.current) return; fillEditor(result); dialog.current?.showModal(); }
    catch (e) { setError((e as APIError).message); }
  }
  function closeEditor() { if (busyRef.current) return; if (dirty) { setConfirmClose(true); return; } dialog.current?.close(); }
  function updateTask(task: Task) {
    setTasks(current => {
      const matches = archive ? task.status === "archived" : task.status !== "archived";
      if (!matches || (assignee && task.assignee_id !== assignee)) return current.filter(item => item.id !== task.id);
      return current.some(item => item.id === task.id) ? current.map(item => item.id === task.id ? task : item) : [task, ...current];
    });
  }
  async function save(event: FormEvent) {
    event.preventDefault(); if (!session || !writable || !startBusy()) return; setEditorError("");
    try {
      const result = await api.json<Task>(detail ? `/v1/admin/tasks/${detail.task.id}` : "/v1/admin/tasks", detail ? "PUT" : "POST", { ...draft, assignee_id: draft.assignee_id || null, version: detail?.task.version ?? 0 }, session.csrf_token);
      updateTask(result); setDetail(current => ({ task: result, comments: current?.comments ?? [], attachments: current?.attachments ?? [] })); savedDraft.current = JSON.stringify(toDraft(result)); setDraft(toDraft(result)); setNotice(`Задача №${result.number} сохранена.`);
    } catch (e) { setEditorError((e as APIError).message); } finally { endBusy(); }
  }
  async function moveTask(task: Task, status: string) {
    if (!session || !writable || task.status === status || !startBusy()) return; setError("");
    try { const result = await api.json<Task>(`/v1/admin/tasks/${task.id}`, "PUT", { ...toDraft(task), assignee_id: task.assignee_id, status, version: task.version }, session.csrf_token); updateTask(result); setNotice(`№${task.number}: ${stages.find(stage => stage[0] === status)?.[1]}.`); }
    catch (e) { setError((e as APIError).message); } finally { endBusy(); }
  }
  async function postComment(event: FormEvent) {
    event.preventDefault(); if (!session || !detail || !comment.trim() || !startBusy()) return; setEditorError("");
    try { const result = await api.json<Comment>(`/v1/admin/tasks/${detail.task.id}/comments`, "POST", { body: comment }, session.csrf_token); setDetail(current => current && ({ ...current, comments: [...current.comments, result] })); setComment(""); }
    catch (e) { setEditorError((e as APIError).message); } finally { endBusy(); }
  }
  async function upload(files: FileList | null) {
    if (!session || !detail || !files?.length || !startBusy()) return; setEditorError("");
    try {
      for (const file of Array.from(files)) {
        if (!file.size || file.size > 10 * 1024 * 1024) throw { message: `${file.name}: нужен непустой файл до 10 МБ.` };
        const form = new FormData(); form.append("file", file);
        const response = await fetch(`/release-upload/v1/admin/tasks/${detail.task.id}/attachments`, { method: "POST", credentials: "include", headers: { "X-CSRF-Token": session.csrf_token }, body: form });
        if (!response.ok) throw await response.json().catch(() => ({ message: "Не удалось загрузить файл." }));
        const result = await response.json() as Attachment;
        setDetail(current => current && ({ ...current, attachments: [...current.attachments, result] }));
      }
    } catch (e) { setEditorError((e as APIError).message); } finally { if (uploadInput.current) uploadInput.current.value = ""; endBusy(); }
  }
  async function removeAttachment(id: string) {
    if (!session || !detail || !startBusy()) return; setEditorError("");
    try { await api.json(`/v1/admin/tasks/${detail.task.id}/attachments/${id}`, "DELETE", {}, session.csrf_token); setDetail(current => current && ({ ...current, attachments: current.attachments.filter(file => file.id !== id) })); setDeleteFile(undefined); }
    catch (e) { setEditorError((e as APIError).message); } finally { endBusy(); }
  }
  return <AdminShell>
    <div className="admin-page-head"><div><span className="task-kicker">Рабочее пространство команды</span><h1 className="vlt-title">Доска задач</h1><p className="vlt-subtitle">От первой идеи до готового результата.</p></div>{writable && <button className="vlt-button" onClick={() => void openTask()} disabled={busy}><Plus size={17} />Новая задача</button>}</div>
    <div className="task-toolbar"><form onSubmit={e => { e.preventDefault(); setSearch(query.trim()); }} className="task-search"><label className="sr-only" htmlFor="task-search">Поиск задач</label><Search size={17} aria-hidden /><input id="task-search" value={query} onChange={e => setQuery(e.target.value)} placeholder="Найти идею или задачу…" /><button className="vlt-button vlt-button-secondary" type="submit">Найти</button></form><label className="sr-only" htmlFor="task-assignee">Фильтр по исполнителю</label><select id="task-assignee" className="vlt-input" value={assignee} onChange={e => setAssignee(e.target.value)}><option value="">Все исполнители</option>{members.map(member => <option value={member.id} key={member.id}>{member.nickname}</option>)}</select><button className="vlt-button vlt-button-secondary" aria-pressed={archive} onClick={() => setArchive(!archive)}>{archive ? "К доске" : "Архив"}</button><button className="admin-icon-button" aria-label="Обновить задачи" disabled={loading || busy} onClick={() => void load()}><RefreshCw size={18} /></button></div>
    {error && <p className="vlt-error" role="alert">{error}</p>}<p className="sr-only" role="status">{notice}</p>
    {!writable && <p className="vlt-muted">Режим просмотра. Право изменения задач выдаёт владелец.</p>}
    <div className="task-board" data-archive={archive} aria-label="Этапы задач" aria-busy={loading}>{stages.filter(([key]) => archive ? key === "archived" : key !== "archived").map(([key, label]) => {
      const items = tasks.filter(task => task.status === key);
      return <section key={key} className="task-column" data-stage={key} data-drop={dropStage === key} onDragOver={e => { if (writable && dragging && !busy) { e.preventDefault(); e.dataTransfer.dropEffect = "move"; setDropStage(key); } }} onDragLeave={e => { if (!e.currentTarget.contains(e.relatedTarget as Node)) setDropStage(undefined); }} onDrop={e => { e.preventDefault(); const task = tasks.find(item => item.id === dragging); setDragging(undefined); setDropStage(undefined); if (task) void moveTask(task, key); }}><header><h2><span className="task-stage-dot" />{label}</h2><span>{items.length}</span></header>
      {items.map(task => <article className="task-card" key={task.id} draggable={writable && !busy} data-dragging={dragging === task.id} onDragStart={e => { setDragging(task.id); e.dataTransfer.setData("text/plain", task.id); e.dataTransfer.effectAllowed = "move"; }} onDragEnd={() => { setDragging(undefined); setDropStage(undefined); }}><button className="task-card-open" onClick={() => void openTask(task)}><span className="task-card-meta"><span>#{task.number}</span><span className="task-priority" data-priority={task.priority}>{priorities.find(p => p[0] === task.priority)?.[1]}</span></span><strong>{task.title}</strong>{task.description && <span className="task-excerpt">{task.description}</span>}<span className="task-author">Идея: {memberName(task.author_id)}</span><span className="task-card-footer"><span className="task-assignee"><span className="task-avatar" aria-hidden>{memberName(task.assignee_id).slice(0, 1)}</span>{memberName(task.assignee_id)}</span>{task.due_date && <span><CalendarDays size={13} />{dateLabel(task.due_date)}</span>}</span></button>{writable && <select className="task-move" aria-label={`Этап задачи №${task.number}`} value={task.status} disabled={busy} onChange={e => void moveTask(task, e.target.value)}>{stages.map(([value, text]) => <option key={value} value={value}>{text}</option>)}</select>}</article>)}
      {!items.length && <p className="task-empty">{loading ? "Загрузка…" : search || assignee ? "Нет подходящих задач" : key === "idea" ? "Здесь начинаются новые идеи" : "Пока нет задач"}</p>}
      {writable && key === "idea" && !search && !assignee && <button className="task-add-inline" onClick={() => void openTask()} disabled={busy}><Plus size={15} />Добавить идею</button>}</section>;
    })}</div>{!!next && <button className="vlt-button vlt-button-secondary" disabled={loading} onClick={() => void load(true)}>{loading ? "Загрузка…" : "Загрузить ещё задачи"}</button>}
    <dialog ref={dialog} className="task-dialog" aria-labelledby="task-editor-title" onCancel={e => { e.preventDefault(); closeEditor(); }} onClick={e => { if (e.target === e.currentTarget) closeEditor(); }}><div className="task-dialog-content"><header className="task-dialog-header"><div><span className="task-kicker">{detail ? `Задача №${detail.task.number}` : "Новая идея"}</span><h2 id="task-editor-title">{detail ? "Карточка задачи" : "Создать задачу"}</h2></div><button className="admin-icon-button" aria-label="Закрыть задачу" disabled={busy} onClick={closeEditor}><X size={20} /></button></header>
      {editorError && <div className="vlt-error" role="alert">{editorError}{detail && <button className="vlt-button vlt-button-secondary" disabled={busy} onClick={() => { if (dirty) setConfirmClose(true); else void openTask(detail.task); }}>Обновить карточку</button>}</div>}
      {confirmClose && <div className="task-confirm" role="alert"><p>Есть несохранённые изменения. Закрыть карточку и потерять их?</p><button className="vlt-button vlt-button-secondary" onClick={() => setConfirmClose(false)}>Продолжить редактирование</button><button className="vlt-button vlt-button-secondary" onClick={() => { setConfirmClose(false); dialog.current?.close(); }}>Закрыть без сохранения</button></div>}
      <form onSubmit={save} className="vlt-stack"><fieldset disabled={!writable || busy} className="vlt-stack task-fields"><label className="vlt-label">Название<input autoFocus className="vlt-input" required maxLength={200} value={draft.title} onChange={e => setDraft({ ...draft, title: e.target.value })} placeholder="Что хочется сделать?" /></label><label className="vlt-label">Описание<textarea className="vlt-input" rows={5} maxLength={20000} value={draft.description} onChange={e => setDraft({ ...draft, description: e.target.value })} placeholder="Контекст, идея и ожидаемый результат" /></label><div className="task-form-grid"><label className="vlt-label">Этап<select className="vlt-input" value={draft.status} onChange={e => setDraft({ ...draft, status: e.target.value })}>{stages.map(([value, label]) => <option key={value} value={value}>{label}</option>)}</select></label><label className="vlt-label">Приоритет<select className="vlt-input" value={draft.priority} onChange={e => setDraft({ ...draft, priority: e.target.value })}>{priorities.map(([value, label]) => <option key={value} value={value}>{label}</option>)}</select></label><label className="vlt-label">Исполнитель<select className="vlt-input" value={draft.assignee_id} onChange={e => setDraft({ ...draft, assignee_id: e.target.value })}><option value="">Не назначен</option>{members.filter(member => member.active || member.id === draft.assignee_id).map(member => <option key={member.id} value={member.id} disabled={!member.active}>{member.nickname}{!member.active ? " (нет доступа)" : ""}</option>)}</select></label><label className="vlt-label">Срок<input className="vlt-input" type="date" value={draft.due_date} onChange={e => setDraft({ ...draft, due_date: e.target.value })} /></label></div></fieldset>
      {detail && <p className="task-provenance">Предложил(а) {memberName(detail.task.author_id)} · {dateLabel(detail.task.created_at)} · Обновлено {dateLabel(detail.task.updated_at)}</p>}{writable && <button className="vlt-button" disabled={busy || !draft.title.trim()}>{busy ? "Сохраняем…" : detail ? "Сохранить изменения" : "Создать задачу"}</button>}</form>
      {detail ? <><section className="task-attachments"><h3><Paperclip size={17} />Файлы <span>{detail.attachments.length}/20</span></h3>{detail.attachments.map(file => <div className="task-file" key={file.id}><a href={`/api/v1/admin/tasks/${detail.task.id}/attachments/${file.id}`} download={file.name}><Paperclip size={16} /><span>{file.name}<small>{(file.size / 1024).toLocaleString("ru-RU", { maximumFractionDigits: 1 })} КБ · {memberName(file.author_id)}</small></span></a>{writable && (deleteFile === file.id ? <span><button className="vlt-button vlt-button-secondary" disabled={busy} onClick={() => void removeAttachment(file.id)}>Удалить файл</button><button className="admin-icon-button" aria-label="Отменить удаление" onClick={() => setDeleteFile(undefined)}><X size={15} /></button></span> : <button className="admin-icon-button" aria-label={`Удалить ${file.name}`} disabled={busy} onClick={() => setDeleteFile(file.id)}><X size={15} /></button>)}</div>)}{writable && <label className="vlt-label">Прикрепить файлы<input ref={uploadInput} className="vlt-input" type="file" multiple disabled={busy || detail.attachments.length >= 20} onChange={e => void upload(e.target.files)} /><small>До 10 МБ каждый, до 20 файлов на задачу.</small></label>}{!detail.attachments.length && !writable && <p className="vlt-muted">Файлов пока нет.</p>}</section>
      <section className="task-comments"><h3><MessageSquare size={17} />Обсуждение</h3>{detail.comments.map(item => <article key={item.id}><header><strong>{memberName(item.author_id)}</strong><time dateTime={item.created_at}>{dateLabel(item.created_at)}</time></header><p>{item.body}</p></article>)}{!detail.comments.length && <p className="vlt-muted">Здесь можно уточнить идею и обсудить результат.</p>}{writable && <form onSubmit={postComment} className="vlt-stack"><label className="vlt-label">Комментарий<textarea className="vlt-input" rows={3} maxLength={10000} value={comment} disabled={busy} onChange={e => setComment(e.target.value)} /></label><button className="vlt-button vlt-button-secondary" disabled={busy || !comment.trim()}>Отправить комментарий</button></form>}</section></> : <p className="vlt-muted">После создания задачи можно прикрепить файлы и начать обсуждение.</p>}
    </div></dialog>
  </AdminShell>;
}
