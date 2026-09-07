"use client";

import type { APIError, Device, Quota, User } from "@vlt/api-client";
import { api } from "@vlt/api-client";
import { Ban, DatabaseZap, KeyRound, RotateCcw, Trash2, Unplug } from "lucide-react";
import { useRouter } from "next/navigation";
import { FormEvent, useEffect, useState } from "react";
import { AdminShell } from "./admin-shell";
import { TelemetryHistory } from "./telemetry-history";
import { CollaborationAccessSwitch } from "./collaboration-access-switch";
import { useAdmin } from "./use-admin";

type Detail = { user: User; devices: Device[]; quota: Quota; subscription: { plan: { display_name: string } }; counts: { launches: number; crashes: number; bugs: number } };
type LedgerEntry = { id: string; kind: string; delta: number; balance_after: number; created_at: string };

export function UserDetail({ id }: { id: string }) {
  const { session } = useAdmin();
  const router = useRouter();
  const [detail, setDetail] = useState<Detail>();
  const [diagnosticsRevision, setDiagnosticsRevision] = useState(0);
  const [ledger, setLedger] = useState<LedgerEntry[]>([]);
  const [error, setError] = useState("");
  const [accessPending, setAccessPending] = useState(false);
  const [accessError, setAccessError] = useState("");
  const [accessSuccess, setAccessSuccess] = useState("");

  async function load() {
    try {
      const [nextDetail, nextLedger] = await Promise.all([
        api.request<Detail>(`/v1/admin/users/${id}`),
        api.request<{ entries: LedgerEntry[] }>(`/v1/admin/users/${id}/ledger?limit=20`),
      ]);
      setDetail(nextDetail); setLedger(nextLedger.entries);
    } catch (reason) {
      setError((reason as APIError).message);
    }
  }
  useEffect(() => {
    if (!session) return;
    void load();
  }, [session, id]);

  async function action(path: string, body: unknown = {}) {
    if (!session) return;
    setError("");
    try {
      await api.json(`/v1/admin/users/${id}${path}`, "POST", body, session.csrf_token);
      await load();
    } catch (reason) { setError((reason as APIError).message); }
  }
  async function setCollaborationAccess(enabled: boolean) {
    if (!session || !detail || accessPending) return;
    const previous = detail.user.collaboration_enabled;
    setAccessPending(true);
    setAccessError("");
    setAccessSuccess("");
    setDetail((current) => current ? { ...current, user: { ...current.user, collaboration_enabled: enabled } } : current);
    try {
      const result = await api.json<{ collaboration_enabled: boolean }>(
        `/v1/admin/users/${id}/collaboration-access`, "PUT", { enabled }, session.csrf_token,
      );
      setDetail((current) => current ? { ...current, user: { ...current.user, collaboration_enabled: result.collaboration_enabled } } : current);
      setAccessSuccess(`Онлайн-доступ ${result.collaboration_enabled ? "включён" : "выключен"}.`);
    } catch (reason) {
      setDetail((current) => current ? { ...current, user: { ...current.user, collaboration_enabled: previous } } : current);
      setAccessError((reason as APIError).message || "Не удалось изменить онлайн-доступ.");
    } finally {
      setAccessPending(false);
    }
  }
  async function addTokens(event: FormEvent<HTMLFormElement>) {
    event.preventDefault(); const form = new FormData(event.currentTarget);
    await action("/tokens/add", { amount: Number(form.get("amount")) });
  }
  async function reset(event: FormEvent<HTMLFormElement>) {
    event.preventDefault(); const form = new FormData(event.currentTarget);
    await action("/tokens/reset", { password: form.get("password") });
  }
  async function deleteDiagnostics() {
    if (!session || !window.confirm("Удалить всю телеметрию, краши, баг-репорты и связанные файлы этого пользователя?")) return;
    try {
      await api.request(`/v1/admin/users/${id}/diagnostics`, { method: "DELETE", headers: { "X-CSRF-Token": session.csrf_token } });
      setDiagnosticsRevision((value) => value + 1);
      await load();
    } catch (reason) { setError((reason as APIError).message); }
  }
  async function remove(event: FormEvent<HTMLFormElement>) {
    event.preventDefault(); if (!session || !detail) return;
    const form = new FormData(event.currentTarget);
    if (form.get("confirmation") !== detail.user.nickname) { setError("Для удаления введите никнейм пользователя точно."); return; }
    try {
      await api.request(`/v1/admin/users/${id}`, { method: "DELETE", headers: { "Content-Type": "application/json", "X-CSRF-Token": session.csrf_token }, body: JSON.stringify({ password: form.get("password") }) });
      router.replace("/users");
    } catch (reason) { setError((reason as APIError).message); }
  }

  if (!detail) return <AdminShell><p className="vlt-muted">{error || "Загрузка профиля…"}</p></AdminShell>;
  const total = detail.quota.base_limit + detail.quota.adjustment;

  return <AdminShell>
    <div className="admin-page-head"><div><h1 className="vlt-title">{detail.user.nickname}</h1><p className="vlt-subtitle">{detail.user.email}</p></div><span className="vlt-badge vlt-badge-accent">Demo</span></div>
    {error && <div className="vlt-error" style={{ marginBottom: 16 }}>{error}</div>}
    <div className="detail-grid"><div className="vlt-stack">
      <section className="vlt-card vlt-card-pad vlt-stack">
        <div className="vlt-row vlt-between"><h2 className="vlt-section-title">Аккаунт</h2><button className={`vlt-button ${detail.user.status === "active" ? "vlt-button-danger" : ""}`} onClick={() => void action(detail.user.status === "active" ? "/suspend" : "/activate")}><Ban size={15} />{detail.user.status === "active" ? "Приостановить" : "Активировать"}</button></div>
        <dl className="definition-list"><dt>ID</dt><dd className="vlt-code">{detail.user.id}</dd><dt>Статус</dt><dd>{detail.user.status}</dd><dt>Согласие</dt><dd>{detail.user.consent_version}</dd><dt>Запусков</dt><dd>{detail.counts.launches}</dd><dt>Крашей</dt><dd>{detail.counts.crashes}</dd><dt>Багов</dt><dd>{detail.counts.bugs}</dd></dl>
        <div className="collaboration-access-setting">
          <div><h3 className="vlt-section-title">Онлайн-доступ</h3><p className="vlt-muted">Выключение отключит активные онлайн-сессии пользователя. Вход в аккаунт и работа с локальными проектами останутся доступны.</p></div>
          <CollaborationAccessSwitch
            enabled={detail.user.collaboration_enabled}
            pending={accessPending}
            label={`Онлайн-доступ для ${detail.user.nickname}`}
            error={accessError}
            success={accessSuccess}
            onChange={(enabled) => void setCollaborationAccess(enabled)}
          />
        </div>
      </section>
      <section className="vlt-card vlt-card-pad vlt-stack"><div className="vlt-row vlt-between"><h2 className="vlt-section-title">Устройства</h2><button className="vlt-button vlt-button-secondary" onClick={() => void action("/sessions/revoke")}><Unplug size={15} />Отозвать все сессии</button></div>{detail.devices.map((device) => <div className="device-row" key={device.id}><div><strong>{device.display_name}</strong><div className="vlt-muted">{device.platform} · {device.os_version} · {device.app_version}</div></div>{!device.revoked_at && <button className="vlt-button vlt-button-danger" onClick={() => void action(`/devices/${device.id}/revoke`)}>Отозвать</button>}</div>)}</section>
      <section className="vlt-card vlt-card-pad vlt-stack"><div className="telemetry-toolbar"><h2 className="vlt-section-title">История состояния программы</h2><button className="vlt-button vlt-button-danger" onClick={() => void deleteDiagnostics()}><DatabaseZap size={15} />Удалить диагностику</button></div><TelemetryHistory key={`${id}-${diagnosticsRevision}`} userID={id} /></section>
    </div><aside className="vlt-stack">
      <section className="vlt-card vlt-card-pad vlt-stack"><h2 className="vlt-section-title">AI-квота UTC</h2><div className="vlt-stat-value">{new Intl.NumberFormat("ru").format(detail.quota.remaining_tokens)}</div><div className="vlt-muted">из {new Intl.NumberFormat("ru").format(total)} осталось</div><div className="vlt-progress"><span style={{ width: `${total > 0 ? Math.min(100, detail.quota.used_tokens / total * 100) : 0}%` }} /></div><form className="vlt-row" onSubmit={addTokens}><input className="vlt-input" name="amount" type="number" min="1" placeholder="Добавить токены" required /><button className="vlt-button" aria-label="Добавить токены"><KeyRound size={15} /></button></form><form className="vlt-stack" onSubmit={reset}><input className="vlt-input" name="password" type="password" placeholder="Пароль администратора" required /><button className="vlt-button vlt-button-secondary"><RotateCcw size={15} />Сбросить расход</button></form><div><h3 className="vlt-section-title">Последние операции</h3>{ledger.map((entry) => <div className="ledger-row" key={entry.id}><span>{entry.kind}</span><span className="vlt-code">{entry.delta > 0 ? "+" : ""}{new Intl.NumberFormat("ru").format(entry.delta)}</span></div>)}</div></section>
      <section className="vlt-card vlt-card-pad vlt-stack danger-zone"><h2 className="vlt-section-title">Полное удаление</h2><p className="vlt-muted">Аккаунт, сессии, квоты, диагностика и файлы будут удалены. В аудите останется обезличенная запись.</p><form className="vlt-stack" onSubmit={remove}><input className="vlt-input" name="confirmation" placeholder={`Введите ${detail.user.nickname}`} required /><input className="vlt-input" name="password" type="password" placeholder="Пароль администратора" required /><button className="vlt-button vlt-button-danger"><Trash2 size={15} />Удалить навсегда</button></form></section>
    </aside></div>
  </AdminShell>;
}
