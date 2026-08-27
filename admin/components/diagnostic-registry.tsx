"use client";

import type { APIError } from "@vlt/api-client";
import { api } from "@vlt/api-client";
import { Download, Save } from "lucide-react";
import { useEffect, useState } from "react";
import { AdminShell } from "./admin-shell";
import { adminPollingAllowed } from "./admin-activity";
import { useAdmin } from "./use-admin";

type BugReport = { id: string; number: number; user_id: string; title: string; description: string; status: string; internal_note: string; created_at: string };
type CrashHealthSample = { recorded_at?: string; process_cpu?: number; system_cpu?: number; dsp_load?: number; dsp_peak?: number; xruns?: number; resident_bytes?: number; sample_rate?: number; buffer_frames?: number; track_count?: number; clip_count?: number; playback_state?: string; last_plugin?: string };
type CrashReport = { id: string; user_id: string; device_id: string; build_id: string; app_version: string; platform: string; reason: string; last_plugin?: string; artifact_bytes: number; occurred_at: string; metadata?: { signal?: string; exception_code?: string; health_samples?: CrashHealthSample[]; modules?: Array<{ name?: string; version?: string; base_address?: string }> } };
type AuditEntry = { id: string; action: string; target_type: string; target_hash: string; ip: string; created_at: string };
type Draft = { status: string; internal_note: string };

export function DiagnosticRegistry({ kind }: { kind: "bugs" | "crashes" | "audit" }) {
  const { session, error: sessionError } = useAdmin();
  const [items, setItems] = useState<Array<BugReport | CrashReport | AuditEntry>>([]);
  const [drafts, setDrafts] = useState<Record<string, Draft>>({});
  const [error, setError] = useState("");

  async function load() {
    try {
      const value = await api.request<{ bugs?: BugReport[]; crashes?: CrashReport[]; entries?: AuditEntry[] }>(`/v1/admin/${kind}`);
      const next = value.bugs ?? value.crashes ?? value.entries ?? [];
      setItems(next);
      setError("");
      if (value.bugs) setDrafts(Object.fromEntries(value.bugs.map((bug) => [bug.id, { status: bug.status, internal_note: bug.internal_note ?? "" }])));
    } catch (reason) {
      setError((reason as APIError).message);
    }
  }
  useEffect(() => {
    if (!session) return;
    void load();
    if (kind !== "crashes") return;
    const timer = window.setInterval(() => { if (adminPollingAllowed()) void load(); }, 15000);
    return () => window.clearInterval(timer);
  }, [session, kind]);

  async function saveBug(id: string) {
    if (!session) return;
    setError("");
    try {
      await api.json(`/v1/admin/bugs/${id}`, "PATCH", drafts[id], session.csrf_token);
      await load();
    } catch (reason) {
      setError((reason as APIError).message);
    }
  }

  const title = kind === "bugs" ? "Баг-репорты" : kind === "crashes" ? "Краши" : "Аудит";
  return <AdminShell>
    <div className="admin-page-head"><div><h1 className="vlt-title">{title}</h1><p className="vlt-subtitle">{kind === "audit" ? "Неизменяемая история административных действий." : "Диагностика связана с пользователем и устройством."}</p></div></div>
    {(sessionError || error) && <div className="vlt-error" style={{ marginBottom: 16 }}>{sessionError || error}</div>}
    <div className="vlt-table-wrap"><table className="vlt-table"><thead><tr>
      {kind === "bugs" ? <><th>№ / заголовок</th><th>Статус</th><th>Внутренняя заметка</th><th>Создан</th><th /></> : kind === "crashes" ? <><th>Причина</th><th>Версия / build</th><th>Платформа</th><th>Дата</th><th /></> : <><th>Действие</th><th>Тип</th><th>Обезличенная цель</th><th>IP / дата</th></>}
    </tr></thead><tbody>{items.map((raw) => {
      if (kind === "bugs") {
        const item = raw as BugReport; const draft = drafts[item.id] ?? { status: item.status, internal_note: item.internal_note ?? "" };
        return <tr key={item.id}><td><span className="vlt-code">#{item.number}</span><div><strong>{item.title}</strong></div><div className="vlt-muted registry-description">{item.description}</div></td><td><label className="sr-only" htmlFor={`status-${item.id}`}>Статус бага #{item.number}</label><select id={`status-${item.id}`} className="vlt-input" value={draft.status} onChange={(event) => setDrafts((old) => ({ ...old, [item.id]: { ...draft, status: event.target.value } }))}><option value="new">new</option><option value="triage">triage</option><option value="in_progress">in progress</option><option value="fixed">fixed</option><option value="duplicate">duplicate</option><option value="wont_fix">won&apos;t fix</option></select></td><td><label className="sr-only" htmlFor={`note-${item.id}`}>Внутренняя заметка</label><textarea id={`note-${item.id}`} className="vlt-input registry-note" value={draft.internal_note} onChange={(event) => setDrafts((old) => ({ ...old, [item.id]: { ...draft, internal_note: event.target.value } }))} /></td><td>{new Date(item.created_at).toLocaleString("ru")}</td><td><button className="vlt-button vlt-button-secondary" onClick={() => void saveBug(item.id)}><Save size={15} />Сохранить</button></td></tr>;
      }
      if (kind === "crashes") {
        const item = raw as CrashReport;
        const health = item.metadata?.health_samples ?? [];
        const latest = health.at(-1);
        const signal = item.metadata?.signal || item.metadata?.exception_code;
        return <tr key={item.id}><td><strong>{item.reason}</strong>{item.last_plugin && <div className="vlt-muted">Плагин: {item.last_plugin}</div>}{signal && <div className="vlt-code vlt-muted">{signal}</div>}{latest && <details className="crash-context"><summary>Контекст перед крашем · {health.length} samples</summary><div className="vlt-muted">CPU {latest.process_cpu?.toFixed(1) ?? "—"}% / {latest.system_cpu?.toFixed(1) ?? "—"}% · DSP {latest.dsp_load?.toFixed(1) ?? "—"}% (peak {latest.dsp_peak?.toFixed(1) ?? "—"}%)</div><div className="vlt-muted">RAM {latest.resident_bytes ? `${(latest.resident_bytes / 1048576).toFixed(0)} MB` : "—"} · tracks/clips {latest.track_count ?? "—"}/{latest.clip_count ?? "—"} · buffer {latest.buffer_frames ?? "—"} @ {latest.sample_rate ?? "—"} Hz</div>{item.metadata?.modules?.length ? <div className="vlt-muted">Modules: {item.metadata.modules.map((module) => module.name).filter(Boolean).join(", ")}</div> : null}</details>}</td><td><span className="vlt-code">{item.app_version}</span><div className="vlt-muted vlt-code">{item.build_id}</div></td><td>{item.platform}</td><td>{new Date(item.occurred_at).toLocaleString("ru")}</td><td>{item.artifact_bytes > 0 && <a className="vlt-link vlt-row" href={`/api/v1/admin/crashes/${item.id}/artifact`}><Download size={15} /> Скачать логи</a>}</td></tr>;
      }
      const item = raw as AuditEntry;
      return <tr key={item.id}><td>{item.action}</td><td>{item.target_type}</td><td className="vlt-code">{item.target_hash.slice(0, 18)}…</td><td>{item.ip}<div className="vlt-muted">{new Date(item.created_at).toLocaleString("ru")}</div></td></tr>;
    })}</tbody></table></div>
  </AdminShell>;
}
