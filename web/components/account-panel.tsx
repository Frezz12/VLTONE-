"use client";

import type { APIError, AccountSession, Device, Quota } from "@vlt/api-client";
import { api } from "@vlt/api-client";
import { Laptop, LogOut, RefreshCw, X } from "lucide-react";
import { useTranslations } from "next-intl";
import { useRouter } from "next/navigation";
import { useEffect, useMemo, useState } from "react";

export function AccountPanel({ locale }: { locale: string }) {
  const t = useTranslations("Account");
  const router = useRouter();
  const [account, setAccount] = useState<AccountSession>();
  const [devices, setDevices] = useState<Device[]>([]);
  const [error, setError] = useState("");
  async function load() {
    try {
      const [next, list] = await Promise.all([
        api.request<AccountSession>("/v1/me"),
        api.request<{ devices: Device[] }>("/v1/me/devices"),
      ]);
      setAccount(next); setDevices(list.devices.filter((device) => !device.revoked_at));
    } catch (reason) {
      const apiError = reason as APIError;
      if (apiError.code === "authentication_required" || apiError.code === "session_expired") router.replace(`/${locale}/login`);
      else setError(apiError.message);
    }
  }
  async function loadQuota() {
    try {
      const quota = await api.request<Quota>("/v1/me/quota");
      setAccount((current) => current ? { ...current, quota } : current);
      setError("");
    } catch (reason) {
      const apiError = reason as APIError;
      if (apiError.code === "authentication_required" || apiError.code === "session_expired") router.replace(`/${locale}/login`);
      else setError(apiError.message);
    }
  }
  useEffect(() => {
    void load();
    const refresh = () => { void loadQuota(); };
    const timer = window.setInterval(refresh, 30000);
    window.addEventListener("focus", refresh);
    return () => { window.clearInterval(timer); window.removeEventListener("focus", refresh); };
  }, []);
  const percent = useMemo(() => account ? Math.min(100, Math.max(0, account.quota.used_tokens / (account.quota.base_limit + account.quota.adjustment) * 100)) : 0, [account]);
  if (!account) return <main className="vlt-main"><p className="vlt-muted">{error || t("loading")}</p></main>;
  async function revoke(id: string) {
    await api.request(`/v1/me/devices/${id}`, { method: "DELETE", headers: { "X-CSRF-Token": account!.csrf_token } });
    await load();
  }
  async function logout() {
    await api.request("/v1/web/auth/logout", { method: "POST", headers: { "X-CSRF-Token": account!.csrf_token } });
    router.replace(`/${locale}/login`); router.refresh();
  }
  const number = new Intl.NumberFormat(locale);
  return <main className="vlt-main account-layout">
    <div className="vlt-row vlt-between"><div><h1 className="vlt-title">{t("title")}</h1><p className="vlt-subtitle">{account.user.nickname} · {account.user.email}</p></div><button className="vlt-button vlt-button-secondary" onClick={logout}><LogOut size={16} aria-hidden /> {t("logout")}</button></div>
    {error && <div className="vlt-error" role="alert">{error}</div>}
    <div className="vlt-grid vlt-grid-2">
      <section className="vlt-card vlt-card-pad vlt-stack"><div className="vlt-row vlt-between"><h2 className="vlt-section-title">{t("plan")}</h2><span className="vlt-badge vlt-badge-accent">Demo</span></div><div><strong>{locale === "ru" ? "Все функции доступны" : "All features enabled"}</strong><p className="vlt-subtitle">{locale === "ru" ? "Бессрочный тестовый доступ" : "Indefinite tester access"}</p></div></section>
      <section className="vlt-card vlt-card-pad vlt-stack"><div className="vlt-row vlt-between"><h2 className="vlt-section-title">{t("tokens")}</h2><RefreshCw size={16} className="vlt-muted" aria-hidden /></div><div><div className="quota-line"><span>{t("used")}: <span className="vlt-code">{number.format(account.quota.used_tokens)}</span></span><span>{t("remaining")}: <span className="vlt-code">{number.format(account.quota.remaining_tokens)}</span></span></div><div className="vlt-progress" role="progressbar" aria-valuenow={percent} aria-valuemin={0} aria-valuemax={100}><span style={{ width: `${percent}%` }} /></div><p className="vlt-subtitle">{t("reset", { date: new Intl.DateTimeFormat(locale, { dateStyle: "medium", timeZone: "UTC" }).format(new Date(account.quota.ends_at)) })} UTC</p></div></section>
    </div>
    <section className="vlt-card vlt-card-pad vlt-stack"><div className="vlt-row vlt-between"><h2 className="vlt-section-title">{t("devices")}</h2><span className="vlt-badge">{devices.length} / 2</span></div><div className="device-list">{devices.length === 0 && <p className="vlt-muted">{t("empty")}</p>}{devices.map((device) => <div className="device-row" key={device.id}><div className="vlt-row"><Laptop size={18} className="vlt-muted" aria-hidden /><div><strong>{device.display_name || device.platform}</strong><div className="vlt-muted">{device.os_version} · VLT {device.app_version} · {new Intl.DateTimeFormat(locale, { dateStyle: "medium", timeStyle: "short" }).format(new Date(device.last_seen_at))}</div></div></div><button className="vlt-button vlt-button-danger" onClick={() => revoke(device.id)}><X size={15} aria-hidden /> {t("revoke")}</button></div>)}</div></section>
  </main>;
}
