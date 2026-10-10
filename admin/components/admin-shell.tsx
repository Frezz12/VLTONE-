"use client";

import { api } from "@vlt/api-client";
import { BellRing, ChevronRight, ExternalLink, LogOut, Menu, PanelLeftClose, PanelLeftOpen, Pause, Play, Search, ShieldCheck, X } from "lucide-react";
import NextImage from "next/image";
import Link from "next/link";
import { usePathname, useRouter } from "next/navigation";
import { useEffect, useRef, useState } from "react";
import appIcon from "@/app/icon.png";
import { adminPollingAllowed, markAdminActivity } from "./admin-activity";
import { adminNavigation } from "./admin-navigation";
import { AdminCommandMenu } from "./admin-command-menu";
import { canAdmin, canVisit, adminHome } from "./admin-permissions";
import { useAdmin } from "./use-admin";
import type { AdminSession } from "./use-admin";

type CrashSummary = { id: string; app_version: string; platform: string; reason: string; occurred_at: string };
const lastCrashKey = "vlt-admin-last-crash";

export function AdminShell({ children }: { children: React.ReactNode }) {
  const pathname = usePathname();
  const router = useRouter();
  const { session: access, error: accessError } = useAdmin();
  const navigation = adminNavigation.map(group => ({ ...group, links: group.links.filter(link => canVisit(access?.admin, link.href)) })).filter(group => group.links.length);
  useEffect(() => { if (access && pathname === "/" && !canVisit(access.admin, "/")) { const home = adminHome(access.admin); if (home !== "/") router.replace(home); } }, [access, pathname, router]);
  const [latestCrash, setLatestCrash] = useState<CrashSummary>();
  const [notificationPermission, setNotificationPermission] = useState<NotificationPermission>("default");
  const [collapsed, setCollapsed] = useState(false);
  const [paused, setPaused] = useState(false);
  const [signingOut, setSigningOut] = useState(false);
  const [shellError, setShellError] = useState("");
  const commandMenu = useRef<HTMLDialogElement>(null);
  const mobileMenu = useRef<HTMLDialogElement>(null);
  const page = adminNavigation.flatMap(group => group.links).find(item => item.href === "/" ? pathname === "/" : pathname.startsWith(item.href));

  useEffect(() => {
    try { setCollapsed(localStorage.getItem("vlt-admin-sidebar-collapsed") === "true"); setPaused(localStorage.getItem("vlt-admin-polling-paused") === "true"); } catch { /* Preferences are optional in private browsing. */ }
    const shortcut = (event: KeyboardEvent) => {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "k") {
        event.preventDefault(); mobileMenu.current?.close();
        if (!commandMenu.current?.open) commandMenu.current?.showModal();
      }
    };
    window.addEventListener("keydown", shortcut);
    return () => window.removeEventListener("keydown", shortcut);
  }, []);
  useEffect(() => {
    document.documentElement.dataset.adminPollingPaused = String(paused);
    return () => { delete document.documentElement.dataset.adminPollingPaused; };
  }, [paused]);

  useEffect(() => {
    if ("Notification" in window) setNotificationPermission(Notification.permission);
    markAdminActivity();
    const markActive = () => markAdminActivity();
    window.addEventListener("pointerdown", markActive, { passive: true });
    window.addEventListener("keydown", markActive);
    let cancelled = false;
    async function pollCrashes() {
      if (!adminPollingAllowed() || !canAdmin(access?.admin, "crashes.read")) return;
      try {
        const result = await api.request<{ crashes: CrashSummary[] }>("/v1/admin/crashes?limit=1");
        if (cancelled || !result.crashes[0]) return;
        const crash = result.crashes[0];
        const previous = window.localStorage.getItem(lastCrashKey);
        window.localStorage.setItem(lastCrashKey, crash.id);
        if (!previous || previous === crash.id) return;
        setLatestCrash(crash);
        if ("Notification" in window && Notification.permission === "granted") {
          new Notification("VLTONE: новый краш", {
            body: `${crash.app_version} · ${crash.platform} · ${crash.reason}`,
            tag: `vlt-crash-${crash.id}`,
          });
        }
      } catch {
        // Authentication redirects and transient network failures are handled
        // by the page itself; the monitor simply tries again on the next tick.
      }
    }
    void pollCrashes();
    const timer = window.setInterval(() => void pollCrashes(), 15000);
    return () => {
      cancelled = true;
      window.clearInterval(timer);
      window.removeEventListener("pointerdown", markActive);
      window.removeEventListener("keydown", markActive);
    };
  }, [access]);

  async function enableNotifications() {
    if (!("Notification" in window)) return;
    try {
      setNotificationPermission(await Notification.requestPermission());
    } catch {
      setNotificationPermission("denied");
    }
  }

  async function signOut() {
    setSigningOut(true); setShellError("");
    try {
      const session = await api.request<AdminSession>("/v1/admin/me");
      await api.json("/v1/admin/auth/logout", "POST", {}, session.csrf_token);
      router.replace("/login"); router.refresh();
    } catch { setShellError("Не удалось завершить сессию. Повторите попытку."); }
    finally { setSigningOut(false); }
  }

  function sidebar(mobile = false) {
    return <>
      <div className="admin-brand-row"><Link className="vlt-brand" href="/" onClick={() => mobileMenu.current?.close()} aria-label="VLTone — обзор">
        <span className="admin-brand-icon"><NextImage src={appIcon} width={36} height={36} alt="" priority /></span>
        <span className="admin-brand-copy"><strong>VLTone</strong><small>Панель управления</small></span>
      </Link>{mobile && <button className="admin-icon-button" onClick={() => mobileMenu.current?.close()} aria-label="Закрыть навигацию"><X size={18} aria-hidden /></button>}</div>
      <nav className="admin-nav" aria-label="Администрирование">{navigation.map(group => <div className="admin-nav-group" key={group.label}>
        <span className="admin-nav-label">{group.label}</span>
        {group.links.map(({ href, label, icon: Icon }) => {
          const active = href === "/" ? pathname === "/" : pathname.startsWith(href);
          return <Link href={href} className={active ? "active" : undefined} aria-label={label} title={collapsed ? label : undefined} aria-current={active ? "page" : undefined} key={href} onClick={() => mobileMenu.current?.close()}><Icon size={19} strokeWidth={1.8} aria-hidden /><span>{label}</span></Link>;
        })}
      </div>)}</nav>
      <div className="admin-side-foot">
        <a className="admin-side-action" aria-label="Открыть сайт" title="Открыть сайт" href="https://vltstudio.ru" target="_blank" rel="noreferrer"><ExternalLink size={18} aria-hidden /><span>Открыть сайт</span></a>
        <button className="admin-notification-button" aria-label="Уведомления о крашах" title="Уведомления о крашах" onClick={() => void enableNotifications()} disabled={!canAdmin(access?.admin, "crashes.read") || notificationPermission === "granted" || notificationPermission === "denied"}><BellRing size={18} aria-hidden /><span>{notificationPermission === "granted" ? "Уведомления включены" : notificationPermission === "denied" ? "Уведомления запрещены" : "Уведомления о крашах"}</span></button>
        <button className="admin-side-action" aria-label="Выйти" title="Выйти" onClick={() => void signOut()} disabled={signingOut}><LogOut size={18} aria-hidden /><span>{signingOut ? "Выходим…" : "Выйти"}</span></button>
        {!mobile && <button className="admin-side-action admin-collapse" aria-label={collapsed ? "Развернуть боковую панель" : "Свернуть боковую панель"} onClick={() => { setCollapsed(!collapsed); try { localStorage.setItem("vlt-admin-sidebar-collapsed", String(!collapsed)); } catch { /* Optional preference. */ } }}>{collapsed ? <PanelLeftOpen size={18} aria-hidden /> : <PanelLeftClose size={18} aria-hidden />}<span>Свернуть панель</span></button>}
        <div className="admin-security-state"><ShieldCheck size={16} aria-hidden /><span>Действия сохраняются в аудите</span></div>
      </div>
    </>;
  }

  return <div className="admin-shell" data-sidebar-collapsed={collapsed}>
    <a className="admin-skip-link" href="#admin-main">К основному содержимому</a>
    <aside className="admin-side">{sidebar()}</aside>
    <div className="admin-workspace">
      <header className="admin-topbar">
        <div className="admin-breadcrumb"><button className="admin-icon-button admin-mobile-toggle" onClick={() => mobileMenu.current?.showModal()} aria-label="Открыть навигацию"><Menu size={20} aria-hidden /></button><span>Админка</span><ChevronRight size={14} aria-hidden /><strong>{page?.label ?? "VLTone"}</strong></div>
        <div className="admin-topbar-actions"><button className="admin-command-trigger" aria-label="Быстрый переход" onClick={() => commandMenu.current?.showModal()}><Search size={17} aria-hidden /><span>Быстрый переход</span><kbd>Ctrl K</kbd></button><button className="admin-icon-button" aria-label={paused ? "Продолжить автообновление" : "Приостановить автообновление"} aria-pressed={paused} title={paused ? "Автообновление приостановлено" : "Приостановить автообновление"} onClick={() => { setPaused(!paused); try { localStorage.setItem("vlt-admin-polling-paused", String(!paused)); } catch { /* Optional preference. */ } }}>{paused ? <Play size={18} aria-hidden /> : <Pause size={18} aria-hidden />}</button></div>
      </header>
      <main className="admin-main" id="admin-main" tabIndex={-1}>
      {shellError && <div className="vlt-error" role="alert">{shellError}</div>}
      {latestCrash && <div className="admin-crash-alert" role="status"><BellRing size={18} aria-hidden /><Link href="/crashes"><strong>Новый краш VLTONE</strong><span>{latestCrash.app_version} · {latestCrash.platform} · {latestCrash.reason}</span></Link><button onClick={() => setLatestCrash(undefined)} aria-label="Закрыть уведомление"><X size={16} /></button></div>}
      {accessError && <div className="vlt-error" role="alert">{accessError}</div>}
      {access ? canVisit(access.admin, pathname) ? children : <div className="vlt-card"><h1 className="vlt-title">Нет доступа к разделу</h1><p>Обратитесь к владельцу, чтобы изменить права.</p></div> : <p role="status">Проверяем доступ…</p>}
      </main>
    </div>
    <AdminCommandMenu dialog={commandMenu} admin={access?.admin} />
    <dialog ref={mobileMenu} className="admin-mobile-menu" aria-label="Навигация админки" onClick={event => { if (event.target === event.currentTarget) mobileMenu.current?.close(); }}><div>{sidebar(true)}</div></dialog>
  </div>;
}
